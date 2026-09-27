// The view: per monitor, where you look on the grid -- a position in cells,
// animated on kinetic springs, or held where a gesture puts it. Every
// workspace switch on a monitor is shown as the view moving there, whatever
// caused it. See View.cpp.
#pragma once

#include "Motion.hpp"

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/SharedDefs.hpp>

#include <cstdint>
#include <optional>

namespace View {
    void init();
    void exit();

    // Hold the view on `monitor` from its active cell, toward the cell (dx,
    // dy) away; false if that cell can't be entered. A view still gliding is
    // held from where it is.
    bool holdToward(const PHLMONITOR& monitor, int dx, int dy);

    // Hold the view on `monitor` from wherever it is (its active cell, or
    // mid-glide); false if it can't be held (not on the grid, or held
    // already).
    bool holdHere(const PHLMONITOR& monitor);

    // Catch a view that is still gliding and hold it there. Returns where it
    // is, relative to the active cell; none if it was still.
    std::optional<Motion::SPoint> grab(const PHLMONITOR& monitor);

    bool held(const PHLMONITOR& monitor);

    struct SMove {
        Motion::SPoint         offset; // from the active cell, after the move
        std::optional<int64_t> land;   // a neighbour's centre reached: the workspace to switch to
        Motion::SCell          step;   // ...and the cell step to it
    };

    // Move the held view by `delta` cells (clamped to cells it can enter);
    // none if it isn't held. After a switch to `land`, the hold carries on
    // from that cell.
    std::optional<SMove> moveHeld(const PHLMONITOR& monitor, const Motion::SPoint& delta, uint32_t timeMs);

    struct SRelease {
        int64_t     id;   // the workspace to switch to
        Motion::SCell step; // the cell step to it
    };

    // How a let-go view lands: on the cell nearest where its velocity would
    // coast, on the next one the way it last moved (Motion::aheadLanding()),
    // or back on the active cell.
    enum class eLand {
        FLICK,
        AHEAD,
        STAY,
    };

    // Let go: the view lands as `land` says. For another cell, returns the
    // workspace to switch to, and the view carries on at the fingers' speed
    // when the switch comes; none if it stayed (or wasn't held).
    std::optional<SRelease> release(const PHLMONITOR& monitor, uint32_t timeMs, eLand land);

    // The grid moved workspaces: views follow their active workspace.
    void gridChanged();

    // One cell's size on screen on `monitor`, in logical pixels, at the
    // current zoom (zoomed in: monitor size + gaps_workspaces).
    Motion::SPoint pitch(const PHLMONITOR& monitor);

    // Where the view is on `monitor` (cells): held, gliding or settled.
    Motion::SPoint position(const PHLMONITOR& monitor);

    // The zoom: 0 the normal view, 1 the overview. zoomTo() animates it on
    // the kinetic spring, launched at `velocity` (zoom units/s); the overview
    // opens as it leaves 0 and goes when it settles back there.
    float zoom(const PHLMONITOR& monitor);
    void  zoomTo(const PHLMONITOR& monitor, float target, float velocity = 0);

    // A gesture moves the zoom by `delta` (kept within [0, 1]), holding it
    // until releaseZoom(), which says where it is, where the hold started,
    // its last movement and its speed (zoom units/ms), for the gesture to
    // settle it with zoomTo().
    void moveZoom(const PHLMONITOR& monitor, float delta, uint32_t timeMs);

    struct SZoomRelease {
        float value, start, last, velocity;
    };
    std::optional<SZoomRelease> releaseZoom(const PHLMONITOR& monitor, uint32_t timeMs);

    // The position or zoom is animating, or held.
    bool moving(const PHLMONITOR& monitor);
}
