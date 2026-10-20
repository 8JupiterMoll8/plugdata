// Unit tests for mcp::SerialWorker (Plan 3 P3.4 S1: the bridge's command worker and reply queue).
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Pd/MCPBridgeWorker.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

using namespace std::chrono_literals;
using mcp::SerialWorker;

namespace {
// Polls `done` for up to `ms`; true once it holds.
template<typename F>
bool eventually(F done, int ms = 2000)
{
    auto const until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        if (done())
            return true;
        std::this_thread::sleep_for(1ms);
    }
    return done();
}
} // namespace

int main()
{
    // 1. Items run in post order, on one thread that is not the poster's.
    {
        std::mutex m;
        std::vector<int> seen;
        std::thread::id runner;
        SerialWorker<int> w;
        w.start([&](int v) {
            std::lock_guard<std::mutex> l(m);
            seen.push_back(v);
            runner = std::this_thread::get_id();
        });
        for (int i = 0; i < 100; ++i)
            CHECK(w.post(i));
        CHECK(eventually([&] { std::lock_guard<std::mutex> l(m); return seen.size() == 100; }));
        std::lock_guard<std::mutex> l(m);
        bool ordered = true;
        for (int i = 0; i < 100 && i < static_cast<int>(seen.size()); ++i)
            ordered = ordered && seen[i] == i;
        CHECK(ordered);
        CHECK(runner != std::this_thread::get_id());
    }

    // 2. post() never waits for the item to run (the network thread only enqueues).
    {
        std::atomic<bool> release { false };
        std::atomic<int> ran { 0 };
        SerialWorker<int> w;
        w.start([&](int) {
            while (!release.load())
                std::this_thread::sleep_for(1ms);
            ran.fetch_add(1);
        });
        auto const t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 5; ++i)
            w.post(i);
        auto const took = std::chrono::steady_clock::now() - t0;
        CHECK(took < 50ms);
        CHECK(ran.load() == 0);
        CHECK(w.pending() >= 4); // one may already be running
        release = true;
        CHECK(eventually([&] { return ran.load() == 5; }));
    }

    // 3. stop() lets the running item finish, drops the rest, joins; posts after stop are refused.
    {
        std::atomic<bool> inside { false };
        std::atomic<bool> release { false };
        std::atomic<int> ran { 0 };
        SerialWorker<int> w;
        w.start([&](int) {
            inside = true;
            while (!release.load())
                std::this_thread::sleep_for(1ms);
            ran.fetch_add(1);
        });
        w.post(1);
        w.post(2);
        w.post(3);
        CHECK(eventually([&] { return inside.load(); }));
        std::thread stopper([&] { w.stop(); });
        std::this_thread::sleep_for(20ms);
        release = true;
        stopper.join();
        CHECK(ran.load() == 1);
        CHECK(!w.running());
        CHECK(!w.post(4));
        CHECK(w.pending() == 0);
    }

    // 4. start() after stop() runs again (bridge restart and port rebind).
    {
        std::atomic<int> ran { 0 };
        SerialWorker<int> w;
        w.start([&](int) { ran.fetch_add(1); });
        w.post(1);
        CHECK(eventually([&] { return ran.load() == 1; }));
        w.stop();
        w.start([&](int v) { ran.fetch_add(v); });
        w.post(10);
        CHECK(eventually([&] { return ran.load() == 11; }));
    }

    // 5. stop() from inside the worker thread does not deadlock (a handler that stops the bridge).
    {
        SerialWorker<int> w;
        std::atomic<bool> stopped { false };
        w.start([&](int) { w.stop(); stopped = true; });
        w.post(1);
        CHECK(eventually([&] { return stopped.load(); }));
        CHECK(!w.post(2));
    }

    // 6. Destructor stops and joins (no std::terminate from a joinable thread).
    {
        std::atomic<int> ran { 0 };
        {
            SerialWorker<std::string> w;
            w.start([&](std::string const&) { ran.fetch_add(1); });
            w.post("a");
            CHECK(eventually([&] { return ran.load() == 1; }));
        }
        CHECK(ran.load() == 1);
    }

    // 7. isWorkerThread() is true only on the worker.
    {
        std::atomic<int> onWorker { -1 };
        SerialWorker<int> w;
        w.start([&](int) { onWorker = w.isWorkerThread() ? 1 : 0; });
        CHECK(!w.isWorkerThread());
        w.post(1);
        CHECK(eventually([&] { return onWorker.load() != -1; }));
        CHECK(onWorker.load() == 1);
    }

    if (failures == 0)
        std::printf("worker_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
