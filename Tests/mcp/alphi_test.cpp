// Unit tests for P12a.1 (Alphi, the flow-mode toggle on the canvas), JUCE-free (Source/Mcp/McpAlphi.h):
//  - where Alphi sits: the top-left corner of the VISIBLE view at a fixed screen size (>= 44 px touch target) at any zoom;
//  - the approval chips never land on Alphi (approvalStackLayout with Alphi as the keep-out), at any zoom and anchor;
//  - the click rule: a click counts only once Alphi has been on screen 400 ms, and a double click does not flip it twice;
//  - the look: off = calm dim eyes, on = warm yellow eyes + antenna light + "FLOW".
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpApprovalState.h"
#include "../../Source/Mcp/McpAlphi.h"
#include "../../Source/Mcp/McpAlphiDock.h"
#include "../../Source/Mcp/McpAlphiMotion.h"
#include "../../Source/Mcp/McpFlowMode.h"

#include <cmath>
#include <cstdint>
#include <optional>
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
    CHECK(f.off(5000));
    // Review of 2a13ec760: every door that switches flow off records when (the way ON waits flowOffGapMs after it);
    // an off while already off keeps the first time.
    CHECK(f.lastOffMs() == 5000);
    CHECK(!f.off(6000));
    CHECK(f.lastOffMs() == 5000);
    CHECK(!mcp::alphiClickAllowed(1000, 5000 + mcp::AlphiMetrics::flowOffGapMs - 1, 0, f.lastOffMs()));
    CHECK(mcp::alphiClickAllowed(1000, 5000 + mcp::AlphiMetrics::flowOffGapMs, 0, f.lastOffMs()));
    mcp::FlowMode restarted;
    CHECK(restarted.lastOffMs() == 0);
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

// Alphi drop 1 F1 (D2, D3): the eyes pulse with the real output. alphiPulse maps the smoothed loudness (mean square)
// to a level 0..1 (-60 dBFS -> 0, -6 dBFS -> 1) and the eye per look; calm (the resting look) below -60 dBFS, on
// silence, NaN, and when no new block came for 200 ms ("nothing measured", never a claim of silence).
static void pulse()
{
    std::printf("Alphi pulse\n");
    auto const off = mcp::alphiLook(false);
    auto const on = mcp::alphiLook(true);
    using P = mcp::AlphiPulseMetrics;
    auto const ms = [](double db) { return static_cast<float>(std::pow(10.0, db / 10.0)); };
    // The curve: dB-linear, -60 -> 0, -6 -> 1, monotonic, clamped.
    CHECK(mcp::alphiPulse(ms(-60.0), 0, off).level == 0.0f);
    CHECK(std::fabs(mcp::alphiPulse(ms(-6.0), 0, off).level - 1.0f) < 1e-4f);
    CHECK(mcp::alphiPulse(1.0f, 0, off).level == 1.0f);
    CHECK(mcp::alphiPulse(4.0f, 0, off).level == 1.0f);
    CHECK(std::fabs(mcp::alphiPulse(ms(-33.0), 0, off).level - 0.5f) < 1e-3f);
    float prev = -1.0f;
    for (int db = -80; db <= 0; db += 2) {
        float const l = mcp::alphiPulse(ms(db), 10, on).level;
        CHECK(l >= prev && l >= 0.0f && l <= 1.0f);
        prev = l;
    }
    // Silence, NaN, inf, negative: calm.
    for (float bad : { 0.0f, -1.0f, std::nanf(""), INFINITY, -INFINITY }) {
        auto const p = mcp::alphiPulse(bad, 0, on);
        CHECK(p.level == 0.0f && p.calm);
    }
    CHECK(mcp::alphiPulse(ms(-61.0), 0, on).calm);
    CHECK(!mcp::alphiPulse(ms(-18.0), 0, on).calm);
    // Stale: no new block for 200 ms (device stopped, ceiling off, plugin build) -> calm; a negative age too.
    CHECK(!mcp::alphiPulse(ms(-18.0), P::staleMs, on).calm);
    CHECK(mcp::alphiPulse(ms(-18.0), P::staleMs + 1, on).calm);
    CHECK(mcp::alphiPulse(ms(-18.0), 5000, on).level == 0.0f);
    CHECK(mcp::alphiPulse(ms(-18.0), -1, on).calm);
    // The off look: the flat dash grows from 2.2 to ~5 design px; the on look: the eye radius grows; calm = resting.
    auto const offRest = mcp::alphiPulse(0.0f, 0, off);
    auto const offFull = mcp::alphiPulse(1.0f, 0, off);
    CHECK(std::fabs(offRest.dashHeight - 2.2f) < 1e-4f && std::fabs(offFull.dashHeight - 5.0f) < 1e-4f);
    CHECK(offRest.eyeRadius == off.eyeRadius && offFull.eyeRadius == off.eyeRadius);
    auto const onRest = mcp::alphiPulse(0.0f, 0, on);
    auto const onFull = mcp::alphiPulse(1.0f, 0, on);
    CHECK(onRest.eyeRadius == on.eyeRadius);
    CHECK(onFull.eyeRadius > on.eyeRadius * 1.3f && onFull.eyeRadius < on.eyeRadius * 1.6f);
    CHECK(onRest.dashHeight == onFull.dashHeight);
    auto const onMid = mcp::alphiPulse(ms(-33.0), 0, on);
    CHECK(onMid.eyeRadius > onRest.eyeRadius && onMid.eyeRadius < onFull.eyeRadius);
    // From a level: the same mapping the canvas uses after its attack/release.
    CHECK(mcp::alphiEyesAt(0.0f, on).eyeRadius == on.eyeRadius);
    CHECK(mcp::alphiEyesAt(1.0f, off).dashHeight == offFull.dashHeight);
}

