// Unit tests for P12a.1 (Alphi, the flow-mode toggle on the canvas), JUCE-free (Source/Mcp/McpAlphi.h):
//  - where Alphi sits: the top-left corner of the VISIBLE view at a fixed screen size (>= 44 px touch target) at any zoom;
//  - the approval chips never land on Alphi (approvalStackLayout with Alphi as the keep-out), at any zoom and anchor;
//  - the click rule: a click counts only once Alphi has been on screen 400 ms, and a double click does not flip it twice;
//  - the look: off = calm dim eyes, on = warm yellow eyes + antenna light + "FLOW".
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpApprovalState.h"
#include "../../Source/Mcp/McpAlphi.h"
#include "../../Source/Mcp/McpAlphiDock.h"
#include "../../Source/Mcp/McpFlowMode.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

using mcp::ApprovalRect;

static bool meets(ApprovalRect a, ApprovalRect b) { return mcp::rectsIntersect(a, b); }
static bool inside(ApprovalRect r, ApprovalRect view)
{
    return r.x >= view.x && r.y >= view.y && r.x + r.w <= view.x + view.w && r.y + r.h <= view.y + view.h;
}

static void placement()
{
    std::printf("Alphi placement\n");
    using A = mcp::AlphiMetrics;
    CHECK(A::w >= 44 && A::h >= 44); // touch target
    for (float zoom : { 0.5f, 1.0f, 1.5f, 2.0f, 3.0f }) {
        ApprovalRect const view { 300, 200, static_cast<int>(1200 / zoom), static_cast<int>(800 / zoom) };
        auto const r = mcp::alphiRect(view, zoom);
        // Top left of the visible view, inside it, at the same SCREEN size whatever the zoom (canvas units * zoom).
        CHECK(r.x >= view.x && r.y >= view.y);
        CHECK(inside(r, view));
        CHECK(std::abs(r.w * zoom - A::w) <= 1.5f && std::abs(r.h * zoom - A::h) <= 1.5f);
        CHECK(std::abs((r.x - view.x) * zoom - A::margin) <= 1.5f);
    }
    // A bad zoom never yields an empty or huge rect.
    auto const r0 = mcp::alphiRect({ 0, 0, 800, 600 }, 0.0f);
    CHECK(r0.w == A::w && r0.h == A::h);
}

static void chipsKeepClear()
{
    std::printf("approval chips keep clear of Alphi\n");
    using M = mcp::ApprovalBarMetrics;
    for (float zoom : { 0.5f, 1.0f, 2.0f }) {
        ApprovalRect const view { 0, 0, static_cast<int>(1400 / zoom), static_cast<int>(900 / zoom) };
        auto const alphi = mcp::alphiRect(view, zoom);
        // The anchor (the pill or the selection) right on Alphi: the stack would clamp into the top-left corner.
        ApprovalRect const anchor { view.x, view.y, 10, 10 };
        for (int shown = 1; shown <= M::maxBars; ++shown) {
            auto const plain = mcp::approvalStackLayout(anchor, view, zoom, M::w, shown, false);
            CHECK(!plain.bars.empty() && meets(plain.frame, alphi)); // the case is real: without the keep-out it lands on Alphi
            auto const kept = mcp::approvalStackLayout(anchor, view, zoom, M::w, shown, false, alphi);
            CHECK(!kept.bars.empty());
            CHECK(!meets(kept.frame, alphi));
            for (auto const& b : kept.bars) {
                CHECK(!meets(b.frame, alphi) && !meets(b.yes, alphi) && !meets(b.no, alphi));
                CHECK(inside(b.frame, view)); // still fully on screen (the 400 ms rule needs it)
            }
        }
        // No anchor (view centre) and an anchor far away: unchanged by the keep-out.
        auto const a = mcp::approvalStackLayout(std::nullopt, view, zoom, M::w, 2, false);
        auto const b = mcp::approvalStackLayout(std::nullopt, view, zoom, M::w, 2, false, alphi);
        CHECK(a.frame.x == b.frame.x && a.frame.y == b.frame.y);
    }
    // A narrow view (no room to the right): the stack goes below Alphi instead.
    {
        ApprovalRect const view { 0, 0, 380, 900 };
        auto const alphi = mcp::alphiRect(view, 1.0f);
        auto const kept = mcp::approvalStackLayout(ApprovalRect { 0, 0, 10, 10 }, view, 1.0f, M::w, 1, false, alphi);
        CHECK(!kept.bars.empty() && !meets(kept.frame, alphi) && kept.frame.y >= alphi.y + alphi.h);
        CHECK(inside(kept.bars.front().frame, view));
    }
    // avoidRect on its own: a rect that does not meet the keep-out stays where it is.
    {
        ApprovalRect const view { 0, 0, 1000, 800 };
        ApprovalRect const keep { 12, 12, 48, 56 };
        ApprovalRect const far { 400, 400, 100, 50 };
        auto const same = mcp::avoidRect(far, keep, view, 12);
        CHECK(same.x == far.x && same.y == far.y);
        auto const moved = mcp::avoidRect(ApprovalRect { 12, 12, 300, 80 }, keep, view, 12);
        CHECK(!meets(moved, keep) && inside(moved, view));
    }
}

