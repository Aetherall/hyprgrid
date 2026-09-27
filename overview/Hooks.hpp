// The overview's hooks into Hyprland (Hooks.cpp): its renderer, frames and
// damage, installed while a screen is open; and its dispatchers.
#pragma once

namespace Overview::Hooks {
    // Install the render, frame and damage hooks; false if another overview
    // plugin holds them.
    bool enable();
    void disable();
    // The hook carrying a SUPER + drag into the overview, installed while
    // cross_monitor_drag is on and a screen is open.
    void reconcileNativeDrag();
    // The plugin is unloading: open nothing.
    bool unloading();

    // Inside, the render hook lets Hyprland draw workspaces itself (while a
    // screen is built, and the overview's own drawing).
    class CRendering {
      public:
        CRendering();
        ~CRendering();

      private:
        bool m_previous;
    };
}
