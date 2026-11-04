// Unit tests for big Alphi step 1a0, B1 (docs/plans/big-alphi-step1-steps.md): where Alphi docks, JUCE-free
// (Source/Mcp/McpAlphiDock.h). The A8 live bug: mini Alphi sat on the top-left corner of the view over the artist's
// boxes; the covered inlet could not take a wire and a click there toggled flow instead of selecting the box.
//  - a box at the view's top left is never under Alphi (whatever corner is preferred);
//  - a covered corner is skipped, in a fixed order starting at the preferred one (default bottom right);
//  - every corner full: folded in the least covered one;
//  - the chip stack, the pill and its drawers (covers) take the corner like a box;
//  - zoom 0.5 / 1 / 1.92 / 2.39 and an 890 x 660 window: the frame stays inside the view, screen-constant size;
//  - AlphiDock: placed at once, re-checked 250 ms after a change, never moved while hovered or during a drag;
//  - 300 boxes: one choice well under 1 ms.
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpAlphiDock.h"

#include <chrono>
#include <cmath>
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
using Rects = std::vector<ApprovalRect>;

static AlphiCorner const allCorners[] = { AlphiCorner::bottomRight, AlphiCorner::bottomLeft, AlphiCorner::topRight, AlphiCorner::topLeft };

static bool meets(ApprovalRect a, ApprovalRect b) { return mcp::rectsIntersect(a, b); }
static bool inside(ApprovalRect r, ApprovalRect view)
{
    return r.x >= view.x && r.y >= view.y && r.x + r.w <= view.x + view.w && r.y + r.h <= view.y + view.h;
}
// The view of a w x h screen-pixel window at this zoom (canvas units), scrolled to (ox, oy).
static ApprovalRect viewOf(int ox, int oy, int w, int h, float zoom)
{
    return { ox, oy, static_cast<int>(std::lround(w / zoom)), static_cast<int>(std::lround(h / zoom)) };
}
static ApprovalRect frameOf(ApprovalRect view, float zoom, mcp::AlphiDockChoice c)
{
    return mcp::alphiCornerRect(view, zoom, c.corner);
}
// A typical object box (GUI bounds incl. the inlet/outlet strips) whose top-left sits `dx, dy` canvas units from the
// view's top-left corner.
static ApprovalRect boxAt(ApprovalRect view, int dx, int dy, int w = 70, int h = 30)
{
    return { view.x + dx, view.y + dy, w, h };
}

static void cornerFrames()
{
    std::printf("Alphi frame at each corner and zoom\n");
    using A = mcp::AlphiMetrics;
    for (float zoom : { 0.5f, 1.0f, 1.92f, 2.39f }) {
        for (auto view : { viewOf(300, 200, 1200, 800, zoom), viewOf(-40, -25, 890, 660, zoom) }) {
            for (auto c : allCorners) {
                auto const r = mcp::alphiCornerRect(view, zoom, c);
                CHECK(inside(r, view));
                // Screen-constant size (the canvas size is rounded up: at most one canvas unit = `zoom` px more).
                CHECK(r.w * zoom >= A::w - 0.01f && r.w * zoom <= A::w + zoom + 0.01f);
                CHECK(r.h * zoom >= A::h - 0.01f && r.h * zoom <= A::h + zoom + 0.01f);
                bool const right = c == AlphiCorner::bottomRight || c == AlphiCorner::topRight;
                bool const bottom = c == AlphiCorner::bottomRight || c == AlphiCorner::bottomLeft;
                // The margin (12 design px) to the two edges of its corner, at any zoom.
                float const gapX = right ? (view.x + view.w - (r.x + r.w)) * zoom : (r.x - view.x) * zoom;
                float const gapY = bottom ? (view.y + view.h - (r.y + r.h)) * zoom : (r.y - view.y) * zoom;
                float const tol = 1.5f * zoom + 1.0f; // rounding of the margin, the size and the view (canvas units)
                CHECK(std::abs(gapX - A::margin) <= tol && std::abs(gapY - A::margin) <= tol);
            }
            // The top-left frame is today's frame (the chips and the hit map of P12a.1 keep their geometry there).
            auto const tl = mcp::alphiCornerRect(view, zoom, AlphiCorner::topLeft);
            auto const old = mcp::alphiRect(view, zoom);
            CHECK(tl.x == old.x && tl.y == old.y && tl.w == old.w && tl.h == old.h);
        }
    }
}

