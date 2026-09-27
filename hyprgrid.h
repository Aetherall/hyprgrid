/* hyprgrid's C API, for other Hyprland plugins (an overview, a bar...).
 *
 * hyprgrid exports these symbols from its shared object. Find it among the
 * loaded plugins by name ("hyprgrid"), dlsym() them from its handle, and check
 * hyprgrid_api_version() against HYPRGRID_API_VERSION before calling anything
 * else. hyprgrid may be loaded after you, or unloaded and reloaded: resolve
 * the symbols again whenever its handle changes, and treat a missing hyprgrid
 * as "no grid". Copy this header into your plugin; the version check guards
 * against it going stale.
 *
 * Call from Hyprland's main thread only.
 */
#ifndef HYPRGRID_H
#define HYPRGRID_H

#include <stdbool.h>
#include <stdint.h>

#define HYPRGRID_PLUGIN_NAME "hyprgrid"
#define HYPRGRID_API_VERSION 2

#ifdef __cplusplus
extern "C" {
#endif

/* The API version this hyprgrid implements. */
int hyprgrid_api_version(void);

/* The cell (x, y) of the numbered workspace `workspace_id`, placing it on the
 * grid if it's the first time hyprgrid sees it. False: not on the grid
 * (special or named workspace, or no such workspace). */
bool hyprgrid_cell(int64_t workspace_id, int* x, int* y);

/* Put workspace `workspace_id` on cell (x, y), swapping with the workspace
 * there if any. False: not on the grid, or already there. */
bool hyprgrid_move_workspace(int64_t workspace_id, int x, int y);

typedef int (*hyprgrid_api_version_fn)(void);
typedef bool (*hyprgrid_cell_fn)(int64_t, int*, int*);
typedef bool (*hyprgrid_move_workspace_fn)(int64_t, int, int);

#ifdef __cplusplus
}
#endif

#endif
