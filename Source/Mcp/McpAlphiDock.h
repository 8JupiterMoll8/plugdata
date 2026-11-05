/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/
#pragma once

// Big Alphi step 1a0 (docs/plans/big-alphi-step1-steps.md, B1 "placement"): where Alphi docks on the visible view.
// The A8 live bug: mini Alphi sat on the view's top-left corner over the artist's boxes; the covered inlet could not
// take a wire and a click there toggled flow instead of selecting the box. Now Alphi takes a corner of the visible view
// (default bottom right) that no box, chip stack, pill or drawer lies under, and steps aside to the next free corner
// when one does; no free corner: folded (today's mini face) in the least covered corner.
// JUCE-free (Tests/mcp/alphi_dock_test.cpp). The boxes are GUI bounds read on the message thread (no Pd lock, no
// census); CanvasMcpFlow.cpp collects them and drives AlphiDock.
//
// - It never moves while the mouse is on or next to it (hover) or while a mouse button is held (an object drag, a
//   lasso, a wire), and it re-checks 250 ms after the view, a box or a cover last changed (scroll, zoom, move, create).
//   It never moves into the corner under the pointer (review M1).
// - Review H1 / round 3: a click point inside a box (8 px pad) is never Alphi's (alphiOwnsPoint, per point); with a
//   box under it Alphi is drawn lightly faded, and the hover hold does not keep it over the box the pointer is on.
//   The pointer's corner is never taken (review M1/N1).
// - Review H2: the 100 ms timer decides a move; a paint applies it only when it redraws both frames, and the 400 ms
//   click gate starts only in a paint that redrew the whole frame (alphiGateAfterPaint). A move or a screen move
//   (review M2) restarts the gate. Guarded in scripts/test-bridge-safety.sh.

#include "McpAlphi.h"
#include "McpApprovalState.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace mcp {

// In the preferred order of stepping aside: from the preferred corner on, then the others in this order.
enum class AlphiCorner : int { bottomRight = 0, bottomLeft = 1, topRight = 2, topLeft = 3 };

// Alphi's frame in DESIGN (screen) pixels. 1a0 has only today's mini face (48 x 56); the unfolded body of 1a passes
// its own size and folds to the mini face when it fits nowhere.
struct AlphiSize {
    int w = AlphiMetrics::w, h = AlphiMetrics::h;
};

struct AlphiDockMetrics {
    static constexpr int boxPad = 8;               // design px around a box (its GUI bounds include the iolet strips)
    static constexpr int hoverPad = 24;            // design px around Alphi that count as "next to it"
    static constexpr std::int64_t settleMs = 250;  // re-check this long after the last change
    static constexpr AlphiCorner preferred = AlphiCorner::bottomRight;
};

struct AlphiDockChoice {
    AlphiCorner corner = AlphiDockMetrics::preferred;
    bool folded = false;        // no corner had room for `frameSize`: the mini face
    std::int64_t covered = 0;   // canvas units^2 of boxes (padded) and covers under the chosen frame; 0 = free
    bool none = false;          // review N1: every corner touches the forbidden rect (the pointer): no choice
};

namespace alphidock {

inline float safeZoom(float zoom) { return (zoom > 0.01f && std::isfinite(zoom)) ? zoom : 1.0f; }

inline std::int64_t overlapArea(ApprovalRect a, ApprovalRect b)
{
    if (!rectsIntersect(a, b))
        return 0;
    std::int64_t const w = std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x);
    std::int64_t const h = std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y);
    return w * h;
}

inline ApprovalRect grown(ApprovalRect r, int by) { return { r.x - by, r.y - by, r.w + 2 * by, r.h + 2 * by }; }

} // namespace alphidock

