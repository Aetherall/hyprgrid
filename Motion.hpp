// Motion: the arithmetic of moving the view (View.cpp) -- velocity tracking,
// UIKit flick projection, how far a held view may go and where a release
// lands. No Hyprland in here, so it is tested on its own (tests/motion.cpp).
//
// Positions are in cells; velocities are per millisecond, the unit the
// gesture samples come in.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <vector>

namespace Motion {
    struct SCell {
        int  x = 0, y = 0;
        bool operator==(const SCell&) const = default;
    };

    struct SPoint {
        double x = 0, y = 0;
    };

    // A velocity v (units/ms) coasts v * r / (1 - r) further under UIKit's
    // normal deceleration rate r = 0.998/ms: ~0.5s worth of travel.
    inline constexpr double PROJECTION_MS = 0.998 / (1 - 0.998);
    // Release velocity: measured over this much of the latest motion...
    inline constexpr uint32_t VELOCITY_WINDOW_MS = 80;
    // ...and zero if the fingers were still this long before lifting.
    inline constexpr uint32_t VELOCITY_STALE_MS = 50;

    inline double project(double pos, double velocityPerMs) {
        return pos + velocityPerMs * PROJECTION_MS;
    }

    // Position samples over time -> release velocity.
    class CVelocityTracker {
      public:
        void reset() {
            m_samples.clear();
        }

        void add(uint32_t timeMs, double pos) {
            m_samples.push_back({timeMs, pos});
            while (m_samples.size() > 2 && m_samples.front().t + 2 * VELOCITY_WINDOW_MS < timeMs)
                m_samples.pop_front();
        }

        // Units/ms at endMs, when the fingers lifted.
        double velocity(uint32_t endMs) const {
            if (m_samples.size() < 2 || endMs - m_samples.back().t > VELOCITY_STALE_MS)
                return 0;
            const auto& last  = m_samples.back();
            auto        first = m_samples.front();
            for (auto it = m_samples.rbegin() + 1; it != m_samples.rend(); ++it) {
                first = *it;
                if (it->t + VELOCITY_WINDOW_MS < last.t)
                    break;
            }
            const double DT = double(last.t) - double(first.t);
            return DT > 0 ? (last.pos - first.pos) / DT : 0;
        }

      private:
        struct SSample {
            uint32_t t;
            double   pos;
        };
        std::deque<SSample> m_samples;
    };

    // Whether the view may go to a cell (Grid::enterable()).
    using CanEnter = std::function<bool(const SCell&)>;

    // How far a view held on `anchor` may go on each axis, relative to it:
    // one cell toward each neighbour it can enter.
    struct SBounds {
        SPoint lo, hi;
    };

    inline SBounds boundsAround(const SCell& anchor, const CanEnter& canEnter) {
        return {{canEnter({anchor.x - 1, anchor.y}) ? -1.0 : 0.0, canEnter({anchor.x, anchor.y - 1}) ? -1.0 : 0.0},
                {canEnter({anchor.x + 1, anchor.y}) ? 1.0 : 0.0, canEnter({anchor.x, anchor.y + 1}) ? 1.0 : 0.0}};
    }

    // A held view at `pos` moved by `delta`, as an offset from `anchor` kept
    // within `bounds`.
    inline SPoint offsetWithin(const SPoint& pos, const SPoint& delta, const SCell& anchor, const SBounds& bounds) {
        return {std::clamp(pos.x + delta.x - anchor.x, bounds.lo.x, bounds.hi.x), std::clamp(pos.y + delta.y - anchor.y, bounds.lo.y, bounds.hi.y)};
    }

    // The neighbour whose centre a view held on `anchor`, `offset` away from
    // it, has reached; none while it is still between cells. On a diagonal
    // that can't be entered, the axis that got there.
    inline std::optional<SCell> reached(const SPoint& offset, const SCell& anchor, const CanEnter& canEnter) {
        const SCell LAND = {anchor.x + (std::abs(offset.x) >= 1 ? int(offset.x) : 0), anchor.y + (std::abs(offset.y) >= 1 ? int(offset.y) : 0)};
        if (LAND == anchor)
            return std::nullopt;
        if (canEnter(LAND))
            return LAND;
        return std::abs(offset.x) >= 1 ? SCell{LAND.x, anchor.y} : SCell{anchor.x, LAND.y};
    }

