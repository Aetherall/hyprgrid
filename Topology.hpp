// Topology: what a step from a workspace leads to, given the board it is on
// and how that board's edges are linked (docs/design.md, "Topology"). No
// Hyprland in here, so it is tested on its own (tests/topology.cpp);
// TopologyConfig.cpp holds the configured boards and links and asks this.
#pragma once

#include "Motion.hpp"

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Topology {
    using Motion::SCell;

    enum class eGrow : uint8_t {
        OPEN_EDGES, // empty cells can be entered, except toward a link
        ALWAYS,     // empty cells first, a link only where the grid can't grow
        NEVER,      // only existing workspaces and links
    };

    enum class eSide : uint8_t {
        LEFT,
        RIGHT,
        UP,
        DOWN,
    };

    // The side a unit step goes toward; none for a diagonal or no step.
    inline std::optional<eSide> sideOf(const SCell& step) {
        if (step.y == 0 && step.x < 0)
            return eSide::LEFT;
        if (step.y == 0 && step.x > 0)
            return eSide::RIGHT;
        if (step.x == 0 && step.y < 0)
            return eSide::UP;
        if (step.x == 0 && step.y > 0)
            return eSide::DOWN;
        return std::nullopt;
    }

    inline eSide opposite(eSide side) {
        switch (side) {
            case eSide::LEFT: return eSide::RIGHT;
            case eSide::RIGHT: return eSide::LEFT;
            case eSide::UP: return eSide::DOWN;
            default: return eSide::UP;
        }
    }

    enum class eKind : uint8_t {
        WORKSPACE, // a workspace on this board
        MONITOR,   // the board's edge is linked to another monitor
        NEW,       // an empty cell that can be entered
        NONE,
    };

    struct SStep {
        eKind       kind = eKind::NONE;
        SCell       cell;    // WORKSPACE / NEW: the cell reached
        std::string monitor; // MONITOR: the monitor linked
    };

    struct SQuery {
        SCell                             from, step;
        std::function<bool(const SCell&)> occupied;  // a workspace on this board sits there
        std::function<bool(const SCell&)> enterable; // an empty cell on this board that may be entered
        std::optional<std::string>        link;      // the monitor this board's edge that way leads to
        eGrow                             grow = eGrow::OPEN_EDGES;
    };

    // An existing workspace first; then, by `grow`, the link or a new cell.
    inline SStep step(const SQuery& q) {
        const SCell TO = {q.from.x + q.step.x, q.from.y + q.step.y};
        if (q.occupied(TO))
            return {eKind::WORKSPACE, TO, {}};

        const bool CANGROW = q.grow != eGrow::NEVER && q.enterable(TO);
        if (q.grow == eGrow::ALWAYS && CANGROW)
            return {eKind::NEW, TO, {}};
        if (q.link)
            return {eKind::MONITOR, {}, *q.link};
        if (CANGROW)
            return {eKind::NEW, TO, {}};
        return {};
    }

    // "row" landing: on the board entered, in the same line as the cell left
    // (its row across a side link, its column across a top/bottom one), the
    // workspace nearest the edge it is entered by; none if that line is empty.
    inline std::optional<SCell> landingInLine(const std::vector<SCell>& cells, const SCell& from, eSide enteredBy) {
        const bool           ACROSS = enteredBy == eSide::LEFT || enteredBy == eSide::RIGHT;
        std::optional<SCell> best;
        for (const auto& c : cells) {
            if (ACROSS ? c.y != from.y : c.x != from.x)
                continue;
            const bool NEARER = !best ||
                (enteredBy == eSide::RIGHT ? c.x > best->x :
                     enteredBy == eSide::LEFT ? c.x < best->x :
                     enteredBy == eSide::DOWN ? c.y > best->y :
                                                c.y < best->y);
            if (NEARER)
                best = c;
        }
        return best;
    }

    // --- One grid, a region per monitor ---
    //
    // Two monitors split the grid along a seam: side by side, a vertical one
    // (cells with x < at belong to the first monitor, the left one); stacked,
    // a horizontal one (y < at: the top one). A region grows everywhere but
    // across its seam, and a workspace's monitor is its cell's region.

    struct SSeam {
        bool vertical = true; // side by side
        int  at       = 0;    // the first cell of the second region
    };

    // The seam two monitors' positions make: along the axis their centres
    // are furthest apart on. `first` is the left (top) one if true.
    inline SSeam seamBetween(double dx, double dy, bool& first) {
        const bool VERTICAL = std::abs(dx) >= std::abs(dy);
        first               = VERTICAL ? dx >= 0 : dy >= 0; // dx, dy: second's centre minus first's
        return {VERTICAL, 0};
    }

    // The cell's region: false the first (left / top), true the second.
    inline bool inSecond(const SSeam& seam, const SCell& cell) {
        return (seam.vertical ? cell.x : cell.y) >= seam.at;
    }

    // A region's cell nearest the seam, `along` it (the row for a vertical
    // seam, the column for a horizontal one): where numbered workspaces start,
    // and where one moved over from the other region lands.
    inline SCell nearSeam(const SSeam& seam, bool second, int along) {
        const int ACROSS = second ? seam.at : seam.at - 1;
        return seam.vertical ? SCell{ACROSS, along} : SCell{along, ACROSS};
    }

    // Where along the seam a cell is.
    inline int alongSeam(const SSeam& seam, const SCell& cell) {
        return seam.vertical ? cell.y : cell.x;
    }

    // Ordered monitors occupy adjacent stripes: the first extends to -inf,
    // the last to +inf. Two monitors keep the original seam at zero.
    inline size_t stripeOwner(const SCell& cell, bool vertical, size_t count) {
        if (!count)
            return 0;
        const int ACROSS = vertical ? cell.x : cell.y;
        return static_cast<size_t>(std::clamp(ACROSS + 1, 0, int(count) - 1));
    }

    inline SCell stripeStart(size_t index, bool vertical, int along) {
        const int ACROSS = int(index) - 1;
        return vertical ? SCell{ACROSS, along} : SCell{along, ACROSS};
    }

    inline std::vector<SSeam> stripeSeams(size_t count, bool vertical) {
        std::vector<SSeam> seams;
        for (size_t i = 1; i < count; ++i)
            seams.push_back({vertical, int(i) - 1});
        return seams;
    }
}
