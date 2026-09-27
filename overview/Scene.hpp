// Scene: where the overview draws things -- the arithmetic from the grid, the
// view and the monitors to boxes on a monitor's screen. No Hyprland in here,
// so it is tested on its own (tests/scene.cpp); the overview asks it for
// every box it draws or hit-tests.
//
// Units: a monitor's layout is in logical pixels (Hyprland's global space);
// what comes out on screen is in the monitor's device pixels, like the
// framebuffer the overview renders to.

#pragma once

#include "Motion.hpp"
#include "Topology.hpp"

#include <algorithm>
#include <functional>
#include <vector>

namespace Scene {
    using Motion::SCell;
    using Motion::SPoint;

    struct SBox {
        double x = 0, y = 0, w = 0, h = 0;
        bool   operator==(const SBox&) const = default;
    };

    struct SMonitor {
        SPoint position, size; // logical
        double scale = 1;      // device pixels per logical pixel
    };

    // How the overview looks at the grid from one monitor.
    struct SCamera {
        SMonitor monitor;
        double   zoomScale = 1; // a workspace's size on screen over the monitor's: 1 zoomed in
        double   gap       = 0; // between cells on screen, logical
        SCell    origin;        // the cell the camera's offsets are from
        SPoint   viewOffset;    // where the view is, from the origin's centre, logical
    };

    // The zoom (0: the normal view, 1: the overview) as a scale, the
    // overview's at 1.
    inline double zoomScale(double zoom, double overviewScale) {
        return 1 + (overviewScale - 1) * std::clamp(zoom, 0.0, 1.0);
    }

    // From one cell to the next, in the monitor's logical pixels before the
    // zoom: the monitor, and the gap as it stays on screen once zoomed.
    inline SPoint logicalPitch(const SMonitor& monitor, double zoomScale, double gap) {
        const double G = gap / std::max(zoomScale, 0.01);
        return {monitor.size.x + G, monitor.size.y + G};
    }

    // The same on screen: logical, and in device pixels.
    inline SPoint screenPitch(const SMonitor& monitor, double zoomScale, double gap) {
        return {monitor.size.x * zoomScale + gap, monitor.size.y * zoomScale + gap};
    }

    inline SPoint devicePitch(const SMonitor& monitor, double zoomScale, double gap) {
        const auto P = screenPitch(monitor, zoomScale, gap);
        return {P.x * monitor.scale, P.y * monitor.scale};
    }

    // The view's offset for a view position (cells, fractional mid-glide).
    inline SPoint viewOffsetAt(const SMonitor& monitor, double zoomScale, double gap, const SPoint& position, const SCell& origin) {
        const auto P = logicalPitch(monitor, zoomScale, gap);
        return {(position.x - origin.x) * P.x, (position.y - origin.y) * P.y};
    }

    // Where a position on the grid sits from the camera's origin cell, in
    // device pixels on screen.
    inline SPoint cellOffset(const SCamera& camera, const SPoint& position) {
        const auto P = devicePitch(camera.monitor, camera.zoomScale, camera.gap);
        return {(position.x - camera.origin.x) * P.x, (position.y - camera.origin.y) * P.y};
    }

    // A workspace sliding from one cell to another (a workspace inserted or
    // removed next to it): where it is at `t` from 0 to 1, in cells.
    inline SPoint slide(const SPoint& from, const SPoint& to, double t) {
        const double T = std::clamp(t, 0.0, 1.0);
        return {from.x + (to.x - from.x) * T, from.y + (to.y - from.y) * T};
    }

    inline SPoint cellOffset(const SCamera& camera, const SCell& cell) {
        return cellOffset(camera, SPoint{double(cell.x), double(cell.y)});
    }

    // A box in the monitor's own space (logical, from its top left) onto the
    // screen: zoomed about the monitor's centre, moved by the view, then by
    // `offset` (device pixels, e.g. a cellOffset()).
    inline SBox toScreen(const SCamera& camera, const SBox& local, const SPoint& offset) {
        const double MS = camera.monitor.scale, S = camera.zoomScale;
        const double CX = camera.monitor.size.x * MS / 2, CY = camera.monitor.size.y * MS / 2;
        return {
            (local.x * MS - CX) * S + CX - camera.viewOffset.x * S * MS + offset.x,
            (local.y * MS - CY) * S + CY - camera.viewOffset.y * S * MS + offset.y,
            local.w * MS * S,
            local.h * MS * S,
        };
    }

