/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Proof plan step 3, part 3c: a pd-lua widget asks the AI from the canvas. JUCE-free and Pd-free (unit-tested by
// Tests/mcp/widget_ask_test.cpp through scripts/test-bridge-safety.sh).
//
// Pd side (MCPBridgeWidgetAsk.h): the bridge binds a receiver to `ai-ask` while it serves. A message
//   ai-ask <widget> <rev> [words...]     the widget's ask chip ("complete it"), rev = the widget's state revision
//   ai-ask hello <widget>                a widget that was made (or loaded) asks whether anyone hears
// is parsed by the receiver and handed to WidgetAskQueue::offer. The receiver runs wherever Pd dispatches (the audio
// thread too), always under the Pd lock: offer never locks, never allocates and never calls JUCE; one offer runs at a
// time (the Pd lock). The bridge's message-thread pump pops the events and sends /pd/widget_ask.
//
// Bounds: widget names ai.<lowercase word> (kWidgetAskNameLen), rev a whole number 0..9999999, words cut at
// kWidgetAskWordsLen bytes, control characters made spaces; at most one ask per widget per kWidgetAskMinGapMs (a hello
// has its own budget); a fixed queue of kWidgetAskQueueSlots (full = dropped and counted).

#pragma once

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mcp {

constexpr int kWidgetAskNameLen = 32;   // ai.<word>, the terminating NUL not counted
constexpr int kWidgetAskWordsLen = 161; // 160 bytes of words + NUL
constexpr int kWidgetAskMaxWords = 16;  // atoms after the rev that are read at all
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
    char words[kWidgetAskWordsLen] = {};
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

// Appends `word` to `buf` (a space before it unless first), cut at the buffer's end. `used` = bytes in buf. Returns
// false when the word did not fit whole (what fits is kept).
inline bool widgetAskAppend(char* buf, std::size_t bufLen, int& used, char const* word)
{
    if (!buf || bufLen == 0 || !word) return false;
    std::size_t pos = used > 0 ? static_cast<std::size_t>(used) : 0;
    if (pos >= bufLen - 1) return false;
    bool whole = true;
    if (pos > 0) buf[pos++] = ' ';
    for (char const* w = word; *w; ++w) {
        if (pos >= bufLen - 1) {
            whole = false;
            break;
        }
        buf[pos++] = *w;
    }
    buf[pos] = 0;
    used = static_cast<int>(pos);
    return whole;
}

// %g with up to 6 significant digits ("0.5", "110"): how Pd itself prints a float atom.
inline void widgetAskFormatNumber(char* out, std::size_t outLen, double v)
{
    std::snprintf(out, outLen, "%g", v);
}

// Single producer (the Pd lock), single consumer (the bridge's message-thread pump). Fixed storage.
class WidgetAskQueue {
public:
    WidgetAskOutcome offer(WidgetAskKind kind, char const* widget, double rev, char const* words, std::int64_t nowMs)
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
        std::snprintf(e.widget, sizeof(e.widget), "%s", widget);
        copyWords(e.words, words);
        writeIndex.store(head + 1, std::memory_order_release);

        if (!slot) slot = claimRateSlot();
        slot->used = true;
        slot->kind = kind;
        std::snprintf(slot->widget, sizeof(slot->widget), "%s", widget);
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

    static void copyWords(char* dst, char const* src)
    {
        std::size_t i = 0;
        if (src)
            for (; src[i] && i < static_cast<std::size_t>(kWidgetAskWordsLen - 1); ++i) {
                auto const c = static_cast<unsigned char>(src[i]);
                dst[i] = (c < 0x20 || c == 0x7f) ? ' ' : static_cast<char>(c);
            }
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
