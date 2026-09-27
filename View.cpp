// The view (see View.hpp). Per monitor, two animated values (x, y, in cells)
// on kinetic springs, or the hold's position while a gesture has it, and the
// zoom (0 normal, 1 overview) on another. Hyprland still performs every switch
// (focus, visibility); its own slide is overridden at once, the view starting
// from wherever it is, so any switch (keybind, app, mid-animation) carries on
// smoothly.
//
// Zoomed in (no overview on the monitor), whenever the view is off its cell,
// every grid workspace on that monitor sits at (cell - view) * (monitor size
// + gaps_workspaces) and the ones in view are force-rendered: a diagonal
// shows all four cells around the corner. Zoomed out, the overview
// (overview/) draws the workspaces from the view instead.

#include "Kinetics.hpp" // first: see its header
#include "View.hpp"
#include "Grid.hpp"
#include "overview/Overview.hpp"

#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>
#include <hyprland/src/output/Monitor.hpp>

#include <map>

using Motion::SCell;
using Motion::SPoint;

namespace {
    // A gesture holding the view.
    struct SHold {
        bool                     active = false;
        SPoint                   pos;    // in cells
        SCell                    anchor; // the active workspace's cell
        SPoint                   last;   // the latest move, for eLand::AHEAD
        Motion::CVelocityTracker vx, vy;
    };

    // A gesture holding the zoom.
    struct SZoomHold {
        bool                     active = false;
        float                    start  = 0; // where it was taken
        float                    last   = 0; // the last movement
        Motion::CVelocityTracker v;
    };

    struct SView {
        PHLANIMVAR<float> x, y; // created on the first glide or zoom
        PHLANIMVAR<float> zoom;
        SHold             hold;
        SZoomHold         zoomHold;
    };

    std::map<MONITORID, SView>           g_views;
    std::map<MONITORID, PHLWORKSPACEREF> g_lastActive;

    bool                    g_applying = false;
    std::optional<Vector2D> g_pendingVelocity; // cells/s, for the next switch
    CHyprSignalListener     g_activeListener;

    // The cells a view on `mon` may go to: on its board.
    Motion::CanEnter canEnter(const PHLMONITOR& mon) {
        return [mon = PHLMONITORREF{mon}](const SCell& cell) { return Grid::enterable(mon.lock(), cell); };
    }

    Vector2D pitchOf(const PHLMONITOR& mon) {
        static auto PGAP = CConfigValue<Config::INTEGER>("general:gaps_workspaces");
        return mon->m_size + Vector2D{double(*PGAP), double(*PGAP)};
    }

    SView* viewOf(const PHLMONITOR& mon) {
        const auto IT = mon ? g_views.find(mon->m_id) : g_views.end();
        return IT == g_views.end() ? nullptr : &IT->second;
    }

    bool holding(const PHLMONITOR& mon) {
        const auto V = viewOf(mon);
        return V && V->hold.active;
    }

    bool gliding(const PHLMONITOR& mon) {
        const auto V = viewOf(mon);
        return V && V->x && (V->x->isBeingAnimated() || V->y->isBeingAnimated());
    }

    float zoomOf(const PHLMONITOR& mon) {
        const auto V = viewOf(mon);
        return V && V->zoom ? V->zoom->value() : 0.F;
    }

    // The cells of the grid workspaces on a monitor: what a zoomed-out view
    // can see and land on.
    std::vector<SCell> cellsOn(const PHLMONITOR& mon) {
        std::vector<SCell> cells;
        for (const auto& ws : State::Workspace::state()->workspacesCopy()) {
            if (ws && ws->m_monitor.lock() == mon) {
                if (const auto CELL = Grid::cellOf(ws))
                    cells.push_back(*CELL);
            }
        }
        return cells;
    }

