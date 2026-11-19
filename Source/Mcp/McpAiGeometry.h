/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/
#pragma once

// The places of the AI overlays that are decided by geometry alone (live review 2026-11-19), JUCE-free so that
// Tests/mcp/ai_geometry_test.cpp compiles it by itself; CanvasMcpNotes.cpp (tags) and CanvasMcpLayer.cpp (region names)
// turn their juce rectangles into these and draw where the answer says.
//
//  - aiTagLeader: the grey line from the boxes to their tag, at most aiTagLeaderMaxPx (14) long;
//  - aiTagPlace: right of the boxes, else above, else below, else left, always aiTagLeaderPx (14) away, never over a
//    box (the tag AND its leader are clear of every other box);
//  - aiRegionTitlePlace: the name at the top left of the region's rectangle, at the top right when a wire or a step
//    disc would cover it on the left.

#include "McpAiStyle.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace mcp {

struct AiPointF {
    float x = 0.0f, y = 0.0f;
};

struct AiRectF {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    constexpr float right() const { return x + w; }
    constexpr float bottom() const { return y + h; }
    constexpr float centreX() const { return x + w * 0.5f; }
    constexpr float centreY() const { return y + h * 0.5f; }
};

/** Two rectangles share area (touching edges do not count; an empty one shares none). */
constexpr bool aiIntersects(AiRectF a, AiRectF b)
{
    return a.w > 0.0f && a.h > 0.0f && b.w > 0.0f && b.h > 0.0f && a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}

constexpr AiRectF aiExpanded(AiRectF r, float by)
{
    return { r.x - by, r.y - by, r.w + 2.0f * by, r.h + 2.0f * by };
}

/** A segment passes through (or touches) a rectangle: slab test. */
inline bool aiSegmentHits(AiPointF a, AiPointF b, AiRectF r)
{
    float t0 = 0.0f, t1 = 1.0f;
    float const d[2] = { b.x - a.x, b.y - a.y };
    float const o[2] = { a.x, a.y };
    float const lo[2] = { r.x, r.y };
    float const hi[2] = { r.right(), r.bottom() };
    for (int i = 0; i < 2; ++i) {
        if (std::fabs(d[i]) < 1e-6f) {
            if (o[i] < lo[i] || o[i] > hi[i])
                return false;
            continue;
        }
        float ta = (lo[i] - o[i]) / d[i];
        float tb = (hi[i] - o[i]) / d[i];
        if (ta > tb)
            std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1)
            return false;
    }
    return true;
}

// ------------------------------------------------------------------------------------------------------- tags

struct AiLeader {
    AiPointF from, to; // from: on the boxes' side, to: at the tag
    bool visible = false;
};

constexpr float aiTagLeaderMaxPx = aiTagLeaderPx; // the longest a leader line ever is
constexpr float aiTagStackStepPx = aiTagHeightPx + 2.0f; // tags on the same boxes stand one line apart

/** The line from the nearest point of the boxes (`group`) to the nearest point of the tag. A tag within 1 px of the boxes
 *  has none. A longer reach is cut at the tag's end: the line is at most aiTagLeaderMaxPx long, so it can never read as a
 *  wire across the canvas (the live picture: a 40 px leader that crossed a box). */
inline AiLeader aiTagLeader(AiRectF group, AiRectF tag)
{
    AiPointF const from { std::clamp(tag.centreX(), group.x, group.right()), std::clamp(tag.centreY(), group.y, group.bottom()) };
    AiPointF const to { std::clamp(from.x, tag.x, tag.right()), std::clamp(from.y, tag.y, tag.bottom()) };
    float const len = std::hypot(from.x - to.x, from.y - to.y);
    AiLeader out { from, to, len > 1.0f };
    if (len > aiTagLeaderMaxPx) {
        float const k = aiTagLeaderMaxPx / len;
        out.from = { to.x + (from.x - to.x) * k, to.y + (from.y - to.y) * k };
    }
    return out;
}

enum class AiTagSide { Right, Above, Below, Left, None };

struct AiTagPlace {
    AiTagSide side = AiTagSide::None; // None: no free side, the caller keeps the place the bridge chose
    AiRectF tag;
};

/** Where a tag of `tagW` x `tagH` px stands next to the boxes it speaks about (`group`): right of them (vertically
 *  centred, the tag's own place for years), else above them, else below them (both left-aligned with the boxes), else
 *  left of them; always aiTagLeaderPx away. `stack` = how many tags already stand on the same boxes: each moves one line
 *  (aiTagStackStepPx) further along its side. A side is taken only when the tag (2 px of air around it) and its leader
 *  line touch no box in `obstacles` (the boxes the tag speaks about are not obstacles). */
inline AiTagPlace aiTagPlace(AiRectF group, float tagW, float tagH, int stack, std::vector<AiRectF> const& obstacles)
{
    float const step = static_cast<float>(stack) * (tagH + 2.0f);
    float const midY = group.centreY() - tagH * 0.5f;
    AiRectF const candidates[4] = {
        { group.right() + aiTagLeaderPx, midY + step, tagW, tagH },                // right
        { group.x, group.y - aiTagLeaderPx - tagH - step, tagW, tagH },            // above
        { group.x, group.bottom() + aiTagLeaderPx + step, tagW, tagH },            // below
        { group.x - aiTagLeaderPx - tagW, midY + step, tagW, tagH },               // left
    };
    AiTagSide const sides[4] = { AiTagSide::Right, AiTagSide::Above, AiTagSide::Below, AiTagSide::Left };
    for (int i = 0; i < 4; ++i) {
        auto const tag = candidates[i];
        auto const leader = aiTagLeader(group, tag);
        bool blocked = false;
        for (auto const& o : obstacles) {
            if (aiIntersects(aiExpanded(tag, 2.0f), o) || (leader.visible && aiSegmentHits(leader.from, leader.to, aiExpanded(o, 1.0f)))) {
                blocked = true;
                break;
            }
        }
        if (!blocked)
            return { sides[i], tag };
    }
    return {};
}

// ----------------------------------------------------------------------------------------------- region names

struct AiTitlePlace {
    float x = 0.0f, y = 0.0f; // the top left of the name
    bool right = false;       // moved to the top right of the rectangle
};

/** Where a region's name of `titleW` x `titleH` px stands: at the top left of `rect` (aiRegionTitleInset from the left
 *  edge, aiRegionTitleTop from the top), or at the top right (the same distance from the right edge) when something in
 *  `covers` (a wire sampled as small rectangles, a step disc) would lie on the name on the left and the right place is
 *  clear. A rectangle that is only a title (no width), one too narrow to hold the name twice, and a right place that
 *  is covered as well all keep the left place. */
inline AiTitlePlace aiRegionTitlePlace(AiRectF rect, float titleW, float titleH, std::vector<AiRectF> const& covers)
{
    AiTitlePlace const left { rect.x + aiRegionTitleInset, rect.y + aiRegionTitleTop, false };
    if (rect.w <= 0.0f)
        return left;
    AiTitlePlace const right { rect.right() - aiRegionTitleInset - titleW, left.y, true };
    if (right.x < left.x + titleW + aiRegionTitleInset)
        return left; // no room for two places: the name stays at the left
    auto const covered = [&](AiTitlePlace p) {
        for (auto const& c : covers)
            if (aiIntersects({ p.x, p.y, titleW, titleH }, c))
                return true;
        return false;
    };
    return covered(left) && !covered(right) ? right : left;
}

} // namespace mcp
