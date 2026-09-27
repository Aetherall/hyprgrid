#include "Config.hpp"

#include <algorithm>
#include <config/shared/complex/ComplexDataType.hpp>
#include <config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/config/values/types/GradientValue.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>

#include <regex>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace {


int dispatcherFactoryLua(lua_State* L, std::string_view name);

using TArgValidator = bool (*)(std::string_view);

struct SDispatcher {
    std::string_view                    name;
    std::regex                          argPattern;
    std::string_view                    defaultArg;
    std::string_view                    typeArgError;
    std::string_view                    invalidArgError;
    TArgValidator                       argValidator = nullptr;
    Overview::Config::TDispatcher dispatcher   = nullptr;
    lua_CFunction                       luaFunction  = nullptr;

    bool isArgValid(const std::string_view arg) const {
        if (argValidator)
            return argValidator(arg);

        return std::regex_match(arg.begin(), arg.end(), argPattern);
    }
};

SDispatcher* findDispatcher(const std::string_view name) {
    static SDispatcher registrations[] = {
        {
            .name            = "overview",
            .argPattern      = std::regex{R"(^(select|(toggle|on|open|enable|off|close|disable)([ \t]+(all|[^ \t"\\]+))?)$)"},
            .defaultArg      = "toggle",
            .typeArgError    = "expected an optional string argument; did you forget quotes around it?",
            .invalidArgError = "expected select or toggle/open/close [monitor|all] (aliases: on, enable, off, disable)",
            .luaFunction     = [](lua_State* L) { return dispatcherFactoryLua(L, "overview"); },
        },
        {
            .name            = "overview_navigate",
            .argPattern      = std::regex{"^(left|right|up|down)$"},
            .typeArgError    = "expected a string argument",
            .invalidArgError = "expected one of: left, right, up, down",
            .luaFunction     = [](lua_State* L) { return dispatcherFactoryLua(L, "overview_navigate"); },
        },
        {
            .name            = "overview_window",
            .argPattern      = std::regex{"^(select|close)$"},
            .typeArgError    = "expected a string argument",
            .invalidArgError = "expected one of: select, close",
            .luaFunction     = [](lua_State* L) { return dispatcherFactoryLua(L, "overview_window"); },
        },
    };

    const auto MATCH = std::ranges::find_if(registrations, [name](const auto& registration) { return registration.name == name; });
    return MATCH == std::end(registrations) ? nullptr : &*MATCH;
}

int runDispatcherNow(lua_State* L, const SDispatcher& dispatcher, const char* arg) {
    if (!dispatcher.dispatcher)
        return luaL_error(L, "%s: dispatcher is not registered", dispatcher.name.data());

    const auto result = dispatcher.dispatcher(arg);
    if (!result.success)
        return luaL_error(L, "%s: %s", dispatcher.name.data(), result.error.c_str());

    return 0;
}

void pushDispatcherBindAction(lua_State* L, const char* name, const char* arg) {
    const std::string CODE = "return function() return hl.plugin.hyprgrid._overview_dispatch(\"" + std::string{name} + "\", \"" + std::string{arg} + "\") end";

    if (luaL_loadstring(L, CODE.c_str()) != LUA_OK)
        lua_error(L);

    if (lua_pcall(L, 0, 1, 0) != LUA_OK)
        lua_error(L);
}

int runDispatcherActionLua(lua_State* L) {
    if (lua_gettop(L) < 2 || lua_isnoneornil(L, 1) || lua_isnoneornil(L, 2))
        return luaL_error(L, "_overview_dispatch: expected dispatcher name and argument");

    if (!lua_isstring(L, 1) || !lua_isstring(L, 2))
        return luaL_error(L, "_overview_dispatch: expected string arguments");

    const char* name = lua_tostring(L, 1);
    const char* arg  = lua_tostring(L, 2);
    const auto  DISPATCHER = findDispatcher(name);

    if (!DISPATCHER)
        return luaL_error(L, "_overview_dispatch: unknown dispatcher '%s'", name);
    if (!DISPATCHER->isArgValid(arg))
        return luaL_error(L, "%s: invalid argument '%s', %s", name, arg, DISPATCHER->invalidArgError.data());

    return runDispatcherNow(L, *DISPATCHER, arg);
}

int dispatcherFactoryLua(lua_State* L, std::string_view name) {
    const auto DISPATCHER = findDispatcher(name);
    if (!DISPATCHER)
        return luaL_error(L, "%s: dispatcher metadata is not registered", name.data());

    const char* arg = DISPATCHER->defaultArg.empty() ? nullptr : DISPATCHER->defaultArg.data();

    if (!arg && (lua_gettop(L) < 1 || lua_isnoneornil(L, 1)))
        return luaL_error(L, "%s: %s", DISPATCHER->name.data(), DISPATCHER->typeArgError.data());

    if (lua_gettop(L) >= 1) {
        if (lua_isnoneornil(L, 1))
            return luaL_error(L, "%s: %s", DISPATCHER->name.data(), DISPATCHER->typeArgError.data());

        if (!lua_isstring(L, 1))
            return luaL_error(L, "%s: %s", DISPATCHER->name.data(), DISPATCHER->typeArgError.data());

        arg = lua_tostring(L, 1);
    }

    if (!DISPATCHER->isArgValid(arg))
        return luaL_error(L, "%s: invalid argument '%s', %s", DISPATCHER->name.data(), arg, DISPATCHER->invalidArgError.data());

    const auto& ACTIONSTATE = Config::Actions::state();
    if (ACTIONSTATE && ACTIONSTATE->m_bindInvocationDepth > 0) {
        return runDispatcherNow(L, *DISPATCHER, arg);
    }

    pushDispatcherBindAction(L, DISPATCHER->name.data(), arg);

    return 1;
}

}

