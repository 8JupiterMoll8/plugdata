/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// See McpAudit.h. Measurement only: nothing here changes what runs or when.

#include <juce_events/juce_events.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "Mcp/McpAudit.h"

extern "C" {
void setup_lock(void const* lock, void (*lock_func)(void*), void (*unlock_func)(void*));
}

namespace mcp::audit {
namespace {

enum Role { Other = 0, Net, Audio, Msg, RoleCount };

struct ThreadState {
    int depth = 0;
    long long heldSince = 0;
    int role = -1; // -1 = not decided yet
};

thread_local ThreadState tls;

struct RoleStats {
    std::atomic<long long> holds { 0 };
    std::atomic<long long> maxHold { 0 };
    std::atomic<long long> sumHold { 0 };
    std::atomic<long long> maxWait { 0 };
};

RoleStats stats[RoleCount];

long long nowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void raiseMax(std::atomic<long long>& slot, long long v)
{
    long long prev = slot.load(std::memory_order_relaxed);
    while (v > prev && !slot.compare_exchange_weak(prev, v, std::memory_order_relaxed)) { }
}

int roleOfThisThread()
{
    if (tls.role < 0) {
        auto* mm = juce::MessageManager::getInstanceWithoutCreating();
        if (mm && mm->isThisTheMessageThread())
            tls.role = Msg;
        else
            return Other; // undecided: a later mark may still tag it
    }
    return tls.role;
}

void lockFn(void* lock)
{
    auto* cs = static_cast<juce::CriticalSection*>(lock);
    long long const t0 = nowUs();
    cs->enter();
    if (tls.depth++ == 0) {
        long long const t1 = nowUs();
        tls.heldSince = t1;
        raiseMax(stats[roleOfThisThread()].maxWait, t1 - t0);
    }
}

void unlockFn(void* lock)
{
    if (tls.depth > 0 && --tls.depth == 0) {
        long long const held = nowUs() - tls.heldSince;
        auto& s = stats[roleOfThisThread()];
        s.holds.fetch_add(1, std::memory_order_relaxed);
        s.sumHold.fetch_add(held, std::memory_order_relaxed);
        raiseMax(s.maxHold, held);
    }
    static_cast<juce::CriticalSection*>(lock)->exit();
}

void resetStats()
{
    for (auto& s : stats) {
        s.holds.store(0, std::memory_order_relaxed);
        s.maxHold.store(0, std::memory_order_relaxed);
        s.sumHold.store(0, std::memory_order_relaxed);
        s.maxWait.store(0, std::memory_order_relaxed);
    }
}

thread_local std::string currentAddress;
thread_local long long commandStart = 0;

} // namespace

bool enabled()
{
    static bool const on = [] {
        char const* v = std::getenv("PLUGDATA_MCP_AUDIT");
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    return on;
}

void installLock(void const* criticalSection)
{
    if (!enabled() || criticalSection == nullptr)
        return;
    setup_lock(criticalSection, lockFn, unlockFn);
    std::fprintf(stderr, "MCP_AUDIT installed\n");
    std::fflush(stderr);
}

void commandBegin(char const* address)
{
    if (!enabled())
        return;
    tls.role = Net;
    currentAddress = address ? address : "";
    resetStats();
    commandStart = nowUs();
    std::fprintf(stderr, "MCP_AUDIT begin %s\n", currentAddress.c_str());
    std::fflush(stderr);
}

void commandEnd()
{
    if (!enabled())
        return;
    auto const& n = stats[Net];
    auto const& a = stats[Audio];
    auto const& m = stats[Msg];
    auto const& o = stats[Other];
    std::fprintf(stderr,
        "MCP_AUDIT end %s us=%lld net=%lld/%lld/%lld audioWaitMax=%lld audioHoldMax=%lld msg=%lld/%lld other=%lld/%lld\n",
        currentAddress.c_str(), nowUs() - commandStart,
        n.holds.load(), n.maxHold.load(), n.sumHold.load(),
        a.maxWait.load(), a.maxHold.load(),
        m.holds.load(), m.maxHold.load(),
        o.holds.load(), o.maxHold.load());
    std::fflush(stderr);
}

void markAudioThread()
{
    if (enabled())
        tls.role = Audio;
}

} // namespace mcp::audit
