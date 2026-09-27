// The overview: hyprgrid's workspaces drawn zoomed out on their grid cells,
// windows live. Derived from hyprland-scroll-overview (see LICENSE); its
// entry points, called from hyprgrid's (main.cpp).
#pragma once

#include "Motion.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>

namespace Overview {
    void init(HANDLE handle);
    void exit();

    // An overview is drawing `monitor`'s workspaces.
    bool drawing(const PHLMONITOR& monitor);
    // The view moved or zoomed: redraw from it.
    void sync(const PHLMONITOR& monitor);
    // The zoom changed: open as it leaves 0, start closing as it heads back,
    // go once it is there.
    void zoomChanged(const PHLMONITOR& monitor, float zoom, float goal);
    // One cell's size on screen at `zoom`, in logical pixels.
    Motion::SPoint cellPitch(const PHLMONITOR& monitor, float zoom);
}