static void liveBug()
{
    std::printf("a box at the view's top left is never under Alphi (A8 live bug)\n");
    for (float zoom : { 0.5f, 1.0f, 1.92f, 2.39f }) {
        auto const view = viewOf(0, 0, 890, 660, zoom);
        Rects const boxes { boxAt(view, 4, 4), boxAt(view, 30, 50, 120, 24) };
        for (auto preferred : allCorners) {
            auto const c = mcp::chooseCorner(view, zoom, boxes, {}, preferred);
            CHECK(!c.folded);
            CHECK(c.covered == 0);
            for (auto const& b : boxes)
                CHECK(!meets(frameOf(view, zoom, c), b));
        }
        // Default: bottom right, the empty corner the artist does not patch from.
        auto const d = mcp::chooseCorner(view, zoom, boxes, {}, mcp::AlphiDockMetrics::preferred);
        CHECK(d.corner == AlphiCorner::bottomRight);
    }
    // Inlet strip + 8 px pad: a box that ends 5 screen px before Alphi's frame still counts as under it.
    {
        float const zoom = 1.0f;
        auto const view = viewOf(0, 0, 1000, 700, zoom);
        auto const tl = mcp::alphiCornerRect(view, zoom, AlphiCorner::topLeft);
        ApprovalRect const near { tl.x + tl.w + 5, tl.y + 10, 60, 24 };
        auto const c = mcp::chooseCorner(view, zoom, { near }, {}, AlphiCorner::topLeft);
        CHECK(c.corner != AlphiCorner::topLeft);
        ApprovalRect const far { tl.x + tl.w + 40, tl.y + 10, 60, 24 };
        CHECK(mcp::chooseCorner(view, zoom, { far }, {}, AlphiCorner::topLeft).corner == AlphiCorner::topLeft);
    }
}

static void skipsCovered()
{
    std::printf("a covered corner is skipped, the next free one taken\n");
    float const zoom = 1.0f;
    auto const view = viewOf(100, 100, 1200, 800, zoom);
    auto const br = mcp::alphiCornerRect(view, zoom, AlphiCorner::bottomRight);
    auto const bl = mcp::alphiCornerRect(view, zoom, AlphiCorner::bottomLeft);
    auto const tr = mcp::alphiCornerRect(view, zoom, AlphiCorner::topRight);
    Rects boxes { { br.x - 10, br.y + 5, 80, 30 } };
    auto c = mcp::chooseCorner(view, zoom, boxes, {}, AlphiCorner::bottomRight);
    CHECK(c.corner == AlphiCorner::bottomLeft && !c.folded);
    boxes.push_back({ bl.x + 2, bl.y + 2, 40, 20 });
    c = mcp::chooseCorner(view, zoom, boxes, {}, AlphiCorner::bottomRight);
    CHECK(c.corner == AlphiCorner::topRight && !c.folded);
    boxes.push_back({ tr.x - 20, tr.y, 50, 50 });
    c = mcp::chooseCorner(view, zoom, boxes, {}, AlphiCorner::bottomRight);
    CHECK(c.corner == AlphiCorner::topLeft && !c.folded);
    // A box elsewhere in the view changes nothing.
    c = mcp::chooseCorner(view, zoom, { { view.x + 500, view.y + 300, 80, 30 } }, {}, AlphiCorner::bottomRight);
    CHECK(c.corner == AlphiCorner::bottomRight);
    // Boxes outside the view never count.
    c = mcp::chooseCorner(view, zoom, { { view.x + view.w + 5, view.y + view.h - 40, 80, 30 } }, {}, AlphiCorner::bottomRight);
    CHECK(c.corner == AlphiCorner::bottomRight);
}

