// What the overview's translation units share: Hyprland's headers (some
// members they need are private: opened below, as the fork always did), and
// the bridges between Hyprland's types and the pure Scene and Interaction.
// Include it first.
#pragma once

#include "Kinetics.hpp" // first: see its header

#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <linux/input-event-codes.h>

#define private public
#define protected public
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/lua/ConfigManager.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/ipc/s2/S2.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/keybinds/Manager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/pointer/cursor/CursorShapeOverrideController.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Group.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowGroupMembership.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/Popup.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/devices/ITouch.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/plugins/PluginSystem.hpp>
#include <hyprland/src/render/pass/Pass.hpp>
#include <hyprland/src/render/pass/ClearPassElement.hpp>
#include <hyprland/src/render/pass/PreBlurElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/RendererHintsPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/render/types.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/workspace/RegularWorkspace.hpp>
#include <hyprland/src/workspace/query/Query.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#include <hyprutils/string/Numeric.hpp>
#undef protected
#undef private

#include "Grid.hpp"
#include "Scene.hpp"
#include "Interaction.hpp"
#include "Config.hpp"

namespace Overview {
    // --- Between Hyprland's types and the Scene's, which does the maths ---

    inline Scene::SMonitor sceneMonitor(const PHLMONITOR& monitor) {
        return {{monitor->m_position.x, monitor->m_position.y}, {monitor->m_size.x, monitor->m_size.y}, monitor->m_scale};
    }

    inline Scene::SCamera sceneCamera(const PHLMONITOR& monitor, float scale, const Vector2D& viewOffset) {
        return {.monitor = sceneMonitor(monitor), .zoomScale = scale, .gap = double(Overview::Config::getWorkspaceGap()), .viewOffset = {viewOffset.x, viewOffset.y}};
    }

    inline CBox toCBox(const Scene::SBox& box, bool round = true) {
        CBox out{box.x, box.y, box.w, box.h};
        if (round)
            out.round();
        return out;
    }

    inline Scene::SBox sceneBox(const CBox& box) {
        return {box.x, box.y, box.width, box.height};
    }

    inline Motion::SPoint scenePoint(const Vector2D& point) {
        return {point.x, point.y};
    }

    inline Vector2D toVector(const Motion::SPoint& point) {
        return {point.x, point.y};
    }

    inline Layout::eRectCorner toRectCorner(const Interaction::SCorner& corner) {
        return corner.left ? (corner.top ? Layout::CORNER_TOPLEFT : Layout::CORNER_BOTTOMLEFT) : (corner.top ? Layout::CORNER_TOPRIGHT : Layout::CORNER_BOTTOMRIGHT);
    }

    inline Interaction::SCorner sceneCorner(Layout::eRectCorner corner) {
        return {Layout::edgeLeft(corner), Layout::edgeTop(corner)};
    }

    inline bool onMonitor(const CBox& box, const PHLMONITOR& monitor) {
        return monitor && Scene::onScreen(sceneMonitor(monitor), sceneBox(box));
    }

    inline bool fullyOnMonitor(const CBox& box, const PHLMONITOR& monitor) {
        return monitor && Scene::fullyOnScreen(sceneMonitor(monitor), sceneBox(box));
    }

    // The pointer in a monitor's own device pixels, from its top left.
    inline Vector2D pointerOn(const PHLMONITOR& monitor) {
        if (!monitor)
            return {};
        return (g_pInputManager->getMouseCoordsInternal() - monitor->m_position) * monitor->m_scale;
    }

    // --- Windows as the overview shows them ---

    // A group shows as its current window.
    inline PHLWINDOW shown(const PHLWINDOW& window) {
        if (!window)
            return nullptr;
        if (window->grouping().group())
            return window->grouping().group()->current();
        return window;
    }

    // Pinned floating windows don't belong to a workspace: drawn apart.
    inline bool isPinnedFloating(const PHLWINDOW& window) {
        const auto WINDOW = shown(window);
        return validMapped(WINDOW) && (WINDOW->m_state & Desktop::View::WINDOW_STATE_PINNED) && WINDOW->isFloating();
    }

    inline bool isShown(const PHLWINDOW& window) {
        const auto WINDOW = shown(window);
        return validMapped(WINDOW) && !isPinnedFloating(WINDOW);
    }

    // A workspace's fullscreen window, when it shows.
    inline PHLWINDOW fullscreenOf(const PHLWORKSPACE& workspace) {
        const auto WINDOW = workspace ? shown(Fullscreen::controller()->getFullscreenWindow(workspace)) : PHLWINDOW{};
        return isShown(WINDOW) && WINDOW->m_workspace == workspace ? WINDOW : PHLWINDOW{};
    }

    // The windows a list holds that show, in stacking order.
    inline std::vector<PHLWINDOW> shownWindows(const std::vector<PHLWINDOWREF>& windows) {
        std::vector<PHLWINDOW> out;
        for (const auto& ref : windows) {
            if (const auto WINDOW = shown(ref.lock()); isShown(WINDOW))
                out.push_back(WINDOW);
        }
        return out;
    }

    // The same, with the workspace's fullscreen window (its index), when it
    // shows.
    inline std::vector<PHLWINDOW> shownWindows(const std::vector<PHLWINDOWREF>& windows, const PHLWORKSPACE& workspace, std::optional<size_t>& fullscreen) {
        auto       out    = shownWindows(windows);
        const auto WINDOW = fullscreenOf(workspace);
        fullscreen.reset();
        if (!WINDOW)
            return out;
        const auto IT = std::ranges::find(out, WINDOW);
        fullscreen    = IT - out.begin();
        if (IT == out.end())
            out.push_back(WINDOW);
        return out;
    }

    // Monitor-wide boxes: a workspace's, where its windows may go.
    inline CBox monitorBoxOf(const PHLWORKSPACE& workspace, const PHLMONITOR& fallback) {
        const auto MONITOR = workspace && workspace->m_monitor ? workspace->m_monitor.lock() : fallback;
        return MONITOR ? CBox{MONITOR->m_position, MONITOR->m_size} : CBox{};
    }

    inline CBox centreInWorkspace(const CBox& box, const PHLWORKSPACE& workspace, const PHLMONITOR& fallback) {
        const auto AREA = monitorBoxOf(workspace, fallback);
        if (AREA.width <= 0 || AREA.height <= 0)
            return box;
        return toCBox(Interaction::centreIn({box.width, box.height}, sceneBox(AREA)), false);
    }

    inline CBox clampToWorkspace(const CBox& box, const PHLWORKSPACE& workspace, const PHLMONITOR& fallback, float margin = 0.F) {
        return toCBox(Interaction::clampInto(sceneBox(box), sceneBox(monitorBoxOf(workspace, fallback)), margin), false);
    }
}
