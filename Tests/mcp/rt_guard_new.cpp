// Realtime guard hooks (Plan 3 P3.4 S6), linked into plugdata_standalone only with -DMCP_RT_GUARD=ON.
// Counts every operator new and every blocking pthread_mutex_lock made inside an mcp::rt::Scope (the MCP
// audio hooks); /pd/perf reports the totals as rtAllocs / rtLocks. Never part of a release build.
#include <cstdlib>
#include <new>
#include <pthread.h>

#include "../../Source/Mcp/McpRtGuard.h"

extern "C" bool mcp_rt_guard_linked() { return true; }

void* operator new(std::size_t n)
{
    mcp::rt::noteAlloc();
    if (void* p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void* operator new(std::size_t n, std::nothrow_t const&) noexcept
{
    mcp::rt::noteAlloc();
    return std::malloc(n ? n : 1);
}
void* operator new[](std::size_t n, std::nothrow_t const& t) noexcept { return operator new(n, t); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::nothrow_t const&) noexcept { std::free(p); }
void operator delete[](void* p, std::nothrow_t const&) noexcept { std::free(p); }

// The executable's definition wins over libc's for every caller (libstdc++, JUCE). glibc exports the
// implementation as __pthread_mutex_lock, but only as a non-default symbol version (@GLIBC_2.2.5), so it
// is bound with .symver (no dlsym, which itself takes a lock).
extern "C" int mcp_real_pthread_mutex_lock(pthread_mutex_t* m);
__asm__(".symver mcp_real_pthread_mutex_lock, __pthread_mutex_lock@GLIBC_2.2.5");
extern "C" int pthread_mutex_lock(pthread_mutex_t* m)
{
    mcp::rt::noteLock();
    return mcp_real_pthread_mutex_lock(m);
}