static void allFull()
{
    std::printf("every corner full: folded in the least covered one\n");
    float const zoom = 1.0f;
    auto const view = viewOf(0, 0, 900, 660, zoom);
    Rects boxes;
    // A big box over each corner, the top-right one only grazed.
    for (auto c : allCorners) {
        auto const r = mcp::alphiCornerRect(view, zoom, c);
        if (c == AlphiCorner::topRight)
            boxes.push_back({ r.x - 30, r.y + r.h - 3, 40, 20 });
        else
            boxes.push_back({ r.x - 20, r.y - 20, r.w + 40, r.h + 40 });
    }
    auto const c = mcp::chooseCorner(view, zoom, boxes, {}, AlphiCorner::bottomRight);
    CHECK(c.folded);
    CHECK(c.corner == AlphiCorner::topRight);
    CHECK(c.covered > 0);
    // Ties go to the preferred order (all equally full: the preferred corner).
    Rects same;
    for (auto cc : allCorners) {
        auto const r = mcp::alphiCornerRect(view, zoom, cc);
        same.push_back({ r.x - 20, r.y - 20, r.w + 40, r.h + 40 });
    }
    auto const t = mcp::chooseCorner(view, zoom, same, {}, AlphiCorner::bottomLeft);
    CHECK(t.folded && t.corner == AlphiCorner::bottomLeft);
    // A bigger body that fits nowhere, a mini face that fits in one corner: folded there, nothing covered.
    {
        mcp::AlphiSize const big { 150, 190 };
        Rects b2;
        for (auto cc : allCorners) {
            auto const r = mcp::alphiCornerRect(view, zoom, cc, big);
            auto const m = mcp::alphiCornerRect(view, zoom, cc);
            if (cc == AlphiCorner::topLeft)
                b2.push_back({ r.x + r.w - 20, r.y + r.h - 20, 30, 30 }); // under the body only, not the mini face
            else
                b2.push_back({ m.x - 4, m.y - 4, m.w + 8, m.h + 8 });
        }
        auto const f = mcp::chooseCorner(view, zoom, b2, {}, AlphiCorner::bottomRight, big);
        CHECK(f.folded && f.corner == AlphiCorner::topLeft && f.covered == 0);
    }
}

static void coversTakeCorner()
{
    std::printf("the chip stack, the pill and its drawers take the corner\n");
    float const zoom = 1.0f;
    auto const view = viewOf(0, 0, 1200, 800, zoom);
    auto const br = mcp::alphiCornerRect(view, zoom, AlphiCorner::bottomRight);
    ApprovalRect const pill { br.x - 200, br.y + 10, 240, 40 };
    auto c = mcp::chooseCorner(view, zoom, {}, { pill }, AlphiCorner::bottomRight);
    CHECK(c.corner != AlphiCorner::bottomRight && !c.folded);
    CHECK(!meets(frameOf(view, zoom, c), pill));
    // A chip stack across the whole bottom and a drawer at the top right: top left.
    ApprovalRect const chips { 0, view.h - 100, view.w, 100 };
    auto const tr = mcp::alphiCornerRect(view, zoom, AlphiCorner::topRight);
    ApprovalRect const drawer { tr.x - 100, tr.y, 160, 30 };
    c = mcp::chooseCorner(view, zoom, {}, { chips, drawer, ApprovalRect {} }, AlphiCorner::bottomRight);
    CHECK(c.corner == AlphiCorner::topLeft && !c.folded);
}

