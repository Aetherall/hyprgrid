// The overview's hooks into Hyprland (see Hooks.hpp): on a monitor with a
// screen open, the screen draws in place of Hyprland's workspace, and decides
// which frames and damage go through. Plus the dispatchers
// (hyprgrid:overview / overview_navigate / overview_window) and the entry points
// hyprgrid calls (Overview.hpp).

#define WLR_USE_UNSTABLE

#include "Internal.hpp"
#include "Session.hpp"
#include "Hooks.hpp"
#include "Overview.hpp"
#include "OverviewPassElement.hpp"
#include "globals.hpp"

#include <hyprland/src/layout/supplementary/DragController.hpp>

#include <format>
#include <stdexcept>

namespace {
    using Overview::overview;

    CFunctionHook* g_renderWorkspaceHook = nullptr;
    CFunctionHook* g_addDamageHookA      = nullptr;
    CFunctionHook* g_addDamageHookB      = nullptr;
    CFunctionHook* g_damageSurfaceHook   = nullptr;
    CFunctionHook* g_scheduleFrameHook   = nullptr;
    CFunctionHook* g_sendFrameEventsHook = nullptr;
    CFunctionHook* g_surfaceFrameHook    = nullptr;
    CFunctionHook* g_moveMouseHook       = nullptr;

    typedef void (*origRenderWorkspace)(void*, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp&, const CBox&);
    typedef void (*origAddDamageA)(void*, const CBox&);
    typedef void (*origAddDamageB)(void*, const pixman_region32_t*);
    typedef void (*origDamageSurface)(void*, SP<CWLSurfaceResource>, double, double, double);
    typedef void (*origScheduleFrame)(void*, Aquamarine::IOutput::scheduleFrameReason);
    typedef void (*origSendFrameEventsToWorkspace)(void*, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp&);
    typedef void (*origSurfaceFrame)(void*, const Time::steady_tp&);
    typedef void (*origMoveMouse)(void*, const Vector2D&);

    bool                g_unloading          = false;
    bool                g_rendering          = false; // Hyprland draws workspaces itself meanwhile
    bool                g_damageFromSurface  = false;
    PHLMONITORREF       g_renderingMonitor;
    bool                g_hooksActive        = false;
    bool                g_moveMouseActive    = false;
    bool                g_moveMouseMissing   = false;
    bool                g_moveMouseWarned    = false;
    CHyprSignalListener g_configReloaded, g_preRender;

    SP<Overview::CScreen> screenFor(const PHLMONITOR& monitor) {
        return overview() ? overview()->screenFor(monitor) : nullptr;
    }

    bool anyScreen() {
        return overview() && !overview()->screens().empty();
    }

