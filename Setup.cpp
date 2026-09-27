// hl.plugin.hyprgrid.setup(options) -> actions: runs the default policy
// (hyprgrid.lua, embedded at build time) against the Lua API, and returns its
// actions (focus, move_window, move_workspace, enter) for custom bindings.
// Shipping the Lua inside the plugin keeps it in step with the API it uses.

#include "Setup.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <stdexcept>

namespace {
    constexpr char POLICY[] =
#include "hyprgrid.lua.inc"
        ;

    int luaSetup(lua_State* L) {
        const bool HASOPTIONS = lua_gettop(L) >= 1;
        if (luaL_loadbuffer(L, POLICY, sizeof(POLICY) - 1, "=hyprgrid.lua") != LUA_OK)
            return lua_error(L);
        lua_call(L, 0, 1); // the module
        lua_getfield(L, -1, "setup");
        if (HASOPTIONS)
            lua_pushvalue(L, 1);
        else
            lua_pushnil(L);
        lua_call(L, 1, 1);
        return 1;
    }
}

void Setup::init(void* handle) {
    if (!HyprlandAPI::addLuaFunction(handle, "hyprgrid", "setup", luaSetup))
        throw std::runtime_error("[hyprgrid] failed to register setup()");
}
