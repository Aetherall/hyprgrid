// Workspace grid (see Grid.hpp).
//
// Positions: a workspace is placed when first seen, on its monitor's board
// -- numbered 1-99 on column 0, row id - 1, if free (else the nearest free
// cell); ones created by entering a cell get it (ids from 100 up off column
// 0). A workspace that turns up on another board (moved to a monitor there,
// or the topology changed) is placed on it again, near where it was.
// Placements belong to that workspace (an id reused later is placed afresh),
// can be moved (moveWorkspace()), and are remembered in
// $XDG_RUNTIME_DIR/hyprgrid so a plugin reload keeps the layout.
//
// Cells can be entered when they hold a workspace, or are empty and next to
// (up/down/left/right) a workspace with windows, on the same board.

#include "Grid.hpp"

#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

using Grid::SCell;

namespace {
    constexpr int64_t STRIP_IDS = 100; // 1-99: column 0; from here: the rest of the grid

    std::map<int64_t, SCell>           g_cells;    // placements, by workspace id
    std::map<int64_t, std::string>     g_boards;   // ...and the board each is on
    std::map<int64_t, PHLWORKSPACEREF> g_owner;    // the workspace each placement belongs to
    std::set<int64_t>                  g_reserved; // placed by idFor(), not created yet
    std::function<void()>              g_onChange;
    std::function<std::string(const PHLMONITOR&)> g_boardOf;
    std::optional<Grid::SRegions>                 g_regions;

    // With regions, a cell is fine for a workspace only in its monitor's.
    bool inRegion(const PHLWORKSPACE& ws, const SCell& cell) {
        return !g_regions || !ws || g_regions->owner(cell) == ws->m_monitor.lock();
    }

    void changed() {
        if (g_onChange)
            g_onChange();
    }

    std::string statePath() {
        const char* dir = std::getenv("XDG_RUNTIME_DIR");
        return std::string{dir ? dir : "/tmp"} + "/hyprgrid";
    }

    std::optional<int64_t> idOf(const PHLWORKSPACE& ws) {
        if (!ws || ws->type() == Workspace::eWorkspaceType::SPECIAL)
            return std::nullopt;
        const auto ID = ws->numberedID();
        return ID ? std::optional<int64_t>{sc<int64_t>(*ID)} : std::nullopt;
    }

    // Placements stay with the workspace they were made for: a new workspace
    // reusing an id is placed afresh, unless the grid reserved that id for a
    // cell being entered (idFor()).
    void save() {
        std::ofstream out(statePath(), std::ios::trunc);
        for (const auto& [id, c] : g_cells) {
            if (g_owner[id].lock() || g_reserved.contains(id))
                out << id << ' ' << c.x << ' ' << c.y << ' ' << g_boards[id] << '\n';
        }
    }

    // Placed, on the board it is on now, and in its monitor's region.
    bool placed(const PHLWORKSPACE& ws, int64_t id) {
        const auto OWNER = g_owner.find(id);
        return OWNER != g_owner.end() && OWNER->second.lock() == ws && g_cells.contains(id) && g_boards[id] == Grid::boardOf(ws) && inRegion(ws, g_cells[id]);
    }

    // What sits on a board's cell among the placed workspaces: live placements
    // only, so stale ones never block it.
    PHLWORKSPACE placedAt(const std::string& board, const SCell& cell) {
        for (const auto& [id, owner] : g_owner) {
            const auto WS = owner.lock();
            if (WS && WS->numberedID() && sc<int64_t>(*WS->numberedID()) == id && g_cells[id] == cell && g_boards[id] == board)
                return WS;
        }
        return nullptr;
    }

    // A free cell for a newcomer: `want` if free, else the nearest free one
    // (in `ws`'s region, with regions).
    SCell freeCellNear(const std::string& board, const SCell& want, const PHLWORKSPACE& ws = nullptr) {
        for (int r = 0; r < 64; ++r) {
            for (int dy = -r; dy <= r; ++dy) {
                for (int dx = -r; dx <= r; ++dx) {
                    const SCell C = {want.x + dx, want.y + dy};
                    if (std::max(std::abs(dx), std::abs(dy)) == r && !placedAt(board, C) && inRegion(ws, C))
                        return C;
                }
            }
        }
        return want;
    }