// A test driver of the canvas glue (CanvasMcpFlow.cpp): every paint looks; the timer decides; a paint applies only
// what its region redraws. `step` runs one timer tick plus one FULL paint and returns whether Alphi moved.
struct Glue {
    mcp::AlphiDock d;
    bool step(ApprovalRect view, float zoom, Rects const& boxes, Rects const& covers, bool hold, mcp::AlphiMouse m, std::int64_t t)
    {
        d.look(view, zoom, boxes, covers, t);
        if (!d.placed() || d.due(t))
            d.decide(view, zoom, boxes, covers, hold, m, t);
        return d.apply(mcp::alphiCornerRect(view, zoom, d.corner()), mcp::alphiCornerRect(view, zoom, d.stagedCorner()), view);
    }
};

static void dockHolds()
{
    std::printf("AlphiDock: placed at once, re-checked 250 ms after a change, holds while hovered or dragging\n");
    using D = mcp::AlphiDockMetrics;
    float const zoom = 1.0f;
    auto const view = viewOf(0, 0, 1200, 800, zoom);
    auto const br = mcp::alphiCornerRect(view, zoom, AlphiCorner::bottomRight);
    Rects const none;
    Rects const onBr { { br.x, br.y, 60, 30 } };
    mcp::AlphiMouse const away {};

    Glue g;
    CHECK(!g.d.placed());
    std::int64_t t = 10000;
    CHECK(g.step(view, zoom, onBr, none, false, away, t)); // the first look places it at once
    CHECK(g.d.placed() && g.d.corner() == AlphiCorner::bottomLeft && !g.d.pending());

    // A box removed: nothing moves before the 250 ms settle.
    CHECK(!g.step(view, zoom, none, none, false, away, t + 10));
    CHECK(g.d.pending() && g.d.corner() == AlphiCorner::bottomLeft);
    CHECK(!g.d.due(t + 10 + D::settleMs - 1) && g.d.due(t + 10 + D::settleMs));
    CHECK(!g.step(view, zoom, none, none, false, away, t + 10 + D::settleMs - 1));
    CHECK(g.step(view, zoom, none, none, false, away, t + 10 + D::settleMs));
    CHECK(g.d.corner() == AlphiCorner::bottomRight && !g.d.pending());

    // An object placed on it while hovered or during a drag: no jump.
    t += 1000;
    CHECK(!g.step(view, zoom, onBr, none, true, away, t));
    CHECK(!g.step(view, zoom, onBr, none, true, away, t + 5000));
    CHECK(g.d.corner() == AlphiCorner::bottomRight && g.d.pending());
    // Hover and drag end: the next look moves it.
    CHECK(g.step(view, zoom, onBr, none, false, away, t + 6001));
    CHECK(g.d.corner() == AlphiCorner::bottomLeft && !g.d.pending());

    // A scroll keeps changing the view: the re-check waits for 250 ms of quiet.
    t += 10000;
    for (int i = 0; i < 10; ++i)
        CHECK(!g.step(viewOf(i * 15, 0, 1200, 800, zoom), zoom, none, none, false, away, t + i * 50));
    CHECK(g.d.corner() == AlphiCorner::bottomLeft && g.d.pending());
    auto const v9 = viewOf(9 * 15, 0, 1200, 800, zoom);
    CHECK(!g.step(v9, zoom, none, none, false, away, t + 9 * 50 + D::settleMs - 1));
    CHECK(g.step(v9, zoom, none, none, false, away, t + 9 * 50 + D::settleMs));
    CHECK(g.d.corner() == AlphiCorner::bottomRight);
    CHECK(!g.step(v9, zoom, none, none, false, away, t + 100000));
    CHECK(!g.d.pending());

    // A hover check: next to Alphi (24 design px around it) counts, far away does not.
    auto const f = mcp::alphiCornerRect(view, 2.0f, AlphiCorner::topLeft);
    CHECK(mcp::alphiHovered(f, 2.0f, f.x + f.w / 2, f.y + f.h / 2));
    CHECK(mcp::alphiHovered(f, 2.0f, f.x + f.w + 10, f.y));
    CHECK(!mcp::alphiHovered(f, 2.0f, f.x + f.w + 20, f.y));
    CHECK(!mcp::alphiHovered(ApprovalRect {}, 1.0f, 0, 0));
}

