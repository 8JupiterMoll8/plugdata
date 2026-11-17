/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Thread phase 2 (docs/plans/PILL-LANES-INBOX-PLAN-2026-11-16.md section 3): live widget history, the core. JUCE-free and
// Pd-free (unit-tested by Tests/mcp/widget_state_test.cpp through scripts/test-bridge-safety.sh).
//
// Pd side (MCPBridgeWidgetState.h): while the bridge serves, a native receiver is bound to the state name of every watched
// ai.* widget: `ai-tap-state`, `ai-draw-state`, `ai-loop_0-state`, `ai-loop-state` (the legacy looper), the family's
// (`ai-sketch-state`, `ai-knob-state`, ...) and, once a named looper has said hello, `ai-loop_<name>-state`. A widget's state
// frames (`t1 ...`, `K ...`, `end <rev>`) all arrive there; the receiver looks at ONE thing, a message whose selector is `end`
// with a rev, and hands it to WidgetStateBook::noteEnd under the Pd lock (wherever Pd dispatches, the audio thread too).
// Everything else is one pointer compare.
//
// The bridge's message-thread pump (every 50 ms) asks WidgetStateBook::popDue: a widget whose last `end` is 1 s old (kWidgetStateQuietMs)
// is ONE event, `/pd/widget_ask state <canvas> <widget> <rev>`, and only when its rev is not the one last forwarded. At most one event per
// widget per 2 s (a change inside the gap waits for it, it is not lost). The same event address as the ask chip: events are listed
// by address in the wire schema, and the kind word (`ask`, `hello`, `state`) is free, so no schema row and no new schema hash.
//
// Nothing is recorded until the server switches the watch on (`/chat/widget_watch on`, capability `widget_state`): an old server never
// sees a state event, and the receivers cost one atomic read per `end`. Fixed storage, no lock, no allocation, no JUCE on the producer
// side (scripts/test-bridge-safety.sh greps this file for them).
//
// The fixed names are the server's ASKABLE_WIDGETS (src/services/widget-ask.ts): ai.draw, ai.tap, ai.loop_0, ai.loop and the family.

#pragma once

#include "McpWidgetAsk.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mcp {

constexpr int kWidgetStateSlots = 32;
constexpr int kWidgetStateNamedMax = 16;               // named loopers watched besides the fixed names
constexpr int kWidgetStateNamedLen = 12;               // ai.loop_<name>: name = [a-z0-9_]{1,12}, as the server reads it
constexpr std::int64_t kWidgetStateQuietMs = 1000;     // a widget is reported after this long without an `end`
constexpr std::int64_t kWidgetStateMinGapMs = 2000;    // and at most once per this long

inline constexpr char const* kWidgetStateFixed[] = {
    "ai.tap", "ai.draw", "ai.loop_0", "ai.loop", "ai.sketch", "ai.knob", "ai.dice", "ai.grid", "ai.paint", "ai.strip", "ai.listen",
};
constexpr int kWidgetStateFixedCount = static_cast<int>(sizeof(kWidgetStateFixed) / sizeof(kWidgetStateFixed[0]));

enum class WidgetStateOutcome : int { Noted, Off, Malformed };

struct WidgetStateEvent {
    int rev = 0;
    char widget[kWidgetAskNameLen + 1] = {};
};

struct WidgetStateCounts {
    int malformed = 0;
    int namedEvicted = 0; // a named looper made room by pushing out the one idle longest
};