// Alphi's frame (canvas coordinates) in `corner` of the visible view, 12 design px from its two edges, at a fixed
// screen size. The top-left frame is exactly today's alphiRect. A view too small for it keeps it at the left/top edge.
inline ApprovalRect alphiCornerRect(ApprovalRect view, float zoom, AlphiCorner corner, AlphiSize size = {})
{
    zoom = alphidock::safeZoom(zoom);
    float const k = 1.0f / zoom;
    int const m = static_cast<int>(std::lround(AlphiMetrics::margin * k));
    int const w = static_cast<int>(std::ceil(std::max(1, size.w) * k));
    int const h = static_cast<int>(std::ceil(std::max(1, size.h) * k));
    bool const right = corner == AlphiCorner::bottomRight || corner == AlphiCorner::topRight;
    bool const bottom = corner == AlphiCorner::bottomRight || corner == AlphiCorner::bottomLeft;
    int const left = view.x + m, top = view.y + m;
    int const x = right ? std::max(left, view.x + view.w - m - w) : left;
    int const y = bottom ? std::max(top, view.y + view.h - m - h) : top;
    return { x, y, w, h };
}

// How much lies under `frame`: boxes grown by the pad (8 design px), covers as they are.
inline std::int64_t alphiCoveredArea(ApprovalRect frame, float zoom, std::vector<ApprovalRect> const& boxes,
    std::vector<ApprovalRect> const& covers)
{
    int const pad = static_cast<int>(std::ceil(AlphiDockMetrics::boxPad / alphidock::safeZoom(zoom)));
    std::int64_t sum = 0;
    for (auto const& b : boxes)
        sum += alphidock::overlapArea(frame, alphidock::grown(b, pad));
    for (auto const& c : covers)
        sum += alphidock::overlapArea(frame, c);
    return sum;
}

// The corner Alphi takes: the first free one for `frameSize` from `preferred` on; else the first free one for the
// mini face (folded); else the least covered one for the mini face (folded; ties go to the earlier corner).
// Review N1: a corner whose mini frame meets `forbidden` (the pointer and the space around it) is never taken, not even
// as the least covered one; when that rules out every corner the choice is `none`.
inline AlphiDockChoice chooseCorner(ApprovalRect view, float zoom, std::vector<ApprovalRect> const& boxes,
    std::vector<ApprovalRect> const& covers, AlphiCorner preferred, AlphiSize frameSize = {},
    std::optional<ApprovalRect> forbidden = std::nullopt)
{
    AlphiCorner all[4] { preferred };
    int n = 1;
    for (int i = 0; i < 4; ++i)
        if (static_cast<AlphiCorner>(i) != preferred)
            all[n++] = static_cast<AlphiCorner>(i);
    AlphiCorner order[4] {};
    n = 0;
    for (auto c : all)
        if (!forbidden || !rectsIntersect(alphiCornerRect(view, zoom, c), *forbidden))
            order[n++] = c;
    if (n == 0) {
        AlphiDockChoice none {};
        none.none = true;
        return none;
    }
    // Only boxes and covers near the view can lie under a corner frame: drop the rest once.
    std::vector<ApprovalRect> near, nearCovers;
    {
        int const pad = static_cast<int>(std::ceil(AlphiDockMetrics::boxPad / alphidock::safeZoom(zoom)));
        auto const area = alphidock::grown(view, pad);
        near.reserve(boxes.size());
        for (auto const& b : boxes)
            if (rectsIntersect(b, area))
                near.push_back(b);
        for (auto const& c : covers)
            if (rectsIntersect(c, view))
                nearCovers.push_back(c);
    }
    AlphiSize const mini {};
    bool const sameSize = frameSize.w == mini.w && frameSize.h == mini.h;
    for (int i = 0; i < n; ++i)
        if (alphiCoveredArea(alphiCornerRect(view, zoom, order[i], frameSize), zoom, near, nearCovers) == 0)
            return { order[i], false, 0 };
    AlphiDockChoice best { order[0], true, -1 };
    for (int i = 0; i < n; ++i) {
        auto const c = order[i];
        auto const cov = alphiCoveredArea(alphiCornerRect(view, zoom, c, mini), zoom, near, nearCovers);
        if (cov == 0 && !sameSize)
            return { c, true, 0 };
        if (best.covered < 0 || cov < best.covered)
            best = { c, true, cov };
    }
    return best;
}


