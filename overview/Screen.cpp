// A screen of the overview (see Screen.hpp): what one zoomed-out monitor
// shows, where, and what is selected there.

#include "Internal.hpp"
#include "Session.hpp"
#include "View.hpp"
#include "Window.hpp"

namespace Overview {
    static constexpr const char* INSERT_FADE_BEZIER = "hyprgridOverviewInsertFade";
    static constexpr const char* REMOVE_FADE_BEZIER = "hyprgridOverviewRemoveFade";

    // Switch `monitor` to `workspace` leaving focus to the overview (the
    // selection has it), so Hyprland announces nothing: announced here, as
    // Hyprland does, when the monitor is the focused one.
    static bool changeWorkspace(const PHLMONITOR& monitor, const PHLWORKSPACE& workspace) {
        if (!monitor || !workspace || monitor->m_activeWorkspace == workspace)
            return false;

        const bool FOCUSED = Desktop::focusState()->monitor() == monitor;
        monitor->changeWorkspace(workspace, false, true, true);
        if (!FOCUSED || monitor->m_activeWorkspace != workspace)
            return true;

        IPC::Socket2::sock()->postEvent({.event = "workspace", .data = workspace->addressableName()});
        IPC::Socket2::sock()->postEvent({.event = "workspacev2", .data = std::format("{},{}", workspace->addressableName(), workspace->displayName())});
        Event::bus()->m_events.workspace.active.emit(workspace);
        return true;
    }

    static SP<Hyprutils::Animation::SAnimationPropertyConfig> fadeConfig(const char* bezier, float speedFactor, float fallbackSpeed) {
        const auto MOVE   = ::Config::animationTree()->getAnimationPropertyConfig("windowsMove");
        const auto VALUES = MOVE && MOVE->pValues ? MOVE->pValues.lock() : MOVE;
        auto       config = makeShared<Hyprutils::Animation::SAnimationPropertyConfig>();
        config->overridden      = true;
        config->internalBezier  = bezier;
        config->internalSpeed   = VALUES ? VALUES->internalSpeed * speedFactor : fallbackSpeed;
        config->internalEnabled = VALUES ? VALUES->internalEnabled : 1;
        config->internalStyle   = VALUES ? VALUES->internalStyle : "";
        config->pValues         = config;
        return config;
    }

    CScreen::CScreen(PHLMONITOR monitor_) : monitor(monitor_), startedOn(monitor_->m_activeWorkspace) {
        realtimePreviewTimer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, realtimePreviewTimerCallback, this);
        scheduleMinimumPreviewFrame();

        if (!Animation::mgr()->bezierExists(INSERT_FADE_BEZIER))
            Animation::mgr()->addBezierWithName(INSERT_FADE_BEZIER, Vector2D{0.5, 0.0}, Vector2D{0.5, 0.0});
        if (!Animation::mgr()->bezierExists(REMOVE_FADE_BEZIER))
            Animation::mgr()->addBezierWithName(REMOVE_FADE_BEZIER, Vector2D{0.5, 1.0}, Vector2D{0.5, 1.0});
        insertFadeConfig = fadeConfig(INSERT_FADE_BEZIER, 1.2F, 12.F);
        removeFadeConfig = fadeConfig(REMOVE_FADE_BEZIER, 1.F, 10.F);

        const auto MOVE = ::Config::animationTree()->getAnimationPropertyConfig("windowsMove");
        Animation::mgr()->createAnimation(1.F, scale, MOVE, AVARDAMAGE_NONE);
        Animation::mgr()->createAnimation({}, viewOffset, MOVE, AVARDAMAGE_NONE);
        Animation::mgr()->createAnimation(1.F, insertProgress, MOVE, AVARDAMAGE_NONE);
        Animation::mgr()->createAnimation(1.F, insertFadeProgress, insertFadeConfig, AVARDAMAGE_NONE);

        // The scale and offset follow hyprgrid's view (syncFromView()); never
        // animated here.
        scale->setUpdateCallback([this](auto) { damage(); });
        viewOffset->setUpdateCallback([this](auto) { damage(); });
        insertProgress->setUpdateCallback([this](auto) { damage(); });
        insertFadeProgress->setUpdateCallback([this](auto) { damage(); });

        const auto FULLSCREEN = fullscreenOf(monitor_->m_activeWorkspace);
        emitFullscreenState(FULLSCREEN ? FULLSCREEN : Desktop::focusState()->window(), true);