// The canvas's attack / release (D2: 30 ms / 150 ms, on the GUI side): the shown level moves toward the target by at
// most dt/30 up and dt/150 down per ms, so a stop is calm within 150 ms (+ the 200 ms stale wait when nothing comes).
static void pulseFollow()
{
    std::printf("Alphi pulse attack / release\n");
    using P = mcp::AlphiPulseMetrics;
    CHECK(P::attackMs == 30.0f && P::releaseMs == 150.0f);
    CHECK(std::fabs(mcp::alphiPulseFollow(0.0f, 1.0f, 15.0f) - 0.5f) < 1e-5f);
    CHECK(mcp::alphiPulseFollow(0.0f, 1.0f, 30.0f) == 1.0f);
    CHECK(mcp::alphiPulseFollow(0.0f, 1.0f, 100.0f) == 1.0f); // never past the target
    CHECK(std::fabs(mcp::alphiPulseFollow(1.0f, 0.0f, 75.0f) - 0.5f) < 1e-5f);
    CHECK(mcp::alphiPulseFollow(1.0f, 0.0f, 150.0f) == 0.0f);
    CHECK(mcp::alphiPulseFollow(0.4f, 0.4f, 33.0f) == 0.4f);
    // Bad input: NaN / negative dt keeps the level; a bad level or target is calm.
    CHECK(mcp::alphiPulseFollow(0.4f, 1.0f, std::nanf("")) == 0.4f);
    CHECK(mcp::alphiPulseFollow(0.4f, 1.0f, -5.0f) == 0.4f);
    CHECK(mcp::alphiPulseFollow(std::nanf(""), 1.0f, 10.0f) >= 0.0f);
    CHECK(mcp::alphiPulseFollow(0.4f, std::nanf(""), 1000.0f) == 0.0f);
    // L1: a stop (target 0 from full) is at rest within 0.5 s, even with the 200 ms stale wait first.
    float l = 1.0f;
    for (int t = 0; t < 300; t += 33)
        l = mcp::alphiPulseFollow(l, 0.0f, 33.0f);
    CHECK(l == 0.0f);
}

