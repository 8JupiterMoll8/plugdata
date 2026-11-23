// Proof plan step 3, part 3c (docs/plans/proof3-widgets-steps.md in the MCP repo): the widget ask core.
// JUCE-free, Pd-free (Source/Mcp/McpWidgetAsk.h). A pd-lua widget sends `ai-ask <widget> <rev> [words...]` (or
// `ai-ask hello <widget>`); the bridge's receiver hands the parsed parts to mcp::WidgetAskQueue::offer under the Pd
// lock, and the bridge's message-thread pump pops them as /pd/widget_ask events.
//  - names: only ai.<word> widgets (bounded length, no Pd/system names);
//  - rev: a whole number 0..9999999;
//  - no free text: the event carries no words (review M4);
//  - rate: at most one ask per widget per 2 s (a hello has its own budget), refused asks are counted, not queued;
//  - the queue is fixed (no allocation): a full queue drops and counts.
//  - the chip's focus word (`ai-ask focus <widget> <rev> <ptr>`): a kind of its own, with the pointer and a short budget;
//  - an ask may carry the asking object's address (`ai-ask <widget> <rev> <paddr>`): kept for the Ask kind, bounded, forwarded as the 5th argument;
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

// The chip's focus word (`ai-ask focus <widget> <rev> <ptr>`): the pressed widget asks the fork to select it and open the pill.
// It carries the pointer of the pd-lua object that sent it (compared by the fork, never dereferenced), has its own short
// budget (a double press is one focus; an ask a moment later is not refused by it) and is no ask.
void focusKind()
{
    WidgetAskQueue q;
    CHECK(q.offer(WidgetAskKind::Focus, "ai.tap", 7, 1000, 0x55d0c8a3b2a0ULL) == WidgetAskOutcome::Queued);
    mcp::WidgetAskEvent e;
    CHECK(q.pop(e));
    CHECK(e.kind == WidgetAskKind::Focus && std::strcmp(e.widget, "ai.tap") == 0 && e.rev == 7);
    CHECK(e.ptr == 0x55d0c8a3b2a0ULL);
    // a focus without a pointer names no object: malformed. A bad rev too.
    CHECK(q.offer(WidgetAskKind::Focus, "ai.tap", 7, 5000, 0) == WidgetAskOutcome::Malformed);
    CHECK(q.offer(WidgetAskKind::Focus, "ai.tap", -1, 5000, 0x10) == WidgetAskOutcome::Malformed);
    CHECK(q.offer(WidgetAskKind::Focus, "pd", 1, 5000, 0x10) == WidgetAskOutcome::Malformed);
    CHECK(!q.pop(e));
    // its own budget: not the ask's 2 s, a short one against a double press
    CHECK(q.offer(WidgetAskKind::Focus, "ai.knob", 1, 10000, 0x10) == WidgetAskOutcome::Queued);
    CHECK(q.offer(WidgetAskKind::Focus, "ai.knob", 1, 10000 + mcp::kWidgetFocusMinGapMs - 1, 0x10) == WidgetAskOutcome::RateLimited);
    CHECK(q.offer(WidgetAskKind::Focus, "ai.knob", 1, 10000 + mcp::kWidgetFocusMinGapMs, 0x10) == WidgetAskOutcome::Queued);
    // the budget is the instance's: another instance of the same widget is not refused, and the console hears nothing of a double press
    CHECK(q.offer(WidgetAskKind::Focus, "ai.knob", 1, 20500, 0x20) == WidgetAskOutcome::Queued);
    CHECK(q.offer(WidgetAskKind::Focus, "ai.knob", 1, 20510, 0x21) == WidgetAskOutcome::Queued);
    CHECK(q.offer(WidgetAskKind::Focus, "ai.knob", 1, 20520, 0x21) == WidgetAskOutcome::RateLimited);
    CHECK(q.takeCounts().rateLimited == 0);
    {
        mcp::WidgetAskEvent drain;
        while (q.pop(drain)) { }
    }
    // a focus does not spend the ask's budget of the same widget, and an ask does not spend the focus's
    CHECK(q.offer(WidgetAskKind::Ask, "ai.knob", 1, 10100) == WidgetAskOutcome::Queued);
    CHECK(q.offer(WidgetAskKind::Focus, "ai.dice", 1, 20000, 0x10) == WidgetAskOutcome::Queued);
    CHECK(q.offer(WidgetAskKind::Ask, "ai.dice", 1, 20010) == WidgetAskOutcome::Queued);
    CHECK(q.offer(WidgetAskKind::Ask, "ai.dice", 1, 20020) == WidgetAskOutcome::RateLimited);
    // an ask or a hello with a pointer given keeps no pointer (the event of those kinds never carries one)
    mcp::WidgetAskEvent a;
    while (q.pop(a)) { }
    CHECK(q.offer(WidgetAskKind::Ask, "ai.grid", 3, 30000, 0x99) == WidgetAskOutcome::Queued);
    CHECK(q.pop(a) && a.kind == WidgetAskKind::Ask && a.ptr == 0);
}