// ai.loop_<name>: a named looper (name [a-z0-9_]{1,12}); `ai.loop` alone (the legacy looper) is not one.
inline bool widgetStateNamedLooperOk(char const* widget)
{
    static constexpr char kPrefix[] = "ai.loop_";
    constexpr std::size_t prefixLen = sizeof(kPrefix) - 1;
    if (!widget || std::strncmp(widget, kPrefix, prefixLen) != 0) return false;
    std::size_t const len = std::strlen(widget + prefixLen);
    if (len < 1 || len > static_cast<std::size_t>(kWidgetStateNamedLen)) return false;
    for (std::size_t i = 0; i < len; ++i) {
        char const c = widget[prefixLen + i];
        bool const ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    return true;
}

// ai.tap -> ai-tap-state. False (out untouched) for a name widgetAskNameOk refuses or a buffer too small.
inline bool widgetStateSymbolName(char const* widget, char* out, std::size_t outLen)
{
    if (!widgetAskNameOk(widget)) return false;
    int const n = std::snprintf(out, outLen, "ai-%s-state", widget + 3);
    return n > 0 && static_cast<std::size_t>(n) < outLen;
}

inline bool widgetStateFixedOk(char const* widget)
{
    if (!widget) return false;
    for (char const* w : kWidgetStateFixed)
        if (std::strcmp(w, widget) == 0) return true;
    return false;
}

// One producer (whatever thread Pd dispatches on, under the Pd lock), one consumer (the bridge's message-thread pump). The producer's
// part is atomics only. claim / nameAt / popDue / rewind / takeCounts are the message thread's (the pump, and bind / unbind under it).
class WidgetStateBook {
public:
    // The slot of a widget, claiming one for a name not held yet: a fixed widget, or a named looper. -1 for any other name. A named
    // looper beyond kWidgetStateNamedMax takes the slot of the named looper that has been idle longest (claimed or last `end`, whichever
    // is later; `nowMs` stamps the claim): `*evicted` says so, and the caller frees the old receiver of that slot BEFORE it binds the new
    // one (it holds the Pd lock for both, so no `end` of the old one arrives in between). Message thread.
    int claim(char const* widget, std::int64_t nowMs = 0, bool* evicted = nullptr)
    {
        if (evicted) *evicted = false;
        if (!widgetAskNameOk(widget)) return -1;
        for (int i = 0; i < kWidgetStateSlots; ++i)
            if (slots[i].used.load(std::memory_order_acquire) && std::strcmp(slots[i].widget, widget) == 0) return i;
        bool const fixed = widgetStateFixedOk(widget);
        if (!fixed && !widgetStateNamedLooperOk(widget)) return -1;
        int target = -1;
        bool const replacing = !fixed && namedClaimed >= kWidgetStateNamedMax;
        if (replacing) {
            target = idlestNamed();
        } else {
            for (int i = 0; i < kWidgetStateSlots && target < 0; ++i)
                if (!slots[i].used.load(std::memory_order_acquire)) target = i;
        }
        if (target < 0) return -1;
        Slot& s = slots[target];
        s.used.store(false, std::memory_order_release);
        std::size_t n = 0;
        for (; widget[n] && n < static_cast<std::size_t>(kWidgetAskNameLen); ++n)
            s.widget[n] = widget[n];
        s.widget[n] = 0;
        s.rev.store(0, std::memory_order_relaxed);
        s.lastEndMs.store(0, std::memory_order_relaxed);
        s.sentSeq = s.seq.load(std::memory_order_relaxed);
        s.forwardedRev = -1;
        s.forwarded = false;
        s.lastForwardMs = 0;
        s.named = !fixed;
        s.claimedMs = nowMs;
        s.used.store(true, std::memory_order_release); // the name is written before the slot is visible
        if (replacing) {
            namedEvicted.fetch_add(1, std::memory_order_relaxed);
            if (evicted) *evicted = true;
        } else if (!fixed) {
            ++namedClaimed;
        }
        return target;
    }

    // The widget held by a slot, or null (the bridge binds a receiver for each held name again after a stop). Message thread.
    char const* nameAt(int slot) const
    {
        if (slot < 0 || slot >= kWidgetStateSlots || !slots[slot].used.load(std::memory_order_acquire)) return nullptr;
        return slots[slot].widget;
    }

    // The server switches the history on or off (`/chat/widget_watch`, any thread).
    void setWatching(bool on) { watching_.store(on, std::memory_order_relaxed); }
    bool watching() const { return watching_.load(std::memory_order_relaxed); }

    // A message `end <rev>` was seen on the state name of `slot`. Pd's dispatch: no lock, no allocation, no JUCE.
    WidgetStateOutcome noteEnd(int slot, double rev, std::int64_t nowMs)
    {
        if (!watching_.load(std::memory_order_relaxed)) return WidgetStateOutcome::Off;
        if (slot < 0 || slot >= kWidgetStateSlots || !widgetAskRevOk(rev)) {
            malformed.fetch_add(1, std::memory_order_relaxed);
            return WidgetStateOutcome::Malformed;
        }
        Slot& s = slots[slot];
        s.rev.store(static_cast<int>(rev), std::memory_order_relaxed);
        s.lastEndMs.store(nowMs, std::memory_order_relaxed);
        s.seq.fetch_add(1, std::memory_order_release);
        return WidgetStateOutcome::Noted;
    }

    // An `end` without a number (the receiver could not read a rev).
    void noteMalformed() { malformed.fetch_add(1, std::memory_order_relaxed); }

    // The next widget that has been quiet for kWidgetStateQuietMs with a rev not forwarded yet, one per call. Message thread.
    bool popDue(WidgetStateEvent& out, std::int64_t nowMs)
    {
        if (!watching_.load(std::memory_order_relaxed)) return false;
        for (int i = 0; i < kWidgetStateSlots; ++i) {
            Slot& s = slots[i];
            if (!s.used.load(std::memory_order_acquire)) continue;
            unsigned const seq = s.seq.load(std::memory_order_acquire);
            if (seq == s.sentSeq) continue;
            std::int64_t const last = s.lastEndMs.load(std::memory_order_relaxed);
            if (nowMs < last || nowMs - last < kWidgetStateQuietMs) continue;
            if (s.forwarded && nowMs - s.lastForwardMs < kWidgetStateMinGapMs) continue; // stays waiting: the change is not lost
            int const rev = s.rev.load(std::memory_order_relaxed);
            s.sentSeq = seq;
            if (rev == s.forwardedRev) continue; // the same state again (a dump, a ghost): nothing new
            s.forwardedRev = rev;
            s.forwarded = true;
            s.lastForwardMs = nowMs;
            out.rev = rev;
            std::size_t n = 0;
            for (; s.widget[n] && n < static_cast<std::size_t>(kWidgetAskNameLen); ++n)
                out.widget[n] = s.widget[n];
            out.widget[n] = 0;
            return true;
        }
        return false;
    }

    // The refusals since the last call (the pump logs them as one console line).
    WidgetStateCounts takeCounts()
    {
        WidgetStateCounts c;
        c.malformed = malformed.exchange(0, std::memory_order_relaxed);
        c.namedEvicted = namedEvicted.exchange(0, std::memory_order_relaxed);
        return c;
    }

    // Every receiver is freed (the bridge stopped): the watch is off and what was noted is forgotten; the names stay held, so the next
    // start binds a receiver for each of them again (a named looper says hello once, at load: it would not be heard of again). Message
    // thread, with no receiver bound.
    void rewind()
    {
        watching_.store(false, std::memory_order_relaxed);
        for (auto& s : slots) {
            s.rev.store(0, std::memory_order_relaxed);
            s.lastEndMs.store(0, std::memory_order_relaxed);
            s.seq.store(0, std::memory_order_relaxed);
            s.sentSeq = 0;
            s.forwardedRev = -1;
            s.forwarded = false;
            s.lastForwardMs = 0;
        }
        malformed.store(0, std::memory_order_relaxed);
        namedEvicted.store(0, std::memory_order_relaxed);
    }

private:
    struct Slot {
        // The producer's, atomics only.
        std::atomic<int> rev { 0 };
        std::atomic<std::int64_t> lastEndMs { 0 };
        std::atomic<unsigned> seq { 0 };
        // The message thread's (`used` is atomic so that a reader never sees a name that is still being written).
        std::atomic<bool> used { false };
        char widget[kWidgetAskNameLen + 1] = {};
        unsigned sentSeq = 0;
        int forwardedRev = -1;
        bool forwarded = false;
        std::int64_t lastForwardMs = 0;
        bool named = false;
        std::int64_t claimedMs = 0;
    };

    // The named looper idle longest (a claim or an `end`, the later one); -1 when none is held.
    int idlestNamed() const
    {
        int best = -1;
        std::int64_t bestAt = 0;
        for (int i = 0; i < kWidgetStateSlots; ++i) {
            Slot const& s = slots[i];
            if (!s.used.load(std::memory_order_acquire) || !s.named) continue;
            std::int64_t const at = std::max(s.claimedMs, s.lastEndMs.load(std::memory_order_relaxed));
            if (best < 0 || at < bestAt) {
                best = i;
                bestAt = at;
            }
        }
        return best;
    }

    Slot slots[kWidgetStateSlots];
    int namedClaimed = 0;
    std::atomic<bool> watching_ { false };
    std::atomic<int> malformed { 0 };
    std::atomic<int> namedEvicted { 0 };
};

} // namespace mcp
