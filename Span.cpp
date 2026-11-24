// Floating windows spanning two monitors. A floating window belongs to the
// workspace of one monitor (its owner) but may lie partly on another; Hyprland
// draws that part there too. The owner's view moves on its own (View.cpp), so
// while it moves, that part would slide over the other monitor's content, or
// vanish at once when its workspace leaves. Instead, that part is shown only
// while the owner shows the window's workspace at rest: it fades out where it
// lies when the owner's view starts moving (a glide, a hold, the overview),
// and fades back in once the view settles on that workspace. On the owner's
// monitor, nothing changes.
//
// Config: plugin:hyprgrid:span:enabled (off: Hyprland's own drawing) and
// plugin:hyprgrid:span:fade (ms; 0: at once). Hooking the renderer relies on
// Hyprland internals: when that fails, hyprgrid loads without this.

#include "Span.hpp"
#include "Grid.hpp"
#include "View.hpp"
#include "overview/Config.hpp"
#include "overview/Overview.hpp"

#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/WindowRenderPresentation.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>

#include <algorithm>
#include <chrono>
#include <unordered_map>

namespace {
    const std::string ENABLED = "plugin:hyprgrid:span:enabled";
    const std::string FADE    = "plugin:hyprgrid:span:fade"; // ms, fully shown to hidden or back

    typedef bool (*origShouldRenderWindow)(void*, PHLWINDOW, PHLMONITOR);
    typedef void (*origRenderWindow)(void*, Render::CRenderContext&, PHLWINDOW, PHLMONITOR, const Render::SWindowRenderPresentation&, const Time::steady_tp&, bool,
                                     Render::eRenderPassMode, bool, bool);

    CFunctionHook* g_shouldRenderHook = nullptr;
    CFunctionHook* g_renderWindowHook = nullptr;
    bool           g_hooked           = false;

    bool enabled() {
        return g_hooked && Overview::Config::getValue<bool>(ENABLED);
    }

    struct SFade {
        PHLWINDOWREF                          window;
        float                                 alpha = 0.F;
        std::chrono::steady_clock::time_point last;
    };
    std::unordered_map<const Desktop::View::CWindow*, SFade> g_fades;

    // The monitor owning `window`, when it is a floating window of a grid
    // workspace lying partly on `monitor`, another monitor; else null.
    PHLMONITOR spanOwner(const PHLWINDOW& window, const PHLMONITOR& monitor) {
        if (!window || !monitor || !enabled() || !window->isFloating() || (window->m_state & Desktop::View::WINDOW_STATE_PINNED))
            return nullptr;
        const auto WS = window->m_workspace;
        if (!WS || WS->type() == Workspace::eWorkspaceType::SPECIAL || !Grid::cellOf(WS))
            return nullptr;
        const auto OWNER = WS->m_monitor.lock();
        if (!OWNER || OWNER == monitor)
            return nullptr;
        if (window->getFullWindowBoundingBox().intersection(monitor->logicalBox()).empty())
            return nullptr;
        return OWNER;
    }

    // Shown: its workspace is the one the owner shows, at rest.
    float targetOf(const PHLWINDOW& window, const PHLMONITOR& owner) {
        return window->m_workspace == owner->m_activeWorkspace && !View::moving(owner) && !Overview::drawing(owner) ? 1.F : 0.F;
    }

    // The fade of `window`'s part outside its owner, moved toward `target`
    // by the time since it was last asked. A window first seen starts there.
    float fadeOf(const PHLWINDOW& window, float target) {
        const auto NOW  = std::chrono::steady_clock::now();
        auto&      fade = g_fades[window.get()];
        if (fade.window.lock() != window) {
            fade = {.window = window, .alpha = target, .last = NOW};
            return target;
        }
        const int   MS   = Overview::Config::getValue<int>(FADE);
        const float STEP = MS <= 0 ? 1.F : std::chrono::duration<float, std::milli>(NOW - fade.last).count() / MS;
        fade.last        = NOW;
        fade.alpha       = fade.alpha < target ? std::min(target, fade.alpha + STEP) : std::max(target, fade.alpha - STEP);
        return fade.alpha;
    }

    void damage(const PHLWINDOW& window) {
        g_pHyprRenderer->damageBox(window->getFullWindowBoundingBox());
    }

