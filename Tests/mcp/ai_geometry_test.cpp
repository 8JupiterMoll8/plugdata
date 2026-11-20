// Unit tests for the AI overlay geometry (Source/Mcp/McpAiGeometry.h), JUCE-free (live review 2026-11-19):
//  - a tag's leader line is at most 14 px long (it was a long wire that crossed a box); a longer reach is cut at the tag's end;
//  - a tag stands right of the boxes it speaks about; when that is taken it goes above, then below, then left, always
//    14 px from the boxes, and never over a box (the tag AND its leader are clear of every other box); with no free side
//    the caller keeps the place the bridge chose;
//  - a region's name stands at the top left of its rectangle, and moves to the top right when a wire or a step disc would
//    cover it there (and stays left when the right is covered too, or the rectangle is only a title);
//  - that decision is cheap and cached (review 2026-11-19, the artist's slow canvas): aiRegionTitlePlaceBy asks "is this
//    spot covered?" for the left spot first and for the right only when the left is covered (never for a name that
//    cannot move); a wire is tested as the flattened segments it is (aiWireSegmentOnTitle), not as samples every 4 px;
//    AiTitleSideCache remembers the side per region and asks again only when its key (a hash of the inputs) changed, and
//    says when a remembered side flipped (the caller then repaints the name's band: a partial repaint would tear it);
//  - a tag's place leaves out the boxes it speaks about by index (`skip`), so one list of all boxes serves every tag of a paint.
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpAiGeometry.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

using mcp::AiPointF;
using mcp::AiRectF;
using mcp::AiTagSide;
using mcp::AiTitlePlace;

namespace {

float dist(AiPointF a, AiPointF b) { return std::hypot(a.x - b.x, a.y - b.y); }
bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }

struct Lcg {
    std::uint32_t s;
    int next(int n)
    {
        s = s * 1664525u + 1013904223u;
        return static_cast<int>((s >> 8) % static_cast<std::uint32_t>(n));
    }
};

// A segment passes through a rectangle (sampled: fine enough for boxes of 20 px or more).
bool segmentTouches(AiPointF a, AiPointF b, AiRectF r)
{
    for (int i = 0; i <= 64; ++i) {
        float const t = static_cast<float>(i) / 64.0f;
        float const x = a.x + (b.x - a.x) * t;
        float const y = a.y + (b.y - a.y) * t;
        if (x > r.x && x < r.x + r.w && y > r.y && y < r.y + r.h)
            return true;
    }
    return false;
}

void leaderIsCappedAt14()
{
    CHECK(mcp::aiTagLeaderMaxPx == 14.0f);
    AiRectF const group { 0, 0, 100, 50 };
    // A tag 14 px right of the boxes: the leader is the whole 14 px, from the box edge to the tag edge.
    auto const near14 = mcp::aiTagLeader(group, { 114, 16, 80, 18 });
    CHECK(near14.visible);
    CHECK(near(near14.from.x, 100.0f) && near(near14.to.x, 114.0f) && near(dist(near14.from, near14.to), 14.0f));
    // A tag 200 px away: the leader is cut to 14 px and ends where the tag begins.
    auto const far = mcp::aiTagLeader(group, { 300, 16, 80, 18 });
    CHECK(far.visible);
    CHECK(near(dist(far.from, far.to), mcp::aiTagLeaderMaxPx));
    CHECK(near(far.to.x, 300.0f));
    CHECK(near(far.from.x, 286.0f));
    // Diagonal reach (tag below and right): still at most 14 px.
    auto const diag = mcp::aiTagLeader(group, { 160, 140, 80, 18 });
    CHECK(diag.visible && dist(diag.from, diag.to) <= mcp::aiTagLeaderMaxPx + 0.01f);
    // A tag that touches the boxes (or is inside the 1 px reach): no leader.
    auto const touching = mcp::aiTagLeader(group, { 100, 16, 80, 18 });
    CHECK(!touching.visible);
    // Any reach: never longer than 14.
    Lcg rng { 99u };
    for (int i = 0; i < 300; ++i) {
        AiRectF const g { static_cast<float>(rng.next(300)), static_cast<float>(rng.next(300)), 20.0f + static_cast<float>(rng.next(150)), 20.0f + static_cast<float>(rng.next(100)) };
        AiRectF const t { static_cast<float>(rng.next(900)), static_cast<float>(rng.next(900)), 30.0f + static_cast<float>(rng.next(200)), 18.0f };
        auto const l = mcp::aiTagLeader(g, t);
        CHECK(dist(l.from, l.to) <= mcp::aiTagLeaderMaxPx + 0.01f);
    }
}

