// Tests for overview/Interaction.hpp: `make test`.

#include "overview/Interaction.hpp"

#include <cmath>
#include <cstdio>
#include <string>

using namespace Interaction;

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

static bool near(const SPoint& a, const SPoint& b) {
    return near(a.x, b.x) && near(a.y, b.y);
}

static bool near(const SBox& a, const SBox& b) {
    return near(a.x, b.x) && near(a.y, b.y) && near(a.w, b.w) && near(a.h, b.h);
}

static void testContains() {
    const SBox B{10, 10, 10, 10};
    CHECK(contains(B, {10, 10}));
    CHECK(contains(B, {19.9, 19.9}));
    // Half-open: the next box starts where this one ends.
    CHECK(!contains(B, {20, 15}));
    CHECK(!contains(B, {15, 20}));
    CHECK(!contains(B, {9.9, 15}));
    CHECK(!contains({0, 0, 0, 0}, {0, 0}));

    CHECK(near(distanceSq({15, 15}, B), 0));
    CHECK(near(distanceSq({5, 15}, B), 25));
    CHECK(near(distanceSq({23, 24}, B), 9 + 16));
    CHECK(near(overlapArea(B, {15, 15, 10, 10}), 25));
    CHECK(near(overlapArea(B, {30, 30, 10, 10}), 0));
}

// Two tiled side by side, a floating one over both.
static const std::vector<SWindow> STACK = {
    {{0, 0, 100, 100}, false},
    {{100, 0, 100, 100}, false},
    {{50, 25, 100, 50}, true},
};

static void testWindowAt() {
    CHECK(windowAt(STACK, {10, 10}) == 0);
    CHECK(windowAt(STACK, {190, 90}) == 1);
    // The floating one is over them.
    CHECK(windowAt(STACK, {60, 50}) == 2);
    CHECK(windowAt(STACK, {140, 50}) == 2);
    CHECK(!windowAt(STACK, {250, 50}));

    // Floating wins even when below in the stack; the topmost of each kind.
    const std::vector<SWindow> LOW = {{{0, 0, 100, 100}, true}, {{0, 0, 100, 100}, false}, {{0, 0, 50, 50}, true}};
    CHECK(windowAt(LOW, {10, 10}) == 2);
    CHECK(windowAt(LOW, {80, 80}) == 0);

    // Over a fullscreen window: the tiled one behind it is hidden, the floating one still shows.
    const std::vector<SWindow> FS = {{{0, 0, 100, 100}, false}, {{0, 0, 200, 100}, false}, {{150, 0, 20, 20}, true}};
    CHECK(windowAt(FS, {10, 10}, 1) == 1);
    CHECK(windowAt(FS, {160, 10}, 1) == 2);
    CHECK(!windowAt(FS, {10, 150}, 1));
}

static void testNearestCentre() {
    const SBox CELL{0, 0, 200, 100};
    CHECK(nearestCentre(STACK, CELL) == 2);
    CHECK(nearestCentre({STACK[0], STACK[1]}, CELL) == 0); // equals: the first
    CHECK(!nearestCentre({}, CELL));
    // Over a fullscreen window, only it and floating ones count.
    const std::vector<SWindow> FS = {{{50, 0, 100, 100}, false}, {{0, 0, 10, 10}, false}, {{150, 60, 10, 10}, true}};
    CHECK(nearestCentre(FS, CELL, 1) == 2);
}

static void testDrop() {
    const SBox B{0, 0, 90, 100};
    CHECK(dropSide(B, {10, 50}) == ESide::LEFT);
    CHECK(dropSide(B, {80, 50}) == ESide::RIGHT);
    CHECK(dropSide(B, {45, 10}) == ESide::UP);
    CHECK(dropSide(B, {45, 90}) == ESide::DOWN);
    CHECK(dropSide(B, {35, 10}) == ESide::UP); // past the left third
    CHECK(dropSide(B, {55, 90}) == ESide::DOWN); // short of the right one

    // The hitbox is where the layout has it; the nearest drawn box wins where hitboxes overlap.
    const std::vector<SDropTarget> T = {
        {{0, 0, 100, 100}, {0, 0, 120, 100}},
        {{110, 0, 100, 100}, {}},
    };
    CHECK(dropTarget(T, {50, 50}) == 0);
    CHECK(dropTarget(T, {115, 50}) == 1); // in both hitboxes, inside the second's box
    CHECK(dropTarget(T, {105, 50}) == 0); // only the first's hitbox
    CHECK(!dropTarget(T, {250, 50}));
}

