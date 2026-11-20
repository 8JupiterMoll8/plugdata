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
//  - aiRegionTitlePlace / aiRegionTitlePlaceBy: the name at the top left of the region's rectangle, at the top right
//    when a wire or a step disc would cover it on the left; the By form asks lazily (the left spot first, the right
//    only when the left is covered), so a paint tests wires only when the name could move;
//  - aiWireSegmentOnTitle: a flattened wire segment lies on a name (the cord is 4 px thick);
//  - aiHashMix + AiTitleSideCache: the side of a name is remembered from paint to paint and asked again only when the
//    hash of its inputs changes; the cache says when a remembered side flipped (the caller repaints the name's band).

#include "McpAiStyle.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
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
 *  centred, the tag's own place for years), else above them, else below them (each left-aligned with the boxes, then
 *  right-aligned), else left of them; always aiTagLeaderPx away. `stack` = how many tags already stand on the same boxes: each moves one line
 *  (aiTagStackStepPx) further along its side. A side is taken only when the tag (2 px of air around it) and its leader
 *  line touch no box in `obstacles` (the boxes the tag speaks about are not obstacles: pass them in `skip`, as indices into
 *  `obstacles`, so that one list of every box serves all the tags of a paint; an index past the list is ignored). */
inline AiTagPlace aiTagPlace(AiRectF group, float tagW, float tagH, int stack, std::vector<AiRectF> const& obstacles, std::vector<std::size_t> const& skip = {})
{
    float const step = static_cast<float>(stack) * (tagH + 2.0f);
    float const midY = group.centreY() - tagH * 0.5f;
    // Above and below come twice: left-aligned with the boxes first, then right-aligned (owner 2026-11-20: a region's name
    // band takes the spot above the left edge, and a tag with no free spot used to be flung far away by the bridge).
    float const alignRight = group.right() - tagW;
    AiRectF const candidates[6] = {
        { group.right() + aiTagLeaderPx, midY + step, tagW, tagH },                // right
        { group.x, group.y - aiTagLeaderPx - tagH - step, tagW, tagH },            // above
        { alignRight, group.y - aiTagLeaderPx - tagH - step, tagW, tagH },         // above, right-aligned
        { group.x, group.bottom() + aiTagLeaderPx + step, tagW, tagH },            // below
        { alignRight, group.bottom() + aiTagLeaderPx + step, tagW, tagH },         // below, right-aligned
        { group.x - aiTagLeaderPx - tagW, midY + step, tagW, tagH },               // left
    };
    AiTagSide const sides[6] = { AiTagSide::Right, AiTagSide::Above, AiTagSide::Above, AiTagSide::Below, AiTagSide::Below, AiTagSide::Left };
    auto const skipped = [&skip](std::size_t index) { return std::find(skip.begin(), skip.end(), index) != skip.end(); };
    for (int i = 0; i < 6; ++i) {
        auto const tag = candidates[i];
        auto const leader = aiTagLeader(group, tag);
        bool blocked = false;
        for (std::size_t k = 0; k < obstacles.size(); ++k) {
            auto const& o = obstacles[k];
            if (!skip.empty() && skipped(k))
                continue;
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

/** The name's top left on one side of `rect`: the left spot (aiRegionTitleInset from the left edge, aiRegionTitleTop from the
 *  top) or the right spot (the same distance from the right edge). */
/** How far a covered name steps right when it cannot take the inner right spot: past a step disc (18 px) and its air. */
inline constexpr float aiRegionTitleNudge = 28.0f;

inline AiTitlePlace aiRegionTitleSpot(AiRectF rect, float titleW, bool right)
{
    return right ? AiTitlePlace { rect.right() - aiRegionTitleInset - titleW, rect.y + aiRegionTitleTop, true }
                 : AiTitlePlace { rect.x + aiRegionTitleInset, rect.y + aiRegionTitleTop, false };
}

/** Where a name stands for a remembered side: left; right = the inner right spot (wide) or the nudge (narrow). */
inline bool aiRegionTitleCanMove(AiRectF rect, float titleW);
inline AiTitlePlace aiRegionTitleAt(AiRectF rect, float titleW, bool right)
{
    if (!right)
        return aiRegionTitleSpot(rect, titleW, false);
    if (aiRegionTitleCanMove(rect, titleW))
        return aiRegionTitleSpot(rect, titleW, true);
    auto const left = aiRegionTitleSpot(rect, titleW, false);
    return AiTitlePlace { left.x + aiRegionTitleNudge, left.y, true };
}

/** A name can move to the other side only in a rectangle that is more than a title (some width) and has room for the name
 *  twice (the right spot must not lie on the left one). Otherwise it stays on the left, whatever is on it. */
inline bool aiRegionTitleCanMove(AiRectF rect, float titleW)
{
    if (rect.w <= 0.0f)
        return false;
    return !(aiRegionTitleSpot(rect, titleW, true).x < aiRegionTitleSpot(rect, titleW, false).x + titleW + aiRegionTitleInset);
}

/** Where a region's name of `titleW` x `titleH` px stands: at the top left of `rect` (aiRegionTitleSpot), or at the top
 *  right when `covered` (asked with the name's spot: a wire or a step disc lies on it) says so for the left spot and the
 *  right spot is clear. A rectangle where the name cannot move (aiRegionTitleCanMove), and a right place that is covered
 *  as well, keep the left place. `covered` is asked LAZILY: never for a name that cannot move, the left spot first, and
 *  the right spot only when the left is covered (a paint that finds nothing on the left tests no more). */
template <class Covered>
inline AiTitlePlace aiRegionTitlePlaceBy(AiRectF rect, float titleW, float titleH, Covered&& covered)
{
    auto const left = aiRegionTitleSpot(rect, titleW, false);
    if (rect.w <= 0.0f)
        return left;
    if (!covered(AiRectF { left.x, left.y, titleW, titleH }))
        return left;
    if (aiRegionTitleCanMove(rect, titleW)) {
        auto const right = aiRegionTitleSpot(rect, titleW, true);
        return covered(AiRectF { right.x, right.y, titleW, titleH }) ? left : right;
    }
    // Owner 2026-11-19 (regions have no fill any more): a name may leave its rectangle. In a region too narrow for an
    // inner right spot, a covered name steps right past a step disc or a cord (aiRegionTitleNudge) if that spot is clear.
    // So `right` means the inner right spot in a wide rectangle and the nudge in a narrow one (aiRegionTitleAt).
    auto const nudged = aiRegionTitleAt(rect, titleW, true);
    return covered(AiRectF { nudged.x, nudged.y, titleW, titleH }) ? left : nudged;
}

/** The same with the covers as a list of rectangles (a wire sampled as small rectangles, a step disc). */
inline AiTitlePlace aiRegionTitlePlace(AiRectF rect, float titleW, float titleH, std::vector<AiRectF> const& covers)
{
    return aiRegionTitlePlaceBy(rect, titleW, titleH, [&covers](AiRectF spot) {
        for (auto const& c : covers)
            if (aiIntersects(spot, c))
                return true;
        return false;
    });
}

constexpr float aiRegionWireHalfWidth = 2.0f; // a cord is drawn about 4 px thick

/** A segment of a flattened wire lies on a name's spot: the segment comes within the cord's half width of the spot. */
inline bool aiWireSegmentOnTitle(AiPointF a, AiPointF b, AiRectF title)
{
    return aiSegmentHits(a, b, aiExpanded(title, aiRegionWireHalfWidth));
}

// ------------------------------------------------------------------------------------- remembering a name's side

constexpr std::uint64_t aiHashSeed = 0xcbf29ce484222325ull;

/** One value mixed into a 64-bit hash (the key of a cached decision). Order matters; a float is mixed by its bits. */
inline std::uint64_t aiHashMix(std::uint64_t h, std::uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h *= 0xff51afd7ed558ccdull;
    return h ^ (h >> 32);
}
inline std::uint64_t aiHashMix(std::uint64_t h, int v) { return aiHashMix(h, static_cast<std::uint64_t>(static_cast<std::int64_t>(v))); }
inline std::uint64_t aiHashMix(std::uint64_t h, float v)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    return aiHashMix(h, static_cast<std::uint64_t>(bits));
}

/** The side each region's name stands on, remembered from one paint to the next. A slot is asked again (`compute`) only when
 *  its key (the caller's hash of everything the answer depends on: the rectangle, the name's size, the wires, the discs)
 *  changed since the slot was last asked. `changed` says that a name which was drawn on one side now belongs on the other:
 *  the render is scissored to the damaged area, so the caller repaints the name's band (a partial repaint would tear it).
 *  The first answer of a slot is never a change: nothing of it is on the screen yet. */
class AiTitleSideCache {
public:
    struct Answer {
        bool right = false;
        bool recomputed = false;
        bool changed = false;
    };

    template <class Compute>
    Answer get(std::size_t slot, std::uint64_t key, Compute&& compute)
    {
        if (slot >= entries_.size())
            entries_.resize(slot + 1);
        auto& e = entries_[slot];
        if (e.valid && e.key == key)
            return { e.right, false, false };
        bool const right = compute();
        bool const changed = e.valid && e.right != right;
        e = { key, right, true };
        return { right, true, changed };
    }
    /** Forget the slots at `slots` and beyond (the regions of the canvas became fewer). */
    void trim(std::size_t slots)
    {
        if (entries_.size() > slots)
            entries_.resize(slots);
    }
    void clear() { entries_.clear(); }

private:
    struct Entry {
        std::uint64_t key = 0;
        bool right = false;
        bool valid = false;
    };
    std::vector<Entry> entries_;
};

} // namespace mcp
