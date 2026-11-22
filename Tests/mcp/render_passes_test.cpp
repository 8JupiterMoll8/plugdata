// Unit tests for the render pass planner (repaint scope, 2026-11-22), JUCE-free (Source/Mcp/McpRenderPasses.h).
//
// Measured live (2026-11-22): about eight moving widgets spread over a wide canvas, each repainting only its own small
// rect, cost PlugData 180-200 % CPU (message thread and GL thread near 95 %). NVGSurface::renderPasses forced any two
// damage rects that touch the canvas into their bounding box, so two widgets at opposite ends redrew nearly the whole
// canvas every frame. The planner is now a pure function: far-apart canvas rects are separate passes, too many clusters
// are merged cheapest pair first (never into one bounding box), and a pass never paints more than the damage's
// bounding box. Presentation mode (whole-canvas quads in every pass) keeps the old one-region rule.
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpRenderPasses.h"

#include <cstdint>
#include <cstdio>

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

using mcp::PassRect;
using Input = mcp::RenderPassInput<PassRect>;

// A 1920 x 1080 editor: toolbar on top (0..40), the canvas below it, a status bar at the bottom.
static PassRect const bounds { 0, 0, 1920, 1080 };
static PassRect const canvasArea { 0, 40, 1920, 1000 };

static PassRect boundingBox(std::vector<PassRect> const& rects)
{
    PassRect box;
    for (auto const& r : rects)
        box = box.getUnion(r);
    return box;
}

static Input frame(std::vector<PassRect> rects)
{
    Input in;
    in.damage = boundingBox(rects);
    in.rects = std::move(rects);
    in.bounds = bounds;
    in.canvasArea = canvasArea;
    return in;
}

static std::int64_t totalArea(std::vector<PassRect> const& passes)
{
    std::int64_t sum = 0;
    for (auto const& p : passes)
        sum += static_cast<std::int64_t>(p.w) * p.h;
    return sum;
}

static bool covers(std::vector<PassRect> const& passes, PassRect const& r)
{
    for (auto const& p : passes)
        if (p.contains(r))
            return true;
    return false;
}

static void farApartCanvasRects()
{
    std::printf("planner: two small widgets at opposite ends of the canvas are two passes\n");
    auto const a = PassRect { 40, 200, 120, 90 };
    auto const b = PassRect { 1700, 800, 120, 90 };
    auto const passes = mcp::planRenderPasses(frame({ a, b }));
    CHECK(passes.size() == 2);
    CHECK(covers(passes, a) && covers(passes, b));
    // Far less than the bounding box between them (the old pass redrew ~1.7 million px for 21,600 px of damage).
    CHECK(totalArea(passes) < 40000);
    CHECK(totalArea(passes) * 20 < static_cast<std::int64_t>(boundingBox({ a, b }).w) * boundingBox({ a, b }).h);
}

static void eightWidgetsOwnerScenario()
{
    std::printf("planner: eight widgets spread over x 40..2300 are at most four passes, far below the bounding box\n");
    std::vector<PassRect> rects;
    for (int i = 0; i < 8; ++i)
        rects.push_back({ 40 + i * 230, 120 + (i % 3) * 260, 110, 80 });
    auto in = frame(rects);
    in.bounds = { 0, 0, 3000, 1080 };
    in.canvasArea = { 0, 40, 3000, 1000 };
    auto const passes = mcp::planRenderPasses(in);
    CHECK(mcp::maxRenderPasses == 4); // each pass is one flush on the GL thread (own scissor, own stencil clear)
    CHECK(passes.size() >= 2 && passes.size() <= 4);
    for (auto const& r : rects)
        CHECK(covers(passes, r));
    auto const box = boundingBox(rects);
    CHECK(totalArea(passes) * 2 < static_cast<std::int64_t>(box.w) * box.h);
}

