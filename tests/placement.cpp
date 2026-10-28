// Initial workspace placement without Hyprland: `make test`.
#include "Placement.hpp"
#include "Topology.hpp"

#include <cassert>
#include <cstdio>
#include <set>
#include <utility>

using Motion::SCell;

static void testNoRegions() {
    const std::function<SCell(int)> none;
    const auto empty = [](const SCell&) { return false; };
    assert((Placement::initialCell(1, none, empty) == SCell{0, 0}));
    assert((Placement::initialCell(4, none, empty) == SCell{0, 3}));
    assert((Placement::initialCell(100, none, empty) == SCell{0, 0}));
}

static void testRegions(bool vertical) {
    std::set<std::pair<int, int>> taken;
    const auto occupied = [&](const SCell& c) { return taken.contains({c.x, c.y}); };
    const auto place = [&](int64_t id, size_t region) {
        const auto near = [=](int along) { return Topology::stripeStart(region, vertical, along); };
        const auto cell = Placement::initialCell(id, near, occupied);
        taken.insert({cell.x, cell.y});
        return cell;
    };

    // IDs are global and may be assigned in any order across the regions.
    for (const auto [id, region] : {std::pair{7, size_t{2}}, {1, size_t{0}}, {12, size_t{1}}})
        assert(place(id, region) == Topology::stripeStart(region, vertical, 0));

    assert(place(2, 0) == Topology::stripeStart(0, vertical, 1));
    assert(place(4, 2) == Topology::stripeStart(2, vertical, 1));

    // Existing gaps do not cause overlap or move an existing workspace.
    const auto blocked = Topology::stripeStart(1, vertical, 1);
    taken.insert({blocked.x, blocked.y});
    assert(place(3, 1) == Topology::stripeStart(1, vertical, 2));
}

int main() {
    testNoRegions();
    testRegions(true);
    testRegions(false);
    std::puts("all placement checks passed");
}
