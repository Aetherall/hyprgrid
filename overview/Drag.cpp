// Dragging, dropping and resizing windows in the overview (see Session.hpp).
// A drag starts on one screen and can end on another: the window lands on the
// workspace under the pointer there, next to the tiled window it is dropped
// on, or in an empty cell next to the grid, which gets a workspace for it.
// With cross_monitor_drag, a monitor not zoomed out opens a screen to drop
// onto, closed after unless it got the window; and a SUPER + drag Hyprland
// started carries on in the overview.

#include "Internal.hpp"
#include "Session.hpp"
#include "NativeDrag.hpp"

namespace Overview {
    static constexpr auto PSEUDO_FOCUS_DURATION = std::chrono::milliseconds(100);

    static bool boxesEqual(const CBox& a, const CBox& b) {
        return std::abs(a.x - b.x) < 0.5 && std::abs(a.y - b.y) < 0.5 && std::abs(a.width - b.width) < 0.5 && std::abs(a.height - b.height) < 0.5;
    }

    static void syncWorkspaceGeometry(const PHLWORKSPACE& workspace) {
        if (!workspace || !workspace->space())
            return;
        for (const auto& ref : workspace->space()->targets()) {
            if (const auto TARGET = ref.lock())
                TARGET->warpPositionSize();
        }
    }

    // Step a tiled window through the layout until it is on `direction`'s
    // side of `anchor` (the layout has no "put it next to that one").
    static void moveTargetNextTo(const SP<Layout::ITarget>& target, const PHLWINDOW& anchor, const std::string& direction) {
        if (!target || !anchor || direction.empty())
            return;

        // Past the layout's edge, a step must not carry it to another monitor.
        const auto PREVFALLBACK = Overview::Config::getValue<int>("binds:window_direction_monitor_fallback");
        Overview::Config::setValue("binds:window_direction_monitor_fallback", 0);
        auto restore = Hyprutils::Utils::CScopeGuard([PREVFALLBACK] { Overview::Config::setValue("binds:window_direction_monitor_fallback", PREVFALLBACK); });

        const bool NEGATIVE = direction == "l" || direction == "u";
        const auto BACKWARD = direction == "l" ? "r" : direction == "r" ? "l" : direction == "u" ? "d" : "u";
        const auto onSide   = [&] {
            if (!anchor->layoutTarget())
                return false;
            const auto T = target->position().middle(), A = anchor->layoutTarget()->position().middle();
            const auto D = direction == "l" || direction == "r" ? T.x - A.x : T.y - A.y;
            return NEGATIVE ? D < 0.0 : D > 0.0;
        };
        const auto step = [&](const std::string& dir) {
            const auto WORKSPACE = target->workspace();
            const auto BEFORE    = target->position();
            g_layoutManager->moveInDirection(target, dir, true);
            return target->workspace() == WORKSPACE && !boxesEqual(BEFORE, target->position());
        };

        for (size_t i = 0; i < 64 && onSide(); ++i) {
            if (!step(BACKWARD))
                break;
            if (!onSide()) {
                step(direction);
                return;
            }
        }
        for (size_t i = 0; i < 64 && !onSide(); ++i) {
            if (!step(direction))
                break;
        }
    }

    static void focusFullscreenIfActive(const PHLWINDOW& window_, const PHLWORKSPACE& workspace, const PHLMONITOR& monitor) {
        const auto WINDOW = shown(window_);
        if (!monitor || !workspace || workspace != monitor->m_activeWorkspace || !validMapped(WINDOW) || WINDOW->m_workspace != workspace || Desktop::focusState()->window() == WINDOW)
            return;
        Desktop::focusState()->fullWindowFocus(WINDOW, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE, nullptr, true);
    }

    PHLWINDOW COverview::dragged() const {
        return shown(drag.window.lock());
    }

    bool COverview::dragging() const {
        return !!dragged();
    }

    PHLWINDOW COverview::pseudoFocused(const Time::steady_tp& now) {
        if (now >= m_pseudoFocusUntil) {
            m_pseudoFocused.reset();
            m_pseudoFocusUntil = {};
            return nullptr;
        }
        const auto WINDOW = shown(m_pseudoFocused.lock());
        return isShown(WINDOW) ? WINDOW : nullptr;
    }