    bool hkShouldRenderWindow(void* thisptr, PHLWINDOW window, PHLMONITOR monitor) {
        const bool ORIGINAL = rc<origShouldRenderWindow>(g_shouldRenderHook->m_original)(thisptr, window, monitor);
        const auto OWNER    = spanOwner(window, monitor);
        if (!OWNER)
            return ORIGINAL;
        const float TARGET = targetOf(window, OWNER);
        const float ALPHA  = fadeOf(window, TARGET);
        if (ALPHA != TARGET)
            damage(window); // fading: draw again
        return ALPHA > 0.F;
    }

    void hkRenderWindow(void* thisptr, Render::CRenderContext& ctx, PHLWINDOW window, PHLMONITOR monitor, const Render::SWindowRenderPresentation& presentation,
                        const Time::steady_tp& time, bool decorate, Render::eRenderPassMode mode, bool ignorePosition, bool standalone) {
        const auto ORIGINAL = rc<origRenderWindow>(g_renderWindowHook->m_original);
        const auto OWNER    = ignorePosition || standalone || presentation.workspaceScene ? nullptr : spanOwner(window, monitor);
        const auto FADE     = OWNER ? g_fades.find(window.get()) : g_fades.end();
        if (FADE == g_fades.end() || FADE->second.window.lock() != window) {
            ORIGINAL(thisptr, ctx, window, monitor, presentation, time, decorate, mode, ignorePosition, standalone);
            return;
        }
        if (FADE->second.alpha <= 0.F)
            return;
        // Where it lies at rest, whatever the owner's view is doing.
        auto shown            = presentation;
        shown.workspaceOffset = {};
        shown.fadeAlpha *= FADE->second.alpha;
        shown.alphaVisible = true;
        ORIGINAL(thisptr, ctx, window, monitor, shown, time, decorate, mode, ignorePosition, standalone);
    }

    void* findFn(HANDLE handle, const std::string& name, const std::string& needle) {
        for (const auto& fn : HyprlandAPI::findFunctionsByName(handle, name)) {
            if (fn.demangled.find(needle) != std::string::npos)
                return fn.address;
        }
        return nullptr;
    }

    CFunctionHook* hookFn(HANDLE handle, const std::string& name, const std::string& needle, void* destination) {
        const auto ADDRESS = findFn(handle, name, needle);
        const auto HOOK    = ADDRESS ? HyprlandAPI::createFunctionHook(handle, ADDRESS, destination) : nullptr;
        return HOOK && HOOK->hook() ? HOOK : nullptr;
    }
}

void Span::init(HANDLE handle) {
    using namespace ::Config::Values;
    HyprlandAPI::addConfigValueV2(handle,
                                  makeShared<CBoolValue>(ENABLED.c_str(), "fade the part of a floating window on another monitor while its own view moves", true));
    HyprlandAPI::addConfigValueV2(handle, makeShared<CIntValue>(FADE.c_str(), "fade duration of the part of a floating window on another monitor, ms", 150, SIntValueOptions{.min = 0}));

    g_shouldRenderHook = hookFn(handle, "shouldRenderWindow",
                                "IHyprRenderer::shouldRenderWindow(Hyprutils::Memory::CSharedPointer<Desktop::View::CWindow>, Hyprutils::Memory::CSharedPointer<Monitor::CMonitor>)",
                                rc<void*>(hkShouldRenderWindow));
    g_renderWindowHook = hookFn(handle, "renderWindow", "IHyprRenderer::renderWindow(Render::CRenderContext&", rc<void*>(hkRenderWindow));
    g_hooked           = g_shouldRenderHook && g_renderWindowHook;
    if (!g_hooked) {
        exit();
        HyprlandAPI::addNotification(handle, "[hyprgrid] could not hook the renderer: floating windows across monitors keep Hyprland's drawing", CHyprColor{1.0, 0.6, 0.2, 1.0},
                                     5000);
    }
}

void Span::exit() {
    g_hooked = false;
    for (auto* hook : {g_renderWindowHook, g_shouldRenderHook}) {
        if (hook)
            hook->unhook();
    }
    g_fades.clear();
}

void Span::viewChanged(const PHLMONITOR& monitor) {
    if (!monitor || !g_pHyprRenderer || !enabled())
        return;
    std::erase_if(g_fades, [](const auto& entry) { return !entry.second.window.lock(); });
    for (const auto& window : Desktop::windowState()->windows()) {
        if (!window || !window->isFloating() || !window->m_workspace || window->m_workspace->m_monitor.lock() != monitor)
            continue;
        const auto BOX = window->getFullWindowBoundingBox();
        const auto ON  = BOX.intersection(monitor->logicalBox());
        if (ON.w < BOX.w || ON.h < BOX.h)
            damage(window); // lies partly elsewhere
    }
}
