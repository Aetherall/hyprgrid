// Initial cells for workspaces new to a board. Saved, reserved and moved
// workspaces keep their own placement; this only chooses a starting point.
#pragma once

#include "Motion.hpp"

#include <cstdint>
#include <functional>

namespace Placement {
    using Motion::SCell;
    inline constexpr int64_t STRIP_IDS = 100; // 1-99: numbered strip; 100+: new cells

    inline SCell initialCell(int64_t id, const std::function<SCell(int)>& nearSeam, const std::function<bool(const SCell&)>& occupied) {
        // Numbered workspaces on a board without regions retain their global
        // column-0 strip. With regions, each monitor fills its own strip from
        // zero, irrespective of workspace ids or how many monitors there are.
        if (id >= 1 && id < STRIP_IDS) {
            if (nearSeam) {
                for (int along = 0;; ++along) {
                    const auto CELL = nearSeam(along);
                    if (!occupied(CELL))
                        return CELL;
                }
            }
            return {0, int(id - 1)};
        }
        // Unreserved high ids start at the board origin as before; a reserved
        // id is handled by the grid before calling this function.
        return nearSeam ? nearSeam(0) : SCell{};
    }
}