    // At the pointer, held where it was picked up, at the size the screen it
    // came from draws it.
    CBox COverview::draggedGlobalBox() const {
        const auto WINDOW  = dragged();
        const auto SOURCE  = drag.source.lock();
        const auto MONITOR = SOURCE ? SOURCE->monitor.lock() : PHLMONITOR{};
        if (!WINDOW || !MONITOR)
            return {};
        const auto IMAGE = SOURCE->imageOf(WINDOW->m_workspace) ? SOURCE->imageOf(WINDOW->m_workspace) : SOURCE->imageOf(SOURCE->selectedWorkspace.lock());
        if (!IMAGE)
            return {};
        const auto SIZE = SOURCE->dragBox(WINDOW, *IMAGE, false).size() * (1.F / std::max(MONITOR->m_scale, 0.01F));
        return toCBox(Interaction::heldAt(scenePoint(SIZE), scenePoint(g_pInputManager->getMouseCoordsInternal()), scenePoint(drag.grabRatio)), false);
    }

    bool COverview::beginDrag(CScreen& screen_, const Vector2D& at) {
        const auto SCREEN = find(&screen_);
        const auto WINDOW = shown(screen_.windowAt(at));
        const auto TARGET = WINDOW ? WINDOW->layoutTarget() : nullptr;
        if (!SCREEN || !isShown(WINDOW) || !TARGET)
            return false;

        const auto IMAGE = SCREEN->imageOf(WINDOW->m_workspace);
        if (IMAGE)
            SCREEN->selectedWorkspace = WINDOW->m_workspace;

        clearDrag();
        drag.window            = WINDOW;
        drag.originalWorkspace = WINDOW->m_workspace;
        drag.originalBox       = TARGET->position();
        drag.originalVisualBox = WINDOW->grouping().group() ? TARGET->position() : WINDOW->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        drag.originalFloatSize = TARGET->lastFloatingSize();
        drag.startedTiled      = !TARGET->floating();
        drag.crossMonitor      = Overview::Config::getCrossMonitorDrag();
        drag.source            = SCREEN;
        if (IMAGE)
            drag.grabRatio = toVector(Interaction::grabRatio(sceneBox(SCREEN->dragBox(WINDOW, *IMAGE)), scenePoint(at)));

        m_pseudoFocused.reset();
        m_pseudoFocusUntil = {};
        SCREEN->selection  = WINDOW;
        SCREEN->rememberSelection(WINDOW);

        updateDrag();
        return dragging();
    }

    // SUPER + drag passed its threshold: the overview takes it from Hyprland.
    bool COverview::adoptNativeDrag(CScreen& screen_, const PHLWINDOW& expected) {
        const auto SCREEN = find(&screen_);
        if (!SCREEN || dragging() || m_finishingAdoptedDrag || !g_layoutManager || !g_pInputManager)
            return false;

        const auto& CONTROLLER = g_layoutManager->dragController();
        if (!CONTROLLER || CONTROLLER->mode() != MBIND_MOVE || !CONTROLLER->dragThresholdReached())
            return false;
        const auto TARGET = CONTROLLER->target();
        const auto WINDOW = TARGET ? shown(TARGET->window()) : PHLWINDOW{};
        if (!isShown(WINDOW) || !WINDOW->layoutTarget() || (expected && WINDOW != shown(expected)))
            return false;

        const auto BOX          = TARGET->position();
        const auto VISUAL       = WINDOW->grouping().group() ? BOX : WINDOW->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        const auto FLOATSIZE    = TARGET->lastFloatingSize();
        const bool STARTEDTILED = CONTROLLER->draggingTiled();

        m_finishingAdoptedDrag = true;
        auto restore           = Hyprutils::Utils::CScopeGuard([this] { m_finishingAdoptedDrag = false; });
        finishNativeDragAdoption(CONTROLLER.get());
        g_layoutManager->endDragTarget();
        if (!isShown(WINDOW) || !WINDOW->layoutTarget())
            return false;

        clearDrag();
        drag.window            = WINDOW;
        drag.originalWorkspace = WINDOW->m_workspace;
        drag.originalBox       = BOX;
        drag.originalVisualBox = VISUAL;
        drag.originalFloatSize = FLOATSIZE;
        drag.startedTiled      = STARTEDTILED;
        drag.crossMonitor      = true;
        drag.source            = SCREEN;
        drag.grabRatio         = toVector(Interaction::grabRatio(sceneBox(VISUAL), scenePoint(g_pInputManager->getMouseCoordsInternal())));

        m_pseudoFocused.reset();
        m_pseudoFocusUntil = {};
        SCREEN->selection  = WINDOW;
        SCREEN->rememberSelection(WINDOW);

        m_pointer.adopt();
        SCREEN->updatePointer();
        m_grab = SCREEN;
        SCREEN->requestInputFrame();
        SCREEN->damage();
        return true;
    }

