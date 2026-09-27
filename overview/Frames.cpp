// Frames and damage while a screen is zoomed out (see Screen.hpp). The
// selected workspace stays live; the others are previews, framed at most
// every OVERVIEW_WINDOW_FRAME_INTERVAL. Windows hidden to Hyprland (on other
// workspaces) get their frame callbacks from here, or they would stop
// drawing.

#include "Internal.hpp"
#include "Session.hpp"
#include "View.hpp"

namespace Overview {
    static constexpr std::chrono::milliseconds OVERVIEW_WINDOW_FRAME_INTERVAL = std::chrono::milliseconds(33);

    // At least this often, even with nothing moving: misc:render_unfocused_fps.
    static std::chrono::milliseconds idleFrameInterval() {
        const int FPS = std::clamp<int>(Overview::Config::getValue<int>("misc:render_unfocused_fps"), 1, 240);
        return std::chrono::milliseconds(std::max(1, 1000 / FPS));
    }

    static bool surfaceTreeHasFrameCallbacks(SP<CWLSurfaceResource> surface) {
        if (!surface)
            return false;
        bool has = false;
        surface->breadthfirst(
            [&has](SP<CWLSurfaceResource> child, const Vector2D&, void*) {
                if (child && !child->m_current.callbacks.empty())
                    has = true;
            },
            nullptr);
        return has;
    }

    static void surfaceTreePresent(SP<CWLSurfaceResource> surface, PHLMONITOR monitor, const Time::steady_tp& now) {
        if (!surface)
            return;
        std::pair<PHLMONITOR, Time::steady_tp> data = {monitor, now};
        surface->breadthfirst(
            [](SP<CWLSurfaceResource> child, const Vector2D&, void* data) {
                if (!child)
                    return;
                const auto [MONITOR, NOW] = *sc<std::pair<PHLMONITOR, Time::steady_tp>*>(data);
                child->presentFeedback(NOW, MONITOR, false);
            },
            &data);
    }

    static bool windowAnimating(const PHLWINDOW& window) {
        return window && (window->positionAnimation()->isBeingAnimated() || window->sizeAnimation()->isBeingAnimated() || window->alpha().isBeingAnimated());
    }

    static bool layerAnimating(const PHLLS& layer) {
        return Desktop::View::validMapped(layer) && (layer->positionAnimation()->isBeingAnimated() || layer->sizeAnimation()->isBeingAnimated() || layer->m_alpha.isBeingAnimated());
    }

    // The window or layer a surface belongs to, through popups.
    static std::pair<PHLLS, PHLWINDOW> ownerOf(SP<CWLSurfaceResource> surface) {
        const auto HLSURFACE = Desktop::View::CWLSurface::fromResource(surface);
        const auto VIEW      = HLSURFACE ? HLSURFACE->view() : nullptr;
        if (!VIEW)
            return {};
        auto layer  = Desktop::View::CLayerSurface::fromView(VIEW);
        auto window = Desktop::View::CWindow::fromView(VIEW);
        if (!layer && !window) {
            if (const auto POPUP = Desktop::View::CPopup::fromView(VIEW)) {
                if (const auto OWNER = POPUP->getT1Owner(); OWNER && OWNER->view()) {
                    layer  = Desktop::View::CLayerSurface::fromView(OWNER->view());
                    window = Desktop::View::CWindow::fromView(OWNER->view());
                }
            }
        }
        return {layer, window};
    }

    void CScreen::damage() {
        blockDamageReporting = true;
        g_pHyprRenderer->damageMonitor(monitor.lock());
        blockDamageReporting = false;
    }

    void CScreen::requestInputFrame() {
        const auto MONITOR = monitor.lock();
        if (closing || !MONITOR)
            return;
        inputFramePending = true;
        MONITOR->scheduleFrame(Aquamarine::IOutput::AQ_SCHEDULE_CURSOR_MOVE);
    }

    bool CScreen::isSelected(const PHLWORKSPACE& workspace) const {
        return workspace && workspace == selectedWorkspace.lock() && imageOf(workspace);
    }

    bool CScreen::hasRunningWorkspaceAnimation() const {
        return View::moving(monitor.lock()) || insertProgress->isBeingAnimated() || insertFadeProgress->isBeingAnimated();
    }

    bool CScreen::shouldAllowRealtimePreviewFrame() const {
        return lastRealtimePreviewFrame.time_since_epoch().count() == 0 || Time::steadyNow() - lastRealtimePreviewFrame >= OVERVIEW_WINDOW_FRAME_INTERVAL;
    }

    // A frame asked for by Hyprland (damage, a client): let it through, or
    // put it off to the next preview frame.
    bool CScreen::shouldAllowRealtimePreviewSchedule() {
        if (inputFramePending) {
            inputFramePending = false;
            return true;
        }
        if (selectedWorkspaceFramePending) {
            selectedWorkspaceFramePending = false;
            return true;
        }
        if (closing || View::moving(monitor.lock()))
            return true;
        if (realtimePreviewFrameQueued) {
            scheduleRealtimePreviewFrame();
            return false;
        }
        if (shouldAllowRealtimePreviewFrame()) {
            realtimePreviewFrameQueued = true;
            return true;
        }
        scheduleRealtimePreviewFrame();
        return false;
    }

