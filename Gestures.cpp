// The gesture engine (Gesture.hpp) on Hyprland: mouse buttons, pointer motion,
// the wheel, touchpad swipes and modifiers from the event bus go into the engine, and what it
// drives goes to the view (View.hpp): its position, held on its first move,
// and its zoom. Registers hl.plugin.hyprgrid.rule(), whose
// fields are in docs/design.md.
//
// A session belongs to the monitor under the cursor when it starts. Input it
// takes is cancelled on the bus, before Hyprland's binds and the apps see it.
// With a rule holding the cursor, the cursor is hidden (cursor:invisible, for
// the session) and warped back to where it was after every move, so motion
// never stops at a screen edge.
//
// A swipe whose begin a rule takes is taken whole (updates, end): neither
// Hyprland's gestures (hl.gesture) nor the app see it.
//
// Hyprland rebuilds its Lua state on every config reload, so the rules (and
// the Lua references of their hooks) are dropped on config.preReload and the
// config declares them again.

#include "Gestures.hpp"
#include "Gesture.hpp"
#include "Grid.hpp"
#include "View.hpp"
#include "Later.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/lua/ConfigManager.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/keybinds/Resolver.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <linux/input-event-codes.h>

#include <chrono>
#include <stdexcept>
#include <string>

using Gesture::eInput;
using Gesture::eSettle;
using Gesture::eTarget;
using Motion::SPoint;