    void failNotif(const std::string& reason) {
        HyprlandAPI::addNotification(OVERVIEW_HANDLE, "[hyprgrid] overview: failure in initialization: " + reason, CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
    }

    // SUPER + drag carried into the overview: when Hyprland's drag passes its
    // threshold with cross_monitor_drag on and a screen open, the screen on
    // its monitor takes it.
    bool adoptNativeDrag() {
        if (!Overview::Config::getCrossMonitorDrag() || !anyScreen() || !g_layoutManager || !g_pInputManager)
            return false;

        const auto& CONTROLLER = g_layoutManager->dragController();
        const auto  TARGET     = CONTROLLER ? CONTROLLER->target() : nullptr;
        const auto  WINDOW     = TARGET ? TARGET->window() : PHLWINDOW{};
        if (!WINDOW || CONTROLLER->mode() != MBIND_MOVE || !CONTROLLER->dragThresholdReached())
            return false;

        const auto SOURCEWORKSPACE = TARGET->workspace();
        const auto SOURCEMONITOR   = SOURCEWORKSPACE ? SOURCEWORKSPACE->m_monitor.lock() : WINDOW->m_monitor.lock();
        if (!SOURCEMONITOR || !SOURCEMONITOR->m_enabled)
            return false;

        bool       created = false;
        const auto SCREEN  = overview()->open(SOURCEMONITOR, true, &created);
        if (SCREEN && SCREEN->isClosing())
            SCREEN->reopen();
        if (!SCREEN || !overview()->adoptNativeDrag(*SCREEN, WINDOW)) {
            if (created && SCREEN)
                SCREEN->dismissTransient();
            return false;
        }
        SCREEN->requestInputFrame();
        SCREEN->damage();
        return true;
    }

    void hkMoveMouse(void* thisptr, const Vector2D& mousePos) {
        rc<origMoveMouse>(g_moveMouseHook->m_original)(thisptr, mousePos);
        // moveMouse() updates dragThresholdReached().
        if (!g_unloading && g_moveMouseActive && Overview::Config::getCrossMonitorDrag()) {
            try {
                adoptNativeDrag();
            } catch (...) {
                // Never unwind through a Hyprland hook.
            }
        }
    }

    // Frames Hyprland asks for while zoomed out go at the screen's pace.
    void hkScheduleFrame(void* thisptr, Aquamarine::IOutput::scheduleFrameReason reason) {
        if (const auto SCREEN = screenFor(sc<Monitor::CMonitor*>(thisptr)->m_self.lock())) {
            using enum Aquamarine::IOutput::scheduleFrameReason;
            const bool THROTTLED = reason == AQ_SCHEDULE_UNKNOWN || reason == AQ_SCHEDULE_CLIENT_UNKNOWN || reason == AQ_SCHEDULE_NEEDS_FRAME || reason == AQ_SCHEDULE_RENDER_MONITOR ||
                reason == AQ_SCHEDULE_DAMAGE;
            if (THROTTLED && !SCREEN->blockDamageReporting && !SCREEN->shouldAllowRealtimePreviewSchedule())
                return;
        }
        rc<origScheduleFrame>(g_scheduleFrameHook->m_original)(thisptr, reason);
    }

    void hkRenderWorkspace(void* thisptr, PHLMONITOR monitor, PHLWORKSPACE workspace, const Time::steady_tp& now, const CBox& geometry) {
        const auto SCREEN = screenFor(monitor);
        if (!SCREEN || g_rendering) {
            rc<origRenderWorkspace>(g_renderWorkspaceHook->m_original)(thisptr, monitor, workspace, now, geometry);
            return;
        }
        const auto               PREVMONITOR = g_renderingMonitor;
        Overview::Hooks::CRendering rendering;
        g_renderingMonitor = monitor;
        SCREEN->render(); // SCREEN keeps it alive meanwhile
        g_renderingMonitor = PREVMONITOR;
    }

    void hkDamageSurface(void* thisptr, SP<CWLSurfaceResource> surface, double x, double y, double scale) {
        const auto& SCREENS = overview() ? overview()->screens() : std::vector<SP<Overview::CScreen>>{};
        const bool  HANDLED = SCREENS.empty() || std::ranges::any_of(SCREENS, [](const auto& s) { return s->blockDamageReporting; }) ||
            std::ranges::all_of(SCREENS, [&surface](const auto& s) { return s->shouldHandleSurfaceDamage(surface); });
        if (!HANDLED)
            return;
        const bool PREV     = g_damageFromSurface;
        g_damageFromSurface = !SCREENS.empty();
        rc<origDamageSurface>(g_damageSurfaceHook->m_original)(thisptr, surface, x, y, scale);
        g_damageFromSurface = PREV;
    }

    // The screen sends frame callbacks itself.
    void hkSendFrameEventsToWorkspace(void* thisptr, PHLMONITOR monitor, PHLWORKSPACE workspace, const Time::steady_tp& now) {
        if (screenFor(monitor))
            return;
        rc<origSendFrameEventsToWorkspace>(g_sendFrameEventsHook->m_original)(thisptr, monitor, workspace, now);
    }

    void hkSurfaceFrame(void* thisptr, const Time::steady_tp& now) {
        const auto SURFACE = sc<CWLSurfaceResource*>(thisptr)->m_self.lock();
        if (overview() && std::ranges::any_of(overview()->screens(), [&](const auto& s) { return !s->shouldAllowSurfaceFrame(SURFACE, now); }))
            return;
        rc<origSurfaceFrame>(g_surfaceFrameHook->m_original)(thisptr, now);
    }

    // The frame's own damage, while nothing on screen animates, is dropped.
    bool suppressDamage(Monitor::CMonitor* monitor) {
        const auto SCREEN = screenFor(monitor->m_self.lock());
        return SCREEN && g_renderingMonitor.lock() == monitor->m_self.lock() && !g_damageFromSurface && SCREEN->shouldSuppressRenderDamage();
    }

    void hkAddDamageA(void* thisptr, const CBox& box) {
        if (suppressDamage(sc<Monitor::CMonitor*>(thisptr)))
            return;
        rc<origAddDamageA>(g_addDamageHookA->m_original)(thisptr, box);
    }

    void hkAddDamageB(void* thisptr, const pixman_region32_t* region) {
        if (suppressDamage(sc<Monitor::CMonitor*>(thisptr)))
            return;
        rc<origAddDamageB>(g_addDamageHookB->m_original)(thisptr, region);
    }

    // --- Dispatchers ---

    std::pair<std::string, std::string> splitArg(const std::string& arg) {
        const auto FIRST = arg.find_first_not_of(" \t");
        if (FIRST == std::string::npos)
            return {"on", ""};
        const auto SEPARATOR = arg.find_first_of(" \t", FIRST);
        if (SEPARATOR == std::string::npos)
            return {arg.substr(FIRST), ""};
        const auto TARGET = arg.find_first_not_of(" \t", SEPARATOR);
        if (TARGET == std::string::npos)
            return {arg.substr(FIRST, SEPARATOR - FIRST), ""};
        const auto LAST = arg.find_last_not_of(" \t");
        return {arg.substr(FIRST, SEPARATOR - FIRST), arg.substr(TARGET, LAST - TARGET + 1)};
    }

    std::vector<PHLMONITOR> targetMonitors(const std::string& target) {
        if (target == "all")
            return State::monitorState()->monitors();
        if (!target.empty()) {
            const auto& MONITORS = State::monitorState()->monitors();
            const auto  IT       = std::ranges::find_if(MONITORS, [&target](const auto& monitor) { return monitor && monitor->m_name == target; });
            return IT == MONITORS.end() ? std::vector<PHLMONITOR>{} : std::vector<PHLMONITOR>{*IT};
        }
        const auto MONITOR = Desktop::focusState()->monitor();
        return MONITOR ? std::vector<PHLMONITOR>{MONITOR} : std::vector<PHLMONITOR>{};
    }

    // From a mouse bind, the screen under the pointer; else the active one.
    SP<Overview::CScreen> dispatcherScreen() {
        if (!overview())
            return nullptr;
        const auto& STATE = ::Config::Actions::state();
        if (STATE && STATE->m_bindInvocationDepth > 0 && STATE->m_lastCode == 0 && STATE->m_lastMouseCode != 0)
            return g_pInputManager ? overview()->screenAt(g_pInputManager->getMouseCoordsInternal()) : nullptr;
        return overview()->activeScreen();
    }

    SDispatchResult onOverviewDispatcher(std::string arg) {
        if (!overview())
            return {};
        const auto [ACTION, TARGET] = splitArg(arg);

        if (ACTION == "select") {
            if (const auto SCREEN = overview()->screenAt(g_pInputManager->getMouseCoordsInternal()))
                SCREEN->selectHoveredWorkspace();
            return {};
        }

        if (ACTION == "off" || ACTION == "close" || ACTION == "disable") {
            if (TARGET.empty() || TARGET == "all") {
                overview()->closeAll();
                return {};
            }
            const auto MONITORS = targetMonitors(TARGET);
            if (MONITORS.empty())
                return {.success = false, .error = "monitor not found: " + TARGET};
            if (const auto SCREEN = overview()->screenFor(MONITORS.front()))
                SCREEN->close();
            return {};
        }

        if (ACTION != "toggle" && ACTION != "on" && ACTION != "open" && ACTION != "enable")
            return {.success = false, .error = "invalid arg. expected toggle|open|close|select [monitor|all]"};

        if (ACTION == "toggle" && TARGET.empty() && overview()->closeLinked())
            return {};

        const auto MONITORS = targetMonitors(TARGET);
        if (MONITORS.empty())
            return {.success = false, .error = TARGET.empty() ? "no active monitor" : "monitor not found: " + TARGET};

        if (ACTION == "toggle" && std::ranges::all_of(MONITORS, [](const auto& monitor) { return !!overview()->screenFor(monitor); })) {
            for (const auto& monitor : MONITORS) {
                if (const auto SCREEN = overview()->screenFor(monitor)) {
                    if (SCREEN->isClosing())
                        SCREEN->reopen();
                    else
                        SCREEN->close();
                }
            }
            return {};
        }

        for (const auto& monitor : MONITORS) {
            if (!overview()->open(monitor, true))
                return {.success = false, .error = "failed enabling overview hooks (is other overview plugin enabled?)"};
        }
        return {};
    }

    SDispatchResult onNavigateDispatcher(std::string arg) {
        const auto SCREEN = dispatcherScreen();
        if (!SCREEN)
            return {};
        if (arg != "left" && arg != "right" && arg != "up" && arg != "down")
            return {.success = false, .error = "invalid arg. expected left|right|up|down"};
        SCREEN->moveSelection(arg);
        return {};
    }

    SDispatchResult onWindowDispatcher(std::string arg) {
        const auto SCREEN = overview() ? overview()->screenAt(g_pInputManager->getMouseCoordsInternal()) : nullptr;
        if (!SCREEN)
            return {};
        if (arg != "select" && arg != "close")
            return {.success = false, .error = "invalid arg. expected select|close"};
        SCREEN->windowAction(arg);
        return {};
    }

    // --- Finding Hyprland's functions ---

    void* findOptionalFn(const std::string& name, const std::string_view needle) {
        try {
            const auto                  FNS = HyprlandAPI::findFunctionsByName(OVERVIEW_HANDLE, name);
            std::vector<SFunctionMatch> matches;
            for (const auto& fn : FNS) {
                if (fn.demangled.find(needle) != std::string::npos)
                    matches.emplace_back(fn);
            }
            return matches.size() == 1 ? matches.front().address : nullptr;
        } catch (...) { return nullptr; }
    }

    // The one function `name` whose demangled name holds one of `needles`
    // (to tell overloads apart).
    void* findFnOrThrow(const std::string& name, std::initializer_list<std::string_view> needles) {
        const auto FNS = HyprlandAPI::findFunctionsByName(OVERVIEW_HANDLE, name);
        if (FNS.empty()) {
            failNotif(std::format("no fns for hook {}", name));
            throw std::runtime_error(std::format("[hyprgrid] overview: no fns for hook {}", name));
        }
        if (needles.size() == 0 || (needles.size() == 1 && needles.begin()->empty()))
            return FNS[0].address;

        std::vector<SFunctionMatch> matches;
        for (const auto& fn : FNS) {
            if (std::ranges::any_of(needles, [&](const auto& needle) { return needle.empty() || fn.demangled.find(needle) != std::string::npos; }))
                matches.push_back(fn);
        }
        if (matches.empty()) {
            failNotif(std::format("no matching overload for hook {}", name));
            throw std::runtime_error(std::format("[hyprgrid] overview: no matching overload for hook {}", name));
        }
        if (matches.size() > 1) {
            failNotif(std::format("ambiguous overload for hook {} ({} matches)", name, matches.size()));
            throw std::runtime_error(std::format("[hyprgrid] overview: ambiguous overload for hook {}", name));
        }
        return matches[0].address;
    }

    void disableMoveMouse() {
        if (g_moveMouseActive && g_moveMouseHook && g_moveMouseHook->unhook())
            g_moveMouseActive = false;
    }
}

namespace Overview::Hooks {
    CRendering::CRendering() : m_previous(g_rendering) {
        g_rendering = true;
    }

