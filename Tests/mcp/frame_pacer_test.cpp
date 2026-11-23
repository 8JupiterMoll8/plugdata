// Unit tests for the GL frame pacer (lag fix, Fable's diagnosis 2026-11-23, fix #2), JUCE-free (Source/Mcp/McpFramePacer.h).
//
// On GL/X11 NVGSurface used to record a frame for every invalidation (AsyncUpdater -> recordFrame), with no limit, while
// the render thread sat in swapBuffers: a recorded frame that is still pending when the next one is published is thrown
// away, and recordFrame folded the thrown-away frame's damage back in, so the damaged area only grew (the ratchet).
// The pacer decides, on the message thread, WHEN a frame is recorded:
//  - invalidations only mark the surface dirty (the damage rects stay in NVGSurface until a frame takes them);
//  - at most one frame per interval (16 ms), the first one after idle at once (leading edge), the last damage of a
//    burst one interval later (trailing edge);
//  - while the render thread still holds the previous frame the tick records nothing, keeps the damage, and looks again
//    shortly (it also wakes the render thread, in case its wake-up got lost);
//  - an urgent request (first frame, a render thread that needs a full frame, readback, screenshot) records at once;
//  - a frame that stays pending for stuckMs (a render thread that cannot take it) is replaced, once per stuckMs.
// The fake clock below plays the owner's role: it arms the wake the pacer asks for and runs the tick when it is due.
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpFrameLog.h"
#include "../../Source/Mcp/McpFramePacer.h"

#include <algorithm>
#include <cstdint>
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

using mcp::FramePacer;

// The owner (NVGSurface) and the render thread, event driven on a fake clock.
struct Sim {
    FramePacer pacer;
    double now = 0.0;
    double wakeAt = -1.0;       // when the armed timer / async update fires; < 0 = nothing armed
    bool pending = false;       // the render thread has not taken the last recorded frame yet
    double pendingSince = 0.0;
    double swapMs = 0.0;        // how long the render thread is busy after taking a frame (swapBuffers)
    double renderFreeAt = 0.0;
    bool renderStuck = false;   // the render thread never takes the frame
    int records = 0;
    int recordsWhilePending = 0; // a record that replaced a pending frame
    int nudges = 0;
    int taken = 0;               // frames the render thread took

    void arm(double delay)
    {
        if (delay >= 0.0)
            wakeAt = now + delay;
    }

    void invalidate() { arm(pacer.invalidate(now)); }
    void invalidateNow() { arm(pacer.invalidateNow(now)); }

    void runTick()
    {
        auto const d = pacer.tick(now, pending);
        if (d.action == FramePacer::Action::Record) {
            ++records;
            if (pending)
                ++recordsWhilePending;
            pending = true;
            pendingSince = now;
        }
        if (d.nudgeRender)
            ++nudges;
        if (d.nextWakeMs >= 0.0)
            wakeAt = now + d.nextWakeMs;
    }

    // Run every event due up to t (the render thread taking a frame, a wake), then move the clock to t.
    void runUntil(double t)
    {
        constexpr double never = 1.0e18;
        for (;;) {
            double const takeAt = pending && !renderStuck ? std::max(pendingSince, renderFreeAt) : never;
            double const wake = wakeAt >= 0.0 ? wakeAt : never;
            double const next = std::min(takeAt, wake);
            if (next > t)
                break;
            now = std::max(now, next);
            if (takeAt <= wake) {
                pending = false;
                ++taken;
                renderFreeAt = now + swapMs;
            } else {
                wakeAt = -1.0;
                runTick();
            }
        }
        now = std::max(now, t);
    }
};

static void burst()
{
    std::printf("pacer: 1000 invalidations in 100 ms record at most 7 frames, the last damage one interval later\n");
    Sim s;
    for (int i = 0; i < 1000; ++i) { // one invalidation every 0.1 ms
        s.runUntil(i * 0.1);
        s.invalidate();
    }
    s.runUntil(99.95);
    CHECK(s.pacer.stats().invalidates == 1000u);
    CHECK(s.records >= 1);
    CHECK(s.records <= 7);
    int const inWindow = s.records;
    std::printf("  1000 invalidations in 100 ms -> %d records\n", inWindow);
    // the damage that arrived after the last record is not lost: exactly one trailing record, then idle
    s.runUntil(300.0);
    CHECK(s.records == inWindow + 1);
    CHECK(!s.pacer.isDirty());
    int const settled = s.records;
    s.runUntil(2000.0);
    CHECK(s.records == settled); // idle: nothing more
}