// The mouse in canvas coordinates; `known` = it is over this canvas (or a button is held on it).
struct AlphiMouse {
    bool known = false;
    int x = 0, y = 0;
};

// The mouse (canvas coordinates) is on Alphi's frame or within 24 design px of it.
inline bool alphiHovered(ApprovalRect frame, float zoom, int x, int y)
{
    if (frame.w <= 0 || frame.h <= 0)
        return false;
    int const pad = static_cast<int>(std::lround(AlphiDockMetrics::hoverPad / alphidock::safeZoom(zoom)));
    auto const g = alphidock::grown(frame, pad);
    return x >= g.x && y >= g.y && x < g.x + g.w && y < g.y + g.h;
}

inline bool alphiRectContains(ApprovalRect outer, ApprovalRect inner)
{
    return inner.w > 0 && inner.h > 0 && outer.w > 0 && outer.h > 0 && inner.x >= outer.x && inner.y >= outer.y
        && inner.x + inner.w <= outer.x + outer.w && inner.y + inner.h <= outer.y + outer.h;
}

// 1a0 review H1: a box (its GUI bounds, unpadded) lies under Alphi's frame: the artist dropped one there, the view
// scrolled boxes under it, the AI made one, or every corner is full and the fold sits on boxes. Then Alphi is drawn
// lightly faded; which click points are still his is alphiOwnsPoint's question (per point).
inline bool alphiFrameBlocked(ApprovalRect frame, std::vector<ApprovalRect> const& boxes)
{
    for (auto const& b : boxes)
        if (rectsIntersect(frame, b))
            return true;
    return false;
}

// The one question the click and the hit map ask (handleAlphiClick, isPointOverNote): is this point Alphi's?
// Only inside its frame, and never inside a box grown by the 8 px pad (1a0 round 3: PER POINT, replacing the frame
// rule: with every corner full the folded Alphi sat on boxes and was never clickable, a dead end for flow). A click on
// a box under Alphi is the box's / the canvas's; a click on a free part of Alphi is his, after the normal gate.
inline bool alphiOwnsPoint(ApprovalRect frame, std::vector<ApprovalRect> const& boxes, int x, int y, float zoom)
{
    if (!alphiHit(frame, x, y))
        return false;
    int const pad = static_cast<int>(std::ceil(AlphiDockMetrics::boxPad / alphidock::safeZoom(zoom)));
    for (auto const& b : boxes)
        if (alphiHit(alphidock::grown(b, pad), x, y))
            return false;
    return true;
}

// Alphi holds still: a mouse button is held (an object drag, a lasso, a wire, a resize), or the mouse is on / next
// to it. Review H1: NOT while the pointer is on a box that lies under Alphi (the artist reaches for that box; Alphi
// steps aside instead of holding over it).
inline bool alphiDockHold(ApprovalRect frame, float zoom, std::vector<ApprovalRect> const& boxes, bool buttonDown, AlphiMouse m)
{
    if (buttonDown)
        return true;
    if (!m.known || !alphiHovered(frame, zoom, m.x, m.y))
        return false;
    for (auto const& b : boxes)
        if (rectsIntersect(frame, b) && alphiHit(b, m.x, m.y))
            return false;
    return true;
}

// Review M2: Alphi's frame moved on SCREEN (a window resize, a split view, the sidebar) by more than a rounding step.
inline bool alphiScreenMoved(int prevX, int prevY, int nowX, int nowY)
{
    return std::abs(prevX - nowX) > 1 || std::abs(prevY - nowY) > 1;
}