namespace {
    // The input events' clock (CLOCK_MONOTONIC), for events that carry no time.
    uint32_t nowMs() {
        return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    // Held modifiers, without the lock ones.
    uint32_t modsNow() {
        const auto IGNORED = Input::HL_MODIFIER_CAPS | Input::HL_MODIFIER_MOD2;
        return sc<uint8_t>(g_pInputManager->getModsFromAllKBs()) & ~sc<uint8_t>(IGNORED);
    }

    PHLMONITOR monitorAt(const Vector2D& pos) {
        for (const auto& mon : State::monitorState()->monitors()) {
            if (mon && mon->logicalBox().containsPoint(pos))
                return mon;
        }
        return nullptr;
    }

    void enter(int onEnter, const PHLMONITOR& monitor, int64_t id, const Motion::SCell& step);

    // The session's end of the engine: the view on its monitor, and the cursor.
    class CTargets : public Gesture::ITargets {
      public:
        bool begin(uint32_t) override {
            const auto MON = monitorAt(Pointer::mgr()->position());
            if (!MON || !Grid::cellOf(MON->m_activeWorkspace) || View::held(MON))
                return false;
            m_monitor = MON;
            return true;
        }

        SPoint pixelsPerCell() override {
            const auto MON = m_monitor.lock();
            return MON ? View::pitch(MON) : SPoint{};
        }

        void move(eTarget target, const SPoint& delta, uint32_t timeMs) override;

        void end(const Gesture::SRule& rule, bool cancelled, const Gesture::STouched& touched, uint32_t timeMs) override {
            const auto MON = m_monitor.lock();
            m_monitor.reset();
            if (!MON)
                return;
            if (touched.zoom) {
                if (const auto R = View::releaseZoom(MON, timeMs))
                    View::zoomTo(MON, zoomTarget(rule.zoom, cancelled, *R), R->velocity * 1000.F);
            }
            if (touched.position) {
                const auto LAND    = cancelled || rule.position.kind == eSettle::STAY ? View::eLand::STAY :
                       rule.position.kind == eSettle::DIRECTION                     ? View::eLand::AHEAD :
                                                                                      View::eLand::FLICK;
                const auto RELEASE = View::release(MON, timeMs, LAND);
                if (RELEASE)
                    enter(rule.onEnter, MON, RELEASE->id, RELEASE->step);
            }
        }

        // Where a released zoom settles: 0 (the normal view) or 1 (the
        // overview), or the rule's value.
        static float zoomTarget(const Gesture::SSettle& settle, bool cancelled, const View::SZoomRelease& r) {
            if (cancelled || settle.kind == eSettle::STAY)
                return r.start;
            switch (settle.kind) {
                case eSettle::FLICK: return Motion::project(r.value, r.velocity) >= 0.5 ? 1.F : 0.F;
                case eSettle::DIRECTION: return r.last > 0 ? 1.F : r.last < 0 ? 0.F : std::round(r.value);
                case eSettle::VALUE: return std::clamp(float(settle.value), 0.F, 1.F);
                default: return std::round(r.value); // KEEP
            }
        }

        // Hidden through cursor:invisible, so Hyprland's own hiding hides it
        // and gives the app under it its cursor back after (setting the cursor
        // image directly desyncs the renderer, which then draws the cursor
        // under the windows, and its ticker would unhide it anyway).
        void cursor(bool hold) override {
            static auto PINVISIBLE = CConfigValue<Config::INTEGER>("cursor:invisible");
            if (hold == m_holding)
                return;
            if (hold) {
                m_anchor        = Pointer::mgr()->position();
                m_wasInvisible  = *PINVISIBLE;
                *PINVISIBLE.ptr() = 1;
            } else
                *PINVISIBLE.ptr() = m_wasInvisible;
            m_holding = hold;
            g_pHyprRenderer->ensureCursorRenderingMode();
        }

        bool zoomed() override {
            auto MON = m_monitor.lock();
            if (!MON)
                MON = monitorAt(Pointer::mgr()->position());
            return MON && View::zoom(MON) > 0;
        }

        bool holding() const {
            return m_holding;
        }

        Vector2D anchor() const {
            return m_anchor;
        }

        PHLMONITOR monitor() const {
            return m_monitor.lock();
        }

      private:
        PHLMONITORREF m_monitor;
        bool          m_holding = false;
        Vector2D      m_anchor;
        Config::INTEGER m_wasInvisible = 0;
    };

    CTargets          g_targets;
    Gesture::CEngine  g_engine{g_targets};
    Vector2D          g_lastPos;
    bool              g_warping = false;
    SP<CEventLoopTimer> g_idle;

    CHyprSignalListener g_buttonListener, g_moveListener, g_axisListener, g_keyListener, g_reloadListener;
    CHyprSignalListener g_swipeBeginListener, g_swipeUpdateListener, g_swipeEndListener;

    void CTargets::move(eTarget target, const SPoint& delta, uint32_t timeMs) {
        const auto MON = m_monitor.lock();
        if (!MON)
            return;
        if (target == eTarget::ZOOM) {
            View::moveZoom(MON, float(delta.x), timeMs);
            return;
        }
        if (!View::held(MON) && !View::holdHere(MON))
            return;
        const auto MOVE = View::moveHeld(MON, delta, timeMs);
        if (MOVE && MOVE->land)
            enter(g_engine.active() ? g_engine.active()->onEnter : -1, MON, *MOVE->land, MOVE->step);
    }

    // Run tick() when a session on modifiers only would go idle.
    void armIdle() {
        if (!g_idle)
            return;
        const auto DEADLINE = g_engine.deadline();
        if (!DEADLINE) {
            g_idle->updateTimeout(std::nullopt);
            return;
        }
        const int32_t LEFT = int32_t(*DEADLINE - nowMs());
        g_idle->updateTimeout(std::chrono::milliseconds(std::max(0, LEFT)));
    }

    int luaCancel(lua_State*) {
        // Not from inside the hook that asked: the engine is mid-event.
        Later::run([] {
            g_engine.cancel(nowMs());
            armIdle();
        });
        return 0;
    }

    // The view reached workspace `id` (a step of `step`): the rule's on_enter
    // switches to it, or hyprgrid does.
    void enter(int onEnter, const PHLMONITOR& monitor, int64_t id, const Motion::SCell& step) {
        if (onEnter >= 0) {
            if (const auto MGR = Config::Lua::mgr()) {
                MGR->callLuaFn(
                    onEnter,
                    [&](lua_State* L) {
                        lua_newtable(L);
                        lua_pushstring(L, monitor->m_name.c_str());
                        lua_setfield(L, -2, "monitor");
                        lua_pushcfunction(L, luaCancel);
                        lua_setfield(L, -2, "cancel");
                        lua_pushinteger(L, id);
                        lua_pushinteger(L, step.x);
                        lua_pushinteger(L, step.y);
                        return 4;
                    },
                    Config::Lua::CConfigManager::LUA_TIMEOUT_EVENT_CALLBACK_MS, "hyprgrid on_enter");
            }
            return;
        }
        for (const auto& ws : State::Workspace::state()->workspacesCopy()) {
            if (ws && ws->numberedID() && sc<int64_t>(*ws->numberedID()) == id) {
                (void)Config::Actions::changeWorkspaceOnCurrentMonitor(ws);
                return;
            }
        }
        (void)Config::Actions::changeWorkspace(std::to_string(id));
    }

    void onButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
        if (info.cancelled)
            return;
        if (g_engine.button(e.button, e.state == WL_POINTER_BUTTON_STATE_PRESSED, modsNow(), e.timeMs))
            info.cancelled = true;
        g_lastPos = Pointer::mgr()->position();
        armIdle();
    }