    // Place the monitor's grid workspaces around its view, or have the
    // overview redraw from it.
    void apply(const PHLMONITOR& mon) {
        if (g_applying)
            return;
        const auto V = viewOf(mon);
        if (!V)
            return;
        if (Overview::drawing(mon)) {
            Overview::sync(mon);
            return;
        }
        Vector2D C;
        bool     moving;
        if (V->hold.active) {
            C      = {V->hold.pos.x, V->hold.pos.y};
            moving = true;
        } else {
            if (!V->x)
                return;
            C      = {V->x->value(), V->y->value()};
            moving = V->x->value() != V->x->goal() || V->y->value() != V->y->goal();
        }
        const auto PITCH = pitchOf(mon);

        g_applying = true;
        for (const auto& ws : State::Workspace::state()->workspacesCopy()) {
            if (!ws || ws->m_monitor.lock() != mon)
                continue;
            const auto CELL = Grid::cellOf(ws);
            if (!CELL)
                continue;
            const Vector2D D       = {CELL->x - C.x, CELL->y - C.y};
            const bool     VISIBLE = std::abs(D.x) < 1 && std::abs(D.y) < 1;
            const bool     ACTIVE  = ws == mon->m_activeWorkspace;
            ws->m_renderOffset->setValueAndWarp(!moving && ACTIVE ? Vector2D{} : Vector2D{D.x * PITCH.x, D.y * PITCH.y});
            if (VISIBLE)
                ws->m_alpha->setValueAndWarp(1.F);
            ws->m_forceRendering = moving && VISIBLE && !ACTIVE;
        }
        g_applying = false;
        g_pHyprRenderer->damageMonitor(mon);
    }

    // The monitor's view, with its animated values (the position starting on
    // the active cell).
    SView& animated(const PHLMONITOR& mon) {
        auto& view = g_views[mon->m_id];
        if (!view.x) {
            const auto CB = [mon = PHLMONITORREF{mon}](auto) {
                if (const auto M = mon.lock())
                    apply(M);
            };
            const auto CELL = Grid::cellOf(mon->m_activeWorkspace).value_or(SCell{});
            Animation::mgr()->createAnimation(float(CELL.x), view.x, Kinetics::configFor("kinetic.grid.x"), AVARDAMAGE_NONE);
            Animation::mgr()->createAnimation(float(CELL.y), view.y, Kinetics::configFor("kinetic.grid.y"), AVARDAMAGE_NONE);
            view.x->setUpdateCallback(CB);
            view.y->setUpdateCallback(CB);

            // The overview comes and goes with the zoom.
            const auto ZOOMED = [mon = PHLMONITORREF{mon}](auto) {
                const auto M = mon.lock();
                const auto V = M ? viewOf(M) : nullptr;
                if (!V || !V->zoom)
                    return;
                Overview::zoomChanged(M, V->zoom->value(), V->zoom->goal());
                apply(M);
            };
            Animation::mgr()->createAnimation(0.F, view.zoom, Kinetics::configFor("kinetic.grid.zoom"), AVARDAMAGE_NONE);
            view.zoom->setUpdateCallback(ZOOMED);
            view.zoom->setCallbackOnEnd(ZOOMED, false);
        }
        return view;
    }

    void launchAxis(const PHLANIMVAR<float>& var, const std::string& name, float velocity) {
        const float REMAINING = var->goal() - var->value();
        if (std::abs(REMAINING) > 1e-4F)
            Kinetics::launch(var, name, velocity / REMAINING);
    }

    // Warp the view to `at` (cells) without placing workspaces yet.
    void warpTo(const PHLMONITOR& mon, const Vector2D& at) {
        auto& view = animated(mon);
        g_applying = true;
        view.x->setValueAndWarp(at.x);
        view.y->setValueAndWarp(at.y);
        g_applying = false;
    }

    // Animate the view from `from` to `to` (cells), carrying on at `velocity`
    // (cells/s).
    void glide(const PHLMONITOR& mon, const Vector2D& from, const SCell& to, const Vector2D& velocity) {
        auto& view = animated(mon);
        warpTo(mon, from);
        g_applying = true;
        *view.x    = float(to.x);
        *view.y    = float(to.y);
        g_applying = false;
        launchAxis(view.x, "kinetic.grid.x", velocity.x);
        launchAxis(view.y, "kinetic.grid.y", velocity.y);
        apply(mon);
    }

