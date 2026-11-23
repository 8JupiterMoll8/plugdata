/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/
#pragma once

// When does NVGSurface (GL) record a frame? (Lag fix, Fable's diagnosis 2026-11-23, fix #2; mcp-server
// docs/review/plugdata-lag-diagnosis-2026-11-23.md.) JUCE-free, on a clock the caller passes in
// (Tests/mcp/frame_pacer_test.cpp).
//
// Before: every invalidation scheduled an AsyncUpdater and recordFrame ran for each one, with no limit. The render
// thread spends most of its time inside swapBuffers; a frame published while the previous one is still pending
// replaces it (the work is thrown away), and recordFrame folded the thrown-away frame's damage back into the new
// one, so while the render thread lagged the redrawn area only grew until the whole window (toolbar, tabs, sidebar)
// was redrawn for every small change (the ratchet).
//
// Now (message thread only):
//  - invalidate() only says "dirty" (NVGSurface keeps the damage rects). It returns the delay after which the owner
//    must call tick(): 0 for the first invalidation after a quiet interval (the frame is recorded at the end of the
//    message-loop turn, no added latency), the rest of the interval otherwise, or -1 when a wake is already armed.
//  - tick() records at most one frame per interval (16 ms, about 60 per second). The invalidations of a burst are
//    one frame: the damage is the union of all of them, and the last damage of the burst is recorded one interval
//    after the previous frame (trailing edge).
//  - While the render thread still holds the previous frame (hasPendingFrame) tick() records NOTHING and keeps the
//    damage, wakes the render thread once per episode (nudgeRender: its wake-up may have been lost) and looks again
//    after retryMs. No frame is ever published over a pending one, so there is nothing to fold back and nothing to
//    ratchet.
//  - invalidateNow() / flushNow(): an urgent frame (first frame, a render thread that needs a full frame, readback,
//    screenshot) is recorded at once, over a pending frame if there is one (overwritePending: the caller then keeps
//    the old behaviour and folds the replaced frame's damage back in, once).
//  - A frame that stays pending for stuckMs (a render thread that cannot take it: no context, a failed framebuffer)
//    is replaced like an urgent one, once per stuckMs, so a frame the render thread refuses can never freeze the UI.

#include <algorithm>
#include <cstdint>

namespace mcp {

class FramePacer {
public:
    struct Config {
        double intervalMs = 16.0; // the least time between two recorded frames
        double retryMs = 4.0;     // while the render thread holds the last frame, look again after this
        double stuckMs = 250.0;   // a frame pending for this long is treated as stuck and replaced
    };

    enum class Action {
        Idle,   // nothing is dirty
        Record, // record a frame now
        Wait    // do not record; call tick() again after nextWakeMs
    };

    struct Decision {
        Action action = Action::Idle;
        double nextWakeMs = -1.0;      // delay until the next tick; < 0 = no wake needed
        bool overwritePending = false; // Record only: the frame still pending is replaced (urgent or stuck)
        bool nudgeRender = false;      // Wait only: the render thread holds a frame, wake it
    };

    // Running totals, for the frame log (PLUGDATA_NVG_FRAME_LOG).
    struct Stats {
        std::uint64_t invalidates = 0;
        std::uint64_t records = 0;
        std::uint64_t skippedPending = 0; // ticks that found the last frame still pending and recorded nothing
        std::uint64_t overwrites = 0;     // records that replaced a pending frame (urgent or stuck)
    };

    FramePacer() = default;
    explicit FramePacer(Config const& c)
        : config(c)
    {
    }

    // Damage arrived. Returns the delay (ms) after which the owner must call tick(), or -1 when a wake is already armed.
    double invalidate(double const nowMs)
    {
        ++totals.invalidates;
        dirty = true;
        if (wakeArmed)
            return -1.0;
        wakeArmed = true;
        return std::max(0.0, lastRecordMs + config.intervalMs - nowMs);
    }

    // An urgent frame: always (re)arms the wake for now, whatever was armed before.
    double invalidateNow(double const /*nowMs*/)
    {
        ++totals.invalidates;
        dirty = true;
        urgent = true;
        wakeArmed = true;
        return 0.0;
    }

    // The armed wake fired (timer or async update). framePending: the render thread has not taken the last frame.
    Decision tick(double const nowMs, bool const framePending)
    {
        wakeArmed = false;
        Decision d;

        if (!dirty) {
            pendingSinceMs = -1.0;
            return d; // Idle
        }

        if (urgent)
            return record(nowMs, framePending);

        double const sinceLast = nowMs - lastRecordMs;
        if (sinceLast < config.intervalMs) {
            wakeArmed = true;
            d.action = Action::Wait;
            d.nextWakeMs = config.intervalMs - sinceLast;
            return d;
        }

        if (framePending) {
            // The first skip of an episode wakes the render thread (a lost wake-up would leave the frame unread); the
            // following ones do not, each wake costs the render thread a pass and a swap.
            bool const firstSkip = pendingSinceMs < 0.0;
            if (firstSkip)
                pendingSinceMs = nowMs;
            if (nowMs - pendingSinceMs >= config.stuckMs)
                return record(nowMs, true);

            ++totals.skippedPending;
            wakeArmed = true;
            d.action = Action::Wait;
            d.nextWakeMs = config.retryMs;
            d.nudgeRender = firstSkip;
            return d;
        }

        return record(nowMs, false);
    }

    // The synchronous path (screenshot, readback): record now if something is dirty, over a pending frame if needed.
    Decision flushNow(double const nowMs, bool const framePending)
    {
        if (!dirty)
            return {};
        urgent = true;
        return tick(nowMs, framePending);
    }

    bool isDirty() const { return dirty; }
    Stats const& stats() const { return totals; }
    Config const& getConfig() const { return config; }

private:
    Decision record(double const nowMs, bool const framePending)
    {
        dirty = false;
        urgent = false;
        lastRecordMs = nowMs;
        pendingSinceMs = -1.0;
        ++totals.records;

        Decision d;
        d.action = Action::Record;
        d.overwritePending = framePending;
        if (framePending)
            ++totals.overwrites;
        return d;
    }

    Config config;
    Stats totals;
    bool dirty = false;
    bool urgent = false;
    bool wakeArmed = false;
    double lastRecordMs = -1.0e12; // long ago: the first invalidation records at once
    double pendingSinceMs = -1.0;  // when a tick first saw the same pending frame; < 0 = not waiting on one
};

} // namespace mcp
