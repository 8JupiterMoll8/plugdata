/*
 // One-shot handoff between a caller that waits with a timeout and a worker
 // that runs later on another thread (Plan 1 Phase 3, Report F3).
 //
 // The worker lambda may capture the caller's stack by reference ONLY if it
 // follows this protocol, and holds the handoff by shared_ptr:
 //
 //     auto h = std::make_shared<mcp::OneShotHandoff>();
 //     queue([&, h] {
 //         if (!h->begin()) return;              // caller gave up: touch nothing
 //         mcp::OneShotHandoff::FinishGuard g(*h);
 //         ... use caller state ...
 //     });
 //     if (h->waitOrAbandon(2000ms) == Result::Abandoned) { ... } // never ran
 //
 // waitOrAbandon() returns early only when the worker has not begun (and then
 // never will); if the worker is mid-run at the timeout it keeps waiting, so
 // the caller's frame outlives every reference into it.
 //
 // JUCE-free so it can be unit-tested: Tests/mcp/handoff_test.cpp.
 */
#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace mcp {

class OneShotHandoff {
public:
    enum class Result { Finished, Abandoned };

    // Worker: call first. False means the caller abandoned the job; the worker
    // must return without touching any caller state.
    bool begin()
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (state != State::Pending)
            return false;
        state = State::Running;
        return true;
    }

    // Worker: call after the last use of caller state.
    void finish()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            state = State::Done;
        }
        cv.notify_all();
    }

    // Caller: wait up to `timeout`. Abandoned = the worker never began and now
    // never will. Finished = it completed (possibly after the timeout: see wasLate).
    Result waitOrAbandon(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (cv.wait_for(lock, timeout, [this] { return state == State::Done; }))
            return Result::Finished;
        if (state == State::Pending) {
            state = State::Abandoned;
            return Result::Abandoned;
        }
        late = true; // running: its references into our frame are live
        cv.wait(lock, [this] { return state == State::Done; });
        return Result::Finished;
    }

    bool hasBegun() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return state == State::Running || state == State::Done;
    }

    bool wasLate() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return late;
    }

    // Calls finish() on every exit path of the worker.
    struct FinishGuard {
        explicit FinishGuard(OneShotHandoff& h)
            : handoff(h)
        {
        }
        ~FinishGuard() { handoff.finish(); }
        FinishGuard(FinishGuard const&) = delete;
        FinishGuard& operator=(FinishGuard const&) = delete;

    private:
        OneShotHandoff& handoff;
    };

private:
    enum class State { Pending, Running, Done, Abandoned };
    mutable std::mutex mutex;
    std::condition_variable cv;
    State state = State::Pending;
    bool late = false;
};

} // namespace mcp
