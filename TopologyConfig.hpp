// The configured topology (hl.plugin.hyprgrid.topology()): which monitors
// share a board, and where a board's edges lead. step() answers what a move
// from a monitor's active workspace reaches. See TopologyConfig.cpp and
// docs/design.md.
#pragma once

#include "Topology.hpp"

#include <hyprland/src/desktop/DesktopTypes.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace TopologyConfig {
    void init(void* handle);
    void exit();

    // The board a monitor's workspaces are on.
    std::string boardOf(const PHLMONITOR& monitor);

    struct SStep {
        Topology::eKind        kind = Topology::eKind::NONE;
        std::optional<int64_t> id;      // the workspace reached (to create, for NEW); none for a named one
        PHLMONITOR             monitor; // the monitor it is on
    };

    // What a (dx, dy) step from `monitor`'s active workspace reaches.
    SStep step(const PHLMONITOR& monitor, const Motion::SCell& d);

    // Boundaries between the monitors' regions in the shared grid.
    std::vector<Topology::SSeam> seams();
}