    void CScreen::schedulePreviewFrameAfter(std::chrono::milliseconds delay) {
        if (!realtimePreviewTimer)
            return;
        const auto DELAY = std::max<int>(1, sc<int>(delay.count()));
        const auto DUE   = Time::steadyNow() + std::chrono::milliseconds(DELAY);
        if (realtimePreviewTimerArmed && realtimePreviewTimerDue <= DUE)
            return;
        realtimePreviewTimerArmed = true;
        realtimePreviewTimerDue   = DUE;
        wl_event_source_timer_update(realtimePreviewTimer, DELAY);
    }

    void CScreen::scheduleMinimumPreviewFrame() {
        schedulePreviewFrameAfter(idleFrameInterval());
    }

    void CScreen::scheduleRealtimePreviewFrame() {
        const auto NOW     = Time::steadyNow();
        const auto ELAPSED = lastRealtimePreviewFrame.time_since_epoch().count() == 0 ? OVERVIEW_WINDOW_FRAME_INTERVAL :
                                                                                        std::chrono::duration_cast<std::chrono::milliseconds>(NOW - lastRealtimePreviewFrame);
        schedulePreviewFrameAfter(OVERVIEW_WINDOW_FRAME_INTERVAL - std::min(ELAPSED, OVERVIEW_WINDOW_FRAME_INTERVAL));
    }

    int CScreen::realtimePreviewTimerCallback(void* data) {
        const auto SCREEN = sc<CScreen*>(data);
        if (!SCREEN)
            return 0;
        SCREEN->realtimePreviewTimerArmed  = false;
        SCREEN->realtimePreviewTimerDue    = {};
        SCREEN->realtimePreviewFrameQueued = false;
        SCREEN->damage();
        SCREEN->scheduleMinimumPreviewFrame();
        return 0;
    }

    // Nothing on screen animates: the frame's own damage needn't go through.
    bool CScreen::shouldSuppressRenderDamage() const {
        const auto MONITOR = monitor.lock();
        if (!MONITOR || closing || View::moving(MONITOR))
            return false;

        const auto DRAGGED  = overview()->dragged();
        const auto animated = [&](const PHLWINDOW& window, const SWorkspaceImage& image) {
            return isShown(window) && window != DRAGGED && onMonitor(windowBox(window, image), MONITOR) && windowAnimating(window);
        };

        for (const auto& ref : pinnedFloatingWindows) {
            const auto WINDOW = shown(ref.lock());
            if (isPinnedFloating(WINDOW) && WINDOW->m_monitor == MONITOR && windowAnimating(WINDOW))
                return false;
        }

        for (const auto& image : images) {
            if (!onMonitor(visibleBox(image, workspaceBox(image), scale->value()), MONITOR))
                continue;
            if (const auto FS = fullscreenOf(image.workspace)) {
                if (animated(FS, image))
                    return false;
                for (const auto& ref : image.windows) {
                    const auto WINDOW = shown(ref.lock());
                    if (WINDOW && WINDOW->isFloating() && animated(WINDOW, image))
                        return false;
                }
                continue;
            }
            for (const auto& ref : image.windows) {
                if (animated(shown(ref.lock()), image))
                    return false;
            }
        }

        for (const auto LAYER : {ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM}) {
            for (const auto& ref : MONITOR->m_layerSurfaceLayers[LAYER]) {
                if (layerAnimating(ref.lock()))
                    return false;
            }
        }
        return true;
    }

