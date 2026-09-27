// The overview (see Session.hpp): its screens, one per monitor zoomed out,
// and what Hyprland is made to do while any is open. Also the entry points
// hyprgrid's view calls (Overview.hpp).

#include "Internal.hpp"
#include "Session.hpp"
#include "Hooks.hpp"
#include "Later.hpp"
#include "Overview.hpp"
#include "View.hpp"

namespace Overview {
    static UP<COverview> g_overview;

    COverview* overview() {
        return g_overview.get();
    }

    void createOverview() {
        g_overview = makeUnique<COverview>();
    }

    void destroyOverview() {
        g_overview.reset();
    }

    COverview::COverview() = default;

    COverview::~COverview() {
        // Each screen restores what it forced; then the session what it did.
        auto screens = std::move(m_screens);
        m_screens.clear();
        screens.clear();
        if (m_started)
            stop();
    }

    // --- Screens ---

    const std::vector<SP<CScreen>>& COverview::screens() const {
        return m_screens;
    }

    SP<CScreen> COverview::find(const CScreen* screen) const {
        const auto IT = std::ranges::find_if(m_screens, [screen](const auto& s) { return s.get() == screen; });
        return IT == m_screens.end() ? nullptr : *IT;
    }

    SP<CScreen> COverview::screenFor(const PHLMONITOR& monitor) const {
        if (!monitor)
            return nullptr;
        const auto IT = std::ranges::find_if(m_screens, [&](const auto& s) { return s->monitor.lock() == monitor; });
        return IT == m_screens.end() ? nullptr : *IT;
    }

    SP<CScreen> COverview::screenAt(const Vector2D& point) const {
        const auto IT = std::ranges::find_if(m_screens, [&](const auto& s) {
            const auto MONITOR = s->monitor.lock();
            return MONITOR && MONITOR->logicalBox().containsPoint(point);
        });
        return IT == m_screens.end() ? nullptr : *IT;
    }

    SP<CScreen> COverview::activeScreen() const {
        if (const auto SCREEN = screenFor(Desktop::focusState()->monitor()))
            return SCREEN;
        return m_screens.empty() ? nullptr : m_screens.front();
    }

    SP<CScreen> COverview::open(const PHLMONITOR& monitor, bool zoom, bool* created) {
        if (created)
            *created = false;
        if (!monitor)
            return nullptr;
        if (const auto SCREEN = screenFor(monitor))
            return SCREEN;
        if (!Hooks::enable())
            return nullptr;

        if (!m_started)
            start();

        SP<CScreen> screen;
        {
            Hooks::CRendering rendering;
            screen = makeShared<CScreen>(monitor);
        }
        m_screens.push_back(screen);
        Hooks::reconcileNativeDrag();
        if (created)
            *created = true;
        // Opened here rather than by the zoom leaving 0 (a gesture): zoom out.
        if (zoom)
            View::zoomTo(monitor, 1.F);
        return screen;
    }

    void COverview::remove(CScreen* screen_) {
        auto SCREEN = find(screen_);
        if (!SCREEN)
            return;
        const auto MONITOR = SCREEN->monitor.lock();

        if (drag.source.lock() == SCREEN)
            cancelDrag();
        if (m_grab.lock() == SCREEN)
            m_grab.reset();
        std::erase_if(m_linked, [&](const auto& ref) { return ref.lock() == SCREEN || !ref.lock(); });
        std::erase_if(drag.transients, [&](const auto& ref) { return ref.lock() == SCREEN || !ref.lock(); });
        std::erase(m_screens, SCREEN);
        // Gone now (even from inside its own call): it restores what it
        // forced before the session restores the rest.
        SCREEN.reset();

        if (m_screens.empty()) {
            stop();
            Hooks::disable();
        }
        Hooks::reconcileNativeDrag();

        if (MONITOR) {
            MONITOR->recheckSolitary();
            g_pHyprRenderer->damageMonitor(MONITOR);
        }
    }

    void COverview::closeAll() {
        const auto SCREENS = m_screens;
        for (const auto& screen : SCREENS)
            screen->close();
    }

    bool COverview::closeLinked() {
        std::erase_if(m_linked, [this](const auto& ref) { return !find(ref.lock().get()); });
        if (m_linked.size() < 2) {
            m_linked.clear();
            return false;
        }
        std::vector<SP<CScreen>> members;
        for (const auto& ref : m_linked)
            members.push_back(ref.lock());
        for (const auto& screen : members)
            screen->close();
        return true;
    }

    void COverview::screenClosing(CScreen* screen) {
        const auto SCREEN = find(screen);
        std::erase_if(m_linked, [&](const auto& ref) { return ref.lock() == SCREEN || !ref.lock(); });
        if (m_linked.size() < 2)
            m_linked.clear();
        if (SCREEN && drag.source.lock() == SCREEN)
            cancelDrag();
        releaseForwardedButtons(Time::millis(Time::steadyNow()));
        if (std::ranges::all_of(m_screens, [](const auto& s) { return s->isClosing(); }))
            restoreSubmap();
    }

