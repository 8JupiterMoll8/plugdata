/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// /pd/wire_activity (V1, 2026-11-06): "is this knob or this message actually sending anything into the filter?"
// PlugData already reports every message that crosses a control wire: Pd's outlet_* functions call
// plugdata_forward_message(1, <the t_outconnect>, selector, argc, argv) for each connection while
// plugdata_debugging_or_activity_enabled() is on, and MessageDispatcher::enqueueMessage receives it (that is what
// flashes a cable). The bridge's hook sits at the top of enqueueMessage (MCP-HOOK(wireActivity)), before the GUI's
// blocking and de-duplication, and hands each control message to Table::record.
//
// This file is the window: up to kMaxWires watched connections (their t_outconnect addresses as opaque keys, sorted),
// per wire a message count, the last selector, the last value (the first atom when it is a float or a symbol) and the
// time. JUCE-free and Pd-free, so Tests/mcp/wire_activity_test.cpp runs it with g++ alone.
//
// One Table per plugdata instance (it lives in McpSession; review 2026-11-06: a process-wide table relied on one
// instance's Pd lock while two instances in a DAW run on their own). Windows() maps each instance (the pd::Instance
// pointer enqueueMessage receives) to its table and counts the open windows: with none open the hook returns after one
// relaxed load. The count's changes call a hook (the bridge turns Pd's message forwarding on and off with it). A window
// closes at its read, or by itself at the first message past its end.
//
// RT cost: with no window armed, one relaxed atomic load per message (`armed()`), nothing else. Armed: a binary search
// over the watched keys; a hit adds one to a counter and copies the value into the slot's preallocated atomics. No
// lock, no allocation, no exceptions. record() runs on the thread that runs Pd (the audio thread, or the message
// thread), always under the Pd lock; arm/disarm/read run on the message or command thread, and arm and disarm take
// the Pd lock too (the caller's), so no record() is in flight while a slot is reset. A key is only compared, never
// dereferenced: a wire deleted during the window simply stops counting (its address reused by a new wire could count
// for it until the read; the window is short).
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

namespace mcp::wire {

constexpr int kMaxWires = 256;
constexpr uint32_t kMinWindowMs = 50;
constexpr uint32_t kMaxWindowMs = 2000;

enum class ValueKind : int { None = 0, Float = 1, Symbol = 2, Other = 3 };

struct Slot {
    void const* key = nullptr;
    std::atomic<uint32_t> count { 0 };
    std::atomic<void const*> lastSelector { nullptr };
    std::atomic<int> lastKind { 0 };
    std::atomic<float> lastFloat { 0.0f };
    std::atomic<void const*> lastSymbol { nullptr };
    std::atomic<uint32_t> lastAtMs { 0 };
};

struct Reading {
    uint32_t count = 0;
    void const* lastSelector = nullptr;
    ValueKind lastKind = ValueKind::None;
    float lastFloat = 0.0f;
    void const* lastSymbol = nullptr;
    uint32_t lastAtMs = 0; // ms after the arm
};

using Clock = uint32_t (*)();
using OpenHook = void (*)(bool anyOpen); // called when the number of open windows changes (any thread; atomics only)
constexpr int kMaxInstances = 32;

class Table;

/** Which instance owns which table, and how many windows are open (process-wide, lock-free). */
struct Windows {
    std::atomic<int> open { 0 };
    std::atomic<OpenHook> hook { nullptr };
    std::array<std::atomic<void const*>, kMaxInstances> owners {};
    std::array<std::atomic<Table*>, kMaxInstances> tables {};
};

inline Windows& windows()
{
    static Windows w;
    return w;
}

inline void windowOpened()
{
    int const n = windows().open.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (auto h = windows().hook.load(std::memory_order_acquire)) h(n > 0);
}

inline void windowClosed()
{
    int const n = windows().open.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (auto h = windows().hook.load(std::memory_order_acquire)) h(n > 0);
}

/** Registers `table` for `owner` (an instance). False when every slot is taken. Message thread, at startup. */
inline bool attach(void const* owner, Table* table)
{
    auto& w = windows();
    for (int i = 0; i < kMaxInstances; ++i) {
        void const* expected = nullptr;
        if (w.owners[static_cast<size_t>(i)].load(std::memory_order_acquire) == owner) {
            w.tables[static_cast<size_t>(i)].store(table, std::memory_order_release);
            return true;
        }
        if (w.owners[static_cast<size_t>(i)].compare_exchange_strong(expected, owner, std::memory_order_acq_rel)) {
            w.tables[static_cast<size_t>(i)].store(table, std::memory_order_release);
            return true;
        }
    }
    return false;
}

/** Forgets `owner`'s table (its session is going away). Under that instance's Pd lock, so no record() is running. */
inline void detach(void const* owner)
{
    auto& w = windows();
    for (int i = 0; i < kMaxInstances; ++i)
        if (w.owners[static_cast<size_t>(i)].load(std::memory_order_acquire) == owner)
            w.tables[static_cast<size_t>(i)].store(nullptr, std::memory_order_release);
}

/** The table of `owner`, or null. Any thread. */
inline Table* tableFor(void const* owner) noexcept
{
    auto& w = windows();
    for (int i = 0; i < kMaxInstances; ++i)
        if (w.owners[static_cast<size_t>(i)].load(std::memory_order_acquire) == owner)
            return w.tables[static_cast<size_t>(i)].load(std::memory_order_acquire);
    return nullptr;
}

class Table {
public:
    /**
     * Opens a window over `keys` (duplicates and nulls dropped, at most kMaxWires kept) for `windowMs` (clamped to
     * 50..2000) from `nowMs`. Returns the number of wires watched. Caller holds the Pd lock.
     */
    int arm(void const* const* keys, int n, uint32_t nowMs, uint32_t windowMs, Clock clock)
    {
        disarm();
        size = 0;
        for (int i = 0; i < n && size < kMaxWires; ++i)
            if (keys[i]) order[static_cast<size_t>(size++)] = keys[i];
        std::sort(order.begin(), order.begin() + size, [](void const* a, void const* b) { return addr(a) < addr(b); });
        size = static_cast<int>(std::unique(order.begin(), order.begin() + size) - order.begin());
        for (int i = 0; i < size; ++i) {
            auto& s = slots[static_cast<size_t>(i)];
            s.key = order[static_cast<size_t>(i)];
            s.count.store(0, std::memory_order_relaxed);
            s.lastSelector.store(nullptr, std::memory_order_relaxed);
            s.lastKind.store(0, std::memory_order_relaxed);
            s.lastFloat.store(0.0f, std::memory_order_relaxed);
            s.lastSymbol.store(nullptr, std::memory_order_relaxed);
            s.lastAtMs.store(0, std::memory_order_relaxed);
        }
        armedAtMs = nowMs;
        windowLenMs = std::clamp(windowMs, kMinWindowMs, kMaxWindowMs);
        now = clock;
        if (size > 0 && clock != nullptr) {
            armedFlag.store(true, std::memory_order_release);
            windowOpened();
        }
        return size;
    }

