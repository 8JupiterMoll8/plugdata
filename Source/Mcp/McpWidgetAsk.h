/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Proof plan step 3, part 3c: a pd-lua widget asks the AI from the canvas. JUCE-free and Pd-free (unit-tested by
// Tests/mcp/widget_ask_test.cpp through scripts/test-bridge-safety.sh).
//
// Pd side (MCPBridgeWidgetAsk.h): the bridge binds a receiver to `ai-ask` while it serves. A message
//   ai-ask <widget> <rev> [<paddr>]      the widget's ask chip ("complete it"), rev = the widget's state revision; paddr = the asking object's
//                                        address as text (`p` and hex digits, the token a widget's `end <rev> <paddr>` carries): the widgets of the shared
//                                        ask block send it, so two boxes of one widget (an ai.seq bass and a glitch grid) are told apart by the server
//   ai-ask hello <widget>                a widget that was made (or loaded) asks whether anyone hears
//   ai-ask focus <widget> <rev> <ptr>    the widget's chip ("AI..."): select THIS widget, open the selection pill (the artist then
//                                        types, or presses Enter on nothing = complete it). ptr = the sending pd-lua object's
//                                        address as hex text; the fork only compares it with the boxes of the open patches.
//   ai-ask focus <widget> <rev> <ptr> <x> <y> <w> <h> [subject <text...>]
//                                        the same, and the pill opens DOCKED in that widget-local strip (unzoomed px) with the subject as its
//                                        first chip (McpPillDock.h). A tail that does not parse = the old floating pill.
// is parsed by the receiver and handed to WidgetAskQueue::offer. The receiver runs wherever Pd dispatches (the audio
// thread too), always under the Pd lock: offer never locks, never allocates and never calls JUCE; one offer runs at a
// time (the Pd lock). The bridge's message-thread pump pops the events and sends /pd/widget_ask.
//
// Bounds: widget names ai.<lowercase word> (kWidgetAskNameLen), rev a whole number 0..9999999. No free text travels
// (review M4: a patch-side text channel into the AI's prompt): anything after the rev is dropped by the receiver, except the address of an ask,
// which the receiver keeps only in the shape widgetCallAddrOk accepts (`p` + 1..16 lower-case hex digits) and the queue bounds. At
// most one ask per widget per kWidgetAskMinGapMs (a hello has its own budget); a fixed queue of kWidgetAskQueueSlots
// (full = dropped and counted).

#pragma once

#include "McpPillDock.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mcp {

constexpr int kWidgetAskNameLen = 32;   // ai.<word>, the terminating NUL not counted
constexpr int kWidgetAskQueueSlots = 32; // a hello burst at load: every fixed widget and every named looper of the widget history (McpWidgetState.h) fit
constexpr int kWidgetAskRateSlots = 16;
constexpr std::int64_t kWidgetAskMinGapMs = 2000;
constexpr std::int64_t kWidgetFocusMinGapMs = 300; // a double press of the chip is one focus
constexpr int kWidgetAskMaxRev = 9999999;
constexpr int kWidgetFocusPtrChars = 16; // a 64-bit address as hex digits
constexpr int kWidgetAskAddrChars = 17;  // an ask's instance address: `p` and 16 hex digits, the terminating NUL not counted (the same token as kWidgetCallAddrChars)

enum class WidgetAskKind : int { Ask = 0, Hello = 1, Focus = 2 };
enum class WidgetAskOutcome : int { Queued, Malformed, RateLimited, Full };

struct WidgetAskEvent {
    WidgetAskKind kind = WidgetAskKind::Ask;
    int rev = 0;
    std::uint64_t ptr = 0; // Focus only: the address of the pd-lua object that sent it
    PillDock dock;         // Focus only: the strip the pill docks in and its subject chip (docked = false: the old floating pill)
    char widget[kWidgetAskNameLen + 1] = {};
    char addr[kWidgetAskAddrChars + 1] = {}; // Ask only: the asking object's address as text (`p` + hex), empty when the widget sent none
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

// The hex text of a pointer as a widget writes it (Lua: tostring of its object, "userdata: 0x55d0c8a3b2a0", cut to the digits):
// 1..16 hex digits, an optional 0x, a value that is not 0. No allocation, nothing is dereferenced: the number is only compared.
inline bool widgetFocusPtrParse(char const* text, std::uint64_t& out)
{
    if (!text) return false;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;
    std::uint64_t v = 0;
    int digits = 0;
    for (; *text; ++text, ++digits) {
        char const c = *text;
        int d = -1;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        if (d < 0 || digits >= kWidgetFocusPtrChars) return false;
        v = (v << 4) | static_cast<std::uint64_t>(d);
    }
    if (digits == 0 || v == 0) return false;
    out = v;
    return true;
}

// The same number as lower-case hex digits, no 0x (outLen >= kWidgetFocusPtrChars + 1).
inline void widgetFocusPtrText(std::uint64_t v, char* out, std::size_t outLen)
{
    if (!out || outLen == 0) return;
    int const n = std::snprintf(out, outLen, "%llx", static_cast<unsigned long long>(v));
    if (n < 0 || static_cast<std::size_t>(n) >= outLen) out[0] = 0;
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
    // `ptr`: the Focus kind's pointer (a focus without one names no object); the other kinds keep none.
    // `addr`: the Ask kind's instance address as the receiver checked it (widgetCallAddrOk), or null; the other kinds keep none. Cut at kWidgetAskAddrChars, never overrun.
    // `dock`: the Focus kind's docked-pill strip and subject as pillDockParse made them, or null (= the old pill); the other kinds keep none.
    WidgetAskOutcome offer(WidgetAskKind kind, char const* widget, double rev, std::int64_t nowMs, std::uint64_t ptr = 0, char const* addr = nullptr,
        PillDock const* dock = nullptr)
    {
        bool const wantsRev = kind == WidgetAskKind::Ask || kind == WidgetAskKind::Focus;
        if (!widgetAskNameOk(widget) || (wantsRev && !widgetAskRevOk(rev)) || (kind == WidgetAskKind::Focus && ptr == 0)) {
            malformed.fetch_add(1, std::memory_order_relaxed);
            return WidgetAskOutcome::Malformed;
        }
        RateSlot* slot = rateSlotFor(kind, widget, ptr);
        std::int64_t const gap = kind == WidgetAskKind::Focus ? kWidgetFocusMinGapMs : kWidgetAskMinGapMs;
        if (slot && slot->used && nowMs >= slot->lastMs && nowMs - slot->lastMs < gap) {
            // a double press of the chip is no news for the console (only the asks' limit is: it is said once)
            if (kind != WidgetAskKind::Focus) rateLimited.fetch_add(1, std::memory_order_relaxed);
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
        e.rev = wantsRev ? static_cast<int>(rev) : 0;
        e.ptr = kind == WidgetAskKind::Focus ? ptr : 0;
        copyName(e.widget, widget);
        copyAddr(e.addr, kind == WidgetAskKind::Ask ? addr : nullptr);
        copyDock(e.dock, kind == WidgetAskKind::Focus ? dock : nullptr);
        writeIndex.store(head + 1, std::memory_order_release);

        if (!slot) slot = claimRateSlot();
        slot->used = true;
        slot->kind = kind;
        slot->ptr = kind == WidgetAskKind::Focus ? ptr : 0;
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
        std::uint64_t ptr = 0; // Focus: the instance (two instances of one widget are two budgets)
        std::int64_t lastMs = 0;
    };

    // Producer only.
    RateSlot* rateSlotFor(WidgetAskKind kind, char const* widget, std::uint64_t ptr)
    {
        for (auto& s : rate)
            if (s.used && s.kind == kind && std::strcmp(s.widget, widget) == 0 && (kind != WidgetAskKind::Focus || s.ptr == ptr)) return &s;
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

    // The address (null or empty = none) into a kWidgetAskAddrChars + 1 buffer, cut at the room. No printf on the dispatch path.
    static void copyAddr(char* dst, char const* src)
    {
        std::size_t i = 0;
        if (src)
            for (; src[i] && i < static_cast<std::size_t>(kWidgetAskAddrChars); ++i)
                dst[i] = src[i];
        dst[i] = 0;
    }

    // The dock (null = none) into the event's slot: field by field, the subject bounded and terminated. No printf on the dispatch path.
    static void copyDock(PillDock& dst, PillDock const* src)
    {
        dst = PillDock {};
        if (!src || !src->docked) return;
        dst.docked = true;
        dst.x = src->x;
        dst.y = src->y;
        dst.w = src->w;
        dst.h = src->h;
        std::size_t i = 0;
        for (; src->subject[i] && i < static_cast<std::size_t>(kPillSubjectChars); ++i)
            dst.subject[i] = src->subject[i];
        dst.subject[i] = 0;
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
