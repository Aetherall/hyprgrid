// The C API (hyprgrid.h), for other plugins.

#include "hyprgrid.h"
#include "Grid.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>

extern "C" {
EXPORT int hyprgrid_api_version() {
    return HYPRGRID_API_VERSION;
}

EXPORT bool hyprgrid_cell(int64_t workspaceID, int* x, int* y) {
    const auto CELL = Grid::cellOf(workspaceID);
    if (!CELL)
        return false;
    if (x)
        *x = CELL->x;
    if (y)
        *y = CELL->y;
    return true;
}

EXPORT bool hyprgrid_move_workspace(int64_t workspaceID, int x, int y) {
    return Grid::moveWorkspace(workspaceID, {x, y});
}
}
