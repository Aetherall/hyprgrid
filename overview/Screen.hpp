// A screen of the overview: one monitor zoomed out, drawing its board from
// that monitor's view (View.hpp). It keeps what that monitor shows (the
// workspaces of its board, laid on their cells) and what is selected there;
// the session (Session.hpp) owns everything shared between screens: input,
// drags, and the changes to Hyprland's state while any is open.
//
// Screen.cpp: the model, the camera, hit tests, selection and closing.
// ScreenRender.cpp: drawing. Frames.cpp: frames and damage while zoomed out.
// Visibility.cpp: making Hyprland draw what it hides.
#pragma once

#include "Internal.hpp"
#include "DropIndicator.hpp"

#include <unordered_map>

struct wl_event_source;

namespace Overview {
    class CScreen {
      public:
        explicit CScreen(PHLMONITOR monitor);
        ~CScreen();

        CScreen(const CScreen&)            = delete;
        CScreen& operator=(const CScreen&) = delete;

        PHLMONITORREF monitor;

        // --- Following the view ---

        void syncFromView();
        // The zoom heads back to 0 (true): stand down without committing; or
        // up again (false): open again.
        void viewClosing(bool heading);
        // The zoom settled at 0: gone.
        void finishClosing();
        // Commit the selection and zoom back in.
        void close();
        // Close, leaving the monitor as it was (a screen opened for a drag).
        void dismissTransient();
        void reopen();
        bool isClosing() const;
        // Waiting for the zoom to settle at 0 to go.
        bool removalPending() const;

        // --- Each frame ---

        void onPreRender();
        void render();
        void damage();
        void requestInputFrame();

        // Frames.cpp: which frames and damage go through while zoomed out.
        bool shouldHandleSurfaceDamage(SP<CWLSurfaceResource> surface);
        bool shouldAllowSurfaceFrame(SP<CWLSurfaceResource> surface, const Time::steady_tp& now);
        bool shouldAllowRealtimePreviewSchedule();
        bool shouldSuppressRenderDamage() const;
        bool blockDamageReporting = false;

        // --- The pointer, in this monitor's device pixels ---

        Vector2D pointerLocal;
        void     updatePointer();

        // --- What is where ---

        struct SWorkspaceImage {
            PHLWORKSPACE              workspace;
            Grid::SCell               cell;
            std::vector<PHLWINDOWREF> windows;  // in stacking order
            Scene::SInsets            overflow; // how far its windows reach past its cell
        };

        const SWorkspaceImage* imageOf(const PHLWORKSPACE& workspace) const;
        // Where a workspace's cell is drawn from the origin, in device pixels.
        Vector2D offsetOf(const SWorkspaceImage& image, float zoomScale) const;
        CBox     workspaceBox(const SWorkspaceImage& image) const;
        CBox     usableBox(const SWorkspaceImage& image) const;
        CBox     windowBox(const PHLWINDOW& window, const SWorkspaceImage& image, bool round = true) const;
        // As dragged: a group by its layout box.
        CBox     dragBox(const PHLWINDOW& window, const SWorkspaceImage& image, bool round = true) const;
        // A box in global layout space (of a window on `source`) on screen.
        CBox     globalBox(const CBox& box, const SWorkspaceImage& image, const PHLMONITOR& source, bool round = true) const;
        // A point on screen back into global layout space, for a workspace.
        Vector2D toGlobal(const SWorkspaceImage& image, const Vector2D& point) const;
        float    zoomScale() const;

        PHLWINDOW    windowAt(const Vector2D& point, PHLWORKSPACE* workspace = nullptr) const;
        PHLWORKSPACE workspaceAt(const Vector2D& point) const;
        // Where a dragged window would land: the workspace of a window under
        // `point`, or the one whose usable box holds it.
        PHLWORKSPACE dropWorkspaceAt(const Vector2D& point, const PHLWINDOW& dragged) const;
        // The tiled window a drop at the pointer would go next to.
        CDropIndicator::SDropAnchor dropAnchorAt(const SWorkspaceImage& image, const PHLWINDOW& dragged);

        // Empty cells next to this monitor's workspaces, where a dropped
        // window creates a workspace.
        std::vector<Grid::SCell>   dropSlots() const;
        CBox                       slotBox(const Grid::SCell& cell) const;
        std::optional<Grid::SCell> slotAt(const Vector2D& point) const;
        PHLWORKSPACE               createSlotWorkspace(const Grid::SCell& cell);

        // Rebuild the model before the next frame.
        void markDirty();
        void redrawAll();

        // --- The selection: the window (or workspace) closing lands on ---

        PHLWINDOWREF    selection;
        PHLWORKSPACEREF selectedWorkspace;

        bool select(const PHLWINDOW& window, bool syncFocus);
        bool selectWindowAtPointer(bool syncFocus);
        void selectHoveredWorkspace();
        bool moveSelection(const std::string& direction);
        bool windowAction(const std::string& action);
        void rememberSelection(const PHLWINDOW& window);
        void onWindowActive(const PHLWINDOW& window);
        void onWindowFullscreen(const PHLWINDOW& window);

        // Visibility.cpp: the bar shows over a fullscreen window while zoomed
        // out: its fullscreen state is announced as off (`hide`), or as it is.
        void emitFullscreenState(PHLWINDOW window, bool hide);

