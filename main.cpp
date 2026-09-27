// hyprgrid: a 2D workspace grid for Hyprland (Grid.cpp) and, per monitor, a
// view moving across it (View.cpp), driven from Lua (hl.plugin.hyprgrid,
// Lua.cpp) and gesture rules (Gestures.cpp), and released iOS-style
// (Motion.hpp, Kinetics.hpp); the overview (overview/) draws it zoomed out.
// Other plugins use it through the C API in hyprgrid.h.

#include "Gestures.hpp"
#include "Grid.hpp"
#include "Later.hpp"
#include "Layout.hpp"
#include "TopologyConfig.hpp"
#include "Lua.hpp"
#include "Setup.hpp"
#include "View.hpp"
#include "hyprgrid.h"
#include "overview/Overview.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/plugins/PluginSystem.hpp>
#include <hyprland/src/config/lua/ConfigManager.hpp>

extern "C" {
#include <lua.h>
}

#include <stdexcept>

static HANDLE PHANDLE = nullptr;

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    // Built against a different Hyprland than the one running: the internals we
    // touch may have moved, so refuse to load rather than corrupt state.
    if (std::string{__hyprland_api_get_hash()} != std::string{__hyprland_api_get_client_hash()}) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprgrid] built for a different Hyprland; not loading", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hyprgrid] version mismatch");
    }

    // Built against another Lua than Hyprland's: its constants (the registry
    // index) would point elsewhere, and the first luaL_ref would crash.
    if (const auto MGR = Config::Lua::mgr()) {
        const auto LIFE = MGR->luaStateLifetime();
        if (LIFE && LIFE->state && int(lua_version(LIFE->state)) != LUA_VERSION_NUM) {
            HyprlandAPI::addNotification(PHANDLE, "[hyprgrid] built for another Lua than Hyprland's; not loading", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
            throw std::runtime_error("[hyprgrid] Lua version mismatch");
        }
    }

    // The standalone hyprgrid-overview hooks the same renderer functions as
    // the overview in here: both loaded, each would undo the other's hooks.
    // Checked before anything is registered.
    for (const auto* plugin : g_pPluginSystem->getAllPlugins()) {
        if (plugin && plugin->m_name == "hyprgrid-overview") {
            HyprlandAPI::addNotification(PHANDLE, "[hyprgrid] unload hyprgrid-overview first: hyprgrid includes the overview; not loading", CHyprColor{1.0, 0.2, 0.2, 1.0}, 8000);
            throw std::runtime_error("[hyprgrid] hyprgrid-overview is loaded");
        }
    }

    // A step failing leaves nothing behind: listeners registered by the ones
    // before it would outlive the plugin once Hyprland drops it.
    try {
        Grid::init();
        TopologyConfig::init(PHANDLE);
        View::init();
        Lua::init(PHANDLE);
        Gestures::init(PHANDLE);
        Layout::init(PHANDLE);
        Overview::init(PHANDLE);
    } catch (...) {
        Later::cancelAll();
        Overview::exit();
        Layout::exit();
        Gestures::exit();
        View::exit();
        TopologyConfig::exit();
        throw;
    }
    Setup::init(PHANDLE);

    return {HYPRGRID_PLUGIN_NAME, "A 2D workspace grid, a view gliding over it, and an overview of it", "aetherall; overview from hyprland-scroll-overview by Vaxry, yayuuu", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    // Hyprland drops a plugin's Lua functions when it unloads.
    Later::cancelAll();
    Overview::exit();
    Layout::exit();
    Gestures::exit();
    TopologyConfig::exit();
    View::exit();
}