void tagStandsRightWhenFree()
{
    AiRectF const group { 100, 100, 80, 24 }; // centre y 112
    auto const p = mcp::aiTagPlace(group, 120.0f, 18.0f, 0, {});
    CHECK(p.side == AiTagSide::Right);
    CHECK(near(p.tag.x, 180.0f + mcp::aiTagLeaderPx) && near(p.tag.y, 112.0f - 9.0f));
    CHECK(near(p.tag.w, 120.0f) && near(p.tag.h, 18.0f));
    // A second tag on the same boxes stands one line (18 + 2) lower.
    auto const second = mcp::aiTagPlace(group, 120.0f, 18.0f, 1, {});
    CHECK(second.side == AiTagSide::Right && near(second.tag.y, p.tag.y + 20.0f));
}

void tagGoesAboveWhenTheRightIsTaken()
{
    // The live picture: the tag's right is blocked by another box ([t b] next to [expr]).
    AiRectF const group { 100, 300, 140, 28 };
    std::vector<AiRectF> const obstacles { { 270, 300, 40, 28 } }; // the box on the right, 30 px away
    auto const p = mcp::aiTagPlace(group, 280.0f, 18.0f, 0, obstacles);
    CHECK(p.side == AiTagSide::Above);
    CHECK(near(p.tag.x, group.x));                                       // left-aligned with the boxes
    CHECK(near(p.tag.y + p.tag.h + mcp::aiTagLeaderPx, group.y));        // 14 px above them
    auto const l = mcp::aiTagLeader(group, p.tag);
    CHECK(l.visible && dist(l.from, l.to) <= mcp::aiTagLeaderMaxPx + 0.01f);
    for (auto const& o : obstacles)
        CHECK(!segmentTouches(l.from, l.to, o));
}

void tagLeaderNeverCrossesABox()
{
    // A small box (a comment, a bang) sits in the 14 px between the boxes and where the tag would stand on the right: the
    // tag itself would be free, but its leader would run through that box, so the tag goes above instead.
    AiRectF const group { 100, 100, 80, 24 };
    std::vector<AiRectF> const gapBox { { 186, 106, 6, 12 } };
    auto const p = mcp::aiTagPlace(group, 120.0f, 18.0f, 0, gapBox);
    CHECK(p.side == AiTagSide::Above);
    auto const l = mcp::aiTagLeader(group, p.tag);
    CHECK(!segmentTouches(l.from, l.to, gapBox[0]));
}

void tagAboveStepsRightPastARegionName()
{
    // The live showcase (owner 2026-11-20): ai.draw's right is taken by ai.tap, and above its left edge stands the region's
    // name band. The tag goes above, right-aligned with the boxes, past the name (it used to be flung 480 px away).
    AiRectF const group { 40, 50, 320, 280 };
    std::vector<AiRectF> const obstacles { { 460, 50, 360, 220 }, { 28, 22, 110, 22 } };
    auto const p = mcp::aiTagPlace(group, 170.0f, 18.0f, 0, obstacles);
    CHECK(p.side == AiTagSide::Above);
    CHECK(near(p.tag.x + p.tag.w, group.x + group.w));            // right-aligned with the boxes
    CHECK(near(p.tag.y + p.tag.h + mcp::aiTagLeaderPx, group.y)); // 14 px above them
    for (auto const& o : obstacles)
        CHECK(!mcp::aiIntersects(p.tag, o));
    // Below works the same way: above fully taken, below-left taken -> below, right-aligned.
    std::vector<AiRectF> const more { { 460, 50, 360, 220 }, { 0, 0, 400, 40 }, { 28, 344, 110, 22 } };
    auto const q = mcp::aiTagPlace(group, 170.0f, 18.0f, 0, more);
    CHECK(q.side == AiTagSide::Below);
    CHECK(near(q.tag.x + q.tag.w, group.x + group.w));
}