// Big Alphi 1a0, B2a: today's mini face uses the dock. Wherever it docks (each corner, each zoom), the frame is
// Alphi's for a click or a double click (no empty box made under it: isPointOverNote holds mcp::alphiHit of the docked
// frame), the point just outside is the canvas's, and the approval chips are laid out around it.
static void docked()
{
    std::printf("Alphi docked: hit map and chips at every corner\n");
    using M = mcp::ApprovalBarMetrics;
    mcp::AlphiCorner const corners[] = { mcp::AlphiCorner::bottomRight, mcp::AlphiCorner::bottomLeft,
        mcp::AlphiCorner::topRight, mcp::AlphiCorner::topLeft };
    for (float zoom : { 0.5f, 1.0f, 1.92f, 2.39f }) {
        for (auto view : { ApprovalRect { 300, 200, static_cast<int>(1400 / zoom), static_cast<int>(900 / zoom) },
                 ApprovalRect { 0, 0, static_cast<int>(890 / zoom), static_cast<int>(660 / zoom) } }) {
            for (auto c : corners) {
                auto const f = mcp::alphiCornerRect(view, zoom, c);
                CHECK(inside(f, view));
                // The hit map: every point of the frame is Alphi's, the ring around it is not.
                CHECK(mcp::alphiHit(f, f.x, f.y) && mcp::alphiHit(f, f.x + f.w - 1, f.y + f.h - 1));
                CHECK(mcp::alphiHit(f, f.x + f.w / 2, f.y + f.h / 2));
                CHECK(!mcp::alphiHit(f, f.x - 1, f.y) && !mcp::alphiHit(f, f.x, f.y - 1));
                CHECK(!mcp::alphiHit(f, f.x + f.w, f.y) && !mcp::alphiHit(f, f.x, f.y + f.h));
                // A box under a double click on Alphi: the dock never puts Alphi on a box when a corner is free, so
                // the double click lands on Alphi only where no box is (checked per corner with a box on the others).
                std::vector<ApprovalRect> others;
                for (auto o : corners)
                    if (o != c) {
                        auto const r = mcp::alphiCornerRect(view, zoom, o);
                        others.push_back({ r.x - 5, r.y - 5, r.w + 10, r.h + 10 });
                    }
                auto const choice = mcp::chooseCorner(view, zoom, others, {}, mcp::AlphiCorner::bottomRight);
                CHECK(choice.corner == c && !choice.folded);
                for (auto const& b : others)
                    CHECK(!meets(mcp::alphiCornerRect(view, zoom, choice.corner), b));
                // The chips, anchored anywhere around Alphi (up to 400 design px up/left, 100 down/right of its
                // corner; the stack clamps into the corner), keep clear of it: at the right and bottom corners they go
                // to its left or above it (avoidRect, 1a0; before, 638 of these layouts sat on Alphi).
                int overlaps = 0;
                for (int dx = -400; dx <= 100; dx += 20)
                    for (int dy = -400; dy <= 100; dy += 20) {
                        ApprovalRect const anchor { f.x + static_cast<int>(dx / zoom), f.y + static_cast<int>(dy / zoom), 10, 10 };
                        for (int shown = 1; shown <= M::maxBars; ++shown) {
                            auto const kept = mcp::approvalStackLayout(anchor, view, zoom, M::w, shown, false, f);
                            CHECK(!kept.bars.empty());
                            if (!kept.bars.empty() && meets(kept.frame, f))
                                ++overlaps;
                            for (auto const& b : kept.bars)
                                CHECK(inside(b.frame, view));
                        }
                    }
                CHECK(overlaps == 0);
            }
        }
    }
}

