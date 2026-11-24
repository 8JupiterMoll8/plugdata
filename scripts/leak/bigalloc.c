// Host leak 2026-11-24: LD_PRELOAD shim that logs WHO makes big allocations (no root, no ptrace).
//
//   gcc -O2 -shared -fPIC -o bigalloc.so bigalloc.c -ldl
//   BIGALLOC_LOG=/path/big.log BIGALLOC_MIN=1048576 LD_PRELOAD=$PWD/bigalloc.so <plugdata ...>
//
// Every malloc/calloc/realloc/aligned alloc of >= BIGALLOC_MIN bytes (default 1 MiB) writes "size thread" and a
// 24-frame backtrace to BIGALLOC_LOG: the first 300 hits in full, then one in 100 (a counter line every 100).
// Symbolise afterwards: the frames are "binary(+offset)"; addr2line -Cfe <binary> <offset>, or read them in gdb.
// Test instances only: never preload into the owner's app.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

extern void* __libc_malloc(size_t);
extern void* __libc_calloc(size_t, size_t);
extern void* __libc_realloc(void*, size_t);
extern void* __libc_memalign(size_t, size_t);

static int logFd = -1;
static size_t minBytes = 1u << 20;
static atomic_ulong hits;
static __thread int inside;

__attribute__((constructor)) static void bigallocInit(void)
{
    char const* path = getenv("BIGALLOC_LOG");
    char const* min = getenv("BIGALLOC_MIN");
    if (min)
        minBytes = strtoull(min, NULL, 10);
    logFd = open(path ? path : "/tmp/bigalloc.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    void* warm[2];
    backtrace(warm, 2); // loads libgcc_s now, not inside the first big malloc
}

static void note(char const* what, size_t size)
{
    if (logFd < 0 || size < minBytes || inside)
        return;
    inside = 1;
    unsigned long n = atomic_fetch_add(&hits, 1) + 1;
    if (n <= 300 || n % 100 == 0) {
        char head[160];
        int len = snprintf(head, sizeof head, "\n#%lu %s %zu bytes tid=%ld\n", n, what, size, (long)syscall(SYS_gettid));
        if (len > 0)
            (void)!write(logFd, head, (size_t)len);
        if (n <= 300) {
            void* frames[24];
            int depth = backtrace(frames, 24);
            backtrace_symbols_fd(frames, depth, logFd);
        }
    }
    inside = 0;
}

void* malloc(size_t size) { note("malloc", size); return __libc_malloc(size); }
void* calloc(size_t n, size_t size) { note("calloc", n * size); return __libc_calloc(n, size); }
void* realloc(void* p, size_t size) { note("realloc", size); return __libc_realloc(p, size); }
void* memalign(size_t a, size_t size) { note("memalign", size); return __libc_memalign(a, size); }
void* aligned_alloc(size_t a, size_t size) { note("aligned_alloc", size); return __libc_memalign(a, size); }
int posix_memalign(void** out, size_t a, size_t size)
{
    note("posix_memalign", size);
    void* p = __libc_memalign(a, size);
    if (!p)
        return 12;
    *out = p;
    return 0;
}