    void COverview::screenReopened() {
        activateSubmap();
    }

    // --- While any screen is open ---

    void COverview::start() {
        m_started    = true;
        m_usesSubmap = hasSubmap();
        applyOverrides();
        g_pInputManager->unconstrainMouse();
        Pointer::Cursor::overrideController->setOverride("left_ptr", Pointer::Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);

        auto& events  = Event::bus()->m_events;
        m_mouseMove   = events.input.mouse.move.listen([this](Vector2D, Event::SCallbackInfo& info) { pointerMoved(info); });
        m_mouseButton = events.input.mouse.button.listen([this](IPointer::SButtonEvent event, Event::SCallbackInfo& info) { pointerButton(event, info); });
        m_touchMove   = events.input.touch.motion.listen([this](ITouch::SMotionEvent, Event::SCallbackInfo& info) { touchMoved(info); });
        m_touchDown   = events.input.touch.down.listen([this](auto, Event::SCallbackInfo& info) { touchDown(info); });
        m_key         = events.input.keyboard.key.listen([this](IKeyboard::SKeyEvent event, Event::SCallbackInfo& info) { keyPressed(event, info); });

        const auto dirty = [this] {
            for (const auto& screen : m_screens) {
                if (!screen->isClosing())
                    screen->markDirty();
            }
        };
        m_windowOpen  = events.window.open.listen([dirty](PHLWINDOW) { dirty(); });
        m_windowMove  = events.window.moveToWorkspace.listen([dirty](PHLWINDOW, PHLWORKSPACE) { dirty(); });
        m_windowClose = events.window.close.listen([this, dirty](PHLWINDOW window) {
            if (dragging() && shown(window) == dragged())
                cancelDrag();
            dirty();
        });
        m_windowActive = events.window.active.listen([this](PHLWINDOW window, Desktop::eFocusReason) {
            for (const auto& screen : std::vector{m_screens})
                screen->onWindowActive(window);
        });
        m_windowFullscreen = events.window.fullscreen.listen([this](PHLWINDOW window) {
            for (const auto& screen : std::vector{m_screens})
                screen->onWindowFullscreen(window);
        });
        m_workspaceCreated = events.workspace.created.listen([dirty](auto) { dirty(); });
        m_workspaceRemoved = events.workspace.removed.listen([dirty](auto) { dirty(); });

        activateSubmap();
    }

    void COverview::stop() {
        releaseForwardedButtons(Time::millis(Time::steadyNow()));
        m_pointer.reset();
        m_grab.reset();
        m_pointerScreen.reset();
        clearDrag();
        drag.transients.clear();
        m_linked.clear();

        for (auto* listener : {&m_mouseMove, &m_mouseButton, &m_touchMove, &m_touchDown, &m_key, &m_windowOpen, &m_windowClose, &m_windowMove, &m_windowActive, &m_windowFullscreen,
                               &m_workspaceCreated, &m_workspaceRemoved})
            listener->reset();

        restoreSubmap();
        restoreOverrides();

        // Every monitor's own workspace shown again as Hyprland left it.
        for (const auto& monitor : State::monitorState()->monitors()) {
            if (!monitor)
                continue;
            for (const auto& workspace : {monitor->m_activeWorkspace, monitor->m_activeSpecialWorkspace}) {
                if (!workspace)
                    continue;
                workspace->setVisible(true);
                workspace->m_alpha->setValueAndWarp(1.F);
                workspace->m_renderOffset->setValueAndWarp(Vector2D{});
            }
            g_pHyprRenderer->damageMonitor(monitor);
        }
        Pointer::Cursor::overrideController->unsetOverride(Pointer::Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);
        m_started = false;
    }

    // Workspace switches don't slide (the view shows them), every workspace
    // is opaque, and the pointer neither warps nor moves focus.
    void COverview::applyOverrides() {
        if (m_overridden)
            return;
        m_overridden = true;

        m_savedAnimations.clear();
        for (const std::string name : {"workspaces", "workspacesIn", "workspacesOut"}) {
            if (const auto CONFIG = ::Config::animationTree()->getAnimationPropertyConfig(name))
                m_savedAnimations.push_back({name, *CONFIG});
        }
        for (const auto& saved : m_savedAnimations)
            ::Config::animationTree()->setConfigForNode(saved.name, false, 1.F, "default", "");

        for (const auto& workspace : State::workspaceState()->workspaces()) {
            if (!workspace || !workspace->m_alpha)
                continue;
            workspace->m_alpha->setValueAndWarp(1.F);
            *workspace->m_alpha = 1.F;
        }

        using Overview::Config::getValue, Overview::Config::setValue;
        m_noWarps                    = getValue<int>("cursor:no_warps");
        m_warpOnChangeWorkspace      = getValue<int>("cursor:warp_on_change_workspace");
        m_warpOnToggleSpecial        = getValue<int>("cursor:warp_on_toggle_special");
        m_warpBackAfterNonMouseInput = getValue<int>("cursor:warp_back_after_non_mouse_input");
        m_followMouse                = getValue<int>("input:follow_mouse");
        setValue("cursor:no_warps", 1);
        setValue("cursor:warp_on_change_workspace", 0);
        setValue("cursor:warp_on_toggle_special", 0);
        setValue("cursor:warp_back_after_non_mouse_input", 0);
        setValue("input:follow_mouse", 0);
    }