        updatePointer();
        redrawAll();
        rememberSelection(Desktop::focusState()->window());
        selectedWorkspace = startedOn;
        syncSelectionToWorkspace();
        syncFromView();
    }

    CScreen::~CScreen() {
        if (const auto OPENGL = g_pHyprRenderer ? g_pHyprRenderer->glBackend() : WP<Render::GL::CHyprOpenGLImpl>{})
            OPENGL->makeEGLCurrent();
        if (realtimePreviewTimer) {
            wl_event_source_remove(realtimePreviewTimer);
            realtimePreviewTimer = nullptr;
        }
        if (backdropBlurFB)
            backdropBlurFB->release();
        backdropBlurFB.reset();

        const auto MONITOR = monitor.lock();
        const auto ACTIVE  = MONITOR ? MONITOR->m_activeWorkspace : PHLWORKSPACE{};
        const auto FS      = fullscreenOf(ACTIVE);
        emitFullscreenState(FS ? FS : shown(Desktop::focusState()->window()), false);
        restoreForcedVisibility();
        images.clear();
        if (MONITOR)
            MONITOR->m_blurFBDirty = true;
    }

    // --- The model ---

    const CScreen::SWorkspaceImage* CScreen::imageOf(const PHLWORKSPACE& workspace) const {
        if (!workspace)
            return nullptr;
        const auto IT = std::ranges::find_if(images, [&](const auto& image) { return image.workspace == workspace; });
        return IT == images.end() ? nullptr : &*IT;
    }

    void CScreen::markDirty() {
        rebuildPending = true;
        damage();
    }

    // The board's workspaces with a cell (special and named ones have none),
    // drawn row by row.
    void CScreen::rebuildImages() {
        const auto REMOVED = pendingRemovedWorkspace.lock();
        images.clear();
        for (const auto& ref : State::workspaceState()->workspaces()) {
            const auto WORKSPACE = ref.lock();
            if (!valid(WORKSPACE) || WORKSPACE == REMOVED || WORKSPACE->type() == Workspace::eWorkspaceType::SPECIAL || !Grid::sameBoard(WORKSPACE->m_monitor.lock(), monitor.lock()))
                continue;
            if (const auto CELL = Grid::cellOf(WORKSPACE))
                images.push_back({.workspace = WORKSPACE, .cell = *CELL});
        }
        std::ranges::sort(images, [](const auto& a, const auto& b) { return a.cell.y != b.cell.y ? a.cell.y < b.cell.y : a.cell.x < b.cell.x; });

        // The selected workspace went: the selection's, the one it started on,
        // or any.
        if (!imageOf(selectedWorkspace.lock())) {
            const auto SELECTION = selection.lock();
            if (SELECTION && imageOf(SELECTION->m_workspace))
                selectedWorkspace = SELECTION->m_workspace;
            else if (imageOf(startedOn))
                selectedWorkspace = startedOn;
            else
                selectedWorkspace = images.empty() ? PHLWORKSPACE{} : images.front().workspace;
        }
        if (images.empty())
            selection.reset();
    }

    void CScreen::seedRememberedSelections() {
        for (const auto& image : images) {
            const auto KEY = Workspace::selector(*image.workspace);
            if (const auto IT = rememberedSelection.find(KEY); IT != rememberedSelection.end()) {
                const auto REMEMBERED = shown(IT->second.lock());
                if (REMEMBERED && REMEMBERED->m_workspace == image.workspace && isShown(REMEMBERED))
                    continue;
            }
            const auto LAST = shown(image.workspace->getLastFocusedWindow());
            if (LAST && LAST->m_workspace == image.workspace && isShown(LAST))
                rememberedSelection[KEY] = LAST;
        }
    }

    void CScreen::rememberSelection(const PHLWINDOW& window_) {
        const auto WINDOW = shown(window_);
        if (WINDOW && WINDOW->m_workspace)
            rememberedSelection[Workspace::selector(*WINDOW->m_workspace)] = WINDOW;
    }

    void CScreen::redrawAll() {
        rebuildImages();
        seedRememberedSelections();
        pinnedFloatingWindows.clear();

        std::unordered_map<std::string, size_t> indexOf;
        for (size_t i = 0; i < images.size(); ++i)
            indexOf.emplace(Workspace::selector(*images[i].workspace), i);

        std::vector<PHLWINDOW> added, addedPinned;
        const auto             add = [&](const PHLWINDOW& window) {
            const auto WINDOW = shown(window);
            if (isPinnedFloating(WINDOW)) {
                if (std::ranges::find(addedPinned, WINDOW) == addedPinned.end()) {
                    addedPinned.push_back(WINDOW);
                    pinnedFloatingWindows.emplace_back(WINDOW);
                }
                return;
            }
            if (!isShown(WINDOW) || !WINDOW->m_workspace || std::ranges::find(added, WINDOW) != added.end())
                return;
            const auto IT = indexOf.find(Workspace::selector(*WINDOW->m_workspace));
            if (IT == indexOf.end())
                return;
            added.push_back(WINDOW);
            images[IT->second].windows.emplace_back(WINDOW);
        };

        // A group's current window first, then the others (which show as it).
        for (const auto& window : Desktop::windowState()->windows()) {
            if (shown(window) == window)
                add(window);
        }
        for (const auto& window : Desktop::windowState()->windows()) {
            if (shown(window) != window)
                add(window);
        }

        updateOverflow();
    }

    // Each frame: cells follow the grid (a layout may move workspaces), and
    // how far windows reach past their cell, in the cell's space (another
    // monitor's windows as they are fitted in).
    void CScreen::updateOverflow() {
        const auto MONITOR = monitor.lock();
        if (!MONITOR)
            return;

        const auto ON = sceneMonitor(MONITOR);
        for (auto& image : images) {
            if (const auto CELL = Grid::cellOf(image.workspace))
                image.cell = *CELL;

            std::vector<Scene::SBox> boxes;
            for (const auto& window : shownWindows(image.windows)) {
                if (window->isFloating())
                    continue;
                const auto SOURCE = window->m_monitor.lock();
                boxes.push_back(Scene::intoCell(ON, sceneMonitor(SOURCE ? SOURCE : MONITOR), sceneBox(window->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT)),
                                                !SOURCE || SOURCE == MONITOR));
            }
            image.overflow = Scene::overflow(ON.size, boxes);
        }
    }

    // --- The camera ---

    float CScreen::zoomScale() const {
        return scale->value();
    }

    // The scale from the view's zoom, and the offset from its position,
    // measured from the cell the screen started on. Drawing never depends on
    // which cell that is.
    void CScreen::syncFromView() {
        const auto MONITOR = monitor.lock();
        if (!MONITOR)
            return;

        const float SCALE = Scene::zoomScale(View::zoom(MONITOR), Overview::Config::getScale());
        scale->setValueAndWarp(SCALE);

        const auto ORIGIN = Grid::cellOf(startedOn);
        if (!ORIGIN) {
            viewOffset->setValueAndWarp(Vector2D{});
            return;
        }
        const auto OFFSET = Scene::viewOffsetAt(sceneMonitor(MONITOR), SCALE, Overview::Config::getWorkspaceGap(), View::position(MONITOR), *ORIGIN);
        viewOffset->setValueAndWarp(toVector(OFFSET));
    }

    Vector2D CScreen::offsetOf(const SWorkspaceImage& image, float zoomScale_) const {
        const auto MONITOR = monitor.lock();
        if (!MONITOR)
            return {};
        auto camera   = sceneCamera(MONITOR, zoomScale_, {});
        camera.origin = Grid::cellOf(startedOn).value_or(Grid::SCell{});
        Scene::SPoint at{double(image.cell.x), double(image.cell.y)};

        // A workspace inserted or removed next to it: sliding to its cell.
        if (insertTransition.active) {
            const auto KEY = Workspace::selector(*image.workspace);
            if (const auto TO = insertTransition.newCells.find(KEY); TO != insertTransition.newCells.end()) {
                const auto FROM = insertTransition.oldCells.find(KEY);
                at              = FROM == insertTransition.oldCells.end() ? TO->second : Scene::slide(FROM->second, TO->second, insertProgress->value());
            }
        }
        return toVector(Scene::cellOffset(camera, at));
    }

    CBox CScreen::globalBox(const CBox& box, const SWorkspaceImage& image, const PHLMONITOR& source, bool round) const {
        const auto MONITOR = monitor.lock();
        if (!MONITOR)
            return {};
        const auto LOCAL = Scene::intoCell(sceneMonitor(MONITOR), sceneMonitor(source ? source : MONITOR), sceneBox(box), !source || source == MONITOR);
        return toCBox(Scene::toScreen(sceneCamera(MONITOR, scale->value(), viewOffset->value()), LOCAL, scenePoint(offsetOf(image, scale->value()))), round);
    }

    CBox CScreen::workspaceBox(const SWorkspaceImage& image) const {
        const auto MONITOR = monitor.lock();
        if (!MONITOR)
            return {};
        return toCBox(Scene::toScreen(sceneCamera(MONITOR, scale->value(), viewOffset->value()), {0, 0, MONITOR->m_size.x, MONITOR->m_size.y}, scenePoint(offsetOf(image, scale->value()))));
    }

    CBox CScreen::usableBox(const SWorkspaceImage& image) const {
        const auto MONITOR = monitor.lock();
        if (!MONITOR || !image.workspace->space())
            return workspaceBox(image);
        auto USABLE = image.workspace->space()->workArea();
        USABLE.translate(-MONITOR->m_position);
        USABLE.w = std::max(USABLE.w, 1.0);
        USABLE.h = std::max(USABLE.h, 1.0);
        return toCBox(Scene::toScreen(sceneCamera(MONITOR, scale->value(), viewOffset->value()), sceneBox(USABLE), scenePoint(offsetOf(image, scale->value()))));
    }

    CBox CScreen::windowBox(const PHLWINDOW& window, const SWorkspaceImage& image, bool round) const {
        if (!window)
            return {};
        return globalBox(window->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT), image, window->m_monitor.lock(), round);
    }

    CBox CScreen::dragBox(const PHLWINDOW& window, const SWorkspaceImage& image, bool round) const {
        if (!window)
            return {};
        if (const auto TARGET = window->layoutTarget(); window->grouping().group() && TARGET)
            return globalBox(TARGET->position(), image, window->m_monitor.lock(), round);
        return windowBox(window, image, round);
    }

    Vector2D CScreen::toGlobal(const SWorkspaceImage& image, const Vector2D& point) const {
        const auto MONITOR = monitor.lock();
        if (!MONITOR)
            return point;
        const auto LOCAL  = Scene::fromScreen(sceneCamera(MONITOR, scale->value(), viewOffset->value()), scenePoint(point), scenePoint(offsetOf(image, scale->value())));
        // Another monitor's workspace (a shared board): back into its own space.
        const auto SOURCE = image.workspace->m_monitor.lock();
        return toVector(Scene::outOfCell(sceneMonitor(MONITOR), sceneMonitor(SOURCE ? SOURCE : MONITOR), LOCAL, !SOURCE || SOURCE == MONITOR));
    }

    float CScreen::alphaOf(const SWorkspaceImage& image) const {
        if (!insertTransition.active || !insertTransition.fadeIn || Workspace::selector(*image.workspace) != insertTransition.key || insertTransition.oldCells.contains(insertTransition.key))
            return 1.F;
        return std::clamp(insertFadeProgress->value(), 0.F, 1.F);
    }

    void CScreen::updatePointer() {
        pointerLocal = pointerOn(monitor.lock());
    }

    // --- Hit tests ---

    PHLWINDOW CScreen::windowAt(const Vector2D& point, PHLWORKSPACE* workspace) const {
        for (const auto& image : images) {
            std::optional<size_t>             fullscreen;
            const auto                        WINDOWS = shownWindows(image.windows, image.workspace, fullscreen);
            std::vector<Interaction::SWindow> boxes;
            for (const auto& window : WINDOWS)
                boxes.push_back({sceneBox(windowBox(window, image)), window->isFloating()});
            if (const auto HIT = Interaction::windowAt(boxes, scenePoint(point), fullscreen)) {
                if (workspace)
                    *workspace = image.workspace;
                return WINDOWS[*HIT];
            }
        }
        return nullptr;
    }

    PHLWORKSPACE CScreen::workspaceAt(const Vector2D& point) const {
        for (const auto& image : images) {
            if (usableBox(image).containsPoint(point))
                return image.workspace;
        }
        return nullptr;
    }

    PHLWORKSPACE CScreen::dropWorkspaceAt(const Vector2D& point, const PHLWINDOW& dragged) const {
        for (const auto& image : images) {
            std::vector<Interaction::SWindow> boxes;
            for (const auto& window : shownWindows(image.windows)) {
                if (window != dragged)
                    boxes.push_back({sceneBox(dragBox(window, image)), window->isFloating()});
            }
            if (Interaction::windowAt(boxes, scenePoint(point)))
                return image.workspace;
        }
        return workspaceAt(point);
    }

    CDropIndicator::SDropAnchor CScreen::dropAnchorAt(const SWorkspaceImage& image, const PHLWINDOW& dragged) {
        CDropIndicator::SDropAnchor result;
        const auto                  MONITOR = monitor.lock();
        const auto&                 DRAG    = overview()->drag;
        if (!MONITOR || !fullyOnMonitor(workspaceBox(image), MONITOR) || !DRAG.startedTiled)
            return result;

        // Back where it was picked up: back in its place.
        const auto ORIGINAL = DRAG.originalWorkspace.lock();
        if (dragged && ORIGINAL == image.workspace && !DRAG.originalVisualBox.empty() && !DRAG.originalBox.empty()) {
            const auto SOURCE = ORIGINAL->m_monitor.lock();
            const auto VISUAL = globalBox(DRAG.originalVisualBox, image, SOURCE);
            const auto HIT    = globalBox(DRAG.originalBox, image, SOURCE, false);
            if ((HIT.empty() ? VISUAL : HIT).containsPoint(pointerLocal)) {
                result.window = dragged;
                result.box    = VISUAL;
                return result;
            }
        }

        std::vector<PHLWINDOW>                windows;
        std::vector<std::pair<CBox, CBox>>    boxes;
        std::vector<Interaction::SDropTarget> targets;
        for (const auto& window : shownWindows(image.windows)) {
            if (window == dragged || window->isFloating())
                continue;
            const auto TARGET = window->layoutTarget();
            const auto BOX    = windowBox(window, image, false);
            const auto LAYOUT = TARGET ? globalBox(TARGET->position(), image, window->m_monitor.lock(), false) : CBox{};
            windows.push_back(window);
            boxes.push_back({BOX, LAYOUT});
            targets.push_back({sceneBox(BOX), LAYOUT.empty() ? Scene::SBox{} : sceneBox(LAYOUT)});
        }

        if (const auto HIT = Interaction::dropTarget(targets, scenePoint(pointerLocal))) {
            const auto& [BOX, LAYOUT] = boxes[*HIT];
            result.window             = windows[*HIT];
            result.box                = BOX;
            result.logicalBox         = LAYOUT;
            switch (Interaction::dropSide(sceneBox(BOX), scenePoint(pointerLocal))) {
                case Interaction::ESide::LEFT: result.direction = "l"; break;
                case Interaction::ESide::RIGHT: result.direction = "r"; break;
                case Interaction::ESide::UP: result.direction = "u"; break;
                default: result.direction = "d"; break;
            }
        }
        return result;
    }

    // --- Drop slots: empty cells a dragged window can open a workspace in ---

    // Empty cells sharing an edge with one of this monitor's workspaces, on
    // its board: the board grows there, staying in one piece.
    std::vector<Grid::SCell> CScreen::dropSlots() const {
        std::vector<Grid::SCell> cells;
        for (const auto& image : images)
            cells.push_back(image.cell);
        const auto BOARD = Grid::boardOf(monitor.lock());
        return Scene::dropSlots(cells, [&BOARD](const Grid::SCell& cell) { return Grid::workspaceAt(BOARD, cell) != nullptr; });
    }

    // A cell's box on screen, as a workspace there would have.
    CBox CScreen::slotBox(const Grid::SCell& cell) const {
        const auto MONITOR = monitor.lock();
        const auto ORIGIN  = Grid::cellOf(startedOn);
        if (!MONITOR || !ORIGIN)
            return {};
        auto camera   = sceneCamera(MONITOR, scale->value(), viewOffset->value());
        camera.origin = *ORIGIN;
        return toCBox(Scene::cellBox(camera, cell));
    }

    std::optional<Grid::SCell> CScreen::slotAt(const Vector2D& point) const {
        for (const auto& slot : dropSlots()) {
            if (slotBox(slot).containsPoint(point))
                return slot;
        }
        return std::nullopt;
    }

    // The workspace for a slot, created with the id the grid reserves for that
    // cell; the model is rebuilt to include it.
    PHLWORKSPACE CScreen::createSlotWorkspace(const Grid::SCell& cell) {
        const auto MONITOR = monitor.lock();
        if (!MONITOR)
            return nullptr;

        // Across the seam, the cell is the other monitor's region: its workspace.
        const auto OWNER = Grid::regionOwner(cell) ? Grid::regionOwner(cell) : MONITOR;
        const auto ID    = Grid::idFor(Grid::boardOf(OWNER), cell);
        // Through the workspace state, which registers it (create() alone doesn't).
        const auto WS = State::Workspace::state()->createNumbered(Workspace::SWorkspaceNumberedID{sc<uint32_t>(ID)}, OWNER, std::to_string(ID));
        if (!WS)
            return nullptr;

        redrawAll();
        return imageOf(WS) ? WS : nullptr;
    }

    // --- The selection ---

    PHLWINDOW CScreen::windowNearestCentre(const SWorkspaceImage& image) const {
        std::optional<size_t>             fullscreen;
        const auto                        WINDOWS = shownWindows(image.windows, image.workspace, fullscreen);
        std::vector<Interaction::SWindow> boxes;
        for (const auto& window : WINDOWS)
            boxes.push_back({sceneBox(windowBox(window, image)), window->isFloating()});
        const auto BEST = Interaction::nearestCentre(boxes, sceneBox(workspaceBox(image)), fullscreen);
        return BEST ? WINDOWS[*BEST] : PHLWINDOW{};
    }

    bool CScreen::select(const PHLWINDOW& window, bool syncFocus) {
        if (!window)
            return false;

        selection         = window;
        selectedWorkspace = window->m_workspace;
        rememberSelection(window);
        if (syncFocus) {
            if (const auto MONITOR = monitor.lock(); MONITOR && Desktop::focusState()->monitor() != MONITOR)
                Desktop::focusState()->rawMonitorFocus(MONITOR);
            syncFocusedSelection();
        }
        damage();
        return true;
    }

    bool CScreen::selectWindowAtPointer(bool syncFocus) {
        updatePointer();
        return select(windowAt(pointerLocal), syncFocus);
    }

    void CScreen::selectHoveredWorkspace() {
        updatePointer();
        const auto WORKSPACE = workspaceAt(pointerLocal);
        if (!WORKSPACE)
            return;

        selection.reset();
        selectedWorkspace  = WORKSPACE;
        const auto MONITOR = monitor.lock();

        // Another monitor's (a shared board): closing goes to that monitor.
        if (MONITOR && WORKSPACE->m_monitor == MONITOR && MONITOR->m_activeWorkspace != WORKSPACE) {
            if (focusSyncedFromWorkspaceKey.empty())
                focusSyncedFromWorkspaceKey = MONITOR->m_activeWorkspace ? Workspace::selector(*MONITOR->m_activeWorkspace) : "";
            changeWorkspace(MONITOR, WORKSPACE);
        }
        damage();
    }

    bool CScreen::windowAction(const std::string& action) {
        updatePointer();
        const auto WINDOW = windowAt(pointerLocal);
        if (!WINDOW)
            return false;
        if (action == "select")
            return select(WINDOW, true);
        if (action == "close") {
            WINDOW->sendClose();
            damage();
            return true;
        }
        return false;
    }

    // Move the selection to the workspace on the cell (dx, dy) away; false if
    // no workspace of this monitor is there.
    bool CScreen::moveToCell(int dx, int dy) {
        const auto FROM = imageOf(selectedWorkspace.lock());
        if (!FROM)
            return false;
        const Grid::SCell TARGET{FROM->cell.x + dx, FROM->cell.y + dy};
        for (const auto& image : images) {
            // Another monitor's workspace (a shared board) is its to show.
            if (image.cell == TARGET && image.workspace->m_monitor == monitor) {
                moveToWorkspace(image.workspace);
                return true;
            }
        }
        return false;
    }

    void CScreen::moveToWorkspace(const PHLWORKSPACE& workspace) {
        const auto IMAGE = imageOf(workspace);
        if (!IMAGE || workspace == selectedWorkspace.lock())
            return;

        selectedWorkspace = workspace;
        selection.reset();
        if (const auto IT = rememberedSelection.find(Workspace::selector(*workspace)); IT != rememberedSelection.end()) {
            const auto REMEMBERED = shown(IT->second.lock());
            if (REMEMBERED && REMEMBERED->m_workspace == workspace && isShown(REMEMBERED))
                selection = REMEMBERED;
        }
        if (!selection)
            selection = windowNearestCentre(*IMAGE);

        if (const auto MONITOR = monitor.lock(); MONITOR && MONITOR->m_activeWorkspace != workspace)
            changeWorkspace(MONITOR, workspace);
        damage();
    }

    // The selection on the selected workspace: kept if it is there, else the
    // one remembered, the focused one, or the one nearest the centre.
    void CScreen::syncSelectionToWorkspace() {
        const auto IMAGE = imageOf(selectedWorkspace.lock());
        if (!IMAGE) {
            selection.reset();
            return;
        }

        const auto holds = [&](const PHLWINDOW& window) { return std::ranges::any_of(IMAGE->windows, [&](const auto& ref) { return shown(ref.lock()) == window; }); };

        if (const auto SELECTED = shown(selection.lock()); SELECTED && SELECTED->m_workspace == IMAGE->workspace && holds(SELECTED)) {
            selection = SELECTED;
            rememberSelection(SELECTED);
            syncFocusedSelection();
            return;
        }

        if (const auto IT = rememberedSelection.find(Workspace::selector(*IMAGE->workspace)); IT != rememberedSelection.end()) {
            const auto REMEMBERED = shown(IT->second.lock());
            if (REMEMBERED && REMEMBERED->m_workspace == IMAGE->workspace && isShown(REMEMBERED) && holds(REMEMBERED)) {
                selection = REMEMBERED;
                syncFocusedSelection();
                return;
            }
        }

        const auto FOCUSED = Desktop::focusState()->window();
        if (isShown(FOCUSED) && FOCUSED->m_workspace == IMAGE->workspace) {
            selection = FOCUSED;
            rememberSelection(FOCUSED);
            syncFocusedSelection();
            return;
        }

        if (const auto NEAREST = windowNearestCentre(*IMAGE)) {
            selection = NEAREST;
            rememberSelection(NEAREST);
            syncFocusedSelection();
            return;
        }

        selection.reset();
        if (overview()->activeScreen().get() == this)
            Desktop::focusState()->fullWindowFocus(nullptr, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
    }

    // The selection is focused while zoomed out, on the active screen.
    void CScreen::syncFocusedSelection() {
        const auto WINDOW  = shown(selection.lock());
        const auto MONITOR = monitor.lock();
        if (!isShown(WINDOW) || !MONITOR)
            return;

        selection = WINDOW;
        if (overview()->activeScreen().get() != this)
            return;
        if (Desktop::focusState()->window() == WINDOW && WINDOW->m_workspace == MONITOR->m_activeWorkspace)
            return;

        const auto PREVIOUS = MONITOR->m_activeWorkspace;
        Desktop::focusState()->fullWindowFocus(WINDOW, Desktop::FOCUS_REASON_KEYBIND);
        if (WINDOW->m_workspace != PREVIOUS && focusSyncedFromWorkspaceKey.empty())
            focusSyncedFromWorkspaceKey = PREVIOUS ? Workspace::selector(*PREVIOUS) : "";
    }

    // The arrow keys: the tiled window next door; past the last one, the
    // workspace on the next cell that way.
    bool CScreen::moveSelection(const std::string& direction) {
        const bool LEFT = direction == "left", RIGHT = direction == "right", UP = direction == "up", DOWN = direction == "down";
        if (!LEFT && !RIGHT && !UP && !DOWN)
            return false;
        const Grid::SCell STEP{RIGHT ? 1 : LEFT ? -1 : 0, DOWN ? 1 : UP ? -1 : 0};

        const auto IMAGE    = imageOf(selectedWorkspace.lock());
        const auto onIt     = [&](const PHLWINDOW& window) { return window && IMAGE && window->m_workspace == IMAGE->workspace && isShown(window) && !window->isFloating(); };
        auto       current  = shown(selection.lock());
        if (IMAGE && !onIt(current)) {
            syncSelectionToWorkspace();
            current = shown(selection.lock());
        }
        if (!onIt(current))
            return moveToCell(STEP.x, STEP.y);

        selection           = current;
        const auto geometry = [](const PHLWINDOW& window) {
            const CBox BOX{window->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT), window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT)};
            return Interaction::SNeighbour{sceneBox(BOX), scenePoint(window->middle())};
        };
        std::vector<PHLWINDOW>               candidates;
        std::vector<Interaction::SNeighbour> geometries;
        for (const auto& window : shownWindows(IMAGE->windows)) {
            if (window == current || window->isFloating() || window->m_workspace != IMAGE->workspace || !Grid::sameBoard(window->m_monitor.lock(), monitor.lock()))
                continue;
            candidates.push_back(window);
            geometries.push_back(geometry(window));
        }

        const auto NEXT = Interaction::neighbour(geometry(current), geometries, STEP);
        if (!NEXT) // past the last window: the next workspace on the grid that way
            return moveToCell(STEP.x, STEP.y);

        selection = candidates[*NEXT];
        rememberSelection(candidates[*NEXT]);
        syncFocusedSelection();
        damage();
        return true;
    }

    void CScreen::onWindowActive(const PHLWINDOW& window_) {
        if (closing)
            return;

        g_pInputManager->unconstrainMouse();

        const auto WINDOW     = shown(window_);
        const auto FULLSCREEN = WINDOW ? fullscreenOf(WINDOW->m_workspace) : PHLWINDOW{};
        emitFullscreenState(FULLSCREEN && WINDOW->isFloating() ? FULLSCREEN : WINDOW, true);

        if (isShown(WINDOW) && Grid::sameBoard(WINDOW->m_monitor.lock(), monitor.lock())) {
            rebuildPending = true;
            selection      = WINDOW;
            rememberSelection(WINDOW);
            if (imageOf(WINDOW->m_workspace))
                selectedWorkspace = WINDOW->m_workspace;
        }
        damage();
    }

    void CScreen::onWindowFullscreen(const PHLWINDOW& window_) {
        if (closing || emittingFullscreenState)
            return;
        const auto WINDOW = shown(window_);
        if (WINDOW && WINDOW->m_monitor == monitor && Fullscreen::controller()->isFullscreen(WINDOW))
            emitFullscreenState(WINDOW, true);
    }

    // --- Each frame ---

    void CScreen::onPreRender() {
        const auto MONITOR = monitor.lock();
        if (MONITOR)
            MONITOR->m_solitaryClient.reset();

        forceLayersAboveFullscreen();
        updateOverflow();

        if (closing)
            return;

        if (MONITOR && MONITOR->m_activeWorkspace && MONITOR->m_activeWorkspace != startedOn) {
            rebuildPending = false;
            overviewBlurDirty = true;
            onWorkspaceChange();
            focusSyncedFromWorkspaceKey.clear();
            emitFullscreenState(Desktop::focusState()->window(), true);
            return;
        }

        focusSyncedFromWorkspaceKey.clear();

        if (rebuildPending) {
            rebuildPending    = false;
            overviewBlurDirty = true;
            redrawAll();
            syncSelectionToWorkspace();
            damage();
        }
    }

    // The monitor switched workspace while zoomed out: the screen follows. A
    // workspace that appeared (or an empty one left behind, removed) slides
    // the others between their cells and fades in (or out).
    void CScreen::onWorkspaceChange() {
        const auto MONITOR = monitor.lock();
        if (!MONITOR || !MONITOR->m_activeWorkspace)
            return;

        const auto                                     PREVIOUS = startedOn;
        std::unordered_map<std::string, Scene::SPoint> previousCells;
        for (const auto& image : images)
            previousCells.emplace(Workspace::selector(*image.workspace), Scene::SPoint{double(image.cell.x), double(image.cell.y)});

        const auto NEW      = MONITOR->m_activeWorkspace;
        const bool INSERTED = !previousCells.contains(Workspace::selector(*NEW));
        const auto REQUESTEDREMOVED = pendingRemovedWorkspace.lock();
        const bool LEFTEMPTY = PREVIOUS && PREVIOUS != NEW && PREVIOUS->type() == Workspace::eWorkspaceType::NORMAL &&
            !sc<Workspace::CRegularWorkspace*>(PREVIOUS.get())->isPersistent() && PREVIOUS->getWindowCount() == 0;
        const auto REMOVED = REQUESTEDREMOVED ? REQUESTEDREMOVED : LEFTEMPTY ? PREVIOUS : PHLWORKSPACE{};
        pendingRemovedWorkspace = REMOVED;

        startedOn = NEW;
        redrawAll();
        selectedWorkspace = startedOn;

        const bool REMOVEDGONE = REMOVED && !imageOf(REMOVED);
        if (INSERTED || REMOVEDGONE) {
            insertTransition.active = true;
            insertTransition.key    = INSERTED ? Workspace::selector(*NEW) : Workspace::selector(*REMOVED);
            insertTransition.fadeIn = INSERTED;
            insertFadeProgress->setConfig(INSERTED ? insertFadeConfig : removeFadeConfig);
            insertTransition.oldCells = previousCells;
            insertTransition.newCells.clear();
            insertTransition.removedCell = {};
            if (REMOVEDGONE && previousCells.contains(Workspace::selector(*REMOVED)))
                insertTransition.removedCell = previousCells.at(Workspace::selector(*REMOVED));
            for (const auto& image : images)
                insertTransition.newCells.emplace(Workspace::selector(*image.workspace), Scene::SPoint{double(image.cell.x), double(image.cell.y)});

            insertProgress->setValueAndWarp(0.F);
            insertFadeProgress->setValueAndWarp(0.F);
            *insertProgress     = 1.F;
            *insertFadeProgress = 1.F;
        } else {
            insertTransition = {};
            insertFadeProgress->setConfig(insertFadeConfig);
            insertProgress->setValueAndWarp(1.F);
            insertFadeProgress->setValueAndWarp(1.F);
        }

        syncSelectionToWorkspace();
        syncFromView();
        overviewBlurDirty = true;
        damage();
    }

    // --- Closing ---

    bool CScreen::isClosing() const {
        return closing;
    }

    bool CScreen::removalPending() const {
        return closeRemovalPending;
    }

    void CScreen::setClosing(bool closing_) {
        closing = closing_;
        if (closing) {
            inputFramePending = false;
            overview()->screenClosing(this);
        } else
            overview()->screenReopened();
    }

    void CScreen::close() {
        close(true);
    }

    void CScreen::dismissTransient() {
        if (closing && !closeRemovalPending) {
            setClosing(false);
            closeApplied = false;
        }
        close(false);
    }

    // `commit`: land on the selection; else leave the monitor as it is.
    void CScreen::close(bool commit) {
        if (closeApplied)
            return;
        closeApplied = true;

        const bool ACTIVATESELECTION = commit && overview()->activeScreen().get() == this;
        const auto MONITOR           = monitor.lock();
        const auto SELECTEDWORKSPACE = commit ? selectedWorkspace.lock() : (MONITOR ? MONITOR->m_activeWorkspace : PHLWORKSPACE{});
        if (!commit) {
            selection.reset();
            focusSyncedFromWorkspaceKey.clear();
        }

        setClosing(true);

        if (commit && !selection && MONITOR && (!SELECTEDWORKSPACE || SELECTEDWORKSPACE == MONITOR->m_activeWorkspace)) {
            const auto FOCUSED = shown(Desktop::focusState()->window());
            if (!SELECTEDWORKSPACE || (FOCUSED && FOCUSED->m_workspace == SELECTEDWORKSPACE))
                selection = FOCUSED;
        }
        selection = shown(selection.lock());

        // Land on the selection: switching to it makes hyprgrid's view glide to
        // its cell while the zoom goes back to the normal view.
        const auto FINALWINDOW    = shown(selection.lock());
        const auto FINALWORKSPACE = FINALWINDOW ? FINALWINDOW->m_workspace : SELECTEDWORKSPACE;
        if (MONITOR && FINALWORKSPACE && FINALWORKSPACE->m_monitor != MONITOR) {
            // Another monitor's workspace (a shared board): go to that
            // monitor, showing it there, rather than bring it over.
            (void)::Config::Actions::changeWorkspace(FINALWORKSPACE);
        } else if (MONITOR && FINALWORKSPACE && FINALWORKSPACE != MONITOR->m_activeWorkspace)
            changeWorkspace(MONITOR, FINALWORKSPACE);
        if (ACTIVATESELECTION && FINALWINDOW && FINALWINDOW != Desktop::focusState()->window())
            Desktop::focusState()->fullWindowFocus(FINALWINDOW, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
        focusSyncedFromWorkspaceKey.clear();

        const auto FS = fullscreenOf(FINALWORKSPACE);
        emitFullscreenState(FS ? FS : FINALWINDOW, false);
        if (!Overview::Config::getValue<int>("animations:enabled")) {
            recalcDecorations(FINALWORKSPACE ? FINALWORKSPACE : (MONITOR ? MONITOR->m_activeWorkspace : PHLWORKSPACE{}));
            damage();
        }

        // Gone once the zoom settles at 0 (finishClosing()).
        closeRemovalPending = true;
        if (MONITOR)
            View::zoomTo(MONITOR, 0.F);
    }

    void CScreen::reopen() {
        if (!closing)
            return;
        viewClosing(false);
        if (const auto MONITOR = monitor.lock())
            View::zoomTo(MONITOR, 1.F);
    }

    // The zoom heads back to 0 (a gesture let go, not a selection): stand
    // down like a close, landing nowhere; or it turned back up: open again.
    void CScreen::viewClosing(bool heading) {
        if (heading == closing)
            return;

        if (heading) {
            closeApplied        = true;
            closeRemovalPending = true;
            focusSyncedFromWorkspaceKey.clear();
            setClosing(true);
            const auto MONITOR = monitor.lock();
            const auto FS      = fullscreenOf(MONITOR ? MONITOR->m_activeWorkspace : PHLWORKSPACE{});
            emitFullscreenState(FS ? FS : shown(Desktop::focusState()->window()), false);
        } else {
            closeApplied        = false;
            closeRemovalPending = false;
            setClosing(false);
            emitFullscreenState(Desktop::focusState()->window(), true);
        }
        damage();
    }

    // The zoom settled at 0: the normal view takes over, unless a cancelled
    // drag's button is still down (its release removes the screen).
    void CScreen::finishClosing() {
        if (!overview()->swallowingPointer())
            overview()->remove(this);
    }
}
