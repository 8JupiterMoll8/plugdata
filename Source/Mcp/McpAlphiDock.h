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
// - A move restarts its 400 ms click gate (CanvasMcpFlow.cpp): a click aimed at what was in that corner is never flow.

#include "McpAlphi.h"
#include "McpApprovalState.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
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
inline AlphiDockChoice chooseCorner(ApprovalRect view, float zoom, std::vector<ApprovalRect> const& boxes,
    std::vector<ApprovalRect> const& covers, AlphiCorner preferred, AlphiSize frameSize = {})
{
    AlphiCorner order[4] { preferred };
    int n = 1;
    for (int i = 0; i < 4; ++i)
        if (static_cast<AlphiCorner>(i) != preferred)
            order[n++] = static_cast<AlphiCorner>(i);
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
    for (auto c : order)
        if (alphiCoveredArea(alphiCornerRect(view, zoom, c, frameSize), zoom, near, nearCovers) == 0)
            return { c, false, 0 };
    AlphiDockChoice best { order[0], true, -1 };
    for (auto c : order) {
        auto const cov = alphiCoveredArea(alphiCornerRect(view, zoom, c, mini), zoom, near, nearCovers);
        if (cov == 0 && !sameSize)
            return { c, true, 0 };
        if (best.covered < 0 || cov < best.covered)
            best = { c, true, cov };
    }
    return best;
}

// The mouse (canvas coordinates) is on Alphi's frame or within 24 design px of it.
inline bool alphiHovered(ApprovalRect frame, float zoom, int x, int y)
{
    if (frame.w <= 0 || frame.h <= 0)
        return false;
    int const pad = static_cast<int>(std::lround(AlphiDockMetrics::hoverPad / alphidock::safeZoom(zoom)));
    auto const g = alphidock::grown(frame, pad);
    return x >= g.x && y >= g.y && x < g.x + g.w && y < g.y + g.h;
}

// Where Alphi sits now, and when it may move. One per canvas view, message thread only.
class AlphiDock {
public:
    // Called with every look at the view (each paint): the first look places Alphi at once; afterwards a change of the
    // view, a box or a cover schedules a re-check settleMs later, which runs at the first look after that time while
    // the mouse is neither on/next to Alphi (`hovered`) nor holding a button (`dragging`). True when Alphi moved.
    bool update(ApprovalRect view, float zoom, std::vector<ApprovalRect> const& boxes, std::vector<ApprovalRect> const& covers,
        bool hovered, bool dragging, std::int64_t nowMs, AlphiSize frameSize = {})
    {
        auto const sig = signature(view, zoom, boxes, covers);
        if (!isPlaced) {
            choice = chooseCorner(view, zoom, boxes, covers, AlphiDockMetrics::preferred, frameSize);
            isPlaced = true;
            lastSig = sig;
            settleAtMs = 0;
            return true;
        }
        if (sig != lastSig) {
            lastSig = sig;
            settleAtMs = nowMs + AlphiDockMetrics::settleMs;
            if (settleAtMs == 0)
                settleAtMs = 1;
        }
        if (!due(nowMs) || hovered || dragging)
            return false;
        settleAtMs = 0;
        auto const next = chooseCorner(view, zoom, boxes, covers, AlphiDockMetrics::preferred, frameSize);
        bool const moved = next.corner != choice.corner || next.folded != choice.folded;
        choice = next;
        return moved;
    }

    AlphiCorner corner() const { return choice.corner; }
    bool folded() const { return choice.folded; }
    bool placed() const { return isPlaced; }
    // A re-check is waiting (for the settle time, the hover or the drag).
    bool pending() const { return settleAtMs != 0; }
    // The re-check may run now (the timer asks, to repaint once the settle time is over).
    bool due(std::int64_t nowMs) const { return settleAtMs != 0 && nowMs >= settleAtMs; }
    // Forget the place (the bridge stopped, the tab was hidden): the next look places Alphi at once.
    void reset()
    {
        isPlaced = false;
        settleAtMs = 0;
        lastSig = 0;
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
        return h;
    }

    AlphiDockChoice choice {};
    bool isPlaced = false;
    std::int64_t settleAtMs = 0;
    std::uint64_t lastSig = 0;
};

} // namespace mcp
