/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Phase 3 of the ai_widget plan (docs/plans/AI-WIDGET-TOOL-PLAN-2026-11-21.md in the MCP repo), capability `widget_event`: a widget tells
// the AI what the artist did with a proposal. JUCE-free and Pd-free (unit-tested by Tests/mcp/widget_event_test.cpp through
// scripts/test-bridge-safety.sh).
//
// Pd side (MCPBridgeWidgetEvent.h): the bridge binds a receiver to `ai-event` while it serves, the same way it binds `ai-ask`. A message
//   ai-event <widget> <rev> <paddr> use|no|back
//     use   the artist took the proposal (a click on the ghost, the keep chip)
//     no    the artist refused it (the bag chip, a stroke over the ghost, a clear)
//     back  the artist took an AI landing back (undo)
// is parsed by the receiver and handed to WidgetEventQueue::offer. rev = the widget's revision AFTER the action; paddr = the sending
// object's address as text (`p` and hex digits), which the fork only compares and tells apart per instance, it is never read as a pointer.
// The receiver runs wherever Pd dispatches (the audio thread too), always under the Pd lock: offer never locks, never allocates and never
// calls JUCE (scripts/test-bridge-safety.sh greps this file for it); one offer runs at a time. The bridge's message-thread pump pops the
// events and sends them on the existing event address: /pd/widget_ask <use|no|back> <canvas> <widget> <rev> (the kind word is free there).
//
// Bounds: no free text travels. Widget names ai.<lowercase word> (kWidgetAskNameLen), rev a whole number 0..9999999, the address
// `p[0-9a-f]{1,16}`, the word one of three. At most one event per widget, instance and word per kWidgetEventMinGapMs (a table of
// kWidgetEventRateSlots keys, the one used longest ago is reused), and over ALL keys at most kWidgetEventGlobalPerWindow events in any
// kWidgetEventGlobalWindowMs (a patch that rotates through more addresses than the table holds would otherwise pass every event: each key is
// evicted before it is asked again); a fixed queue of kWidgetEventQueueSlots (full = dropped and counted, the rate clock and the budget
// untouched). The console: an event dropped by a rate limit (the per-key gap, the global budget) prints nothing, a double click is normal;
// malformed events and a full queue are logged at most once per kWidgetEventLogGapMs with the count (WidgetEventLogGate, the pump's).

#pragma once

#include "McpWidgetCall.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace mcp {

constexpr int kWidgetEventQueueSlots = 32;
constexpr int kWidgetEventRateSlots = 16;
constexpr std::int64_t kWidgetEventMinGapMs = 500; // a double click is one event; a flood of them is not forwarded
constexpr int kWidgetEventGlobalPerWindow = 8;           // events forwarded over all widgets and instances ...
constexpr std::int64_t kWidgetEventGlobalWindowMs = 1000; // ... in any window of this length
constexpr std::int64_t kWidgetEventLogGapMs = 5000;      // a console line for refused events at most this often

enum class WidgetEventKind : int { Use = 0, No = 1, Back = 2 };
enum class WidgetEventOutcome : int { Queued, Malformed, UnknownWord, BadAddress, RateLimited, BudgetLimited, Full };

inline char const* widgetEventKindWord(WidgetEventKind kind)
{
    switch (kind) {
    case WidgetEventKind::Use: return "use";
    case WidgetEventKind::No: return "no";
    case WidgetEventKind::Back: return "back";
    }
    return "use";
}

// Exactly use, no or back (lower case, nothing around it).
inline bool widgetEventKindParse(char const* word, WidgetEventKind& out)
{
    if (!word) return false;
    if (std::strcmp(word, "use") == 0) out = WidgetEventKind::Use;
    else if (std::strcmp(word, "no") == 0) out = WidgetEventKind::No;
    else if (std::strcmp(word, "back") == 0) out = WidgetEventKind::Back;
    else return false;
    return true;
}