// ---- Alphi calm (2026-11-22): the eyes repaint on what is DRAWN, not on the level -------------------------------------
// The idle cost found by reading: the ceiling publishes the raw per-block mean square, so a steady hiss / drone / music
// bed at -55 dBFS +-1.5 dB moved the followed level by more than the old 0.02 step on most 33 ms ticks, and the motion
// timer (30 Hz) never settled. Now the eye's drawn size (the on look's radius, the off look's dash height) snaps to a
// 0.25 design px grid anchored at the resting look, with a one-step hold (hysteresis), and a repaint is asked only
// when that drawn size changes. The tests drive the same chain the canvas runs per tick: block mean square ->
// alphiPulse -> alphiPulseFollow -> alphiPulseWake -> (repaint) alphiPulseHeld.
namespace calm {

struct Lcg {
    std::uint32_t s = 12345u;
    float next() // 0..1
    {
        s = s * 1664525u + 1013904223u;
        return static_cast<float>(s >> 8) / 16777216.0f;
    }
};

constexpr float tickMs = 33.0f;
float msOf(double db) { return static_cast<float>(std::pow(10.0, db / 10.0)); }

// The new chain, one 33 ms tick at a time.
struct Sim {
    mcp::AlphiLook look;
    float level = 0.0f, drawn = 0.0f;
    int repaints = 0, lastRepaint = -1, tick = 0;
    void step(float meanSquare)
    {
        float const target = mcp::alphiPulse(meanSquare, 0, look).level; // a fresh block
        level = mcp::alphiPulseFollow(level, target, tickMs);
        if (mcp::alphiPulseWake(drawn, level, look)) {
            drawn = mcp::alphiPulseHeld(drawn, level, look); // the repaint draws, and records, this
            ++repaints;
            lastRepaint = tick;
        }
        ++tick;
    }
};

// Today's rule (before this change), kept here to show what it cost: repaint whenever the followed level is 0.02 or
// more from the drawn one, or crossed -60 dBFS; the drawn level is the followed one.
struct Legacy {
    float level = 0.0f, drawn = 0.0f;
    int repaints = 0;
    void step(float meanSquare)
    {
        float const target = mcp::alphiPulseLevel(meanSquare);
        level = mcp::alphiPulseFollow(level, target, tickMs);
        if (std::fabs(level - drawn) >= 0.02f || ((drawn > 0.0f) != (level > 0.0f))) {
            drawn = level;
            ++repaints;
        }
    }
};

float growthPx(float level, mcp::AlphiLook const& look) { return level * mcp::alphiEyeGrowthPx(look); }

} // namespace calm

static void pulseCalmJitter()
{
    std::printf("Alphi calm: a steady -55 dBFS +-1.5 dB for 10 s settles\n");
    int const ticks = 10000 / 33; // one sample per 33 ms over 10 s
    for (bool flow : { false, true }) {
        auto const look = mcp::alphiLook(flow);
        calm::Sim sim;
        sim.look = look;
        calm::Legacy old;
        calm::Lcg rng;
        for (int i = 0; i < ticks; ++i) {
            float const ms = calm::msOf(-55.0 + (rng.next() * 2.0 - 1.0) * 1.5);
            sim.step(ms);
            old.step(ms);
        }
        std::printf("  %s look: old rule %d repaints in %d ticks, new rule %d (limit 10, last at tick %d)\n",
            flow ? "on " : "off", old.repaints, ticks, sim.repaints, sim.lastRepaint);
        CHECK(old.repaints >= ticks / 3); // the cost this removes: a repaint on a large share of the ticks
        CHECK(sim.repaints <= 10);
        CHECK(sim.lastRepaint < 30); // settled within the first second
    }
    // Every steady level (+-1.5 dB of jitter, the whole range): settled within a second, nothing after.
    for (bool flow : { false, true }) {
        auto const look = mcp::alphiLook(flow);
        for (int centre = -59; centre <= -7; ++centre) {
            calm::Sim sim;
            sim.look = look;
            calm::Lcg rng;
            rng.s = 777u + static_cast<std::uint32_t>(centre + 100);
            for (int i = 0; i < ticks; ++i)
                sim.step(calm::msOf(centre + (rng.next() * 2.0 - 1.0) * 1.5));
            CHECK(sim.repaints <= 10);
            CHECK(sim.lastRepaint < 30);
        }
    }
}

