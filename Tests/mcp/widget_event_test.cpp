// Phase 3 of the ai_widget plan (docs/plans/AI-WIDGET-TOOL-PLAN-2026-11-21.md in the MCP repo): the `ai-event` core, capability
// `widget_event`. JUCE-free, Pd-free (Source/Mcp/McpWidgetEvent.h). A widget tells the AI what the artist did with a proposal:
//   ai-event <widget> <rev> <paddr> use|no|back      (a Lua widget: pd.send("ai-event", "list", { word, rev, addr, kind }))
// The bridge's receiver (MCPBridgeWidgetEvent.h) hands the parsed parts to mcp::WidgetEventQueue::offer under the Pd lock, and the
// message-thread pump pops them as `/pd/widget_ask <use|no|back> <canvas> <widget> <rev>`. The same pattern as the ask queue:
//  - fixed words and numbers only: widget ai.<word>, rev a whole number 0..9999999, the address `p` + 1..16 lower-case hex digits (compared
//    as text, never read as a pointer), the word use, no or back; anything else is malformed and counted, never queued;
//  - rate: at most one event per widget, instance (address) and word per kWidgetEventMinGapMs; a refused one is counted and does not
//    restart the clock; two instances of one widget have two budgets;
//  - the queue is fixed (no allocation, no lock): a full queue drops and counts, and does not touch the rate clock (a retry may go through).
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpWidgetEvent.h"

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

using mcp::WidgetEvent;
using mcp::WidgetEventKind;
using mcp::WidgetEventOutcome;
using mcp::WidgetEventQueue;

