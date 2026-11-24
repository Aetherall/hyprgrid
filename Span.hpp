// Floating windows spanning two monitors (see Span.cpp).
#pragma once

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

namespace Span {
    void init(HANDLE handle);
    void exit();

    // The view on `monitor` moved or settled: the parts of its floating
    // windows lying on other monitors may fade out or back in.
    void viewChanged(const PHLMONITOR& monitor);
}
