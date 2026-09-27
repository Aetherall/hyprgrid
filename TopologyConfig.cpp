// The topology (see TopologyConfig.hpp). Registers:
//   hl.plugin.hyprgrid.topology({ boards, links, land, grow })
//       boards: "regions" (one grid, a region per monitor split by seams
//               from their layout, the default), "monitor" (a grid each),
//               "shared" (one grid, no seams), or groups
//               { name = { "DP-2", ... }, ... } (a monitor in none: its own)
//       links:  "physical" (an edge facing another monitor leads to it, the
//               default), "none", a list { { from, side, to }, ... }, or a
//               function(monitor, side) -> monitor name (or nil), called on
//               a move at a board's edge only
//       land:   "active" (the linked monitor's current workspace, the
//               default) or "row" (its workspace in the same row / column,
//               nearest the edge entered by)
//       grow:   "open_edges" (empty cells can be entered except toward a
//               link, the default), "always" (empty cells first), "never"
//   hl.plugin.hyprgrid.step(dx, dy) -> { kind, id, monitor, dx, dy }
//       What a step from the focused monitor's active workspace reaches:
//       kind "workspace", "new" (id reserved for the empty cell), "monitor"
//       (a link; id: the workspace to land on) or "none".
//
// A config reload keeps it until the config sets it again, and resets it to
// the defaults if the config no longer does; a link function goes at once
// (it lives in the Lua state the reload replaces).

#include "TopologyConfig.hpp"
#include "Grid.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/config/lua/ConfigManager.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/state/MonitorQuery.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <map>
#include <stdexcept>
#include <vector>

using Topology::eGrow;
using Topology::eKind;
using Topology::eSide;

namespace {
    enum class eBoards : uint8_t {
        REGIONS,
        MONITOR,
        SHARED,
        GROUPS,
    };
    enum class eLinks : uint8_t {
        PHYSICAL,
        NONE,
        TABLE,
        FUNCTION,
    };
    enum class eLand : uint8_t {
        ACTIVE,
        ROW,
    };

    struct SLink {
        std::string from;
        eSide       side;
        std::string to;
    };

    struct SConfig {
        eBoards                            boards = eBoards::REGIONS;
        std::map<std::string, std::string> groupOf; // monitor -> group, for GROUPS
        eLinks                             links = eLinks::PHYSICAL;
        std::vector<SLink>                 table;
        int                                fn   = LUA_NOREF;
        eLand                              land = eLand::ACTIVE;
        eGrow                              grow = eGrow::OPEN_EDGES;
    };

    SConfig             g_config;
    bool                g_setThisReload = false;
    CHyprSignalListener g_preReloadListener, g_reloadedListener, g_monitorListener;

    const char* nameOf(eSide side) {
        switch (side) {
            case eSide::LEFT: return "left";
            case eSide::RIGHT: return "right";
            case eSide::UP: return "up";
            default: return "down";
        }
    }

    std::optional<eSide> sideNamed(const std::string& name) {
        for (const auto S : {eSide::LEFT, eSide::RIGHT, eSide::UP, eSide::DOWN}) {
            if (name == nameOf(S))
                return S;
        }
        return std::nullopt;
    }

    Math::eDirection directionOf(eSide side) {
        switch (side) {
            case eSide::LEFT: return Math::DIRECTION_LEFT;
            case eSide::RIGHT: return Math::DIRECTION_RIGHT;
            case eSide::UP: return Math::DIRECTION_UP;
            default: return Math::DIRECTION_DOWN;
        }
    }

    PHLMONITOR monitorNamed(const std::string& name) {
        for (const auto& mon : State::monitorState()->monitors()) {
            if (mon && mon->m_name == name)
                return mon;
        }
        return nullptr;
    }

    std::optional<int64_t> idOf(const PHLWORKSPACE& ws) {
        if (!ws || !ws->numberedID())
            return std::nullopt;
        return sc<int64_t>(*ws->numberedID());
    }