static void leadingEdge()
{
    std::printf("pacer: the first invalidation after idle records at once; the second one waits for the interval\n");
    Sim s;
    s.now = 1000.0;
    auto const first = s.pacer.invalidate(s.now);
    CHECK(first == 0.0); // wake now (async update), no 16 ms of added latency
    s.arm(first);
    s.runUntil(1001.0);
    CHECK(s.records == 1);
    s.pending = false;
    s.now = 1002.0;
    auto const second = s.pacer.invalidate(s.now);
    CHECK(second > 0.0); // within the interval: a timer, not an immediate record
    CHECK(second <= FramePacer::Config {}.intervalMs);
    // a third one while the wake is armed does not arm again
    CHECK(s.pacer.invalidate(s.now + 1.0) < 0.0);
}

static void idle()
{
    std::printf("pacer: idle records nothing, whatever the ticks\n");
    Sim s;
    for (int i = 0; i < 20000; ++i) {
        s.now = i * 0.5;
        auto const d = s.pacer.tick(s.now, i % 3 == 0);
        CHECK(d.action == FramePacer::Action::Idle);
        CHECK(d.nextWakeMs < 0.0);
    }
    CHECK(s.pacer.stats().records == 0u);
    CHECK(s.pacer.stats().skippedPending == 0u);
}

static void pendingDefers()
{
    std::printf("pacer: a pending frame defers the record, keeps the damage, and the render thread is nudged once\n");
    Sim s;
    s.renderStuck = true;
    s.pending = true; // the last recorded frame is still waiting
    // pending since long ago is not the case here: stay below the stuck limit
    for (int i = 0; i < 100; ++i) { // 100 invalidations over 100 ms
        s.runUntil(i * 1.0);
        s.invalidate();
    }
    s.runUntil(100.0);
    CHECK(s.records == 0);                    // nothing recorded over the pending frame
    CHECK(s.pacer.isDirty());                 // the damage is still wanted
    CHECK(s.pacer.stats().skippedPending > 0u);
    CHECK(s.nudges == 1); // the first skip of the episode woke the render thread, the next ones do not (an extra GL pass each)
    CHECK(s.wakeAt >= 0.0);                   // and the pacer keeps looking
    // the render thread takes the frame: the next look records exactly once
    s.renderStuck = false;
    s.runUntil(120.0);
    CHECK(s.records == 1);
    CHECK(s.recordsWhilePending == 0);
    CHECK(!s.pacer.isDirty());
    s.runUntil(400.0);
    CHECK(s.records == 1);
}

static void damageNotLost()
{
    std::printf("pacer: damage that arrives after a skipped tick is not lost\n");
    Sim s;
    s.pending = true;
    s.renderStuck = true;
    s.invalidate();
    s.runUntil(20.0); // a tick found the frame pending and waited
    CHECK(s.records == 0);
    CHECK(s.pacer.stats().skippedPending >= 1u);
    s.invalidate(); // new damage after the skipped tick
    s.runUntil(40.0);
    CHECK(s.records == 0);
    s.renderStuck = false;
    s.runUntil(80.0);
    CHECK(s.records == 1);                     // one frame carries both
    CHECK(!s.pacer.isDirty());
    s.runUntil(300.0);
    CHECK(s.records == 1);
}

static void urgent()
{
    std::printf("pacer: an urgent request records at once, over a pending frame and inside the interval\n");
    Sim s;
    s.renderStuck = true; // the render thread holds every frame
    s.invalidate();
    s.runUntil(1.0);
    CHECK(s.records == 1); // a frame just went out and the render thread holds it
    CHECK(s.pending);
    auto const at = s.now;
    s.invalidateNow(); // e.g. the render thread needs a full frame
    CHECK(s.wakeAt >= 0.0 && s.wakeAt <= at); // due now, even though a wake may have been armed
    s.runUntil(at + 1.0);
    CHECK(s.records == 2);
    CHECK(s.recordsWhilePending == 1);
    CHECK(s.pacer.stats().overwrites == 1u);
    CHECK(!s.pacer.isDirty());
    s.runUntil(500.0);
    CHECK(s.records == 2);

    // flushNow: the synchronous path (screenshot, readback) records only when something is dirty
    Sim t;
    CHECK(t.pacer.flushNow(5.0, false).action == FramePacer::Action::Idle);
    t.pacer.invalidate(5.0);
    auto const f = t.pacer.flushNow(6.0, true);
    CHECK(f.action == FramePacer::Action::Record);
    CHECK(f.overwritePending);
    CHECK(!t.pacer.isDirty());
    CHECK(t.pacer.flushNow(7.0, false).action == FramePacer::Action::Idle);
}