static void clicks()
{
    std::printf("Alphi click rule\n");
    using A = mcp::AlphiMetrics;
    // Not on screen yet (never painted) or for less than 400 ms: no click.
    CHECK(!mcp::alphiClickAllowed(0, 5000));
    CHECK(!mcp::alphiClickAllowed(5000, 5000 + A::minVisibleMs - 1));
    CHECK(mcp::alphiClickAllowed(5000, 5000 + A::minVisibleMs));
    // Review M2: for 400 ms after ANY approval chip left the glass (it may have sat on Alphi, in a view too small to
    // move it) a click on Alphi does nothing: a ✓ click whose chip the AI just cleared never lands on the switch.
    CHECK(!mcp::alphiClickAllowed(1000, 9000, 9000 - 1));
    CHECK(!mcp::alphiClickAllowed(1000, 9000, 9000 - A::minVisibleMs + 1));
    CHECK(mcp::alphiClickAllowed(1000, 9000, 9000 - A::minVisibleMs));
    CHECK(mcp::alphiClickAllowed(1000, 9000, 0)); // no chip ever went
    // Review M1: a click (or double click) inside Alphi is Alphi's, never the canvas's (no empty box under it).
    {
        ApprovalRect const f { 12, 12, 48, 56 };
        CHECK(mcp::alphiHit(f, 12, 12) && mcp::alphiHit(f, 59, 67) && mcp::alphiHit(f, 30, 40));
        CHECK(!mcp::alphiHit(f, 60, 40) && !mcp::alphiHit(f, 30, 68) && !mcp::alphiHit(f, 11, 40));
        CHECK(!mcp::alphiHit(ApprovalRect {}, 0, 0)); // not drawn: nothing is Alphi's
    }
    // Review M2: anything covering Alphi (a chip stack that could not move, the pill, its drawers) hides it.
    {
        ApprovalRect const a { 12, 12, 48, 56 };
        CHECK(mcp::alphiCovered(a, { ApprovalRect { 40, 50, 340, 74 } }));
        CHECK(!mcp::alphiCovered(a, { ApprovalRect { 72, 12, 340, 74 }, ApprovalRect {} }));
        CHECK(mcp::alphiCovered(a, { ApprovalRect {}, ApprovalRect { 0, 0, 13, 13 } }));
    }
    // A double click flips once; a later click flips again.
    mcp::AlphiClickGate g;
    CHECK(g.accept(10000));
    CHECK(!g.accept(10000 + A::debounceMs - 1));
    CHECK(g.accept(10000 + A::debounceMs + 1));
    CHECK(!g.accept(0)); // a broken clock never counts
}

static void toggle()
{
    std::printf("Alphi toggles the flow switch\n");
    // The host's click handler does exactly this (McpHost::alphiClicked): on when off, off when on.
    mcp::FlowMode f;
    CHECK(!f.isOn()); // restart = off
    CHECK(mcp::alphiNextOn(f.isOn()) == true);
    f.artistTurnOn(1000);
    CHECK(mcp::alphiNextOn(f.isOn()) == false);
    CHECK(f.off());
    mcp::FlowMode restarted;
    CHECK(!restarted.isOn());
}

static void look()
{
    std::printf("Alphi look\n");
    auto const off = mcp::alphiLook(false);
    auto const on = mcp::alphiLook(true);
    CHECK(!off.antennaLit && !off.label);
    CHECK(on.antennaLit && on.label && std::string(on.labelText) == "FLOW");
    // On: warm yellow eyes (red and green high, blue low), brighter than off; off: dim.
    CHECK(on.eye.r >= 230 && on.eye.g >= 170 && on.eye.b <= 90);
    CHECK(off.eye.r + off.eye.g + off.eye.b < on.eye.r + on.eye.g + on.eye.b);
    CHECK(off.eye.a < on.eye.a || off.eye.r < 160);
    CHECK(on.glow && !off.glow);
}

int main()
{
    placement();
    chipsKeepClear();
    docked();
    clicks();
    toggle();
    look();
    if (failures == 0)
        std::printf("alphi_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
