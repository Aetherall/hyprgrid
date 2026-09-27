// Tests for overview/Scene.hpp: `make test`.

#include "overview/Scene.hpp"

#include <cmath>
#include <cstdio>

using namespace Scene;

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

static bool inside(const SBox& inner, const SBox& outer) {
    return inner.x >= outer.x - 1e-6 && inner.y >= outer.y - 1e-6 && inner.x + inner.w <= outer.x + outer.w + 1e-6 && inner.y + inner.h <= outer.y + outer.h + 1e-6;
}

// This machine: DP-2 on the left, DP-3 on the right; and a scaled one.
static const SMonitor DP2    = {{0, 0}, {1920, 1080}, 1};
static const SMonitor DP3    = {{1920, 0}, {3440, 1440}, 1};
static const SMonitor HIDPI  = {{0, 0}, {1280, 800}, 1.5};
static const SMonitor MONS[] = {DP2, DP3, HIDPI};

static SCamera camera(const SMonitor& m, double zoomScale, double gap, SCell origin, SPoint position) {
    SCamera c{.monitor = m, .zoomScale = zoomScale, .gap = gap, .origin = origin};
    c.viewOffset = viewOffsetAt(m, zoomScale, gap, position, origin);
    return c;
}

static SBox screen(const SMonitor& m) {
    return {0, 0, m.size.x * m.scale, m.size.y * m.scale};
}

static void testZoomScale() {
    CHECK(near(zoomScale(0, 0.5), 1));
    CHECK(near(zoomScale(1, 0.5), 0.5));
    CHECK(near(zoomScale(0.5, 0.5), 0.75));
    CHECK(near(zoomScale(-1, 0.5), 1));
    CHECK(near(zoomScale(2, 0.5), 0.5));
}

// Zoomed in, the view on a cell shows that cell as the monitor shows it:
// opening or closing the overview doesn't jump.
static void testZoomedInIsTheScreen() {
    for (const auto& m : MONS) {
        for (const SCell c : {SCell{0, 0}, SCell{3, -2}}) {
            const auto CAM = camera(m, 1, 40, {1, 1}, {double(c.x), double(c.y)});
            CHECK(near(cellBox(CAM, c), screen(m)));
            // A window there lands where Hyprland draws it.
            const SBox WINDOW = {m.position.x + 10, m.position.y + 20, 300, 200};
            CHECK(near(toScreen(CAM, intoCell(m, m, WINDOW, true), cellOffset(CAM, c)), SBox{10 * m.scale, 20 * m.scale, 300 * m.scale, 200 * m.scale}));
        }
    }
}

// The pitch, however it is asked for, is the distance between cell boxes.
static void testPitch() {
    for (const auto& m : MONS) {
        for (const double S : {1.0, 0.5, 0.3}) {
            const auto CAM = camera(m, S, 30, {0, 0}, {0.4, -1.2});
            const auto A = cellBox(CAM, SCell{0, 0}), B = cellBox(CAM, SCell{1, 1});
            const auto P = devicePitch(m, S, 30);
            CHECK(near(B.x - A.x, P.x) && near(B.y - A.y, P.y));
            CHECK(near(screenPitch(m, S, 30), SPoint{P.x / m.scale, P.y / m.scale}));
            CHECK(near(logicalPitch(m, S, 30), SPoint{P.x / (m.scale * S), P.y / (m.scale * S)}));
            // The gap on screen stays what it is set to, whatever the zoom.
            CHECK(near(B.x - (A.x + A.w), 30 * m.scale));
        }
    }
}

// Zoomed out, the view on a cell keeps that cell centred on screen.
static void testViewCentres() {
    for (const auto& m : MONS) {
        const auto CAM = camera(m, 0.5, 20, {0, 0}, {2, 1});
        const auto B   = cellBox(CAM, SCell{2, 1});
        CHECK(near(SPoint{B.x + B.w / 2, B.y + B.h / 2}, SPoint{m.size.x * m.scale / 2, m.size.y * m.scale / 2}));
        CHECK(near(B.w, m.size.x * m.scale * 0.5));
    }
}

