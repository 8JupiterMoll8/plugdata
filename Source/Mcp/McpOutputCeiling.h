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
//    change below 0 dBFS: a signal inside +-1.0 leaves bit-identical while the breaker is idle. It stops overs; it is
//    NOT hearing protection on its own (a +40 dB mistake becomes a full-scale square), hence
//  - the breaker (review round 1: it judges sustained LOUDNESS, not overs): per output channel, a one-pole mean square
//    (time constant kLoudnessTauMs) of the breaker's INPUT (after the clamp, before the breaker gain). When the
//    loudest channel's short-term RMS exceeds kTripRmsDb the output gain ramps down to kBreakerGain (-20 dB) in
//    kBreakerAttackMs; it releases once every channel has stayed under kReleaseRmsDb for kBreakerHoldMs (ramp
//    kBreakerReleaseMs), or when the owner resets it. What trips it is sustained CLIPPED or SQUARE-LIKE full-scale
//    sound: a full-scale square / tanh~ / [clip~ -1 1] blast trips it in ~250 ms whether or not it exceeds +-1.0.
//    Round 2: the trip sits at -2.5 dBFS RMS, so any full-scale waveform whose short-term RMS stays under that (a
//    0 dBFS sine at any frequency, -3.01 dBFS RMS with its one-pole ripple; a saw, triangle, noise, a pulse train under
//    ~55 % duty; a loud master at -4 dBFS RMS) is only clamped, never tripped. The loudness starts afresh on an
//    output channel-count change and on requestLoudnessReset (prepareToPlay).
// Runs only in the standalone app (a plugin's host owns the output) and while the owner setting
// `mcp_bridge_ai_ceiling` is on; whether PlugData's limiter is on or not does not matter (it clips at +-sqrt(2) and
// its threshold goes up to +3 dB).
//
// Threads: process() on the audio thread only (no allocation, no locks: atomics, per-channel doubles, one smoothed
// gain, counters); setEnabled / setStandalone / requestBreakerReset / read from any thread. Counters are cumulative
// since construction (startedAtMs, wall clock); readers work in deltas. clampBuffer() is the clamp alone, for other
// device callbacks (MCPBridge::playVoiceTake).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>

#if defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__
#error "McpOutputCeiling.h needs IEEE non-finite checks: do not build it with -ffinite-math-only / -ffast-math"
#endif

namespace mcp {

class OutputCeiling {
public:
    // The breaker's constants (recorded in the P9 step file, review round 1).
    static constexpr double kLoudnessTauMs = 300.0; // the one-pole mean square's time constant
    static constexpr double kTripRmsDb = -2.5;      // the loudest channel's RMS above this trips it (round 2)
    static constexpr double kReleaseRmsDb = -6.0;   // every channel under this ...
    static constexpr double kBreakerHoldMs = 1000.0; // ... for this long releases it
    static constexpr double kTripMeanSquare = 0.56234132519034907;    // 10^(kTripRmsDb / 10)
    static constexpr double kReleaseMeanSquare = 0.25118864315095796; // 10^(kReleaseRmsDb / 10)
    static constexpr float kBreakerGain = 0.1f;        // -20 dB while engaged
    static constexpr double kBreakerAttackMs = 20.0;   // ramp 1.0 -> kBreakerGain
    static constexpr double kBreakerReleaseMs = 200.0; // ramp kBreakerGain -> 1.0
    static constexpr int kMaxChannels = 64; // loudness slots; channels past the last slot share it (the loudest wins)

    struct Telemetry {
        uint64_t clampedSamples = 0;   // finite samples that arrived over +-1.0 (clamped; attenuated too while engaged)
        uint64_t nonFiniteSamples = 0; // NaN / inf samples written as 0
        uint64_t clampedBlocks = 0;    // audio blocks with at least one of the two
        uint64_t breakerTrips = 0;
        uint64_t breakerReleases = 0;
        bool breakerEngaged = false;
        int64_t lastClampMs = -1; // the caller's millisecond clock at the last such block; -1 = never
        int64_t startedAtMs = 0;  // wall-clock ms when this ceiling (= this PlugData process) started
    };

    OutputCeiling() noexcept
        : startedAtMs(std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
                  .count())
    {
    }

    // The clamp alone (no breaker, no counters), for an output path outside processBlock. Any thread.
    static void clampBuffer(float* const* channels, int const numChannels, int const numSamples) noexcept
    {
        uint64_t overs = 0, nonFinite = 0;
        if (channels == nullptr) return;
        for (int ch = 0; ch < numChannels; ++ch)
            if (float* d = channels[ch])
                for (int i = 0; i < numSamples; ++i)
                    clampSample(d[i], overs, nonFinite);
    }

    void setStandalone(bool const isStandalone) noexcept { standalone.store(isStandalone, std::memory_order_relaxed); }
    void setEnabled(bool const on) noexcept { enabled.store(on, std::memory_order_relaxed); }
    bool isEnabled() const noexcept { return enabled.load(std::memory_order_relaxed); }
    bool isStandalone() const noexcept { return standalone.load(std::memory_order_relaxed); }
    bool isActive() const noexcept { return isEnabled() && isStandalone(); }
    void requestBreakerReset() noexcept { resetRequested.store(true, std::memory_order_relaxed); }
    // Round 2: start the loudness measure afresh (McpHost::prepare calls it on prepareToPlay). Any thread.
    void requestLoudnessReset() noexcept { loudnessResetRequested.store(true, std::memory_order_relaxed); }

