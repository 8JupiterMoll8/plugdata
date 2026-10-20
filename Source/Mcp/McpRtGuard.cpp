/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// See McpRtGuard.h. The counting hooks live in Tests/mcp/rt_guard_new.cpp.

#include <atomic>

#include "Mcp/McpRtGuard.h"

// Defined (strong) by Tests/mcp/rt_guard_new.cpp in a guard build; this weak one answers false otherwise.
extern "C" __attribute__((weak)) bool mcp_rt_guard_linked() { return false; }

namespace mcp::rt {
namespace {
thread_local int depth = 0;
std::atomic<uint64_t> allocCount { 0 };
std::atomic<uint64_t> lockCount { 0 };
} // namespace

Scope::Scope() { ++depth; }
Scope::~Scope() { --depth; }
Pause::Pause() : saved(depth) { depth = 0; }
Pause::~Pause() { depth = saved; }

bool inScope() { return depth > 0; }
void noteAlloc()
{
    if (depth > 0)
        allocCount.fetch_add(1, std::memory_order_relaxed);
}
void noteLock()
{
    if (depth > 0)
        lockCount.fetch_add(1, std::memory_order_relaxed);
}
uint64_t allocs() { return allocCount.load(std::memory_order_relaxed); }
uint64_t locks() { return lockCount.load(std::memory_order_relaxed); }
bool guardLinked() { return mcp_rt_guard_linked(); }

} // namespace mcp::rt