void tagGoesBelowThenLeft()
{
    AiRectF const group { 200, 200, 100, 30 };
    std::vector<AiRectF> const rightAndAbove { { 330, 190, 50, 50 }, { 190, 150, 200, 30 } };
    auto const below = mcp::aiTagPlace(group, 150.0f, 18.0f, 0, rightAndAbove);
    CHECK(below.side == AiTagSide::Below);
    CHECK(near(below.tag.x, group.x) && near(below.tag.y, group.y + group.h + mcp::aiTagLeaderPx));
    std::vector<AiRectF> const rightAboveBelow { { 330, 190, 50, 50 }, { 190, 150, 200, 30 }, { 190, 250, 200, 30 } };
    auto const left = mcp::aiTagPlace(group, 150.0f, 18.0f, 0, rightAboveBelow);
    CHECK(left.side == AiTagSide::Left);
    CHECK(near(left.tag.x + left.tag.w + mcp::aiTagLeaderPx, group.x));
    // Every side taken: no place; the caller keeps the bridge's own.
    std::vector<AiRectF> const all { { 330, 190, 50, 50 }, { 190, 150, 200, 30 }, { 190, 250, 200, 30 }, { 20, 190, 130, 50 } };
    CHECK(mcp::aiTagPlace(group, 150.0f, 18.0f, 0, all).side == AiTagSide::None);
}

void tagNeverStandsOverABox()
{
    // Any layout: where a side is chosen, neither the tag nor the leader line touches a box, and the leader is <= 14 px.
    Lcg rng { 4242u };
    int placed = 0, unplaced = 0;
    for (int round = 0; round < 600; ++round) {
        AiRectF const group { 300.0f + static_cast<float>(rng.next(100)), 300.0f + static_cast<float>(rng.next(100)), 30.0f + static_cast<float>(rng.next(120)), 20.0f + static_cast<float>(rng.next(40)) };
        std::vector<AiRectF> boxes;
        int const n = rng.next(8);
        for (int i = 0; i < n; ++i) {
            // some boxes are small (a bang, a comment of one word): they can sit in the 14 px gap
            float const bw = rng.next(4) == 0 ? 6.0f + static_cast<float>(rng.next(10)) : 30.0f + static_cast<float>(rng.next(100));
            float const bh = rng.next(4) == 0 ? 6.0f + static_cast<float>(rng.next(10)) : 20.0f + static_cast<float>(rng.next(50));
            AiRectF const b { static_cast<float>(150 + rng.next(500)), static_cast<float>(150 + rng.next(450)), bw, bh };
            // boxes of the group itself are not obstacles: keep the generated ones off the group
            if (!mcp::aiIntersects(b, group))
                boxes.push_back(b);
        }
        float const tagW = 60.0f + static_cast<float>(rng.next(260));
        auto const p = mcp::aiTagPlace(group, tagW, 18.0f, 0, boxes);
        if (p.side == AiTagSide::None) {
            ++unplaced;
            continue;
        }
        ++placed;
        CHECK(near(p.tag.w, tagW) && near(p.tag.h, 18.0f));
        auto const l = mcp::aiTagLeader(group, p.tag);
        CHECK(dist(l.from, l.to) <= mcp::aiTagLeaderMaxPx + 0.01f);
        for (auto const& b : boxes) {
            CHECK(!mcp::aiIntersects(p.tag, b));
            CHECK(!segmentTouches(l.from, l.to, b));
        }
        CHECK(!mcp::aiIntersects(p.tag, group));
    }
    CHECK(placed > 100 && unplaced >= 0); // the generator does exercise the placement
}

