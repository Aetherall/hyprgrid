// Workspace grid: every numbered workspace sits on a cell of a 2D grid
// (decoupled from its id), on the board of the monitor it is on (which
// monitors share a board is the topology, TopologyConfig.hpp). This is
// placement only; where each monitor looks on the grid is the view
// (View.hpp). See Grid.cpp.
#pragma once

#include "Motion.hpp"

#include <hyprland/src/desktop/DesktopTypes.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Grid {
    using SCell = Motion::SCell;

    // Load the placements saved before a plugin reload.
    void init();

    // Which board a monitor's workspaces are on (TopologyConfig); every
    // monitor has its own until set.
    void setBoards(std::function<std::string(const PHLMONITOR&)> boardOf);
    std::string boardOf(const PHLMONITOR& monitor);
    std::string boardOf(const PHLWORKSPACE& workspace);
    // Both monitors' workspaces are on the same board.
    bool sameBoard(const PHLMONITOR& a, const PHLMONITOR& b);

    // Where a workspace sits on its board (none for special/named ones, or no
    // such workspace), and what sits on a board's cell.
    std::optional<SCell> cellOf(const PHLWORKSPACE& workspace);
    std::optional<SCell> cellOf(int64_t workspaceID);
    PHLWORKSPACE         workspaceAt(const std::string& board, const SCell& cell);

    // One grid, a region per monitor (TopologyConfig): who owns a cell, and a
    // monitor's region's cell nearest the seam, `along` it. None: no regions.
    struct SRegions {
        std::function<PHLMONITOR(const SCell&)>           owner;
        std::function<SCell(const PHLMONITOR&, int along)> nearSeam;
        std::function<int(const SCell&)>                   along;
    };
    void setRegions(std::optional<SRegions> regions);

    // The monitor whose region a cell is in (none: no regions).
    PHLMONITOR regionOwner(const SCell& cell);

    // A cell a view on `from` may go to: holding a workspace, or empty and
    // next to (up/down/left/right) a workspace with windows, on its board and,
    // with regions, in its region: a board only grows around what's in use.
    bool enterable(const PHLMONITOR& from, const SCell& cell);

    // The workspace id for a board's cell: the one there, or a fresh one
    // reserved there. Switching or moving a window to that id (on a monitor
    // of that board) creates it.
    int64_t idFor(const std::string& board, const SCell& cell);

    // Put a workspace on another cell of its board, swapping with the one
    // there if any.
    bool moveWorkspace(const PHLWORKSPACE& workspace, const SCell& cell);
    bool moveWorkspace(int64_t workspaceID, const SCell& cell);

    // The live workspaces on the grid.
    struct SPlaced {
        int64_t      id;
        SCell        cell;
        std::string  board;
        PHLWORKSPACE workspace;
    };
    std::vector<SPlaced> all();

    // Put several workspaces on cells of their boards at once; nothing moves
    // (false) if one isn't on the grid or two would share a board's cell.
    bool placeAll(const std::vector<std::pair<int64_t, SCell>>& moves);

    // Called after any change of placement: a workspace placed or moved. (A
    // workspace going is Hyprland's event, workspace.removed.)
    void onChange(std::function<void()> callback);
}