    void fingersLifted(uint32_t timeMs);
    bool fingersChanging();

    void onMove(Event::SCallbackInfo& info) {
        if (g_warping)
            return;
        // One finger moving the pointer: the swipe's are gone.
        if (fingersChanging())
            fingersLifted(nowMs());
        const auto POS   = Pointer::mgr()->position();
        const auto FROM  = g_targets.holding() ? g_targets.anchor() : g_lastPos;
        const auto DELTA = POS - FROM;
        // Settle the reference before the engine runs: what it drives can
        // switch workspaces, and a switch simulates pointer motion that comes
        // back here (Gesture.hpp drops it; it must also measure nothing).
        g_lastPos = POS;
        if (g_targets.holding() && POS != g_targets.anchor()) {
            g_warping = true;
            Pointer::mgr()->warpTo(g_targets.anchor());
            g_warping = false;
            g_lastPos = g_targets.anchor();
        }
        if (g_engine.motion({DELTA.x, DELTA.y}, modsNow(), nowMs()))
            info.cancelled = true;
        armIdle();
    }

    // The wheel, in notches (120 to one; high-resolution wheels send
    // fractions); other sources (a touchpad scrolling) by 15 px.
    void onAxis(const IPointer::SAxisEvent& e, Event::SCallbackInfo& info) {
        if (info.cancelled)
            return;
        const double NOTCHES = e.source == WL_POINTER_AXIS_SOURCE_WHEEL ? e.deltaDiscrete / 120.0 : e.delta / 15.0;
        if (NOTCHES == 0)
            return;
        const auto INPUT = e.axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL ? eInput::WHEEL_X : eInput::WHEEL;
        if (g_engine.wheel(NOTCHES, modsNow(), e.timeMs, INPUT))
            info.cancelled = true;
        armIdle();
    }

    // --- The touchpad ---
    //
    // libinput ends a swipe normally when every finger is lifted. When the
    // finger count changes, it waits 100 ms, ends the swipe cancelled, and
    // begins another with the new count once the fingers move. So a
    // cancelled end holds the session: the next begin carries it on with its
    // count (4 fingers zooming, 3 panning), and without one within
    // FINGER_CHANGE_MS the fingers count as lifted then.
    constexpr uint32_t FINGER_CHANGE_MS = 300;

    bool                g_swipeTaken = false; // a rule took this swipe's begin
    SP<CEventLoopTimer> g_fingerChange;       // armed by a cancelled end
    uint32_t            g_fingerChangeMs = 0; // ...at its time
    bool                g_changing       = false; // between a cancelled end and the next begin

    bool fingersChanging() {
        return g_changing;
    }

    void fingersLifted(uint32_t timeMs) {
        g_changing = false;
        if (g_fingerChange)
            g_fingerChange->updateTimeout(std::nullopt);
        g_engine.fingers(0, modsNow(), timeMs);
        armIdle();
    }

    void onSwipeBegin(const IPointer::SSwipeBeginEvent& e, Event::SCallbackInfo& info) {
        if (info.cancelled)
            return;
        g_changing = false;
        if (g_fingerChange)
            g_fingerChange->updateTimeout(std::nullopt);
        g_swipeTaken = g_engine.fingers(int(e.fingers), modsNow(), e.timeMs);
        if (g_swipeTaken)
            info.cancelled = true;
        armIdle();
    }

    void onSwipeUpdate(const IPointer::SSwipeUpdateEvent& e, Event::SCallbackInfo& info) {
        if (!g_swipeTaken || info.cancelled)
            return;
        g_engine.motion({e.delta.x, e.delta.y}, modsNow(), e.timeMs, true);
        info.cancelled = true; // even once the session is over: the swipe was taken
        armIdle();
    }

