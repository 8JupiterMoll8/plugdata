// Unit tests for the canvas approval state and the chip bar geometry (P12c, Source/Mcp/McpApprovalState.h).
// Build + run: scripts/test-bridge-safety.sh
//
// P12c (TS docs/plans/p12-flow-mode-steps.md, "Review round 1, F6 addendum"): the chips are the bridge's own state,
// one per pending record, keyed by the server's nonce; a click answers exactly that record; Enter only when exactly
// one record is pending and its chip has been on screen for >= 400 ms; expiry drops a chip; the whole bar is clamped
// inside the visible viewport and falls back to the viewport centre, never to the canvas origin.
#include "../../Source/Mcp/McpApprovalState.h"

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

using mcp::ApprovalState;
using mcp::ApprovalRect;

static bool inside(ApprovalRect const& r, ApprovalRect const& view)
{
    return r.x >= view.x && r.y >= view.y && r.x + r.w <= view.x + view.w && r.y + r.h <= view.y + view.h;
}

int main()
{
    // 1. One chip per record, keyed by nonce; a click takes exactly that record.
    {
        ApprovalState s;
        CHECK(!s.show("", "pd-main", "no nonce", {}, 60000, 0)); // a chip without a nonce is refused
        CHECK(s.show("n_old", "pd-main", "load old.pd", {}, 60000, 0));
        CHECK(s.show("n_new", "pd-main", "load new.pd", { "a", "b" }, 60000, 10));
        auto v = s.visible(20);
        CHECK(v.size() == 2);
        CHECK(v[0].nonce == "n_old" && v[1].nonce == "n_new"); // in the order asked
        CHECK((v[1].targets == std::vector<std::string> { "a", "b" }));
        auto t = s.take("n_old");
        CHECK(t.has_value() && t->reason == "load old.pd");
        CHECK(s.visible(30).size() == 1 && s.visible(30)[0].nonce == "n_new"); // the newer one is untouched
        CHECK(!s.take("n_old").has_value());                                    // a spent chip answers nothing
        CHECK(!s.take("n_guess").has_value());
        CHECK(s.clear("n_new"));
        CHECK(!s.clear("n_new"));
        CHECK(s.visible(40).empty());
    }

    // 2. Showing the same nonce again replaces it (no duplicate chip) and restarts its on-screen time.
    {
        ApprovalState s;
        s.show("n", "pd-main", "first", {}, 60000, 0);
        s.markPainted(0);
        s.show("n", "pd-main", "second", {}, 60000, 1000);
        auto v = s.visible(1000);
        CHECK(v.size() == 1 && v[0].reason == "second");
        CHECK(v[0].firstPaintedMs < 0);
    }

    // 3. Expiry drops the chip (the server also clears it; this covers a server that went away).
    {
        ApprovalState s;
        s.show("n", "pd-main", "r", {}, 5000, 0);
        CHECK(s.visible(4999).size() == 1);
        CHECK(s.visible(5001).empty());
        CHECK(!s.take("n").has_value());
        // A TTL is clamped: never 0 (gone at once) and never more than ten minutes.
        s.show("long", "pd-main", "r", {}, 999999999, 0);
        CHECK(s.visible(ApprovalState::kMaxTtlMs + 1).empty());
    }

    // 4. Enter: exactly one record, painted, on screen >= 400 ms.
    {
        ApprovalState s;
        s.show("n1", "pd-main", "r1", {}, 60000, 0);
        CHECK(!s.enterTarget(1000).has_value()); // never painted yet
        s.markPainted(1000);
        CHECK(!s.enterTarget(1000).has_value());
        CHECK(!s.enterTarget(1000 + ApprovalState::kEnterMinVisibleMs - 1).has_value()); // too soon
        auto e = s.enterTarget(1000 + ApprovalState::kEnterMinVisibleMs);
        CHECK(e.has_value() && *e == "n1");
        s.show("n2", "pd-main", "r2", {}, 60000, 1500);
        s.markPainted(1500);
        CHECK(!s.enterTarget(5000).has_value()); // two pending: Enter approves nothing
        // Esc cancels: every chip on screen.
        auto esc = s.escTargets(5000);
        CHECK((esc == std::vector<std::string> { "n1", "n2" }));
        s.clear("n1");
        CHECK(!s.enterTarget(1500 + ApprovalState::kEnterMinVisibleMs - 1).has_value());
        CHECK(s.enterTarget(1500 + ApprovalState::kEnterMinVisibleMs).value_or("") == "n2");
    }

    // 5. Geometry: the whole bar stays inside the visible viewport; no anchor -> viewport centre (never the origin).
    {
        ApprovalRect const view { 1000, 800, 900, 600 }; // the canvas scrolled far away from its origin
        auto r = mcp::approvalBarRect(std::nullopt, view, 440, 62);
        CHECK(inside(r, view));
        CHECK(r.x + r.w / 2 == view.x + view.w / 2);  // centred
        CHECK(r.y + r.h / 2 == view.y + view.h / 2);
        CHECK(!(r.x < 200 && r.y < 200));               // not near canvasOrigin (0,0)

        // An anchor (the pill / the selection) far off-screen: clamped back inside.
        auto off = mcp::approvalBarRect(ApprovalRect { -5000, -5000, 100, 20 }, view, 440, 62);
        CHECK(inside(off, view));
        auto offRight = mcp::approvalBarRect(ApprovalRect { 5000, 5000, 100, 20 }, view, 440, 62);
        CHECK(inside(offRight, view));
        // An anchor inside: the bar sits under it.
        auto under = mcp::approvalBarRect(ApprovalRect { 1300, 900, 100, 20 }, view, 440, 62);
        CHECK(inside(under, view));
        CHECK(under.y >= 900 + 20);
        CHECK(under.x + under.w / 2 == 1350);
        // An anchor at the bottom edge: the bar flips above or is clamped, but stays inside.
        auto bottom = mcp::approvalBarRect(ApprovalRect { 1300, 1390, 100, 8 }, view, 440, 62);
        CHECK(inside(bottom, view));
        // A stack taller than the margin still starts inside; a bar wider than the view starts at its left edge.
        auto wide = mcp::approvalBarRect(std::nullopt, ApprovalRect { 0, 0, 300, 200 }, 440, 62);
        CHECK(wide.x == 0);
        CHECK(wide.y >= 0 && wide.y + wide.h <= 200);
        // An empty viewport (not laid out yet) is not a reason to go to the origin's corner: centre of what there is.
        auto empty = mcp::approvalBarRect(std::nullopt, ApprovalRect { 500, 400, 0, 0 }, 440, 62);
        CHECK(empty.x + empty.w / 2 == 500 && empty.y + empty.h / 2 == 400);
    }

    if (failures == 0)
        std::printf("approval_state_test: OK\n");
    return failures == 0 ? 0 : 1;
}