    // First sight of a workspace on its board: the cell reserved for its id
    // there if still free (idFor()); a workspace that came from another board,
    // near its old cell; else numbered 1-99 on column 0, row id - 1; else the
    // nearest free cell.
    void place(int64_t id, const PHLWORKSPACE& ws) {
        const auto BOARD    = Grid::boardOf(ws);
        const bool RESERVED = g_reserved.erase(id) && g_cells.contains(id) && g_boards[id] == BOARD && inRegion(ws, g_cells[id]);
        const bool MOVED    = !RESERVED && g_owner[id].lock() == ws && g_cells.contains(id);
        const auto MON      = ws->m_monitor.lock();
        // Numbered ones start on column 0 -- with regions, the region's cell
        // nearest the seam in that row; one come over from another region
        // lands right across the seam, in the same row.
        const auto NUMBERED = id >= 1 && id < STRIP_IDS ? SCell{0, int(id - 1)} : SCell{};
        const auto WANT     = RESERVED   ? g_cells[id] :
                MOVED && g_regions && MON ? g_regions->nearSeam(MON, g_regions->along(g_cells[id])) :
                MOVED                     ? g_cells[id] :
                g_regions && MON          ? g_regions->nearSeam(MON, NUMBERED.y) :
                                            NUMBERED;
        if (!(RESERVED && !placedAt(BOARD, g_cells[id]))) // else: the reserved cell, still free
            g_cells[id] = freeCellNear(BOARD, WANT, ws);
        g_boards[id] = BOARD;
        g_owner[id]  = ws;
        changed();
    }

    // Place every live workspace not on its board yet, in id order, so what
    // sits on a cell never depends on which workspace was asked about first.
    void placeNewcomers() {
        std::vector<std::pair<int64_t, PHLWORKSPACE>> newcomers;
        for (const auto& ws : State::Workspace::state()->workspacesCopy()) {
            if (const auto ID = idOf(ws); ID && !placed(ws, *ID))
                newcomers.emplace_back(*ID, ws);
        }
        if (newcomers.empty())
            return;
        std::ranges::sort(newcomers, {}, &std::pair<int64_t, PHLWORKSPACE>::first);
        for (const auto& [id, ws] : newcomers)
            place(id, ws);
        save();
    }

    PHLWORKSPACE workspaceOf(int64_t id) {
        for (const auto& ws : State::Workspace::state()->workspacesCopy()) {
            if (idOf(ws) == id)
                return ws;
        }
        return nullptr;
    }

    bool idExists(int64_t id) {
        return workspaceOf(id) != nullptr;
    }

    // Placements from before a plugin reload belong to the workspaces still
    // there. (A line without a board, from before boards, has an empty one:
    // its workspace is placed again, near that cell.)
    void load() {
        std::ifstream in(statePath());
        std::string   line;
        while (std::getline(in, line)) {
            std::istringstream fields(line);
            int64_t            id;
            SCell              c;
            std::string        board;
            if (!(fields >> id >> c.x >> c.y))
                continue;
            fields >> board;
            g_cells[id]  = c;
            g_boards[id] = board;
        }
        for (const auto& ws : State::Workspace::state()->workspacesCopy()) {
            if (const auto ID = idOf(ws); ID && g_cells.contains(*ID))
                g_owner[*ID] = ws;
        }
    }
}

void Grid::init() {
    load();
}

void Grid::setRegions(std::optional<SRegions> regions) {
    g_regions = std::move(regions);
    changed(); // workspaces may be in another region now
}

PHLMONITOR Grid::regionOwner(const SCell& cell) {
    return g_regions ? g_regions->owner(cell) : nullptr;
}

void Grid::setBoards(std::function<std::string(const PHLMONITOR&)> boardOf) {
    g_boardOf = std::move(boardOf);
    changed(); // workspaces may be on other boards now
}

std::string Grid::boardOf(const PHLMONITOR& monitor) {
    if (!monitor)
        return {};
    return g_boardOf ? g_boardOf(monitor) : monitor->m_name;
}

std::string Grid::boardOf(const PHLWORKSPACE& workspace) {
    return workspace ? boardOf(workspace->m_monitor.lock()) : std::string{};
}

