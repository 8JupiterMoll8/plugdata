// Phase 3 of the ai_widget plan (docs/plans/AI-WIDGET-TOOL-PLAN-2026-11-21.md in the MCP repo): the `ai-event` core, capability
// `widget_event`. JUCE-free, Pd-free (Source/Mcp/McpWidgetEvent.h). A widget tells the AI what the artist did with a proposal:
//   ai-event <widget> <rev> <paddr> use|no|back      (a Lua widget: pd.send("ai-event", "list", { word, rev, addr, kind }))
// The bridge's receiver (MCPBridgeWidgetEvent.h) hands the parsed parts to mcp::WidgetEventQueue::offer under the Pd lock, and the
// message-thread pump pops them as `/pd/widget_ask <use|no|back> <canvas> <widget> <rev>`. The same pattern as the ask queue:
//  - fixed words and numbers only: widget ai.<word>, rev a whole number 0..9999999, the address `p` + 1..16 lower-case hex digits (compared
//    as text, never read as a pointer), the word use, no or back; anything else is malformed and counted, never queued;
//  - rate: at most one event per widget, instance (address) and word per kWidgetEventMinGapMs; a refused one is counted and does not
//    restart the clock; two instances of one widget have two budgets;
//  - the queue is fixed (no allocation, no lock): a full queue drops and counts, and does not touch the rate clock (a retry may go through);
//  - a GLOBAL budget over all keys (kWidgetEventGlobalPerWindow events in any kWidgetEventGlobalWindowMs): a patch that rotates through
//    more addresses than the 16 rate slots hold (each key evicted before it is asked again) is still cut to the budget;
//  - the console: an event dropped by a rate limit (the per-key gap or the global budget) prints NOTHING (a double click is normal);
//    malformed events and a full queue are logged at most once per kWidgetEventLogGapMs, with the count of all that came meanwhile.
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpWidgetEvent.h"

#include <cstdio>
#include <cstring>
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
    CHECK(std::strcmp(mcp::widgetEventKindWord(WidgetEventKind::Another), "another") == 0);
    // another: the proposal row's third answer, the same bounds as the other words
    CHECK(q.offer("ai.seq", 5, "p5", "another", 4000) == WidgetEventOutcome::Queued);
    CHECK(q.pop(e) && e.kind == WidgetEventKind::Another && std::strcmp(e.widget, "ai.seq") == 0 && e.rev == 5);
    CHECK(std::strcmp(e.addr, "p5") == 0); // the pump forwards it as the 5th argument: two boxes of one widget stay two requests
    // a bass grid and a glitch grid at one rev: two events, told apart by the address (the rate key is per instance)
    CHECK(q.offer("ai.seq", 9, "p6a", "another", 5000) == WidgetEventOutcome::Queued);
    CHECK(q.offer("ai.seq", 9, "p6b", "another", 5000) == WidgetEventOutcome::Queued);
    CHECK(q.pop(e) && std::strcmp(e.addr, "p6a") == 0);
    CHECK(q.pop(e) && std::strcmp(e.addr, "p6b") == 0);
    CHECK(!q.pop(e));
    CHECK(q.offer("ai.seq", 5, "p5", "Another", 4000) == WidgetEventOutcome::UnknownWord);
    CHECK(q.offer("ai.seq", 5, "p5", "another ", 4000) == WidgetEventOutcome::UnknownWord);
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
        CHECK(q.offer("ai.draw", 1, addr, "use", 5000 + 200 * i) == WidgetEventOutcome::Queued); // 200 ms apart: inside the global budget
        WidgetEvent e;
        CHECK(q.pop(e));
    }
}

