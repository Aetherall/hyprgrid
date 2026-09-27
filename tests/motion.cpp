// Tests for Motion.hpp: `make test`.

#include "Motion.hpp"

#include <cstdio>
#include <set>
#include <utility>

using Motion::SCell;
using Motion::SPoint;

static int g_failures = 0;

#define CHECK(cond)                                                                                                                                                                \
    do {                                                                                                                                                                           \
        if (!(cond)) {                                                                                                                                                             \
            std::printf("%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);                                                                                               \
            ++g_failures;                                                                                                                                                          \
        }                                                                                                                                                                          \
    } while (0)

static bool near(double a, double b) {
    return std::abs(a - b) < 1e-6;
}

// A grid of enterable cells.
static Motion::CanEnter cells(std::set<std::pair<int, int>> in) {
    return [in](const SCell& c) { return in.contains({c.x, c.y}); };
}

static void testProjection() {
    CHECK(near(Motion::project(2, 0), 2));
    CHECK(near(Motion::project(0, 1), 499)); // 0.998 / 0.002
}

static void testVelocity() {
    Motion::CVelocityTracker t;
    CHECK(t.velocity(0) == 0); // no samples

    t.add(0, 0);
    CHECK(t.velocity(0) == 0); // one sample

    for (uint32_t ms = 10; ms <= 100; ms += 10)
        t.add(ms, ms * 0.01);
    CHECK(near(t.velocity(100), 0.01));
    CHECK(near(t.velocity(150), 0.01)); // still within VELOCITY_STALE_MS
    CHECK(t.velocity(151) == 0);        // the fingers stopped before lifting

    // Only the latest VELOCITY_WINDOW_MS count: a slow start doesn't drag a
    // fast finish down.
    Motion::CVelocityTracker f;
    for (uint32_t ms = 0; ms <= 200; ms += 10)
        f.add(ms, ms <= 100 ? 0 : (ms - 100) * 0.05);
    CHECK(near(f.velocity(200), 0.05));
}

static void testBounds() {
    const auto B = Motion::boundsAround({0, 0}, cells({{1, 0}, {0, -1}}));
    CHECK(near(B.lo.x, 0) && near(B.hi.x, 1));
    CHECK(near(B.lo.y, -1) && near(B.hi.y, 0));
}

static void testOffsetWithin() {
    const SCell  A = {3, 5};
    const auto   B = Motion::boundsAround(A, cells({{4, 5}}));
    const SPoint O = Motion::offsetWithin({3.5, 5}, {0.8, 0.4}, A, B);
    CHECK(near(O.x, 1)); // clamped at the neighbour's centre
    CHECK(near(O.y, 0)); // nothing below
    CHECK(near(Motion::offsetWithin({3, 5}, {-0.5, 0}, A, B).x, 0)); // nothing left
}

static void testReached() {
    const SCell A = {0, 0};
    CHECK(!Motion::reached({0.99, 0}, A, cells({{1, 0}})));
    CHECK((Motion::reached({1, 0}, A, cells({{1, 0}})) == SCell{1, 0}));
    CHECK((Motion::reached({0, -1}, A, cells({{0, -1}})) == SCell{0, -1}));
    // A diagonal it can enter.
    CHECK((Motion::reached({1, 1}, A, cells({{1, 1}})) == SCell{1, 1}));
    // A diagonal it can't: the horizontal neighbour.
    CHECK((Motion::reached({1, 1}, A, cells({{1, 0}, {0, 1}})) == SCell{1, 0}));
    CHECK((Motion::reached({0.5, 1}, A, cells({{0, 1}})) == SCell{0, 1}));
}

static void testLanding() {
    const SCell A      = {0, 0};
    const auto  RIGHT  = cells({{1, 0}});
    const auto  AROUND = cells({{1, 0}, {0, 1}, {1, 1}});

    // Still, near the anchor: stays.
    CHECK((Motion::landing({0.2, 0}, {0, 0}, A, RIGHT) == A));
    // Still, past half way: the neighbour.
    CHECK((Motion::landing({0.6, 0}, {0, 0}, A, RIGHT) == SCell{1, 0}));
    // A flick carries a short drag over.
    CHECK((Motion::landing({0.1, 0}, {0.002, 0}, A, RIGHT) == SCell{1, 0}));
    // A flick back wins over a drag past half way.
    CHECK((Motion::landing({0.7, 0}, {-0.002, 0}, A, RIGHT) == A));
    // Toward a cell it can't enter: stays.
    CHECK((Motion::landing({0.2, 0}, {-0.01, 0}, A, RIGHT) == A));
    // Diagonal flick with the diagonal enterable.
    CHECK((Motion::landing({0.3, 0.3}, {0.01, 0.01}, A, AROUND) == SCell{1, 1}));
    // Diagonal flick without it: the nearer axis.
    CHECK((Motion::landing({0.3, 0.1}, {0.01, 0.002}, A, cells({{1, 0}, {0, 1}})) == SCell{1, 0}));
}

static void testZoomedOut() {
    const std::vector<SCell> CELLS = {{0, 0}, {0, 1}, {2, 1}, {-1, 3}};
    const auto               B     = Motion::boundsOver(CELLS, {0, 1});
    CHECK(near(B.lo.x, -1) && near(B.hi.x, 2) && near(B.lo.y, -1) && near(B.hi.y, 2));

    // Still: the nearest workspace, not the nearest cell (1, 1 is empty).
    CHECK((Motion::nearestLanding({1.2, 1}, {0, 0}, CELLS, {0, 1}) == SCell{2, 1}));
    CHECK((Motion::nearestLanding({0.9, 1}, {0, 0}, CELLS, {0, 1}) == SCell{0, 1}));
    // A flick coasts past the one under it.
    CHECK((Motion::nearestLanding({0, 1}, {-0.002, 0.004}, CELLS, {0, 1}) == SCell{-1, 3}));
    // Nothing to land on: stays.
    CHECK((Motion::nearestLanding({5, 5}, {0, 0}, {}, {0, 1}) == SCell{0, 1}));
}

static void testAhead() {
    // A row 0..3, a cell under 1, and one far below 3.
    const std::vector<SCell> CELLS = {{0, 0}, {1, 0}, {2, 0}, {3, 0}, {1, 1}, {3, 5}};
    // A notch right, barely moved: the next one on.
    CHECK((Motion::aheadLanding({0.1, 0}, {0.1, 0}, CELLS, {0, 0}) == SCell{1, 0}));
    CHECK((Motion::aheadLanding({2.0, 0}, {-0.1, 0}, CELLS, {2, 0}) == SCell{1, 0}));
    // Another notch mid-glide, almost there: past it.
    CHECK((Motion::aheadLanding({1.05, 0}, {0.1, 0}, CELLS, {1, 0}) == SCell{2, 0}));
    // Down: the cell under, not the far one.
    CHECK((Motion::aheadLanding({1, 0.1}, {0, 0.1}, CELLS, {1, 0}) == SCell{1, 1}));
    // Only in its column: nothing under 0, whatever is under 1.
    CHECK((Motion::aheadLanding({0, 0.1}, {0, 0.1}, CELLS, {0, 0}) == SCell{0, 0}));
    CHECK((Motion::aheadLanding({3, 0.1}, {0, 0.1}, CELLS, {3, 0}) == SCell{3, 5}));
    // Mid-glide off its row, the row it is nearest.
    CHECK((Motion::aheadLanding({1.4, 0.3}, {0.1, 0}, CELLS, {1, 0}) == SCell{2, 0}));
    CHECK((Motion::aheadLanding({0.4, 0.7}, {0.1, 0}, CELLS, {1, 0}) == SCell{1, 1}));
    // Nothing ahead: back to the nearest.
    CHECK((Motion::aheadLanding({3.1, 0}, {0.1, 0}, CELLS, {3, 0}) == SCell{3, 0}));
    CHECK((Motion::aheadLanding({0, -0.1}, {0, -0.1}, CELLS, {0, 0}) == SCell{0, 0}));
    // Not moved: the nearest.
    CHECK((Motion::aheadLanding({1.4, 0}, {0, 0}, CELLS, {0, 0}) == SCell{1, 0}));
    CHECK((Motion::aheadLanding({0, 0}, {1, 0}, {}, {7, 7}) == SCell{7, 7}));
}

int main() {
    testProjection();
    testVelocity();
    testBounds();
    testOffsetWithin();
    testReached();
    testLanding();
    testZoomedOut();
    testAhead();
    if (g_failures)
        std::printf("%d check(s) failed\n", g_failures);
    else
        std::printf("all motion checks passed\n");
    return g_failures ? 1 : 0;
}
