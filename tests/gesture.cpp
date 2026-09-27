// Tests for Gesture.hpp: `make test`.

#include "Gesture.hpp"

#include <cstdio>
#include <string>

using namespace Gesture;

static int g_failures = 0;

#define CHECK(cond)                                                                                                                                                                \
    do {                                                                                                                                                                           \
        if (!(cond)) {                                                                                                                                                             \
            std::printf("%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);                                                                                               \
            ++g_failures;                                                                                                                                                          \
        }                                                                                                                                                                          \
    } while (0)

static bool near(double a, double b) {
    return std::abs(a - b) < 1e-9;
}

constexpr uint32_t SUPER  = 64;
constexpr uint32_t SHIFT  = 1;
constexpr uint32_t LEFT   = 0x110;
constexpr uint32_t MIDDLE = 0x112;

// Records what the engine asks of the targets.
struct SFake : ITargets {
    bool        canBegin = true;
    int         begins = 0, ends = 0;
    bool        lastCancelled = false;
    bool        hold          = false;
    int         cursorCalls   = 0;
    SPoint      moved, zoom;
    bool        inOverview = false;
    SPoint      ppc = {1000, 500};
    const SRule* endedIn = nullptr;
    SSettle     endedPosition;
    STouched    touched;

    bool begin(uint32_t) override {
        if (canBegin)
            ++begins;
        return canBegin;
    }
    SPoint pixelsPerCell() override {
        return ppc;
    }
    void move(eTarget target, const SPoint& d, uint32_t) override {
        auto& to = target == eTarget::POSITION ? moved : zoom;
        to       = {to.x + d.x, to.y + d.y};
    }
    void end(const SRule& rule, bool cancelled, const STouched& t, uint32_t) override {
        ++ends;
        lastCancelled = cancelled;
        endedPosition = rule.position;
        touched       = t;
    }
    void cursor(bool h) override {
        hold = h;
        ++cursorCalls;
    }
    bool zoomed() override {
        return inOverview;
    }
};

static SRule pan(std::optional<uint32_t> whenMods, std::optional<uint32_t> startMods) {
    SRule r;
    r.when.button = MIDDLE;
    r.when.mods   = whenMods;
    r.start.mods  = startMods;
    r.drive       = {{eInput::MOTION, eTarget::POSITION}};
    return r;
}

static void testStartNeedsStartKeys() {
    SFake   t;
    CEngine e(t);
    e.add(pan(std::nullopt, SUPER));

    CHECK(!e.button(MIDDLE, true, 0, 0)); // no SUPER: the click goes to the app
    CHECK(!e.active());
    CHECK(!e.button(MIDDLE, false, 0, 10)); // and so does its release

    CHECK(e.button(MIDDLE, true, SUPER, 20)); // SUPER + middle: taken
    CHECK(e.active() && t.begins == 1);
    e.mods(0, 30); // SUPER was only needed to start
    CHECK(e.active());
    CHECK(e.button(MIDDLE, false, 0, 40)); // the release of a taken press is taken
    CHECK(!e.active() && t.ends == 1 && !t.lastCancelled);
}

static void testWhenKeysMustStayHeld() {
    SFake   t;
    CEngine e(t);
    e.add(pan(SUPER, std::nullopt));

    CHECK(e.button(MIDDLE, true, SUPER, 0));
    e.mods(0, 10); // SUPER is part of `when`: letting go of it ends the session
    CHECK(!e.active() && t.ends == 1);
    CHECK(e.button(MIDDLE, false, 0, 20)); // the press was taken, so is the release
}

static void testMostKeysWinThenFirstDeclared() {
    SFake   t;
    CEngine e(t);
    SRule   a = pan(std::nullopt, std::nullopt);
    a.position = {eSettle::STAY};
    SRule b    = pan(SHIFT, std::nullopt);
    b.position = {eSettle::FLICK};
    SRule c    = pan(std::nullopt, std::nullopt); // same keys as a, declared later: never wins
    c.idleMs   = 999;
    e.add(a);
    e.add(b);
    e.add(c);

    e.button(MIDDLE, true, 0, 0);
    CHECK(e.active() && e.active()->position.kind == eSettle::STAY);
    e.mods(SHIFT, 10); // b has more keys
    CHECK(e.active() && e.active()->position.kind == eSettle::FLICK);
    e.mods(0, 20); // back to a: same session
    CHECK(e.active() && e.active()->position.kind == eSettle::STAY && t.begins == 1);
    e.mods(SHIFT, 30);
    e.button(MIDDLE, false, SHIFT, 40); // ends in b: b decides how things settle
    CHECK(t.ends == 1 && t.endedPosition.kind == eSettle::FLICK);
}

static void testPathIndependence() {
    // Whatever the order keys come in, the same held state gives the same rule.
    SFake   t;
    CEngine e(t);
    SRule   one = pan(std::nullopt, std::nullopt);
    SRule   two = pan(std::nullopt, std::nullopt);
    two.when.button.reset();
    two.when.mods  = SHIFT;
    two.start.mods.reset();
    two.drive      = {{eInput::MOTION_X, eTarget::POSITION}};
    SRule both     = pan(SHIFT, std::nullopt);
    both.drive     = {{eInput::MOTION_Y, eTarget::POSITION}};
    e.add(one);
    e.add(two);
    e.add(both);

    e.button(MIDDLE, true, 0, 0);
    e.mods(SHIFT, 10);
    const auto* VIA_BUTTON = e.active();
    e.button(MIDDLE, false, SHIFT, 20);
    e.mods(0, 30);

    e.mods(SHIFT, 40);
    e.button(MIDDLE, true, SHIFT, 50);
    CHECK(e.active() == VIA_BUTTON && e.active()->drive[0].input == eInput::MOTION_Y);
}

static void testMotion() {
    SFake   t;
    CEngine e(t);
    SRule   follow = pan(std::nullopt, std::nullopt);
    e.add(follow);

    CHECK(!e.motion({10, 10}, 0, 0)); // no session: motion goes on
    e.button(MIDDLE, true, 0, 0);
    CHECK(e.motion({100, -50}, 0, 10));
    // Follow the content: the view moves against the drag, a monitor per cell.
    CHECK(near(t.moved.x, -0.1) && near(t.moved.y, 0.1));

    SFake   u;
    CEngine f(u);
    SRule   fast       = pan(std::nullopt, std::nullopt);
    fast.drive[0].gain = 400;
    fast.drive[0].invert = true;
    fast.drive.push_back({eInput::MOTION_X, eTarget::POSITION, 200});
    f.add(fast);
    f.button(MIDDLE, true, 0, 0);
    f.motion({100, 40}, 0, 10);
    // invert: +100/400 on x, +40/400 on y; and the x-only drive: -100/200.
    CHECK(near(u.moved.x, 0.25 - 0.5) && near(u.moved.y, 0.1));
}

static void testBeginCanFail() {
    SFake t;
    t.canBegin = false;
    CEngine e(t);
    e.add(pan(std::nullopt, std::nullopt));
    CHECK(!e.button(MIDDLE, true, 0, 0)); // the view can't be held: the click goes on
    CHECK(!e.active());
    CHECK(!e.button(MIDDLE, false, 0, 10));
}

static void testOtherButtonsDuringASession() {
    SFake   t;
    CEngine e(t);
    e.add(pan(std::nullopt, std::nullopt));
    e.button(MIDDLE, true, 0, 0);
    CHECK(e.button(LEFT, true, 0, 10)); // the session owns the pointer's buttons
    CHECK(e.active());
    e.button(MIDDLE, false, 0, 20); // ends: no rule matches with left alone
    CHECK(!e.active());
    CHECK(e.button(LEFT, false, 0, 30)); // left's press was taken
}

static void testModifierOnlyRuleGoesIdle() {
    SFake   t;
    CEngine e(t);
    SRule   r;
    r.when.mods = SUPER;
    r.drive     = {{eInput::MOTION, eTarget::POSITION}};
    r.idleMs    = 100;
    e.add(r);

    e.mods(SUPER, 0);
    CHECK(!e.active()); // held modifiers alone don't start it...
    CHECK(e.motion({10, 0}, SUPER, 10)); // ...an input it drives does
    CHECK(e.active() && e.deadline() == 110u);
    e.motion({10, 0}, SUPER, 60);
    e.tick(150);
    CHECK(e.active()); // kept alive by the last motion
    e.tick(160);
    CHECK(!e.active() && t.ends == 1 && !t.lastCancelled);
}

static void testCursor() {
    SFake   t;
    CEngine e(t);
    SRule   held    = pan(std::nullopt, std::nullopt);
    held.holdCursor = true;
    SRule free      = pan(SHIFT, std::nullopt);
    e.add(held);
    e.add(free);

    e.button(MIDDLE, true, 0, 0);
    CHECK(t.hold && t.cursorCalls == 1);
    e.mods(SHIFT, 10);
    CHECK(!t.hold && t.cursorCalls == 2);
    e.mods(0, 20);
    CHECK(t.hold && t.cursorCalls == 3);
    e.button(MIDDLE, false, 0, 30);
    CHECK(!t.hold && t.cursorCalls == 4);
}

// Applying an input can bring another in: a workspace switch makes Hyprland
// simulate pointer motion, before the first input is done.
struct SReentrant : SFake {
    CEngine* engine   = nullptr;
    int      nested   = 0;
    bool     release  = false; // the nested input releases the button
    void     move(eTarget target, const SPoint& d, uint32_t t) override {
        SFake::move(target, d, t);
        if (nested++ < 50) {
            CHECK(!engine->motion({100, 0}, 0, t)); // dropped: it moved nothing
            if (release)
                engine->button(MIDDLE, false, 0, t);
        }
    }
};

static void testReentrantInput() {
    SReentrant t;
    CEngine    e(t);
    t.engine = &e;
    e.add(pan(std::nullopt, std::nullopt));
    e.button(MIDDLE, true, 0, 0);
    e.motion({100, 0}, 0, 10);
    CHECK(t.nested == 1 && near(t.moved.x, -0.1)); // applied once

    // A release nested in an input still ends the session, once the outer
    // input is done.
    SReentrant u;
    CEngine    f(u);
    u.engine  = &f;
    u.release = true;
    f.add(pan(std::nullopt, std::nullopt));
    f.button(MIDDLE, true, 0, 0);
    f.motion({100, 0}, 0, 10);
    CHECK(!f.active() && u.ends == 1);
}

static void testWheelDrivesZoom() {
    SFake   t;
    CEngine e(t);
    SRule   zoom;
    zoom.when.mods = SUPER;
    zoom.drive     = {{eInput::WHEEL, eTarget::ZOOM}};
    e.add(zoom);

    CHECK(!e.wheel(1, 0, 0)); // no SUPER: the app scrolls
    CHECK(e.wheel(1, SUPER, 10)); // down: out
    CHECK(e.wheel(0.5, SUPER, 20)); // a high-resolution half notch
    CHECK(near(t.zoom.x, 0.375) && near(t.moved.x, 0) && near(t.moved.y, 0));
    CHECK(!e.motion({50, 0}, SUPER, 30)); // the rule drives the wheel only
    e.tick(20 + zoom.idleMs);
    CHECK(!e.active() && t.ends == 1 && t.touched.zoom && !t.touched.position);
}

static void testTouchedTargets() {
    // Pan while holding the button, scroll to zoom: both settle; pan only:
    // the zoom is left alone.
    SFake   t;
    CEngine e(t);
    SRule   r = pan(std::nullopt, std::nullopt);
    r.drive.push_back({eInput::WHEEL, eTarget::ZOOM});
    e.add(r);

    e.button(MIDDLE, true, 0, 0);
    e.motion({10, 0}, 0, 10);
    e.button(MIDDLE, false, 0, 20);
    CHECK(t.touched.position && !t.touched.zoom);

    e.button(MIDDLE, true, 0, 30);
    e.motion({10, 0}, 0, 40);
    CHECK(e.wheel(-1, 0, 50)); // up: in
    e.button(MIDDLE, false, 0, 60);
    CHECK(t.touched.position && t.touched.zoom && near(t.zoom.x, -0.25));

    // Motion driving the zoom: fingers up zoom out, ZOOM_GAIN pixels for all of it.
    SFake   u;
    CEngine f(u);
    SRule   z = pan(std::nullopt, std::nullopt);
    z.drive   = {{eInput::MOTION_Y, eTarget::ZOOM}};
    f.add(z);
    f.button(MIDDLE, true, 0, 0);
    f.motion({40, -ZOOM_GAIN / 2}, 0, 10);
    CHECK(near(u.zoom.x, 0.5) && near(u.moved.x, 0));
}

static void testCancelAndClear() {
    SFake   t;
    CEngine e(t);
    e.add(pan(std::nullopt, std::nullopt));
    e.button(MIDDLE, true, 0, 0);
    e.cancel(10);
    CHECK(!e.active() && t.ends == 1 && t.lastCancelled);
    CHECK(!e.motion({10, 0}, 0, 15)); // still held: no new session until let go
    CHECK(!e.active() && t.begins == 1);
    CHECK(e.button(MIDDLE, false, 0, 20)); // still its press

    e.button(MIDDLE, true, 0, 30);
    e.clear(40);
    CHECK(!e.active() && t.ends == 2 && t.lastCancelled);
    e.button(MIDDLE, false, 0, 50);
    CHECK(!e.button(MIDDLE, true, 0, 60)); // no rules left
}

// The overview's wheel: plain wheel, only zoomed out; SUPER + wheel still zooms.
static void testZoomedKey() {
    SFake   t;
    CEngine e(t);
    SRule   zoom;
    zoom.when.mods = SUPER;
    zoom.drive     = {{eInput::WHEEL, eTarget::ZOOM}};
    SRule scroll;
    scroll.when.zoomed = true;
    scroll.when.mods   = 0;
    scroll.drive       = {{eInput::WHEEL, eTarget::POSITION, 0, false, 0.1}, {eInput::WHEEL_X, eTarget::POSITION, 0, false, 0.1}};
    e.add(zoom);
    e.add(scroll);

    CHECK(!e.wheel(1, 0, 0)); // zoomed in: the app scrolls
    t.inOverview = true;
    CHECK(e.wheel(1, 0, 10)); // zoomed out: the view moves down
    CHECK(near(t.moved.y, 0.1) && near(t.moved.x, 0) && near(t.zoom.x, 0));
    CHECK(e.wheel(-2, 0, 20, eInput::WHEEL_X)); // and left
    CHECK(near(t.moved.x, -0.2) && near(t.moved.y, 0.1));
    e.tick(20 + scroll.idleMs);
    CHECK(!e.active() && t.ends == 1 && t.touched.position && !t.touched.zoom);

    // SUPER held: its rule, not the overview's.
    CHECK(e.wheel(1, SUPER, 500));
    CHECK(near(t.zoom.x, 0.25) && near(t.moved.y, 0.1));
    e.tick(500 + zoom.idleMs);

    // Zoomed back in mid-session: the rule stops holding at the next input.
    CHECK(e.wheel(1, 0, 1000));
    t.inOverview = false;
    CHECK(!e.wheel(1, 0, 1010));
    CHECK(!e.active() && t.ends == 3);
    CHECK(!e.wheel(1, 0, 1020, eInput::WHEEL_X));
}

int main() {
    testStartNeedsStartKeys();
    testWhenKeysMustStayHeld();
    testMostKeysWinThenFirstDeclared();
    testPathIndependence();
    testMotion();
    testBeginCanFail();
    testOtherButtonsDuringASession();
    testModifierOnlyRuleGoesIdle();
    testCursor();
    testReentrantInput();
    testWheelDrivesZoom();
    testZoomedKey();
    testTouchedTargets();
    testCancelAndClear();
    if (g_failures)
        std::printf("%d check(s) failed\n", g_failures);
    else
        std::printf("all gesture checks passed\n");
    return g_failures ? 1 : 0;
}
