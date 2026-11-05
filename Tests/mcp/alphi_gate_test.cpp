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
#include "../../Source/Mcp/McpAlphiStrip.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
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
            CHECK(mcp::alphiOnlyPaint(region, true, frame, strip, zoom, true));
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
    CHECK(mcp::alphiOnlyPaint(motion, true, frame, {}, zoom, true));
    CHECK(!mcp::alphiOnlyPaint(motion, false, frame, {}, zoom, true)); // a box moved under Alphi: the dock must look
    CHECK(!mcp::alphiOnlyPaint(view, true, frame, {}, zoom, true));    // coalesced with a canvas repaint
    CHECK(!mcp::alphiOnlyPaint({}, true, frame, {}, zoom, true));
    CHECK(!mcp::alphiOnlyPaint(motion, true, {}, {}, zoom, true));     // Alphi not drawn
    ApprovalRect const strip { frame.x - 240, frame.y + 10, 230, 36 };
    auto const both = ApprovalRect { strip.x - 3, frame.y - 3, frame.x + frame.w - strip.x + 6, frame.h + 6 };
    CHECK(mcp::alphiOnlyPaint(both, true, frame, strip, zoom, true));
    CHECK(!mcp::alphiOnlyPaint(both, true, frame, {}, zoom, true));
    // The motion area grows the frame by 3 design px at any zoom.
    for (float z : { 0.25f, 0.5f, 1.0f, 2.39f }) {
        auto const f = mcp::alphiCornerRect(viewOf(0, 0, z), z, AlphiCorner::topLeft);
        auto const m = mcp::alphiMotionArea(f, z);
        int const pad = static_cast<int>(std::ceil(3.0f / z));
        CHECK(m.x == f.x - pad && m.y == f.y - pad && m.w == f.w + 2 * pad && m.h == f.h + 2 * pad);
    }
}

// Review HIGH (the reviewer's skip_test.cpp): the dock pass is never skipped for a real layout change near Alphi.
static void skipNeverHidesALayoutChange()
{
    std::printf("review HIGH: a box created / moved near Alphi always gets the dock pass\n");
    float const zoom = 1.0f;
    ApprovalRect const view { 0, 0, 1200, 800 };
    auto const face = mcp::alphiCornerRect(view, zoom, AlphiCorner::bottomRight);
    auto const stripSpot = mcp::alphiStripRect(face, AlphiCorner::bottomRight, view, zoom);
    auto const motion = mcp::alphiMotionArea(face, zoom);
    // (a) No strip drawn: a box moved from the strip's spot onto the face, coalesced with a motion repaint: no skip.
    ApprovalRect const boxOld { stripSpot.x + 100, stripSpot.y + 4, 60, 28 }, boxNew { face.x - 10, face.y + 10, 60, 28 };
    auto const region = mcp::alphimotion::unite(mcp::alphimotion::unite(boxOld, boxNew), motion);
    CHECK(!mcp::alphiOnlyPaint(region, true, face, {}, zoom, true));
    // (b) The layout key near Alphi: a box created on the face, or moved inside his zone, changes it.
    auto const zone = mcp::alphiZone(face, {}, zoom);
    std::vector<ApprovalRect> boxes { { 100, 100, 60, 28 }, { 400, 300, 60, 28 } };
    auto const keyOf = [&](std::vector<ApprovalRect> const& bs, bool button) {
        mcp::AlphiZoneKey k(zone);
        for (auto const& b : bs)
            k.add(b);
        return k.value(button);
    };
    auto const k0 = keyOf(boxes, false);
    CHECK(keyOf(boxes, false) == k0);
    auto withCreated = boxes;
    withCreated.push_back({ face.x + 2, face.y + 20, 40, 24 }); // the AI creates a box on the face
    CHECK(keyOf(withCreated, false) != k0);
    auto created = mcp::alphiOnlyPaint(ApprovalRect { face.x + 2, face.y + 20, 40, 24 }, true, face, {}, zoom,
        keyOf(withCreated, false) == k0);
    CHECK(!created);
    auto moved = withCreated;
    moved.back().x -= 3; // dragged inside the zone
    CHECK(keyOf(moved, false) != keyOf(withCreated, false));
    auto far = boxes;
    far[0].x += 50; // a box far away moved: the zone key does not care (that paint is not Alphi-only anyway)
    CHECK(keyOf(far, false) == k0);
    CHECK(keyOf(boxes, true) != k0); // a button held (a drag, a lasso): never skipped
    // A box just outside the face but inside its 8 px pad counts.
    auto padded = boxes;
    padded.push_back({ face.x - 30, face.y, 25, 20 });
    CHECK(keyOf(padded, false) != k0);
    // The zone grows with a drawn strip.
    auto const z2 = mcp::alphiZone(face, stripSpot, zoom);
    CHECK(z2.x < zone.x);
}