static void testInverses() {
    for (const auto& m : MONS) {
        const auto   CAM = camera(m, 0.4, 25, {0, 0}, {1.3, -0.7});
        const SPoint OFFSET = cellOffset(CAM, SCell{-1, 2});
        for (const SPoint p : {SPoint{0, 0}, SPoint{123.5, 456.25}, SPoint{m.size.x, m.size.y}}) {
            const auto B = toScreen(CAM, {p.x, p.y, 0, 0}, OFFSET);
            CHECK(near(fromScreen(CAM, {B.x, B.y}, OFFSET), p));
        }
    }
    for (const auto& [on, source] : {std::pair{DP3, DP2}, std::pair{DP2, DP3}, std::pair{DP2, DP2}}) {
        const bool   SAME = &on == &source || (on.position.x == source.position.x && on.size.x == source.size.x);
        const SPoint P    = {source.position.x + 100, source.position.y + 50};
        const auto   L    = intoCell(on, source, {P.x, P.y, 0, 0}, SAME);
        CHECK(near(outOfCell(on, source, {L.x, L.y}, SAME), P));
    }
}

// Another monitor's workspace fits inside the cell at its own shape, centred.
static void testOtherMonitor() {
    CHECK(near(fit(DP3, DP2), 1440.0 / 1080));
    CHECK(near(pad(DP3, DP2), SPoint{(3440 - 1920 * 1440.0 / 1080) / 2, 0}));
    CHECK(near(intoCell(DP3, DP2, {0, 0, 1920, 1080}, false), SBox{440, 0, 2560, 1440}));
    CHECK(near(intoCell(DP2, DP3, {1920, 0, 3440, 1440}, false), SBox{0, (1080 - 1440 * 1920.0 / 3440) / 2, 1920, 1440 * 1920.0 / 3440}));

    for (const auto& [on, other] : {std::pair{DP3, DP2}, std::pair{DP2, DP3}}) {
        const auto CAM = camera(on, 0.5, 20, {0, 0}, {0, 0});
        // A window filling the other monitor stays inside the cell...
        const auto WIN = toScreen(CAM, intoCell(on, other, {other.position.x, other.position.y, other.size.x, other.size.y}, false), cellOffset(CAM, SCell{1, 0}));
        CHECK(inside(WIN, cellBox(CAM, SCell{1, 0})));
        // ...and is where the other monitor's frame is drawn.
        CHECK(near(WIN, frameBox(CAM, other, {1, 0}, false)));
    }
    const auto CAM = camera(DP3, 0.5, 20, {0, 0}, {0, 0});
    CHECK(near(frameBox(CAM, DP3, {0.5, 0}, true), cellBox(CAM, SPoint{0.5, 0})));
}

static void testSeam() {
    const auto CAM = camera(DP3, 0.5, 20, {0, 0}, {0, 0});
    const auto L   = seamLine(CAM, {true, 0}, 4);
    const auto A = cellBox(CAM, SCell{-1, 0}), B = cellBox(CAM, SCell{0, 0});
    CHECK(near(L.x + L.w / 2, (A.x + A.w + B.x) / 2));
    CHECK(near(L.w, 4) && near(L.y, 0) && near(L.h, 1440));
    const auto H = seamLine(CAM, {false, 2}, 4);
    CHECK(near(H.h, 4) && near(H.w, 3440) && near(H.y + 2, (cellBox(CAM, SCell{0, 1}).y + cellBox(CAM, SCell{0, 1}).h + cellBox(CAM, SCell{0, 2}).y) / 2));
}