    // Where a view held on `anchor` and let go at `pos` lands: the cell nearest
    // where `velocityPerMs` would coast it, among the ones within reach it can
    // enter, or back on `anchor`.
    inline SCell landing(const SPoint& pos, const SPoint& velocityPerMs, const SCell& anchor, const CanEnter& canEnter) {
        const auto   BOUNDS = boundsAround(anchor, canEnter);
        const SPoint P      = {project(pos.x, velocityPerMs.x), project(pos.y, velocityPerMs.y)};
        const SCell  WANT   = {anchor.x + int(std::clamp(std::round(P.x - anchor.x), BOUNDS.lo.x, BOUNDS.hi.x)),
                               anchor.y + int(std::clamp(std::round(P.y - anchor.y), BOUNDS.lo.y, BOUNDS.hi.y))};
        SCell        target = anchor;
        double       best   = -1;
        for (const auto& c : {WANT, SCell{WANT.x, anchor.y}, SCell{anchor.x, WANT.y}, anchor}) {
            const double D = std::pow(c.x - P.x, 2) + std::pow(c.y - P.y, 2);
            if ((c == anchor || canEnter(c)) && (best < 0 || D < best)) {
                target = c;
                best   = D;
            }
        }
        return target;
    }

    // Zoomed out, the view pans freely over the workspaces it can see: the
    // box around `cells`, relative to `anchor`.
    inline SBounds boundsOver(const std::vector<SCell>& cells, const SCell& anchor) {
        SBounds bounds;
        for (const auto& c : cells) {
            bounds.lo = {std::min(bounds.lo.x, double(c.x - anchor.x)), std::min(bounds.lo.y, double(c.y - anchor.y))};
            bounds.hi = {std::max(bounds.hi.x, double(c.x - anchor.x)), std::max(bounds.hi.y, double(c.y - anchor.y))};
        }
        return bounds;
    }

    // Where a zoomed-out view let go at `pos` lands: among `cells`, the one
    // nearest where `velocityPerMs` would coast it (`fallback` if none).
    inline SCell nearestLanding(const SPoint& pos, const SPoint& velocityPerMs, const std::vector<SCell>& cells, const SCell& fallback) {
        const SPoint P      = {project(pos.x, velocityPerMs.x), project(pos.y, velocityPerMs.y)};
        SCell        target = fallback;
        double       best   = -1;
        for (const auto& c : cells) {
            const double D = std::pow(c.x - P.x, 2) + std::pow(c.y - P.y, 2);
            if (best < 0 || D < best) {
                target = c;
                best   = D;
            }
        }
        return target;
    }

    // Where a view let go at `pos`, last moved by `last`, lands going on that
    // way: among `cells`, the nearest ahead in its row or column, on the axis
    // it moved along most; a wheel notch is enough to get there. Nothing
    // ahead: the nearest to `pos` (`fallback` if none).
    inline SCell aheadLanding(const SPoint& pos, const SPoint& last, const std::vector<SCell>& cells, const SCell& fallback) {
        if (last.x == 0 && last.y == 0)
            return nearestLanding(pos, {}, cells, fallback);
        const bool   HORIZONTAL = std::abs(last.x) >= std::abs(last.y);
        const double SIGN       = (HORIZONTAL ? last.x : last.y) > 0 ? 1 : -1;
        std::optional<SCell> target;
        double               best = 0;
        for (const auto& c : cells) {
            const double ALONG  = ((HORIZONTAL ? c.x - pos.x : c.y - pos.y)) * SIGN;
            const double ACROSS = HORIZONTAL ? c.y - pos.y : c.x - pos.x;
            if (ALONG <= 1e-3 || std::abs(ACROSS) >= 0.5)
                continue;
            if (!target || ALONG < best) {
                target = c;
                best   = ALONG;
            }
        }
        return target ? *target : nearestLanding(pos, {}, cells, fallback);
    }
}
