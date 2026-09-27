// The pointer and the keys while zoomed out (see Session.hpp). The buttons go
// through Interaction::CPointer, on the screen under the pointer, or the one
// a button went down on until it goes up. Input over a layer (a bar) goes to
// it, unless a button is busy with a window. The gesture engine (Gestures.cpp)
// sees everything first; what it takes never reaches here.

#include "Internal.hpp"
#include "Session.hpp"
#include "Later.hpp"
#include "View.hpp"

namespace Overview {
    static xkb_keysym_t keysymOf(const IKeyboard::SKeyEvent& event) {
        const auto KEYBOARD = g_pSeatManager->m_keyboard.lock();
        if (!KEYBOARD)
            return XKB_KEY_NoSymbol;
        xkb_state* const STATE = KEYBOARD->m_resolveBindsBySym && KEYBOARD->m_xkbSymState ? KEYBOARD->m_xkbSymState : KEYBOARD->m_xkbState;
        return STATE ? xkb_state_key_get_one_sym(STATE, event.keycode + 8) : XKB_KEY_NoSymbol;
    }

    // A layer (or its popup) on `monitor`, above the windows, has the keyboard.
    static bool layerHasKeyboard(const PHLMONITOR& monitor) {
        const auto FOCUSED   = g_pSeatManager->m_state.keyboardFocus.lock();
        const auto HLSURFACE = FOCUSED ? Desktop::View::CWLSurface::fromResource(FOCUSED) : nullptr;
        const auto VIEW      = HLSURFACE ? HLSURFACE->view() : nullptr;
        if (!VIEW)
            return false;

        auto layer = Desktop::View::CLayerSurface::fromView(VIEW);
        if (!layer) {
            if (const auto POPUP = Desktop::View::CPopup::fromView(VIEW); POPUP && POPUP->getT1Owner())
                layer = Desktop::View::CLayerSurface::fromView(POPUP->getT1Owner()->view());
        }
        return layer && layer->m_monitor == monitor && layer->m_layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP;
    }

    static bool pointerOnLayer(const PHLMONITOR& monitor) {
        if (!monitor)
            return false;
        const auto POS = g_pInputManager->getMouseCoordsInternal();
        Vector2D   surfaceCoords;
        PHLLS      layer;
        for (const auto LEVEL : {ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, ZWLR_LAYER_SHELL_V1_LAYER_TOP}) {
            if (Desktop::viewState()->hitTest().layerSurfaceAt(POS, &monitor->m_layerSurfaceLayers[LEVEL], &surfaceCoords, &layer))
                return true;
        }
        return false;
    }

    SP<CScreen> COverview::grab() const {
        return m_grab.lock();
    }

    bool COverview::swallowingPointer() const {
        return m_pointer.swallowing();
    }

    // Buttons passed to a layer while the submap is on get their release too.
    void COverview::releaseForwardedButtons(uint32_t timeMs) {
        if (m_forwardedButtons.empty())
            return;
        for (const auto BUTTON : m_forwardedButtons)
            g_pSeatManager->sendPointerButton(timeMs, BUTTON, WL_POINTER_BUTTON_STATE_RELEASED);
        m_forwardedButtons.clear();
        g_pSeatManager->sendPointerFrame();
    }

    Interaction::SPointerConfig COverview::pointerConfig(const CScreen& screen) const {
        const bool  LEFTHANDED = Overview::Config::getLeftHanded();
        const auto  MONITOR    = screen.monitor.lock();
        const float SCALE      = MONITOR ? MONITOR->m_scale : 1.F;
        return {
            .main        = LEFTHANDED ? BTN_RIGHT : BTN_LEFT,
            .resize      = LEFTHANDED ? BTN_LEFT : BTN_RIGHT,
            .middle      = BTN_MIDDLE,
            .middleDrags = Overview::Config::getDragMode() == 1,
            .threshold   = Overview::Config::getDragThreshold() * SCALE,
            .clickSlop   = 10.0 * SCALE,
        };
    }

    void COverview::pointerMoved(Event::SCallbackInfo& info) {
        // Every screen follows the pointer, for a drag crossing them.
        for (const auto& screen : m_screens)
            screen->updatePointer();

        const auto SCREEN = m_grab.lock() ? m_grab.lock() : screenAt(g_pInputManager->getMouseCoordsInternal());
        if (!SCREEN || SCREEN->isClosing())
            return;

        if (m_pointer.swallowing()) {
            info.cancelled = true;
            return;
        }
        if (!m_pointer.busy() && pointerOnLayer(SCREEN->monitor.lock())) {
            m_pointer.forgetClick();
            return;
        }

        info.cancelled = true;
        SCREEN->requestInputFrame();
        m_pointerScreen  = SCREEN;
        m_pointer.config = pointerConfig(*SCREEN);
        m_pointer.move(scenePoint(SCREEN->pointerLocal));
    }