    // After drawing: frame callbacks to what is on screen, the previews at
    // their pace.
    void CScreen::sendFrameCallbacks(const Time::steady_tp& now) {
        const auto MONITOR = monitor.lock();
        if (!MONITOR)
            return;

        const auto DRAGGED   = overview()->dragged();
        const bool CANPREVIEW = closing || shouldAllowRealtimePreviewFrame();
        bool       sentPreview = false;

        const bool PREVSENDING = sendingFrameCallbacks;
        sendingFrameCallbacks  = CANPREVIEW;
        auto restoreSending    = Hyprutils::Utils::CScopeGuard([this, PREVSENDING] { sendingFrameCallbacks = PREVSENDING; });

        const auto frame = [&](const PHLWINDOW& window, const SWorkspaceImage& image, bool live) {
            if (!isShown(window))
                return;
            const bool ISDRAGGED = window == DRAGGED;
            if (!ISDRAGGED && !onMonitor(windowBox(window, image), MONITOR))
                return;
            if (!live && !ISDRAGGED && !CANPREVIEW) {
                scheduleRealtimePreviewFrame();
                return;
            }
            surfaceTreePresent(window->wlSurface() ? window->wlSurface()->resource() : nullptr, MONITOR, now);
            if (!live && !ISDRAGGED)
                sentPreview = true;
        };

        for (const auto& ref : pinnedFloatingWindows) {
            const auto WINDOW = shown(ref.lock());
            if (!isPinnedFloating(WINDOW) || WINDOW->m_monitor != MONITOR)
                continue;
            if (!CANPREVIEW) {
                scheduleRealtimePreviewFrame();
                continue;
            }
            surfaceTreePresent(WINDOW->wlSurface() ? WINDOW->wlSurface()->resource() : nullptr, MONITOR, now);
            sentPreview = true;
        }

        for (const auto& image : images) {
            if (!onMonitor(visibleBox(image, workspaceBox(image), scale->value()), MONITOR))
                continue;
            const bool LIVE = isSelected(image.workspace);
            if (const auto FS = fullscreenOf(image.workspace)) {
                frame(FS, image, LIVE);
                for (const auto& ref : image.windows) {
                    const auto WINDOW = shown(ref.lock());
                    if (WINDOW && WINDOW->isFloating())
                        frame(WINDOW, image, LIVE);
                }
                continue;
            }
            for (const auto& ref : image.windows)
                frame(shown(ref.lock()), image, LIVE);
        }

        if (sentPreview)
            lastRealtimePreviewFrame = now;
        realtimePreviewFrameQueued = false;

        for (const auto LAYER : {ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, ZWLR_LAYER_SHELL_V1_LAYER_TOP, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY}) {
            for (const auto& ref : MONITOR->m_layerSurfaceLayers[LAYER]) {
                const auto LS      = ref.lock();
                const auto SURFACE = LS && LS->wlSurface() ? LS->wlSurface()->resource() : nullptr;
                if (Desktop::View::validMapped(LS) && surfaceTreeHasFrameCallbacks(SURFACE))
                    surfaceTreePresent(SURFACE, MONITOR, now);
            }
        }
    }

    // A client asks for a frame: live windows get it, previews at their pace,
    // windows off screen or behind a fullscreen one not at all.
    bool CScreen::shouldAllowSurfaceFrame(SP<CWLSurfaceResource> surface, const Time::steady_tp&) {
        const auto MONITOR = monitor.lock();
        if (!MONITOR || closing || !surface)
            return true;

        const auto [LAYER, OWNER] = ownerOf(surface);
        if (LAYER || (!LAYER && !OWNER))
            return true;

        const auto WINDOW = shown(OWNER);
        if (WINDOW && WINDOW == overview()->dragged())
            return true;
        if (!WINDOW || WINDOW->m_monitor != MONITOR)
            return true;

        if (isPinnedFloating(WINDOW)) {
            if (sendingFrameCallbacks)
                return true;
            scheduleRealtimePreviewFrame();
            return false;
        }
        if (!isShown(WINDOW) || !WINDOW->m_workspace)
            return true;

        const auto IMAGE = imageOf(WINDOW->m_workspace);
        if (!IMAGE)
            return false;
        if (const auto FS = fullscreenOf(IMAGE->workspace); FS && FS != WINDOW && !WINDOW->isFloating())
            return false;
        if (!onMonitor(windowBox(WINDOW, *IMAGE), MONITOR))
            return false;
        if (isSelected(IMAGE->workspace) || sendingFrameCallbacks)
            return true;
        scheduleRealtimePreviewFrame();
        return false;
    }

    // A surface reports damage: whether it goes through now.
    bool CScreen::shouldHandleSurfaceDamage(SP<CWLSurfaceResource> surface) {
        const auto MONITOR = monitor.lock();
        if (!MONITOR || closing || !surface)
            return true;

        const auto [LAYER, OWNER] = ownerOf(surface);
        if (LAYER) {
            if (LAYER->m_monitor != MONITOR)
                return true;
            if (LAYER->m_layer > ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM)
                return false;
            overviewBlurDirty = true;
            if (LAYER->m_layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND)
                backdropBlurDirty = true;
            return true;
        }
        if (!OWNER)
            return true;

        const auto WINDOW = shown(OWNER);
        if (WINDOW && WINDOW == overview()->dragged()) {
            const auto BOX = overview()->draggedGlobalBox();
            if (!BOX.empty() && !BOX.intersection(MONITOR->logicalBox()).empty())
                damage();
            return true;
        }

        if (isPinnedFloating(WINDOW)) {
            if (WINDOW->m_monitor != MONITOR)
                return true;
            if (!realtimePreviewFrameQueued && shouldAllowRealtimePreviewFrame())
                return true;
            scheduleRealtimePreviewFrame();
            return false;
        }

        if (WINDOW && WINDOW->m_monitor != MONITOR)
            return true;
        if (!isShown(WINDOW) || !WINDOW->m_workspace)
            return false;

        const auto IMAGE = imageOf(WINDOW->m_workspace);
        if (!IMAGE)
            return false;
        if (const auto FS = fullscreenOf(IMAGE->workspace); FS && FS != WINDOW && !WINDOW->isFloating())
            return false;
        if (!onMonitor(windowBox(WINDOW, *IMAGE), MONITOR))
            return false;
        if (isSelected(IMAGE->workspace)) {
            selectedWorkspaceFramePending = true;
            return true;
        }
        if (!realtimePreviewFrameQueued && shouldAllowRealtimePreviewFrame())
            return true;
        scheduleRealtimePreviewFrame();
        return false;
    }
}
