// Unit tests for the canvas approval state and the chip bar geometry (P12c, Source/Mcp/McpApprovalState.h).
// Build + run: scripts/test-bridge-safety.sh
//
// P12c (TS docs/plans/p12-flow-mode-steps.md, "Review round 1, F6 addendum"): the chips are the bridge's own state,
// one per pending record, keyed by the server's nonce; a click answers exactly that record; Enter only when exactly
// one record is pending and its chip has been on screen for >= 400 ms; expiry drops a chip; the whole bar is clamped
// inside the visible viewport and falls back to the viewport centre, never to the canvas origin.
#include "../../Source/Mcp/McpApprovalState.h"

#include <cmath>
#include <chrono>
#include <cstdio>
#include <optional>
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

static std::int64_t wallMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
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

    ApprovalRect const screen { 0, 0, 2000, 2000 };
    ApprovalRect const onScreen { 100, 100, 440, 62 };

    // 2. Showing the same nonce again replaces it (no duplicate chip) and restarts its on-screen time.
    {
        ApprovalState s;
        s.show("n", "pd-main", "first", {}, 60000, 0);
        s.markPaintedIfInside("n", onScreen, screen, 0);
        s.show("n", "pd-main", "second", {}, 60000, 1000);
        auto v = s.visible(1000);
        CHECK(v.size() == 1 && v[0].reason == "second");
        CHECK(v[0].onScreenSince() < 0);
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
        s.markPaintedIfInside("n1", onScreen, screen, 1000);
        CHECK(!s.enterTarget(1000).has_value());
        CHECK(!s.enterTarget(1000 + ApprovalState::kEnterMinVisibleMs - 1).has_value()); // too soon
        auto e = s.enterTarget(1000 + ApprovalState::kEnterMinVisibleMs);
        CHECK(e.has_value() && *e == "n1");
        s.show("n2", "pd-main", "r2", {}, 60000, 1500);
        s.markPaintedIfInside("n2", onScreen, screen, 1500);
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

    // 6. P12c review HIGH: the visible area is getViewArea() as it is (already canvas coordinates); at every zoom and
    //    an odd scroll position the bar lands inside it. (The old code divided by the zoom again: off-screen at 0.5,
    //    1.5 and 2.)
    {
        for (float zoom : { 0.5f, 1.0f, 1.5f, 2.0f })
            for (float scroll : { 0.0f, 333.3f, 64000.7f, -1234.5f }) {
                float const W = 1200, H = 800;
                // CanvasViewport::getViewArea(): { viewPosition / scale, size / scale }
                auto const view = mcp::approvalVisibleArea(scroll / zoom, (scroll * 0.75f) / zoom, W / zoom, H / zoom);
                auto const bar = mcp::approvalBarRect(std::nullopt, view, 440, 4 * 62 + 3 * 6 + 22);
                CHECK(inside(bar, view));
                auto const anchored = mcp::approvalBarRect(ApprovalRect { view.x - 9000, view.y + 9000, 50, 20 }, view, 440, 62);
                CHECK(inside(anchored, view));
            }
    }

    // 7. P12c review HIGH: a chip drawn off-screen never starts its on-screen time: Enter and a click cannot answer it.
    {
        ApprovalState s;
        s.show("n_x", "pd-main", "clear the entire canvas", { "*" }, 60000, 1000);
        ApprovalRect const view { 64000, 64000, 1200, 800 };
        CHECK(!s.markPaintedIfInside("n_x", ApprovalRect { 16000, 16069, 440, 62 }, view, 1000)); // off-screen
        CHECK(!s.enterTarget(1500).has_value());
        CHECK(!s.clickAllowed("n_x", 1500));
        CHECK(!s.markPaintedIfInside("n_x", ApprovalRect { 64000 + 1000, 64100, 440, 62 }, view, 1500)); // cut by the edge
        CHECK(!s.enterTarget(2500).has_value());
        CHECK(s.markPaintedIfInside("n_x", ApprovalRect { 64100, 64100, 440, 62 }, view, 3000)); // fully visible now
        CHECK(!s.enterTarget(3000 + ApprovalState::kEnterMinVisibleMs - 1).has_value());
        CHECK(s.enterTarget(3000 + ApprovalState::kEnterMinVisibleMs).value_or("") == "n_x");
        // Scrolled away again: the clock restarts.
        s.markPaintedIfInside("n_x", ApprovalRect { 0, 0, 440, 62 }, view, 4000);
        CHECK(!s.enterTarget(4100).has_value());
    }

    // 8. P12c review MEDIUM: a click needs the same 400 ms on screen as Enter (a chip that pops up under the cursor).
    {
        ApprovalState s;
        s.show("n_a", "pd-main", "r", {}, 60000, 0);
        CHECK(!s.clickAllowed("n_a", 0));
        s.markPaintedIfInside("n_a", onScreen, screen, 1000);
        CHECK(!s.clickAllowed("n_a", 1000 + ApprovalState::kEnterMinVisibleMs - 1));
        CHECK(s.clickAllowed("n_a", 1000 + ApprovalState::kEnterMinVisibleMs));
        CHECK(!s.clickAllowed("n_unknown", 5000));
        // A new chip is appended: the older one keeps its place (no reflow under the cursor).
        s.show("n_b", "pd-main", "r2", {}, 60000, 2000);
        auto v = s.visible(2000);
        CHECK(v.size() == 2 && v[0].nonce == "n_a" && v[1].nonce == "n_b");
    }

    // 9. P12c review MEDIUM (item 5): line 2 is cut with an ellipsis at a code point; line 1 is stored as given; the
    //    chip remembers which canvas it is about.
    {
        auto fits = [](std::size_t n) { return [n](std::string const& t) { return t.size() <= n; }; };
        CHECK(mcp::approvalEllipsize("file: short.pd", fits(40)) == "file: short.pd");
        auto cut = mcp::approvalEllipsize("file: gentle-tail-for-the-pad-only-adds-reverb.pd", fits(20));
        CHECK(cut.size() <= 20 && cut.substr(cut.size() - 3) == "\xe2\x80\xa6");
        auto umlaut = mcp::approvalEllipsize("array: l\xc3\xb6sch-alles-\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4", fits(16));
        CHECK(umlaut.size() <= 16);
        for (std::size_t i = 0; i + 3 < umlaut.size(); ++i) // no torn code point before the ellipsis
            if ((static_cast<unsigned char>(umlaut[i]) & 0xE0) == 0xC0) CHECK((static_cast<unsigned char>(umlaut[i + 1]) & 0xC0) == 0x80);
        CHECK(mcp::approvalCanvasLabel("pd-main") == "main");
        CHECK(mcp::approvalCanvasLabel("pd-scratch") == "scratch");
        CHECK(mcp::approvalCanvasLabel("song.pd") == "song.pd");

        ApprovalState s;
        mcp::ApprovalChip c;
        c.nonce = "n_t";
        c.canvas = "pd-scratch";
        c.title = "CLEAR \xe2\x80\x94 removes every object \xe2\x80\x94 canvas 'scratch'";
        c.detail = "";
        c.canvasId = 0x1234;
        CHECK(s.show(c, 60000, 0));
        auto v = s.visible(1);
        CHECK(v.size() == 1 && v[0].title == c.title && v[0].canvasId == 0x1234);
    }

    // 10. Round 3 item 3: a chip whose position changes (a chip above it went away) starts its on-screen time again.
    {
        ApprovalState s;
        s.show("n_top", "pd-main", "r", {}, 60000, 0);
        s.show("n_low", "pd-main", "r", {}, 60000, 0);
        ApprovalRect const view { 0, 0, 2000, 2000 };
        s.markPaintedIfInside("n_top", 1, ApprovalRect { 100, 100, 440, 78 }, view, 1000);
        s.markPaintedIfInside("n_low", 1, ApprovalRect { 100, 184, 440, 78 }, view, 1000);
        s.clear("n_top");
        s.markPaintedIfInside("n_low", 1, ApprovalRect { 100, 100, 440, 78 }, view, 1300); // moved up under the cursor
        CHECK(!s.clickAllowed("n_low", 1500));
        CHECK(!s.enterTarget(1500).has_value());
        CHECK(s.clickAllowed("n_low", 1300 + ApprovalState::kEnterMinVisibleMs));
        // Same place again: the time keeps running.
        s.markPaintedIfInside("n_low", 1, ApprovalRect { 100, 100, 440, 78 }, view, 1800);
        CHECK(s.clickAllowed("n_low", 1800));
    }

    // 11. Round 3 item 6: the timer is per chip, shared across views: fully visible in ANY view counts, and one view
    //     drawing it outside its area does not reset what another view started.
    {
        ApprovalState s;
        s.show("n", "pd-main", "r", {}, 60000, 0);
        ApprovalRect const viewA { 0, 0, 1000, 1000 };
        ApprovalRect const viewB { 5000, 5000, 300, 200 };
        s.markPaintedIfInside("n", 0xA, ApprovalRect { 100, 100, 440, 78 }, viewA, 1000);
        s.markPaintedIfInside("n", 0xB, ApprovalRect { 100, 100, 440, 78 }, viewB, 1200); // outside B
        CHECK(s.enterTarget(1000 + ApprovalState::kEnterMinVisibleMs).value_or("") == "n");
        CHECK(s.clickAllowed("n", 1000 + ApprovalState::kEnterMinVisibleMs));
        // A scrolls it away; B never showed it: no longer on screen anywhere.
        s.markPaintedIfInside("n", 0xA, ApprovalRect { 5000, 5000, 440, 78 }, viewA, 2000);
        CHECK(!s.clickAllowed("n", 2100));
    }


    // 12. P12c live "Zoom finding" (2026-11-02): from zoom ~1.6 the 440-wide bar, drawn in canvas units, no longer fit
    //     the view, never counted as fully on screen, and only Esc worked. The bar now has a fixed SCREEN size (canvas
    //     units = design px / zoom), shrinks further only when the window is smaller than the bar, and is clamped
    //     inside the view: at every zoom and window size every bar is inside, clickable and its buttons hit-testable.
    //     Owner 2026-11-02 ("easy to click but a bit big"): ~70-75% of the old 440x78 footprint, buttons >= 36 px.
    {
        using M = mcp::ApprovalBarMetrics;
        CHECK(M::w * M::h <= 440 * 78 * 75 / 100);
        CHECK(M::w * M::h >= 440 * 78 * 65 / 100);
        CHECK(M::btnH >= 36);
        CHECK(M::btnY + M::btnH <= M::h);
        CHECK(M::minTitleFont >= 9.0f);
        int const half = mcp::approvalButtonW(M::w);
        CHECK(M::btnX + half + M::btnGap + half <= M::w - M::btnX);

        struct Win { float w, h; };
        for (float zoom : { 0.5f, 1.0f, 1.6f, 2.0f, 3.0f })
            for (Win win : { Win { 1400, 900 }, Win { 1200, 800 }, Win { 800, 500 }, Win { 420, 300 }, Win { 300, 200 }, Win { 200, 120 } })
                for (float scroll : { 0.0f, 333.3f, 64000.7f, -1234.5f })
                    for (int shown : { 1, 2, 4 })
                        for (bool more : { false, true })
                            for (int a = 0; a < 3; ++a) {
                                auto const view = mcp::approvalVisibleArea(scroll / zoom, (scroll * 0.75f) / zoom, win.w / zoom, win.h / zoom);
                                std::optional<ApprovalRect> anchor;
                                if (a == 1) anchor = ApprovalRect { view.x - 9000, view.y + 9000, 50, 20 };
                                if (a == 2) anchor = ApprovalRect { view.x + view.w / 3, view.y + view.h / 3, 60, 20 };
                                for (int barW : { M::w, 520 }) {
                                    auto const L = mcp::approvalStackLayout(anchor, view, zoom, barW, shown, more);
                                    CHECK(static_cast<int>(L.bars.size()) == shown);
                                    CHECK(inside(L.frame, view));
                                    CHECK(L.scale > 0.0f && L.scale <= 1.0f / zoom + 1e-6f);
                                    // The drawn bar (design size times the scale) never exceeds its rect.
                                    CHECK(barW * L.scale <= L.frame.w + 1e-3f);
                                    for (std::size_t i = 0; i < L.bars.size(); ++i) {
                                        auto const& b = L.bars[i];
                                        CHECK(inside(b.frame, view));
                                        CHECK(inside(b.frame, L.frame));
                                        CHECK(M::h * L.scale <= b.frame.h + 1e-3f);
                                        CHECK(b.yes.w > 0 && b.yes.h > 0 && b.no.w > 0 && b.no.h > 0);
                                        CHECK(inside(b.yes, b.frame) && inside(b.no, b.frame));
                                        CHECK(b.yes.x + b.yes.w <= b.no.x); // side by side, no overlap
                                        if (i > 0) CHECK(L.bars[i - 1].frame.y + L.bars[i - 1].frame.h <= b.frame.y);
                                        // Clickable: the full-visibility rule holds for this rect, so the 400 ms clock
                                        // starts and a click / Enter can answer it.
                                        ApprovalState s;
                                        s.show("n", "pd-main", "r", {}, 60000, 0);
                                        CHECK(s.markPaintedIfInside("n", b.frame, view, 1000));
                                        CHECK(s.clickAllowed("n", 1000 + ApprovalState::kEnterMinVisibleMs));
                                    }
                                    // A window that holds the bar: it keeps its design size on screen (fixed pixels,
                                    // whatever the zoom), so the buttons stay >= 36 px tall.
                                    float const stackPx = static_cast<float>(shown * M::h + (shown - 1) * M::gap + (more ? M::more : 0));
                                    if (win.w >= barW + 2 * M::margin + 2 && win.h >= stackPx + 2 * M::margin + 2) {
                                        CHECK(std::abs(L.scale * zoom - 1.0f) < 0.02f);
                                        CHECK(L.bars[0].yes.h * zoom >= M::btnH - zoom - 1.0f);
                                        CHECK(std::abs(L.frame.w * zoom - barW) <= zoom + 1.0f);
                                    }
                                }
                            }
        // No reflow under the cursor when the window holds the full stack: a second chip leaves the first in place.
        {
            ApprovalRect const view = mcp::approvalVisibleArea(0, 0, 1200 / 2.0f, 800 / 2.0f);
            auto const one = mcp::approvalStackLayout(std::nullopt, view, 2.0f, M::w, 1, false);
            auto const two = mcp::approvalStackLayout(std::nullopt, view, 2.0f, M::w, 2, false);
            CHECK(one.bars[0].frame.x == two.bars[0].frame.x && one.bars[0].frame.y == two.bars[0].frame.y);
        }
        // Hit test: the centre of ✓ is in ✓ only, at zoom 2.
        {
            ApprovalRect const view = mcp::approvalVisibleArea(10, 10, 1200 / 2.0f, 800 / 2.0f);
            auto const L = mcp::approvalStackLayout(std::nullopt, view, 2.0f, M::w, 1, false);
            auto const& b = L.bars[0];
            int const cx = b.yes.x + b.yes.w / 2, cy = b.yes.y + b.yes.h / 2;
            auto contains = [](ApprovalRect const& r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; };
            CHECK(contains(b.yes, cx, cy) && !contains(b.no, cx, cy));
        }
        // Nothing to place: no bars.
        CHECK(mcp::approvalStackLayout(std::nullopt, ApprovalRect { 0, 0, 800, 600 }, 1.0f, M::w, 0, false).bars.empty());
    }

    // 13. LOW (live check 2026-11-02): an expired chip that is still drawn (no repaint yet) cannot be answered by a
    //     click or Enter, and the state says when the next chip expires so the canvas repaints it away on time.
    {
        ApprovalState s;
        CHECK(!s.nextExpiryMs().has_value());
        s.show("n_e", "pd-main", "r", {}, 1000, 0); // expires at 1000
        s.show("n_f", "pd-main", "r", {}, 5000, 0); // expires at 5000
        CHECK(s.nextExpiryMs().value_or(-1) == 1000);
        ApprovalRect const view { 0, 0, 2000, 2000 };
        CHECK(s.markPaintedIfInside("n_e", ApprovalRect { 100, 100, 340, 74 }, view, 100));
        CHECK(s.clickAllowed("n_e", 999));
        CHECK(!s.clickAllowed("n_e", 1000));             // expired, maybe still on the glass: the click is ignored
        CHECK(!s.take("n_e", 1000).has_value());         // and cannot be answered
        CHECK(s.nextExpiryMs().value_or(-1) == 5000);    // purged: the next one is the remaining chip
        s.clear("n_f");
        CHECK(!s.nextExpiryMs().has_value());
    }

    // 14. P12a.1 review M2: when ANY chip leaves the glass (answered, cleared by the server, expired, pushed out), the
    //     state says when, so Alphi (the flow switch under where a chip may have been) ignores clicks for a while.
    {
        ApprovalState s;
        CHECK(s.lastChipGoneMs() == 0);
        s.show("g_a", "pd-main", "r", {}, 60000, 1000);
        CHECK(s.lastChipGoneMs() == 0);           // showing is not going
        CHECK(s.take("g_a", 2000).has_value());
        CHECK(s.lastChipGoneMs() == 2000);        // answered
        CHECK(!s.take("g_a", 3000).has_value());
        CHECK(s.lastChipGoneMs() == 2000);        // nothing went: unchanged
        s.show("g_b", "pd-main", "r", {}, 60000, 4000);
        auto const before = wallMs();
        CHECK(s.clear("g_b"));                    // the server's /pd/approval_clear (no clock passed): the wall clock
        CHECK(s.lastChipGoneMs() >= before && s.lastChipGoneMs() <= wallMs());
        CHECK(!s.clear("g_b"));
        ApprovalState e;
        e.show("g_c", "pd-main", "r", {}, 1000, 0); // expires at 1000
        CHECK(e.visible(5000).empty());
        CHECK(e.lastChipGoneMs() == 1000);        // expired: stamped with its expiry
        ApprovalState f;
        for (std::size_t i = 0; i <= ApprovalState::kMaxChips; ++i)
            f.show("p" + std::to_string(i), "pd-main", "r", {}, 60000, 7000);
        CHECK(f.lastChipGoneMs() == 7000);        // the oldest pushed out by a new one
    }

    if (failures == 0)
        std::printf("approval_state_test: OK\n");
    return failures == 0 ? 0 : 1;
}