    // --- Regions: one grid, split between (up to) two monitors by a seam ---

    // The monitors sharing the grid by regions: one or two (more: a grid each,
    // for now).
    std::vector<PHLMONITOR> regionMonitors() {
        if (g_config.boards != eBoards::REGIONS)
            return {};
        const auto& MONITORS = State::monitorState()->monitors();
        return MONITORS.size() <= 2 ? MONITORS : std::vector<PHLMONITOR>{};
    }

    // The seam, and the monitors either side of it (first: left / top).
    struct SSplit {
        Topology::SSeam seam;
        PHLMONITOR      first, second;
    };

    std::optional<SSplit> split() {
        const auto MONS = regionMonitors();
        if (MONS.size() != 2)
            return std::nullopt;
        const auto  CA = MONS[0]->m_position + MONS[0]->m_size / 2.0, CB = MONS[1]->m_position + MONS[1]->m_size / 2.0;
        bool        aFirst = true;
        const auto  SEAM   = Topology::seamBetween(CB.x - CA.x, CB.y - CA.y, aFirst);
        return SSplit{SEAM, aFirst ? MONS[0] : MONS[1], aFirst ? MONS[1] : MONS[0]};
    }

    // Who owns a cell: with one monitor, it; with two, the seam's side.
    PHLMONITOR ownerOf(const Motion::SCell& cell) {
        if (const auto S = split())
            return Topology::inSecond(S->seam, cell) ? S->second : S->first;
        const auto MONS = regionMonitors();
        return MONS.empty() ? nullptr : MONS.front();
    }

    // Hand the grid the regions (or none), which places workspaces again.
    void installRegions() {
        if (regionMonitors().empty()) {
            Grid::setRegions(std::nullopt);
            return;
        }
        Grid::setRegions(Grid::SRegions{
            .owner = ownerOf,
            .nearSeam =
                [](const PHLMONITOR& mon, int along) {
                    const auto S = split();
                    return S ? Topology::nearSeam(S->seam, mon == S->second, along) : Motion::SCell{0, along};
                },
            .along =
                [](const Motion::SCell& cell) {
                    const auto S = split();
                    return S ? Topology::alongSeam(S->seam, cell) : cell.y;
                },
        });
    }