namespace Overview::Config {

void registerDispatcher(const std::string& name, TDispatcher dispatcher) {
    HyprlandAPI::addDispatcherV2(OVERVIEW_HANDLE, "hyprgrid:" + name, dispatcher);

    if (::Config::mgr()->type() != ::Config::CONFIG_LUA)
        return;

    const auto DISPATCHER = findDispatcher(name);
    if (!DISPATCHER)
        return;

    DISPATCHER->dispatcher = dispatcher;
    HyprlandAPI::addLuaFunction(OVERVIEW_HANDLE, "hyprgrid", std::string{DISPATCHER->name}, DISPATCHER->luaFunction);
}

static void registerLuaFunctions() {
    if (::Config::mgr()->type() != ::Config::CONFIG_LUA)
        return;

    HyprlandAPI::addLuaFunction(OVERVIEW_HANDLE, "hyprgrid", "_overview_dispatch", ::runDispatcherActionLua);
}

static void registerConfigValues() {
    using namespace ::Config::Values;

    HyprlandAPI::addConfigValueV2(OVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:hyprgrid:overview:scale", "overview scale", 0.5F, SFloatValueOptions{.min = 0.1F, .max = 0.9F}));
    HyprlandAPI::addConfigValueV2(OVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:hyprgrid:overview:workspace_gap", "gap between overview workspaces", 0, SIntValueOptions{.min = 0}));
    HyprlandAPI::addConfigValueV2(OVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:hyprgrid:overview:cross_monitor_drag", "enable cross-monitor window dragging", false));
    HyprlandAPI::addConfigValueV2(OVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:hyprgrid:overview:input:left_handed", "overview left handed mouse buttons, 2 follows input:left_handed", 2,
                                                        SIntValueOptions{.min = 0, .max = 2}));
    HyprlandAPI::addConfigValueV2(OVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:hyprgrid:overview:input:drag_mode", "overview mouse drag behavior", 0,
                                                        SIntValueOptions{.min = 0, .max = 1}));
    HyprlandAPI::addConfigValueV2(OVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:hyprgrid:overview:input:drag_threshold", "overview drag threshold", 10,
                                                        SIntValueOptions{.min = 0}));
    HyprlandAPI::addConfigValueV2(OVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:hyprgrid:overview:wallpaper", "wallpaper mode", 0, SIntValueOptions{.min = 0, .max = 2}));
    HyprlandAPI::addConfigValueV2(OVERVIEW_HANDLE, makeShared<CBoolValue>("plugin:hyprgrid:overview:blur", "blur the overview wallpaper", false));
    HyprlandAPI::addConfigValueV2(OVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:hyprgrid:overview:shadow:enabled", "draw a shadow around each workspace card", false));
    HyprlandAPI::addConfigValueV2(OVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:hyprgrid:overview:shadow:range", "workspace card shadow range", -1));
    HyprlandAPI::addConfigValueV2(OVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:hyprgrid:overview:shadow:render_power", "workspace card shadow render power", -1));
    HyprlandAPI::addConfigValueV2(OVERVIEW_HANDLE,
                                  makeShared<CGradientValue>("plugin:hyprgrid:overview:shadow:color", "workspace card shadow color", -1));
}

void registerConfig() {
    registerLuaFunctions();
    registerConfigValues();
    HyprlandAPI::reloadConfig();
}

float getScale() {
    return std::clamp(getValue<float>("plugin:hyprgrid:overview:scale"), 0.1F, 0.9F);
}

int getWorkspaceGap() {
    return std::max<int>(0, getValue<int>("plugin:hyprgrid:overview:workspace_gap"));
}

bool getCrossMonitorDrag() {
    return getValue<bool>("plugin:hyprgrid:overview:cross_monitor_drag");
}

bool getLeftHanded() {
    const auto LEFT_HANDED = getValue<int>("plugin:hyprgrid:overview:input:left_handed");
    if (LEFT_HANDED <= 1)
        return LEFT_HANDED != 0;

    return getValue<bool>("input:left_handed");
}

int getDragMode() {
    return std::clamp(getValue<int>("plugin:hyprgrid:overview:input:drag_mode"), 0, 1);
}

int getDragThreshold() {
    return std::max<int>(0, getValue<int>("plugin:hyprgrid:overview:input:drag_threshold"));
}

int getWallpaperMode() {
    return std::clamp<int>(getValue<int>("plugin:hyprgrid:overview:wallpaper"), 0, 2);
}

bool getBlur() {
    return getValue<bool>("plugin:hyprgrid:overview:blur");
}

int getShadowEnabled() {
    return getValue<bool>("plugin:hyprgrid:overview:shadow:enabled") ? 1 : 0;
}

int getShadowRange() {
    return getValue<int>("plugin:hyprgrid:overview:shadow:range");
}

int getShadowRenderPower() {
    return getValue<int>("plugin:hyprgrid:overview:shadow:render_power");
}

std::optional<::Config::CGradientValueData> getShadowColor() {
    constexpr auto NAME = "plugin:hyprgrid:overview:shadow:color";

    if (!::Config::mgr()->getConfigValue(NAME).setByUser)
        return std::nullopt;

    return getValue<::Config::CGradientValueData>(NAME);
}

}