    void onSwipeEnd(const IPointer::SSwipeEndEvent& e, Event::SCallbackInfo& info) {
        if (!g_swipeTaken || info.cancelled)
            return;
        g_swipeTaken   = false;
        info.cancelled = true;
        if (!e.cancelled || !g_engine.onFingers() || !g_fingerChange) {
            fingersLifted(e.timeMs);
            return;
        }
        g_changing       = true;
        g_fingerChangeMs = e.timeMs;
        g_fingerChange->updateTimeout(std::chrono::milliseconds(FINGER_CHANGE_MS));
    }

    // --- hl.plugin.hyprgrid.rule() ---

    // Each parser returns an error message, empty when fine; the rule table is
    // at index 1.

    std::string parseButton(lua_State* L, int idx, uint32_t& out) {
        if (lua_isinteger(L, idx)) {
            out = uint32_t(lua_tointeger(L, idx));
            return {};
        }
        const std::string NAME = lua_isstring(L, idx) ? lua_tostring(L, idx) : "";
        static const std::pair<const char*, uint32_t> BUTTONS[] = {
            {"left", BTN_LEFT}, {"right", BTN_RIGHT}, {"middle", BTN_MIDDLE}, {"side", BTN_SIDE}, {"extra", BTN_EXTRA}, {"forward", BTN_FORWARD}, {"back", BTN_BACK},
        };
        for (const auto& [name, code] : BUTTONS) {
            if (NAME == name) {
                out = code;
                return {};
            }
        }
        return "button: expected left, right, middle, side, extra, forward, back or a button code";
    }

    std::string parseMods(const std::string& text, uint32_t& out) {
        out = 0;
        size_t from = 0;
        while (from <= text.size()) {
            const size_t PLUS  = text.find('+', from);
            auto         token = text.substr(from, PLUS == std::string::npos ? std::string::npos : PLUS - from);
            token.erase(0, token.find_first_not_of(" \t"));
            token.erase(token.find_last_not_of(" \t") + 1);
            if (!token.empty()) {
                const auto MOD = Keybinds::modifierFromString(token);
                if (!MOD)
                    return "mod: unknown modifier '" + token + "'";
                out |= sc<uint8_t>(*MOD);
            }
            if (PLUS == std::string::npos)
                break;
            from = PLUS + 1;
        }
        return {};
    }