void titleStandsTopLeft()
{
    AiRectF const rect { 100, 200, 300, 400 };
    auto const t = mcp::aiRegionTitlePlace(rect, 80.0f, 15.0f, {});
    CHECK(!t.right);
    CHECK(near(t.x, 100.0f + mcp::aiRegionTitleInset) && near(t.y, 200.0f + mcp::aiRegionTitleTop));
    // A cover far from the name (down the rectangle, or beside it) leaves it where it is.
    CHECK(!mcp::aiRegionTitlePlace(rect, 80.0f, 15.0f, { { 120, 300, 10, 10 }, { 500, 200, 20, 20 } }).right);
}

void titleMovesRightWhenCovered()
{
    AiRectF const rect { 100, 200, 300, 400 };
    // A step disc (14 px) on the name's left spot.
    auto const disc = mcp::aiRegionTitlePlace(rect, 80.0f, 15.0f, { { 130, 204, 14, 14 } });
    CHECK(disc.right);
    CHECK(near(disc.x, 100.0f + 300.0f - mcp::aiRegionTitleInset - 80.0f) && near(disc.y, 200.0f + mcp::aiRegionTitleTop));
    // A wire sampled as small rectangles through the name.
    std::vector<AiRectF> wire;
    for (int i = 0; i < 12; ++i)
        wire.push_back({ 148.0f, 200.0f + static_cast<float>(i) * 4.0f, 3.0f, 4.0f });
    CHECK(mcp::aiRegionTitlePlace(rect, 80.0f, 15.0f, wire).right);
    // Covered on the right as well: it stays on the left (it never jumps to a place that is no better).
    auto const both = mcp::aiRegionTitlePlace(rect, 80.0f, 15.0f, { { 130, 204, 14, 14 }, { 330, 204, 14, 14 } });
    CHECK(!both.right);
}

void titleStaysWhereItMust()
{
    // A rectangle that is only a title (empty): always the left place, whatever covers it.
    AiRectF const empty { 100, 200, 0, 0 };
    auto const e = mcp::aiRegionTitlePlace(empty, 80.0f, 15.0f, { { 100, 200, 100, 30 } });
    CHECK(!e.right && near(e.x, 100.0f + mcp::aiRegionTitleInset));
    // A rectangle too narrow for an inner right spot (owner 2026-11-19: regions have no fill, a name may leave its
    // rectangle): a covered name steps right by aiRegionTitleNudge when that is clear, else it stays left.
    AiRectF const narrow { 100, 200, 90, 300 };
    auto const n = mcp::aiRegionTitlePlace(narrow, 80.0f, 15.0f, { { 100, 204, 14, 14 } });
    CHECK(n.right && near(n.x, 100.0f + mcp::aiRegionTitleInset + mcp::aiRegionTitleNudge));
    CHECK(!mcp::aiRegionTitlePlace(narrow, 80.0f, 15.0f, { { 100, 204, 14, 14 }, { 150, 204, 14, 14 } }).right);
    // aiRegionTitleAt draws a remembered side where aiRegionTitlePlace put it (wide: inner right; narrow: the nudge).
    CHECK(near(mcp::aiRegionTitleAt(narrow, 80.0f, true).x, n.x));
    AiRectF const wide { 100, 200, 300, 400 };
    CHECK(near(mcp::aiRegionTitleAt(wide, 80.0f, true).x, mcp::aiRegionTitleSpot(wide, 80.0f, true).x));
    // A cover below the name's line (a wire under it) does not move it.
    CHECK(!mcp::aiRegionTitlePlace({ 100, 200, 300, 400 }, 80.0f, 15.0f, { { 110, 230, 14, 14 } }).right);
}


// ------------------------------------------------------------------------------------- cheap + cached region names