// Review H1 + the per-point rule (1a0 round 3, replaces the frame rule): a click point inside any box (with the 8 px
// pad) is the box's / the canvas's, never flow; a click on a part of Alphi with no box under it is Alphi's (after the
// normal gate), so a folded Alphi on a full canvas stays usable. The hover hold does not keep Alphi over the box the
// pointer is on.
static void reviewH1BoxUnderAlphi()
{
    std::printf("review H1 / per point: a click on a box under Alphi is never flow; Alphi's free parts stay his\n");
    float const zoom = 1.0f;
    auto const view = viewOf(0, 0, 1200, 800, zoom);
    auto const f = mcp::alphiCornerRect(view, zoom, AlphiCorner::bottomRight);
    ApprovalRect const box { f.x - 30, f.y + 10, 40, 24 }; // its right 10 units under Alphi's left edge
    Rects const boxes { box };
    CHECK(mcp::alphiFrameBlocked(f, boxes));
    CHECK(!mcp::alphiFrameBlocked(f, { { f.x - 100, f.y, 60, 24 } })); // a box beside it
    // On the box part inside Alphi's frame, and within the 8 px pad of the box: not Alphi's.
    CHECK(!mcp::alphiOwnsPoint(f, boxes, f.x + 5, f.y + 20, 1.0f));
    CHECK(!mcp::alphiOwnsPoint(f, boxes, f.x + 10 + 7, f.y + 20, 1.0f));
    CHECK(!mcp::alphiOwnsPoint(f, boxes, f.x + 3, f.y + 10 - 7, 1.0f));
    // A free part of Alphi beyond the pad: his.
    CHECK(mcp::alphiOwnsPoint(f, boxes, f.x + f.w - 3, f.y + f.h - 3, 1.0f));
    CHECK(mcp::alphiOwnsPoint(f, boxes, f.x + 10 + 9, f.y + 20, 1.0f));
    // The pad is 8 SCREEN px: at zoom 2 it is 4 canvas units.
    CHECK(mcp::alphiOwnsPoint(f, boxes, f.x + 10 + 5, f.y + 20, 2.0f));
    CHECK(!mcp::alphiOwnsPoint(f, boxes, f.x + 10 + 3, f.y + 20, 2.0f));
    // No box: the frame is his, outside is not.
    CHECK(mcp::alphiOwnsPoint(f, {}, f.x + 5, f.y + 20, 1.0f));
    CHECK(!mcp::alphiOwnsPoint(f, {}, f.x - 1, f.y + 20, 1.0f));
    // Every corner full (a dense patch): the folded Alphi sits partly on boxes; the box parts are never his, his free
    // parts are (no dead end: the artist can still switch flow).
    {
        Rects full;
        for (auto c : allCorners) {
            auto const r = mcp::alphiCornerRect(view, zoom, c);
            full.push_back({ r.x + r.w / 2, r.y - 4, r.w, r.h + 8 }); // the right half of each frame
        }
        auto const ch = mcp::chooseCorner(view, zoom, full, {}, AlphiCorner::bottomRight);
        auto const ff = mcp::alphiCornerRect(view, zoom, ch.corner);
        CHECK(ch.folded && mcp::alphiFrameBlocked(ff, full));
        CHECK(!mcp::alphiOwnsPoint(ff, full, ff.x + ff.w / 2 + 2, ff.y + ff.h / 2, 1.0f));
        CHECK(mcp::alphiOwnsPoint(ff, full, ff.x + 3, ff.y + ff.h / 2, 1.0f));
    }
    // The hold: the pointer on that box (reaching for its inlet) never holds Alphi there; on Alphi's free part, next
    // to it on empty canvas, or with a button held, it does.
    mcp::AlphiMouse onBox { true, f.x + 5, f.y + 20 };
    CHECK(!mcp::alphiDockHold(f, zoom, boxes, false, onBox));
    CHECK(mcp::alphiDockHold(f, zoom, boxes, true, onBox)); // a drag in progress: wait for the release
    CHECK(mcp::alphiDockHold(f, zoom, {}, false, mcp::AlphiMouse { true, f.x + 5, f.y + 20 }));
    CHECK(mcp::alphiDockHold(f, zoom, boxes, false, mcp::AlphiMouse { true, f.x + f.w + 5, f.y - 5 }));
    CHECK(!mcp::alphiDockHold(f, zoom, boxes, false, mcp::AlphiMouse {})); // mouse elsewhere
    Rects const beside { { f.x - 70, f.y, 60, 24 } };
    CHECK(mcp::alphiDockHold(f, zoom, beside, false, mcp::AlphiMouse { true, f.x - 15, f.y + 5 }));
    // End to end: the box dropped into Alphi's corner while the pointer is on it: Alphi steps aside after 250 ms.
    Glue g;
    std::int64_t t = 1000;
    CHECK(g.step(view, zoom, {}, {}, false, {}, t) && g.d.corner() == AlphiCorner::bottomRight);
    bool const hold = mcp::alphiDockHold(f, zoom, boxes, false, onBox);
    g.step(view, zoom, boxes, {}, hold, onBox, t + 10);
    CHECK(g.step(view, zoom, boxes, {}, hold, onBox, t + 10 + mcp::AlphiDockMetrics::settleMs));
    CHECK(g.d.corner() != AlphiCorner::bottomRight);
}

