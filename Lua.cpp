// hyprgrid's Lua API. Registers, on the focused monitor:
//   hl.plugin.hyprgrid.cell(id)             -> x, y, board  (nil: not on the grid)
//   hl.plugin.hyprgrid.neighbor(dx, dy)     -> id    (nil: can't go there)
//       The workspace on the cell (dx, dy) from the active one, on its board;
//       on an empty cell (if it can be entered), a fresh id placed there.
//       Switching or moving a window to that id creates it. (step() also
//       knows where a board's edges lead: TopologyConfig.cpp.)
//   hl.plugin.hyprgrid.drag_begin(dx, dy)   -> ok
//       Start holding the view from the active cell toward (dx, dy); false
//       if that cell can't be entered.
//   hl.plugin.hyprgrid.grab()               -> x, y  (nil: nothing moving)
//       Catch the view mid-glide and hold it from there; returns where it
//       is, relative to the active cell.
//   hl.plugin.hyprgrid.drag(dx, dy, time_ms) -> x, y, land_id
//       Move the held view by (dx, dy) cells (clamped to cells it can
//       enter); x, y is where it now is relative to the active cell. Reaching a
//       neighbour's centre returns that workspace's id: switch to it (the hold
//       carries on from there).
//   hl.plugin.hyprgrid.move(dx, dy)         -> moved
//       Put the active workspace on the cell (dx, dy) away, swapping with the
//       workspace there if any (you stay on it: only the grid changes).
//   hl.plugin.hyprgrid.drag_end(time_ms, stay) -> id, dx, dy  (nil: stayed)
//       Release: the view lands on the cell nearest where its velocity would
//       coast (UIKit deceleration), or back on the active cell if `stay`. For
//       another cell it returns that workspace to switch to (and the cell step
//       taken), and the view carries on at the fingers' speed.

#include "Lua.hpp"
#include "Grid.hpp"
#include "View.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/output/Monitor.hpp>

extern "C" {
#include <lua.h>
}

#include <stdexcept>

namespace {
    // The monitor drag_begin() / grab() held the view on: drag() and
    // drag_end() carry on there wherever the focus goes.
    PHLMONITORREF g_held;

    PHLWORKSPACE activeOn(const PHLMONITOR& mon) {
        return mon ? mon->m_activeWorkspace : nullptr;
    }

    // One Lua-driven hold at a time: a new one lets go of the last.
    void holding(const PHLMONITOR& mon) {
        if (const auto LAST = g_held.lock(); LAST && LAST != mon)
            View::release(LAST, 0, View::eLand::STAY);
        g_held = mon;
    }

    int luaCell(lua_State* L) {
        const auto ID   = int64_t(lua_tointeger(L, 1));
        const auto CELL = Grid::cellOf(ID);
        if (!CELL) {
            lua_pushnil(L);
            return 1;
        }
        lua_pushinteger(L, CELL->x);
        lua_pushinteger(L, CELL->y);
        std::string board;
        for (const auto& p : Grid::all()) {
            if (p.id == ID)
                board = p.board;
        }
        lua_pushstring(L, board.c_str());
        return 3;
    }

    int luaNeighbor(lua_State* L) {
        const auto MON   = Desktop::focusState()->monitor();
        const auto CELL  = Grid::cellOf(activeOn(MON));
        const auto BOARD = Grid::boardOf(MON);
        if (!CELL) {
            lua_pushnil(L);
            return 1;
        }
        const Grid::SCell TARGET = {CELL->x + int(lua_tointeger(L, 1)), CELL->y + int(lua_tointeger(L, 2))};
        if (!Grid::enterable(MON, TARGET)) {
            lua_pushnil(L);
            return 1;
        }
        lua_pushinteger(L, Grid::idFor(BOARD, TARGET));
        return 1;
    }

    int luaMove(lua_State* L) {
        const auto WS   = activeOn(Desktop::focusState()->monitor());
        const auto FROM = Grid::cellOf(WS);
        lua_pushboolean(L, FROM && Grid::moveWorkspace(WS, {FROM->x + int(lua_tointeger(L, 1)), FROM->y + int(lua_tointeger(L, 2))}));
        return 1;
    }

    int luaDragBegin(lua_State* L) {
        const auto MON = Desktop::focusState()->monitor();
        const bool OK  = View::holdToward(MON, int(lua_tointeger(L, 1)), int(lua_tointeger(L, 2)));
        if (OK)
            holding(MON);
        lua_pushboolean(L, OK);
        return 1;
    }

    int luaGrab(lua_State* L) {
        const auto MON = Desktop::focusState()->monitor();
        const auto POS = View::grab(MON);
        if (!POS) {
            lua_pushnil(L);
            return 1;
        }
        holding(MON);
        lua_pushnumber(L, POS->x);
        lua_pushnumber(L, POS->y);
        return 2;
    }

    int luaDrag(lua_State* L) {
        const auto MOVE = View::moveHeld(g_held.lock(), {lua_tonumber(L, 1), lua_tonumber(L, 2)}, uint32_t(lua_tointeger(L, 3)));
        if (!MOVE) {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, MOVE->offset.x);
        lua_pushnumber(L, MOVE->offset.y);
        if (!MOVE->land)
            return 2;
        lua_pushinteger(L, *MOVE->land);
        return 3;
    }

    int luaDragEnd(lua_State* L) {
        const auto RELEASE = View::release(g_held.lock(), uint32_t(lua_tointeger(L, 1)), lua_toboolean(L, 2) ? View::eLand::STAY : View::eLand::FLICK);
        if (!RELEASE) {
            lua_pushnil(L);
            return 1;
        }
        lua_pushinteger(L, RELEASE->id);
        lua_pushinteger(L, RELEASE->step.x);
        lua_pushinteger(L, RELEASE->step.y);
        return 3;
    }
}

void Lua::init(void* handle) {
    const bool OK = HyprlandAPI::addLuaFunction(handle, "hyprgrid", "cell", luaCell) && HyprlandAPI::addLuaFunction(handle, "hyprgrid", "neighbor", luaNeighbor) &&
        HyprlandAPI::addLuaFunction(handle, "hyprgrid", "drag_begin", luaDragBegin) && HyprlandAPI::addLuaFunction(handle, "hyprgrid", "grab", luaGrab) &&
        HyprlandAPI::addLuaFunction(handle, "hyprgrid", "drag", luaDrag) && HyprlandAPI::addLuaFunction(handle, "hyprgrid", "drag_end", luaDragEnd) &&
        HyprlandAPI::addLuaFunction(handle, "hyprgrid", "move", luaMove);
    if (!OK)
        throw std::runtime_error("[hyprgrid] failed to register its Lua functions");
}