    std::string parseKeys(lua_State* L, const char* field, Gesture::SKeys& keys) {
        lua_getfield(L, 1, field);
        std::string err;
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "mod");
            if (lua_isstring(L, -1)) {
                uint32_t mods = 0;
                err           = parseMods(lua_tostring(L, -1), mods);
                keys.mods     = mods;
            }
            lua_pop(L, 1);
            lua_getfield(L, -1, "button");
            if (err.empty() && !lua_isnil(L, -1)) {
                uint32_t button = 0;
                err             = parseButton(L, -1, button);
                keys.button     = button;
            }
            lua_pop(L, 1);
            lua_getfield(L, -1, "fingers");
            if (err.empty() && lua_isinteger(L, -1) && lua_tointeger(L, -1) >= 3 && lua_tointeger(L, -1) <= 5)
                keys.fingers = int(lua_tointeger(L, -1));
            else if (err.empty() && !lua_isnil(L, -1))
                err = "fingers: expected 3, 4 or 5 (fewer are scrolling)";
            lua_pop(L, 1);
            lua_getfield(L, -1, "zoomed");
            if (err.empty() && lua_isboolean(L, -1))
                keys.zoomed = bool(lua_toboolean(L, -1));
            else if (err.empty() && !lua_isnil(L, -1))
                err = "zoomed: expected true or false";
            lua_pop(L, 1);
        } else if (!lua_isnil(L, -1))
            err = "expected a table";
        lua_pop(L, 1);
        return err.empty() ? err : std::string{field} + "." + err;
    }

    std::string parseDrive(lua_State* L, Gesture::SRule& rule) {
        lua_getfield(L, 1, "drive");
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            return "drive: required, e.g. { motion = \"position\" }";
        }
        std::string err;
        lua_pushnil(L);
        while (err.empty() && lua_next(L, -2)) {
            // key at -2, value at -1
            const std::string INPUT = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : "";
            Gesture::SDrive   d;
            if (INPUT == "motion")
                d.input = eInput::MOTION;
            else if (INPUT == "motion_x")
                d.input = eInput::MOTION_X;
            else if (INPUT == "motion_y")
                d.input = eInput::MOTION_Y;
            else if (INPUT == "wheel")
                d.input = eInput::WHEEL;
            else if (INPUT == "wheel_x")
                d.input = eInput::WHEEL_X;
            else
                err = "drive: unknown input '" + INPUT + "' (motion, motion_x, motion_y, wheel, wheel_x)";

            std::string target;
            if (err.empty() && lua_isstring(L, -1))
                target = lua_tostring(L, -1);
            else if (err.empty() && lua_istable(L, -1)) {
                lua_getfield(L, -1, "target");
                target = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
                lua_pop(L, 1);
                lua_getfield(L, -1, "gain");
                if (lua_isnumber(L, -1))
                    d.gain = lua_tonumber(L, -1);
                else if (!lua_isnil(L, -1) && !(lua_isstring(L, -1) && std::string{lua_tostring(L, -1)} == "follow"))
                    err = "drive." + INPUT + ".gain: expected a number (pixels per cell) or \"follow\"";
                lua_pop(L, 1);
                lua_getfield(L, -1, "invert");
                d.invert = lua_toboolean(L, -1);
                lua_pop(L, 1);
                lua_getfield(L, -1, "step");
                if (lua_isnumber(L, -1))
                    d.step = lua_tonumber(L, -1);
                else if (!lua_isnil(L, -1))
                    err = "drive." + INPUT + ".step: expected a number (zoom or cells per notch)";
                lua_pop(L, 1);
            }
            if (err.empty()) {
                if (target == "position")
                    d.target = eTarget::POSITION;
                else if (target == "zoom")
                    d.target = eTarget::ZOOM;
                else
                    err = "drive." + INPUT + ": unknown target '" + target + "' (position, zoom)";
            }
            if (err.empty())
                rule.drive.push_back(d);
            lua_pop(L, 1); // the value, keeping the key for lua_next
        }
        if (!err.empty())
            lua_pop(L, 1); // the key lua_next left
        lua_pop(L, 1);     // drive
        if (err.empty() && rule.drive.empty())
            err = "drive: required, e.g. { motion = \"position\" }";
        return err;
    }

    std::string parseSettle(lua_State* L, const char* target, Gesture::SSettle& out) {
        lua_getfield(L, -1, target);
        std::string err;
        if (lua_isnumber(L, -1))
            out = {eSettle::VALUE, lua_tonumber(L, -1)};
        else if (lua_isstring(L, -1)) {
            const std::string V = lua_tostring(L, -1);
            if (V == "flick")
                out = {eSettle::FLICK};
            else if (V == "direction")
                out = {eSettle::DIRECTION};
            else if (V == "stay")
                out = {eSettle::STAY};
            else if (V == "keep")
                out = {eSettle::KEEP};
            else
                err = std::string{"release."} + target + ": expected flick, direction, stay, keep or a number";
        }
        lua_pop(L, 1);
        if (err.empty() && std::string{target} == "position" && out.kind != eSettle::FLICK && out.kind != eSettle::DIRECTION && out.kind != eSettle::STAY)
            err = "release.position: flick, direction or stay";
        return err;
    }

    std::string parseRule(lua_State* L, Gesture::SRule& rule) {
        if (!lua_istable(L, 1))
            return "expected a table";
        auto err = parseKeys(L, "when", rule.when);
        if (err.empty())
            err = parseKeys(L, "start", rule.start);
        if (err.empty() && rule.keys() == 0)
            err = "when: required, e.g. { button = \"middle\" }";
        if (err.empty())
            err = parseDrive(L, rule);

        if (err.empty()) {
            lua_getfield(L, 1, "cursor");
            if (lua_isstring(L, -1)) {
                const std::string C = lua_tostring(L, -1);
                if (C == "hold")
                    rule.holdCursor = true;
                else if (C != "free")
                    err = "cursor: expected \"free\" or \"hold\"";
            }
            lua_pop(L, 1);
        }
        if (err.empty()) {
            lua_getfield(L, 1, "release");
            if (lua_istable(L, -1)) {
                err = parseSettle(L, "position", rule.position);
                if (err.empty())
                    err = parseSettle(L, "zoom", rule.zoom);
            }
            lua_pop(L, 1);
        }
        if (err.empty()) {
            lua_getfield(L, 1, "idle");
            if (lua_isinteger(L, -1))
                rule.idleMs = uint32_t(std::max<lua_Integer>(0, lua_tointeger(L, -1)));
            lua_pop(L, 1);
        }
        if (err.empty()) {
            lua_getfield(L, 1, "on_enter");
            if (lua_isfunction(L, -1))
                rule.onEnter = luaL_ref(L, LUA_REGISTRYINDEX); // pops it
            else {
                if (!lua_isnil(L, -1))
                    err = "on_enter: expected a function";
                lua_pop(L, 1);
            }
        }
        return err;
    }

    int luaRule(lua_State* L) {
        {
            Gesture::SRule rule;
            const auto     ERR = parseRule(L, rule);
            if (ERR.empty()) {
                g_engine.add(std::move(rule));
                return 0;
            }
            if (rule.onEnter >= 0)
                luaL_unref(L, LUA_REGISTRYINDEX, rule.onEnter);
            lua_pushstring(L, ("hyprgrid.rule: " + ERR).c_str());
        }
        // Raised with no C++ object alive: lua_error() longjmps.
        return lua_error(L);
    }
}