    /** Closes the window (once); the readings stay until the next arm. Under the Pd lock. */
    void disarm() noexcept
    {
        if (armedFlag.exchange(false, std::memory_order_acq_rel))
            windowClosed();
    }

    ~Table() { disarm(); }

    bool armed() const noexcept { return armedFlag.load(std::memory_order_relaxed); }
    uint32_t windowMs() const { return windowLenMs; }
    int watched() const { return size; }

    /** One message on wire `key`. Pd thread, under the Pd lock. Counts only inside the window. */
    void record(void const* key, void const* selector, ValueKind kind, float f, void const* symbol) noexcept
    {
        if (!armedFlag.load(std::memory_order_acquire)) return;
        int const at = find(key);
        if (at < 0) return;
        uint32_t const elapsed = now() - armedAtMs;
        if (elapsed > windowLenMs) { // past the window: it closes itself (forwarding off); the read comes later
            disarm();
            return;
        }
        auto& s = slots[static_cast<size_t>(at)];
        s.count.store(s.count.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); // one writer (Pd lock)
        s.lastSelector.store(selector, std::memory_order_relaxed);
        s.lastKind.store(static_cast<int>(kind), std::memory_order_relaxed);
        s.lastFloat.store(kind == ValueKind::Float ? f : 0.0f, std::memory_order_relaxed);
        s.lastSymbol.store(kind == ValueKind::Symbol ? symbol : nullptr, std::memory_order_relaxed);
        s.lastAtMs.store(elapsed, std::memory_order_relaxed);
    }

    /** The reading of wire `key` (false when it is not watched). After disarm, under the Pd lock. */
    bool read(void const* key, Reading& out) const
    {
        int const at = find(key);
        if (at < 0) return false;
        auto const& s = slots[static_cast<size_t>(at)];
        out.count = s.count.load(std::memory_order_relaxed);
        out.lastSelector = s.lastSelector.load(std::memory_order_relaxed);
        out.lastKind = static_cast<ValueKind>(s.lastKind.load(std::memory_order_relaxed));
        out.lastFloat = s.lastFloat.load(std::memory_order_relaxed);
        out.lastSymbol = s.lastSymbol.load(std::memory_order_relaxed);
        out.lastAtMs = s.lastAtMs.load(std::memory_order_relaxed);
        return true;
    }

    int find(void const* key) const noexcept
    {
        int lo = 0, hi = size - 1;
        while (lo <= hi) {
            int const mid = (lo + hi) / 2;
            void const* const k = slots[static_cast<size_t>(mid)].key;
            if (k == key) return mid;
            if (addr(k) < addr(key)) lo = mid + 1;
            else hi = mid - 1;
        }
        return -1;
    }

private:
    static uintptr_t addr(void const* p) { return reinterpret_cast<uintptr_t>(p); }

    std::atomic<bool> armedFlag { false };
    int size = 0;
    uint32_t armedAtMs = 0;
    uint32_t windowLenMs = 0;
    Clock now = nullptr;
    std::array<void const*, kMaxWires> order {};
    std::array<Slot, kMaxWires> slots {};
};

} // namespace mcp::wire