static void pulseCalmSteadyAndZero()
{
    std::printf("Alphi calm: steady level, digital zero, real changes\n");
    for (bool flow : { false, true }) {
        auto const look = mcp::alphiLook(flow);
        // A perfectly steady level: at most the climb, then no repaint at all.
        calm::Sim steady;
        steady.look = look;
        for (int i = 0; i < 303; ++i)
            steady.step(calm::msOf(-30.0));
        CHECK(steady.repaints <= 2);
        CHECK(steady.lastRepaint < 5);
        // Digital zero (a stopped transport): calm, exactly, within the 150 ms release (+ the tick), then silence.
        calm::Sim s;
        s.look = look;
        for (int i = 0; i < 60; ++i)
            s.step(calm::msOf(-20.0));
        CHECK(s.drawn > 0.0f);
        int calmAt = -1;
        for (int i = 0; i < 150; ++i) {
            s.step(0.0f);
            if (calmAt < 0 && s.drawn == 0.0f)
                calmAt = i;
        }
        CHECK(calmAt >= 0 && calmAt <= 150 / 33 + 1); // within ~5 ticks = the release time
        CHECK(s.level == 0.0f && s.drawn == 0.0f);
        int const repaintsAtCalm = s.repaints;
        for (int i = 0; i < 150; ++i)
            s.step(0.0f);
        CHECK(s.repaints == repaintsAtCalm); // 5 more seconds of zeros: no repaint
        mcp::AlphiMotionInput in;
        in.shown = true;
        in.drawn = s.drawn;
        in.target = mcp::alphiPulseHeld(s.drawn, s.level, look);
        CHECK(!mcp::alphiMotionRun(in)); // the motion timer has nothing left to run for
        // The eyes are exactly the resting look when calm.
        auto const e = mcp::alphiEyesAt(s.drawn, look);
        CHECK(e.calm && e.eyeRadius == look.eyeRadius && e.dashHeight == mcp::AlphiPulseMetrics::dashRest);
    }
}

static void pulseCalmMusicStillMoves()
{
    std::printf("Alphi calm: a real level change, or music, still repaints the eyes\n");
    for (bool flow : { false, true }) {
        auto const look = mcp::alphiLook(flow);
        calm::Sim s;
        s.look = look;
        for (int i = 0; i < 40; ++i)
            s.step(calm::msOf(-50.0));
        float const quiet = calm::growthPx(s.drawn, look);
        int const before = s.repaints;
        for (int i = 0; i < 6; ++i)
            s.step(calm::msOf(-20.0)); // a hit
        CHECK(s.repaints > before);
        float const loud = calm::growthPx(s.drawn, look);
        CHECK(loud >= quiet + 0.75f); // visibly bigger (the full range is 1.6 px on, 2.8 px off)
        int const mid = s.repaints;
        for (int i = 0; i < 12; ++i)
            s.step(calm::msOf(-50.0)); // and back
        CHECK(s.repaints > mid);
        CHECK(std::fabs(calm::growthPx(s.drawn, look) - quiet) <= mcp::AlphiPulseMetrics::drawStepPx + 1e-3f);
        // Music: a level that wanders 18 dB, a new value every 100 ms, for 10 s: the eyes pulse with it.
        calm::Sim m;
        m.look = look;
        calm::Lcg rng;
        rng.s = 4242u;
        double db = -24.0;
        for (int i = 0; i < 303; ++i) {
            if (i % 3 == 0)
                db = -34.0 + rng.next() * 24.0;
            m.step(calm::msOf(db));
        }
        CHECK(m.repaints >= 30);
        CHECK(m.repaints <= 303);
    }
}

