// Unit tests for P12a.1 (Alphi, the flow-mode toggle on the canvas), JUCE-free (Source/Mcp/McpAlphi.h):
//  - where Alphi sits: the top-left corner of the VISIBLE view at a fixed screen size (>= 44 px touch target) at any zoom;
//  - the approval chips never land on Alphi (approvalStackLayout with Alphi as the keep-out), at any zoom and anchor;
//  - the click rule: a click counts only once Alphi has been on screen 400 ms, and a double click does not flip it twice;
//  - the look: off = calm dim eyes, on = warm yellow eyes + antenna light + "FLOW".
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpApprovalState.h"
#include "../../Source/Mcp/McpAlphi.h"
#include "../../Source/Mcp/McpFlowMode.h"

#include <cmath>
#include <cstdio>
#include <string>

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

static void clicks()
{
    std::printf("Alphi click rule\n");
    using A = mcp::AlphiMetrics;
    // Not on screen yet (never painted) or for less than 400 ms: no click.
    CHECK(!mcp::alphiClickAllowed(0, 5000));
    CHECK(!mcp::alphiClickAllowed(5000, 5000 + A::minVisibleMs - 1));
    CHECK(mcp::alphiClickAllowed(5000, 5000 + A::minVisibleMs));
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
    clicks();
    toggle();
    look();
    if (failures == 0)
        std::printf("alphi_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
