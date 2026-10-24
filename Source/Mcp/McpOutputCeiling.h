/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/
#pragma once

// The output ceiling and its breaker (P9 Part B, docs/plans/p9-fork-catchup-ceiling-steps.md in the MCP repo,
// "Decisions after review"). JUCE-free; unit-tested by Tests/mcp/output_ceiling_test.cpp.
//
// What it does, on the FINAL output (McpHost::audioBlockFinished, after master gain and PlugData's limiter):
//  - the ceiling: every non-finite sample -> 0, every sample clamped to +-1.0 (0 dBFS). No lookahead and no gain
//    change below 0 dBFS: a signal inside +-1.0 leaves bit-identical. It stops overs; it is NOT hearing protection
//    on its own (a +40 dB mistake becomes a full-scale square), hence
//  - the breaker: when more than kBreakerTripFraction of the output samples were over 0 dBFS across a
//    kBreakerWindowMs window, the output gain ramps down to kBreakerGain (-20 dB) in kBreakerAttackMs; it releases
//    after kBreakerHoldMs without an over (ramp kBreakerReleaseMs) or when the owner resets it.
// Runs only in the standalone app (a plugin's host owns the output) and while the owner setting
// `mcp_bridge_ai_ceiling` is on; whether PlugData's limiter is on or not does not matter (it clips at +-sqrt(2) and
// its threshold goes up to +3 dB).
//
// Threads: process() on the audio thread only (no allocation, no locks: atomics, one smoothed gain, counters);
// setEnabled / setStandalone / requestBreakerReset / read from any thread. Counters are cumulative since process
// start (one relaxed fetch_add per block per counter); readers work in deltas.

#include <atomic>
#include <cmath>
#include <cstdint>

#if defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__
#error "McpOutputCeiling.h needs IEEE non-finite checks: do not build it with -ffinite-math-only / -ffast-math"
#endif

namespace mcp {

class OutputCeiling {
public:
    // The breaker's thresholds (recorded in the P9 step file).
    static constexpr double kBreakerWindowMs = 150.0;    // the window the over fraction is measured across
    static constexpr double kBreakerTripFraction = 0.10; // more than this share of samples over 0 dBFS trips it
    static constexpr float kBreakerGain = 0.1f;          // -20 dB while engaged
    static constexpr double kBreakerAttackMs = 20.0;     // ramp 1.0 -> kBreakerGain
    static constexpr double kBreakerHoldMs = 1000.0;     // released after this long without an over
    static constexpr double kBreakerReleaseMs = 200.0;   // ramp kBreakerGain -> 1.0

    struct Telemetry {
        uint64_t clampedSamples = 0;   // finite samples that arrived over +-1.0 (clamped; attenuated too while engaged)
        uint64_t nonFiniteSamples = 0; // NaN / inf samples written as 0
        uint64_t clampedBlocks = 0;    // audio blocks with at least one of the two
        uint64_t breakerTrips = 0;
        bool breakerEngaged = false;
        int64_t lastClampMs = -1; // the caller's millisecond clock at the last such block; -1 = never
    };

    void setStandalone(bool const isStandalone) noexcept { standalone.store(isStandalone, std::memory_order_relaxed); }
    void setEnabled(bool const on) noexcept { enabled.store(on, std::memory_order_relaxed); }
    bool isEnabled() const noexcept { return enabled.load(std::memory_order_relaxed); }
    bool isStandalone() const noexcept { return standalone.load(std::memory_order_relaxed); }
    bool isActive() const noexcept { return isEnabled() && isStandalone(); }
    void requestBreakerReset() noexcept { resetRequested.store(true, std::memory_order_relaxed); }

    Telemetry read() const noexcept
    {
        Telemetry t;
        t.clampedSamples = clampedSamples.load(std::memory_order_relaxed);
        t.nonFiniteSamples = nonFiniteSamples.load(std::memory_order_relaxed);
        t.clampedBlocks = clampedBlocks.load(std::memory_order_relaxed);
        t.breakerTrips = breakerTrips.load(std::memory_order_relaxed);
        t.breakerEngaged = breakerEngaged.load(std::memory_order_relaxed);
        t.lastClampMs = lastClampMs.load(std::memory_order_relaxed);
        return t;
    }