static void pulseCalmQuantize()
{
    std::printf("Alphi calm: the drawn size is on the 0.25 px grid, anchored at the resting look\n");
    using P = mcp::AlphiPulseMetrics;
    CHECK(P::drawStepPx == 0.25f);
    for (bool flow : { false, true }) {
        auto const look = mcp::alphiLook(flow);
        float const step = P::drawStepPx;
        CHECK(mcp::alphiEyeGrowthPx(look) > step * 4.0f); // the grid has room (1.6 px on, 2.8 px off)
        // Calm is exact, whatever was drawn before.
        for (float d : { 0.0f, 0.09f, 0.5f, 0.93f, 1.0f })
            CHECK(mcp::alphiPulseHeld(d, 0.0f, look) == 0.0f);
        // Up and down sweeps: always on the grid, never more than one step off the raw level, ends exactly calm.
        for (int dir = 0; dir < 2; ++dir) {
            float drawn = dir == 0 ? 0.0f : mcp::alphiPulseHeld(0.0f, 1.0f, look); // the top grid point
            float maxErr = 0.0f;
            for (int i = 0; i <= 1000; ++i) {
                float const level = dir == 0 ? static_cast<float>(i) / 1000.0f : static_cast<float>(1000 - i) / 1000.0f;
                drawn = mcp::alphiPulseHeld(drawn, level, look);
                float const g = calm::growthPx(drawn, look) / step;
                CHECK(std::fabs(g - std::round(g)) < 1e-3f);
                CHECK(drawn >= 0.0f && drawn <= 1.0f);
                if (level > 0.0f)
                    maxErr = std::fmax(maxErr, std::fabs(calm::growthPx(drawn, look) - calm::growthPx(level, look)));
            }
            CHECK(maxErr < step + 1e-3f);
            CHECK(mcp::alphiPulseHeld(drawn, 0.0f, look) == 0.0f); // and the way back to calm is exact
        }
        // Hold: anything within one step of the drawn size stays drawn; one full step away moves it.
        float const g3 = 3.0f * step / mcp::alphiEyeGrowthPx(look); // the grid point #3 as a level
        CHECK(mcp::alphiPulseHeld(g3, g3 + 0.99f * step / mcp::alphiEyeGrowthPx(look), look) == g3);
        CHECK(mcp::alphiPulseHeld(g3, g3 - 0.99f * step / mcp::alphiEyeGrowthPx(look), look) == g3);
        CHECK(mcp::alphiPulseHeld(g3, g3 + 1.2f * step / mcp::alphiEyeGrowthPx(look), look) != g3);
        // The resting look is unchanged: calm eyes draw the look's own radius / dash.
        auto const rest = mcp::alphiEyesAt(mcp::alphiPulseHeld(0.4f, 0.0f, look), look);
        CHECK(rest.eyeRadius == look.eyeRadius && rest.dashHeight == P::dashRest);
        // Bad input: never NaN, never out of 0..1.
        for (float bad : { std::nanf(""), INFINITY, -INFINITY, -3.0f, 7.0f }) {
            float const h = mcp::alphiPulseHeld(0.3f, bad, look);
            CHECK(std::isfinite(h) && h >= 0.0f && h <= 1.0f);
            float const h2 = mcp::alphiPulseHeld(bad, 0.6f, look);
            CHECK(std::isfinite(h2) && h2 >= 0.0f && h2 <= 1.0f);
        }
    }
}

// Alphi calm, item 4: a paint that did not cover his rect leaves the pulse / gaze "drawn" as it was; the next one that
// does records it (before: every canvas paint recorded, so eyes could stay stale until the level moved again).
static void pulseCalmRecordsOnlyWhenCovered()
{
    std::printf("Alphi calm: only a paint that covered his rect records the eyes as drawn\n");
    CHECK(mcp::alphiDrawnAfterPaint(0.2f, 0.8f, true) == 0.8f);
    CHECK(mcp::alphiDrawnAfterPaint(0.2f, 0.8f, false) == 0.2f);
    auto const g = mcp::alphiDrawnAfterPaint(mcp::AlphiGaze { 1.0f, 2.0f }, mcp::AlphiGaze { 3.0f, -3.0f }, false);
    CHECK(g.dx == 1.0f && g.dy == 2.0f);
    // The dizzy spiral is drawn on the face: it is recorded with his rect (not with the strip), so a paint elsewhere
    // while it starts or ends never marks it as drawn, and the motion timer still repaints his face.
    bool dizzyDrawn = false;
    dizzyDrawn = mcp::alphiDrawnAfterPaint(dizzyDrawn, true, false); // a failed final arrives, a paint elsewhere
    CHECK(!dizzyDrawn);                                              // still "not dizzy" on screen: the wake sees it
    dizzyDrawn = mcp::alphiDrawnAfterPaint(dizzyDrawn, true, true);  // his own repaint
    CHECK(dizzyDrawn);
    dizzyDrawn = mcp::alphiDrawnAfterPaint(dizzyDrawn, false, false); // it ends, a paint elsewhere
    CHECK(dizzyDrawn);                                                // the spiral is still on screen: still woken
    // The stale-eyes sequence: the level rose to 0.8 while 0.2 was drawn. A canvas paint far away (a box dragged) calls
    // paintAlphi too: it must not mark 0.8 as drawn, so the 10 Hz wake still asks for his rect.
    auto const look = mcp::alphiLook(true);
    float drawn = 0.2f;
    float const level = 0.8f;
    CHECK(mcp::alphiPulseWake(drawn, level, look));
    drawn = mcp::alphiDrawnAfterPaint(drawn, mcp::alphiPulseHeld(drawn, level, look), false); // paint elsewhere
    CHECK(mcp::alphiPulseWake(drawn, level, look));                                       // still stale: still woken
    drawn = mcp::alphiDrawnAfterPaint(drawn, mcp::alphiPulseHeld(drawn, level, look), true); // his own repaint
    CHECK(!mcp::alphiPulseWake(drawn, level, look));
}