    // With cross_monitor_drag, over a monitor not zoomed out: a screen there.
    void COverview::ensureDropScreen() {
        const auto SOURCE = drag.source.lock();
        if (!dragging() || !drag.crossMonitor || !g_pInputManager)
            return;

        const auto CURSOR  = g_pInputManager->getMouseCoordsInternal();
        const auto MONITOR = State::monitorState()->query().vec(CURSOR).run();
        if (!MONITOR || !MONITOR->m_enabled || !MONITOR->logicalBox().containsPoint(CURSOR) || (SOURCE && MONITOR == SOURCE->monitor.lock()))
            return;

        const auto untransient = [this](const SP<CScreen>& screen) { std::erase_if(drag.transients, [&](const auto& ref) { return ref.lock() == screen; }); };
        if (const auto EXISTING = screenFor(MONITOR)) {
            if (EXISTING->isClosing()) {
                EXISTING->reopen();
                untransient(EXISTING);
            }
            return;
        }

        bool       created = false;
        const auto SCREEN  = open(MONITOR, true, &created);
        if (!SCREEN)
            return;
        if (created && SCREEN != SOURCE)
            drag.transients.emplace_back(SCREEN);
        else if (SCREEN->isClosing()) {
            SCREEN->reopen();
            untransient(SCREEN);
        }
        SCREEN->requestInputFrame();
        SCREEN->damage();
    }

    void COverview::updateDrag() {
        if (!drag.window)
            return;
        const auto WINDOW = dragged();
        if (!isShown(WINDOW) || !WINDOW->layoutTarget()) {
            cancelDrag();
            return;
        }

        ensureDropScreen();
        for (const auto& screen : m_screens) {
            screen->requestInputFrame();
            screen->damage();
        }
    }

    void COverview::clearDrag() {
        auto transients = std::move(drag.transients);
        drag            = SDrag{};
        drag.transients = std::move(transients);
    }

    // The drag is over (dropped on `destination`, or not): screens opened for
    // it close, except the one that got the window, which now closes with the
    // one it came from.
    void COverview::finishDrag(const SP<CScreen>& destination) {
        const auto SOURCE     = drag.source.lock();
        auto       transients = std::move(drag.transients);
        drag.transients.clear();
        clearDrag();

        if (destination && std::ranges::any_of(transients, [&](const auto& ref) { return ref.lock() == destination; }) && SOURCE && SOURCE != destination) {
            std::erase_if(m_linked, [this](const auto& ref) { return !find(ref.lock().get()); });
            if (std::ranges::none_of(m_linked, [&](const auto& ref) { return ref.lock() == SOURCE; }))
                m_linked = {SOURCE};
            if (std::ranges::none_of(m_linked, [&](const auto& ref) { return ref.lock() == destination; }))
                m_linked.emplace_back(destination);
        }

        for (const auto& ref : transients) {
            const auto SCREEN = ref.lock();
            if (SCREEN && SCREEN != destination && SCREEN != SOURCE && find(SCREEN.get()))
                SCREEN->dismissTransient();
        }

        for (const auto& screen : m_screens) {
            screen->requestInputFrame();
            screen->damage();
        }
    }

    void COverview::cancelDrag() {
        if (!drag.window && drag.transients.empty())
            return;
        m_pointer.cancelled(!!m_grab.lock());
        finishDrag();
    }