void Gestures::init(void* handle) {
    if (!HyprlandAPI::addLuaFunction(handle, "hyprgrid", "rule", luaRule))
        throw std::runtime_error("[hyprgrid] failed to register rule()");

    g_idle = makeShared<CEventLoopTimer>(
        std::nullopt,
        [](SP<CEventLoopTimer>, void*) {
            g_engine.tick(nowMs());
            armIdle();
        },
        nullptr);
    g_pEventLoopManager->addTimer(g_idle);
    g_fingerChange = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { fingersLifted(g_fingerChangeMs); }, nullptr);
    g_pEventLoopManager->addTimer(g_fingerChange);

    g_lastPos        = Pointer::mgr()->position();
    g_buttonListener = Event::bus()->m_events.input.mouse.button.listen([](IPointer::SButtonEvent e, Event::SCallbackInfo& info) { onButton(e, info); });
    g_moveListener   = Event::bus()->m_events.input.mouse.move.listen([](Vector2D, Event::SCallbackInfo& info) { onMove(info); });
    g_axisListener   = Event::bus()->m_events.input.mouse.axis.listen([](IPointer::SAxisEvent e, Event::SCallbackInfo& info) { onAxis(e, info); });
    // The modifier state changes after the key event: read it once it has.
    g_keyListener = Event::bus()->m_events.input.keyboard.key.listen([](IKeyboard::SKeyEvent, Event::SCallbackInfo&) {
        Later::run([] {
            g_engine.mods(modsNow(), nowMs());
            armIdle();
        });
    });
    g_swipeBeginListener  = Event::bus()->m_events.gesture.swipe.begin.listen([](IPointer::SSwipeBeginEvent e, Event::SCallbackInfo& info) { onSwipeBegin(e, info); });
    g_swipeUpdateListener = Event::bus()->m_events.gesture.swipe.update.listen([](IPointer::SSwipeUpdateEvent e, Event::SCallbackInfo& info) { onSwipeUpdate(e, info); });
    g_swipeEndListener    = Event::bus()->m_events.gesture.swipe.end.listen([](IPointer::SSwipeEndEvent e, Event::SCallbackInfo& info) { onSwipeEnd(e, info); });
    g_reloadListener = Event::bus()->m_events.config.preReload.listen([] {
        g_engine.clear(nowMs());
        armIdle();
    });
}

void Gestures::exit() {
    g_buttonListener.reset();
    g_moveListener.reset();
    g_axisListener.reset();
    g_keyListener.reset();
    g_reloadListener.reset();
    g_swipeBeginListener.reset();
    g_swipeUpdateListener.reset();
    g_swipeEndListener.reset();
    g_engine.clear(nowMs());
    for (auto* timer : {&g_idle, &g_fingerChange}) {
        if (*timer) {
            g_pEventLoopManager->removeTimer(*timer);
            timer->reset();
        }
    }
}