    // Where `mon`'s board edge on `side` leads.
    PHLMONITOR linked(const PHLMONITOR& mon, eSide side) {
        switch (g_config.links) {
            case eLinks::PHYSICAL: {
                const auto TO = State::monitorState()->query().inDirection(directionOf(side)).relativeTo(mon).run();
                return TO != mon ? TO : nullptr;
            }
            case eLinks::TABLE:
                for (const auto& l : g_config.table) {
                    if (l.from == mon->m_name && l.side == side)
                        return monitorNamed(l.to);
                }
                return nullptr;
            case eLinks::FUNCTION: {
                // Called on Hyprland's Lua state, which may be running the
                // key's own function: a nested call, like any Lua call.
                const auto MGR  = Config::Lua::mgr();
                const auto LIFE = MGR ? MGR->luaStateLifetime() : nullptr;
                lua_State* L    = LIFE ? LIFE->state : nullptr;
                if (!L || g_config.fn == LUA_NOREF)
                    return nullptr;
                lua_rawgeti(L, LUA_REGISTRYINDEX, g_config.fn);
                lua_pushstring(L, mon->m_name.c_str());
                lua_pushstring(L, nameOf(side));
                if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
                    lua_pop(L, 1); // the error
                    return nullptr;
                }
                const std::string TO = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
                lua_pop(L, 1);
                return TO.empty() ? nullptr : monitorNamed(TO);
            }
            default: return nullptr;
        }
    }

    // The workspace to land on across a link from `from` (at `cell`, stepping
    // to `side`) onto `to`.
    PHLWORKSPACE landing(const PHLMONITOR& to, const std::optional<Motion::SCell>& cell, eSide side) {
        if (g_config.land == eLand::ROW && cell) {
            const auto               BOARD = Grid::boardOf(to);
            std::vector<Motion::SCell> cells;
            for (const auto& p : Grid::all()) {
                if (p.board == BOARD && p.workspace->m_monitor.lock() == to)
                    cells.push_back(p.cell);
            }
            if (const auto C = Topology::landingInLine(cells, *cell, Topology::opposite(side)))
                return Grid::workspaceAt(BOARD, *C);
        }
        return to->m_activeWorkspace;
    }

    // --- Lua ---

    // Each parser returns an error message, empty when fine.
    std::string parseBoards(lua_State* L, SConfig& c) {
        lua_getfield(L, 1, "boards");
        std::string err;
        if (lua_isstring(L, -1)) {
            const std::string V = lua_tostring(L, -1);
            if (V == "regions")
                c.boards = eBoards::REGIONS;
            else if (V == "monitor")
                c.boards = eBoards::MONITOR;
            else if (V == "shared")
                c.boards = eBoards::SHARED;
            else
                err = "boards: expected \"regions\", \"monitor\", \"shared\" or groups { name = { monitors } }";
        } else if (lua_istable(L, -1)) {
            c.boards = eBoards::GROUPS;
            lua_pushnil(L);
            while (err.empty() && lua_next(L, -2)) {
                const std::string GROUP = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : "";
                if (GROUP.empty() || !lua_istable(L, -1))
                    err = "boards: each group is name = { \"monitor\", ... }";
                else {
                    for (int i = 1;; ++i) {
                        lua_rawgeti(L, -1, i);
                        if (lua_isnil(L, -1)) {
                            lua_pop(L, 1);
                            break;
                        }
                        if (lua_isstring(L, -1))
                            c.groupOf[lua_tostring(L, -1)] = GROUP;
                        else
                            err = "boards." + GROUP + ": expected monitor names";
                        lua_pop(L, 1);
                    }
                }
                lua_pop(L, 1);
            }
            if (!err.empty())
                lua_pop(L, 1); // the key lua_next left
        } else if (!lua_isnil(L, -1))
            err = "boards: expected a string or a table";
        lua_pop(L, 1);
        return err;
    }

    std::string parseLinks(lua_State* L, SConfig& c) {
        lua_getfield(L, 1, "links");
        std::string err;
        if (lua_isstring(L, -1)) {
            const std::string V = lua_tostring(L, -1);
            if (V == "physical")
                c.links = eLinks::PHYSICAL;
            else if (V == "none")
                c.links = eLinks::NONE;
            else
                err = "links: expected \"physical\", \"none\", a list of links or a function";
        } else if (lua_isfunction(L, -1)) {
            c.links = eLinks::FUNCTION;
            lua_pushvalue(L, -1);
            c.fn = luaL_ref(L, LUA_REGISTRYINDEX);
        } else if (lua_istable(L, -1)) {
            c.links = eLinks::TABLE;
            for (int i = 1; err.empty(); ++i) {
                lua_rawgeti(L, -1, i);
                if (lua_isnil(L, -1)) {
                    lua_pop(L, 1);
                    break;
                }
                SLink link;
                bool  ok = lua_istable(L, -1);
                if (ok) {
                    lua_getfield(L, -1, "from");
                    lua_getfield(L, -2, "side");
                    lua_getfield(L, -3, "to");
                    const auto SIDE = lua_isstring(L, -2) ? sideNamed(lua_tostring(L, -2)) : std::nullopt;
                    ok              = lua_isstring(L, -3) && SIDE && lua_isstring(L, -1);
                    if (ok)
                        link = {lua_tostring(L, -3), *SIDE, lua_tostring(L, -1)};
                    lua_pop(L, 3);
                }
                if (ok)
                    c.table.push_back(link);
                else
                    err = "links[" + std::to_string(i) + "]: expected { from = \"DP-3\", side = \"left\", to = \"DP-2\" }";
                lua_pop(L, 1);
            }
        } else if (!lua_isnil(L, -1))
            err = "links: expected a string, a list of links or a function";
        lua_pop(L, 1);
        return err;
    }

    std::string parseTopology(lua_State* L, SConfig& c) {
        if (!lua_istable(L, 1))
            return "expected a table";
        auto err = parseBoards(L, c);
        if (err.empty())
            err = parseLinks(L, c);
        if (err.empty()) {
            lua_getfield(L, 1, "land");
            if (lua_isstring(L, -1)) {
                const std::string V = lua_tostring(L, -1);
                if (V == "active")
                    c.land = eLand::ACTIVE;
                else if (V == "row")
                    c.land = eLand::ROW;
                else
                    err = "land: expected \"active\" or \"row\"";
            } else if (!lua_isnil(L, -1))
                err = "land: expected \"active\" or \"row\"";
            lua_pop(L, 1);
        }
        if (err.empty()) {
            lua_getfield(L, 1, "grow");
            if (lua_isstring(L, -1)) {
                const std::string V = lua_tostring(L, -1);
                if (V == "open_edges")
                    c.grow = eGrow::OPEN_EDGES;
                else if (V == "always")
                    c.grow = eGrow::ALWAYS;
                else if (V == "never")
                    c.grow = eGrow::NEVER;
                else
                    err = "grow: expected \"open_edges\", \"always\" or \"never\"";
            } else if (!lua_isnil(L, -1))
                err = "grow: expected \"open_edges\", \"always\" or \"never\"";
            lua_pop(L, 1);
        }
        return err;
    }

    void apply() {
        Grid::setBoards(TopologyConfig::boardOf); // workspaces may be on other boards now
        installRegions();
    }

    int luaTopology(lua_State* L) {
        {
            SConfig    config;
            const auto ERR = parseTopology(L, config);
            if (ERR.empty()) {
                if (g_config.fn != LUA_NOREF)
                    luaL_unref(L, LUA_REGISTRYINDEX, g_config.fn);
                g_config        = std::move(config);
                g_setThisReload = true;
                apply();
                return 0;
            }
            if (config.fn != LUA_NOREF)
                luaL_unref(L, LUA_REGISTRYINDEX, config.fn);
            lua_pushstring(L, ("hyprgrid.topology: " + ERR).c_str());
        }
        // Raised with no C++ object alive: lua_error() longjmps.
        return lua_error(L);
    }

    int luaStep(lua_State* L) {
        const Motion::SCell D    = {int(lua_tointeger(L, 1)), int(lua_tointeger(L, 2))};
        const auto          STEP = TopologyConfig::step(Desktop::focusState()->monitor(), D);
        lua_createtable(L, 0, 5);
        const char* KIND = STEP.kind == eKind::WORKSPACE ? "workspace" : STEP.kind == eKind::NEW ? "new" : STEP.kind == eKind::MONITOR ? "monitor" : "none";
        lua_pushstring(L, KIND);
        lua_setfield(L, -2, "kind");
        if (STEP.id) {
            lua_pushinteger(L, *STEP.id);
            lua_setfield(L, -2, "id");
        }
        if (STEP.monitor) {
            lua_pushstring(L, STEP.monitor->m_name.c_str());
            lua_setfield(L, -2, "monitor");
        }
        lua_pushinteger(L, D.x);
        lua_setfield(L, -2, "dx");
        lua_pushinteger(L, D.y);
        lua_setfield(L, -2, "dy");
        return 1;
    }
}

