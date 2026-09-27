// Interaction: what the pointer and the keys pick in the overview -- the
// window under a point, where a dragged window lands, how a resize reshapes a
// box, the window next door for the arrow keys. No Hyprland in here, so it is
// tested on its own (tests/interaction.cpp); the overview hands it the
// boxes Scene put on screen and acts on what it picks.
//
// Boxes are wherever the caller measures them (the screen, for the pointer);
// windows come in stacking order, the bottom one first.

#pragma once

#include "Scene.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace Interaction {
    using Motion::SCell;
    using Motion::SPoint;
    using Scene::SBox;

    // Half-open, like Hyprland's CBox: the right and bottom edges are outside.
    inline bool contains(const SBox& box, const SPoint& p) {
        return p.x >= box.x && p.x < box.x + box.w && p.y >= box.y && p.y < box.y + box.h;
    }

    inline SPoint centre(const SBox& box) {
        return {box.x + box.w / 2, box.y + box.h / 2};
    }

    inline double distanceSq(const SPoint& a, const SPoint& b) {
        return (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y);
    }

    // From a point to the nearest point of a box: 0 inside it.
    inline double distanceSq(const SPoint& p, const SBox& box) {
        const double DX = p.x < box.x ? box.x - p.x : p.x > box.x + box.w ? p.x - (box.x + box.w) : 0;
        const double DY = p.y < box.y ? box.y - p.y : p.y > box.y + box.h ? p.y - (box.y + box.h) : 0;
        return DX * DX + DY * DY;
    }

    inline double overlapArea(const SBox& a, const SBox& b) {
        const double W = std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x);
        const double H = std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y);
        return std::max(0.0, W) * std::max(0.0, H);
    }

    struct SWindow {
        SBox box;
        bool floating = false;
    };

    // --- Picking ---

    // The window under `p`: floating ones over tiled ones, the topmost of
    // each. Over a fullscreen window (its index), only the floating ones and
    // that one show.
    inline std::optional<size_t> windowAt(const std::vector<SWindow>& windows, const SPoint& p, std::optional<size_t> fullscreen = {}) {
        for (const bool FLOATING : {true, false}) {
            for (size_t i = windows.size(); i-- > 0;) {
                if (windows[i].floating != FLOATING || (fullscreen && !FLOATING))
                    continue;
                if (contains(windows[i].box, p))
                    return i;
            }
        }
        if (fullscreen && *fullscreen < windows.size() && contains(windows[*fullscreen].box, p))
            return fullscreen;
        return std::nullopt;
    }

    // The window whose centre is nearest the centre of `area`, e.g. the one to
    // select on a workspace scrolled to; the first of equals. Over a fullscreen
    // window, the tiled ones behind it don't count.
    inline std::optional<size_t> nearestCentre(const std::vector<SWindow>& windows, const SBox& area, std::optional<size_t> fullscreen = {}) {
        std::optional<size_t> best;
        double                bestD = std::numeric_limits<double>::max();
        for (size_t i = 0; i < windows.size(); ++i) {
            if (fullscreen && i != *fullscreen && !windows[i].floating)
                continue;
            const double D = distanceSq(centre(windows[i].box), centre(area));
            if (D < bestD) {
                best  = i;
                bestD = D;
            }
        }
        return best;
    }

    // --- Dropping a window onto another ---

    // Which side of a window a drop at `p` goes: the outer thirds are left
    // and right, the middle splits into up and down.
    enum class ESide {
        LEFT,
        RIGHT,
        UP,
        DOWN,
    };

    inline ESide dropSide(const SBox& box, const SPoint& p) {
        const double X = p.x - box.x, Y = p.y - box.y;
        if (X < box.w / 3)
            return ESide::LEFT;
        if (X > box.w * 2 / 3)
            return ESide::RIGHT;
        return Y < box.h / 2 ? ESide::UP : ESide::DOWN;
    }

    // A tiled window a drop can land next to: `box` as drawn, `hitbox` where
    // its layout puts it (empty: the box).
    struct SDropTarget {
        SBox box, hitbox;
    };

    // The target whose hitbox holds `p`, the nearest to its drawn box if they
    // overlap; the topmost of equals.
    inline std::optional<size_t> dropTarget(const std::vector<SDropTarget>& targets, const SPoint& p) {
        std::optional<size_t> best;
        double                bestD = std::numeric_limits<double>::max();
        for (size_t i = targets.size(); i-- > 0;) {
            const auto& T   = targets[i];
            const auto  HIT = T.hitbox.w > 0 && T.hitbox.h > 0 ? T.hitbox : T.box;
            if (!contains(HIT, p))
                continue;
            const double D = distanceSq(p, T.box);
            if (D < bestD) {
                best  = i;
                bestD = D;
            }
        }
        return best;
    }

    // --- Dragging ---

    // Where on a box the pointer holds it, 0 to 1 across (the middle for an
    // empty box).
    inline SPoint grabRatio(const SBox& box, const SPoint& p) {
        return {box.w > 0 ? (p.x - box.x) / box.w : 0.5, box.h > 0 ? (p.y - box.y) / box.h : 0.5};
    }

    // A box of `size` held at `ratio` by a pointer at `p`.
    inline SBox heldAt(const SPoint& size, const SPoint& p, const SPoint& ratio) {
        return {p.x - size.x * std::clamp(ratio.x, 0.0, 1.0), p.y - size.y * std::clamp(ratio.y, 0.0, 1.0), size.x, size.y};
    }

    // A box moved inside `area`, `margin` from its edges where it fits; a box
    // too big keeps to the top left.
    inline SBox clampInto(SBox box, const SBox& area, double margin = 0) {
        if (area.w <= 0 || area.h <= 0)
            return box;
        const double M = std::max(0.0, margin);
        box.x = std::clamp(box.x, area.x + M, area.x + M + std::max(0.0, area.w - box.w - 2 * M));
        box.y = std::clamp(box.y, area.y + M, area.y + M + std::max(0.0, area.h - box.h - 2 * M));
        return box;
    }

    // A box of `size` in the middle of `area` (at its top left, too big).
    inline SBox centreIn(const SPoint& size, const SBox& area) {
        return {area.x + std::max(0.0, area.w - size.x) / 2, area.y + std::max(0.0, area.h - size.y) / 2, size.x, size.y};
    }

    // --- Resizing ---

    // The corner a resize moves: the one nearest the pointer, the others stay.
    struct SCorner {
        bool left = false, top = false;
        bool operator==(const SCorner&) const = default;
    };

    inline SCorner cornerAt(const SBox& box, const SPoint& p) {
        const auto C = centre(box);
        return {p.x < C.x, p.y < C.y};
    }

    // `box` with `corner` moved by `delta`, its size kept within `min` and
    // `max` by the moved corner giving way.
    inline SBox resize(const SBox& box, const SPoint& delta, const SCorner& corner, const SPoint& min, const std::optional<SPoint>& max = {}) {
        double left = box.x, top = box.y, right = box.x + box.w, bottom = box.y + box.h;
        (corner.left ? left : right) += delta.x;
        (corner.top ? top : bottom) += delta.y;

        const double MINW = std::max(1.0, min.x), MINH = std::max(1.0, min.y);
        const double MAXW = max ? std::max(MINW, max->x) : std::numeric_limits<double>::max();
        const double MAXH = max ? std::max(MINH, max->y) : std::numeric_limits<double>::max();
        const double W = std::clamp(right - left, MINW, MAXW), H = std::clamp(bottom - top, MINH, MAXH);

        if (corner.left)
            left = right - W;
        else
            right = left + W;
        if (corner.top)
            top = bottom - H;
        else
            bottom = top + H;
        return {left, top, right - left, bottom - top};
    }

    // The moved corner of a resized box kept inside `area`, `margin` in.
    inline SBox clampResize(const SBox& box, const SBox& area, const SCorner& corner, double margin) {
        if (area.w <= 0 || area.h <= 0)
            return box;
        const double M = std::max(0.0, margin);
        double       left = box.x, top = box.y, right = box.x + box.w, bottom = box.y + box.h;
        if (corner.left)
            left = std::max(left, area.x + M);
        else
            right = std::min(right, area.x + area.w - M);
        if (corner.top)
            top = std::max(top, area.y + M);
        else
            bottom = std::min(bottom, area.y + area.h - M);
        return {left, top, right - left, bottom - top};
    }

    // --- The keys ---

    // A tiled window for the arrow keys: its box, and the centre it is headed
    // for (mid-animation, they differ).
    struct SNeighbour {
        SBox   box;
        SPoint centre;
    };

    // The window next to `from` in `dir` (one step on one axis). Nearest that
    // way wins, preferring windows that overlap it across that axis (going up
    // or down, only those count); among the equally near, the one overlapping
    // most, or the one best lined up.
    inline std::optional<size_t> neighbour(const SNeighbour& from, const std::vector<SNeighbour>& windows, const SCell& dir) {
        const bool HORIZONTAL = dir.x != 0;
        const auto overlap    = [&](const SBox& b) {
            const double O = HORIZONTAL ? std::min(from.box.y + from.box.h, b.y + b.h) - std::max(from.box.y, b.y) :
                                          std::min(from.box.x + from.box.w, b.x + b.w) - std::max(from.box.x, b.x);
            return std::max(0.0, O);
        };

        std::optional<size_t> best;
        double                bestAlong = 0, bestAcross = 0, bestOverlap = -1;
        bool                  bestOverlaps = false;
        for (size_t i = 0; i < windows.size(); ++i) {
            const auto&  W        = windows[i];
            const double ALONG    = HORIZONTAL ? (W.centre.x - from.centre.x) * dir.x : (W.centre.y - from.centre.y) * dir.y;
            const double ACROSS   = HORIZONTAL ? std::abs(W.centre.y - from.centre.y) : std::abs(W.centre.x - from.centre.x);
            const double OVERLAP  = overlap(W.box);
            const bool   OVERLAPS = OVERLAP > 0;
            if (ALONG <= 0 || (!HORIZONTAL && !OVERLAPS))
                continue;

            bool better = false;
            if (!best)
                better = true;
            else if (OVERLAPS != bestOverlaps)
                better = OVERLAPS;
            else if (ALONG < bestAlong - 0.5)
                better = true;
            else if (std::abs(ALONG - bestAlong) <= 0.5)
                better = OVERLAPS ? OVERLAP > bestOverlap + 0.5 : ACROSS < bestAcross - 0.5;

            if (better) {
                best         = i;
                bestAlong    = ALONG;
                bestAcross   = ACROSS;
                bestOverlap  = OVERLAP;
                bestOverlaps = OVERLAPS;
            }
        }
        return best;
    }

    // --- Pinned windows ---

    // The quadrant of a `size` screen a box sits most in, as the corner a
    // pinned window keeps to in the overview; the first of equals, top left
    // first.
    inline SCorner quadrant(const SBox& box, const SPoint& size) {
        const double W = size.x / 2, H = size.y / 2;
        SCorner      best;
        double       bestArea = -1;
        for (const auto& [RIGHT, BOTTOM] : {std::pair{false, false}, {true, false}, {false, true}, {true, true}}) {
            const double AREA = overlapArea(box, {RIGHT ? W : 0, BOTTOM ? H : 0, W, H});
            if (AREA > bestArea) {
                best     = {!RIGHT, !BOTTOM};
                bestArea = AREA;
            }
        }
        return best;
    }

    // --- The pointer's buttons ---
    //
    // In the overview: the main button clicks (select what is under the
    // pointer and close) or, moved past the threshold, drags the window it
    // was pressed on; the resize button drags a window's corner. With
    // `middleDrags` the middle button drags instead, at once, and the main
    // one only clicks. A drag let go near where it started is a click.
    //
    // With the overview's submap, a click runs the submap's bind for that
    // button instead; drags stay.

    class IPointerTargets {
      public:
        virtual ~IPointerTargets() = default;

        // The overview's submap takes clicks.
        virtual bool submap()                        = 0;
        virtual void submapClick(uint32_t button)    = 0;
        virtual void click()                         = 0;
        // Pick up the window at `at` (where the button went down). False:
        // none there.
        virtual bool beginDrag(const SPoint& at)     = 0;
        virtual void drag()                          = 0;
        virtual void drop()                          = 0;
        // Put it back, not dropped.
        virtual void abandonDrag()                   = 0;
        // The window under `at` can be resized: remember it. False: none.
        virtual bool armResize(const SPoint& at)     = 0;
        virtual void disarmResize()                  = 0;
        virtual bool beginResize()                   = 0;
        virtual void resize()                        = 0;
        virtual void endResize()                     = 0;
    };

    struct SPointerConfig {
        uint32_t main = 0, resize = 0, middle = 0;
        bool     middleDrags = false;
        double   threshold   = 0; // travel before a drag starts
        double   clickSlop   = 0; // a drag let go within this of its start is a click
    };

    class CPointer {
      public:
        explicit CPointer(IPointerTargets& targets) : m_targets(targets) {}

        SPointerConfig config;

        void press(uint32_t button, const SPoint& at) {
            if (m_swallowing)
                return;
            if (button == config.main) {
                if (!pendClick(button, at)) {
                    m_pendingDrag = true;
                    m_start       = at;
                }
            } else if (button == config.middle && !config.middleDrags)
                pendClick(button, at);
            else if (button == config.resize) {
                pendClick(button, at);
                m_resizeArmed = m_targets.armResize(at);
                m_resizeStart = at;
            } else if (button == config.middle && !pendClick(button, at)) {
                m_start = at;
                beginDrag();
            }
        }

        void move(const SPoint& at) {
            if (m_swallowing)
                return;
            // Moved away, a pending click is no click: the dragging button drags.
            if (m_clickButton && !m_dragging && past(m_start, at)) {
                if (*m_clickButton == config.main) {
                    m_clickButton.reset();
                    if (!config.middleDrags)
                        beginDrag();
                } else if (*m_clickButton == config.middle && config.middleDrags) {
                    m_clickButton.reset();
                    beginDrag();
                }
            }
            if (m_pendingDrag && !m_dragging && !config.middleDrags && past(m_start, at))
                beginDrag();
            if (m_dragging)
                m_targets.drag();
            if (m_resizeArmed) {
                if (!m_resizing && past(m_resizeStart, at))
                    m_resizing = m_targets.beginResize();
                if (m_resizing)
                    m_targets.resize();
            }
        }

        void release(uint32_t button, const SPoint& at) {
            if (m_adopted) {
                m_adopted = m_swallowing = false;
                if (m_dragging)
                    drop();
                return;
            }
            if (m_swallowing) {
                m_swallowing = false;
                return;
            }
            if (button == config.main || (button == config.middle && config.middleDrags))
                finishDragOrClick(button, at);
            else if (button == config.middle)
                pendingClick(button);
            else if (button == config.resize) {
                if (m_resizing) {
                    m_targets.endResize();
                    m_resizing = m_resizeArmed = false;
                    m_clickButton.reset();
                    return;
                }
                m_resizeArmed = false;
                m_targets.disarmResize();
                pendingClick(button);
            }
        }

        // The drag ended from outside (Escape, the window closing): with a
        // button still `held`, its buttons do nothing until it goes up.
        void cancelled(bool held) {
            if (m_dragging && held)
                m_swallowing = true;
            m_dragging = m_pendingDrag = m_adopted = false;
        }

        // A drag started elsewhere (Hyprland's own) carried on here: dropped
        // when its button goes up.
        void adopt() {
            m_dragging = m_adopted = true;
            m_swallowing           = false;
        }

        // A click pressed here went elsewhere (the pointer is over a layer).
        void forgetClick() {
            m_clickButton.reset();
        }

        void reset() {
            m_clickButton.reset();
            m_pendingDrag = m_dragging = m_adopted = m_swallowing = m_resizeArmed = m_resizing = false;
        }

        // A button is down on a window: the pointer's events are the
        // overview's even over a layer.
        bool busy() const {
            return m_pendingDrag || m_dragging || m_resizeArmed || m_resizing;
        }

        bool dragging() const {
            return m_dragging;
        }

        bool adopted() const {
            return m_adopted;
        }

        bool resizing() const {
            return m_resizing;
        }

        // Waiting for the button of a cancelled drag to go up.
        bool swallowing() const {
            return m_swallowing;
        }

      private:
        bool past(const SPoint& from, const SPoint& to) const {
            return distanceSq(from, to) > config.threshold * config.threshold;
        }

        // With the submap, a press waits to be a click.
        bool pendClick(uint32_t button, const SPoint& at) {
            if (!m_targets.submap())
                return false;
            m_clickButton = button;
            m_start       = at;
            return true;
        }

        // The click `button` waited for, if it did: the submap's bind.
        bool pendingClick(uint32_t button) {
            if (m_clickButton != button)
                return false;
            m_clickButton.reset();
            m_targets.submapClick(button);
            return true;
        }

        void beginDrag() {
            m_dragging = m_targets.beginDrag(m_start);
        }

        void drop() {
            m_dragging = m_pendingDrag = false;
            m_targets.drop();
        }

        void finishDragOrClick(uint32_t button, const SPoint& at) {
            if (m_dragging) {
                if (distanceSq(m_start, at) >= config.clickSlop * config.clickSlop) {
                    m_clickButton.reset();
                    drop();
                    return;
                }
                m_dragging = m_pendingDrag = false;
                m_targets.abandonDrag();
            }
            m_pendingDrag = false;
            if (!pendingClick(button) && !m_targets.submap())
                m_targets.click();
        }

        IPointerTargets&        m_targets;
        SPoint                  m_start, m_resizeStart;
        std::optional<uint32_t> m_clickButton; // a press waiting to be a click, with the submap
        bool                    m_pendingDrag = false, m_dragging = false, m_adopted = false, m_swallowing = false;
        bool                    m_resizeArmed = false, m_resizing = false;
    };
}
