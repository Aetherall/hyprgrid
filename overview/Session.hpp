// The overview: one for all monitors, with a screen (Screen.hpp) on each
// monitor zoomed out. It holds what screens share: the changes to Hyprland's
// state while any screen is open, the pointer and the keys, and a window
// being dragged or resized, which can cross from one screen to another.
//
// Session.cpp: opening and closing screens, and those changes.
// Input.cpp: the pointer and the keys. Drag.cpp: dragging, dropping and
// resizing windows.
#pragma once

#include "Screen.hpp"

#include <unordered_set>

namespace Overview {
    class COverview {
      public:
        COverview();
        ~COverview();

        // --- Screens ---

        const std::vector<SP<CScreen>>& screens() const;
        SP<CScreen>                     screenFor(const PHLMONITOR& monitor) const;
        SP<CScreen>                     screenAt(const Vector2D& globalPoint) const;
        // The focused monitor's, or any.
        SP<CScreen>                     activeScreen() const;
        // Open a screen on `monitor` (`zoom`: zoom it out too); `created`
        // says whether it wasn't open already.
        SP<CScreen>                     open(const PHLMONITOR& monitor, bool zoom, bool* created = nullptr);
        void                            remove(CScreen* screen);
        void                            closeAll();
        // Screens opened for a drag that received its window close with the
        // one it came from: true if such a group was closed.
        bool                            closeLinked();

        // A screen starts closing: whatever of the session it holds lets go.
        void screenClosing(CScreen* screen);
        // One reopened: the submap comes back.
        void screenReopened();

        // --- The pointer (Input.cpp) ---

        void pointerMoved(Event::SCallbackInfo& info);
        void pointerButton(const IPointer::SButtonEvent& event, Event::SCallbackInfo& info);
        void touchDown(Event::SCallbackInfo& info);
        void touchMoved(Event::SCallbackInfo& info);
        void keyPressed(const IKeyboard::SKeyEvent& event, Event::SCallbackInfo& info);
        // The screen a button went down on holds the pointer until it goes up.
        SP<CScreen> grab() const;
        bool        swallowingPointer() const;
        void        releaseForwardedButtons(uint32_t timeMs);

        // --- A window dragged or resized (Drag.cpp) ---

        struct SDrag {
            PHLWINDOWREF    window;
            PHLWORKSPACEREF originalWorkspace;
            CBox            originalBox, originalVisualBox; // global
            Vector2D        originalFloatSize;
            Vector2D        grabRatio    = {0.5, 0.5};
            bool            startedTiled = false;
            bool            crossMonitor = false;
            WP<CScreen>     source;
            // Screens opened to drop onto, closed after unless they got it.
            std::vector<WP<CScreen>> transients;
        };
        SDrag drag;

        PHLWINDOW dragged() const;
        bool      dragging() const;
        // The dragged window's box at the pointer, in global layout space.
        CBox      draggedGlobalBox() const;
        bool      beginDrag(CScreen& screen, const Vector2D& at);
        void      updateDrag();
        void      endDrag();
        void      cancelDrag();
        // Carry on a drag Hyprland started (SUPER + drag) in `screen`.
        bool      adoptNativeDrag(CScreen& screen, const PHLWINDOW& expected);

        struct SResize {
            PHLWINDOWREF        pending, active;
            WP<CScreen>         screen;
            PHLWORKSPACEREF     workspace;
            Vector2D            start, last;
            CBox                originalBox;
            Layout::eRectCorner corner = Layout::CORNER_NONE;
        };
        SResize resize;

        bool armResize(CScreen& screen, const Vector2D& at);
        bool beginResize();
        void updateResize();
        void endResize();

        // A just-dropped window drawn as focused for a moment, so its colours
        // don't flash to inactive for a frame.
        PHLWINDOW pseudoFocused(const Time::steady_tp& now);

      private:
        // Session.cpp
        void start();
        void stop();
        void applyOverrides();
        void restoreOverrides();
        void activateSubmap();
        void restoreSubmap();

        // Input.cpp
        Interaction::SPointerConfig pointerConfig(const CScreen& screen) const;
        SP<CScreen>                 find(const CScreen* screen) const;
        bool                        dispatchSubmapClick(uint32_t button);

        // Drag.cpp
        void        finishDrag(const SP<CScreen>& destination = {});
        void        clearDrag();
        void        ensureDropScreen();
        CBox        resizedBox() const;
        SP<CScreen> resizeScreen() const;

        struct SPointerTargets : Interaction::IPointerTargets {
            explicit SPointerTargets(COverview& o) : overview(o) {}
            COverview& overview;

            bool submap() override;
            void submapClick(uint32_t button) override;
            void click() override;
            bool beginDrag(const Interaction::SPoint& at) override;
            void drag() override;
            void drop() override;
            void abandonDrag() override;
            bool armResize(const Interaction::SPoint& at) override;
            void disarmResize() override;
            bool beginResize() override;
            void resize() override;
            void endResize() override;
        };

        std::vector<SP<CScreen>> m_screens;
        std::vector<WP<CScreen>> m_linked;
        bool                     m_started = false;

        SPointerTargets              m_pointerTargets{*this};
        Interaction::CPointer        m_pointer{m_pointerTargets};
        WP<CScreen>                  m_grab;
        WP<CScreen>                  m_pointerScreen; // the one the pointer's last event was for
        std::unordered_set<uint32_t> m_forwardedButtons; // passed to a layer while the submap is on

        PHLWINDOWREF    m_pseudoFocused;
        Time::steady_tp m_pseudoFocusUntil = {};
        bool            m_finishingAdoptedDrag = false;

        // Hyprland's state while open.
        struct SAnimationConfig {
            std::string                                    name;
            Hyprutils::Animation::SAnimationPropertyConfig config;
        };
        std::vector<SAnimationConfig> m_savedAnimations;
        bool                          m_overridden = false;
        int m_noWarps = 0, m_warpOnChangeWorkspace = 0, m_warpOnToggleSpecial = 0, m_warpBackAfterNonMouseInput = 0, m_followMouse = 0;
        bool        m_usesSubmap   = false;
        bool        m_submapActive = false;
        std::string m_previousSubmap;

        CHyprSignalListener m_mouseMove, m_mouseButton, m_touchMove, m_touchDown, m_key, m_windowOpen, m_windowClose, m_windowMove, m_windowActive,
            m_windowFullscreen, m_workspaceCreated, m_workspaceRemoved;
    };

    // The one overview, from init() to exit().
    COverview* overview();
    void       createOverview();
    void       destroyOverview();

    // The overview's own submap, when the config defines one: its binds
    // replace the overview's keys and clicks while it is open.
    inline constexpr const char* SUBMAP = "hyprgrid_overview";

    inline bool hasSubmap() {
        return Keybinds::mgr() && Keybinds::mgr()->registry().hasSubmap(SUBMAP);
    }

    inline bool submapActive() {
        return Keybinds::mgr() && Keybinds::mgr()->currentSubmap() == SUBMAP;
    }
}