    // toScreen()'s inverse for a point: from the screen back into the
    // monitor's own space.
    inline SPoint fromScreen(const SCamera& camera, const SPoint& point, const SPoint& offset) {
        const double MS = std::max(camera.monitor.scale, 0.01), S = std::max(camera.zoomScale, 0.01);
        const double CX = camera.monitor.size.x * MS / 2, CY = camera.monitor.size.y * MS / 2;
        return {
            (point.x - offset.x + camera.viewOffset.x * S * MS - CX) / (S * MS) + camera.monitor.size.x / 2,
            (point.y - offset.y + camera.viewOffset.y * S * MS - CY) / (S * MS) + camera.monitor.size.y / 2,
        };
    }

    // A box on screen shows on the monitor at all, or all of it does.
    inline bool onScreen(const SMonitor& monitor, const SBox& box) {
        return box.w > 0 && box.h > 0 && box.x < monitor.size.x * monitor.scale && box.x + box.w > 0 && box.y < monitor.size.y * monitor.scale && box.y + box.h > 0;
    }

    inline bool fullyOnScreen(const SMonitor& monitor, const SBox& box) {
        return box.w > 0 && box.h > 0 && box.x >= 0 && box.y >= 0 && box.x + box.w <= monitor.size.x * monitor.scale && box.y + box.h <= monitor.size.y * monitor.scale;
    }

    // A whole cell on screen, as a workspace there is drawn.
    inline SBox cellBox(const SCamera& camera, const SPoint& position) {
        return toScreen(camera, {0, 0, camera.monitor.size.x, camera.monitor.size.y}, cellOffset(camera, position));
    }

    inline SBox cellBox(const SCamera& camera, const SCell& cell) {
        return cellBox(camera, SPoint{double(cell.x), double(cell.y)});
    }

    // Another monitor's workspace in one of this monitor's cells: fitted
    // inside at its own shape, centred.
    inline double fit(const SMonitor& on, const SMonitor& source) {
        return std::min(on.size.x / std::max(source.size.x, 1.0), on.size.y / std::max(source.size.y, 1.0));
    }

    inline SPoint pad(const SMonitor& on, const SMonitor& source) {
        const double F = fit(on, source);
        return {(on.size.x - source.size.x * F) / 2, (on.size.y - source.size.y * F) / 2};
    }

    // A box in global layout space, of a window on `source`, into the space of
    // a cell on `on` (logical, from the cell's top left).
    inline SBox intoCell(const SMonitor& on, const SMonitor& source, const SBox& global, bool sameMonitor) {
        if (sameMonitor)
            return {global.x - on.position.x, global.y - on.position.y, global.w, global.h};
        const double F = fit(on, source);
        const auto   P = pad(on, source);
        return {(global.x - source.position.x) * F + P.x, (global.y - source.position.y) * F + P.y, global.w * F, global.h * F};
    }

    // intoCell()'s inverse for a point: from a cell's space back into global
    // layout space.
    inline SPoint outOfCell(const SMonitor& on, const SMonitor& source, const SPoint& local, bool sameMonitor) {
        if (sameMonitor)
            return {local.x + on.position.x, local.y + on.position.y};
        const double F = fit(on, source);
        const auto   P = pad(on, source);
        return {(local.x - P.x) / F + source.position.x, (local.y - P.y) / F + source.position.y};
    }

    // How far past a cell's edges boxes in its space reach (logical), e.g.
    // a workspace's windows: drawing it must reach that far too.
    struct SInsets {
        double left = 0, right = 0, top = 0, bottom = 0;
        bool   operator==(const SInsets&) const = default;
    };