namespace {

void queuedAndPopped()
{
    WidgetEventQueue q;
    CHECK(q.offer("ai.draw", 7, "p55d0c8a3b2a0", "use", 1000) == WidgetEventOutcome::Queued);
    WidgetEvent e;
    CHECK(q.pop(e));
    CHECK(e.kind == WidgetEventKind::Use);
    CHECK(std::strcmp(e.widget, "ai.draw") == 0);
    CHECK(e.rev == 7);
    CHECK(std::strcmp(e.addr, "p55d0c8a3b2a0") == 0);
    CHECK(!q.pop(e));
    // the three words, in order
    CHECK(q.offer("ai.tap", 3, "p1", "no", 2000) == WidgetEventOutcome::Queued);
    CHECK(q.offer("ai.loop_kick", 4, "p2", "back", 2000) == WidgetEventOutcome::Queued);
    CHECK(q.pop(e) && e.kind == WidgetEventKind::No && std::strcmp(e.widget, "ai.tap") == 0 && e.rev == 3);
    CHECK(q.pop(e) && e.kind == WidgetEventKind::Back && std::strcmp(e.widget, "ai.loop_kick") == 0 && e.rev == 4);
    CHECK(!q.pop(e));
    // the words the pump sends as the event's kind
    CHECK(std::strcmp(mcp::widgetEventKindWord(WidgetEventKind::Use), "use") == 0);
    CHECK(std::strcmp(mcp::widgetEventKindWord(WidgetEventKind::No), "no") == 0);
    CHECK(std::strcmp(mcp::widgetEventKindWord(WidgetEventKind::Back), "back") == 0);
    // rev 0 and the highest rev are numbers like any other
    CHECK(q.offer("ai.draw", 0, "p3", "use", 3000) == WidgetEventOutcome::Queued);
    CHECK(q.offer("ai.draw", 9999999, "p4", "use", 3000) == WidgetEventOutcome::Queued);
}

void malformedIsRefusedAndCounted()
{
    WidgetEventQueue q;
    CHECK(q.offer(nullptr, 1, "p1", "use", 0) == WidgetEventOutcome::Malformed);
    CHECK(q.offer("pd", 1, "p1", "use", 0) == WidgetEventOutcome::Malformed);       // no Pd or system name
    CHECK(q.offer("draw", 1, "p1", "use", 0) == WidgetEventOutcome::Malformed);
    CHECK(q.offer("ai.Draw", 1, "p1", "use", 0) == WidgetEventOutcome::Malformed);
    CHECK(q.offer("ai.draw-in", 1, "p1", "use", 0) == WidgetEventOutcome::Malformed);
    CHECK(q.offer("ai.draw", -1, "p1", "use", 0) == WidgetEventOutcome::Malformed);
    CHECK(q.offer("ai.draw", 1.5, "p1", "use", 0) == WidgetEventOutcome::Malformed);
    CHECK(q.offer("ai.draw", 10000000, "p1", "use", 0) == WidgetEventOutcome::Malformed);
    CHECK(q.offer("ai.draw", 0.0 / 0.0, "p1", "use", 0) == WidgetEventOutcome::Malformed);
    CHECK(q.offer("ai.draw", 1, nullptr, "use", 0) == WidgetEventOutcome::BadAddress);
    CHECK(q.offer("ai.draw", 1, "p1", nullptr, 0) == WidgetEventOutcome::UnknownWord);
    WidgetEvent e;
    CHECK(!q.pop(e));
    auto const c = q.takeCounts();
    CHECK(c.malformed == 11 && c.rateLimited == 0 && c.full == 0); // the unknown words and the bad addresses count with the malformed
    CHECK(q.takeCounts().malformed == 0);                          // taken = reset
}

void anUnknownWordIsRefused()
{
    WidgetEventQueue q;
    for (char const* word : { "", "yes", "keep", "kept", "USE", "Use", "use ", " use", "nope", "backs", "undo", "ask", "hello", "state", "bag", "refused" })
        CHECK(q.offer("ai.draw", 1, "p1", word, 0) == WidgetEventOutcome::UnknownWord);
    WidgetEvent e;
    CHECK(!q.pop(e));
    CHECK(q.takeCounts().malformed == 16);
}

void aBadAddressTextIsRefused()
{
    WidgetEventQueue q;
    // the address is text only: nothing is parsed into a pointer, so a forged or stale one is only a word that is compared
    for (char const* addr : { "", "p", "55d0c8", "0x55d0c8", "P55d0c8", "p55D0C8", "p55g0", "p-1", "pf0000000000000000", "p55d0 use", "p55d0;", "p 5" })
        CHECK(q.offer("ai.draw", 1, addr, "use", 0) == WidgetEventOutcome::BadAddress);
    WidgetEvent e;
    CHECK(!q.pop(e));
    CHECK(q.takeCounts().malformed == 12);
    // a widget that sends no address (an old copy) is not an event: there is nothing to tell which instance acted
    CHECK(q.offer("ai.draw", 1, nullptr, "use", 0) == WidgetEventOutcome::BadAddress);
}

void rateLimit()
{
    WidgetEventQueue q;
    int64_t const gap = mcp::kWidgetEventMinGapMs;
    CHECK(q.offer("ai.draw", 1, "p1", "use", 10000) == WidgetEventOutcome::Queued);
    CHECK(q.offer("ai.draw", 2, "p1", "use", 10000 + gap / 2) == WidgetEventOutcome::RateLimited);     // a double click is one
    CHECK(q.offer("ai.draw", 2, "p1", "use", 10000 + gap - 1) == WidgetEventOutcome::RateLimited);
    // another word of the same instance has its own budget (use, then back)
    CHECK(q.offer("ai.draw", 2, "p1", "back", 10000 + 10) == WidgetEventOutcome::Queued);
    // another instance of the same widget has its own budget (two ai.draw on the canvas)
    CHECK(q.offer("ai.draw", 1, "p2", "use", 10000 + 20) == WidgetEventOutcome::Queued);
    // another widget has its own
    CHECK(q.offer("ai.tap", 1, "p1", "use", 10000 + 30) == WidgetEventOutcome::Queued);
    // after the gap, counted from the last ACCEPTED event (a refused one does not restart the clock)
    CHECK(q.offer("ai.draw", 3, "p1", "use", 10000 + gap) == WidgetEventOutcome::Queued);
    auto const c = q.takeCounts();
    CHECK(c.rateLimited == 2 && c.malformed == 0 && c.full == 0);
    // a clock that went backwards (another time source) never locks a widget out for long
    CHECK(q.offer("ai.draw", 4, "p1", "use", 100) == WidgetEventOutcome::Queued);
}

void theRateTableIsBounded()
{
    WidgetEventQueue q;
    // more keys than the rate table holds: the entry used longest ago is reused, nothing allocates, no event of a new key is lost
    char addr[24];
    for (int i = 0; i < mcp::kWidgetEventRateSlots + 5; ++i) {
        std::snprintf(addr, sizeof(addr), "p%x", 0x100 + i);
        CHECK(q.offer("ai.draw", 1, addr, "use", 5000 + i) == WidgetEventOutcome::Queued);
        WidgetEvent e;
        CHECK(q.pop(e));
    }
}

void aFullQueueDropsAndCounts()
{
    WidgetEventQueue q;
    char addr[24];
    // 32 slots: fill them with distinct instances (each has its own budget)
    for (int i = 0; i < mcp::kWidgetEventQueueSlots; ++i) {
        std::snprintf(addr, sizeof(addr), "p%x", 0x200 + i);
        CHECK(q.offer("ai.draw", 1, addr, "use", 1000) == WidgetEventOutcome::Queued);
    }
    CHECK(q.offer("ai.draw", 1, "p999", "use", 1000) == WidgetEventOutcome::Full);
    CHECK(q.offer("ai.tap", 1, "p999", "no", 1000) == WidgetEventOutcome::Full);
    auto c = q.takeCounts();
    CHECK(c.full == 2 && c.malformed == 0 && c.rateLimited == 0);
    // the first one pops (oldest first), which makes room: a retry of the same key goes through (a full queue did not use its budget)
    WidgetEvent e;
    CHECK(q.pop(e) && std::strcmp(e.addr, "p200") == 0);
    CHECK(q.offer("ai.draw", 1, "p999", "use", 1000) == WidgetEventOutcome::Queued);
    // all of them come out in order, then the retry
    int popped = 0;
    while (q.pop(e)) ++popped;
    CHECK(popped == mcp::kWidgetEventQueueSlots); // 31 left of the first fill, plus the retry
}

void theQueueWrapsAround()
{
    // more events than slots over time, one at a time: the ring index wraps without losing an event
    WidgetEventQueue q;
    char addr[24];
    for (int i = 0; i < 3 * mcp::kWidgetEventQueueSlots; ++i) {
        std::snprintf(addr, sizeof(addr), "p%x", 0x300 + i);
        CHECK(q.offer("ai.tap", i % 5000, addr, i % 2 ? "no" : "use", 100000 + i) == WidgetEventOutcome::Queued);
        WidgetEvent e;
        CHECK(q.pop(e) && e.rev == i % 5000 && std::strcmp(e.addr, addr) == 0);
    }
}

void aLongNameNeverOverrunsTheSlot()
{
    WidgetEventQueue q;
    std::string const longName = "ai." + std::string(80, 'x');
    CHECK(q.offer(longName.c_str(), 1, "p1", "use", 0) == WidgetEventOutcome::Malformed); // the name check bounds it before any copy
    std::string const longAddr = "p" + std::string(60, 'a');
    CHECK(q.offer("ai.draw", 1, longAddr.c_str(), "use", 0) == WidgetEventOutcome::BadAddress);
    WidgetEvent e;
    CHECK(!q.pop(e));
}

} // namespace

int main()
{
    queuedAndPopped();
    malformedIsRefusedAndCounted();
    anUnknownWordIsRefused();
    aBadAddressTextIsRefused();
    rateLimit();
    theRateTableIsBounded();
    aFullQueueDropsAndCounts();
    theQueueWrapsAround();
    aLongNameNeverOverrunsTheSlot();
    if (failures != 0) {
        std::printf("widget_event_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("widget_event_test: ok\n");
    return 0;
}
