/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Realtime guard (Plan 3 P3.4 S6). The MCP audio hooks open a Scope; with the
// guard linked (CMake -DMCP_RT_GUARD=ON adds Tests/mcp/rt_guard_new.cpp, which
// replaces operator new and pthread_mutex_lock), every allocation and every
// BLOCKING lock taken inside a scope is counted. Try-locks are not counted.
// /pd/perf reports the counts (rtAllocs, rtLocks) only in a guard build, so
// normal builds and their wire are unchanged. Without the guard a Scope is two
// thread-local increments.
#pragma once

#include <cstdint>

namespace mcp::rt {

struct Scope {
    Scope();
    ~Scope();
    Scope(Scope const&) = delete;
    Scope& operator=(Scope const&) = delete;
};

// Suspends counting inside a Scope: for the Pd message processing a hook triggers (a sequencer step sent to a
// receiver runs the patch's own objects, e.g. [print]); that is the patch's work, as in any DSP tick.
struct Pause {
    Pause();
    ~Pause();
    Pause(Pause const&) = delete;
    Pause& operator=(Pause const&) = delete;

private:
    int saved;
};

bool inScope();
void noteAlloc();
void noteLock();
uint64_t allocs();
uint64_t locks();
bool guardLinked(); // true only in a -DMCP_RT_GUARD=ON build

} // namespace mcp::rt