      private:
        // Screen.cpp
        void      rebuildImages();
        void      seedRememberedSelections();
        void      updateOverflow();
        void      onWorkspaceChange();
        void      syncSelectionToWorkspace();
        void      syncFocusedSelection();
        bool      moveToCell(int dx, int dy);
        void      moveToWorkspace(const PHLWORKSPACE& workspace);
        PHLWINDOW windowNearestCentre(const SWorkspaceImage& image) const;
        float     alphaOf(const SWorkspaceImage& image) const;
        void      setClosing(bool closing);
        void      close(bool commit);

        // ScreenRender.cpp
        void renderGlobalWallpaper(const Time::steady_tp& now);
        void renderWallpaperLayers(const CBox& box, float scale, const Time::steady_tp& now, float alpha = 1.F);
        void updateBackdropBlurCache(int wallpaperMode, const Time::steady_tp& now);
        void renderBackdropBlurCache();
        void renderWorkspaceBackground(const SWorkspaceImage& image, float scale, int wallpaperMode, const Time::steady_tp& now);
        void renderWorkspaceLive(const SWorkspaceImage& image, float scale, const Time::steady_tp& now);
        bool hasVisiblePrecomputedBlurWindow(float scale) const;
        void renderWindowLive(const PHLWINDOW& window, const CBox& box, float scale, const Time::steady_tp& now, const CBox* workspaceBox = nullptr, bool dragged = false);
        void renderDraggedWindow(const Time::steady_tp& now);
        void renderPinnedFloatingWindows(float scale, const Time::steady_tp& now);
        void renderDropSlots(float scale);
        void renderMonitorFrames(float scale);
        void renderGridCells();
        void renderSeam(float scale);
        CBox visibleBox(const SWorkspaceImage& image, const CBox& box, float scale) const;

        // Frames.cpp
        void       sendFrameCallbacks(const Time::steady_tp& now);
        bool       isSelected(const PHLWORKSPACE& workspace) const;
        bool       hasRunningWorkspaceAnimation() const;
        bool       shouldAllowRealtimePreviewFrame() const;
        void       scheduleMinimumPreviewFrame();
        void       schedulePreviewFrameAfter(std::chrono::milliseconds delay);
        void       scheduleRealtimePreviewFrame();
        static int realtimePreviewTimerCallback(void* data);

        // Visibility.cpp
        void forceSurfaceVisible(SP<CWLSurfaceResource> surface);
        void forceWindowSurfacesVisible(const PHLWINDOW& window);
        void forceWindowVisible(const PHLWINDOW& window);
        void forceLayersAboveFullscreen();
        void restoreForcedVisibility();
        void recalcDecorations(const PHLWORKSPACE& workspace);

        // The workspace the camera's offsets are from (the monitor's active
        // one when the screen last followed it).
        PHLWORKSPACE startedOn;

        std::vector<SWorkspaceImage> images;
        std::vector<PHLWINDOWREF>    pinnedFloatingWindows;
        // Each workspace's last selection, by selector.
        std::unordered_map<std::string, PHLWINDOWREF> rememberedSelection;
        std::string                  focusSyncedFromWorkspaceKey;
        bool                         rebuildPending = false;

        // A workspace inserted or removed while zoomed out: the others slide
        // between their cells, it fades in or out.
        struct SInsertTransition {
            bool                                           active  = false;
            std::string                                    key     = "";
            bool                                           fadeIn  = true;
            std::unordered_map<std::string, Scene::SPoint> oldCells, newCells;
            Scene::SPoint                                  removedCell;
        };
        SInsertTransition insertTransition;
        PHLWORKSPACEREF   pendingRemovedWorkspace;

        PHLANIMVAR<float>                                  scale;
        PHLANIMVAR<Vector2D>                               viewOffset;
        PHLANIMVAR<float>                                  insertProgress;
        PHLANIMVAR<float>                                  insertFadeProgress;
        SP<Hyprutils::Animation::SAnimationPropertyConfig> insertFadeConfig, removeFadeConfig;

        // Blur caches.
        bool                     overviewBlurDirty         = true;
        bool                     backdropBlurDirty         = true;
        bool                     overviewBlurStateValid    = false;
        float                    lastOverviewBlurScale     = 1.F;
        int                      lastBackdropWallpaperMode = -1;
        Vector2D                 lastOverviewBlurViewOffset;
        SP<Render::IFramebuffer> backdropBlurFB;

        // Frame pacing.
        Time::steady_tp  lastRealtimePreviewFrame      = {};
        Time::steady_tp  realtimePreviewTimerDue       = {};
        wl_event_source* realtimePreviewTimer          = nullptr;
        bool             realtimePreviewTimerArmed     = false;
        bool             realtimePreviewFrameQueued    = false;
        bool             selectedWorkspaceFramePending = false;
        bool             inputFramePending             = false;
        bool             sendingFrameCallbacks         = false;

        // What Hyprland hides, drawn anyway while zoomed out; restored after.
        struct SForcedSurface {
            WP<CWLSurfaceResource> surface;
            CRegion                visibleRegion;
        };
        struct SForcedWindow {
            PHLWINDOWREF window;
            bool         hidden = false;
        };
        struct SForcedLayer {
            PHLLSREF layer;
            bool     aboveFullscreen = true;
            float    alpha           = 1.F;
        };
        std::vector<SForcedSurface> forcedSurfaces;
        std::vector<SForcedWindow>  forcedWindows;
        std::vector<SForcedLayer>   forcedLayers;
        bool                        emittingFullscreenState = false;

        bool closing             = false;
        bool closeApplied        = false; // close() ran; guards against a second
        bool closeRemovalPending = false;
    };
}