    void COverview::endDrag() {
        SP<CScreen> destination;
        auto        finish = Hyprutils::Utils::CScopeGuard([this, &destination] { finishDrag(destination); });

        ensureDropScreen();

        const auto WINDOW = dragged();
        const auto TARGET = WINDOW ? WINDOW->layoutTarget() : nullptr;
        const auto SPACE  = TARGET ? TARGET->space() : nullptr;
        const auto ALGO   = SPACE ? SPACE->algorithm() : nullptr;
        const auto SOURCE = drag.source.lock();
        const auto DROP   = screenAt(g_pInputManager->getMouseCoordsInternal());
        if (!WINDOW || !TARGET || !SOURCE || !DROP)
            return;

        DROP->updatePointer();
        const auto DROPMONITOR = DROP->monitor.lock();
        const auto DROPPOINT   = DROP->pointerLocal;
        const bool RETILE      = drag.startedTiled && SPACE && ALGO;

        auto dropWorkspace = DROP->dropWorkspaceAt(DROPPOINT, WINDOW);
        // Onto an empty cell next to the grid: create the workspace there first.
        if (!dropWorkspace) {
            if (const auto SLOT = DROP->slotAt(DROPPOINT))
                dropWorkspace = DROP->createSlotWorkspace(*SLOT);
        }
        const auto DROPWORKSPACE = dropWorkspace;
        const auto DROPIMAGE     = DROP->imageOf(DROPWORKSPACE);
        if (!DROPWORKSPACE || !DROPIMAGE)
            return;

        const auto ORIGINAL = drag.originalWorkspace.lock();
        const bool MOVE     = DROPWORKSPACE != ORIGINAL;
        const auto DRAGBOX  = toCBox(Interaction::heldAt(scenePoint(DROP->dragBox(WINDOW, *DROPIMAGE).size()), scenePoint(DROPPOINT), scenePoint(drag.grabRatio)), false);
        if (DROP != SOURCE)
            destination = DROP;

        // A floating window dragged off a fullscreen one: that one gets focus back.
        const auto SOURCEFS          = fullscreenOf(ORIGINAL);
        const bool RESTORESOURCEFOCUS = WINDOW->isFloating() && MOVE && SOURCEFS && SOURCEFS != WINDOW && Fullscreen::controller()->isFullscreen(SOURCEFS);

        // Moving a window switches workspace; the monitors stay where they were.
        const auto MONITOR          = SOURCE->monitor.lock();
        const auto ACTIVEBEFORE     = MONITOR ? MONITOR->m_activeWorkspace : PHLWORKSPACE{};
        const auto DROPACTIVEBEFORE = DROPMONITOR ? DROPMONITOR->m_activeWorkspace : PHLWORKSPACE{};
        const auto restoreActive    = [&] {
            if (MONITOR && ACTIVEBEFORE && MONITOR->m_activeWorkspace != ACTIVEBEFORE)
                MONITOR->changeWorkspace(ACTIVEBEFORE, false, true, true);
            if (DROPMONITOR && DROPMONITOR != MONITOR && DROPACTIVEBEFORE && DROPMONITOR->m_activeWorkspace != DROPACTIVEBEFORE)
                DROPMONITOR->changeWorkspace(DROPACTIVEBEFORE, false, true, true);
        };
        const auto restoreSelected = [&] {
            if (ACTIVEBEFORE && SOURCE->imageOf(ACTIVEBEFORE))
                SOURCE->selectedWorkspace = ACTIVEBEFORE;
        };

        const bool FULLYVISIBLE = fullyOnMonitor(DROP->workspaceBox(*DROPIMAGE), DROPMONITOR);
        const auto ANCHOR       = DROP->dropAnchorAt(*DROPIMAGE, WINDOW);
        // Where on the drop screen the dropped box is, in global layout space.
        const auto DROPSCALE = std::max(DROP->zoomScale(), 0.01F) * std::max(DROPMONITOR ? DROPMONITOR->m_scale : 1.F, 0.01F);
        const auto droppedBox = [&] {
            const auto IMAGE = DROP->imageOf(DROPWORKSPACE);
            return CBox{IMAGE ? DROP->toGlobal(*IMAGE, DRAGBOX.pos()) : DRAGBOX.pos(), DRAGBOX.size() * (1.F / DROPSCALE)};
        };

        if (RETILE && MOVE) {
            // Tiled, onto another workspace: its layout places it.
            Desktop::globalWindowController()->moveWindowToWorkspace(WINDOW, DROPWORKSPACE);
            restoreActive();
            TARGET->rememberFloatingSize(drag.originalFloatSize);
            restoreActive();
            restoreSelected();
        } else if (RETILE) {
            // Tiled, within its workspace: swapped with the window it is
            // dropped on, or moved next to it.
            TARGET->damageEntire();
            if (ANCHOR.window && !ANCHOR.direction.empty() && ANCHOR.window->layoutTarget()) {
                ANCHOR.window->layoutTarget()->damageEntire();
                g_layoutManager->switchTargets(TARGET, ANCHOR.window->layoutTarget(), true);
                ANCHOR.window->layoutTarget()->damageEntire();
            } else if (ANCHOR.window && !ANCHOR.direction.empty())
                moveTargetNextTo(TARGET, ANCHOR.window, ANCHOR.direction);

            TARGET->rememberFloatingSize(drag.originalFloatSize);
            TARGET->warpPositionSize();
            TARGET->damageEntire();
            Desktop::focusState()->fullWindowFocus(WINDOW, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
            if (const auto WORKSPACE = SPACE->workspace())
                WORKSPACE->updateWindows();
        } else if (MOVE) {
            // Floating, onto another workspace: where it is dropped, if that
            // workspace is all on screen; else in its middle.
            Desktop::globalWindowController()->moveWindowToWorkspace(WINDOW, DROPWORKSPACE);
            restoreActive();
            const auto DROPPED = droppedBox();
            auto       box     = FULLYVISIBLE ? DROPPED : centreInWorkspace(CBox{Vector2D{}, DROPPED.size()}, DROPWORKSPACE, DROPMONITOR);
            if (FULLYVISIBLE)
                box = clampToWorkspace(box, DROPWORKSPACE, DROPMONITOR, WINDOW->presentation().borderSize());
            TARGET->setPositionGlobal(box);
            TARGET->warpPositionSize();
            restoreActive();
            restoreSelected();
        } else if (!drag.startedTiled) {
            // Floating, within its workspace: where it is dropped.
            const auto BOX = clampToWorkspace(droppedBox(), DROPWORKSPACE, DROPMONITOR, WINDOW->presentation().borderSize());
            TARGET->damageEntire();
            TARGET->setPositionGlobal(BOX);
            TARGET->warpPositionSize();
            TARGET->damageEntire();
        }

        if (RESTORESOURCEFOCUS && MONITOR && ORIGINAL == MONITOR->m_activeWorkspace) {
            SOURCE->selection = SOURCEFS;
            SOURCE->rememberSelection(SOURCEFS);
            focusFullscreenIfActive(SOURCEFS, ORIGINAL, MONITOR);
            SOURCE->emitFullscreenState(SOURCEFS, true);
        }

        if (DROPMONITOR && DROPWORKSPACE == DROPMONITOR->m_activeWorkspace) {
            if (const auto FS = fullscreenOf(DROPWORKSPACE))
                DROP->emitFullscreenState(FS, true);
            DROP->select(WINDOW, true);
            m_pseudoFocused    = WINDOW;
            m_pseudoFocusUntil = Time::steadyNow() + PSEUDO_FOCUS_DURATION;
        }

        SOURCE->markDirty();
        if (DROP != SOURCE)
            DROP->markDirty();
    }

    // --- Resizing: the resize button drags a window's nearest corner ---

    SP<CScreen> COverview::resizeScreen() const {
        return resize.screen.lock();
    }

    bool COverview::armResize(CScreen& screen_, const Vector2D& at) {
        const auto   SCREEN = find(&screen_);
        PHLWORKSPACE workspace;
        const auto   WINDOW = SCREEN ? SCREEN->windowAt(at, &workspace) : PHLWINDOW{};
        const auto   IMAGE  = SCREEN ? SCREEN->imageOf(workspace) : nullptr;
        resize.pending.reset();
        if (!isShown(WINDOW) || !IMAGE)
            return false;

        resize.start     = at;
        resize.pending   = WINDOW;
        resize.screen    = SCREEN;
        resize.workspace = workspace;
        resize.corner    = toRectCorner(Interaction::cornerAt(sceneBox(SCREEN->windowBox(WINDOW, *IMAGE)), scenePoint(at)));
        return true;
    }

    bool COverview::beginResize() {
        const auto SCREEN = resizeScreen();
        const auto WINDOW = shown(resize.pending.lock());
        const auto IMAGE  = SCREEN && WINDOW ? SCREEN->imageOf(WINDOW->m_workspace) : nullptr;
        if (!isShown(WINDOW) || !WINDOW->layoutTarget() || !IMAGE)
            return false;

        SCREEN->selection = WINDOW;
        SCREEN->rememberSelection(WINDOW);
        SCREEN->selectedWorkspace = WINDOW->m_workspace;
        resize.workspace          = WINDOW->m_workspace;
        resize.originalBox        = SCREEN->windowBox(WINDOW, *IMAGE);
        resize.active             = WINDOW;
        resize.last               = SCREEN->pointerLocal;
        updateResize();
        return true;
    }

    // A floating window's box on screen, its corner at the pointer, within
    // its size limits and its workspace.
    CBox COverview::resizedBox() const {
        const auto SCREEN  = resizeScreen();
        const auto WINDOW  = shown(resize.active.lock());
        const auto MONITOR = SCREEN ? SCREEN->monitor.lock() : PHLMONITOR{};
        const auto IMAGE   = SCREEN ? SCREEN->imageOf(resize.workspace.lock()) : nullptr;
        if (!WINDOW || !MONITOR || !IMAGE || !WINDOW->isFloating())
            return {};

        const float SCALE  = std::max(SCREEN->zoomScale(), 0.01F) * std::max(MONITOR->m_scale, 0.01F);
        const auto  TARGET = WINDOW->layoutTarget();

        Vector2D minSize = {1.F, 1.F};
        if (const auto MIN = TARGET ? TARGET->minSize() : std::nullopt)
            minSize = {std::max(1.F, sc<float>(MIN->x * SCALE)), std::max(1.F, sc<float>(MIN->y * SCALE))};
        std::optional<Vector2D> maxSize;
        if (const auto MAX = TARGET ? TARGET->maxSize() : std::nullopt; MAX && MAX->x > 0.F && MAX->y > 0.F)
            maxSize = Vector2D{std::max(minSize.x, sc<double>(MAX->x * SCALE)), std::max(minSize.y, sc<double>(MAX->y * SCALE))};

        const auto CORNER = sceneCorner(resize.corner);
        const auto BOX    = Interaction::resize(sceneBox(resize.originalBox), scenePoint(SCREEN->pointerLocal - resize.start), CORNER, scenePoint(minSize),
                                                maxSize ? std::optional{scenePoint(*maxSize)} : std::nullopt);
        const auto MARGIN = WINDOW->presentation().borderSize() * MONITOR->m_scale * SCREEN->zoomScale();
        return toCBox(Interaction::clampResize(BOX, sceneBox(SCREEN->workspaceBox(*IMAGE)), CORNER, MARGIN), false);
    }

    void COverview::updateResize() {
        const auto SCREEN  = resizeScreen();
        const auto WINDOW  = shown(resize.active.lock());
        const auto TARGET  = WINDOW ? WINDOW->layoutTarget() : nullptr;
        const auto MONITOR = SCREEN ? SCREEN->monitor.lock() : PHLMONITOR{};
        const auto IMAGE   = SCREEN ? SCREEN->imageOf(resize.workspace.lock()) : nullptr;
        if (!TARGET || !MONITOR || !IMAGE)
            return;

        const auto SCALE = std::max(SCREEN->zoomScale(), 0.01F) * std::max(MONITOR->m_scale, 0.01F);
        TARGET->damageEntire();
        if (WINDOW->isFloating()) {
            const auto BOX    = resizedBox();
            const auto GLOBAL = CBox{SCREEN->toGlobal(*IMAGE, BOX.pos()), BOX.size() * (1.F / SCALE)};
            TARGET->rememberFloatingSize(GLOBAL.size());
            TARGET->setPositionGlobal(GLOBAL);
            TARGET->warpPositionSize();
        } else {
            // Tiled: the layout resizes it by the pointer's travel.
            const auto DELTA = (SCREEN->pointerLocal - resize.last) * (1.F / SCALE);
            if (std::abs(DELTA.x) > 0.01F || std::abs(DELTA.y) > 0.01F) {
                g_layoutManager->resizeTarget(DELTA, TARGET, resize.corner);
                syncWorkspaceGeometry(TARGET->workspace());
                resize.last = SCREEN->pointerLocal;
            }
        }
        TARGET->damageEntire();
        SCREEN->damage();
    }

    void COverview::endResize() {
        const auto SCREEN  = resizeScreen();
        const auto WINDOW  = shown(resize.active.lock());
        const auto TARGET  = WINDOW ? WINDOW->layoutTarget() : nullptr;
        const auto MONITOR = SCREEN ? SCREEN->monitor.lock() : PHLMONITOR{};
        const auto IMAGE   = SCREEN ? SCREEN->imageOf(resize.workspace.lock()) : nullptr;

        if (TARGET && IMAGE && WINDOW->isFloating()) {
            const auto SCALE  = std::max(SCREEN->zoomScale(), 0.01F) * std::max(MONITOR ? MONITOR->m_scale : 1.F, 0.01F);
            const auto BOX    = resizedBox();
            const auto GLOBAL = CBox{SCREEN->toGlobal(*IMAGE, BOX.pos()), BOX.size() * (1.F / SCALE)};
            TARGET->damageEntire();
            TARGET->rememberFloatingSize(GLOBAL.size());
            TARGET->setPositionGlobal(GLOBAL);
            TARGET->warpPositionSize();
            TARGET->damageEntire();
        }

        resize = SResize{};
        if (SCREEN)
            SCREEN->markDirty();
    }
}
