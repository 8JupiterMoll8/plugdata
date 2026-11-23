// Unit tests for the cached "wants rounded corners" flag (lag fix, Fable's diagnosis 2026-11-23, fix #1), JUCE-free
// (Source/Mcp/McpRefreshedFlag.h).
//
// PluginEditor::paint asked wantsRoundedCorners() on EVERY frame; that read PlugDataWindow::isMaximised() and
// ProjectInfo::canUseSemiTransparentWindows(), both synchronous X round trips on X11 (perf 2026-11-23: about 9 % of the
// message thread while dragging, 24 % idle, and wall time the profiler does not even show). The flag is now computed on
// events (resized, parentHierarchyChanged, a window state change) and by a 2 s refresh, and paint only reads it.
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpRefreshedFlag.h"

#include <cstdint>
#include <cstdio>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

// The owner: paint reads, events and the timer compute (and count how often the expensive question is asked).
struct Owner {
    mcp::RefreshedFlag flag { true };
    int xRoundTrips = 0;
    bool answer = true; // what the X server would say

    bool ask()
    {
        ++xRoundTrips;
        return answer;
    }
    bool paint() const { return flag.get(); }                       // never asks
    void event(std::uint32_t nowMs) { flag.set(ask(), nowMs); }     // resized, parentHierarchyChanged, a state change
    void timer(std::uint32_t nowMs)                                 // the editor's 90 Hz timer
    {
        if (flag.dueForRefresh(nowMs))
            flag.set(ask(), nowMs);
    }
};

static void paintNeverAsks()
{
    std::printf("rounded corners: 100 paints (and a 90 Hz timer for a second) ask the X server nothing\n");
    Owner o;
    o.event(1000); // the window attached
    CHECK(o.xRoundTrips == 1);
    for (int i = 0; i < 100; ++i) {
        CHECK(o.paint());
        o.timer(1000 + static_cast<std::uint32_t>(i) * 11); // 90 Hz
    }
    CHECK(o.xRoundTrips == 1);
}

static void refreshesEveryTwoSeconds()
{
    std::printf("rounded corners: the timer re-asks once per 2 s, and a changed answer reaches the next paint\n");
    Owner o;
    o.event(0);
    CHECK(o.xRoundTrips == 1);
    o.answer = false; // e.g. the compositor went away
    for (std::uint32_t t = 0; t < 1999; t += 11)
        o.timer(t);
    CHECK(o.xRoundTrips == 1);
    CHECK(o.paint()); // not yet
    o.timer(2000);
    CHECK(o.xRoundTrips == 2);
    CHECK(!o.paint());
    for (std::uint32_t t = 2000; t < 3999; t += 11)
        o.timer(t);
    CHECK(o.xRoundTrips == 2);
    o.timer(4000);
    CHECK(o.xRoundTrips == 3);
}

static void eventsRecomputeAtOnce()
{
    std::printf("rounded corners: a resize (maximise / restore) recomputes at once and restarts the 2 s\n");
    Owner o;
    o.event(0);
    o.answer = false;
    o.event(500); // resized: maximised
    CHECK(!o.paint());
    CHECK(o.xRoundTrips == 2);
    o.timer(2400); // 1900 ms after the last compute: not due
    CHECK(o.xRoundTrips == 2);
    o.timer(2500);
    CHECK(o.xRoundTrips == 3);
}

static void reportsChange()
{
    std::printf("rounded corners: set() says whether the value changed (the caller repaints and tells the render thread)\n");
    mcp::RefreshedFlag f(true);
    CHECK(!f.set(true, 10));
    CHECK(f.set(false, 20));
    CHECK(!f.set(false, 30));
    CHECK(f.set(true, 40));
    CHECK(f.get());
}

static void clockWrap()
{
    std::printf("rounded corners: the 32-bit millisecond counter wrapping around does not stall or storm the refresh\n");
    mcp::RefreshedFlag f(true);
    f.set(true, 0xFFFFFF00u);
    CHECK(!f.dueForRefresh(0xFFFFFF00u + 100u));
    CHECK(!f.dueForRefresh(0xFFFFFF00u + 1999u)); // wrapped to a small number, 1999 ms later
    CHECK(f.dueForRefresh(0xFFFFFF00u + 2000u));
    // a flag never computed yet is due at once
    mcp::RefreshedFlag fresh(true);
    CHECK(fresh.dueForRefresh(0));
}

int main()
{
    paintNeverAsks();
    refreshesEveryTwoSeconds();
    eventsRecomputeAtOnce();
    reportsChange();
    clockWrap();
    if (failures != 0) {
        std::printf("refreshed_flag_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("refreshed_flag_test: ok\n");
    return 0;
}
