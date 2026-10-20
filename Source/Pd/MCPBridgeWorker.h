/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// One thread that runs posted items in post order (Plan 3 P3.4 S1).
//
// The bridge uses two: the command worker (the OSC network thread only posts
// the decoded message; handlers run here, serially, so their order is the
// arrival order as before) and the reply queue (owns the OSCSender; any thread
// posts a reply, this thread sends it).
//
//   mcp::SerialWorker<Msg> w;
//   w.start([](Msg m) { ... });   // the runner, called on the worker thread
//   w.post(msg);                  // any thread; never waits for the item
//   w.stop();                     // the running item finishes, the rest is dropped
//
// stop() called from the worker itself (a handler that stops the bridge) does
// not join; the thread ends after that item and is joined by the next start()
// or the destructor. JUCE-free so it can be unit-tested: Tests/mcp/worker_test.cpp.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace mcp {

template<typename Item>
class SerialWorker {
public:
    using Runner = std::function<void(Item)>;

    SerialWorker() = default;
    SerialWorker(SerialWorker const&) = delete;
    SerialWorker& operator=(SerialWorker const&) = delete;
    ~SerialWorker()
    {
        stop();
        joinIfFinished();
    }

    void start(Runner newRunner)
    {
        joinIfFinished();
        std::lock_guard<std::mutex> lock(mutex);
        if (accepting)
            return;
        runner = std::move(newRunner);
        accepting = true;
        thread = std::thread([this] { loop(); });
        workerId.store(thread.get_id());
    }

    // False (and the item is dropped) when the worker is not running.
    bool post(Item item)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!accepting)
                return false;
            queue.push_back(std::move(item));
        }
        cv.notify_one();
        return true;
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!accepting && !thread.joinable())
                return;
            accepting = false;
            queue.clear();
        }
        cv.notify_all();
        if (isWorkerThread())
            return; // joined later: a thread cannot join itself
        if (thread.joinable())
            thread.join();
    }

    bool running() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return accepting;
    }

    size_t pending() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return queue.size();
    }

    bool isWorkerThread() const
    {
        return std::this_thread::get_id() == workerId.load();
    }

private:
    void loop()
    {
        for (;;) {
            std::optional<Item> item; // Item need not be default-constructible (juce::OSCMessage)
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [this] { return !accepting || !queue.empty(); });
                if (!accepting)
                    return;
                item.emplace(std::move(queue.front()));
                queue.pop_front();
            }
            runner(std::move(*item));
        }
    }

    // A thread that stopped itself (or was stopped) is joined before reuse.
    void joinIfFinished()
    {
        if (thread.joinable() && !isWorkerThread()) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (accepting)
                    return;
            }
            thread.join();
        }
    }

    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<Item> queue;
    Runner runner;
    std::thread thread;
    std::atomic<std::thread::id> workerId {};
    bool accepting = false;
};

} // namespace mcp