bool Grid::sameBoard(const PHLMONITOR& a, const PHLMONITOR& b) {
    return a && b && (a == b || boardOf(a) == boardOf(b));
}

std::optional<SCell> Grid::cellOf(const PHLWORKSPACE& workspace) {
    const auto ID = idOf(workspace);
    if (!ID)
        return std::nullopt;
    if (!placed(workspace, *ID)) {
        placeNewcomers();
        if (!placed(workspace, *ID)) { // not listed in the workspace state (yet)
            place(*ID, workspace);
            save();
        }
    }
    return g_cells[*ID];
}

std::optional<SCell> Grid::cellOf(int64_t workspaceID) {
    if (const auto WS = workspaceOf(workspaceID))
        return cellOf(WS);
    if (g_reserved.contains(workspaceID) && g_cells.contains(workspaceID))
        return g_cells[workspaceID];
    return std::nullopt;
}

PHLWORKSPACE Grid::workspaceAt(const std::string& board, const SCell& cell) {
    placeNewcomers();
    return placedAt(board, cell);
}

bool Grid::enterable(const PHLMONITOR& from, const SCell& cell) {
    if (g_regions && g_regions->owner(cell) != from)
        return false; // across the seam: the other monitor's
    const auto board = boardOf(from);
    if (workspaceAt(board, cell))
        return true;
    for (const auto& n : {SCell{cell.x - 1, cell.y}, SCell{cell.x + 1, cell.y}, SCell{cell.x, cell.y - 1}, SCell{cell.x, cell.y + 1}}) {
        const auto WS = workspaceAt(board, n);
        if (WS && WS->getWindowCount() > 0)
            return true;
    }
    return false;
}

// Column 0 keeps the strip's numbering when that id is free.
int64_t Grid::idFor(const std::string& board, const SCell& cell) {
    if (const auto WS = workspaceAt(board, cell))
        return *idOf(WS);
    int64_t id = cell.x == 0 && cell.y >= 0 && cell.y + 1 < STRIP_IDS ? cell.y + 1 : STRIP_IDS;
    if (id < STRIP_IDS && (idExists(id) || g_reserved.contains(id)))
        id = STRIP_IDS;
    if (id >= STRIP_IDS) {
        while (idExists(id) || (g_reserved.contains(id) && !(g_cells[id] == cell && g_boards[id] == board)))
            ++id;
    }
    g_cells[id]  = cell;
    g_boards[id] = board;
    g_reserved.insert(id);
    save();
    return id;
}

bool Grid::moveWorkspace(const PHLWORKSPACE& workspace, const SCell& cell) {
    const auto ID   = idOf(workspace);
    const auto FROM = cellOf(workspace);
    if (!ID || !FROM || *FROM == cell || !inRegion(workspace, cell))
        return false;
    if (const auto OTHER = workspaceAt(g_boards[*ID], cell))
        g_cells[*idOf(OTHER)] = *FROM;
    g_cells[*ID] = cell;
    save();
    changed();
    return true;
}

bool Grid::moveWorkspace(int64_t workspaceID, const SCell& cell) {
    const auto WS = workspaceOf(workspaceID);
    return WS && moveWorkspace(WS, cell);
}

std::vector<Grid::SPlaced> Grid::all() {
    placeNewcomers();
    std::vector<SPlaced> out;
    for (const auto& [id, owner] : g_owner) {
        const auto WS = owner.lock();
        if (WS && idOf(WS) == id && g_cells.contains(id))
            out.push_back({id, g_cells[id], g_boards[id], WS});
    }
    return out;
}

bool Grid::placeAll(const std::vector<std::pair<int64_t, SCell>>& moves) {
    auto after = all();
    for (const auto& [id, cell] : moves) {
        const auto IT = std::ranges::find(after, id, &SPlaced::id);
        if (IT == after.end() || !inRegion(IT->workspace, cell))
            return false;
        IT->cell = cell;
    }
    for (size_t i = 0; i < after.size(); ++i) {
        for (size_t j = i + 1; j < after.size(); ++j) {
            if (after[i].board == after[j].board && after[i].cell == after[j].cell)
                return false;
        }
    }
    for (const auto& p : after)
        g_cells[p.id] = p.cell;
    save();
    changed();
    return true;
}

void Grid::onChange(std::function<void()> callback) {
    g_onChange = std::move(callback);
}