static void spreadFrameBoundingBoxRule()
{
    std::printf("planner: a frame whose bounding box is at most 2.5x the passes' area is one pass\n");
    {
        // Two big rects on a diagonal: not cheap as a pair, but their bounding box is only 2.4x their area.
        auto const a = PassRect { 0, 0, 300, 300 };
        auto const b = PassRect { 350, 350, 300, 300 };
        auto const passes = mcp::planRenderPasses(frame({ a, b }));
        CHECK(passes.size() == 1);
        CHECK(passes[0] == a.getUnion(b));
    }
    {
        // Pushed apart the box is 2.7x the sum: two passes again.
        auto const a = PassRect { 0, 0, 300, 300 };
        auto const b = PassRect { 420, 420, 300, 300 };
        CHECK(mcp::planRenderPasses(frame({ a, b })).size() == 2);
    }
}

static void defaultCapIsFour()
{
    std::printf("planner: nine far-apart widgets merge cheapest pair first down to four passes\n");
    std::vector<PassRect> rects;
    for (int i = 0; i < 9; ++i)
        rects.push_back({ 40 + (i % 3) * 800, 120 + (i / 3) * 330, 90, 70 });
    auto in = frame(rects);
    in.bounds = { 0, 0, 3000, 1400 };
    in.canvasArea = { 0, 40, 3000, 1360 };
    auto const passes = mcp::planRenderPasses(in);
    CHECK(passes.size() >= 2 && passes.size() <= 4);
    for (auto const& r : rects)
        CHECK(covers(passes, r));
    auto const box = boundingBox(rects);
    CHECK(totalArea(passes) * 2 < static_cast<std::int64_t>(box.w) * box.h);
}

static void toolbarAndCanvasStaySeparate()
{
    std::printf("planner: the toolbar meter and Alphi's corner are two passes (the first split, unchanged)\n");
    auto const meter = PassRect { 1700, 4, 160, 30 };
    auto const alphi = PassRect { 1800, 940, 60, 70 };
    auto const passes = mcp::planRenderPasses(frame({ meter, alphi }));
    CHECK(passes.size() == 2);
    CHECK(covers(passes, meter) && covers(passes, alphi));
}

static void overlappingAndNearRectsMerge()
{
    std::printf("planner: overlapping and nearby rects still merge\n");
    {
        auto const a = PassRect { 100, 200, 200, 200 };
        auto const b = PassRect { 250, 300, 200, 200 };
        auto const passes = mcp::planRenderPasses(frame({ a, b }));
        CHECK(passes.size() == 1);
        CHECK(passes[0] == a.getUnion(b));
    }
    {
        // Two tiny rects 10 px apart: one pass is cheaper than two.
        auto const a = PassRect { 100, 200, 40, 40 };
        auto const b = PassRect { 150, 200, 40, 40 };
        auto const passes = mcp::planRenderPasses(frame({ a, b }));
        CHECK(passes.size() == 1);
    }
    {
        // A chain: the third touches the union of the first two, so all three become one.
        auto const a = PassRect { 100, 200, 100, 100 };
        auto const b = PassRect { 180, 200, 100, 100 };
        auto const c = PassRect { 260, 200, 100, 100 };
        auto const passes = mcp::planRenderPasses(frame({ a, b, c }));
        CHECK(passes.size() == 1);
        CHECK(passes[0] == PassRect({ 100, 200, 260, 100 }));
    }
}