// Review N2: after a paint, Alphi's frame is repainted when its look changed (a box arrived under it or left), or
// when its gate is still 0 because the paint did not redraw his frame; once per state, so it never loops.
static void reviewN2Repaint()
{
    std::printf("review N2: Alphi's frame is repainted after a box arrives or leaves, once\n");
    mcp::AlphiRepaintState st;
    // A box leaves: the look changes -> repaint once.
    CHECK(mcp::alphiAfterPaint(st, /*blocked*/ true, /*gate*/ 5000, /*frameInRegion*/ true, /*covered*/ false) == false);
    CHECK(mcp::alphiAfterPaint(st, false, 5000, false, false) == true);
    CHECK(mcp::alphiAfterPaint(st, false, 5000, false, false) == false); // asked already, nothing changed
    // The gate is 0 and this paint missed his frame: one repaint, not one per paint.
    mcp::AlphiRepaintState g0;
    CHECK(mcp::alphiAfterPaint(g0, false, 0, false, false) == true);
    CHECK(mcp::alphiAfterPaint(g0, false, 0, false, false) == false);
    // The frame is painted: the gate starts, and a later miss asks again only after a new change.
    CHECK(mcp::alphiAfterPaint(g0, false, 9000, true, false) == false);
    CHECK(mcp::alphiAfterPaint(g0, false, 0, false, false) == true);
    // Covered (not drawn): nothing to repaint for.
    mcp::AlphiRepaintState c;
    CHECK(mcp::alphiAfterPaint(c, false, 0, false, true) == false);
}