// An ask may carry the asking object's address (`ai-ask <widget> <rev> <paddr>`, the shared ask block of the widgets, 2026-11-23b): the queue keeps it for the Ask kind only, bounded, and the pump
// forwards it as the 5th argument of /pd/widget_ask so the server can tell two boxes of one widget apart (an ai.seq bass and a glitch grid). The receiver checks the shape (widgetCallAddrOk) before
// it offers; the queue's own guard is the length.
void askCarriesItsAddress()
{
    WidgetAskQueue q;
    mcp::WidgetAskEvent e;
    CHECK(q.offer(WidgetAskKind::Ask, "ai.seq", 5, 1000, 0, "p55d0c8a3b2a0") == WidgetAskOutcome::Queued);
    CHECK(q.pop(e) && e.kind == WidgetAskKind::Ask && e.rev == 5 && std::strcmp(e.widget, "ai.seq") == 0);
    CHECK(std::strcmp(e.addr, "p55d0c8a3b2a0") == 0);
    // no address (an older widget, or one whose address the receiver refused): empty, and the ask is the same ask
    CHECK(q.offer(WidgetAskKind::Ask, "ai.draw", 7, 1000) == WidgetAskOutcome::Queued);
    CHECK(q.pop(e) && e.addr[0] == 0);
    CHECK(q.offer(WidgetAskKind::Ask, "ai.knob", 7, 1000, 0, nullptr) == WidgetAskOutcome::Queued);
    CHECK(q.pop(e) && e.addr[0] == 0);
    // a hello and a focus keep none, whatever they are given (the pump never sends one for them)
    CHECK(q.offer(WidgetAskKind::Hello, "ai.tap", 0, 1000, 0, "p1") == WidgetAskOutcome::Queued);
    CHECK(q.pop(e) && e.kind == WidgetAskKind::Hello && e.addr[0] == 0);
    CHECK(q.offer(WidgetAskKind::Focus, "ai.tap", 3, 1000, 0x55d0ULL, "p1") == WidgetAskOutcome::Queued);
    CHECK(q.pop(e) && e.kind == WidgetAskKind::Focus && e.ptr == 0x55d0ULL && e.addr[0] == 0);
    // a text longer than the room is cut, never overrun (the receiver refuses the wrong shape; the queue bounds the length)
    CHECK(q.offer(WidgetAskKind::Ask, "ai.paint", 2, 1000, 0, "p0123456789abcdef0123456789") == WidgetAskOutcome::Queued);
    CHECK(q.pop(e) && static_cast<int>(std::strlen(e.addr)) == mcp::kWidgetAskAddrChars);
    // the budget is still the widget's, not the box's: a second box of the same widget within the gap is limited as before (documented limit)
    CHECK(q.offer(WidgetAskKind::Ask, "ai.seq", 5, 20000, 0, "p1") == WidgetAskOutcome::Queued);
    CHECK(q.offer(WidgetAskKind::Ask, "ai.seq", 5, 20100, 0, "p2") == WidgetAskOutcome::RateLimited);
    CHECK(q.pop(e) && std::strcmp(e.addr, "p1") == 0);
    CHECK(!q.pop(e));
}

void focusPtrText()
{
    std::uint64_t v = 1;
    CHECK(mcp::widgetFocusPtrParse("55d0c8a3b2a0", v) && v == 0x55d0c8a3b2a0ULL);
    CHECK(mcp::widgetFocusPtrParse("0x55D0C8A3B2A0", v) && v == 0x55d0c8a3b2a0ULL); // Lua's tostring spelling, upper case too
    CHECK(mcp::widgetFocusPtrParse("1", v) && v == 1);
    CHECK(mcp::widgetFocusPtrParse("ffffffffffffffff", v) && v == 0xffffffffffffffffULL);
    CHECK(!mcp::widgetFocusPtrParse("", v));
    CHECK(!mcp::widgetFocusPtrParse("0x", v));
    CHECK(!mcp::widgetFocusPtrParse("0", v));                 // null names nothing
    CHECK(!mcp::widgetFocusPtrParse("00000000", v));
    CHECK(!mcp::widgetFocusPtrParse("12345678901234567", v)); // more than 16 digits
    CHECK(!mcp::widgetFocusPtrParse("userdata:", v));
    CHECK(!mcp::widgetFocusPtrParse("55d0 c8", v));
    CHECK(!mcp::widgetFocusPtrParse("-5", v));
    CHECK(!mcp::widgetFocusPtrParse(nullptr, v));
    char out[mcp::kWidgetFocusPtrChars + 1] = {};
    mcp::widgetFocusPtrText(0x55d0c8a3b2a0ULL, out, sizeof(out));
    CHECK(std::strcmp(out, "55d0c8a3b2a0") == 0);
    std::uint64_t back = 0;
    CHECK(mcp::widgetFocusPtrParse(out, back) && back == 0x55d0c8a3b2a0ULL);
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
    focusKind();
    askCarriesItsAddress();
    focusPtrText();
    producerConsumerThreads();
    if (failures) {
        std::printf("widget_ask_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("widget_ask_test: ok\n");
    return 0;
}