    void COverview::restoreOverrides() {
        if (!m_overridden)
            return;
        m_overridden = false;

        // Their children inherit their values again.
        const auto propagate = [](const SP<Hyprutils::Animation::SAnimationPropertyConfig>& parent, auto&& self) -> void {
            if (!parent)
                return;
            for (const auto& [name, animation] : ::Config::animationTree()->getAnimationConfig()) {
                if (!animation || animation->overridden || animation->pParentAnimation != parent)
                    continue;
                animation->pValues = parent->pValues;
                self(animation, self);
            }
        };
        for (const auto& saved : m_savedAnimations) {
            if (const auto CONFIG = ::Config::animationTree()->getAnimationPropertyConfig(saved.name)) {
                *CONFIG = saved.config;
                propagate(CONFIG, propagate);
            }
        }
        m_savedAnimations.clear();

        using Overview::Config::setValue;
        setValue("cursor:no_warps", m_noWarps);
        setValue("cursor:warp_on_change_workspace", m_warpOnChangeWorkspace);
        setValue("cursor:warp_on_toggle_special", m_warpOnToggleSpecial);
        setValue("cursor:warp_back_after_non_mouse_input", m_warpBackAfterNonMouseInput);
        setValue("input:follow_mouse", m_followMouse);
    }

    void COverview::activateSubmap() {
        if (!m_usesSubmap || m_submapActive || !Keybinds::mgr())
            return;
        m_previousSubmap = Keybinds::mgr()->currentSubmap();
        if (!::Config::Actions::setSubmap(SUBMAP)) {
            m_usesSubmap = false;
            return;
        }
        m_submapActive = true;
    }

    void COverview::restoreSubmap() {
        if (!m_submapActive || !Keybinds::mgr())
            return;
        if (Keybinds::mgr()->currentSubmap() == SUBMAP)
            (void)::Config::Actions::setSubmap(m_previousSubmap.empty() ? "reset" : m_previousSubmap);
        m_submapActive = false;
    }

    // --- The view's side (Overview.hpp) ---

    bool drawing(const PHLMONITOR& monitor) {
        return monitor && overview() && overview()->screenFor(monitor);
    }

    void sync(const PHLMONITOR& monitor) {
        if (const auto SCREEN = overview() ? overview()->screenFor(monitor) : nullptr)
            SCREEN->syncFromView();
    }

    void zoomChanged(const PHLMONITOR& monitor, float zoom, float goal) {
        if (Hooks::unloading() || !monitor || !overview())
            return;

        const auto SCREEN = overview()->screenFor(monitor);
        if (!SCREEN) {
            // Opened from the event loop, not from inside the animation's
            // callback (opening hooks the renderer).
            if (zoom > 0 && goal > 0)
                Later::run([mon = PHLMONITORREF{monitor}] {
                    const auto M = mon.lock();
                    if (M && overview() && !Hooks::unloading() && View::zoom(M) > 0 && !overview()->screenFor(M))
                        overview()->open(M, false);
                });
            return;
        }

        SCREEN->viewClosing(goal <= 0);

        // Removed from the event loop, not from inside the animation's
        // callback; by then the zoom may have turned back.
        if (zoom <= 0 && goal <= 0) {
            Later::run([mon = PHLMONITORREF{monitor}] {
                // By pointer: the session's list holds the screen, and drops
                // it inside (it goes before the session restores the rest).
                CScreen* screen = nullptr;
                if (const auto M = mon.lock(); M && overview()) {
                    const auto S = overview()->screenFor(M);
                    if (S && View::zoom(M) <= 0 && !View::moving(M))
                        screen = S.get();
                }
                if (screen)
                    screen->finishClosing();
            });
        }
    }

    Motion::SPoint cellPitch(const PHLMONITOR& monitor, float zoom) {
        return Scene::screenPitch(sceneMonitor(monitor), Scene::zoomScale(zoom, Overview::Config::getScale()), Overview::Config::getWorkspaceGap());
    }
}