void titlePlaceByAsksLazily()
{
    AiRectF const rect { 100, 200, 300, 400 };
    int calls = 0;
    // Nothing covers the left spot: one question, the right spot is never asked.
    auto const free = mcp::aiRegionTitlePlaceBy(rect, 80.0f, 15.0f, [&](AiRectF) { ++calls; return false; });
    CHECK(!free.right && calls == 1);
    // The left spot is covered, the right is clear: two questions, the name moves.
    calls = 0;
    AiRectF asked[2];
    auto const moved = mcp::aiRegionTitlePlaceBy(rect, 80.0f, 15.0f, [&](AiRectF r) { asked[calls < 2 ? calls : 1] = r; return ++calls == 1; });
    CHECK(moved.right && calls == 2);
    CHECK(near(asked[0].x, 108.0f) && near(asked[0].y, 206.0f) && near(asked[0].w, 80.0f) && near(asked[0].h, 15.0f)); // left spot first
    CHECK(near(asked[1].x, 100.0f + 300.0f - mcp::aiRegionTitleInset - 80.0f) && near(asked[1].y, 206.0f));              // then the right
    // Both covered: it stays left.
    calls = 0;
    CHECK(!mcp::aiRegionTitlePlaceBy(rect, 80.0f, 15.0f, [&](AiRectF) { ++calls; return true; }).right && calls == 2);
    // A title-only rectangle asks nothing at all; a narrow one asks the left spot, then the nudge only.
    calls = 0;
    CHECK(!mcp::aiRegionTitlePlaceBy(AiRectF { 100, 200, 0, 0 }, 80.0f, 15.0f, [&](AiRectF) { ++calls; return true; }).right);
    CHECK(calls == 0);
    CHECK(!mcp::aiRegionTitlePlaceBy(AiRectF { 100, 200, 90, 300 }, 80.0f, 15.0f, [&](AiRectF) { ++calls; return true; }).right);
    CHECK(calls == 2);
}

void titlePlaceByEqualsTheVectorVersion()
{
    // aiRegionTitlePlace (a list of covers) is aiRegionTitlePlaceBy with "any cover touches the spot".
    Lcg rng { 777u };
    for (int round = 0; round < 500; ++round) {
        AiRectF const rect { static_cast<float>(rng.next(200)), static_cast<float>(rng.next(200)), static_cast<float>(rng.next(5) == 0 ? 0 : 60 + rng.next(400)), static_cast<float>(rng.next(300)) };
        float const titleW = 20.0f + static_cast<float>(rng.next(120));
        std::vector<AiRectF> covers;
        int const n = rng.next(5);
        for (int i = 0; i < n; ++i)
            covers.push_back({ static_cast<float>(rng.next(700)), static_cast<float>(rng.next(260)), 3.0f + static_cast<float>(rng.next(20)), 3.0f + static_cast<float>(rng.next(20)) });
        auto const a = mcp::aiRegionTitlePlace(rect, titleW, 15.0f, covers);
        auto const b = mcp::aiRegionTitlePlaceBy(rect, titleW, 15.0f, [&](AiRectF spot) {
            for (auto const& c : covers)
                if (mcp::aiIntersects(spot, c))
                    return true;
            return false;
        });
        CHECK(a.right == b.right && near(a.x, b.x) && near(a.y, b.y));
    }
}

