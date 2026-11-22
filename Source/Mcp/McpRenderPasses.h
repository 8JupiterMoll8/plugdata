/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/
#pragma once

// Repaint scope (2026-11-22): how one frame's damage is split into clipped render passes. JUCE-free, so
// Tests/mcp/render_passes_test.cpp builds it with plain g++; NVGSurface instantiates the same templates with
// juce::Rectangle<int> (the rect type only needs isEmpty / getWidth / getHeight / intersects / getUnion /
// getIntersection / contains, which both PassRect below and juce::Rectangle have).
//
// Why: NVGSurface kept the frame's damage as ONE bounding box. Eight moving widgets spread over a wide canvas, each
// repainting only its own small rect, redrew nearly the whole canvas (every box, cable and overlay) every frame at
// render scale 2: 180-200 % CPU, message thread and GL thread both near 95 %. The first split (toolbar meter vs. the
// canvas) still forced every damage rect that touched the canvas into one pass (`bothOnCanvas`), because the canvas
// render reads the invalid area and the MCP layer keeps per-paint state. That audit is done (CanvasMcpLayer: every
// per-paint field is either recomputed whole in each pass, or recorded only by a pass that covers what it records), so
// canvas damage may now be several passes. What stays: the passes are pairwise disjoint, never paint more than the
// bounding box of the damage, and a canvas that cannot take two passes (mergeCanvasDamage: presentation mode draws
// whole-canvas quads, one per pass) keeps the old rule.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mcp {

/** A plain rectangle with the subset of juce::Rectangle<int> the pass planner uses. */
struct PassRect {
    int x = 0, y = 0, w = 0, h = 0;

    int getX() const { return x; }
    int getY() const { return y; }
    int getWidth() const { return w; }
    int getHeight() const { return h; }
    int getRight() const { return x + w; }
    int getBottom() const { return y + h; }
    bool isEmpty() const { return w <= 0 || h <= 0; }
    bool operator==(PassRect const& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
    bool operator!=(PassRect const& o) const { return !(*this == o); }

    // juce::Rectangle semantics: touching edges do not intersect; an empty rectangle intersects nothing.
    bool intersects(PassRect const& o) const
    {
        return !isEmpty() && !o.isEmpty() && getRight() > o.x && getBottom() > o.y && o.getRight() > x && o.getBottom() > y;
    }
    bool contains(PassRect const& o) const
    {
        return x <= o.x && y <= o.y && getRight() >= o.getRight() && getBottom() >= o.getBottom();
    }
    PassRect getUnion(PassRect const& o) const
    {
        if (isEmpty())
            return o;
        if (o.isEmpty())
            return *this;
        int const nx = std::min(x, o.x), ny = std::min(y, o.y);
        return { nx, ny, std::max(getRight(), o.getRight()) - nx, std::max(getBottom(), o.getBottom()) - ny };
    }
    PassRect getIntersection(PassRect const& o) const
    {
        int const nx = std::max(x, o.x), ny = std::max(y, o.y);
        int const nr = std::min(getRight(), o.getRight()), nb = std::min(getBottom(), o.getBottom());
        if (nr <= nx || nb <= ny)
            return {};
        return { nx, ny, nr - nx, nb - ny };
    }
};

/** More damage clusters than this are merged, cheapest pair first (never into one bounding box: that was the bug). */
constexpr std::size_t maxRenderPasses = 8;
/** Separately painted rects are merged when their union is hardly bigger than the two (area in px^2 of slack). */
constexpr std::int64_t renderPassNearSlack = 64 * 64;

template <class Rect>
struct RenderPassInput {
    std::vector<Rect> rects; // the frame's separate damage rects (the editor's coordinates)
    Rect damage;             // their bounding box (NVGSurface::invalidArea)
    Rect bounds;             // the editor's bounds
    Rect canvasArea;         // the tab component's area; read only with mergeCanvasDamage
    bool pluginMode = false; // plugin mode renders its one canvas as a whole: one pass
    // A canvas that must see ONE region per frame (presentation mode fills whole-canvas quads in every pass): every
    // rect that touches canvasArea is merged with the others that do, as before the split.
    bool mergeCanvasDamage = false;
    std::size_t maxPasses = maxRenderPasses;
};

namespace detail {
    template <class Rect>
    std::int64_t passArea(Rect const& r)
    {
        return r.isEmpty() ? 0 : static_cast<std::int64_t>(r.getWidth()) * r.getHeight();
    }

