// Gesture: rules deciding, from what is held (modifiers, buttons, fingers),
// what the moving inputs (motion, wheel) drive. Re-matched whenever the held
// state changes; the targets keep their value and speed across a change of
// rule, so the config describes states, not paths (docs/design.md). No
// Hyprland in here, so it is tested on its own (tests/gesture.cpp);
// Gestures.cpp feeds it Hyprland's input and applies it to the view.
//
// Motion is in pixels, targets in their own units (cells for position),
// times in ms on the input events' clock.

#pragma once

#include "Motion.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace Gesture {
    using Motion::SPoint;

    enum class eTarget : uint8_t {
        POSITION, // where the view is on the grid, in cells
        ZOOM,     // 0 normal, 1 overview
    };

    enum class eInput : uint8_t {
        MOTION,
        MOTION_X,
        MOTION_Y,
        WHEEL,   // the vertical wheel
        WHEEL_X, // the horizontal one (tilting it)
    };

    // What is held right now.
    struct SHeld {
        uint32_t              mods = 0;
        std::vector<uint32_t> buttons; // pressed, in press order
        int                   fingers = 0;
        bool                  zoomed  = false; // the view under the cursor is zoomed out

        bool pressed(uint32_t button) const {
            return std::ranges::find(buttons, button) != buttons.end();
        }
    };

    // Conditions on what is held; unset keys don't matter.
    struct SKeys {
        std::optional<uint32_t> mods;
        std::optional<uint32_t> button;
        std::optional<int>      fingers;
        std::optional<bool>     zoomed;

        int count() const {
            return int(mods.has_value()) + int(button.has_value()) + int(fingers.has_value()) + int(zoomed.has_value());
        }

        bool holds(const SHeld& held) const {
            return (!mods || held.mods == *mods) && (!button || held.pressed(*button)) && (!fingers || held.fingers == *fingers) && (!zoomed || held.zoomed == *zoomed);
        }
    };

    struct SDrive {
        eInput  input  = eInput::MOTION;
        eTarget target = eTarget::POSITION;
        double  gain   = 0;     // motion: pixels per target unit; 0: follow the content (position), ZOOM_GAIN (zoom)
        bool    invert = false; // motion: move the view the way the input moves; wheel: scrolling down zooms in
        double  step   = 0.25;  // wheel: target units per notch
    };

    // Motion driving the zoom without a gain: pixels for a full zoom.
    inline constexpr double ZOOM_GAIN = 300;

    // The targets a session moved: only those settle when it ends.
    struct STouched {
        bool position = false;
        bool zoom     = false;
    };

    enum class eSettle : uint8_t {
        FLICK,     // the stop nearest where the velocity would coast
        DIRECTION, // the next stop in the direction of the last movement
        STAY,      // back where the session started
        KEEP,      // where it is (zoom)
        VALUE,     // `value` (zoom)
    };

    struct SSettle {
        eSettle kind  = eSettle::FLICK;
        double  value = 0;
    };

    struct SRule {
        SKeys               when;  // held while the rule is active
        SKeys               start; // also held to enter it
        std::vector<SDrive> drive;
        bool                holdCursor = false;
        SSettle             position   = {eSettle::FLICK};
        SSettle             zoom       = {eSettle::DIRECTION};
        uint32_t            idleMs     = 150;
        int                 onEnter    = -1; // the adapter's hook (a Lua reference)

        int keys() const {
            return when.count() + start.count();
        }

        // Something is held besides modifiers: the rule lasts as long as it
        // is. A rule on modifiers only is entered by an input it drives and
        // ends idleMs after the last one.
        bool held() const {
            return when.button || when.fingers || start.button || start.fingers;
        }

        // A touchpad rule: its motion is the fingers'.
        bool onFingers() const {
            return when.fingers || start.fingers;
        }

        bool drives(eInput input) const {
            return std::ranges::any_of(drive, [&](const SDrive& d) { return d.input == input; });
        }

        bool drivesMotion() const {
            return drives(eInput::MOTION) || drives(eInput::MOTION_X) || drives(eInput::MOTION_Y);
        }
    };

    // What a session drives.
    class ITargets {
      public:
        virtual ~ITargets() = default;

        // A session starts: hold the targets. False: they can't be held now,
        // and there is no session.
        virtual bool begin(uint32_t timeMs) = 0;
        // Pixels per cell at the current zoom, for gain "follow".
        virtual SPoint pixelsPerCell() = 0;
        virtual void   move(eTarget target, const SPoint& delta, uint32_t timeMs) = 0;
        // The session ended in `rule`: settle the targets it touched as the
        // rule says.
        virtual void end(const SRule& rule, bool cancelled, const STouched& touched, uint32_t timeMs) = 0;
        // Whether the cursor is held in place, on every change.
        virtual void cursor(bool hold) = 0;
        // The view under the cursor (or the session's) is zoomed out: the
        // `zoomed` key, read whenever the rules are matched.
        virtual bool zoomed() {
            return false;
        }
    };

    class CEngine {
      public:
        explicit CEngine(ITargets& targets) : m_targets(targets) {}

        void add(SRule rule) {
            m_rules.push_back(std::move(rule));
        }

        // Drop every rule, ending a session in place ("stay").
        void clear(uint32_t timeMs) {
            cancel(timeMs);
            m_rules.clear();
        }

        // The rule in effect, while a session runs.
        const SRule* active() const {
            return m_active ? &m_rules[*m_active] : nullptr;
        }

        // Each input returns whether the session takes it (so it goes no
        // further).
        //
        // Input can arrive while the engine is still applying one: a
        // workspace switch or a hook makes Hyprland simulate pointer motion.
        // Motion arriving then is dropped (it moved nothing); held-state
        // changes are recorded and matched once the outer input is done.
        bool button(uint32_t button, bool pressed, uint32_t mods, uint32_t timeMs) {
            SBusy busy(*this, timeMs);
            m_held.mods = mods;
            if (pressed) {
                if (!m_held.pressed(button))
                    m_held.buttons.push_back(button);
                if (busy.nested())
                    return false;
                update(timeMs, std::nullopt);
                if (m_active)
                    m_taken.push_back(button);
                return m_active.has_value();
            }
            std::erase(m_held.buttons, button);
            const bool TAKEN = std::ranges::find(m_taken, button) != m_taken.end();
            std::erase(m_taken, button);
            if (!busy.nested())
                update(timeMs, std::nullopt);
            return TAKEN; // a release goes where its press went
        }

        void mods(uint32_t mods, uint32_t timeMs) {
            if (mods == m_held.mods)
                return;
            SBusy busy(*this, timeMs);
            m_held.mods = mods;
            if (!busy.nested())
                update(timeMs, std::nullopt);
        }

        // Fingers on the touchpad (0: none; fewer than 3 are scrolling, not a
        // swipe). Returns whether a session runs on a rule holding fingers:
        // it then takes the swipe.
        bool fingers(int count, uint32_t mods, uint32_t timeMs) {
            SBusy busy(*this, timeMs);
            m_held.mods    = mods;
            m_held.fingers = count;
            if (!busy.nested())
                update(timeMs, std::nullopt);
            return onFingers();
        }

        // A session runs on a rule holding fingers.
        bool onFingers() const {
            return m_active && active()->onFingers();
        }

        // Pointer motion, or with `fromFingers` the fingers' on the touchpad.
        // Each drives only its own kind of rule: finger motion rules holding
        // fingers, pointer motion the others. Finger motion enters no rule
        // (fingers() does); pointer motion can enter one on modifiers only.
        bool motion(const SPoint& deltaPx, uint32_t mods, uint32_t timeMs, bool fromFingers = false) {
            SBusy busy(*this, timeMs);
            if (busy.nested())
                return false;
            m_held.mods = mods;
            update(timeMs, fromFingers ? std::nullopt : std::optional{eInput::MOTION});
            if (!m_active || !active()->drivesMotion() || active()->onFingers() != fromFingers)
                return false;
            m_lastDriven = timeMs;
            for (const auto& d : active()->drive) {
                if (d.input != eInput::MOTION && d.input != eInput::MOTION_X && d.input != eInput::MOTION_Y)
                    continue;
                const bool   ZOOM = d.target == eTarget::ZOOM;
                const SPoint PER  = d.gain > 0 ? SPoint{d.gain, d.gain} : ZOOM ? SPoint{ZOOM_GAIN, ZOOM_GAIN} : m_targets.pixelsPerCell();
                const double SIGN = d.invert ? 1 : -1;
                const SPoint UNITS = {d.input == eInput::MOTION_Y || PER.x <= 0 ? 0 : SIGN * deltaPx.x / PER.x,
                                      d.input == eInput::MOTION_X || PER.y <= 0 ? 0 : SIGN * deltaPx.y / PER.y};
                if (ZOOM) // one axis: the one moved along (fingers up: out)
                    drive(d.target, {d.input == eInput::MOTION_X ? UNITS.x : UNITS.y, 0}, timeMs);
                else
                    drive(d.target, UNITS, timeMs);
            }
            return true;
        }

        // `notches` of the wheel (> 0: down, or right for WHEEL_X; fractions
        // from high-resolution wheels).
        bool wheel(double notches, uint32_t mods, uint32_t timeMs, eInput input = eInput::WHEEL) {
            SBusy busy(*this, timeMs);
            if (busy.nested())
                return false;
            m_held.mods = mods;
            update(timeMs, input);
            if (!m_active || !active()->drives(input))
                return false;
            m_lastDriven = timeMs;
            for (const auto& d : active()->drive) {
                if (d.input != input)
                    continue;
                // Scrolling down zooms out; for the position, moves down (or right).
                const double UNITS = (d.invert ? -1 : 1) * notches * d.step;
                drive(d.target, d.target == eTarget::ZOOM || input == eInput::WHEEL_X ? SPoint{UNITS, 0} : SPoint{0, UNITS}, timeMs);
            }
            return true;
        }

        // When tick() should run next: a session on modifiers only going idle.
        std::optional<uint32_t> deadline() const {
            if (!m_active || active()->held())
                return std::nullopt;
            return m_lastDriven + active()->idleMs;
        }

        void tick(uint32_t nowMs) {
            SBusy busy(*this, nowMs);
            if (const auto D = deadline(); D && nowMs >= *D)
                finish(nowMs, false);
        }

        // End now, everything back where it started.
        void cancel(uint32_t timeMs) {
            SBusy busy(*this, timeMs);
            if (busy.nested())
                m_cancelPending = true;
            else if (m_active)
                finish(timeMs, true);
        }

      private:
        void drive(eTarget target, const SPoint& delta, uint32_t timeMs) {
            (target == eTarget::ZOOM ? m_touched.zoom : m_touched.position) = true;
            m_targets.move(target, delta, timeMs);
        }

        // Marks the engine busy for the outermost input; held-state changes
        // recorded by nested ones are matched when it is done.
        class SBusy {
          public:
            SBusy(CEngine& engine, uint32_t timeMs) : m_engine(engine), m_nested(engine.m_busy), m_timeMs(timeMs) {
                if (m_nested)
                    m_engine.m_heldChanged = true;
                m_engine.m_busy = true;
            }
            ~SBusy() {
                if (m_nested)
                    return;
                while (m_engine.m_heldChanged || m_engine.m_cancelPending) {
                    if (std::exchange(m_engine.m_cancelPending, false) && m_engine.m_active)
                        m_engine.finish(m_timeMs, true);
                    m_engine.m_heldChanged = false;
                    m_engine.update(m_timeMs, std::nullopt);
                }
                m_engine.m_busy = false;
            }
            bool nested() const {
                return m_nested;
            }

          private:
            CEngine& m_engine;
            bool     m_nested;
            uint32_t m_timeMs;
        };

        // The rule that should be in effect now: the current one while its
        // `when` holds, or one that can be entered; the most keys win, then
        // the first declared.
        std::optional<size_t> match(std::optional<eInput> driven) const {
            std::optional<size_t> best;
            for (size_t i = 0; i < m_rules.size(); ++i) {
                const auto& R        = m_rules[i];
                const bool  CURRENT  = m_active == i;
                const bool  INPUT    = R.held() || CURRENT || (driven && (driven == eInput::MOTION ? R.drivesMotion() : R.drives(*driven)));
                const bool  CANENTER = R.when.holds(m_held) && (CURRENT || R.start.holds(m_held)) && INPUT;
                if (CANENTER && (!best || R.keys() > m_rules[*best].keys()))
                    best = i;
            }
            return best;
        }

        void update(uint32_t timeMs, std::optional<eInput> driven) {
            m_held.zoomed   = m_targets.zoomed();
            const auto NEXT = match(driven);
            if (m_cancelled) {
                // No new session until what was held when it was cancelled is let go.
                if (!NEXT)
                    m_cancelled = false;
                return;
            }
            if (NEXT == m_active)
                return;
            if (!NEXT) {
                finish(timeMs, false);
                return;
            }
            if (!m_active) {
                if (!m_targets.begin(timeMs))
                    return;
                m_lastDriven = timeMs;
                m_touched    = {};
            }
            const bool HOLD = m_rules[*NEXT].holdCursor;
            const bool WAS  = m_active && active()->holdCursor;
            m_active        = NEXT;
            if (HOLD != WAS)
                m_targets.cursor(HOLD);
        }

        void finish(uint32_t timeMs, bool cancelled) {
            const auto RULE = *active();
            m_active.reset();
            m_cancelled = cancelled;
            if (RULE.holdCursor)
                m_targets.cursor(false);
            m_targets.end(RULE, cancelled, m_touched, timeMs);
        }

        ITargets&             m_targets;
        std::vector<SRule>    m_rules;
        SHeld                 m_held;
        std::vector<uint32_t> m_taken; // buttons whose press a session took
        STouched              m_touched;
        std::optional<size_t> m_active;
        uint32_t              m_lastDriven  = 0;
        bool                  m_busy        = false;
        bool                  m_heldChanged   = false; // by an input nested in another
        bool                  m_cancelPending = false; // asked for while busy
        bool                  m_cancelled     = false; // until the held state stops matching
    };
}