// Alphi drop 1 F2 (D5): his eyes look toward the selection's centre, clamped to +-3 design px per axis (direction
// kept); no selection: straight ahead. Screen-constant: the same screen distance gives the same look at any zoom.
static void gaze()
{
    std::printf("Alphi gaze\n");
    using A = mcp::AlphiMetrics;
    using G = mcp::AlphiGazeMetrics;
    CHECK(G::maxPx == 3.0f);
    ApprovalRect const view { 0, 0, 1200, 800 };
    auto const face = mcp::alphiCornerRect(view, 1.0f, mcp::AlphiCorner::bottomRight);
    int const cx = face.x + face.w / 2, cy = face.y + face.h / 2;
    auto const at = [](int x, int y) { return ApprovalRect { x - 10, y - 5, 20, 10 }; };
    // None: straight ahead (also an empty selection rect, and no face).
    auto g = mcp::alphiGaze(face, std::nullopt);
    CHECK(g.dx == 0.0f && g.dy == 0.0f);
    g = mcp::alphiGaze(face, ApprovalRect {});
    CHECK(g.dx == 0.0f && g.dy == 0.0f);
    g = mcp::alphiGaze(ApprovalRect {}, at(10, 10));
    CHECK(g.dx == 0.0f && g.dy == 0.0f);
    // On his centre: straight ahead.
    g = mcp::alphiGaze(face, at(cx, cy));
    CHECK(std::fabs(g.dx) < 1e-4f && std::fabs(g.dy) < 1e-4f);
    // Left / right at his height: x only, toward the selection.
    g = mcp::alphiGaze(face, at(cx - 80, cy));
    CHECK(g.dx < -0.5f && g.dx >= -G::maxPx && std::fabs(g.dy) < 1e-4f);
    float const left80 = g.dx;
    g = mcp::alphiGaze(face, at(cx + 80, cy));
    CHECK(std::fabs(g.dx + left80) < 1e-4f && std::fabs(g.dy) < 1e-4f);
    // Nearer = less turned (until the clamp).
    CHECK(std::fabs(mcp::alphiGaze(face, at(cx - 20, cy)).dx) < std::fabs(left80));
    // Far: clamped to +-3 per axis, direction kept (the larger axis at exactly 3).
    g = mcp::alphiGaze(face, at(cx - 1200, cy - 600));
    CHECK(std::fabs(g.dx + G::maxPx) < 1e-4f);
    CHECK(g.dy < 0.0f && std::fabs(g.dy - (-G::maxPx * 0.5f)) < 0.01f);
    g = mcp::alphiGaze(face, at(cx, cy - 100000));
    CHECK(std::fabs(g.dy + G::maxPx) < 1e-4f && std::fabs(g.dx) < 1e-3f);
    // Zoomed: canvas units are 1/zoom design px; the same screen distance gives the same gaze.
    for (float zoom : { 0.5f, 2.0f, 2.39f }) {
        ApprovalRect const v { 100, 50, static_cast<int>(1200 / zoom), static_cast<int>(800 / zoom) };
        auto const f = mcp::alphiCornerRect(v, zoom, mcp::AlphiCorner::bottomRight);
        CHECK(std::abs(static_cast<int>(f.w * zoom) - A::w) <= 3); // the frame is ceil(48 / zoom) canvas units
        int const fx = f.x + f.w / 2, fy = f.y + f.h / 2;
        auto const z = mcp::alphiGaze(f, at(fx - static_cast<int>(std::lround(80 / zoom)), fy));
        CHECK(std::fabs(z.dx - left80) < 0.12f);
        CHECK(std::fabs(z.dy) < 0.12f);
    }
    // Never NaN, whatever comes in.
    g = mcp::alphiGaze(ApprovalRect { 0, 0, 1, 1 }, ApprovalRect { 2147480000, 2147480000, 10, 10 });
    CHECK(std::isfinite(g.dx) && std::isfinite(g.dy) && std::fabs(g.dx) <= G::maxPx && std::fabs(g.dy) <= G::maxPx);
}

int main()
{
    gaze();
    pulse();
    pulseFollow();
    pulseCalmQuantize();
    pulseCalmJitter();
    pulseCalmSteadyAndZero();
    pulseCalmMusicStillMoves();
    pulseCalmRecordsOnlyWhenCovered();
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