// Review H2: a move lands only in a paint that redraws both frames; the 400 ms gate starts only in a paint that
// redrew Alphi's whole frame. L1: a staged move asks for at most maxApplyTries full repaints.
static void reviewH2PartialRender()
{
    std::printf("review H2: moves and the click gate need a full paint of Alphi's frames\n");
    float const zoom = 1.0f;
    auto const view = viewOf(0, 0, 1200, 800, zoom);
    auto const br = mcp::alphiCornerRect(view, zoom, AlphiCorner::bottomRight);
    auto const bl = mcp::alphiCornerRect(view, zoom, AlphiCorner::bottomLeft);
    mcp::AlphiDock d;
    std::int64_t t = 1000;
    d.look(view, zoom, {}, {}, t);
    CHECK(d.decide(view, zoom, {}, {}, false, {}, t));
    // A partial render elsewhere (a blinking number box): the first place does not land.
    ApprovalRect const damage { 400, 300, 60, 20 };
    CHECK(!d.apply(br, br, damage));
    CHECK(!d.placed());
    CHECK(d.apply(br, br, view) && d.placed() && d.corner() == AlphiCorner::bottomRight);
    // A box onto BR; after the settle the timer stages bottom left.
    Rects const onBr { { br.x, br.y, 60, 30 } };
    d.look(view, zoom, onBr, {}, t + 10);
    CHECK(d.decide(view, zoom, onBr, {}, false, {}, t + 10 + mcp::AlphiDockMetrics::settleMs));
    CHECK(d.hasStaged() && d.stagedCorner() == AlphiCorner::bottomLeft);
    // Partial renders: one holding only the new frame (the old one would stay a ghost), one only the old.
    CHECK(!d.apply(br, bl, ApprovalRect { bl.x - 2, bl.y - 2, bl.w + 4, bl.h + 4 }));
    CHECK(!d.apply(br, bl, ApprovalRect { br.x - 2, br.y - 2, br.w + 4, br.h + 4 }));
    CHECK(d.corner() == AlphiCorner::bottomRight);
    CHECK(d.apply(br, bl, view) && d.corner() == AlphiCorner::bottomLeft && !d.hasStaged());
    // The gate: a partial paint that did not redraw Alphi never starts it; a full one does; once started, a partial
    // paint keeps it.
    mcp::AlphiGateInput partial;
    CHECK(mcp::alphiGateAfterPaint(0, 9000, partial) == 0);
    mcp::AlphiGateInput full;
    full.frameInRegion = true;
    CHECK(mcp::alphiGateAfterPaint(0, 9000, full) == 9000);
    CHECK(mcp::alphiGateAfterPaint(8000, 9000, partial) == 8000);
    // L1: a staged move that never meets a full paint is dropped after maxApplyTries asks.
    mcp::AlphiDock e;
    e.look(view, zoom, {}, {}, t);
    CHECK(e.decide(view, zoom, {}, {}, false, {}, t));
    int asks = 0;
    while (e.retryStaged() && asks < 100)
        ++asks;
    CHECK(asks == mcp::AlphiDock::maxApplyTries);
    CHECK(!e.hasStaged());
}