static void tooManyClustersMergeCheapestFirst()
{
    std::printf("planner: more clusters than maxPasses merge the closest pair, never into one bounding box\n");
    std::vector<PassRect> rects;
    // Twelve widgets: two groups of five far apart in the grid corners, two loners.
    for (int i = 0; i < 5; ++i)
        rects.push_back({ 100 + i * 150, 100, 60, 60 });
    for (int i = 0; i < 5; ++i)
        rects.push_back({ 1000 + i * 150, 900, 60, 60 });
    rects.push_back({ 1800, 100, 60, 60 });
    rects.push_back({ 100, 900, 60, 60 });
    auto in = frame(rects);
    in.maxPasses = 8;
    auto const passes = mcp::planRenderPasses(in);
    CHECK(passes.size() <= 8);
    CHECK(passes.size() >= 2);
    for (auto const& r : rects)
        CHECK(covers(passes, r));
    // Never the one bounding pass the old code fell back to.
    CHECK(totalArea(passes) * 4 < static_cast<std::int64_t>(boundingBox(rects).w) * boundingBox(rects).h);

    in.maxPasses = 2;
    auto const two = mcp::planRenderPasses(in);
    CHECK(two.size() == 2);
    for (auto const& r : rects)
        CHECK(covers(two, r));
    CHECK(totalArea(two) < static_cast<std::int64_t>(boundingBox(rects).w) * boundingBox(rects).h);

    in.maxPasses = 1;
    auto const one = mcp::planRenderPasses(in);
    CHECK(one.size() == 1);
    CHECK(one[0] == boundingBox(rects));
}

static void fullRepaintPluginModeAndDegenerateInputs()
{
    std::printf("planner: a full repaint, plugin mode, one rect and empty damage are one pass\n");
    {
        auto const passes = mcp::planRenderPasses(frame({ bounds, { 10, 10, 50, 50 } }));
        CHECK(passes.size() == 1);
        CHECK(passes[0] == bounds);
    }
    {
        auto in = frame({ { 40, 200, 120, 90 }, { 1700, 800, 120, 90 } });
        in.pluginMode = true;
        auto const passes = mcp::planRenderPasses(in);
        CHECK(passes.size() == 1);
        CHECK(passes[0] == in.damage);
    }
    {
        auto const passes = mcp::planRenderPasses(frame({ { 40, 200, 120, 90 } }));
        CHECK(passes.size() == 1);
        CHECK(passes[0] == PassRect({ 40, 200, 120, 90 }));
    }
    {
        auto const passes = mcp::planRenderPasses(frame({}));
        CHECK(passes.size() == 1 && passes[0].isEmpty());
    }
    {
        // Damage outside the editor is clipped away; what is left is planned.
        auto const passes = mcp::planRenderPasses(frame({ { 40, 200, 120, 90 }, { 5000, 5000, 100, 100 }, { 1700, 800, 120, 90 } }));
        CHECK(passes.size() == 2);
        for (auto const& p : passes)
            CHECK(bounds.contains(p));
    }
    {
        // A rect that straddles the edge is clipped to it.
        auto const passes = mcp::planRenderPasses(frame({ { 1850, 10, 200, 40 }, { 100, 900, 50, 50 } }));
        CHECK(passes.size() == 2);
        for (auto const& p : passes)
            CHECK(bounds.contains(p));
    }
}

static void presentationModeKeepsOneCanvasRegion()
{
    std::printf("planner: mergeCanvasDamage (presentation mode) keeps all canvas damage in one region\n");
    auto const meter = PassRect { 1700, 4, 160, 30 }; // the toolbar: not on the canvas
    auto const a = PassRect { 40, 200, 120, 90 };
    auto const b = PassRect { 400, 300, 120, 90 }; // two widgets, too far apart to be one cheap pass
    auto in = frame({ meter, a, b });
    in.mergeCanvasDamage = true;
    auto const passes = mcp::planRenderPasses(in);
    CHECK(passes.size() == 2);
    CHECK(covers(passes, meter));
    CHECK(covers(passes, a.getUnion(b)));
    // The same frame without the flag is three passes.
    in.mergeCanvasDamage = false;
    CHECK(mcp::planRenderPasses(in).size() == 3);
}

// A tiny deterministic generator (no <random>: the sequence must not depend on the standard library).
struct Lcg {
    std::uint32_t s;
    int next(int n)
    {
        s = s * 1664525u + 1013904223u;
        return static_cast<int>((s >> 8) % static_cast<std::uint32_t>(n));
    }
};