void aFullQueueDropsAndCounts()
{
    WidgetEventQueue q;
    char addr[24];
    // 32 slots: fill them with distinct instances (each has its own budget), 150 ms apart (7 a second: inside the global budget; the
    // pump would have to stand still for 4 s to let the queue fill)
    std::int64_t t = 1000;
    for (int i = 0; i < mcp::kWidgetEventQueueSlots; ++i, t += 150) {
        std::snprintf(addr, sizeof(addr), "p%x", 0x200 + i);
        CHECK(q.offer("ai.draw", 1, addr, "use", t) == WidgetEventOutcome::Queued);
    }
    CHECK(q.offer("ai.draw", 1, "p999", "use", t) == WidgetEventOutcome::Full);
    CHECK(q.offer("ai.tap", 1, "p999", "no", t) == WidgetEventOutcome::Full);
    auto c = q.takeCounts();
    CHECK(c.full == 2 && c.malformed == 0 && c.rateLimited == 0 && c.budgetLimited == 0);
    // the first one pops (oldest first), which makes room: a retry of the same key goes through (a full queue used neither its rate
    // clock nor the global budget)
    WidgetEvent e;
    CHECK(q.pop(e) && std::strcmp(e.addr, "p200") == 0);
    CHECK(q.offer("ai.draw", 1, "p999", "use", t) == WidgetEventOutcome::Queued);
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
        CHECK(q.offer("ai.tap", i % 5000, addr, i % 2 ? "no" : "use", 100000 + 150 * i) == WidgetEventOutcome::Queued);
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

// A patch that rotates through 17+ addresses: the 16 rate slots evict the key that is asked next, so the per-key gap stops binding. The global
// budget is the bound: at most kWidgetEventGlobalPerWindow events in any kWidgetEventGlobalWindowMs, 1 ms ticks or not. `keys` = how many
// different instances the patch rotates through (each tick another one); the pump keeps up (pops), so only the limits can be what stops them.
void floodStaysInTheGlobalBudget(int keys)
{
    WidgetEventQueue q;
    char addr[24];
    std::vector<std::int64_t> accepted;
    int refused = 0;
    for (int i = 0; i < 3000; ++i) { // 3 s at 1 kHz = 1000 events a second offered
        std::snprintf(addr, sizeof(addr), "p%x", 0x400 + i % keys);
        std::int64_t const now = 50000 + i;
        auto const r = q.offer("ai.draw", 1, addr, "use", now);
        if (r == WidgetEventOutcome::Queued) accepted.push_back(now);
        else {
            CHECK(r == WidgetEventOutcome::BudgetLimited || r == WidgetEventOutcome::RateLimited);
            ++refused;
        }
        WidgetEvent e;
        while (q.pop(e)) {}
    }
    CHECK(accepted.size() >= 8 && accepted.size() <= 8 * 3); // the budget lets 8 a second through: not 1000 a second, and not none
    // in EVERY window of 1000 ms at most 8 were forwarded (a sliding window, not a counter that a boundary resets)
    for (std::size_t i = 0; i < accepted.size(); ++i) {
        int inWindow = 0;
        for (std::size_t k = i; k < accepted.size() && accepted[k] - accepted[i] < mcp::kWidgetEventGlobalWindowMs; ++k)
            ++inWindow;
        CHECK(inWindow <= mcp::kWidgetEventGlobalPerWindow);
    }
    auto const c = q.takeCounts();
    CHECK(c.budgetLimited + c.rateLimited == refused && c.budgetLimited > 0 && c.malformed == 0 && c.full == 0);
}

void moreKeysThanRateSlotsStayInTheGlobalBudget()
{
    CHECK(mcp::kWidgetEventGlobalPerWindow == 8 && mcp::kWidgetEventGlobalWindowMs == 1000);
    floodStaysInTheGlobalBudget(mcp::kWidgetEventRateSlots + 1); // 17 rotating instances: each evicted just before it is asked again
    floodStaysInTheGlobalBudget(mcp::kWidgetEventRateSlots + 4);
    floodStaysInTheGlobalBudget(3000);                           // every event another instance: the per-key gap never applies at all
}

void theBudgetIsGlobalAndRefillsWithTime()
{
    WidgetEventQueue q;
    char addr[24];
    WidgetEvent e;
    // eight different widgets and instances in one burst use the whole budget...
    for (int i = 0; i < mcp::kWidgetEventGlobalPerWindow; ++i) {
        std::snprintf(addr, sizeof(addr), "p%x", 0x500 + i);
        CHECK(q.offer(i % 2 ? "ai.tap" : "ai.draw", 1, addr, i % 3 ? "use" : "no", 70000 + i) == WidgetEventOutcome::Queued);
    }
    // ...the ninth, of a new key, is dropped (and counted), and its key's own clock is not started
    CHECK(q.offer("ai.loop_kick", 1, "p600", "back", 70100) == WidgetEventOutcome::BudgetLimited);
    CHECK(q.takeCounts().budgetLimited == 1);
    // one window after the FIRST one the budget has room again, for that key too (a dropped event did not start its rate clock)
    CHECK(q.offer("ai.loop_kick", 1, "p600", "back", 70000 + mcp::kWidgetEventGlobalWindowMs) == WidgetEventOutcome::Queued);
    // the per-key gap still applies first: the same key again at once is a plain rate limit, not the budget
    CHECK(q.offer("ai.loop_kick", 1, "p600", "back", 70000 + mcp::kWidgetEventGlobalWindowMs + 10) == WidgetEventOutcome::RateLimited);
    auto const c = q.takeCounts();
    CHECK(c.rateLimited == 1 && c.budgetLimited == 0);
    while (q.pop(e)) {}
    // a clock that went backwards does not freeze the budget
    CHECK(q.offer("ai.draw", 1, "p700", "use", 10) == WidgetEventOutcome::Queued);
}

// What reaches the console. A double click (the per-key gap) and a flood (the global budget) are normal and print nothing; a malformed
// event and a full queue are logged, at most once per kWidgetEventLogGapMs, carrying the count of all that came in between.
void aRateLimitedEventPrintsNothing()
{
    WidgetEventQueue q;
    mcp::WidgetEventLogGate gate;
    CHECK(q.offer("ai.draw", 1, "p1", "use", 10000) == WidgetEventOutcome::Queued);
    CHECK(q.offer("ai.draw", 1, "p1", "use", 10040) == WidgetEventOutcome::RateLimited); // the double click
    mcp::WidgetEventLogCounts line;
    CHECK(!gate.take(q.takeCounts(), 10050, line));
    // a flood to the global budget is silent too, however long it lasts
    char addr[24];
    for (int i = 0; i < 100; ++i) {
        std::snprintf(addr, sizeof(addr), "p%x", 0x800 + i);
        (void)q.offer("ai.draw", 1, addr, "use", 10100 + i);
    }
    auto const c = q.takeCounts();
    CHECK(c.budgetLimited > 0);
    CHECK(!gate.take(c, 10300, line));
    CHECK(!gate.take(mcp::WidgetEventCounts {}, 99999, line)); // nothing waiting, nothing printed, whenever it is asked
}

void malformedLoggingIsThrottledWithACount()
{
    mcp::WidgetEventLogGate gate;
    mcp::WidgetEventLogCounts line;
    std::int64_t const gap = mcp::kWidgetEventLogGapMs;
    CHECK(gap == 5000);
    mcp::WidgetEventCounts bad;
    bad.malformed = 3;
    // the first one is said at once
    CHECK(gate.take(bad, 100000, line) && line.malformed == 3 && line.full == 0);
    // more come every 50 ms (the pump tick): nothing is said until the gap is over, and nothing is lost: they are summed
    for (int tick = 1; tick < 100; ++tick) {
        mcp::WidgetEventCounts more;
        more.malformed = 2;
        more.full = tick == 50 ? 4 : 0;
        CHECK(!gate.take(more, 100000 + 50 * tick, line)); // up to 4950 ms after the line
    }
    mcp::WidgetEventCounts last;
    last.malformed = 1;
    CHECK(gate.take(last, 100000 + gap, line));
    CHECK(line.malformed == 99 * 2 + 1 && line.full == 4);
    // said = reset: the next quiet pump tick says nothing, and the next window starts from this line
    CHECK(!gate.take(mcp::WidgetEventCounts {}, 100000 + gap + 50, line));
    mcp::WidgetEventCounts again;
    again.malformed = 1;
    CHECK(!gate.take(again, 100000 + gap + 100, line));
    CHECK(gate.take(mcp::WidgetEventCounts {}, 100000 + 2 * gap, line) && line.malformed == 1 && line.full == 0);
}

void aFullQueueIsLoggedThrottledToo()
{
    mcp::WidgetEventLogGate gate;
    mcp::WidgetEventLogCounts line;
    mcp::WidgetEventCounts full;
    full.full = 5;
    CHECK(gate.take(full, 1000, line) && line.full == 5 && line.malformed == 0);
    CHECK(!gate.take(full, 2000, line));
    CHECK(gate.take(mcp::WidgetEventCounts {}, 1000 + mcp::kWidgetEventLogGapMs, line) && line.full == 5);
    // a clock that went backwards says what waits instead of waiting for ever
    mcp::WidgetEventCounts bad;
    bad.malformed = 1;
    CHECK(gate.take(bad, 10, line) && line.malformed == 1);
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
    moreKeysThanRateSlotsStayInTheGlobalBudget();
    theBudgetIsGlobalAndRefillsWithTime();
    aRateLimitedEventPrintsNothing();
    malformedLoggingIsThrottledWithACount();
    aFullQueueIsLoggedThrottledToo();
    if (failures != 0) {
        std::printf("widget_event_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("widget_event_test: ok\n");
    return 0;
}
