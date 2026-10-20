// Unit tests for mcp::OneShotHandoff (Plan 1 Phase 3, Report F3).
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Pd/MCPBridgeHandoff.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

using namespace std::chrono_literals;
using mcp::OneShotHandoff;

int main()
{
    // 1. Worker finishes in time.
    {
        auto h = std::make_shared<OneShotHandoff>();
        std::thread t([h] { if (h->begin()) { std::this_thread::sleep_for(10ms); h->finish(); } });
        CHECK(h->waitOrAbandon(1000ms) == OneShotHandoff::Result::Finished);
        CHECK(!h->wasLate());
        t.join();
    }

    // 2. Worker never starts before the timeout: abandoned, and a late start is refused.
    {
        auto h = std::make_shared<OneShotHandoff>();
        CHECK(h->waitOrAbandon(20ms) == OneShotHandoff::Result::Abandoned);
        std::atomic<bool> touched { false };
        std::thread t([h, &touched] { if (h->begin()) { touched = true; h->finish(); } });
        t.join();
        CHECK(!touched.load());
    }

    // 3. Worker already running at the timeout: the caller keeps waiting until it
    //    finishes (its stack frame must outlive the worker), and is told it was late.
    {
        auto h = std::make_shared<OneShotHandoff>();
        std::atomic<bool> finished { false };
        std::thread t([h, &finished] {
            if (h->begin()) { std::this_thread::sleep_for(150ms); finished = true; h->finish(); }
        });
        while (!h->hasBegun()) std::this_thread::yield();
        CHECK(h->waitOrAbandon(20ms) == OneShotHandoff::Result::Finished);
        CHECK(finished.load());   // the caller did not return early
        CHECK(h->wasLate());
        t.join();
    }

    // 4. The RAII guard finishes on every exit path.
    {
        auto h = std::make_shared<OneShotHandoff>();
        std::thread t([h] {
            if (!h->begin()) return;
            OneShotHandoff::FinishGuard guard(*h);
            // early exit without calling finish() explicitly
        });
        CHECK(h->waitOrAbandon(1000ms) == OneShotHandoff::Result::Finished);
        t.join();
    }

    if (failures == 0) std::printf("handoff_test: all passed\n");
    else std::printf("handoff_test: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
