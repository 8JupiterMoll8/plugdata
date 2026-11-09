// Proof plan step 3, part 3c (docs/plans/proof3-widgets-steps.md in the MCP repo): the widget ask core.
// JUCE-free, Pd-free (Source/Mcp/McpWidgetAsk.h). A pd-lua widget sends `ai-ask <widget> <rev> [words...]` (or
// `ai-ask hello <widget>`); the bridge's receiver hands the parsed parts to mcp::WidgetAskQueue::offer under the Pd
// lock, and the bridge's message-thread pump pops them as /pd/widget_ask events.
//  - names: only ai.<word> widgets (bounded length, no Pd/system names);
//  - rev: a whole number 0..9999999;
//  - no free text: the event carries no words (review M4);
//  - rate: at most one ask per widget per 2 s (a hello has its own budget), refused asks are counted, not queued;
//  - the queue is fixed (no allocation): a full queue drops and counts.
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpWidgetAsk.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

using mcp::WidgetAskKind;
using mcp::WidgetAskOutcome;
using mcp::WidgetAskQueue;

namespace {

void namesAndRevs()
{
    CHECK(mcp::widgetAskNameOk("ai.draw"));
    CHECK(mcp::widgetAskNameOk("ai.tap"));
    CHECK(mcp::widgetAskNameOk("ai.my_widget2"));
    CHECK(!mcp::widgetAskNameOk(""));
    CHECK(!mcp::widgetAskNameOk("ai."));
    CHECK(!mcp::widgetAskNameOk("draw"));
    CHECK(!mcp::widgetAskNameOk("pd"));
    CHECK(!mcp::widgetAskNameOk("ai.Draw"));          // lower case only: the in-name is derived from it
    CHECK(!mcp::widgetAskNameOk("ai.draw-in"));
    CHECK(!mcp::widgetAskNameOk("ai.dr aw"));
    CHECK(!mcp::widgetAskNameOk("ai.a.b"));
    CHECK(!mcp::widgetAskNameOk("ai.abcdefghijklmnopqrstuvwxyz0123456789")); // > kWidgetAskNameLen
    CHECK(mcp::widgetAskNameOk(nullptr) == false);

    CHECK(mcp::widgetAskRevOk(0));
    CHECK(mcp::widgetAskRevOk(7));
    CHECK(mcp::widgetAskRevOk(9999999));
    CHECK(!mcp::widgetAskRevOk(-1));
    CHECK(!mcp::widgetAskRevOk(1.5));
    CHECK(!mcp::widgetAskRevOk(10000000));
    CHECK(!mcp::widgetAskRevOk(0.0 / 0.0));
}

void askQueuedAndPopped()
{
    WidgetAskQueue q;
    CHECK(q.offer(WidgetAskKind::Ask, "ai.draw", 7, 1000) == WidgetAskOutcome::Queued);
    mcp::WidgetAskEvent e;
    CHECK(q.pop(e));
    CHECK(e.kind == WidgetAskKind::Ask);
    CHECK(std::strcmp(e.widget, "ai.draw") == 0);
    CHECK(e.rev == 7);
    CHECK(!q.pop(e));
    // hello: no rev, no words
    CHECK(q.offer(WidgetAskKind::Hello, "ai.tap", 0, 1000) == WidgetAskOutcome::Queued);
    CHECK(q.pop(e));
    CHECK(e.kind == WidgetAskKind::Hello && std::strcmp(e.widget, "ai.tap") == 0 && e.rev == 0);
}

void malformedRefused()
{
    WidgetAskQueue q;
    CHECK(q.offer(WidgetAskKind::Ask, "pd", 1, 0) == WidgetAskOutcome::Malformed);
    CHECK(q.offer(WidgetAskKind::Ask, "ai.draw", -3, 0) == WidgetAskOutcome::Malformed);
    CHECK(q.offer(WidgetAskKind::Ask, nullptr, 1, 0) == WidgetAskOutcome::Malformed);
    mcp::WidgetAskEvent e;
    CHECK(!q.pop(e));
    auto const c = q.takeCounts();
    CHECK(c.malformed == 3 && c.rateLimited == 0 && c.full == 0);
    auto const again = q.takeCounts();
    CHECK(again.malformed == 0); // taken = reset
}

void rateLimit()
{
    WidgetAskQueue q;
    CHECK(q.offer(WidgetAskKind::Ask, "ai.draw", 1, 10000) == WidgetAskOutcome::Queued);
    CHECK(q.offer(WidgetAskKind::Ask, "ai.draw", 2, 10500) == WidgetAskOutcome::RateLimited);
    CHECK(q.offer(WidgetAskKind::Ask, "ai.draw", 2, 11999) == WidgetAskOutcome::RateLimited);
    // another widget has its own budget
    CHECK(q.offer(WidgetAskKind::Ask, "ai.tap", 1, 10600) == WidgetAskOutcome::Queued);
    // a hello does not use the ask's budget (a widget made and asked within 2 s)
    CHECK(q.offer(WidgetAskKind::Hello, "ai.draw", 0, 10700) == WidgetAskOutcome::Queued);
    CHECK(q.offer(WidgetAskKind::Hello, "ai.draw", 0, 10800) == WidgetAskOutcome::RateLimited);
    // 2 s after the last ACCEPTED ask (a refused one does not restart the clock)
    CHECK(q.offer(WidgetAskKind::Ask, "ai.draw", 3, 12000) == WidgetAskOutcome::Queued);
    auto const c = q.takeCounts();
    CHECK(c.rateLimited == 3);
    // a clock that went backwards (another time source) never locks a widget out for long
    CHECK(q.offer(WidgetAskKind::Ask, "ai.draw", 4, 100) == WidgetAskOutcome::Queued);
}

void rateTableBounded()
{
    WidgetAskQueue q;
    // more widgets than the table holds: the oldest entry is reused, nothing allocates, no ask of a new widget is lost
    char name[16];
    for (int i = 0; i < mcp::kWidgetAskRateSlots + 5; ++i) {
        std::snprintf(name, sizeof(name), "ai.w%d", i);
        CHECK(q.offer(WidgetAskKind::Ask, name, 1, 5000 + i) == WidgetAskOutcome::Queued);
        mcp::WidgetAskEvent e;
        CHECK(q.pop(e));
    }
}

void queueFull()
{
    WidgetAskQueue q;
    char name[16];
    int queued = 0;
    for (int i = 0; i < mcp::kWidgetAskQueueSlots + 3; ++i) {
        std::snprintf(name, sizeof(name), "ai.q%d", i);
        if (q.offer(WidgetAskKind::Ask, name, 1, 1000) == WidgetAskOutcome::Queued) ++queued;
    }
    CHECK(queued == mcp::kWidgetAskQueueSlots);
    CHECK(q.takeCounts().full == 3);
    mcp::WidgetAskEvent e;
    int popped = 0;
    while (q.pop(e)) ++popped;
    CHECK(popped == mcp::kWidgetAskQueueSlots);
}

void inName()
{
    char out[mcp::kWidgetAskNameLen + 4] = {};
    CHECK(mcp::widgetAskInName("ai.draw", out, sizeof(out)));
    CHECK(std::strcmp(out, "ai-draw-in") == 0);
    CHECK(mcp::widgetAskInName("ai.tap", out, sizeof(out)));
    CHECK(std::strcmp(out, "ai-tap-in") == 0);
    CHECK(!mcp::widgetAskInName("pd", out, sizeof(out)));
}

void producerConsumerThreads()
{
    // One producer (the Pd lock serialises every receiver call) and one consumer (the message thread's pump).
    WidgetAskQueue q;
    constexpr int total = 5000;
    std::thread producer([&] {
        char name[16];
        for (int i = 0; i < total; ++i) {
            std::snprintf(name, sizeof(name), "ai.p%d", i % 200);
            while (q.offer(WidgetAskKind::Ask, name, i, static_cast<std::int64_t>(i) * 3000) == WidgetAskOutcome::Full)
                std::this_thread::yield();
        }
    });
    int got = 0;
    int lastRev = -1;
    bool ordered = true;
    while (got < total) {
        mcp::WidgetAskEvent e;
        if (q.pop(e)) {
            if (e.rev <= lastRev) ordered = false;
            lastRev = e.rev;
            ++got;
        } else {
            std::this_thread::yield();
        }
    }
    producer.join();
    CHECK(got == total);
    CHECK(ordered);
}

} // namespace

int main()
{
    namesAndRevs();
    askQueuedAndPopped();
    malformedRefused();
    rateLimit();
    rateTableBounded();
    queueFull();
    inName();
    producerConsumerThreads();
    if (failures) {
        std::printf("widget_ask_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("widget_ask_test: ok\n");
    return 0;
}