void titleSpotsAndMobility()
{
    AiRectF const rect { 100, 200, 300, 400 };
    auto const l = mcp::aiRegionTitleSpot(rect, 80.0f, false);
    auto const r = mcp::aiRegionTitleSpot(rect, 80.0f, true);
    CHECK(!l.right && near(l.x, 108.0f) && near(l.y, 206.0f));
    CHECK(r.right && near(r.x, 100.0f + 300.0f - mcp::aiRegionTitleInset - 80.0f) && near(r.y, 206.0f));
    // The same spots aiRegionTitlePlaceBy answers with.
    auto const byLeft = mcp::aiRegionTitlePlaceBy(rect, 80.0f, 15.0f, [](AiRectF) { return false; });
    CHECK(byLeft.right == l.right && near(byLeft.x, l.x) && near(byLeft.y, l.y));
    // Only a rectangle with room for the name twice can move it.
    CHECK(mcp::aiRegionTitleCanMove(rect, 80.0f));
    CHECK(!mcp::aiRegionTitleCanMove({ 100, 200, 0, 0 }, 80.0f));   // a title only
    CHECK(!mcp::aiRegionTitleCanMove({ 100, 200, 90, 300 }, 80.0f)); // too narrow
    CHECK(!mcp::aiRegionTitleCanMove({ 100, 200, 183, 300 }, 80.0f)); // 8 + 80 + 8 + 80 + 8 = 184 px are needed
    CHECK(mcp::aiRegionTitleCanMove({ 100, 200, 184, 300 }, 80.0f));
    // ... and a free left spot is asked once in every rectangle with a width (none in a title-only one).
    for (float w = 0.0f; w < 400.0f; w += 7.0f) {
        int calls = 0;
        mcp::aiRegionTitlePlaceBy(AiRectF { 100, 200, w, 300 }, 80.0f, 15.0f, [&](AiRectF) { ++calls; return false; });
        CHECK(calls == (w > 0.0f ? 1 : 0));
    }
}

void wireSegmentsCoverTheName()
{
    AiRectF const title { 108, 206, 80, 15 }; // the name's left spot in a rectangle at (100, 200)
    // A wire running down through the name, one along its line, one clipping its corner.
    CHECK(mcp::aiWireSegmentOnTitle({ 140, 150 }, { 140, 300 }, title));
    CHECK(mcp::aiWireSegmentOnTitle({ 50, 213 }, { 400, 213 }, title));
    CHECK(mcp::aiWireSegmentOnTitle({ 90, 200 }, { 110, 208 }, title));
    // The cord is 4 px thick (2 px each side): a segment 1.5 px beside the name touches it, one 3 px beside does not.
    CHECK(mcp::aiWireSegmentOnTitle({ 106.5f, 150 }, { 106.5f, 300 }, title));
    CHECK(!mcp::aiWireSegmentOnTitle({ 105.0f, 150 }, { 105.0f, 300 }, title));
    // A wire above, below, beside, or one that ends before the name, leaves it.
    CHECK(!mcp::aiWireSegmentOnTitle({ 50, 190 }, { 400, 190 }, title));
    CHECK(!mcp::aiWireSegmentOnTitle({ 50, 240 }, { 400, 240 }, title));
    CHECK(!mcp::aiWireSegmentOnTitle({ 200, 150 }, { 200, 300 }, title));
    CHECK(!mcp::aiWireSegmentOnTitle({ 50, 213 }, { 90, 213 }, title));
    // A segment of no length is a point.
    CHECK(mcp::aiWireSegmentOnTitle({ 140, 213 }, { 140, 213 }, title));
    CHECK(!mcp::aiWireSegmentOnTitle({ 140, 100 }, { 140, 100 }, title));
}

void segmentTestCoversWhatTheSamplesDid()
{
    // The old test sampled a wire every 4 px as 4 px squares; the segment test must say "covered" wherever a sample square
    // lay on the name (so a name never lands on a wire that the old test would have avoided).
    Lcg rng { 31337u };
    int sampledHits = 0;
    for (int round = 0; round < 2000; ++round) {
        AiRectF const title { static_cast<float>(100 + rng.next(300)), static_cast<float>(100 + rng.next(300)), 30.0f + static_cast<float>(rng.next(120)), 15.0f };
        AiPointF const a { static_cast<float>(rng.next(700)), static_cast<float>(rng.next(700)) };
        AiPointF const b { static_cast<float>(rng.next(700)), static_cast<float>(rng.next(700)) };
        float const len = dist(a, b);
        bool sampled = false;
        for (float at = 0.0f; at <= len && !sampled; at += 4.0f) {
            float const t = len > 0.0f ? at / len : 0.0f;
            AiPointF const p { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t };
            sampled = mcp::aiIntersects(title, { p.x - 2.0f, p.y - 2.0f, 4.0f, 4.0f });
        }
        if (sampled) {
            ++sampledHits;
            CHECK(mcp::aiWireSegmentOnTitle(a, b, title));
        }
    }
    CHECK(sampledHits > 100); // the generator does put wires across names
}

