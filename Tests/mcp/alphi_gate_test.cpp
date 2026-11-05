// Alphi drop 1, F0 (docs/plans/alphi-drop1-alive-steps.md in the MCP repo, R5 / S4): the 400 ms click gate vs. repaints.
// JUCE-free (Source/Mcp/McpAlphiDock.h, Source/Mcp/McpAlphiMotion.h).
//
// The 1a0 leftover (PROGRESS "gate-after-repaint", 3/11): a click on Alphi right after a canvas click was sometimes
// swallowed. Drop 1 repaints Alphi 30 times a second while sound plays and on every say, so the gate must never
// restart on a repaint, only when Alphi was hidden / covered or moved (on the canvas or on screen).
//  - 100 pulse repaints (Alphi's motion area, through the canvas's screen <-> canvas rounding at four zooms) and
//    10 say updates (full repaints from the say handler, strip-only repaints on expiry) keep shownSinceMs;
//  - a click 500 ms into playback counts;
//  - every pulse repaint redraws the whole frame (so it can never leave a half-drawn, unstarted Alphi) and is an
//    Alphi-only paint (the dock pass is skipped, review N3); a canvas repaint that only touches Alphi is not;
//  - what does restart the gate: covered (chip stack / pill), a move, a screen move; never a plain repaint.
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpAlphiMotion.h"

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

using mcp::AlphiCorner;
using mcp::ApprovalRect;

namespace {

int roundInt(double v) { return static_cast<int>(std::lround(v)); }

// The canvas's view of a 1200 x 800 screen-pixel window at `zoom`, scrolled to (ox, oy) canvas units.
ApprovalRect viewOf(int ox, int oy, float zoom)
{
    return mcp::approvalVisibleArea(static_cast<float>(ox), static_cast<float>(oy), 1200.0f / zoom, 800.0f / zoom);
}

// What Canvas::performRender hands paintUnderObjects for a repaint of canvas area `a`: canvas -> surface pixels
// (nvgSurface.getLocalArea), then back (translated by the view position, `/= zoom`). JUCE rounds x, y, w and h of an
// int rectangle each on its own (copyWithRounding), both ways: the worst case loses a pixel per edge.
ApprovalRect renderRegionOf(ApprovalRect a, ApprovalRect view, float zoom)
{
    double const z = zoom;
    int const sx = roundInt((a.x - view.x) * z), sy = roundInt((a.y - view.y) * z);
    int const sw = roundInt(a.w * z), sh = roundInt(a.h * z);
    double const vx = view.x * z, vy = view.y * z; // the viewport's view position (surface pixels)
    return { roundInt((sx + vx) / z), roundInt((sy + vy) / z), roundInt(sw / z), roundInt(sh / z) };
}

// One paint of Alphi as CanvasMcpLayer::paintAlphi runs it (the gate part only): Alphi is uncovered and did not move.
struct Painter {
    ApprovalRect frame;
    std::int64_t shownSinceMs = 0;
    void paint(ApprovalRect region, std::int64_t nowMs, bool covered = false, bool screenMoved = false)
    {
        mcp::AlphiGateInput in;
        in.covered = covered;
        in.screenMoved = screenMoved;
        in.frameInRegion = mcp::alphiRectContains(region, frame);
        shownSinceMs = mcp::alphiGateAfterPaint(shownSinceMs, nowMs, in);
    }
};

} // namespace

static void pulseAndSayKeepGate()
{
    std::printf("F0: 100 pulse repaints + 10 say updates keep the gate; a click 500 ms into playback counts\n");
    for (float zoom : { 0.5f, 1.0f, 1.92f, 2.39f }) {
        auto const view = viewOf(137, 911, zoom);
        auto const frame = mcp::alphiCornerRect(view, zoom, AlphiCorner::bottomRight);
        Painter p { frame };
        std::int64_t const t0 = 50000;
        p.paint(view, t0); // the first full paint: the gate starts
        CHECK(p.shownSinceMs == t0);
        std::int64_t now = t0;
        auto const motion = mcp::alphiMotionArea(frame, zoom);
        auto const strip = ApprovalRect { frame.x - 260, frame.y, 250, 40 };
        int sayCount = 0;
        for (int i = 0; i < 100; ++i) {
            now += 5; // 100 repaints within the first 500 ms (worst case: faster than the 30 Hz timer)
            auto const region = renderRegionOf(motion, view, zoom);
            CHECK(mcp::alphiRectContains(region, frame)); // the whole frame is redrawn: never a half-drawn Alphi
            CHECK(mcp::alphiOnlyPaint(region, true, frame, strip, zoom));
            p.paint(region, now);
            if (i % 10 == 9) { // a say: the handler repaints all; its expiry repaints the strip only
                p.paint(view, now);
                p.paint(renderRegionOf(mcp::alphiMotionArea(strip, zoom), view, zoom), now);
                ++sayCount;
            }
        }
        CHECK(sayCount == 10);
        CHECK(p.shownSinceMs == t0);
        CHECK(mcp::alphiClickAllowed(p.shownSinceMs, t0 + 500));
    }
}