    Telemetry read() const noexcept
    {
        Telemetry t;
        t.clampedSamples = clampedSamples.load(std::memory_order_relaxed);
        t.nonFiniteSamples = nonFiniteSamples.load(std::memory_order_relaxed);
        t.clampedBlocks = clampedBlocks.load(std::memory_order_relaxed);
        t.breakerTrips = breakerTrips.load(std::memory_order_relaxed);
        t.breakerReleases = breakerReleases.load(std::memory_order_relaxed);
        t.startedAtMs = startedAtMs;
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
        if (loudnessResetRequested.exchange(false, std::memory_order_relaxed) || numChannels != lastNumChannels)
            clearLoudness();
        lastNumChannels = numChannels;

        double const rate = sampleRate > 0.0 ? sampleRate : 44100.0;
        if (rate != alphaRate) {
            alpha = 1.0 - std::exp(-1000.0 / (kLoudnessTauMs * rate));
            alphaRate = rate;
        }
        float const target = engaged ? kBreakerGain : 1.0f;
        float const step = static_cast<float>((1.0 - kBreakerGain) * 1000.0
            / ((engaged ? kBreakerAttackMs : kBreakerReleaseMs) * rate));
        bool const ramping = !(gain == 1.0f && target == 1.0f);

        // Channel by channel: the clamp, the channel's loudness (of the clamped signal, before the breaker gain), then
        // the breaker gain, whose ramp is the same on every channel (same start, same step).
        uint64_t overs = 0, nonFinite = 0;
        double loudest = 0.0; // the highest mean square any channel reached in this block
        double sharedEnd = 0.0;
        float gainEnd = gain;
        for (int ch = 0; ch < numChannels; ++ch) {
            float* d = channels[ch];
            if (d == nullptr) continue;
            int const slot = std::min(ch, kMaxChannels - 1);
            double ms = meanSquare[slot];
            double peakMs = ms;
            float g = gain;
            for (int i = 0; i < numSamples; ++i) {
                clampSample(d[i], overs, nonFinite);
                double const x = d[i];
                ms += alpha * (x * x - ms);
                peakMs = std::max(peakMs, ms);
                if (ramping) {
                    g = g > target ? std::fmax(target, g - step) : std::fmin(target, g + step);
                    d[i] *= g;
                }
            }
            if (ms < 1e-30) ms = 0.0; // no denormals after long silence
            if (ch < kMaxChannels - 1)
                meanSquare[slot] = ms;
            else
                sharedEnd = std::max(sharedEnd, ms);
            loudest = std::max(loudest, peakMs);
            gainEnd = g;
        }
        if (numChannels >= kMaxChannels) meanSquare[kMaxChannels - 1] = sharedEnd;
        if (ramping) gain = gainEnd;

        updateBreaker(loudest, static_cast<uint64_t>(numSamples), rate);
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

    void updateBreaker(double const loudest, uint64_t const frames, double const rate) noexcept
    {
        if (!engaged) {
            if (loudest > kTripMeanSquare) {
                engaged = true;
                framesCalm = 0;
                breakerTrips.fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }
        framesCalm = loudest < kReleaseMeanSquare ? framesCalm + frames : 0;
        if (static_cast<double>(framesCalm) >= kBreakerHoldMs * rate / 1000.0) {
            engaged = false;
            breakerReleases.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void clearLoudness() noexcept
    {
        for (auto& m : meanSquare)
            m = 0.0;
    }

    // Audio thread only. The gain jumps back to 1 only when the ceiling was off (nothing to ramp from); an owner
    // reset releases the breaker (not counted as a release: the owner did it), lets the gain ramp up and starts the
    // loudness measure afresh.
    void resetBreakerState() noexcept
    {
        if (!wasActive) gain = 1.0f;
        engaged = false;
        framesCalm = 0;
        clearLoudness();
        breakerEngaged.store(false, std::memory_order_relaxed);
    }

    std::atomic<bool> standalone { false };
    std::atomic<bool> enabled { true };
    std::atomic<bool> resetRequested { false };
    std::atomic<bool> loudnessResetRequested { false };
    std::atomic<uint64_t> clampedSamples { 0 };
    std::atomic<uint64_t> nonFiniteSamples { 0 };
    std::atomic<uint64_t> clampedBlocks { 0 };
    std::atomic<uint64_t> breakerTrips { 0 };
    std::atomic<uint64_t> breakerReleases { 0 };
    std::atomic<bool> breakerEngaged { false };
    std::atomic<int64_t> lastClampMs { -1 };
    int64_t const startedAtMs;
    static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<int64_t>::is_always_lock_free
            && std::atomic<bool>::is_always_lock_free,
        "the ceiling's counters must be lock-free (audio thread)");

    // Audio-thread state.
    bool wasActive = false;
    bool engaged = false;
    float gain = 1.0f;
    uint64_t framesCalm = 0;
    int lastNumChannels = 0;
    double meanSquare[kMaxChannels] = {};
    double alpha = 0.0, alphaRate = 0.0;
};

} // namespace mcp