static void stuck()
{
    std::printf("pacer: a frame the render thread never takes is replaced once per stuck interval, not every tick\n");
    Sim s;
    s.renderStuck = true; // the render thread cannot take any frame
    s.invalidate();
    s.runUntil(1.0);
    CHECK(s.records == 1);
    for (int i = 0; i < 2000; ++i) { // 2 s of constant invalidations
        s.runUntil(1.0 + i);
        s.invalidate();
    }
    CHECK(s.records >= 2);                    // it did recover
    CHECK(s.records <= 1 + 2000 / 250 + 1);   // at most one replacement per stuck interval
    CHECK(s.pacer.stats().overwrites == static_cast<std::uint64_t>(s.recordsWhilePending));
    CHECK(s.recordsWhilePending == s.records - 1);
}

static void slowRenderThread()
{
    std::printf("pacer: a slow render thread (40 ms swaps) gets at most one frame per swap and never a discarded one\n");
    Sim s;
    s.swapMs = 40.0;
    for (int i = 0; i < 1000; ++i) { // an invalidation every ms for a second
        s.runUntil(i * 1.0);
        s.invalidate();
    }
    s.runUntil(1000.0);
    std::printf("  1000 invalidations in 1 s, 40 ms swaps -> %d records, %d replaced a pending frame\n", s.records, s.recordsWhilePending);
    CHECK(s.taken <= 26);               // one swap per 40 ms
    CHECK(s.records <= s.taken + 1);    // every record is taken, except the one still in flight
    CHECK(s.records >= 20);
    CHECK(s.recordsWhilePending == 0); // no frame was published over a pending one
    CHECK(s.pacer.stats().skippedPending > 0u);

    std::printf("pacer: a fast render thread is held to 60 frames per second\n");
    Sim f;
    for (int i = 0; i < 1000; ++i) {
        f.runUntil(i * 1.0);
        f.invalidate();
    }
    f.runUntil(1000.0);
    std::printf("  1000 invalidations in 1 s, fast render thread -> %d records\n", f.records);
    CHECK(f.records <= 63);
    CHECK(f.records >= 55);
    CHECK(f.recordsWhilePending == 0);
}

static void frameLog()
{
    std::printf("frame log: one line with the counters of the window, then the window starts over\n");
    mcp::FrameLogWindow w;
    mcp::FramePacer::Stats st;
    st.invalidates = 500;
    st.records = 40;
    st.skippedPending = 12;
    w.addRecord(/*damagePixels*/ 600 * 400, /*boundsPixels*/ 1000 * 800, /*recordMs*/ 2.0);
    w.addRecord(/*damagePixels*/ 1000 * 800, /*boundsPixels*/ 1000 * 800, /*recordMs*/ 4.0);
    auto const line = w.takeLine(st, /*renderFrames*/ 38, /*swaps*/ 61);
    CHECK(line.find("inv=500") != std::string::npos);
    CHECK(line.find("rec=40") != std::string::npos);
    CHECK(line.find("skipped=12") != std::string::npos);
    CHECK(line.find("damage=65%") != std::string::npos); // (240000 + 800000) of 2 x 800000 window pixels
    CHECK(line.find("recMs=3.00") != std::string::npos); // (2 + 4) / 2
    CHECK(line.find("renderFrames=38") != std::string::npos);
    CHECK(line.find("swaps=61") != std::string::npos);
    CHECK(line.rfind("[nvg-frame]", 0) == 0);
    CHECK(line.find('\n') == std::string::npos);

    // the next window: nothing recorded, the counters are the delta
    mcp::FramePacer::Stats st2 = st;
    st2.invalidates = 500;
    auto const idleLine = w.takeLine(st2, 38, 61);
    CHECK(idleLine.find("inv=0") != std::string::npos);
    CHECK(idleLine.find("rec=0") != std::string::npos);
    CHECK(idleLine.find("skipped=0") != std::string::npos);
    CHECK(idleLine.find("damage=0%") != std::string::npos);
    CHECK(idleLine.find("recMs=0.00") != std::string::npos);
    CHECK(idleLine.find("renderFrames=0") != std::string::npos);
    CHECK(idleLine.find("swaps=0") != std::string::npos);
}

int main()
{
    burst();
    leadingEdge();
    idle();
    pendingDefers();
    damageNotLost();
    urgent();
    stuck();
    slowRenderThread();
    frameLog();
    if (failures != 0) {
        std::printf("frame_pacer_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("frame_pacer_test: ok\n");
    return 0;
}
