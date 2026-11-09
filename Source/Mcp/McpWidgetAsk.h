/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Proof plan step 3, part 3c: a pd-lua widget asks the AI from the canvas. JUCE-free and Pd-free (unit-tested by
// Tests/mcp/widget_ask_test.cpp through scripts/test-bridge-safety.sh).
//
// Pd side (MCPBridgeWidgetAsk.h): the bridge binds a receiver to `ai-ask` while it serves. A message
//   ai-ask <widget> <rev>                the widget's ask chip ("complete it"), rev = the widget's state revision
//   ai-ask hello <widget>                a widget that was made (or loaded) asks whether anyone hears
// is parsed by the receiver and handed to WidgetAskQueue::offer. The receiver runs wherever Pd dispatches (the audio
// thread too), always under the Pd lock: offer never locks, never allocates and never calls JUCE; one offer runs at a
// time (the Pd lock). The bridge's message-thread pump pops the events and sends /pd/widget_ask.
//
// Bounds: widget names ai.<lowercase word> (kWidgetAskNameLen), rev a whole number 0..9999999. No free text travels
// (review M4: a patch-side text channel into the AI's prompt): anything after the rev is dropped by the receiver. At
// most one ask per widget per kWidgetAskMinGapMs (a hello has its own budget); a fixed queue of kWidgetAskQueueSlots
// (full = dropped and counted).

#pragma once

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mcp {

constexpr int kWidgetAskNameLen = 32;   // ai.<word>, the terminating NUL not counted
constexpr int kWidgetAskQueueSlots = 16;
constexpr int kWidgetAskRateSlots = 16;
constexpr std::int64_t kWidgetAskMinGapMs = 2000;
constexpr int kWidgetAskMaxRev = 9999999;

enum class WidgetAskKind : int { Ask = 0, Hello = 1 };
enum class WidgetAskOutcome : int { Queued, Malformed, RateLimited, Full };

struct WidgetAskEvent {
    WidgetAskKind kind = WidgetAskKind::Ask;
    int rev = 0;
    char widget[kWidgetAskNameLen + 1] = {};
};

struct WidgetAskCounts {
    int malformed = 0;
    int rateLimited = 0;
    int full = 0;
};

// ai.<word>: lower-case letters, digits and '_' after "ai.", the word starting with a letter; at most
// kWidgetAskNameLen bytes. The widget's in-name (ai-<word>-in) is derived from it, so nothing else may pass.
inline bool widgetAskNameOk(char const* name)
{
    if (!name || std::strncmp(name, "ai.", 3) != 0) return false;
    std::size_t const len = std::strlen(name);
    if (len < 4 || len > static_cast<std::size_t>(kWidgetAskNameLen)) return false;
    if (name[3] < 'a' || name[3] > 'z') return false;
    for (std::size_t i = 4; i < len; ++i) {
        char const c = name[i];
        bool const ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    return true;
}

inline bool widgetAskRevOk(double rev)
{
    return std::isfinite(rev) && rev >= 0.0 && rev <= kWidgetAskMaxRev && rev == std::floor(rev);
}

// ai.draw -> ai-draw-in. False (out untouched) for a name widgetAskNameOk refuses or a buffer too small.
inline bool widgetAskInName(char const* widget, char* out, std::size_t outLen)
{
    if (!widgetAskNameOk(widget)) return false;
    int const n = std::snprintf(out, outLen, "ai-%s-in", widget + 3);
    return n > 0 && static_cast<std::size_t>(n) < outLen;
}

// Single producer (the Pd lock), single consumer (the bridge's message-thread pump). Fixed storage.
class WidgetAskQueue {
public:
    WidgetAskOutcome offer(WidgetAskKind kind, char const* widget, double rev, std::int64_t nowMs)
    {
        if (!widgetAskNameOk(widget) || (kind == WidgetAskKind::Ask && !widgetAskRevOk(rev))) {
            malformed.fetch_add(1, std::memory_order_relaxed);
            return WidgetAskOutcome::Malformed;
        }
        RateSlot* slot = rateSlotFor(kind, widget);
        if (slot && slot->used && nowMs >= slot->lastMs && nowMs - slot->lastMs < kWidgetAskMinGapMs) {
            rateLimited.fetch_add(1, std::memory_order_relaxed);
            return WidgetAskOutcome::RateLimited;
        }
        unsigned const head = writeIndex.load(std::memory_order_relaxed);
        unsigned const tail = readIndex.load(std::memory_order_acquire);
        if (head - tail >= static_cast<unsigned>(kWidgetAskQueueSlots)) {
            full.fetch_add(1, std::memory_order_relaxed); // the rate clock is not touched: a retry may go through
            return WidgetAskOutcome::Full;
        }
        WidgetAskEvent& e = slots[head % kWidgetAskQueueSlots];
        e.kind = kind;
        e.rev = kind == WidgetAskKind::Ask ? static_cast<int>(rev) : 0;
        copyName(e.widget, widget);
        writeIndex.store(head + 1, std::memory_order_release);

        if (!slot) slot = claimRateSlot();
        slot->used = true;
        slot->kind = kind;
        copyName(slot->widget, widget);
        slot->lastMs = nowMs;
        return WidgetAskOutcome::Queued;
    }

    bool pop(WidgetAskEvent& out)
    {
        unsigned const tail = readIndex.load(std::memory_order_relaxed);
        unsigned const head = writeIndex.load(std::memory_order_acquire);
        if (tail == head) return false;
        out = slots[tail % kWidgetAskQueueSlots];
        readIndex.store(tail + 1, std::memory_order_release);
        return true;
    }

    // The refusals since the last call (the pump logs them as one console line).
    WidgetAskCounts takeCounts()
    {
        WidgetAskCounts c;
        c.malformed = malformed.exchange(0, std::memory_order_relaxed);
        c.rateLimited = rateLimited.exchange(0, std::memory_order_relaxed);
        c.full = full.exchange(0, std::memory_order_relaxed);
        return c;
    }

private:
    struct RateSlot {
        bool used = false;
        WidgetAskKind kind = WidgetAskKind::Ask;
        char widget[kWidgetAskNameLen + 1] = {};
        std::int64_t lastMs = 0;
    };

    // Producer only.
    RateSlot* rateSlotFor(WidgetAskKind kind, char const* widget)
    {
        for (auto& s : rate)
            if (s.used && s.kind == kind && std::strcmp(s.widget, widget) == 0) return &s;
        return nullptr;
    }

    // A free slot, else the one used longest ago (its widget's next ask is then not limited: never a lost ask).
    RateSlot* claimRateSlot()
    {
        RateSlot* oldest = &rate[0];
        for (auto& s : rate) {
            if (!s.used) return &s;
            if (s.lastMs < oldest->lastMs) oldest = &s;
        }
        return oldest;
    }

    // A checked name (widgetAskNameOk: <= kWidgetAskNameLen bytes) into a kWidgetAskNameLen + 1 buffer. No printf on
    // the dispatch path (review L5: no locale, no formatting).
    static void copyName(char* dst, char const* src)
    {
        std::size_t i = 0;
        for (; src[i] && i < static_cast<std::size_t>(kWidgetAskNameLen); ++i)
            dst[i] = src[i];
        dst[i] = 0;
    }

    WidgetAskEvent slots[kWidgetAskQueueSlots];
    std::atomic<unsigned> writeIndex { 0 };
    std::atomic<unsigned> readIndex { 0 };
    RateSlot rate[kWidgetAskRateSlots];
    std::atomic<int> malformed { 0 };
    std::atomic<int> rateLimited { 0 };
    std::atomic<int> full { 0 };
};

} // namespace mcp