static void invariants()
{
    std::printf("planner: invariants over 3000 random frames\n");
    Lcg rng { 12345u };
    for (int round = 0; round < 3000; ++round) {
        std::vector<PassRect> rects;
        int const count = 1 + rng.next(14);
        for (int i = 0; i < count; ++i) {
            int const w = 1 + rng.next(rng.next(4) == 0 ? 900 : 160);
            int const h = 1 + rng.next(rng.next(4) == 0 ? 500 : 120);
            rects.push_back({ rng.next(2100) - 100, rng.next(1200) - 60, w, h });
        }
        auto in = frame(rects);
        in.maxPasses = 1 + static_cast<std::size_t>(rng.next(9));
        in.mergeCanvasDamage = rng.next(5) == 0;
        in.pluginMode = rng.next(11) == 0;
        auto const passes = mcp::planRenderPasses(in);

        CHECK(!passes.empty());
        CHECK(passes.size() <= std::max<std::size_t>(in.maxPasses, 1));
        // Everything damaged inside the editor is painted.
        for (auto const& r : rects) {
            auto const clipped = r.getIntersection(bounds);
            if (!clipped.isEmpty())
                CHECK(covers(passes, clipped));
        }
        // Inside the editor, pairwise disjoint (a pixel is painted once) and never above the bounding box of the damage.
        for (std::size_t i = 0; i < passes.size(); ++i) {
            CHECK(bounds.contains(passes[i]));
            for (std::size_t j = i + 1; j < passes.size(); ++j)
                CHECK(!passes[i].intersects(passes[j]));
        }
        auto const clippedBox = in.damage.getIntersection(bounds);
        CHECK(totalArea(passes) <= static_cast<std::int64_t>(clippedBox.w) * clippedBox.h);
        // Deterministic.
        CHECK(mcp::planRenderPasses(in) == passes);
    }
}

static void accumulate()
{
    std::printf("accumulateDamage: contained rects are dropped, covered ones replaced, overflow is the bounding box\n");
    std::vector<PassRect> rects;
    auto const box = PassRect { 0, 0, 1000, 1000 };
    mcp::accumulateDamage(rects, PassRect { 10, 10, 50, 50 }, box, 4);
    mcp::accumulateDamage(rects, PassRect { 10, 10, 50, 50 }, box, 4); // the same widget repainted twice in a frame
    mcp::accumulateDamage(rects, PassRect { 20, 20, 10, 10 }, box, 4); // inside the first
    CHECK(rects.size() == 1);
    mcp::accumulateDamage(rects, PassRect { 0, 0, 100, 100 }, box, 4); // covers the first
    CHECK(rects.size() == 1 && rects[0] == PassRect({ 0, 0, 100, 100 }));
    mcp::accumulateDamage(rects, PassRect {}, box, 4); // empty: ignored
    CHECK(rects.size() == 1);
    mcp::accumulateDamage(rects, PassRect { 300, 0, 10, 10 }, box, 4);
    mcp::accumulateDamage(rects, PassRect { 600, 0, 10, 10 }, box, 4);
    mcp::accumulateDamage(rects, PassRect { 900, 0, 10, 10 }, box, 4);
    CHECK(rects.size() == 4);
    mcp::accumulateDamage(rects, PassRect { 900, 900, 10, 10 }, box, 4); // one too many: the bounding box
    CHECK(rects.size() == 1 && rects[0] == box);
}

int main()
{
    farApartCanvasRects();
    eightWidgetsOwnerScenario();
    defaultCapIsFour();
    spreadFrameBoundingBoxRule();
    toolbarAndCanvasStaySeparate();
    overlappingAndNearRectsMerge();
    tooManyClustersMergeCheapestFirst();
    fullRepaintPluginModeAndDegenerateInputs();
    presentationModeKeepsOneCanvasRegion();
    invariants();
    accumulate();
    if (failures == 0)
        std::printf("render_passes_test: all passed\n");
    else
        std::printf("render_passes_test: %d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
