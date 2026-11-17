// Thread phase 2 (docs/plans/PILL-LANES-INBOX-PLAN-2026-11-16.md section 3): live widget history, the core.
// JUCE-free, Pd-free (Source/Mcp/McpWidgetState.h). While the bridge serves, a native receiver is bound to the state name
// of every ai.* widget (`ai-tap-state`, `ai-draw-state`, `ai-loop_<name>-state`, the family's); the widget's state frames
// end with `end <rev>`. The receiver hands that `end` to WidgetStateBook::noteEnd under the Pd lock; the bridge's
// message-thread pump pops what has been quiet for 1 s as one /pd/widget_ask `state` event.
//  - names: the fixed widgets, plus named loopers (ai.loop_<name>) up to a cap; nothing else gets a slot;
//  - nothing is recorded until the server switches the watch on (an old server never sees an event);
//  - one event after 1 s of quiet, whatever the number of frames; the same rev again is not an event;
//  - at most one event per widget per 2 s (a changed state waits for the gap, it is not lost);
//  - the book is fixed (no allocation, no lock): the producer side is atomics only.
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpWidgetState.h"

#include <cstdio>
#include <cstring>
#include <string>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

using mcp::WidgetStateBook;
using mcp::WidgetStateEvent;

namespace {

void names()
{
    CHECK(mcp::widgetStateNamedLooperOk("ai.loop_kick"));
    CHECK(mcp::widgetStateNamedLooperOk("ai.loop_0"));
    CHECK(mcp::widgetStateNamedLooperOk("ai.loop_a1_b2"));
    CHECK(mcp::widgetStateNamedLooperOk("ai.loop_abcdefghijkl"));  // 12 characters
    CHECK(!mcp::widgetStateNamedLooperOk("ai.loop_abcdefghijklm")); // 13
    CHECK(!mcp::widgetStateNamedLooperOk("ai.loop_"));
    CHECK(!mcp::widgetStateNamedLooperOk("ai.loop"));    // the legacy looper is a fixed name, not a named one
    CHECK(!mcp::widgetStateNamedLooperOk("ai.loop_Kick"));
    CHECK(!mcp::widgetStateNamedLooperOk("ai.loop_ki-ck"));
    CHECK(!mcp::widgetStateNamedLooperOk("ai.tap"));
    CHECK(!mcp::widgetStateNamedLooperOk(nullptr));

    char out[64];
    CHECK(mcp::widgetStateSymbolName("ai.tap", out, sizeof out) && std::strcmp(out, "ai-tap-state") == 0);
    CHECK(mcp::widgetStateSymbolName("ai.loop_kick", out, sizeof out) && std::strcmp(out, "ai-loop_kick-state") == 0);
    CHECK(mcp::widgetStateSymbolName("ai.loop", out, sizeof out) && std::strcmp(out, "ai-loop-state") == 0);
    CHECK(!mcp::widgetStateSymbolName("pd", out, sizeof out));
    CHECK(!mcp::widgetStateSymbolName("ai.tap", out, 5)); // buffer too small
    CHECK(!mcp::widgetStateSymbolName(nullptr, out, sizeof out));

    // The fixed list is what the server asks (ASKABLE_WIDGETS in src/services/widget-ask.ts): 11 widgets, all valid names.
    int n = 0;
    for (char const* w : mcp::kWidgetStateFixed) {
        CHECK(mcp::widgetAskNameOk(w));
        ++n;
    }
    CHECK(n == mcp::kWidgetStateFixedCount && n == 11);
}

void claiming()
{
    WidgetStateBook b;
    int const tap = b.claim("ai.tap");
    CHECK(tap >= 0);
    CHECK(b.claim("ai.tap") == tap); // the same widget keeps its slot
    int const draw = b.claim("ai.draw");
    CHECK(draw >= 0 && draw != tap);
    CHECK(b.claim("ai.nothing") < 0);   // not a watched widget
    CHECK(b.claim("pd") < 0);
    CHECK(b.claim(nullptr) < 0);
    CHECK(b.claim("ai.loop_kick") >= 0); // a named looper
    CHECK(b.claim("ai.loop_0") >= 0);     // fixed

    // Named loopers are capped; the one idle longest makes room (its slot is reused), the fixed names always find a slot.
    WidgetStateBook c;
    c.setWatching(true);
    int slotOf[mcp::kWidgetStateNamedMax] = {};
    for (int i = 0; i < mcp::kWidgetStateNamedMax; ++i) {
        std::string const w = "ai.loop_n" + std::to_string(i);
        slotOf[i] = c.claim(w.c_str(), i * 100);
        CHECK(slotOf[i] >= 0);
    }
    c.noteEnd(slotOf[0], 1, 3000); // n0 is the oldest claim but was heard from last: n1 is idle longest now
    bool evicted = false;
    int const over = c.claim("ai.loop_over", 5000, &evicted);
    CHECK(evicted && over == slotOf[1]);
    CHECK(c.nameAt(over) && std::strcmp(c.nameAt(over), "ai.loop_over") == 0);
    CHECK(c.claim("ai.loop_over", 6000) == over); // held now: its own slot, nothing evicted
    CHECK(c.takeCounts().namedEvicted == 1);
    CHECK(c.claim("ai.loop_n0") == slotOf[0]);    // still held (it was not the idlest)
    evicted = false;
    CHECK(c.claim("ai.loop_n1", 7000, &evicted) >= 0 && evicted); // n1 was pushed out: coming back pushes the idlest out in turn
    for (char const* w : mcp::kWidgetStateFixed)
        CHECK(c.claim(w) >= 0);
    CHECK(mcp::kWidgetStateSlots >= mcp::kWidgetStateFixedCount + mcp::kWidgetStateNamedMax);
    // A fixed name is never the one pushed out: filling every named slot again leaves them held.
    for (char const* w : mcp::kWidgetStateFixed)
        CHECK(c.nameAt(c.claim(w)) != nullptr);

    // An evicted slot carries nothing of the old looper: no pending event, no remembered rev.
    WidgetStateBook d;
    d.setWatching(true);
    for (int i = 0; i < mcp::kWidgetStateNamedMax; ++i) {
        std::string const w = "ai.loop_m" + std::to_string(i);
        int const sl = d.claim(w.c_str(), i);
        d.noteEnd(sl, 7, 100 + i);
    }
    bool ev = false;
    int const taken = d.claim("ai.loop_fresh", 10'000, &ev);
    CHECK(ev && taken >= 0);
    WidgetStateEvent e;
    int due = 0;
    while (d.popDue(e, 20'000)) {
        CHECK(std::strcmp(e.widget, "ai.loop_fresh") != 0 && std::strcmp(e.widget, "ai.loop_m0") != 0); // fresh has no end; m0 was the evicted one
        ++due;
    }
    CHECK(due == mcp::kWidgetStateNamedMax - 1);
}

void offUntilWatched()
{
    WidgetStateBook b;
    int const s = b.claim("ai.tap");
    CHECK(!b.watching());
    CHECK(b.noteEnd(s, 4, 1000) == mcp::WidgetStateOutcome::Off);
    WidgetStateEvent e;
    CHECK(!b.popDue(e, 5000));
    b.setWatching(true);
    CHECK(b.watching());
    CHECK(b.noteEnd(s, 4, 6000) == mcp::WidgetStateOutcome::Noted);
    CHECK(b.popDue(e, 7100) && std::strcmp(e.widget, "ai.tap") == 0 && e.rev == 4);
    b.setWatching(false);
    CHECK(b.noteEnd(s, 5, 8000) == mcp::WidgetStateOutcome::Off);
    CHECK(!b.popDue(e, 20000));
}

void quietThenOneEvent()
{
    WidgetStateBook b;
    b.setWatching(true);
    int const s = b.claim("ai.tap");
    WidgetStateEvent e;
    // 20 frames-worth of ends over 600 ms: nothing while it is busy, ONE event 1 s after the last.
    for (int i = 0; i < 20; ++i) CHECK(b.noteEnd(s, 7, 1000 + i * 30) == mcp::WidgetStateOutcome::Noted);
    CHECK(!b.popDue(e, 1000 + 19 * 30 + 999)); // 999 ms after the last end
    CHECK(b.popDue(e, 1000 + 19 * 30 + 1000));
    CHECK(std::strcmp(e.widget, "ai.tap") == 0 && e.rev == 7);
    CHECK(!b.popDue(e, 9000)); // taken: nothing again
}

void unchangedRevIsNoEvent()
{
    WidgetStateBook b;
    b.setWatching(true);
    int const s = b.claim("ai.tap");
    WidgetStateEvent e;
    b.noteEnd(s, 3, 1000);
    CHECK(b.popDue(e, 2100) && e.rev == 3);
    // A dump (or a ghost) answers with the same rev: no event, and it is not left waiting.
    b.noteEnd(s, 3, 10000);
    CHECK(!b.popDue(e, 11100));
    CHECK(!b.popDue(e, 30000));
    // A new rev is one.
    b.noteEnd(s, 4, 31000);
    CHECK(b.popDue(e, 32100) && e.rev == 4);
}

void rateLimitKeepsTheChange()
{
    WidgetStateBook b;
    b.setWatching(true);
    int const s = b.claim("ai.knob");
    WidgetStateEvent e;
    b.noteEnd(s, 1, 1000);
    CHECK(b.popDue(e, 2100)); // forwarded at 2100
    b.noteEnd(s, 2, 2200);
    CHECK(!b.popDue(e, 3300)); // quiet for 1 s, but only 1200 ms after the last event: waits
    CHECK(!b.popDue(e, 4099));
    CHECK(b.popDue(e, 4100) && e.rev == 2); // 2 s after the last event: it goes
}

void slotsAreIndependent()
{
    WidgetStateBook b;
    b.setWatching(true);
    int const tap = b.claim("ai.tap");
    int const draw = b.claim("ai.draw");
    WidgetStateEvent e;
    b.noteEnd(tap, 1, 1000);
    b.noteEnd(draw, 9, 1500);
    CHECK(b.popDue(e, 2100) && std::strcmp(e.widget, "ai.tap") == 0 && e.rev == 1);
    CHECK(!b.popDue(e, 2100)); // draw has been quiet for 600 ms only
    CHECK(b.popDue(e, 2500) && std::strcmp(e.widget, "ai.draw") == 0 && e.rev == 9);
}

void malformed()
{
    WidgetStateBook b;
    b.setWatching(true);
    int const s = b.claim("ai.tap");
    CHECK(b.noteEnd(s, -1, 1000) == mcp::WidgetStateOutcome::Malformed);
    CHECK(b.noteEnd(s, 1.5, 1000) == mcp::WidgetStateOutcome::Malformed);
    CHECK(b.noteEnd(s, 10000000, 1000) == mcp::WidgetStateOutcome::Malformed);
    CHECK(b.noteEnd(s, 0.0 / 0.0, 1000) == mcp::WidgetStateOutcome::Malformed);
    CHECK(b.noteEnd(-1, 2, 1000) == mcp::WidgetStateOutcome::Malformed);
    CHECK(b.noteEnd(mcp::kWidgetStateSlots, 2, 1000) == mcp::WidgetStateOutcome::Malformed);
    b.noteMalformed();
    WidgetStateEvent e;
    CHECK(!b.popDue(e, 9000)); // none of them left an event
    auto const c = b.takeCounts();
    CHECK(c.malformed == 7);
    CHECK(b.takeCounts().malformed == 0); // taken
}

void rewindKeepsNamesDropsState()
{
    WidgetStateBook b;
    b.setWatching(true);
    int const s = b.claim("ai.tap");
    int const k = b.claim("ai.loop_kick", 5);
    b.noteEnd(s, 3, 1000);
    b.noteMalformed();
    b.rewind();
    WidgetStateEvent e;
    CHECK(!b.popDue(e, 9000));
    CHECK(!b.watching());
    CHECK(b.takeCounts().malformed == 0);
    // the names stay held: the bridge binds a receiver for each again after a stop
    CHECK(b.nameAt(s) && std::strcmp(b.nameAt(s), "ai.tap") == 0);
    CHECK(b.nameAt(k) && std::strcmp(b.nameAt(k), "ai.loop_kick") == 0);
    CHECK(b.nameAt(mcp::kWidgetStateSlots) == nullptr && b.nameAt(-1) == nullptr);
    CHECK(b.claim("ai.tap") == s && b.claim("ai.loop_kick") == k);
    b.setWatching(true);
    b.noteEnd(s, 3, 10000); // the old forwarded rev is forgotten too
    CHECK(b.popDue(e, 11100) && e.rev == 3);
}

// Review 2026-11-17: a hello burst at load (every fixed widget and every named looper the history can hold, in one pump tick) must fit the
// ask queue: a hello that finds it full is dropped, and a named looper dropped there is never watched (it says hello once).
void helloBurstFitsTheAskQueue()
{
    mcp::WidgetAskQueue q;
    char name[mcp::kWidgetAskNameLen + 1];
    int queued = 0;
    for (int i = 0; i < mcp::kWidgetStateFixedCount + mcp::kWidgetStateNamedMax; ++i) {
        std::snprintf(name, sizeof name, "ai.loop_h%d", i);
        if (q.offer(mcp::WidgetAskKind::Hello, name, 0, 1000) == mcp::WidgetAskOutcome::Queued) ++queued;
    }
    CHECK(queued == mcp::kWidgetStateFixedCount + mcp::kWidgetStateNamedMax);
    CHECK(q.takeCounts().full == 0);
    CHECK(mcp::kWidgetAskQueueSlots >= 32);
}

} // namespace

int main()
{
    helloBurstFitsTheAskQueue();
    names();
    claiming();
    offUntilWatched();
    quietThenOneEvent();
    unchangedRevIsNoEvent();
    rateLimitKeepsTheChange();
    slotsAreIndependent();
    malformed();
    rewindKeepsNamesDropsState();
    if (failures) {
        std::printf("widget_state_test: %d FAILED\n", failures);
        return 1;
    }
    std::printf("widget_state_test: OK\n");
    return 0;
}