static void testHold() {
    const SBox B{100, 100, 200, 100};
    const auto R = grabRatio(B, {150, 175});
    CHECK(near(R, {0.25, 0.75}));
    CHECK(near(heldAt({200, 100}, {150, 175}, R), B));
    // Held the same way at half the size: the same spot under the pointer.
    CHECK(near(heldAt({100, 50}, {150, 175}, R), {125, 137.5, 100, 50}));
    CHECK(near(grabRatio({0, 0, 0, 0}, {5, 5}), {0.5, 0.5}));
    CHECK(near(heldAt({10, 10}, {0, 0}, {2, -1}), {-10, 0, 10, 10})); // ratios clamp

    const SBox AREA{0, 0, 1000, 500};
    CHECK(near(clampInto({-50, 450, 100, 100}, AREA, 10), {10, 390, 100, 100}));
    CHECK(near(clampInto({500, 200, 100, 100}, AREA, 10), {500, 200, 100, 100}));
    CHECK(near(clampInto({500, 200, 2000, 100}, AREA, 10), {10, 200, 2000, 100})); // too wide: the left edge
    CHECK(near(clampInto({5, 5, 1, 1}, {}, 10), {5, 5, 1, 1}));
    CHECK(near(centreIn({200, 100}, AREA), {400, 200, 200, 100}));
    CHECK(near(centreIn({2000, 100}, AREA), {0, 200, 2000, 100}));
}

static void testResize() {
    const SBox B{100, 100, 200, 100};
    CHECK(cornerAt(B, {110, 110}) == (SCorner{true, true}));
    CHECK(cornerAt(B, {290, 190}) == (SCorner{false, false}));
    CHECK(cornerAt(B, {290, 110}) == (SCorner{false, true}));

    // The corner moves, the opposite one stays.
    CHECK(near(resize(B, {10, 20}, {false, false}, {1, 1}), {100, 100, 210, 120}));
    CHECK(near(resize(B, {10, 20}, {true, true}, {1, 1}), {110, 120, 190, 80}));
    CHECK(near(resize(B, {-10, 5}, {true, false}, {1, 1}), {90, 100, 210, 105}));
    // Past the minimum, the moved corner stops there.
    CHECK(near(resize(B, {-500, 0}, {false, false}, {50, 50}), {100, 100, 50, 100}));
    CHECK(near(resize(B, {500, 0}, {true, false}, {50, 50}), {250, 100, 50, 100}));
    CHECK(near(resize(B, {500, 500}, {false, false}, {1, 1}, SPoint{250, 120}), {100, 100, 250, 120}));

    const SBox AREA{0, 0, 400, 300};
    CHECK(near(clampResize({-20, 50, 200, 100}, AREA, {true, false}, 5), {5, 50, 175, 100}));
    CHECK(near(clampResize({250, 250, 200, 100}, AREA, {false, false}, 5), {250, 250, 145, 45}));
    // Only the moved corner: the fixed one stays even outside.
    CHECK(near(clampResize({-20, 50, 200, 100}, AREA, {false, false}, 0), {-20, 50, 200, 100}));
}

static SNeighbour at(const SBox& b) {
    return {b, centre(b)};
}

static void testNeighbour() {
    // A 2x2 of tiled windows, and one wide window under them.
    const auto TL = at({0, 0, 100, 100}), TR = at({100, 0, 100, 100}), BL = at({0, 100, 100, 100}), BR = at({100, 100, 100, 100});
    const std::vector<SNeighbour> GRID = {TR, BL, BR};
    CHECK(neighbour(TL, GRID, {1, 0}) == 0);
    CHECK(neighbour(TL, GRID, {0, 1}) == 1);
    CHECK(!neighbour(TL, GRID, {-1, 0}));
    CHECK(!neighbour(TL, GRID, {0, -1}));

    // Sideways, one that lines up beats a nearer one that doesn't.
    const auto LINED = at({300, 0, 100, 100}), OFF = at({150, 150, 100, 100});
    CHECK(neighbour(TL, {OFF, LINED}, {1, 0}) == 1);
    // Both lined up: the nearer.
    CHECK(neighbour(TL, {LINED, TR}, {1, 0}) == 1);
    // With none lined up, the nearest still counts sideways ...
    CHECK(neighbour(TL, {OFF}, {1, 0}) == 0);
    // ... but going down, only one below it across counts.
    CHECK(!neighbour(TL, {at({150, 150, 100, 100})}, {0, 1}));

    // Equally near: the one overlapping most.
    const auto A = at({100, 0, 100, 30}), B = at({100, 35, 100, 70}); // centres 150,15 and 150,70
    const SNeighbour FROM = {{0, 20, 100, 60}, {50, 50}};
    CHECK(neighbour(FROM, {A, B}, {1, 0}) == 1);

    // Mid-animation, the centre it is headed for decides the order.
    const SNeighbour MOVING = {{100, 0, 100, 100}, {-50, 50}};
    CHECK(neighbour(TL, {MOVING, TR}, {1, 0}) == 1);
}

