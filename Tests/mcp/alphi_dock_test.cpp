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

static void dockHolds()
{
    std::printf("AlphiDock: placed at once, re-checked 250 ms after a change, holds while hovered or dragging\n");
    using D = mcp::AlphiDockMetrics;
    float const zoom = 1.0f;
    auto const view = viewOf(0, 0, 1200, 800, zoom);
    auto const br = mcp::alphiCornerRect(view, zoom, AlphiCorner::bottomRight);
    Rects const none;
    Rects const onBr { { br.x, br.y, 60, 30 } };

    mcp::AlphiDock d;
    CHECK(!d.placed());
    std::int64_t t = 10000;
    d.update(view, zoom, onBr, none, false, false, t); // the first look places it at once
    CHECK(d.placed() && d.corner() == AlphiCorner::bottomLeft && !d.pending());

    // A box removed: nothing moves before the 250 ms settle.
    CHECK(!d.update(view, zoom, none, none, false, false, t + 10));
    CHECK(d.pending() && d.corner() == AlphiCorner::bottomLeft);
    CHECK(!d.due(t + 10 + D::settleMs - 1) && d.due(t + 10 + D::settleMs));
    CHECK(!d.update(view, zoom, none, none, false, false, t + 10 + D::settleMs - 1));
    CHECK(d.corner() == AlphiCorner::bottomLeft);
    CHECK(d.update(view, zoom, none, none, false, false, t + 10 + D::settleMs));
    CHECK(d.corner() == AlphiCorner::bottomRight && !d.pending());

    // An object placed on it while hovered: no jump while the mouse is next to it, nor during a drag.
    t += 1000;
    CHECK(!d.update(view, zoom, onBr, none, true, false, t));
    CHECK(!d.update(view, zoom, onBr, none, true, false, t + 5000));
    CHECK(d.corner() == AlphiCorner::bottomRight && d.pending());
    CHECK(!d.update(view, zoom, onBr, none, false, true, t + 6000));
    CHECK(d.corner() == AlphiCorner::bottomRight);
    // Hover and drag end: the next look moves it.
    CHECK(d.update(view, zoom, onBr, none, false, false, t + 6001));
    CHECK(d.corner() == AlphiCorner::bottomLeft && !d.pending());

    // A scroll keeps changing the view: the re-check waits for 250 ms of quiet.
    t += 10000;
    for (int i = 0; i < 10; ++i) {
        auto const v = viewOf(i * 15, 0, 1200, 800, zoom);
        CHECK(!d.update(v, zoom, none, none, false, false, t + i * 50));
    }
    CHECK(d.corner() == AlphiCorner::bottomLeft && d.pending());
    auto const v9 = viewOf(9 * 15, 0, 1200, 800, zoom);
    CHECK(!d.update(v9, zoom, none, none, false, false, t + 9 * 50 + D::settleMs - 1));
    CHECK(d.update(v9, zoom, none, none, false, false, t + 9 * 50 + D::settleMs));
    CHECK(d.corner() == AlphiCorner::bottomRight);

    // Nothing changed: no re-check is pending, nothing moves.
    CHECK(!d.update(v9, zoom, none, none, false, false, t + 100000));
    CHECK(!d.pending());

    // A hover check: next to Alphi (24 design px around it) counts, far away does not.
    auto const f = mcp::alphiCornerRect(view, 2.0f, AlphiCorner::topLeft);
    CHECK(mcp::alphiHovered(f, 2.0f, f.x + f.w / 2, f.y + f.h / 2));
    CHECK(mcp::alphiHovered(f, 2.0f, f.x + f.w + 10, f.y)); // 20 design px to the right at zoom 2
    CHECK(!mcp::alphiHovered(f, 2.0f, f.x + f.w + 20, f.y)); // 40 design px
    CHECK(!mcp::alphiHovered(ApprovalRect {}, 1.0f, 0, 0));
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
    for (int i = 0; i < runs; ++i)
        d.update(view, zoom, boxes, {}, false, false, 1000 + i);
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
    speed();
    if (failures == 0)
        std::printf("alphi_dock_test: all passed\n");
    else
        std::printf("alphi_dock_test: %d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
