/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/
#pragma once

// The output ceiling's console lines (P9 review rounds 1 and 2): each breaker trip and release is said on the PlugData
// console, from a message-thread timer (McpHost::pollOutputCeiling), never the audio thread. Rate limit: at most one
// line per kind (trip, release) per kMinGapMs; what happened in between is said as one summary,
// "Output ceiling: breaker tripped N times in the last Xs" (a single held-back event: "1 time in the last Xs"). JUCE-free; unit-tested by Tests/mcp/ceiling_console_test.cpp.

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace mcp {

class CeilingConsole {
public:
    static constexpr int64_t kMinGapMs = 10000;
    static constexpr char const* kTripLine = "Output ceiling: sustained full-scale sound, turned down 20 dB";
    static constexpr char const* kReleaseLine = "Output ceiling: level back to normal, breaker released";

    // The counters as they are now: nothing before this is said.
    void start(uint64_t const trips, uint64_t const releases, int64_t const nowMs)
    {
        trip = Kind { trips };
        release = Kind { releases };
        (void)nowMs;
    }

    // The lines to post now for the counters as they are now (cumulative breakerTrips / breakerReleases).
    std::vector<std::string> poll(uint64_t const trips, uint64_t const releases, int64_t const nowMs)
    {
        std::vector<std::string> out;
        step(trip, trips, nowMs, kTripLine, "tripped", out);
        step(release, releases, nowMs, kReleaseLine, "released", out);
        return out;
    }

private:
    struct Kind {
        uint64_t seen = 0;
        uint64_t pending = 0;
        int64_t lastPostMs = 0;
        bool posted = false;
        bool heldBack = false; // something pending could not be said when it happened
    };

    // nowMs: a steady millisecond clock (McpHost passes juce::Time::getMillisecondCounterHiRes). A clock that goes
    // back anyway rebases the gap at the jump, so nothing waits longer than kMinGapMs from it.
    static void step(Kind& k, uint64_t const count, int64_t const nowMs, char const* single, char const* verb,
        std::vector<std::string>& out)
    {
        if (count >= k.seen)
            k.pending += count - k.seen;
        k.seen = count; // a counter that fell is a new ceiling: a new baseline
        if (k.posted && nowMs < k.lastPostMs)
            k.lastPostMs = nowMs;
        if (k.pending == 0) return;
        if (!k.posted) {
            out.emplace_back(single);
            --k.pending;
            k.heldBack = k.pending > 0;
        } else if (nowMs - k.lastPostMs >= kMinGapMs) {
            if (k.pending == 1 && !k.heldBack) {
                out.emplace_back(single); // it happened just now: the present tense
            } else {
                // round 3: what was held back is told as past events, a single one too
                auto const secs = static_cast<long long>(std::llround(static_cast<double>(nowMs - k.lastPostMs) / 1000.0));
                out.emplace_back("Output ceiling: breaker " + std::string(verb) + " " + std::to_string(k.pending)
                    + (k.pending == 1 ? " time" : " times") + " in the last " + std::to_string(secs) + "s");
            }
            k.pending = 0;
            k.heldBack = false;
        } else {
            k.heldBack = true;
            return;
        }
        k.posted = true;
        k.lastPostMs = nowMs;
    }

    Kind trip, release;
};

} // namespace mcp
