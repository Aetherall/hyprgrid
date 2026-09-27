// The layout strategy. Registers:
//   hl.plugin.hyprgrid.layout(arrange)     arrange(workspaces) runs after the
//       grid changes (a workspace created, gone, or moved), once per batch of
//       changes and from the event loop, never inside Hyprland's own event;
//       nil removes it. The moves it makes don't run it again.
//   hl.plugin.hyprgrid.workspaces()       -> { { id, x, y, board, region,
//       windows, monitor, active }, ... }: the grid's live workspaces (what
//       arrange gets); cells are per board, and with regions, `region` is the
//       monitor whose part of the grid the cell is in.
//   hl.plugin.hyprgrid.place(moves)       -> ok: moves = { [id] = { x, y } },
//       all at once; nothing moves if two workspaces would share a cell.
//
// Hyprland rebuilds its Lua state on a config reload, so the strategy is
// dropped on config.preReload and the config sets it again. Whatever moved,
// the views follow their active workspace afterwards (View::gridChanged()).

#include "Layout.hpp"
#include "Grid.hpp"
#include "View.hpp"
#include "Later.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/config/lua/ConfigManager.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <stdexcept>

namespace {
    int  g_arrange   = LUA_NOREF;
    bool g_pending   = false; // a run is scheduled
    bool g_arranging = false; // the strategy is running: its own moves don't count

    CHyprSignalListener g_createdListener, g_removedListener, g_reloadListener;

    void pushWorkspaces(lua_State* L) {
        const auto PLACED = Grid::all();
        lua_createtable(L, int(PLACED.size()), 0);
        int i = 0;
        for (const auto& p : PLACED) {
            const auto MON = p.workspace->m_monitor.lock();
            lua_createtable(L, 0, 8);
            lua_pushstring(L, p.board.c_str());
            lua_setfield(L, -2, "board");
            if (const auto OWNER = Grid::regionOwner(p.cell)) {
                lua_pushstring(L, OWNER->m_name.c_str());
                lua_setfield(L, -2, "region");
            }
            lua_pushinteger(L, p.id);
            lua_setfield(L, -2, "id");
            lua_pushinteger(L, p.cell.x);
            lua_setfield(L, -2, "x");
            lua_pushinteger(L, p.cell.y);
            lua_setfield(L, -2, "y");
            lua_pushinteger(L, p.workspace->getWindowCount());
            lua_setfield(L, -2, "windows");
            lua_pushstring(L, MON ? MON->m_name.c_str() : "");
            lua_setfield(L, -2, "monitor");
            lua_pushboolean(L, MON && MON->m_activeWorkspace == p.workspace);
            lua_setfield(L, -2, "active");
            lua_rawseti(L, -2, ++i);
        }
    }

    void run() {
        if (g_arrange != LUA_NOREF) {
            if (const auto MGR = Config::Lua::mgr()) {
                g_arranging = true;
                MGR->callLuaFn(
                    g_arrange,
                    [](lua_State* L) {
                        pushWorkspaces(L);
                        return 1;
                    },
                    Config::Lua::CConfigManager::LUA_TIMEOUT_EVENT_CALLBACK_MS, "hyprgrid layout");
                g_arranging = false;
            }
        }
        View::gridChanged();
    }

    // Once per batch of changes, after the event that made them.
    void schedule() {
        if (g_pending || g_arranging)
            return;
        g_pending = true;
        Later::run([] {
            g_pending = false;
            run();
        });
    }

    int luaLayout(lua_State* L) {
        if (g_arrange != LUA_NOREF)
            luaL_unref(L, LUA_REGISTRYINDEX, g_arrange);
        g_arrange = LUA_NOREF;
        if (lua_isfunction(L, 1)) {
            lua_pushvalue(L, 1);
            g_arrange = luaL_ref(L, LUA_REGISTRYINDEX);
            schedule(); // arrange what is there already
        } else if (!lua_isnil(L, 1))
            return luaL_error(L, "hyprgrid.layout: expected a function (or nil)");
        return 0;
    }

    int luaWorkspaces(lua_State* L) {
        pushWorkspaces(L);
        return 1;
    }

    int luaPlace(lua_State* L) {
        bool ok = lua_istable(L, 1);
        {
            std::vector<std::pair<int64_t, Grid::SCell>> moves;
            if (ok) {
                lua_pushnil(L);
                while (lua_next(L, 1)) {
                    // key: the id; value: { x, y } or { x = , y = }
                    if (lua_isinteger(L, -2) && lua_istable(L, -1)) {
                        lua_getfield(L, -1, "x");
                        lua_getfield(L, -2, "y");
                        if (lua_isnil(L, -2)) {
                            lua_pop(L, 2);
                            lua_rawgeti(L, -1, 1);
                            lua_rawgeti(L, -2, 2);
                        }
                        if (lua_isinteger(L, -2) && lua_isinteger(L, -1))
                            moves.emplace_back(lua_tointeger(L, -4), Grid::SCell{int(lua_tointeger(L, -2)), int(lua_tointeger(L, -1))});
                        else
                            ok = false;
                        lua_pop(L, 2);
                    } else
                        ok = false;
                    lua_pop(L, 1);
                }
            }
            ok = ok && Grid::placeAll(moves);
        }
        lua_pushboolean(L, ok);
        return 1;
    }
}

void Layout::init(void* handle) {
    const bool OK = HyprlandAPI::addLuaFunction(handle, "hyprgrid", "layout", luaLayout) && HyprlandAPI::addLuaFunction(handle, "hyprgrid", "workspaces", luaWorkspaces) &&
        HyprlandAPI::addLuaFunction(handle, "hyprgrid", "place", luaPlace);
    if (!OK)
        throw std::runtime_error("[hyprgrid] failed to register the layout functions");

    Grid::onChange(schedule);
    g_createdListener = Event::bus()->m_events.workspace.created.listen([](PHLWORKSPACEREF) { schedule(); });
    g_removedListener = Event::bus()->m_events.workspace.removed.listen([](PHLWORKSPACEREF) { schedule(); });
    g_reloadListener  = Event::bus()->m_events.config.preReload.listen([] { g_arrange = LUA_NOREF; });
}

void Layout::exit() {
    Grid::onChange(nullptr);
    g_createdListener.reset();
    g_removedListener.reset();
    g_reloadListener.reset();
    g_arrange = LUA_NOREF;
}