// Review M1: Alphi never moves into the corner where the pointer is. M2: the gate restarts when the frame moves on
// screen (window resize, split view, sidebar), not only when the corner changes.
static void reviewM1M2()
{
    std::printf("review M1/M2: never under the pointer; screen moves restart the gate\n");
    float const zoom = 1.0f;
    auto const view = viewOf(0, 0, 1200, 800, zoom);
    auto const br = mcp::alphiCornerRect(view, zoom, AlphiCorner::bottomRight);
    auto const bl = mcp::alphiCornerRect(view, zoom, AlphiCorner::bottomLeft);
    Rects const onBr { { br.x, br.y, 60, 30 } };
    mcp::AlphiMouse const inBl { true, bl.x + bl.w / 2, bl.y + bl.h / 2 };
    mcp::AlphiDock d;
    d.look(view, zoom, onBr, {}, 1000);
    CHECK(d.decide(view, zoom, onBr, {}, false, inBl, 1000));
    CHECK(d.stagedCorner() != AlphiCorner::bottomLeft && d.stagedCorner() != AlphiCorner::bottomRight);
    // Pointer just next to the BL frame (within 24 px): still not there.
    mcp::AlphiMouse const nearBl { true, bl.x + bl.w + 15, bl.y };
    mcp::AlphiDock e;
    e.look(view, zoom, onBr, {}, 1000);
    CHECK(e.decide(view, zoom, onBr, {}, false, nearBl, 1000));
    CHECK(e.stagedCorner() == AlphiCorner::topRight);
    // N1: three corners full and the pointer in the fourth: the fallback never folds under the pointer; Alphi stays.
    {
        Rects three;
        for (auto c : { AlphiCorner::bottomRight, AlphiCorner::bottomLeft, AlphiCorner::topRight }) {
            auto const r = mcp::alphiCornerRect(view, zoom, c);
            three.push_back({ r.x - 10, r.y - 10, r.w + 20, r.h + 20 });
        }
        auto const tl = mcp::alphiCornerRect(view, zoom, AlphiCorner::topLeft);
        mcp::AlphiDock p;
        p.look(view, zoom, {}, {}, 1);
        p.decide(view, zoom, {}, {}, false, {}, 1);
        CHECK(p.apply({}, br, view) && p.corner() == AlphiCorner::bottomRight);
        p.look(view, zoom, three, {}, 10);
        mcp::AlphiMouse const inTl { true, tl.x + tl.w / 2, tl.y + tl.h / 2 };
        p.decide(view, zoom, three, {}, false, inTl, 1000);
        CHECK(p.stagedCorner() != AlphiCorner::topLeft);
        CHECK(p.stagedCorner() == AlphiCorner::bottomRight); // stays in its corner (folded there at most)
        // The first place with the pointer in the only free corner: never under the pointer either.
        mcp::AlphiDock q;
        q.look(view, zoom, three, {}, 1);
        q.decide(view, zoom, three, {}, false, inTl, 1);
        CHECK(q.stagedCorner() != AlphiCorner::topLeft);
    }
    // M2.
    CHECK(!mcp::alphiScreenMoved(500, 400, 501, 400));
    CHECK(mcp::alphiScreenMoved(500, 400, 520, 400));
    CHECK(mcp::alphiScreenMoved(500, 400, 500, 360));
    mcp::AlphiGateInput moved;
    moved.screenMoved = true;
    moved.frameInRegion = true;
    CHECK(mcp::alphiGateAfterPaint(5000, 9000, moved) == 9000); // restarted now, not kept at 5000
    moved.frameInRegion = false;
    CHECK(mcp::alphiGateAfterPaint(5000, 9000, moved) == 0);
}

static void speed()
{
    std::printf("300 boxes: one choice under 1 ms\n");
    float const zoom = 1.0f;
    auto const view = viewOf(0, 0, 1600, 1000, zoom);
    Rects boxes;
    for (int i = 0; i < 300; ++i)
        boxes.push_back({ (i % 20) * 80, (i / 20) * 66, 70, 30 }); // a full grid: every corner is covered
    auto const t0 = std::chrono::steady_clock::now();
    int const runs = 200;
    mcp::AlphiDockChoice c {};
    for (int i = 0; i < runs; ++i)
        c = mcp::chooseCorner(view, zoom, boxes, {}, AlphiCorner::bottomRight);
    auto const us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / runs;
    std::printf("  %.1f us per choice\n", us);
    CHECK(us < 1000.0);
    CHECK(c.folded);
    // The dock with 300 boxes per look (the signature included).
    mcp::AlphiDock d;
    auto const t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < runs; ++i) {
        d.look(view, zoom, boxes, {}, 1000 + i);
        d.decide(view, zoom, boxes, {}, false, {}, 1000 + i);
    }
    auto const us2 = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t1).count() / runs;
    std::printf("  %.1f us per dock update\n", us2);
    CHECK(us2 < 1000.0);
}

int main()
{
    cornerFrames();
    liveBug();
    skipsCovered();
    allFull();
    coversTakeCorner();
    dockHolds();
    reviewH1BoxUnderAlphi();
    reviewN2Repaint();
    reviewH2PartialRender();
    reviewM1M2();
    speed();
    if (failures == 0)
        std::printf("alphi_dock_test: all passed\n");
    else
        std::printf("alphi_dock_test: %d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