struct WidgetEvent {
    WidgetEventKind kind = WidgetEventKind::Use;
    int rev = 0;
    char widget[kWidgetAskNameLen + 1] = {};
    char addr[kWidgetCallAddrChars + 1] = {};
};

struct WidgetEventCounts {
    int malformed = 0; // a bad name, rev, word or address text
    int rateLimited = 0;   // the per-key gap: silent in the console
    int budgetLimited = 0; // the global budget: silent in the console
    int full = 0;
};

// Single producer (the Pd lock), single consumer (the bridge's message-thread pump). Fixed storage.
class WidgetEventQueue {
public:
    WidgetEventOutcome offer(char const* widget, double rev, char const* addr, char const* word, std::int64_t nowMs)
    {
        if (!widgetAskNameOk(widget) || !widgetAskRevOk(rev)) return refuse(WidgetEventOutcome::Malformed);
        WidgetEventKind kind = WidgetEventKind::Use;
        if (!widgetEventKindParse(word, kind)) return refuse(WidgetEventOutcome::UnknownWord);
        if (!widgetCallAddrOk(addr)) return refuse(WidgetEventOutcome::BadAddress);

        RateSlot* slot = rateSlotFor(kind, widget, addr);
        if (slot && slot->used && nowMs >= slot->lastMs && nowMs - slot->lastMs < kWidgetEventMinGapMs) {
            rateLimited.fetch_add(1, std::memory_order_relaxed);
            return WidgetEventOutcome::RateLimited;
        }
        // The global budget: a free entry of the window, else the window is spent. Only looked at here; an event that is dropped later (a full
        // queue) leaves it alone.
        BudgetEntry* const budgetEntry = freeBudgetEntry(nowMs);
        if (!budgetEntry) {
            budgetLimited.fetch_add(1, std::memory_order_relaxed);
            return WidgetEventOutcome::BudgetLimited;
        }
        unsigned const head = writeIndex.load(std::memory_order_relaxed);
        unsigned const tail = readIndex.load(std::memory_order_acquire);
        if (head - tail >= static_cast<unsigned>(kWidgetEventQueueSlots)) {
            full.fetch_add(1, std::memory_order_relaxed); // the rate clock is not touched: a retry may go through
            return WidgetEventOutcome::Full;
        }
        WidgetEvent& e = slots[head % kWidgetEventQueueSlots];
        e.kind = kind;
        e.rev = static_cast<int>(rev);
        copyText(e.widget, kWidgetAskNameLen, widget);
        copyText(e.addr, kWidgetCallAddrChars, addr);
        writeIndex.store(head + 1, std::memory_order_release);

        if (!slot) slot = claimRateSlot();
        slot->used = true;
        slot->kind = kind;
        copyText(slot->widget, kWidgetAskNameLen, widget);
        copyText(slot->addr, kWidgetCallAddrChars, addr);
        slot->lastMs = nowMs;
        budgetEntry->used = true;
        budgetEntry->ms = nowMs;
        return WidgetEventOutcome::Queued;
    }

    bool pop(WidgetEvent& out)
    {
        unsigned const tail = readIndex.load(std::memory_order_relaxed);
        unsigned const head = writeIndex.load(std::memory_order_acquire);
        if (tail == head) return false;
        out = slots[tail % kWidgetEventQueueSlots];
        readIndex.store(tail + 1, std::memory_order_release);
        return true;
    }

    // The refusals since the last call (the pump logs them as one console line).
    WidgetEventCounts takeCounts()
    {
        WidgetEventCounts c;
        c.malformed = malformed.exchange(0, std::memory_order_relaxed);
        c.rateLimited = rateLimited.exchange(0, std::memory_order_relaxed);
        c.budgetLimited = budgetLimited.exchange(0, std::memory_order_relaxed);
        c.full = full.exchange(0, std::memory_order_relaxed);
        return c;
    }

private:
    struct RateSlot {
        bool used = false;
        WidgetEventKind kind = WidgetEventKind::Use;
        char widget[kWidgetAskNameLen + 1] = {};
        char addr[kWidgetCallAddrChars + 1] = {}; // the instance: two objects of one widget are two budgets
        std::int64_t lastMs = 0;
    };