static void alphiOnlyPaintRule()
{
    std::printf("F0 / N3: which paints skip the dock pass\n");
    float const zoom = 1.0f;
    auto const view = viewOf(0, 0, zoom);
    auto const frame = mcp::alphiCornerRect(view, zoom, AlphiCorner::bottomRight);
    auto const motion = mcp::alphiMotionArea(frame, zoom);
    // Only a repaint the motion timer asked for, and only when it stays near Alphi (or his strip).
    CHECK(mcp::alphiOnlyPaint(motion, true, frame, {}, zoom));
    CHECK(!mcp::alphiOnlyPaint(motion, false, frame, {}, zoom)); // a box moved under Alphi: the dock must look
    CHECK(!mcp::alphiOnlyPaint(view, true, frame, {}, zoom));    // coalesced with a canvas repaint
    CHECK(!mcp::alphiOnlyPaint({}, true, frame, {}, zoom));
    CHECK(!mcp::alphiOnlyPaint(motion, true, {}, {}, zoom));     // Alphi not drawn
    ApprovalRect const strip { frame.x - 240, frame.y + 10, 230, 36 };
    auto const both = ApprovalRect { strip.x - 3, frame.y - 3, frame.x + frame.w - strip.x + 6, frame.h + 6 };
    CHECK(mcp::alphiOnlyPaint(both, true, frame, strip, zoom));
    CHECK(!mcp::alphiOnlyPaint(both, true, frame, {}, zoom));
    // The motion area grows the frame by 3 design px at any zoom.
    for (float z : { 0.25f, 0.5f, 1.0f, 2.39f }) {
        auto const f = mcp::alphiCornerRect(viewOf(0, 0, z), z, AlphiCorner::topLeft);
        auto const m = mcp::alphiMotionArea(f, z);
        int const pad = static_cast<int>(std::ceil(3.0f / z));
        CHECK(m.x == f.x - pad && m.y == f.y - pad && m.w == f.w + 2 * pad && m.h == f.h + 2 * pad);
    }
}

static void whatRestartsTheGate()
{
    std::printf("F0: covered / moved restart the gate; a plain repaint never does\n");
    auto const view = viewOf(0, 0, 1.0f);
    Painter p { mcp::alphiCornerRect(view, 1.0f, AlphiCorner::bottomRight) };
    p.paint(view, 1000);
    for (int i = 0; i < 50; ++i)
        p.paint(view, 1000 + i * 16); // full repaints (a canvas click, a selection, a scroll that left him in place)
    CHECK(p.shownSinceMs == 1000);
    // The selection pill or a chip stack covers him: hidden, gate 0; uncovered: 400 ms again from the next full paint.
    p.paint(view, 3000, true);
    CHECK(p.shownSinceMs == 0);
    p.paint(view, 3100);
    CHECK(p.shownSinceMs == 3100);
    CHECK(!mcp::alphiClickAllowed(p.shownSinceMs, 3300));
    CHECK(mcp::alphiClickAllowed(p.shownSinceMs, 3500));
    // His frame moved on screen (a resize, the sidebar): restart.
    p.paint(view, 4000, false, true);
    CHECK(p.shownSinceMs == 4000);
}

int main()
{
    pulseAndSayKeepGate();
    alphiOnlyPaintRule();
    whatRestartsTheGate();
    if (failures == 0)
        std::printf("alphi_gate_test: all passed\n");
    else
        std::printf("alphi_gate_test: %d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