    // Audio thread. `channels` are the OUTPUT channels only (the caller passes no input-only channel).
    void process(float* const* channels, int const numChannels, int const numSamples, double const sampleRate,
        uint32_t const nowMs) noexcept
    {
        if (!isActive()) {
            if (wasActive) resetBreakerState();
            wasActive = false;
            return;
        }
        if (!wasActive) resetBreakerState();
        wasActive = true;
        if (channels == nullptr || numChannels <= 0 || numSamples <= 0) return;
        if (resetRequested.exchange(false, std::memory_order_relaxed)) resetBreakerState();

        double const rate = sampleRate > 0.0 ? sampleRate : 44100.0;
        float const target = engaged ? kBreakerGain : 1.0f;
        float const step = static_cast<float>((1.0 - kBreakerGain) * 1000.0
            / ((engaged ? kBreakerAttackMs : kBreakerReleaseMs) * rate));

        uint64_t overs = 0, nonFinite = 0;
        if (gain == 1.0f && target == 1.0f) {
            for (int ch = 0; ch < numChannels; ++ch) { // the common case: the clamp alone, gain untouched
                float* d = channels[ch];
                if (d == nullptr) continue;
                for (int i = 0; i < numSamples; ++i)
                    clampSample(d[i], overs, nonFinite);
            }
        } else {
            for (int i = 0; i < numSamples; ++i) { // frame by frame: one gain per frame on every channel
                gain = gain > target ? std::fmax(target, gain - step) : std::fmin(target, gain + step);
                for (int ch = 0; ch < numChannels; ++ch) {
                    float* d = channels[ch];
                    if (d == nullptr) continue;
                    clampSample(d[i], overs, nonFinite);
                    d[i] *= gain;
                }
            }
        }

        updateBreaker(overs, static_cast<uint64_t>(numSamples), static_cast<uint64_t>(numChannels), rate);
        if (overs > 0) clampedSamples.fetch_add(overs, std::memory_order_relaxed);
        if (nonFinite > 0) nonFiniteSamples.fetch_add(nonFinite, std::memory_order_relaxed);
        if (overs > 0 || nonFinite > 0) {
            clampedBlocks.fetch_add(1, std::memory_order_relaxed);
            lastClampMs.store(static_cast<int64_t>(nowMs), std::memory_order_relaxed);
        }
        breakerEngaged.store(engaged, std::memory_order_relaxed);
    }

private:
    // The clamp (review L2), exactly: a NaN fails both comparisons, so it lands in the branch with the infinities.
    static void clampSample(float& x, uint64_t& overs, uint64_t& nonFinite) noexcept
    {
        if (!(x >= -1.f && x <= 1.f)) {
            if (std::isfinite(x)) {
                ++overs;
                x = std::copysign(1.f, x);
            } else {
                ++nonFinite;
                x = 0.f;
            }
        }
    }

    void updateBreaker(uint64_t const overs, uint64_t const frames, uint64_t const numChannels, double const rate) noexcept
    {
        windowFrames += frames;
        windowSamples += frames * numChannels;
        windowOvers += overs;
        framesSinceOver = overs > 0 ? 0 : framesSinceOver + frames;
        if (static_cast<double>(windowFrames) >= kBreakerWindowMs * rate / 1000.0) {
            if (!engaged && static_cast<double>(windowOvers) > kBreakerTripFraction * static_cast<double>(windowSamples)) {
                engaged = true;
                breakerTrips.fetch_add(1, std::memory_order_relaxed);
            }
            windowFrames = windowSamples = windowOvers = 0;
        }
        if (engaged && static_cast<double>(framesSinceOver) >= kBreakerHoldMs * rate / 1000.0)
            engaged = false;
    }

    // Audio thread only. The gain jumps back to 1 only when the ceiling was off (nothing to ramp from); an owner
    // reset releases the breaker and lets the gain ramp up.
    void resetBreakerState() noexcept
    {
        if (!wasActive) gain = 1.0f;
        engaged = false;
        windowFrames = windowSamples = windowOvers = framesSinceOver = 0;
        breakerEngaged.store(false, std::memory_order_relaxed);
    }

    std::atomic<bool> standalone { false };
    std::atomic<bool> enabled { true };
    std::atomic<bool> resetRequested { false };
    std::atomic<uint64_t> clampedSamples { 0 };
    std::atomic<uint64_t> nonFiniteSamples { 0 };
    std::atomic<uint64_t> clampedBlocks { 0 };
    std::atomic<uint64_t> breakerTrips { 0 };
    std::atomic<bool> breakerEngaged { false };
    std::atomic<int64_t> lastClampMs { -1 };
    static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<int64_t>::is_always_lock_free
            && std::atomic<bool>::is_always_lock_free,
        "the ceiling's counters must be lock-free (audio thread)");

    // Audio-thread state.
    bool wasActive = false;
    bool engaged = false;
    float gain = 1.0f;
    uint64_t windowFrames = 0, windowSamples = 0, windowOvers = 0, framesSinceOver = 0;
};

} // namespace mcp