    CRendering::~CRendering() {
        g_rendering = m_previous;
    }

    bool unloading() {
        return g_unloading;
    }

    bool enable() {
        if (g_hooksActive)
            return true;

        bool ok = g_renderWorkspaceHook->hook();
        ok      = ok && g_scheduleFrameHook->hook();
        ok      = ok && g_damageSurfaceHook->hook();
        ok      = ok && g_sendFrameEventsHook->hook();
        ok      = ok && g_surfaceFrameHook->hook();
        ok      = ok && g_addDamageHookA->hook();
        ok      = ok && g_addDamageHookB->hook();
        if (!ok) {
            disable();
            failNotif("Failed enabling overview hooks (is other overview plugin enabled?)");
            return false;
        }
        g_hooksActive = true;
        reconcileNativeDrag();
        return true;
    }

    void disable() {
        disableMoveMouse();
        for (auto* hook : {g_addDamageHookB, g_addDamageHookA, g_surfaceFrameHook, g_sendFrameEventsHook, g_damageSurfaceHook, g_scheduleFrameHook, g_renderWorkspaceHook}) {
            if (hook)
                hook->unhook();
        }
        g_hooksActive = false;
    }

    void reconcileNativeDrag() {
        if (g_unloading || !g_hooksActive || !anyScreen() || !Overview::Config::getCrossMonitorDrag()) {
            disableMoveMouse();
            return;
        }
        if (g_moveMouseActive)
            return;

        try {
            if (!g_moveMouseHook && !g_moveMouseMissing) {
                if (const auto ADDRESS = findOptionalFn("moveMouse", "CLayoutManager::moveMouse("))
                    g_moveMouseHook = HyprlandAPI::createFunctionHook(OVERVIEW_HANDLE, ADDRESS, rc<void*>(hkMoveMouse));
                if (!g_moveMouseHook)
                    g_moveMouseMissing = true;
            }
            if (!g_moveMouseMissing && g_moveMouseHook) {
                if (g_moveMouseHook->hook()) {
                    g_moveMouseActive = true;
                    return;
                }
                g_moveMouseMissing = true;
            }
        } catch (...) { g_moveMouseMissing = true; }

        if (!g_moveMouseWarned) {
            g_moveMouseWarned = true;
            HyprlandAPI::addNotification(OVERVIEW_HANDLE,
                                         "[hyprgrid] overview: cross-monitor drag is enabled, but Hyprland drag adoption is unavailable; overview-origin cross-monitor dragging remains available",
                                         CHyprColor{1.0, 0.75, 0.2, 1.0}, 7500);
        }
    }
}

void Overview::init(HANDLE handle) {
    OVERVIEW_HANDLE = handle;

    g_renderWorkspaceHook = HyprlandAPI::createFunctionHook(handle, findFnOrThrow("renderWorkspace", {"CHyprRenderer::renderWorkspace(", "IHyprRenderer::renderWorkspace("}),
                                                            rc<void*>(hkRenderWorkspace));
    g_scheduleFrameHook   = HyprlandAPI::createFunctionHook(handle, findFnOrThrow("_ZN7Monitor8CMonitor13scheduleFrameEN10Aquamarine7IOutput19scheduleFrameReasonE", {""}),
                                                            rc<void*>(hkScheduleFrame));
    g_damageSurfaceHook   = HyprlandAPI::createFunctionHook(handle, findFnOrThrow("damageSurface", {"CHyprRenderer::damageSurface(", "IHyprRenderer::damageSurface("}),
                                                            rc<void*>(hkDamageSurface));
    g_sendFrameEventsHook = HyprlandAPI::createFunctionHook(
        handle, findFnOrThrow("sendFrameEventsToWorkspace", {"CHyprRenderer::sendFrameEventsToWorkspace(", "IHyprRenderer::sendFrameEventsToWorkspace("}),
        rc<void*>(hkSendFrameEventsToWorkspace));
    g_surfaceFrameHook = HyprlandAPI::createFunctionHook(
        handle, findFnOrThrow("_ZN18CWLSurfaceResource5frameERKNSt6chrono10time_pointINS0_3_V212steady_clockENS0_8durationIlSt5ratioILl1ELl1000000000EEEEEE", {""}),
        rc<void*>(hkSurfaceFrame));
    g_addDamageHookB = HyprlandAPI::createFunctionHook(handle, findFnOrThrow("addDamageEPK15pixman_region32", {"CMonitor::addDamage"}), rc<void*>(hkAddDamageB));
    g_addDamageHookA = HyprlandAPI::createFunctionHook(handle, findFnOrThrow("_ZN7Monitor8CMonitor9addDamageERKN9Hyprutils4Math4CBoxE", {""}), rc<void*>(hkAddDamageA));

    createOverview();

    g_preRender = Event::bus()->m_events.render.pre.listen([](PHLMONITOR monitor) {
        if (const auto SCREEN = screenFor(monitor))
            SCREEN->onPreRender();
    });
    g_configReloaded = Event::bus()->m_events.config.reloaded.listen([] { Hooks::reconcileNativeDrag(); });

    Overview::Config::registerDispatcher("overview", onOverviewDispatcher);
    Overview::Config::registerDispatcher("overview_navigate", onNavigateDispatcher);
    Overview::Config::registerDispatcher("overview_window", onWindowDispatcher);
    Overview::Config::registerConfig();
}

void Overview::exit() {
    g_unloading = true;
    g_configReloaded.reset();
    g_preRender.reset();
    destroyOverview();
    Hooks::disable();
}
