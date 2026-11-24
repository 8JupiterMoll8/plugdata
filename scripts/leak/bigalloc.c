// Host leak 2026-11-24: LD_PRELOAD shim that logs WHO makes big allocations (no root, no ptrace).
//
//   gcc -O2 -shared -fPIC -o bigalloc.so bigalloc.c -ldl
//   BIGALLOC_LOG=/path/big.log BIGALLOC_MIN=1048576 LD_PRELOAD=$PWD/bigalloc.so <plugdata ...>
//
// Every malloc/calloc/realloc/aligned alloc of >= BIGALLOC_MIN bytes (default 1 MiB) writes "size thread" and a
// 24-frame backtrace to BIGALLOC_LOG: the first 300 hits in full, then one in 100 (a counter line every 100).
// Symbolise afterwards: the frames are "binary(+offset)"; addr2line -Cfe <binary> <offset>, or read them in gdb.
//
// Watchdog (review 2026-11-24, on by default, BIGALLOC_WATCH=0 turns it off): the incident's signature is the MAIN
// thread at 100 % while RSS climbs ~400 MB/s, which reads as one loop that never returns, not as many repaints. A
// helper thread reads /proc/self/statm twice a second; when RSS grew by more than BIGALLOC_WATCH_MB (default 300)
// within 2 s it signals the main thread (SIGRTMIN+6), whose handler writes "#WATCH" and the main thread's backtrace
// to the same log: the stack of the loop itself, no gdb and no root. At most 8 dumps, one per second. It also
// writes one "#STAT rss=<MB> virt=<MB>" line per second while RSS is above BIGALLOC_STAT_MB (default 1024) (virt far above rss = one container
// growing geometrically; virt close to rss = many fully written buffers, e.g. pixel copies).
// Test instances only: never preload into the owner's app.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

extern void* __libc_malloc(size_t);
extern void* __libc_calloc(size_t, size_t);
extern void* __libc_realloc(void*, size_t);
extern void* __libc_memalign(size_t, size_t);

static int logFd = -1;
static size_t minBytes = 1u << 20;
static atomic_ulong hits;
// initial-exec: a malloc interposer must not use the dynamic TLS model (its first access per thread can itself call
// malloc, which re-enters this shim before `inside` exists).
static __thread int inside __attribute__((tls_model("initial-exec")));
static int watchSignal;
static atomic_int watchDumps;
static pid_t mainTid;

static long statmPages(int field) // 0 = size (virtual), 1 = resident
{
    char buf[128];
    int fd = open("/proc/self/statm", O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = 0;
    char* p = buf;
    long v = 0;
    for (int i = 0; i <= field; ++i)
        v = strtol(p, &p, 10);
    return v;
}

// Runs on the MAIN thread, interrupting whatever it is doing. backtrace() was warmed up in the constructor, and
// backtrace_symbols_fd() does not allocate; good enough for a test tool (a deadlock here costs one run, never data).
static void watchHandler(int sig)
{
    (void)sig;
    if (logFd < 0 || inside)
        return;
    inside = 1;
    static char const head[] = "\n#WATCH main thread while RSS climbs\n";
    (void)!write(logFd, head, sizeof head - 1);
    void* frames[48];
    int depth = backtrace(frames, 48);
    backtrace_symbols_fd(frames, depth, logFd);
    inside = 0;
}

static void* watchThread(void* arg)
{
    (void)arg;
    long const pageKb = sysconf(_SC_PAGESIZE) / 1024;
    char const* limit = getenv("BIGALLOC_WATCH_MB");
    long const limitMb = limit ? strtol(limit, NULL, 10) : 300;
    char const* statFloor = getenv("BIGALLOC_STAT_MB");
    long const statFloorMb = statFloor ? strtol(statFloor, NULL, 10) : 1024;
    long history[4] = { 0, 0, 0, 0 }; // RSS in MB, 0.5 s apart: history[0] is 2 s old
    int tick = 0;
    struct timespec const half = { 0, 500000000 };
    for (;;) {
        nanosleep(&half, NULL);
        long const rssMb = statmPages(1) * pageKb / 1024;
        long const oldMb = history[0];
        memmove(history, history + 1, sizeof history - sizeof history[0]);
        history[3] = rssMb;
        ++tick;
        if (rssMb > statFloorMb && tick % 2 == 0 && logFd >= 0) {
            char line[96];
            int len = snprintf(line, sizeof line, "\n#STAT rss=%ld virt=%ld\n", rssMb, statmPages(0) * pageKb / 1024);
            if (len > 0)
                (void)!write(logFd, line, (size_t)len);
        }
        if (oldMb > 0 && rssMb - oldMb > limitMb && tick % 2 == 0 && atomic_load(&watchDumps) < 8) {
            atomic_fetch_add(&watchDumps, 1);
            syscall(SYS_tgkill, getpid(), mainTid, watchSignal);
        }
    }
    return NULL;
}

__attribute__((constructor)) static void bigallocInit(void)
{
    char const* path = getenv("BIGALLOC_LOG");
    char const* min = getenv("BIGALLOC_MIN");
    if (min)
        minBytes = strtoull(min, NULL, 10);
    logFd = open(path ? path : "/tmp/bigalloc.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    void* warm[2];
    backtrace(warm, 2); // loads libgcc_s now, not inside the first big malloc

    // Only in the process this was meant for (BIGALLOC_COMM, default plugdata): the launcher's helpers (systemd-run,
    // pw-jack) inherit LD_PRELOAD too and must not log or get a watchdog thread. The constructor runs on the main thread.
    char comm[32] = { 0 };
    int fd = open("/proc/self/comm", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, comm, sizeof comm - 1);
        close(fd);
        if (n > 0 && comm[n - 1] == '\n')
            comm[n - 1] = 0;
    }
    char const* want = getenv("BIGALLOC_COMM");
    if (strcmp(comm, want ? want : "plugdata") != 0) {
        if (logFd >= 0)
            close(logFd);
        logFd = -1;
        return;
    }
    char const* watch = getenv("BIGALLOC_WATCH");
    if (watch && watch[0] == '0')
        return;
    mainTid = (pid_t)syscall(SYS_gettid);
    watchSignal = SIGRTMIN + 6;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = watchHandler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(watchSignal, &sa, NULL) != 0)
        return;
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old); // the helper thread takes no signals (it inherits this mask)
    if (pthread_create(&t, &attr, watchThread, NULL) == 0)
        pthread_setname_np(t, "bigalloc-watch");
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    pthread_attr_destroy(&attr);
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