    // One forwarded event of the global window (its time).
    struct BudgetEntry {
        bool used = false;
        std::int64_t ms = 0;
    };

    WidgetEventOutcome refuse(WidgetEventOutcome why)
    {
        malformed.fetch_add(1, std::memory_order_relaxed);
        return why;
    }

    // Producer only.
    RateSlot* rateSlotFor(WidgetEventKind kind, char const* widget, char const* addr)
    {
        for (auto& s : rate)
            if (s.used && s.kind == kind && std::strcmp(s.widget, widget) == 0 && std::strcmp(s.addr, addr) == 0) return &s;
        return nullptr;
    }

    // Producer only. An entry that is unused or older than the window (or from a clock that has gone backwards) is free; null = all of the
    // kWidgetEventGlobalPerWindow are inside the window, which is a sliding one: the first of them frees its entry a window after it was sent.
    BudgetEntry* freeBudgetEntry(std::int64_t nowMs)
    {
        for (auto& b : budget)
            if (!b.used || nowMs < b.ms || nowMs - b.ms >= kWidgetEventGlobalWindowMs) return &b;
        return nullptr;
    }

    // A free slot, else the one used longest ago (its key's next event is then not limited: never a lost event).
    RateSlot* claimRateSlot()
    {
        RateSlot* oldest = &rate[0];
        for (auto& s : rate) {
            if (!s.used) return &s;
            if (s.lastMs < oldest->lastMs) oldest = &s;
        }
        return oldest;
    }

    // A checked text (at most `room` bytes: the name and address checks above bound it) into a room + 1 buffer. No printf on the dispatch path.
    static void copyText(char* dst, int room, char const* src)
    {
        std::size_t i = 0;
        for (; src[i] && i < static_cast<std::size_t>(room); ++i)
            dst[i] = src[i];
        dst[i] = 0;
    }

    WidgetEvent slots[kWidgetEventQueueSlots];
    std::atomic<unsigned> writeIndex { 0 };
    std::atomic<unsigned> readIndex { 0 };
    RateSlot rate[kWidgetEventRateSlots];
    BudgetEntry budget[kWidgetEventGlobalPerWindow];
    std::atomic<int> malformed { 0 };
    std::atomic<int> rateLimited { 0 };
    std::atomic<int> budgetLimited { 0 };
    std::atomic<int> full { 0 };
};

// What the pump says in the console about refused events: only what is wrong (a malformed event, a full queue), never a rate-limited one
// (a double click is normal, a flood is shed by the budget). At most one line per kWidgetEventLogGapMs; what came meanwhile is summed into
// it, nothing is lost. Message-thread pump only (no atomics, not thread safe).
struct WidgetEventLogCounts {
    int malformed = 0;
    int full = 0;
};

class WidgetEventLogGate {
public:
    // Adds `fresh` (the counts since the last tick) to what waits. True, with `out` = everything that waited (then reset), when something
    // printable waits and the gap since the last line is over (or there was no line yet, or the clock went backwards).
    bool take(WidgetEventCounts const& fresh, std::int64_t nowMs, WidgetEventLogCounts& out)
    {
        waiting.malformed = sum(waiting.malformed, fresh.malformed);
        waiting.full = sum(waiting.full, fresh.full);
        if (waiting.malformed == 0 && waiting.full == 0) return false;
        if (said && nowMs >= lastMs && nowMs - lastMs < kWidgetEventLogGapMs) return false;
        out = waiting;
        waiting = WidgetEventLogCounts {};
        said = true;
        lastMs = nowMs;
        return true;
    }

private:
    static int sum(int a, int b) { return a > std::numeric_limits<int>::max() - b ? std::numeric_limits<int>::max() : a + b; }

    WidgetEventLogCounts waiting;
    bool said = false;
    std::int64_t lastMs = 0;
};

} // namespace mcp