static void testQuadrant() {
    const SPoint SCREEN{1000, 500};
    CHECK(quadrant({10, 10, 100, 100}, SCREEN) == (SCorner{true, true}));
    CHECK(quadrant({800, 10, 100, 100}, SCREEN) == (SCorner{false, true}));
    CHECK(quadrant({10, 400, 100, 50}, SCREEN) == (SCorner{true, false}));
    CHECK(quadrant({800, 400, 100, 50}, SCREEN) == (SCorner{false, false}));
    CHECK(quadrant({0, 0, 1000, 500}, SCREEN) == (SCorner{true, true})); // equals: top left
}

// --- The pointer ---

enum : uint32_t {
    LEFT   = 272,
    RIGHT  = 273,
    MIDDLE = 274,
    SIDE   = 275,
};

// The overview, as a log of what the pointer asks of it.
struct SFake : IPointerTargets {
    std::string log;
    bool        withSubmap = false, windowThere = true, resizable = true;

    bool submap() override {
        return withSubmap;
    }
    void submapClick(uint32_t b) override {
        log += "bind" + std::to_string(b) + " ";
    }
    void click() override {
        log += "click ";
    }
    bool beginDrag(const SPoint& at) override {
        log += "pick(" + std::to_string(int(at.x)) + ") ";
        return windowThere;
    }
    void drag() override {
        log += "drag ";
    }
    void drop() override {
        log += "drop ";
    }
    void abandonDrag() override {
        log += "abandon ";
    }
    bool armResize(const SPoint&) override {
        log += "arm ";
        return resizable;
    }
    void disarmResize() override {
        log += "disarm ";
    }
    bool beginResize() override {
        log += "grab ";
        return true;
    }
    void resize() override {
        log += "resize ";
    }
    void endResize() override {
        log += "release ";
    }
};

static SPointerConfig CONFIG = {.main = LEFT, .resize = RIGHT, .middle = MIDDLE, .threshold = 5, .clickSlop = 10};

static void testClick() {
    SFake    f;
    CPointer p(f);
    p.config = CONFIG;
    p.press(LEFT, {0, 0});
    CHECK(p.busy());
    p.move({3, 0}); // within the threshold
    p.release(LEFT, {3, 0});
    CHECK(f.log == "click ");
    CHECK(!p.busy());

    // Other buttons do nothing.
    f.log.clear();
    p.press(SIDE, {0, 0});
    p.release(SIDE, {0, 0});
    p.press(MIDDLE, {0, 0});
    p.release(MIDDLE, {0, 0});
    CHECK(f.log.empty());
}

static void testDrag() {
    SFake    f;
    CPointer p(f);
    p.config = CONFIG;
    p.press(LEFT, {100, 0});
    p.move({104, 0});
    CHECK(f.log.empty());
    // Past the threshold: the window where it went down.
    p.move({120, 0});
    CHECK(f.log == "pick(100) drag ");
    CHECK(p.dragging());
    p.move({130, 0});
    p.release(LEFT, {130, 0});
    CHECK(f.log == "pick(100) drag drag drop ");
    CHECK(!p.dragging() && !p.busy());

    // Back near the start: a click.
    f.log.clear();
    p.press(LEFT, {100, 0});
    p.move({120, 0});
    p.move({105, 0});
    p.release(LEFT, {105, 0});
    CHECK(f.log == "pick(100) drag drag abandon click ");

    // Nothing to pick: it keeps trying, and letting go clicks.
    f.log.clear();
    f.windowThere = false;
    p.press(LEFT, {100, 0});
    p.move({120, 0});
    p.move({140, 0});
    p.release(LEFT, {140, 0});
    CHECK(f.log == "pick(100) pick(100) click ");
}

static void testMiddleDrags() {
    SFake    f;
    CPointer p(f);
    p.config             = CONFIG;
    p.config.middleDrags = true;
    // The middle button picks up at once ...
    p.press(MIDDLE, {100, 0});
    CHECK(f.log == "pick(100) ");
    p.move({101, 0});
    p.release(MIDDLE, {150, 0});
    CHECK(f.log == "pick(100) drag drop ");
    // ... and the main one only clicks.
    f.log.clear();
    p.press(LEFT, {0, 0});
    p.move({50, 0});
    p.release(LEFT, {50, 0});
    CHECK(f.log == "click ");
}

