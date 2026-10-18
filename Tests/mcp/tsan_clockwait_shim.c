/*
 * LD_PRELOAD shim for the TSan runs only (Plan 3 P3.4 S11). GCC 11's libtsan has no interceptor for
 * pthread_cond_clockwait, which libstdc++ 11 uses for std::condition_variable::wait_for/wait_until on glibc >= 2.30.
 * TSan then never sees the wait release and re-take its mutex, and reports phantom "double lock of a mutex" and
 * data races on everything handed over through a condition variable (mcp::OneShotHandoff, McpHost::onMessageThread,
 * juce::WaitableEvent). This forwards the call to pthread_cond_timedwait, which TSan intercepts, converting the
 * deadline to CLOCK_REALTIME (the clock a default-initialised condvar waits on).
 *
 *   gcc -O2 -shared -fPIC -o tsan_clockwait_shim.so Tests/mcp/tsan_clockwait_shim.c
 *   LD_PRELOAD=$PWD/tsan_clockwait_shim.so <tsan binary>
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <time.h>

int pthread_cond_clockwait(pthread_cond_t* cond, pthread_mutex_t* mutex, clockid_t clock, const struct timespec* abstime)
{
    struct timespec deadline = *abstime;
    if (clock != CLOCK_REALTIME) {
        struct timespec now_clock, now_real;
        clock_gettime(clock, &now_clock);
        clock_gettime(CLOCK_REALTIME, &now_real);
        long long ns = (abstime->tv_sec - now_clock.tv_sec) * 1000000000LL + (abstime->tv_nsec - now_clock.tv_nsec);
        if (ns < 0)
            ns = 0;
        ns += now_real.tv_nsec;
        deadline.tv_sec = now_real.tv_sec + ns / 1000000000LL;
        deadline.tv_nsec = ns % 1000000000LL;
    }
    return pthread_cond_timedwait(cond, mutex, &deadline);
}
