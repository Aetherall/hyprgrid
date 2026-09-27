// What Hyprland hides and a zoomed-out screen draws anyway (see Screen.hpp):
// surfaces culled by what covers them, windows behind a fullscreen one,
// layers below it. Each is forced while drawn and restored when the screen
// goes. And the bar: over a fullscreen window it hides, so while zoomed out
// the window's fullscreen state is announced as off.

#include "Internal.hpp"
#include "Screen.hpp"
#include "Window.hpp"

namespace Overview {
    void CScreen::forceSurfaceVisible(SP<CWLSurfaceResource> surface) {
        const auto HLSURFACE = surface ? Desktop::View::CWLSurface::fromResource(surface) : nullptr;
        if (!HLSURFACE)
            return;
        if (std::ranges::none_of(forcedSurfaces, [&](const auto& entry) { return entry.surface.lock() == surface; }))
            forcedSurfaces.push_back({surface, HLSURFACE->m_visibleRegion});
        HLSURFACE->m_visibleRegion = {};
    }

    void CScreen::forceWindowSurfacesVisible(const PHLWINDOW& window) {
        if (!window || !window->wlSurface() || !window->wlSurface()->resource())
            return;

        window->wlSurface()->resource()->breadthfirst([this](SP<CWLSurfaceResource> surface, const Vector2D&, void*) { forceSurfaceVisible(surface); }, nullptr);
        if (window->backend().isX11() || !window->popupHead())
            return;
        window->popupHead()->breadthfirst(
            [this](SP<Desktop::View::CPopup> popup, void*) {
                if (!popup || !popup->mapped() || !popup->wlSurface() || !popup->wlSurface()->resource())
                    return;
                popup->wlSurface()->resource()->breadthfirst([this](SP<CWLSurfaceResource> surface, const Vector2D&, void*) { forceSurfaceVisible(surface); }, nullptr);
            },
            nullptr);
    }

    void CScreen::forceWindowVisible(const PHLWINDOW& window) {
        if (!window)
            return;
        if (std::ranges::none_of(forcedWindows, [&](const auto& entry) { return entry.window == window; }))
            forcedWindows.push_back({window, window->isHidden()});
        window->setHidden(false);
        window->presentation().alpha(Desktop::View::WINDOW_ALPHA_FULLSCREEN)->setValueAndWarp(1.F);
    }

    void CScreen::forceLayersAboveFullscreen() {
        const auto MONITOR = monitor.lock();
        if (!MONITOR)
            return;
        for (const auto LEVEL : {ZWLR_LAYER_SHELL_V1_LAYER_TOP, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY}) {
            for (const auto& ls : MONITOR->m_layerSurfaceLayers[LEVEL]) {
                if (!ls)
                    continue;
                auto& fade = ls->alpha()[Desktop::View::LS_ALPHA_FADE];
                if (std::ranges::none_of(forcedLayers, [&](const auto& entry) { return entry.layer == ls; }))
                    forcedLayers.push_back({ls, bool(ls->m_flags & Desktop::View::LAYER_FLAG_ABOVE_FULLSCREEN), fade->value()});
                ls->m_flags |= Desktop::View::LAYER_FLAG_ABOVE_FULLSCREEN;
                if (fade->value() != 1.F || fade->goal() != 1.F || fade->isBeingAnimated())
                    fade->setValueAndWarp(1.F);
            }
        }
    }

    void CScreen::restoreForcedVisibility() {
        for (const auto& entry : forcedSurfaces) {
            const auto SURFACE   = entry.surface.lock();
            const auto HLSURFACE = SURFACE ? Desktop::View::CWLSurface::fromResource(SURFACE) : nullptr;
            if (HLSURFACE)
                HLSURFACE->m_visibleRegion = entry.visibleRegion;
        }
        forcedSurfaces.clear();

        // A group's windows are hidden by the group, not by us: it redoes them.
        std::vector<SP<Desktop::View::CGroup>> groups;
        for (const auto& entry : forcedWindows) {
            const auto WINDOW = entry.window.lock();
            if (!WINDOW)
                continue;
            WINDOW->updateFullscreenInputState();
            *WINDOW->presentation().alpha(Desktop::View::WINDOW_ALPHA_FULLSCREEN) = WINDOW->isBlockedByFullscreen() ? 0.F : 1.F;
            if (const auto GROUP = WINDOW->grouping().group()) {
                if (std::ranges::find(groups, GROUP) == groups.end())
                    groups.push_back(GROUP);
                continue;
            }
            WINDOW->setHidden(entry.hidden);
        }
        for (const auto& group : groups) {
            if (group)
                group->updateWindowVisibility();
        }
        forcedWindows.clear();

        for (const auto& entry : forcedLayers) {
            if (!entry.layer)
                continue;
            if (entry.aboveFullscreen)
                entry.layer->m_flags |= Desktop::View::LAYER_FLAG_ABOVE_FULLSCREEN;
            else
                entry.layer->m_flags &= ~Desktop::View::LAYER_FLAG_ABOVE_FULLSCREEN;

            auto&      fade    = entry.layer->alpha()[Desktop::View::LS_ALPHA_FADE];
            const auto MONITOR = entry.layer->m_monitor.lock();
            if (!MONITOR) {
                fade->setValueAndWarp(entry.alpha);
                continue;
            }
            const bool FULLSCREEN = Fullscreen::controller()->hasFullscreen(MONITOR);
            const bool VISIBLE    = !FULLSCREEN || entry.layer->m_layer >= ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY || (entry.layer->m_flags & Desktop::View::LAYER_FLAG_ABOVE_FULLSCREEN);
            fade->setValueAndWarp(VISIBLE ? 1.F : 0.F);
        }
        forcedLayers.clear();
    }

    // With animations off, decorations are laid out again at once on closing.
    void CScreen::recalcDecorations(const PHLWORKSPACE& workspace) {
        if (const auto IMAGE = imageOf(workspace)) {
            for (const auto& ref : IMAGE->windows)
                OverviewWindow::forceDecoRecalc(ref.lock());
        }
    }

    void CScreen::emitFullscreenState(PHLWINDOW window, bool hide) {
        if (emittingFullscreenState)
            return;
        emittingFullscreenState = true;
        auto reset              = Hyprutils::Utils::CScopeGuard([this] { emittingFullscreenState = false; });

        window = shown(window);
        if (!validMapped(window) || !window->m_workspace || window->m_monitor != monitor) {
            IPC::Socket2::sock()->postEvent(IPC::Socket2::SEvent{.event = "fullscreen", .data = "0"});
            return;
        }

        if (!hide || !Fullscreen::controller()->isFullscreen(window)) {
            Event::bus()->m_events.window.fullscreen.emit(window);
            IPC::Socket2::sock()->postEvent(IPC::Socket2::SEvent{.event = "fullscreen", .data = Fullscreen::controller()->isFullscreen(window) ? "1" : "0"});
            return;
        }

        // Announced as off, then put back as it was.
        const auto MODES = Fullscreen::controller()->getFullscreenModes(window);
        Fullscreen::controller()->setFullscreenMode(window, Fullscreen::FSMODE_NONE, Fullscreen::FSMODE_NONE);
        Event::bus()->m_events.window.fullscreen.emit(window);
        IPC::Socket2::sock()->postEvent(IPC::Socket2::SEvent{.event = "fullscreen", .data = "0"});
        Fullscreen::controller()->setFullscreenMode(window, MODES.internal, MODES.client);
    }
}
