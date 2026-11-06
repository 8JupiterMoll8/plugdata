/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// V1 AC taps: the level of what can be HEARD, next to the raw peak/RMS. A [phasor~] through a [lop~] with its cutoff
// at 0 puts out pure DC; every meter read it as "-22 dBFS, audible" while the artist heard nothing (2026-11-06). Each
// tap now also runs a one-pole DC blocker and reports the AC level (what is left above ~10 Hz), the DC level (the
// mean of the input) and `tone` (AC RMS above -60 dBFS).
//
// JUCE-free and Pd-free, so Tests/mcp/ac_level_test.cpp runs it with g++ alone. Realtime-safe: no allocation, no
// locks, no exceptions; every call is O(samples). The OUTPUT is never touched: the blocker reads samples and keeps its
// own state. Non-finite samples are skipped (they neither count nor enter the blocker state, so one NaN cannot poison
// a tap for the rest of the window), the way the armed taps skip them. Denormals: the blocker state is flushed to 0
// below 1e-20 (a decaying tail would otherwise sit in denormal range for seconds).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mcp::ac {

constexpr float kCornerHz = 10.0f;        // the blocker's -3 dB corner: below every audible fundamental
constexpr float kToneFloorDb = -60.0f;    // AC RMS above this = a tone someone can hear
constexpr float kFloorDb = -100.0f;       // the bridge's level for "nothing" (as peakToDb)
constexpr float kDenormalFlush = 1.0e-20f;
constexpr double kDefaultRate = 48000.0;

/** The pole of the blocker for `sampleRate` (R = exp(-2 pi fc / fs)); a missing or absurd rate uses 48 kHz. */
inline float coefficientFor(double sampleRate)
{
    if (!(sampleRate >= 1000.0 && sampleRate <= 1.0e6))
        sampleRate = kDefaultRate;
    double const r = std::exp(-2.0 * 3.14159265358979323846 * static_cast<double>(kCornerHz) / sampleRate);
    return static_cast<float>(r);
}

/** A linear amplitude as dBFS; anything under 1e-7 (-140 dBFS) is kFloorDb. */
inline float ampToDb(double amp)
{
    return (amp > 1.0e-7 && std::isfinite(amp)) ? static_cast<float>(20.0 * std::log10(amp)) : kFloorDb;
}

/**
 * One-pole DC blocker, y[n] = x[n] - x[n-1] + R * y[n-1]. A fresh blocker is primed with an estimate of the input's DC
 * (accumulate() uses the first block's mean) as if that level had always been there, so a constant input gives exactly
 * 0 from the first sample (no step transient that a peak would read as sound). A tap that opens in the middle of a
 * tone primes slightly off the tone's true mean; that offset decays with the ~16 ms time constant and can lift acPeak
 * in the first ~50 ms, while acRms (and so `tone`) moves by a fraction of a dB.
 */
struct DcBlocker {
    float r = coefficientFor(kDefaultRate);
    float x1 = 0.0f;
    float y1 = 0.0f;
    bool primed = false;

    void reset(float coefficient)
    {
        r = (coefficient > 0.0f && coefficient < 1.0f) ? coefficient : coefficientFor(kDefaultRate);
        x1 = 0.0f;
        y1 = 0.0f;
        primed = false;
    }

    void prime(float dcEstimate)
    {
        primed = true;
        x1 = dcEstimate;
        y1 = 0.0f;
    }

    /** The AC part of a FINITE sample (callers skip non-finite ones). Unprimed, it primes with this sample. */
    float process(float x)
    {
        if (!primed)
            prime(x);
        float y = x - x1 + r * y1;
        x1 = x;
        if (std::fabs(y) < kDenormalFlush)
            y = 0.0f;
        y1 = y;
        return y;
    }
};

/**
 * What one tap accumulated: the AC part's sum of squares and peak, the signed sum of the input (its mean is the DC
 * level) and the finite samples counted.
 */
struct AcStats {
    double acSumSq = 0.0;
    double sum = 0.0;
    float acPeak = 0.0f;
    uint64_t count = 0;

    void add(AcStats const& other)
    {
        acSumSq += other.acSumSq;
        sum += other.sum;
        if (other.acPeak > acPeak) acPeak = other.acPeak;
        count += other.count;
    }
};

/** Runs `n` samples through `blocker` and adds them to `stats`. Reads `vec` only; skips NaN/inf. */
inline void accumulate(DcBlocker& blocker, AcStats& stats, float const* vec, int n)
{
    if (!vec || n <= 0) return;
    if (!blocker.primed) {
        double first = 0.0;
        int finite = 0;
        for (int i = 0; i < n; ++i)
            if (std::isfinite(vec[i])) {
                first += vec[i];
                ++finite;
            }
        if (finite > 0)
            blocker.prime(static_cast<float>(first / finite));
    }
    double acSq = 0.0, sum = 0.0;
    float peak = stats.acPeak;
    uint64_t counted = 0;
    for (int i = 0; i < n; ++i) {
        float const x = vec[i];
        if (!std::isfinite(x)) continue;
        float const y = blocker.process(x);
        acSq += static_cast<double>(y) * y;
        sum += x;
        float const a = std::fabs(y);
        if (a > peak) peak = a;
        ++counted;
    }
    stats.acSumSq += acSq;
    stats.sum += sum;
    stats.acPeak = peak;
    stats.count += counted;
}

/**
 * A one-block SNAPSHOT (no history, e.g. /meter/master and /meter/query read the last Pd block): the block mean is the
 * DC part, the rest the AC part. Pure DC reads exactly 0 AC; a tone whose period is much longer than the block (a
 * 55 Hz saw in a 64-sample block) reads LOW, never high, so a snapshot can miss a deep bass but never calls DC a tone.
 */
inline void accumulateBlockMean(AcStats& stats, float const* vec, int n)
{
    if (!vec || n <= 0) return;
    double sum = 0.0;
    uint64_t counted = 0;
    for (int i = 0; i < n; ++i)
        if (std::isfinite(vec[i])) {
            sum += vec[i];
            ++counted;
        }
    if (counted == 0) return;
    double const mean = sum / static_cast<double>(counted);
    double acSq = 0.0;
    float peak = stats.acPeak;
    for (int i = 0; i < n; ++i) {
        if (!std::isfinite(vec[i])) continue;
        double const y = vec[i] - mean;
        acSq += y * y;
        float const a = static_cast<float>(std::fabs(y));
        if (a > peak) peak = a;
    }
    stats.acSumSq += acSq;
    stats.sum += sum;
    stats.acPeak = peak;
    stats.count += counted;
}

struct AcLevels {
    float acRmsDb = kFloorDb;
    float acPeakDb = kFloorDb;
    float dcDb = kFloorDb;
    bool tone = false;
};

/** The levels from an AC RMS, an AC peak and a DC amplitude (all linear). */
inline AcLevels levelsFrom(double acRms, double acPeak, double dcAmp)
{
    AcLevels out;
    out.acRmsDb = ampToDb(acRms);
    out.acPeakDb = ampToDb(acPeak);
    out.dcDb = ampToDb(std::fabs(dcAmp));
    out.tone = out.acRmsDb > kToneFloorDb;
    return out;
}

/** The levels of one tap; DC = |mean| of the input. Nothing counted: the floor, no tone. */
inline AcLevels levelsOf(AcStats const& s)
{
    if (s.count == 0) return {};
    double const n = static_cast<double>(s.count);
    return levelsFrom(std::sqrt(std::max(0.0, s.acSumSq) / n), s.acPeak, s.sum / n);
}

} // namespace mcp::ac