    void COverview::pointerButton(const IPointer::SButtonEvent& event, Event::SCallbackInfo& info) {
        if (info.cancelled)
            return;

        const bool RELEASED   = event.state == WL_POINTER_BUTTON_STATE_RELEASED;
        const bool FORWARDED  = RELEASED && m_forwardedButtons.contains(event.button);
        const auto GRAB       = m_grab.lock();
        const auto SCREEN     = GRAB ? GRAB : screenAt(g_pInputManager->getMouseCoordsInternal());
        const bool ADOPTED    = m_pointer.adopted() && GRAB && RELEASED;
        const bool SWALLOWING = m_pointer.swallowing();
        // A cancelled drag's button going up on a closing screen: its end.
        const bool CANCELLEDRELEASE = SCREEN && SCREEN->isClosing() && SWALLOWING && GRAB && RELEASED;

        if (FORWARDED && !SCREEN) {
            info.cancelled = true;
            m_forwardedButtons.erase(event.button);
            g_pSeatManager->sendPointerButton(event.timeMs, event.button, event.state);
            g_pSeatManager->sendPointerFrame();
            return;
        }
        if (!SCREEN || (SCREEN->isClosing() && !CANCELLEDRELEASE))
            return;

        if (SWALLOWING && !RELEASED) {
            info.cancelled = true;
            return;
        }

        auto releaseGrab = Hyprutils::Utils::CScopeGuard([this, RELEASED, SWALLOWING, SCREEN] {
            if (!RELEASED || m_grab.lock() != SCREEN)
                return;
            // The screen stayed for this release: it can go now.
            const auto MONITOR = SCREEN->monitor.lock();
            if (SWALLOWING && SCREEN->isClosing() && SCREEN->removalPending() && View::zoom(MONITOR) <= 0 && !View::moving(MONITOR))
                Later::run([screen = WP<CScreen>{SCREEN}] {
                    CScreen* gone = nullptr;
                    if (const auto S = screen.lock(); S && overview() && S->isClosing() && S->removalPending() && !overview()->swallowingPointer())
                        gone = S.get();
                    if (gone)
                        overview()->remove(gone);
                });
            m_grab.reset();
        });

        SCREEN->updatePointer();
        m_pointerScreen  = SCREEN;
        m_pointer.config = pointerConfig(*SCREEN);

        if (ADOPTED) {
            m_pointer.release(event.button, scenePoint(SCREEN->pointerLocal));
            return;
        }
        if (SWALLOWING) {
            info.cancelled = true;
            m_pointer.release(event.button, scenePoint(SCREEN->pointerLocal));
            return;
        }

        const bool ONLAYER = !m_pointer.busy() && pointerOnLayer(SCREEN->monitor.lock());

        // With the submap, a layer's clicks are passed to it by hand.
        if (FORWARDED || (!RELEASED && ONLAYER && m_usesSubmap && submapActive())) {
            m_pointer.forgetClick();
            info.cancelled = true;
            if (RELEASED)
                m_forwardedButtons.erase(event.button);
            else
                m_forwardedButtons.emplace(event.button);
            g_pSeatManager->sendPointerButton(event.timeMs, event.button, event.state);
            g_pSeatManager->sendPointerFrame();
            return;
        }
        if (ONLAYER) {
            m_pointer.forgetClick();
            return;
        }

        if (!RELEASED)
            m_grab = SCREEN;

        info.cancelled                              = true;
        ::Config::Actions::state()->m_lastMouseCode = event.button;
        ::Config::Actions::state()->m_lastCode      = 0;
        ::Config::Actions::state()->m_timeLastMs    = event.timeMs;
        // Buttons taken here never reach Hyprland's input manager: without
        // releasing them, it thinks they are still down, which locks focus.
        releaseForwardedButtons(event.timeMs);
        g_pInputManager->releaseAllMouseButtons();

        if (RELEASED)
            m_pointer.release(event.button, scenePoint(SCREEN->pointerLocal));
        else
            m_pointer.press(event.button, scenePoint(SCREEN->pointerLocal));
    }

    // A tap: the window under it, and close.
    void COverview::touchDown(Event::SCallbackInfo& info) {
        const auto SCREEN = screenAt(g_pInputManager->getMouseCoordsInternal());
        if (!SCREEN || SCREEN->isClosing() || pointerOnLayer(SCREEN->monitor.lock()))
            return;
        info.cancelled = true;
        SCREEN->selectWindowAtPointer(true);
        closeAll();
    }

    void COverview::touchMoved(Event::SCallbackInfo& info) {
        const auto SCREEN = screenAt(g_pInputManager->getMouseCoordsInternal());
        if (!SCREEN || SCREEN->isClosing())
            return;
        info.cancelled = true;
        SCREEN->updatePointer();
        SCREEN->requestInputFrame();
    }