    inline SInsets overflow(const SPoint& cellSize, const std::vector<SBox>& boxes) {
        SInsets out;
        for (const auto& b : boxes) {
            // Windows hidden under a fullscreen one can have sentinel sizes (-2x-2).
            if (b.w <= 0 || b.h <= 0)
                continue;
            out.left   = std::max(out.left, -b.x);
            out.right  = std::max(out.right, b.x + b.w - cellSize.x);
            out.top    = std::max(out.top, -b.y);
            out.bottom = std::max(out.bottom, b.y + b.h - cellSize.y);
        }
        return out;
    }

    // A box on screen grown by insets given in logical pixels, `unit` device
    // pixels each.
    inline SBox expand(const SBox& box, const SInsets& insets, double unit) {
        return {box.x - insets.left * unit, box.y - insets.top * unit, box.w + (insets.left + insets.right) * unit, box.h + (insets.top + insets.bottom) * unit};
    }

    // A cell's box on screen grown by its overflow, at the camera's zoom.
    inline SBox grow(const SCamera& camera, const SBox& box, const SInsets& insets) {
        return expand(box, insets, camera.zoomScale * camera.monitor.scale);
    }

    // The frame over where another monitor's view is (`position`, cells):
    // the cell there, shrunk to that monitor's shape.
    inline SBox frameBox(const SCamera& camera, const SMonitor& other, const SPoint& position, bool sameMonitor) {
        const auto CELL = cellBox(camera, position);
        if (sameMonitor)
            return CELL;
        const double U = camera.zoomScale * camera.monitor.scale, F = fit(camera.monitor, other);
        const auto   P = pad(camera.monitor, other);
        return {CELL.x + P.x * U, CELL.y + P.y * U, other.size.x * F * U, other.size.y * F * U};
    }

    // The seam between two regions, across the whole screen: halfway between
    // the last cell of the first region and the first of the second.
    inline SBox seamLine(const SCamera& camera, const Topology::SSeam& seam, double thickness) {
        const auto   FIRST  = cellBox(camera, seam.vertical ? SCell{seam.at - 1, 0} : SCell{0, seam.at - 1});
        const auto   SECOND = cellBox(camera, seam.vertical ? SCell{seam.at, 0} : SCell{0, seam.at});
        const double W = camera.monitor.size.x * camera.monitor.scale, H = camera.monitor.size.y * camera.monitor.scale;
        if (seam.vertical)
            return {(FIRST.x + FIRST.w + SECOND.x) / 2 - thickness / 2, 0, thickness, H};
        return {0, (FIRST.y + FIRST.h + SECOND.y) / 2 - thickness / 2, W, thickness};
    }

    // The empty cells in and around the workspaces' extent: the grid they sit on.
    inline std::vector<SCell> outlineCells(const std::vector<SCell>& workspaces) {
        std::vector<SCell> out;
        if (workspaces.empty())
            return out;
        auto lo = workspaces.front(), hi = workspaces.front();
        for (const auto& c : workspaces) {
            lo = {std::min(lo.x, c.x), std::min(lo.y, c.y)};
            hi = {std::max(hi.x, c.x), std::max(hi.y, c.y)};
        }
        for (int y = lo.y - 1; y <= hi.y + 1; ++y) {
            for (int x = lo.x - 1; x <= hi.x + 1; ++x) {
                if (std::ranges::find(workspaces, SCell{x, y}) == workspaces.end())
                    out.push_back({x, y});
            }
        }
        return out;
    }

    // Where a dragged window may create a workspace: the empty cells touching
    // `workspaces`, not `taken` by another workspace of the board.
    inline std::vector<SCell> dropSlots(const std::vector<SCell>& workspaces, const std::function<bool(const SCell&)>& taken) {
        std::vector<SCell> slots;
        for (const auto& c : workspaces) {
            for (const auto& n : {SCell{c.x + 1, c.y}, SCell{c.x - 1, c.y}, SCell{c.x, c.y + 1}, SCell{c.x, c.y - 1}}) {
                if (std::ranges::find(workspaces, n) != workspaces.end() || std::ranges::find(slots, n) != slots.end() || (taken && taken(n)))
                    continue;
                slots.push_back(n);
            }
        }
        return slots;
    }
}