// A workspace's drawing reaches as far as its windows do past the cell; for
// another monitor's workspace, as far as they do once fitted in.
static void testOverflow() {
    CHECK(overflow(DP2.size, {}) == SInsets{});
    CHECK(overflow(DP2.size, {{0, 0, 1920, 1080}}) == SInsets{});
    CHECK(overflow(DP2.size, {{-50, 10, 100, 100}, {1900, 1000, 100, 200}}) == (SInsets{50, 80, 0, 120}));
    CHECK(overflow(DP2.size, {{-500, -500, -2, -2}}) == SInsets{}); // hidden under a fullscreen window

    // DP-2's windows fill DP-2: fitted into a DP-3 cell they stay inside it,
    // though measured in DP-3's own space they would seem to spill past it.
    const SBox FULL = {0, 0, 1920, 1080};
    CHECK(overflow(DP3.size, {intoCell(DP3, DP2, FULL, false)}) == SInsets{});
    // 100 past DP-2's left edge: fitted in at x = 440 - 133, still inside the
    // cell (measured in DP-3's space it was 2020 past it).
    CHECK(overflow(DP3.size, {intoCell(DP3, DP2, {-100, 0, 400, 1080}, false)}) == SInsets{});
    // 400 past it reaches past the cell's padding: by 400 * 4/3 - 440.
    CHECK(near(overflow(DP3.size, {intoCell(DP3, DP2, {-400, 0, 800, 1080}, false)}).left, 400 * 1440.0 / 1080 - 440));

    const auto CAM = camera(HIDPI, 0.5, 0, {0, 0}, {0, 0});
    const auto B   = cellBox(CAM, SCell{0, 0});
    const auto G   = grow(CAM, B, {10, 20, 30, 40});
    const double U = 0.5 * 1.5;
    CHECK(near(G, SBox{B.x - 10 * U, B.y - 30 * U, B.w + 30 * U, B.h + 70 * U}));
    CHECK(near(expand(B, {}, 3), B));
}

// Zoomed in, only the view's cell shows; zoomed out, its neighbours too.
static void testOnScreen() {
    for (const auto& m : MONS) {
        const auto IN = camera(m, 1, 20, {0, 0}, {0, 0});
        CHECK(fullyOnScreen(m, cellBox(IN, SCell{0, 0})));
        CHECK(!onScreen(m, cellBox(IN, SCell{1, 0})) && !onScreen(m, cellBox(IN, SCell{0, -1})));
        const auto OUT = camera(m, 0.5, 20, {0, 0}, {0, 0});
        CHECK(onScreen(m, cellBox(OUT, SCell{1, 0})) && !fullyOnScreen(m, cellBox(OUT, SCell{1, 0})));
        CHECK(!onScreen(m, {10, 10, 0, 5}));
    }
}

// A workspace sliding over to its new cell passes through the cells between,
// at whatever zoom it is drawn.
static void testSlide() {
    CHECK(near(slide({0, 0}, {2, -1}, 0), SPoint{0, 0}));
    CHECK(near(slide({0, 0}, {2, -1}, 1), SPoint{2, -1}));
    CHECK(near(slide({0, 0}, {2, -1}, 0.5), SPoint{1, -0.5}));
    CHECK(near(slide({1, 1}, {3, 1}, 2), SPoint{3, 1}));
    CHECK(near(slide({1, 1}, {3, 1}, -1), SPoint{1, 1}));
    for (const double S : {1.0, 0.5}) {
        const auto CAM = camera(DP3, S, 20, {0, 0}, {0, 0});
        CHECK(near(cellBox(CAM, slide({0, 0}, {2, 0}, 0.5)), cellBox(CAM, SCell{1, 0})));
    }
}

static void testCells() {
    CHECK(outlineCells({}).empty());
    CHECK(outlineCells({{0, 0}}).size() == 8);
    const auto OUT = outlineCells({{0, 0}, {1, 0}});
    CHECK(OUT.size() == 10);
    CHECK(std::ranges::find(OUT, SCell{0, 0}) == OUT.end() && std::ranges::find(OUT, SCell{2, 1}) != OUT.end());

    CHECK(dropSlots({{0, 0}}, {}).size() == 4);
    // Shared neighbours once; not onto a workspace, or a cell another monitor's workspace takes.
    const auto S = dropSlots({{0, 0}, {1, 0}}, [](const SCell& c) { return c == SCell{0, 1}; });
    CHECK(S.size() == 5);
    CHECK(std::ranges::find(S, SCell{0, 1}) == S.end() && std::ranges::find(S, SCell{1, 0}) == S.end());
}

int main() {
    testZoomScale();
    testZoomedInIsTheScreen();
    testPitch();
    testViewCentres();
    testInverses();
    testOtherMonitor();
    testSeam();
    testOverflow();
    testOnScreen();
    testSlide();
    testCells();
    if (g_failures)
        std::printf("%d failure(s)\n", g_failures);
    else
        std::printf("scene: ok\n");
    return g_failures ? 1 : 0;
}