// What the 400 ms click gate (`shownSinceMs`, 0 = not on screen) is after one paint. Review H2: it starts only in a
// paint that redrew Alphi's whole frame (`frameInRegion`); a partial NVG render never starts it.
struct AlphiGateInput {
    bool covered = false;       // a chip stack, the pill or its drawers cover Alphi (not drawn)
    bool screenMoved = false;   // its frame moved on screen since the last paint (alphiScreenMoved)
    bool frameInRegion = false; // this paint redrew Alphi's whole frame
};

inline std::int64_t alphiGateAfterPaint(std::int64_t shownSinceMs, std::int64_t nowMs, AlphiGateInput in)
{
    if (in.covered)
        return 0;
    if (in.screenMoved || shownSinceMs <= 0)
        return in.frameInRegion ? nowMs : 0;
    return shownSinceMs;
}

// Review N2: after a paint, does Alphi's frame need one more repaint? Yes when his look changed (a box arrived under
// him or left) or his gate is still 0, and this paint did not redraw his frame; once per state, so it never loops.
struct AlphiRepaintState {
    bool known = false;
    bool blocked = false;
    bool asked = false;
};

inline bool alphiAfterPaint(AlphiRepaintState& st, bool blocked, std::int64_t gateMs, bool frameInRegion, bool covered)
{
    if (covered) {
        st.known = false;
        st.asked = false;
        return false;
    }
    bool const changed = st.known && blocked != st.blocked;
    st.known = true;
    st.blocked = blocked;
    if (frameInRegion) {
        st.asked = false;
        return false;
    }
    if (changed || (gateMs <= 0 && !st.asked)) {
        st.asked = true;
        return true;
    }
    return false;
}

// Where Alphi sits now, and when it may move. One per canvas view, message thread only.
// Review H2: a move is DECIDED by the 100 ms timer (decide) and APPLIED only in a paint that redraws both the old and
// the new frame (apply), so a partial render never leaves a ghost Alphi or an invisible, clickable one.
class AlphiDock {
public:
    static constexpr int maxApplyTries = 5; // review L1: full repaints asked for one staged move, then it is dropped

    // Every paint: a change of the view, a box or a cover schedules a re-check settleMs later.
    void look(ApprovalRect view, float zoom, std::vector<ApprovalRect> const& boxes, std::vector<ApprovalRect> const& covers,
        std::int64_t nowMs)
    {
        auto const sig = signature(view, zoom, boxes, covers);
        if (sig == lastSig)
            return;
        lastSig = sig;
        if (isPlaced) {
            settleAtMs = nowMs + AlphiDockMetrics::settleMs;
            if (settleAtMs == 0)
                settleAtMs = 1;
        }
    }

    // Not placed yet: choose now. Placed: once the re-check is due and Alphi is not held, choose again. The mouse
    // and the space around it count as a cover (review M1: Alphi never jumps under the artist's hand). True when a
    // move (or the first place) is staged and waits for a full paint.
    bool decide(ApprovalRect view, float zoom, std::vector<ApprovalRect> const& boxes, std::vector<ApprovalRect> const& covers,
        bool hold, AlphiMouse mouse, std::int64_t nowMs, AlphiSize frameSize = {})
    {
        if (isPlaced && (!due(nowMs) || hold))
            return false;
        settleAtMs = 0;
        // Review M1 / N1: no corner whose frame meets the pointer (grown by 24 px), not even as the least covered one;
        // if that rules out every corner, Alphi stays where it is (the first place then ignores the pointer).
        std::optional<ApprovalRect> pointer;
        if (mouse.known) {
            int const pad = static_cast<int>(std::lround(AlphiDockMetrics::hoverPad / alphidock::safeZoom(zoom)));
            pointer = alphidock::grown(ApprovalRect { mouse.x, mouse.y, 1, 1 }, pad);
        }
        auto next = chooseCorner(view, zoom, boxes, covers, AlphiDockMetrics::preferred, frameSize, pointer);
        if (next.none) {
            if (isPlaced) {
                isStaged = false;
                return false;
            }
            next = chooseCorner(view, zoom, boxes, covers, AlphiDockMetrics::preferred, frameSize);
        }
        if (isPlaced && next.corner == choice.corner && next.folded == choice.folded) {
            isStaged = false;
            return false;
        }
        staged = next;
        isStaged = true;
        tries = 0;
        return true;
    }