void hashIsDeterministicAndSensitive()
{
    using mcp::aiHashMix;
    std::uint64_t const seed = mcp::aiHashSeed;
    CHECK(aiHashMix(seed, 5) == aiHashMix(seed, 5));
    CHECK(aiHashMix(seed, 5) != aiHashMix(seed, 6));
    CHECK(aiHashMix(aiHashMix(seed, 1), 2) != aiHashMix(aiHashMix(seed, 2), 1)); // order matters
    CHECK(aiHashMix(seed, 1.5f) == aiHashMix(seed, 1.5f));
    CHECK(aiHashMix(seed, 1.5f) != aiHashMix(seed, 1.5001f)); // a moved wire changes the key
    CHECK(aiHashMix(seed, 0.0f) != aiHashMix(seed, 1.0f));
    CHECK(aiHashMix(seed, std::uint64_t { 7 }) != seed);
    // Mixing a count first keeps {1, 2} apart from {1} {2}.
    CHECK(aiHashMix(aiHashMix(aiHashMix(seed, 2), 1), 2) != aiHashMix(aiHashMix(aiHashMix(seed, 1), 1), 2));
}

void sideCacheAsksOnlyWhenTheKeyChanges()
{
    mcp::AiTitleSideCache cache;
    int computed = 0;
    bool answer = false;
    auto const ask = [&](std::size_t slot, std::uint64_t key) {
        return cache.get(slot, key, [&] { ++computed; return answer; });
    };
    // The first paint of a slot asks once; the same key afterwards never asks again.
    auto r = ask(0, 11);
    CHECK(computed == 1 && !r.right && r.recomputed && !r.changed);
    for (int i = 0; i < 100; ++i) {
        r = ask(0, 11);
        CHECK(!r.right && !r.recomputed && !r.changed);
    }
    CHECK(computed == 1);
    // A new key asks again; the same answer is no change (no repaint is needed).
    r = ask(0, 12);
    CHECK(computed == 2 && r.recomputed && !r.changed && !r.right);
    // A new key that moves the name to the right: changed = true, once.
    answer = true;
    r = ask(0, 13);
    CHECK(computed == 3 && r.right && r.recomputed && r.changed);
    r = ask(0, 13);
    CHECK(computed == 3 && r.right && !r.changed);
    // And back to the left: changed again.
    answer = false;
    r = ask(0, 14);
    CHECK(computed == 4 && !r.right && r.changed);
    // A slot that was never drawn has nothing on the screen to tear: its first answer is not a change, even on the right.
    answer = true;
    r = ask(5, 1);
    CHECK(r.right && r.recomputed && !r.changed);
    // Slots are independent.
    CHECK(!ask(0, 14).right && ask(5, 1).right);
    CHECK(computed == 5);
}

void sideCacheKeepsAnUnpaintedSlot()
{
    // A region culled from a paint (outside the invalid area) is not asked about: its entry stays as it was drawn, so
    // when it is painted again after a change, "changed" compares with what is on the screen.
    mcp::AiTitleSideCache cache;
    bool answer = false;
    auto const ask = [&](std::size_t slot, std::uint64_t key) { return cache.get(slot, key, [&] { return answer; }); };
    ask(3, 1);
    ask(4, 1);
    answer = true;
    auto const r = ask(3, 2); // slot 4 was not painted in between
    CHECK(r.changed && r.right);
    CHECK(!ask(4, 1).recomputed); // still remembered
    // trim: entries beyond the number of regions go; the rest stay.
    cache.trim(4);
    CHECK(ask(4, 1).recomputed); // gone: asked again, no "change" because nothing is remembered
    CHECK(!ask(3, 2).recomputed);
    cache.clear();
    CHECK(ask(3, 2).recomputed);
}