    // Escape drops a drag where it started. Without the submap, the arrows
    // move the selection and Enter closes on it.
    void COverview::keyPressed(const IKeyboard::SKeyEvent& event, Event::SCallbackInfo& info) {
        if (event.state != WL_KEYBOARD_KEY_STATE_PRESSED)
            return;

        const auto KEYSYM = keysymOf(event);
        if (KEYSYM == XKB_KEY_Escape && dragging()) {
            info.cancelled = true;
            cancelDrag();
            return;
        }
        if (m_usesSubmap)
            return;

        const auto SCREEN = activeScreen();
        if (!SCREEN || SCREEN->isClosing() || layerHasKeyboard(SCREEN->monitor.lock()))
            return;

        // Modified, these are someone else's binds.
        const auto IGNORED = Input::HL_MODIFIER_CAPS | Input::HL_MODIFIER_MOD2;
        const auto MODS    = sc<Input::ModifierMask>(sc<uint8_t>(g_pInputManager->getModsFromAllKBs()) & ~sc<uint8_t>(IGNORED));
        if (MODS != Input::HL_MODIFIER_NONE)
            return;

        switch (KEYSYM) {
            case XKB_KEY_Left:
            case XKB_KEY_KP_Left: SCREEN->moveSelection("left"); break;
            case XKB_KEY_Right:
            case XKB_KEY_KP_Right: SCREEN->moveSelection("right"); break;
            case XKB_KEY_Up:
            case XKB_KEY_KP_Up: SCREEN->moveSelection("up"); break;
            case XKB_KEY_Down:
            case XKB_KEY_KP_Down: SCREEN->moveSelection("down"); break;
            case XKB_KEY_Return:
            case XKB_KEY_KP_Enter: closeAll(); break;
            default: return;
        }
        info.cancelled = true;
    }

    // With the submap, a click runs its bind for that button (with the mods
    // held), as Hyprland would.
    bool COverview::dispatchSubmapClick(uint32_t button) {
        if (!m_usesSubmap || !submapActive() || !Keybinds::mgr() || !g_pInputManager)
            return false;

        const auto KEYNAME = "mouse:" + std::to_string(button);
        const auto MODS    = g_pInputManager->getModsFromAllKBs();
        const auto BINDS   = Keybinds::mgr()->registry().binds();
        const auto BIND    = std::ranges::find_if(BINDS, [&](const auto& bind) {
            return bind && bind->enabled() && bind->metadata().submap == SUBMAP && std::ranges::contains(bind->keyNames(), KEYNAME) &&
                (bind->modifierMask() == MODS || bind->hasFlag(Keybinds::BIND_FLAG_IGNORE_MODS));
        });
        if (BIND == BINDS.end())
            return false;

        const auto INDEX = Hyprutils::String::strToNumber<int>((*BIND)->metadata().argument);
        const auto MGR   = ::Config::Lua::mgr();
        if (!INDEX || !MGR)
            return false;

        auto&          state         = *::Config::Actions::state();
        const int      PASSPRESSED   = state.m_passPressed;
        const int      DEPTH         = state.m_bindInvocationDepth;
        const uint32_t LASTCODE      = state.m_lastCode;
        const uint32_t LASTMOUSECODE = state.m_lastMouseCode;
        state.m_passPressed          = 0;
        state.m_bindInvocationDepth++;
        state.m_lastCode      = 0;
        state.m_lastMouseCode = button;
        auto restore          = Hyprutils::Utils::CScopeGuard([&state, PASSPRESSED, DEPTH, LASTCODE, LASTMOUSECODE] {
            state.m_passPressed         = PASSPRESSED;
            state.m_bindInvocationDepth = DEPTH;
            state.m_lastCode            = LASTCODE;
            state.m_lastMouseCode       = LASTMOUSECODE;
        });
        return MGR->callLuaFnBind(*INDEX).success;
    }

    // --- What the buttons do (Interaction::CPointer) ---

    bool COverview::SPointerTargets::submap() {
        return overview.m_usesSubmap && submapActive();
    }

    void COverview::SPointerTargets::submapClick(uint32_t button) {
        overview.dispatchSubmapClick(button);
    }

    void COverview::SPointerTargets::click() {
        if (const auto SCREEN = overview.m_pointerScreen.lock()) {
            SCREEN->selectHoveredWorkspace();
            SCREEN->selectWindowAtPointer(true);
        }
        overview.closeAll();
    }

    bool COverview::SPointerTargets::beginDrag(const Interaction::SPoint& at) {
        const auto SCREEN = overview.m_pointerScreen.lock();
        return SCREEN && overview.beginDrag(*SCREEN, toVector(at));
    }

    void COverview::SPointerTargets::drag() {
        overview.updateDrag();
    }

    void COverview::SPointerTargets::drop() {
        overview.endDrag();
    }

    void COverview::SPointerTargets::abandonDrag() {
        overview.finishDrag();
    }

    bool COverview::SPointerTargets::armResize(const Interaction::SPoint& at) {
        const auto SCREEN = overview.m_pointerScreen.lock();
        return SCREEN && overview.armResize(*SCREEN, toVector(at));
    }

    void COverview::SPointerTargets::disarmResize() {
        overview.resize.pending.reset();
    }

    bool COverview::SPointerTargets::beginResize() {
        return overview.beginResize();
    }

    void COverview::SPointerTargets::resize() {
        overview.updateResize();
    }

    void COverview::SPointerTargets::endResize() {
        overview.endResize();
    }
}
