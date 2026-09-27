// The grid's layout strategy, in Lua (hl.plugin.hyprgrid.layout()): called
// after the grid changes, it may move workspaces. See Layout.cpp.
#pragma once

namespace Layout {
    void init(void* handle);
    void exit();
}
