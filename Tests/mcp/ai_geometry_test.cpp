// Unit tests for the AI overlay geometry (Source/Mcp/McpAiGeometry.h), JUCE-free (live review 2026-11-19):
//  - a tag's leader line is at most 14 px long (it was a long wire that crossed a box); a longer reach is cut at the tag's end;
//  - a tag stands right of the boxes it speaks about; when that is taken it goes above, then below, then left, always
//    14 px from the boxes, and never over a box (the tag AND its leader are clear of every other box); with no free side
//    the caller keeps the place the bridge chose;
//  - a region's name stands at the top left of its rectangle, and moves to the top right when a wire or a step disc would
//    cover it there (and stays left when the right is covered too, or the rectangle is only a title).
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
    // A rectangle too narrow for the name on the right without lying on the left spot: stays left.
    AiRectF const narrow { 100, 200, 90, 300 };
    CHECK(!mcp::aiRegionTitlePlace(narrow, 80.0f, 15.0f, { { 118, 204, 14, 14 } }).right);
    // A cover below the name's line (a wire under it) does not move it.
    CHECK(!mcp::aiRegionTitlePlace({ 100, 200, 300, 400 }, 80.0f, 15.0f, { { 110, 230, 14, 14 } }).right);
}

} // namespace

int main()
{
    leaderIsCappedAt14();
    tagStandsRightWhenFree();
    tagGoesAboveWhenTheRightIsTaken();
    tagLeaderNeverCrossesABox();
    tagGoesBelowThenLeft();
    tagNeverStandsOverABox();
    titleStandsTopLeft();
    titleMovesRightWhenCovered();
    titleStaysWhereItMust();
    std::printf(failures ? "ai_geometry_test: %d FAILED\n" : "ai_geometry_test: OK\n", failures);
    return failures ? 1 : 0;
}