std::string TopologyConfig::boardOf(const PHLMONITOR& monitor) {
    if (!monitor)
        return {};
    switch (g_config.boards) {
        case eBoards::REGIONS: return regionMonitors().empty() ? monitor->m_name : std::string{"grid"};
        case eBoards::SHARED: return "shared";
        case eBoards::GROUPS: {
            const auto IT = g_config.groupOf.find(monitor->m_name);
            return IT != g_config.groupOf.end() ? IT->second : monitor->m_name;
        }
        default: return monitor->m_name;
    }
}

TopologyConfig::SStep TopologyConfig::step(const PHLMONITOR& monitor, const Motion::SCell& d) {
    if (!monitor)
        return {};
    const auto WS    = monitor->m_activeWorkspace;
    const auto CELL  = Grid::cellOf(WS);
    const auto SIDE  = Topology::sideOf(d);
    const auto LINK  = SIDE ? linked(monitor, *SIDE) : nullptr;
    const auto BOARD = Grid::boardOf(monitor);

    const auto crossTo = [&](const PHLMONITOR& to) {
        const auto LAND = landing(to, CELL, *SIDE);
        return SStep{eKind::MONITOR, idOf(LAND), to};
    };

    // A named workspace isn't on the grid: only links lead anywhere.
    if (!CELL)
        return LINK ? crossTo(LINK) : SStep{};

    // With regions, the cell that way may be across the seam: the other
    // monitor's, reached by crossing (unless links are off), never entered.
    const auto          TO     = Motion::SCell{CELL->x + d.x, CELL->y + d.y};
    const bool          ACROSS = !regionMonitors().empty() && ownerOf(TO) != monitor;
    const auto          OWNER  = ACROSS ? ownerOf(TO) : nullptr;
    const PHLMONITOR    TARGET = ACROSS ? (g_config.links == eLinks::NONE ? nullptr : OWNER) : LINK;

    Topology::SQuery q;
    q.from      = *CELL;
    q.step      = d;
    q.occupied  = [&](const Motion::SCell& c) { return !ACROSS && Grid::workspaceAt(BOARD, c) != nullptr; };
    q.enterable = [&](const Motion::SCell& c) { return !ACROSS && !Grid::workspaceAt(BOARD, c) && Grid::enterable(monitor, c); };
    q.grow      = g_config.grow;
    // Without regions, a link applies at a board's edge; with them, only across the seam.
    if (TARGET && (ACROSS || regionMonitors().empty()))
        q.link = TARGET->m_name;

    const auto RESULT = Topology::step(q);
    switch (RESULT.kind) {
        case eKind::WORKSPACE: {
            const auto TO = Grid::workspaceAt(BOARD, RESULT.cell);
            return {eKind::WORKSPACE, idOf(TO), TO ? TO->m_monitor.lock() : nullptr};
        }
        case eKind::NEW: return {eKind::NEW, Grid::idFor(BOARD, RESULT.cell), monitor};
        case eKind::MONITOR: return crossTo(ACROSS ? TARGET : LINK);
        default: return {};
    }
}