// Review M (strip frame): the strip's hit frame changes only in a render that redrew it (and where it was).
static void stripFrameAfterPaint()
{
    std::printf("review M: the strip's frame follows only full renders of it\n");
    ApprovalRect const view { 0, 0, 1200, 800 };
    ApprovalRect const st { 900, 700, 250, 36 };
    auto u = mcp::alphiStripAfterPaint({}, st, view);
    CHECK(u.frame.w == st.w && !u.repaint);
    // A partial render elsewhere: the drawn strip stays a click target and a repaint is asked.
    u = mcp::alphiStripAfterPaint(st, {}, ApprovalRect { 0, 0, 100, 100 });
    CHECK(u.frame.x == st.x && u.frame.w == st.w && u.repaint);
    u = mcp::alphiStripAfterPaint({}, st, ApprovalRect { 0, 0, 100, 100 });
    CHECK(u.frame.w == 0 && u.repaint); // never an invisible click sink
    u = mcp::alphiStripAfterPaint(st, {}, view);
    CHECK(u.frame.w == 0 && !u.repaint);
    u = mcp::alphiStripAfterPaint({}, {}, ApprovalRect { 0, 0, 10, 10 });
    CHECK(u.frame.w == 0 && !u.repaint);
}

// Review L6: the motion repaint takes the strip's spot only while a strip is drawn or the say changed.
static void motionRepaintArea()
{
    std::printf("review L6: the motion repaint area\n");
    float const zoom = 1.0f;
    ApprovalRect const view { 0, 0, 1200, 800 };
    auto const face = mcp::alphiCornerRect(view, zoom, AlphiCorner::bottomRight);
    auto const spot = mcp::alphiStripRect(face, AlphiCorner::bottomRight, view, zoom);
    auto const m = mcp::alphiMotionArea(face, zoom);
    auto a = mcp::alphiMotionRepaintArea(face, spot, false, false, zoom);
    CHECK(a.x == m.x && a.w == m.w);
    a = mcp::alphiMotionRepaintArea(face, spot, true, false, zoom);
    CHECK(a.x < spot.x + 1 && a.x + a.w >= face.x + face.w);
    a = mcp::alphiMotionRepaintArea(face, spot, false, true, zoom);
    CHECK(a.x < spot.x + 1);
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

// D4 / D8 / D6 timing: the line's clock (2 s dizzy spiral for a failed final, then the line; 6 s, then a 0.5 s fade),
// when the motion timer runs, when the 10 Hz tick wakes it, and a say's expiry repaint (W does not repaint on expiry).
static void lineClock()
{
    std::printf("D8 / D6: the line clock (dizzy, fade)\n");
    using T = mcp::AlphiLineTiming;
    CHECK(T::dizzyMs == 2000 && T::showMs == 6000);
    auto const slot = [](char const* text, mcp::AlphiSayPhase ph, char const* turn) {
        mcp::AlphiSaySlot s;
        s.text = text;
        s.kind = ph == mcp::AlphiSayPhase::Failed ? mcp::AlphiSayKind::Fail : mcp::AlphiSayKind::Plain;
        s.phase = ph;
        s.turn = turn;
        s.expiresAtMs = 1000000;
        return s;
    };
    mcp::AlphiLineClock c;
    CHECK(c.lineAlpha(0) == 0.0f && !c.dizzy(0) && !c.moving(0));
    c.observe(slot("plays at -18 dB", mcp::AlphiSayPhase::Idle, "t1"), 1000);
    CHECK(!c.dizzy(1000));
    CHECK(c.lineAlpha(1000) == 1.0f && c.lineAlpha(6999) == 1.0f);
    CHECK(c.lineAlpha(7250) > 0.0f && c.lineAlpha(7250) < 1.0f);
    CHECK(c.moving(7250) && !c.moving(3000));
    CHECK(c.lineAlpha(7500) == 0.0f && !c.moving(7600));
    // The same line seen again (every paint) never restarts its clock (no hover re-show, no revive).
    c.observe(slot("plays at -18 dB", mcp::AlphiSayPhase::Idle, "t1"), 7000);
    CHECK(c.lineAlpha(7600) == 0.0f);
    // A failed final: 2 s spiral, then the line for 6 s.
    c.observe(slot("didn't work: no dac~", mcp::AlphiSayPhase::Failed, "t2"), 10000);
    CHECK(c.dizzy(10000) && c.dizzy(11999) && !c.dizzy(12000));
    CHECK(c.lineAlpha(11000) == 0.0f);
    CHECK(c.lineAlpha(12000) == 1.0f && c.lineAlpha(17999) == 1.0f && c.lineAlpha(18600) == 0.0f);
    CHECK(c.moving(10500));
    CHECK(c.spiralTurns(10000) == 0.0f && c.spiralTurns(11000) > 0.0f);
    // The line expired on the board (ttl): gone, no spiral.
    c.observe(std::nullopt, 10500);
    CHECK(!c.dizzy(10600) && c.lineAlpha(10600) == 0.0f);
    // The same text in a new turn is a new line.
    c.observe(slot("plays at -18 dB", mcp::AlphiSayPhase::Idle, "t3"), 20000);
    CHECK(c.lineAlpha(20000) == 1.0f);
    c.reset();
    CHECK(c.lineAlpha(20000) == 0.0f);
}

static void motionTimer()
{
    std::printf("D4: the motion timer and its wake\n");
    mcp::AlphiMotionInput in;
    in.shown = true;
    in.target = 0.5f;
    in.drawn = 0.5f;
    CHECK(!mcp::alphiMotionRun(in)); // steady: no timer
    in.drawn = 0.49f;
    CHECK(!mcp::alphiMotionRun(in)); // under 0.02: skipped
    in.drawn = 0.47f;
    CHECK(mcp::alphiMotionRun(in));
    in.covered = true;
    CHECK(!mcp::alphiMotionRun(in)); // covered: not drawn, nothing to move
    in.covered = false;
    in.shown = false;
    CHECK(!mcp::alphiMotionRun(in));
    in.shown = true;
    in.drawn = in.target;
    in.busy = true;
    CHECK(mcp::alphiMotionRun(in)); // sparks / dots
    in.busy = false;
    in.lineMoving = true;
    CHECK(mcp::alphiMotionRun(in)); // spiral, fade
    in.lineMoving = false;
    in.target = 0.0f;
    in.drawn = 0.01f;
    CHECK(mcp::alphiMotionRun(in)); // the last step to calm is always drawn
    // The 10 Hz tick wakes it when the ceiling's block counter moved or the level crossed -60 dBFS.
    // Review L7: not on every block; on a level change of minStep or more, or a -60 dBFS crossing.
    CHECK(!mcp::alphiPulseWake(0.0f, 0.0f));
    CHECK(!mcp::alphiPulseWake(0.40f, 0.41f));
    CHECK(mcp::alphiPulseWake(0.40f, 0.43f));
    CHECK(mcp::alphiPulseWake(0.3f, 0.0f));
    CHECK(mcp::alphiPulseWake(0.0f, 0.01f)); // crossing, however small
    CHECK(mcp::alphiPulseWake(0.01f, 0.0f));
}

static void sayExpiryRepaint()
{
    std::printf("D4: a say's expiry repaints (the board's visible key changes)\n");
    mcp::AlphiSayBoard board;
    auto const k0 = mcp::alphiSayKey(board, 1000);
    auto say = mcp::alphiSayFrom("placed lop~", "busy", "building", "t1", 3000);
    CHECK(say.has_value());
    board.apply(*say, 1000);
    auto const k1 = mcp::alphiSayKey(board, 1000);
    CHECK(k1 != k0);
    CHECK(mcp::alphiSayKey(board, 3999) == k1);
    CHECK(mcp::alphiSayKey(board, 4000) == k0); // expired: the key changes, the tick repaints
    auto fin = mcp::alphiSayFrom("built, not heard yet", "plain", "idle", "t1", 9000);
    board.apply(*fin, 5000);
    auto const k2 = mcp::alphiSayKey(board, 5000);
    CHECK(k2 != k0 && k2 != k1);
}

int main()
{
    lineClock();
    motionTimer();
    sayExpiryRepaint();
    pulseAndSayKeepGate();
    alphiOnlyPaintRule();
    skipNeverHidesALayoutChange();
    stripFrameAfterPaint();
    motionRepaintArea();
    whatRestartsTheGate();
    if (failures == 0)
        std::printf("alphi_gate_test: all passed\n");
    else
        std::printf("alphi_gate_test: %d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