    // Hyprland just switched `ws` in on its monitor (and started its own
    // slide): replace that with the view moving from where it is now.
    void onActive(PHLWORKSPACE ws) {
        const auto MON = ws ? ws->m_monitor.lock() : nullptr;
        if (!MON || ws->type() == Workspace::eWorkspaceType::SPECIAL)
            return;
        auto&      last = g_lastActive[MON->m_id];
        const auto OLD  = last.lock();
        last            = ws;

        const auto CO = Grid::cellOf(OLD), CN = Grid::cellOf(ws);
        if (Overview::drawing(MON) && CN && !holding(MON)) {
            // Zoomed out: the view moves to the new cell from wherever it is.
            const auto&    VIEW     = animated(MON);
            const Vector2D FROM     = {VIEW.x->value(), VIEW.y->value()};
            const Vector2D VELOCITY = g_pendingVelocity.value_or(Vector2D{Kinetics::velocityOf(VIEW.x), Kinetics::velocityOf(VIEW.y)});
            g_pendingVelocity.reset();
            glide(MON, FROM, *CN, VELOCITY);
            return;
        }
        if (holding(MON) && CN) {
            // The fingers reached this cell: the hold carries on from it.
            viewOf(MON)->hold.anchor = *CN;
            apply(MON);
            return;
        }
        if (!OLD || OLD == ws || OLD->m_monitor.lock() != MON || !CO || !CN) {
            g_pendingVelocity.reset();
            return;
        }

        // The old workspace hasn't moved yet (Hyprland only set its goal): its
        // offset says where the view is, mid-glide or mid-hold alike.
        const auto     PITCH  = pitchOf(MON);
        const auto     OLDOFF = OLD->m_renderOffset->value();
        const Vector2D FROM   = {CO->x - OLDOFF.x / PITCH.x, CO->y - OLDOFF.y / PITCH.y};
        const auto&    VIEW   = animated(MON);
        const Vector2D VELOCITY = g_pendingVelocity.value_or(Vector2D{Kinetics::velocityOf(VIEW.x), Kinetics::velocityOf(VIEW.y)});
        g_pendingVelocity.reset();
        glide(MON, FROM, *CN, VELOCITY);
    }

    void startHold(const PHLMONITOR& mon, const SPoint& pos, const SCell& anchor) {
        auto& hold  = g_views[mon->m_id].hold;
        hold.active = true; // first: freezing the glide below must not re-place it
        hold.pos    = pos;
        hold.anchor = anchor;
        hold.last   = {};
        hold.vx.reset();
        hold.vy.reset();
        if (const auto V = viewOf(mon); V && V->x) {
            V->x->setValueAndWarp(pos.x);
            V->y->setValueAndWarp(pos.y);
        }
        apply(mon);
    }
}

bool View::holdToward(const PHLMONITOR& monitor, int dx, int dy) {
    const auto CELL = Grid::cellOf(monitor ? monitor->m_activeWorkspace : nullptr);
    if (!CELL || !Grid::enterable(monitor, {CELL->x + dx, CELL->y + dy}))
        return false;
    SPoint pos = {double(CELL->x), double(CELL->y)};
    if (gliding(monitor)) {
        const auto V = viewOf(monitor);
        pos          = {V->x->value(), V->y->value()};
    }
    startHold(monitor, pos, *CELL);
    return true;
}

bool View::holdHere(const PHLMONITOR& monitor) {
    const auto CELL = Grid::cellOf(monitor ? monitor->m_activeWorkspace : nullptr);
    if (!CELL || holding(monitor))
        return false;
    SPoint pos = {double(CELL->x), double(CELL->y)};
    if (gliding(monitor)) {
        const auto V = viewOf(monitor);
        pos          = {V->x->value(), V->y->value()};
    }
    startHold(monitor, pos, *CELL);
    return true;
}

std::optional<SPoint> View::grab(const PHLMONITOR& monitor) {
    const auto CELL = Grid::cellOf(monitor ? monitor->m_activeWorkspace : nullptr);
    if (!CELL || !gliding(monitor))
        return std::nullopt;
    const auto   V   = viewOf(monitor);
    const SPoint POS = {V->x->value(), V->y->value()};
    startHold(monitor, POS, *CELL);
    return SPoint{POS.x - CELL->x, POS.y - CELL->y};
}

bool View::held(const PHLMONITOR& monitor) {
    return holding(monitor);
}