std::optional<Topology::SSeam> TopologyConfig::seam() {
    const auto S = split();
    return S ? std::optional<Topology::SSeam>{S->seam} : std::nullopt;
}

void TopologyConfig::init(void* handle) {
    const bool OK = HyprlandAPI::addLuaFunction(handle, "hyprgrid", "topology", luaTopology) && HyprlandAPI::addLuaFunction(handle, "hyprgrid", "step", luaStep);
    if (!OK)
        throw std::runtime_error("[hyprgrid] failed to register topology()");
    apply();
    // Monitors come and go, or move: the regions follow.
    g_monitorListener = Event::bus()->m_events.monitor.layoutChanged.listen([] { installRegions(); });
    g_preReloadListener = Event::bus()->m_events.config.preReload.listen([] {
        g_setThisReload = false;
        if (g_config.links == eLinks::FUNCTION) { // its reference dies with the Lua state
            g_config.links = eLinks::NONE;
            g_config.fn    = LUA_NOREF;
        }
    });
    g_reloadedListener = Event::bus()->m_events.config.reloaded.listen([] {
        if (g_setThisReload)
            return;
        g_config = {};
        apply();
    });
}

void TopologyConfig::exit() {
    g_preReloadListener.reset();
    g_reloadedListener.reset();
    g_monitorListener.reset();
    Grid::setRegions(std::nullopt);
    Grid::setBoards(nullptr);
    g_config = {};
}