static void testResizeButton() {
    SFake    f;
    CPointer p(f);
    p.config = CONFIG;
    p.press(RIGHT, {0, 0});
    CHECK(f.log == "arm ");
    p.move({2, 0});
    p.move({20, 0});
    p.move({30, 0});
    p.release(RIGHT, {30, 0});
    CHECK(f.log == "arm grab resize resize release ");
    CHECK(!p.busy());

    // Let go before moving: nothing but disarming.
    f.log.clear();
    p.press(RIGHT, {0, 0});
    p.release(RIGHT, {0, 0});
    CHECK(f.log == "arm disarm ");

    // Nothing resizable there: moving does nothing.
    f.log.clear();
    f.resizable = false;
    p.press(RIGHT, {0, 0});
    CHECK(!p.busy());
    p.move({50, 0});
    p.release(RIGHT, {50, 0});
    CHECK(f.log == "arm disarm ");
}

static void testSubmap() {
    SFake    f;
    CPointer p(f);
    p.config     = CONFIG;
    f.withSubmap = true;
    // Clicks run the submap's binds.
    p.press(LEFT, {0, 0});
    p.release(LEFT, {0, 0});
    p.press(MIDDLE, {0, 0});
    p.release(MIDDLE, {0, 0});
    p.press(RIGHT, {0, 0});
    p.release(RIGHT, {0, 0});
    CHECK(f.log == "bind272 bind274 arm disarm bind273 ");

    // Dragging still drags, and is no click.
    f.log.clear();
    p.press(LEFT, {100, 0});
    p.move({120, 0});
    p.release(LEFT, {150, 0});
    CHECK(f.log == "pick(100) drag drop ");
    // Back near the start: moving away already dropped the click.
    f.log.clear();
    p.press(LEFT, {100, 0});
    p.move({120, 0});
    p.release(LEFT, {101, 0});
    CHECK(f.log == "pick(100) drag abandon ");

    // A resize is no click either.
    f.log.clear();
    p.press(RIGHT, {0, 0});
    p.move({20, 0});
    p.release(RIGHT, {20, 0});
    CHECK(f.log == "arm grab resize release ");

    // Middle dragging: the main button moved away is neither click nor drag.
    f.log.clear();
    p.config.middleDrags = true;
    p.press(LEFT, {0, 0});
    p.move({50, 0});
    p.release(LEFT, {50, 0});
    CHECK(f.log.empty());
    p.press(MIDDLE, {0, 0});
    p.move({50, 0});
    p.release(MIDDLE, {60, 0});
    CHECK(f.log == "pick(0) drag drop ");
}

static void testCancel() {
    SFake    f;
    CPointer p(f);
    p.config = CONFIG;
    p.press(LEFT, {100, 0});
    p.move({120, 0});
    // Escape mid-drag, the button still down: nothing more until it goes up.
    p.cancelled(true);
    CHECK(p.swallowing() && !p.dragging());
    f.log.clear();
    p.move({150, 0});
    p.press(RIGHT, {150, 0});
    p.release(LEFT, {150, 0});
    CHECK(f.log.empty());
    CHECK(!p.swallowing());
    p.press(LEFT, {0, 0});
    p.release(LEFT, {0, 0});
    CHECK(f.log == "click ");

    // With no button down (a window closed after), nothing to wait for.
    f.log.clear();
    p.cancelled(false);
    CHECK(!p.swallowing());
    // Nor without a drag.
    p.press(LEFT, {0, 0});
    p.cancelled(true);
    CHECK(!p.swallowing() && !p.busy());
}

static void testAdopt() {
    SFake    f;
    CPointer p(f);
    p.config = CONFIG;
    // Hyprland's own drag, carried on: any button going up drops it.
    p.adopt();
    CHECK(p.dragging());
    p.move({10, 0});
    p.release(SIDE, {10, 0});
    CHECK(f.log == "drag drop ");
    CHECK(!p.dragging());
    // Cancelled first: nothing to drop.
    f.log.clear();
    p.adopt();
    p.cancelled(true);
    p.release(LEFT, {0, 0});
    CHECK(f.log.empty() && !p.swallowing());
}

int main() {
    testContains();
    testWindowAt();
    testNearestCentre();
    testDrop();
    testHold();
    testResize();
    testNeighbour();
    testQuadrant();
    testClick();
    testDrag();
    testMiddleDrags();
    testResizeButton();
    testSubmap();
    testCancel();
    testAdopt();
    if (g_failures)
        std::printf("%d failure(s)\n", g_failures);
    else
        std::printf("interaction: ok\n");
    return g_failures ? 1 : 0;
}