    // Merge every pair that overlaps, lies close enough to be cheaper as one pass, or (mergeCanvas) touches the canvas
    // on both sides, until none is left. The result is pairwise disjoint.
    template <class Rect>
    void mergeNeighbours(std::vector<Rect>& passes, bool mergeCanvas, Rect const& canvasArea)
    {
        bool merged = true;
        while (merged) {
            merged = false;
            for (std::size_t i = 0; i < passes.size() && !merged; ++i) {
                for (std::size_t j = i + 1; j < passes.size(); ++j) {
                    auto const a = passes[i];
                    auto const b = passes[j];
                    auto const u = a.getUnion(b);
                    bool const onCanvas = mergeCanvas && a.intersects(canvasArea) && b.intersects(canvasArea);
                    bool const cheap = passArea(u) * 2 <= (passArea(a) + passArea(b)) * 3 + renderPassNearSlack;
                    if (onCanvas || a.intersects(b) || cheap) {
                        passes[i] = u;
                        passes.erase(passes.begin() + static_cast<std::ptrdiff_t>(j));
                        merged = true;
                        break;
                    }
                }
            }
        }
    }

    // One merge too many clusters need: the pair whose union adds the least area over the two.
    template <class Rect>
    void mergeCheapestPair(std::vector<Rect>& passes)
    {
        std::size_t bestI = 0, bestJ = 1;
        std::int64_t best = INT64_MAX;
        for (std::size_t i = 0; i < passes.size(); ++i) {
            for (std::size_t j = i + 1; j < passes.size(); ++j) {
                auto const extra = passArea(passes[i].getUnion(passes[j])) - passArea(passes[i]) - passArea(passes[j]);
                if (extra < best) {
                    best = extra;
                    bestI = i;
                    bestJ = j;
                }
            }
        }
        passes[bestI] = passes[bestI].getUnion(passes[bestJ]);
        passes.erase(passes.begin() + static_cast<std::ptrdiff_t>(bestJ));
    }
} // namespace detail

/** The clipped passes one frame is painted in (editor coordinates, inside `bounds`). Pairwise disjoint; never more than
 *  maxPasses; their total area never exceeds the bounding box of the damage. A single rect, a full repaint and plugin
 *  mode are one pass over the damage's bounding box. */
template <class Rect>
std::vector<Rect> planRenderPasses(RenderPassInput<Rect> const& in)
{
    auto const single = std::vector<Rect> { in.damage.getIntersection(in.bounds) };
    if (in.rects.size() < 2 || in.pluginMode || in.maxPasses < 2)
        return single;
    if (single.front().contains(in.bounds))
        return single; // a full repaint

    std::vector<Rect> passes;
    passes.reserve(in.rects.size());
    for (auto const& r : in.rects) {
        auto const clipped = r.getIntersection(in.bounds);
        if (!clipped.isEmpty())
            passes.push_back(clipped);
    }

    detail::mergeNeighbours(passes, in.mergeCanvasDamage, in.canvasArea);
    while (passes.size() > in.maxPasses) {
        detail::mergeCheapestPair(passes);
        detail::mergeNeighbours(passes, in.mergeCanvasDamage, in.canvasArea); // the bigger one may now touch others
    }

    if (passes.empty())
        return single;
    return passes;
}

/** Add one damage rect to the frame's list: a rect the list already covers is dropped, the ones it covers are replaced.
 *  More than maxRects separate rects fall back to `boundingBox` alone (the bounding box of everything added, this rect
 *  included). Several repaints of one widget in a frame (25 Hz pd-lua plus its own repaint) stay one rect. */
template <class Rect>
void accumulateDamage(std::vector<Rect>& rects, Rect const& area, Rect const& boundingBox, std::size_t maxRects)
{
    if (area.isEmpty())
        return;
    for (auto const& r : rects)
        if (r.contains(area))
            return;
    rects.erase(std::remove_if(rects.begin(), rects.end(), [&](Rect const& r) { return area.contains(r); }), rects.end());
    if (rects.size() < maxRects)
        rects.push_back(area);
    else
        rects.assign(1, boundingBox);
}

} // namespace mcp