std::optional<View::SMove> View::moveHeld(const PHLMONITOR& monitor, const SPoint& delta, uint32_t timeMs) {
    if (!holding(monitor))
        return std::nullopt;
    auto&        hold   = viewOf(monitor)->hold;
    const auto   A      = hold.anchor;
    // Zoomed out, it pans over all the workspaces in sight and switches on
    // release only; zoomed in, it steps from cell to cell.
    const bool   ZOOMED = zoomOf(monitor) > 0;
    const SPoint OFFSET = Motion::offsetWithin(hold.pos, delta, A, ZOOMED ? Motion::boundsOver(cellsOn(monitor), A) : Motion::boundsAround(A, canEnter(monitor)));
    hold.pos            = {A.x + OFFSET.x, A.y + OFFSET.y};
    if (delta.x != 0 || delta.y != 0)
        hold.last = delta;
    hold.vx.add(timeMs, hold.pos.x);
    hold.vy.add(timeMs, hold.pos.y);
    apply(monitor);

    SMove result = {OFFSET, std::nullopt, {}};
    if (ZOOMED)
        return result;
    if (const auto CELL = Motion::reached(OFFSET, A, canEnter(monitor))) {
        result.land = Grid::idFor(Grid::boardOf(monitor), *CELL);
        result.step = {CELL->x - A.x, CELL->y - A.y};
    }
    return result;
}

std::optional<View::SRelease> View::release(const PHLMONITOR& monitor, uint32_t timeMs, eLand land) {
    if (!holding(monitor))
        return std::nullopt;
    auto&        hold   = viewOf(monitor)->hold;
    const auto   A      = hold.anchor;
    const auto   POS    = hold.pos;
    const bool   ZOOMED = zoomOf(monitor) > 0;
    const SPoint V      = land == eLand::STAY ? SPoint{} : SPoint{hold.vx.velocity(timeMs), hold.vy.velocity(timeMs)}; // cells/ms

    SCell TARGET = A;
    if (land == eLand::FLICK)
        TARGET = ZOOMED ? Motion::nearestLanding(POS, V, cellsOn(monitor), A) : Motion::landing(POS, V, A, canEnter(monitor));
    else if (land == eLand::AHEAD) {
        // Zoomed in, only as far as a neighbour.
        auto cells = ZOOMED ? cellsOn(monitor) : std::vector<SCell>{A};
        if (!ZOOMED) {
            for (const auto& n : {SCell{A.x + 1, A.y}, SCell{A.x - 1, A.y}, SCell{A.x, A.y + 1}, SCell{A.x, A.y - 1}}) {
                if (Grid::enterable(monitor, n))
                    cells.push_back(n);
            }
        }
        TARGET = Motion::aheadLanding(POS, hold.last, cells, A);
    }
    hold.active        = false;

    const Vector2D VELOCITY = Vector2D{V.x, V.y} * 1000.0; // cells/s
    if (TARGET == A) {
        glide(monitor, {POS.x, POS.y}, A, VELOCITY);
        return std::nullopt;
    }
    // The switch hands over to onActive(), which glides from here.
    warpTo(monitor, {POS.x, POS.y});
    g_pendingVelocity = VELOCITY;
    return SRelease{Grid::idFor(Grid::boardOf(monitor), TARGET), {TARGET.x - A.x, TARGET.y - A.y}};
}

