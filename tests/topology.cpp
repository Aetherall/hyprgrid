// Tests for Topology.hpp: `make test`.

#include "Topology.hpp"

#include <cstdio>
#include <set>
#include <utility>

using namespace Topology;

static int g_failures = 0;

#define CHECK(cond)                                                                                                                                                                \
    do {                                                                                                                                                                           \
        if (!(cond)) {                                                                                                                                                             \
            std::printf("%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);                                                                                               \
            ++g_failures;                                                                                                                                                          \
        }                                                                                                                                                                          \
    } while (0)

static std::function<bool(const SCell&)> in(std::set<std::pair<int, int>> cells) {
    return [cells](const SCell& c) { return cells.contains({c.x, c.y}); };
}

static SQuery query(SCell step, std::set<std::pair<int, int>> occupied, std::set<std::pair<int, int>> enterable, std::optional<std::string> link, eGrow grow) {
    return {{0, 0}, step, in(occupied), in(enterable), link, grow};
}

static void testOrder() {
    // A workspace that way comes first, even toward a link.
    auto s = step(query({-1, 0}, {{-1, 0}}, {}, "DP-2", eGrow::OPEN_EDGES));
    CHECK(s.kind == eKind::WORKSPACE && s.cell == (SCell{-1, 0}));

    // Nothing there: the link, rather than growing toward it.
    s = step(query({-1, 0}, {}, {{-1, 0}}, "DP-2", eGrow::OPEN_EDGES));
    CHECK(s.kind == eKind::MONITOR && s.monitor == "DP-2");

    // No link: grow.
    s = step(query({0, 1}, {}, {{0, 1}}, std::nullopt, eGrow::OPEN_EDGES));
    CHECK(s.kind == eKind::NEW && s.cell == (SCell{0, 1}));

    // "always": grow first, the link only where it can't.
    s = step(query({-1, 0}, {}, {{-1, 0}}, "DP-2", eGrow::ALWAYS));
    CHECK(s.kind == eKind::NEW);
    s = step(query({-1, 0}, {}, {}, "DP-2", eGrow::ALWAYS));
    CHECK(s.kind == eKind::MONITOR);

    // "never": no new cells.
    s = step(query({0, 1}, {}, {{0, 1}}, std::nullopt, eGrow::NEVER));
    CHECK(s.kind == eKind::NONE);

    // Nothing at all.
    s = step(query({1, 0}, {}, {}, std::nullopt, eGrow::OPEN_EDGES));
    CHECK(s.kind == eKind::NONE);
}

static void testSides() {
    CHECK(sideOf({-1, 0}) == eSide::LEFT && sideOf({1, 0}) == eSide::RIGHT && sideOf({0, -1}) == eSide::UP && sideOf({0, 1}) == eSide::DOWN);
    CHECK(!sideOf({1, 1}) && !sideOf({0, 0}));
    CHECK(opposite(eSide::LEFT) == eSide::RIGHT && opposite(eSide::UP) == eSide::DOWN);
}

static void testLandingInLine() {
    const std::vector<SCell> BOARD = {{0, 0}, {1, 0}, {2, 0}, {0, 1}, {3, 1}};
    // Entered by its right edge (coming from a monitor on the right): the rightmost in the row.
    CHECK((landingInLine(BOARD, {7, 0}, eSide::RIGHT) == SCell{2, 0}));
    CHECK((landingInLine(BOARD, {7, 1}, eSide::RIGHT) == SCell{3, 1}));
    // By its left edge: the leftmost.
    CHECK((landingInLine(BOARD, {-4, 1}, eSide::LEFT) == SCell{0, 1}));
    // Nothing in that row.
    CHECK(!landingInLine(BOARD, {7, 5}, eSide::RIGHT));
    // Across a top/bottom link: the column, nearest the edge entered.
    CHECK((landingInLine(BOARD, {0, -3}, eSide::UP) == SCell{0, 0}));
    CHECK((landingInLine(BOARD, {0, 9}, eSide::DOWN) == SCell{0, 1}));
}

static void testRegions() {
    bool       first = false;
    const auto SIDE  = seamBetween(2680, 180, first); // DP-3's centre is right of DP-2's
    CHECK(SIDE.vertical && first);
    const auto STACK = seamBetween(100, -900, first); // the second is above
    CHECK(!STACK.vertical && !first);

    CHECK(!inSecond(SIDE, {-1, 5}) && inSecond(SIDE, {0, -3}) && inSecond(SIDE, {4, 0}));
    CHECK(!inSecond(STACK, {9, -1}) && inSecond(STACK, {-9, 0}));

    CHECK((nearSeam(SIDE, false, 3) == SCell{-1, 3}) && (nearSeam(SIDE, true, 3) == SCell{0, 3}));
    CHECK((nearSeam(STACK, false, 2) == SCell{2, -1}) && (nearSeam(STACK, true, 2) == SCell{2, 0}));
    CHECK(alongSeam(SIDE, {7, 4}) == 4 && alongSeam(STACK, {7, 4}) == 7);

    // Three side-by-side monitors share a board without sharing a region.
    const auto SEAMS = stripeSeams(3, true);
    CHECK(SEAMS.size() == 2 && SEAMS[0].vertical && SEAMS[0].at == 0 && SEAMS[1].at == 1);
    CHECK(stripeOwner({-3, 5}, true, 3) == 0);
    CHECK(stripeOwner({0, 5}, true, 3) == 1);
    CHECK(stripeOwner({1, 5}, true, 3) == 2);
    CHECK((stripeStart(0, true, 5) == SCell{-1, 5}));
    CHECK((stripeStart(1, true, 5) == SCell{0, 5}));
    CHECK((stripeStart(2, true, 5) == SCell{1, 5}));

    CHECK(stripeOwner({4, -2}, false, 3) == 0);
    CHECK(stripeOwner({4, 0}, false, 3) == 1);
    CHECK(stripeOwner({4, 1}, false, 3) == 2);
    CHECK((stripeStart(2, false, 4) == SCell{4, 1}));
    CHECK(stripeSeams(1, true).empty());
    CHECK(stripeOwner({-1, 2}, true, 2) == 0 && stripeOwner({0, 2}, true, 2) == 1);
}

int main() {
    testOrder();
    testSides();
    testLandingInLine();
    testRegions();
    if (g_failures)
        std::printf("%d check(s) failed\n", g_failures);
    else
        std::printf("all topology checks passed\n");
    return g_failures ? 1 : 0;
}