    // A paint that redrew `region` (canvas coordinates): the staged move lands only when the region holds the new
    // frame and, once placed, the old one too. True when Alphi moved (the caller restarts its 400 ms gate).
    bool apply(ApprovalRect oldFrame, ApprovalRect newFrame, ApprovalRect region)
    {
        if (!isStaged || !alphiRectContains(region, newFrame))
            return false;
        if (isPlaced && !alphiRectContains(region, oldFrame))
            return false;
        choice = staged;
        isPlaced = true;
        isStaged = false;
        return true;
    }

    // The timer asks for one more full repaint for the staged move; false (and the move dropped, to be decided
    // again at the next change) once maxApplyTries repaints never reached a full paint.
    bool retryStaged()
    {
        if (!isStaged)
            return false;
        if (++tries > maxApplyTries) {
            isStaged = false;
            tries = 0;
            return false;
        }
        return true;
    }

    AlphiCorner corner() const { return choice.corner; }
    AlphiCorner stagedCorner() const { return isStaged ? staged.corner : choice.corner; }
    bool folded() const { return choice.folded; }
    bool placed() const { return isPlaced; }
    bool hasStaged() const { return isStaged; }
    // A re-check is waiting (for the settle time, the hover or the drag).
    bool pending() const { return settleAtMs != 0; }
    bool due(std::int64_t nowMs) const { return settleAtMs != 0 && nowMs >= settleAtMs; }
    // Forget the place (the bridge stopped, the tab was hidden): the next paint places Alphi again.
    void reset()
    {
        isPlaced = false;
        isStaged = false;
        settleAtMs = 0;
        lastSig = 0;
        tries = 0;
    }

private:
    static std::uint64_t mix(std::uint64_t h, std::uint64_t v)
    {
        h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
    static std::uint64_t mixRect(std::uint64_t h, ApprovalRect r)
    {
        h = mix(h, static_cast<std::uint32_t>(r.x));
        h = mix(h, static_cast<std::uint32_t>(r.y));
        h = mix(h, static_cast<std::uint32_t>(r.w));
        return mix(h, static_cast<std::uint32_t>(r.h));
    }
    static std::uint64_t signature(ApprovalRect view, float zoom, std::vector<ApprovalRect> const& boxes,
        std::vector<ApprovalRect> const& covers)
    {
        std::uint32_t zb = 0;
        static_assert(sizeof(zb) == sizeof(zoom), "float is 32 bit");
        std::memcpy(&zb, &zoom, sizeof(zb));
        std::uint64_t h = mix(mixRect(1469598103934665603ULL, view), zb);
        h = mix(h, boxes.size());
        for (auto const& b : boxes)
            h = mixRect(h, b);
        h = mix(h, covers.size());
        for (auto const& c : covers)
            h = mixRect(h, c);
        return h | 1; // never 0 (0 = nothing seen yet)
    }

    AlphiDockChoice choice {};
    AlphiDockChoice staged {};
    bool isPlaced = false;
    bool isStaged = false;
    std::int64_t settleAtMs = 0;
    std::uint64_t lastSig = 0;
    int tries = 0;
};

// Alphi drop 1, F3 (D11, owner 2026-11-04): with no free corner Alphi no longer folds onto boxes; he becomes a 28 x 56
// tab behind the view's right edge (one eye showing), at the first edge spot free of padded boxes, searching outward
// from the middle; no free spot: the middle, drawn under the boxes' layer. A click on the tab brings him out over the
// edge (the normal face, same flow-click rules: shown 400 ms before a click counts); the next click sends him back.
// The tab itself is never a flow click (it has no hit frame of Alphi's). Tests/mcp/alphi_dock_test.cpp.
struct AlphiTabMetrics {
    static constexpr int w = 28; // design px (one eye showing)
    static constexpr int h = 56;
    static constexpr int step = 8; // design px between the edge spots tried
};

struct AlphiEdgeTab {
    ApprovalRect rect;       // canvas coordinates, flush with the view's right edge
    bool underBoxes = false; // no free spot: drawn under the boxes (their clicks stay theirs)
};

// No free corner for the mini face (chooseCorner folded it onto boxes or covers): the edge tab instead.
inline bool alphiUseTab(AlphiDockChoice const& c) { return c.folded && c.covered > 0; }

inline AlphiEdgeTab alphiEdgeTab(ApprovalRect view, float zoom, std::vector<ApprovalRect> const& boxes)
{
    using T = AlphiTabMetrics;
    float const k = 1.0f / alphidock::safeZoom(zoom);
    int const w = static_cast<int>(std::ceil(T::w * k));
    int const h = static_cast<int>(std::ceil(T::h * k));
    int const m = static_cast<int>(std::lround(AlphiMetrics::margin * k));
    int const step = std::max(1, static_cast<int>(std::lround(T::step * k)));
    int const pad = static_cast<int>(std::ceil(AlphiDockMetrics::boxPad * k));
    int const x = view.x + view.w - w;
    int const yMin = view.y + m, yMax = view.y + view.h - m - h;
    int const mid = view.y + (view.h - h) / 2;
    // Only boxes near the right edge can lie under the tab.
    std::vector<ApprovalRect> near;
    ApprovalRect const column { x - pad, view.y - pad, w + 2 * pad, view.h + 2 * pad };
    for (auto const& b : boxes)
        if (rectsIntersect(b, column))
            near.push_back(alphidock::grown(b, pad));
    auto const freeAt = [&](int y) {
        ApprovalRect const r { x, y, w, h };
        for (auto const& b : near)
            if (rectsIntersect(r, b))
                return false;
        return true;
    };
    if (yMax >= yMin) {
        for (int d = 0; mid - d >= yMin || mid + d <= yMax; d += step) {
            if (mid - d >= yMin && mid - d <= yMax && freeAt(mid - d))
                return { { x, mid - d, w, h }, false };
            if (d > 0 && mid + d <= yMax && mid + d >= yMin && freeAt(mid + d))
                return { { x, mid + d, w, h }, false };
        }
    }
    return { { x, mid, w, h }, true };
}

// The face while out: the normal 48 x 56 face at the right edge (12 design px in), at the tab's height.
inline ApprovalRect alphiOutRect(ApprovalRect view, float zoom, ApprovalRect tab)
{
    float const k = 1.0f / alphidock::safeZoom(zoom);
    int const w = static_cast<int>(std::ceil(AlphiMetrics::w * k));
    int const h = static_cast<int>(std::ceil(AlphiMetrics::h * k));
    int const m = static_cast<int>(std::lround(AlphiMetrics::margin * k));
    int const y = std::max(view.y + m, std::min(tab.y, view.y + view.h - m - h));
    return { std::max(view.x + m, view.x + view.w - m - w), y, w, h };
}

// Tab or out. A click while tabbed: on the tab -> out (consumed, never flow); elsewhere -> nothing. Any click while out
// sends him back; only a click on his face may then be a flow click (judged by the normal face path and its gate).
class AlphiPeek {
public:
    struct Click {
        bool consumed = false; // the tab took the click
        bool mayFlow = false;  // the click lands on his out face: the normal face path judges it
    };
    Click click(bool onTab, bool onFace)
    {
        if (!isOut) {
            if (!onTab) return {};
            isOut = true;
            return { true, false };
        }
        isOut = false;
        return { false, onFace };
    }
    bool out() const { return isOut; }
    void reset() { isOut = false; }

private:
    bool isOut = false;
};

} // namespace mcp