void View::gridChanged() {
    for (const auto& mon : State::monitorState()->monitors()) {
        const auto V    = viewOf(mon);
        const auto CELL = Grid::cellOf(mon ? mon->m_activeWorkspace : nullptr);
        if (!V || !CELL)
            continue;
        // Where the view is headed is the active workspace's cell (or, held,
        // its anchor); if that cell moved, move the view with it.
        const SPoint FROM  = V->hold.active ? SPoint{double(V->hold.anchor.x), double(V->hold.anchor.y)} :
            V->x                            ? SPoint{V->x->goal(), V->y->goal()} :
                                              SPoint{double(CELL->x), double(CELL->y)};
        const SPoint SHIFT = {CELL->x - FROM.x, CELL->y - FROM.y};
        if (SHIFT.x == 0 && SHIFT.y == 0)
            continue;
        if (V->hold.active) {
            V->hold.anchor = *CELL;
            V->hold.pos    = {V->hold.pos.x + SHIFT.x, V->hold.pos.y + SHIFT.y};
            V->hold.vx.reset();
            V->hold.vy.reset();
        }
        if (V->x) {
            const bool     MOVING   = V->x->isBeingAnimated() || V->y->isBeingAnimated();
            const Vector2D VELOCITY = {Kinetics::velocityOf(V->x), Kinetics::velocityOf(V->y)}; // before the warp stops it
            const auto     VX = V->x->value() + float(SHIFT.x), VY = V->y->value() + float(SHIFT.y);
            g_applying = true;
            V->x->setValueAndWarp(VX);
            V->y->setValueAndWarp(VY);
            g_applying = false;
            if (MOVING) // carry on toward the moved cell
                glide(mon, {VX, VY}, *CELL, VELOCITY);
        }
        apply(mon);
    }
}

Motion::SPoint View::pitch(const PHLMONITOR& monitor) {
    if (const float Z = zoomOf(monitor); Z > 0)
        return Overview::cellPitch(monitor, Z);
    const auto P = pitchOf(monitor);
    return {P.x, P.y};
}

Motion::SPoint View::position(const PHLMONITOR& monitor) {
    const auto V = viewOf(monitor);
    if (V && V->hold.active)
        return V->hold.pos;
    if (V && V->x)
        return {V->x->value(), V->y->value()};
    const auto CELL = Grid::cellOf(monitor ? monitor->m_activeWorkspace : nullptr).value_or(SCell{});
    return {double(CELL.x), double(CELL.y)};
}

float View::zoom(const PHLMONITOR& monitor) {
    return zoomOf(monitor);
}

void View::zoomTo(const PHLMONITOR& monitor, float target, float velocity) {
    if (!monitor)
        return;
    auto& view = animated(monitor);
    view.zoomHold.active = false;
    if (view.zoom->value() == target) {
        // Already there: settle, so the overview hears it (it goes at 0).
        view.zoom->setValueAndWarp(target);
        return;
    }
    *view.zoom = target;
    launchAxis(view.zoom, "kinetic.grid.zoom", velocity);
}

void View::moveZoom(const PHLMONITOR& monitor, float delta, uint32_t timeMs) {
    if (!monitor)
        return;
    auto& view = animated(monitor);
    auto& hold = view.zoomHold;
    if (!hold.active) {
        hold.active = true;
        hold.start  = view.zoom->value();
        hold.v.reset();
    }
    const float Z = std::clamp(view.zoom->value() + delta, 0.F, 1.F);
    hold.last     = delta;
    hold.v.add(timeMs, Z);
    view.zoom->setValueAndWarp(Z);
}

std::optional<View::SZoomRelease> View::releaseZoom(const PHLMONITOR& monitor, uint32_t timeMs) {
    const auto V = viewOf(monitor);
    if (!V || !V->zoom || !V->zoomHold.active)
        return std::nullopt;
    auto& hold  = V->zoomHold;
    hold.active = false;
    return SZoomRelease{V->zoom->value(), hold.start, hold.last, float(hold.v.velocity(timeMs))};
}

bool View::moving(const PHLMONITOR& monitor) {
    const auto V = viewOf(monitor);
    return holding(monitor) || gliding(monitor) || (V && V->zoom && (V->zoom->isBeingAnimated() || V->zoomHold.active));
}

void View::init() {
    for (const auto& mon : State::monitorState()->monitors()) {
        if (mon && mon->m_activeWorkspace)
            g_lastActive[mon->m_id] = mon->m_activeWorkspace;
    }
    g_activeListener = Event::bus()->m_events.workspace.active.listen([](PHLWORKSPACE ws) { onActive(ws); });
}

void View::exit() {
    g_activeListener.reset();
    for (auto& [id, view] : g_views) {
        view.hold     = {};
        view.zoomHold = {};
        if (view.x) {
            view.x->warp();
            view.y->warp();
            view.zoom->setCallbackOnEnd(nullptr);
            view.zoom->setUpdateCallback(nullptr);
        }
    }
    g_views.clear();
}