void tagPlaceSkipsItsOwnBoxesByIndex()
{
    // One list of every box serves all tags of a paint; the boxes a tag speaks about are skipped by index (they are the
    // group itself: its edge is where the leader starts, so they must never count as obstacles).
    AiRectF const group { 100, 100, 80, 24 };
    std::vector<AiRectF> const all { { 100, 100, 80, 24 }, { 186, 100, 40, 24 }, { 400, 400, 40, 24 } };
    // The target is skipped, the neighbour 6 px to its right blocks the right place: the tag goes above.
    CHECK(mcp::aiTagPlace(group, 120.0f, 18.0f, 0, all, { 0 }).side == AiTagSide::Above);
    // The neighbour is another box the tag speaks about: skipped too, the right is free.
    auto const right = mcp::aiTagPlace(group, 120.0f, 18.0f, 0, all, { 0, 1 });
    CHECK(right.side == AiTagSide::Right && near(right.tag.x, 180.0f + mcp::aiTagLeaderPx));
    // An index past the list is ignored.
    CHECK(mcp::aiTagPlace(group, 120.0f, 18.0f, 0, all, { 0, 1, 99 }).side == AiTagSide::Right);
    // Equal to removing the skipped boxes from the list (the old code built that list per tag).
    Lcg rng { 2026u };
    for (int round = 0; round < 400; ++round) {
        AiRectF const g { 300.0f + static_cast<float>(rng.next(100)), 300.0f + static_cast<float>(rng.next(100)), 30.0f + static_cast<float>(rng.next(120)), 20.0f + static_cast<float>(rng.next(40)) };
        std::vector<AiRectF> boxes;
        std::vector<std::size_t> skip;
        std::vector<AiRectF> filtered;
        int const n = 1 + rng.next(10);
        for (int i = 0; i < n; ++i) {
            AiRectF const b { static_cast<float>(150 + rng.next(500)), static_cast<float>(150 + rng.next(450)), 6.0f + static_cast<float>(rng.next(120)), 6.0f + static_cast<float>(rng.next(60)) };
            if (rng.next(3) == 0)
                skip.push_back(boxes.size());
            else
                filtered.push_back(b);
            boxes.push_back(b);
        }
        float const tagW = 60.0f + static_cast<float>(rng.next(260));
        int const stack = rng.next(3);
        auto const a = mcp::aiTagPlace(g, tagW, 18.0f, stack, filtered);
        auto const b = mcp::aiTagPlace(g, tagW, 18.0f, stack, boxes, skip);
        CHECK(a.side == b.side && near(a.tag.x, b.tag.x) && near(a.tag.y, b.tag.y));
    }
}

} // namespace

int main()
{
    leaderIsCappedAt14();
    tagStandsRightWhenFree();
    tagGoesAboveWhenTheRightIsTaken();
    tagLeaderNeverCrossesABox();
    tagAboveStepsRightPastARegionName();
    tagGoesBelowThenLeft();
    tagNeverStandsOverABox();
    titleStandsTopLeft();
    titleMovesRightWhenCovered();
    titleStaysWhereItMust();
    titlePlaceByAsksLazily();
    titlePlaceByEqualsTheVectorVersion();
    titleSpotsAndMobility();
    wireSegmentsCoverTheName();
    segmentTestCoversWhatTheSamplesDid();
    hashIsDeterministicAndSensitive();
    sideCacheAsksOnlyWhenTheKeyChanges();
    sideCacheKeepsAnUnpaintedSlot();
    tagPlaceSkipsItsOwnBoxesByIndex();
    std::printf(failures ? "ai_geometry_test: %d FAILED\n" : "ai_geometry_test: OK\n", failures);
    return failures ? 1 : 0;
}
