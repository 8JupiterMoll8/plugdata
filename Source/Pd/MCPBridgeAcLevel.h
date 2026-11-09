/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// V1 AC taps: the level of what can be HEARD, next to the raw peak/RMS. A [phasor~] through a [lop~] with its cutoff
// at 0 puts out pure DC; every meter read it as "-22 dBFS, audible" while the artist heard nothing (2026-11-06). Each
// tap now also runs a one-pole DC blocker and reports the AC level (what is left above ~10 Hz), the DC level (the
// mean of the input) and `tone`.
//
// `tone` of a tap that runs the blocker (probe, trace tap, final tap) = the AC RMS is above -60 dBFS AND the AC part
// swung across zero at that level at least kMinSwings times (two full cycles). The second condition is what tells a
// tone from a DC STEP: a step (a gated DC voice, a cutoff that snaps to 0) leaves a one-signed exponential through the
// blocker, loud enough for the RMS test (a -40 dBFS step reads about -58) but with no swing. It keeps short hits: a
// 30 ms hi-hat or a 50 Hz kick swings many times. Limits: a tone below 2 cycles per window (a 30 Hz tone in a 50 ms
// window) reads no tone; a DC voice gated three times or more in one window swings and reads tone (those are clicks).
//
// `tone` of a one-block SNAPSHOT (/meter/master, /meter/query: the last 64-sample block, no history) is three-valued,
// because the block mean cannot see a bass whose period is longer than the block (a 55 Hz sine at -20 dBFS reads -58):
// 1 = AC above -60 AND a full cycle in the block (2 swings, as the blocker taps' rule); 0 = no sound (the block is
// exactly flat, or quiet raw as well); -1 = unknown: the raw level is up, or there is AC without a full cycle (a DC step
// inside the block, or a tone below ~700 Hz), and one block cannot tell: use an armed read.
//
// JUCE-free and Pd-free, so Tests/mcp/ac_level_test.cpp runs it with g++ alone. Realtime-safe: no allocation, no
// locks, no exceptions; every call is O(samples). The OUTPUT is never touched: the blocker reads samples and keeps its
// own state. Non-finite samples, and finite ones past +-1e6 (+120 dBFS, no audio), are skipped: they neither count
// nor enter the blocker state, and a huge one re-primes the blocker at the next usable sample (else its step would
// ring for seconds); an output that still overflows re-primes as well, so nothing can poison a blocker for good. Denormals: the
// state is flushed to 0 below 1e-20.
//
// Window edges: an armed window gathers whole audio blocks, so it may start up to one block (~1-10 ms) before the arm
// and end up to one block after the read's cut-off (the existing re-arm note, review L3). The AC sums share that skew
// with the raw ones; it does not change a verdict for windows of 50 ms and more.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mcp::ac {

constexpr float kCornerHz = 10.0f;        // the blocker's corner: below every audible fundamental
constexpr float kToneFloorDb = -60.0f;    // AC RMS above this = a tone someone can hear
constexpr float kFloorDb = -100.0f;       // the bridge's level for "nothing" (as peakToDb)
constexpr float kDenormalFlush = 1.0e-20f;
constexpr double kDefaultRate = 48000.0;
constexpr float kSwingLevel = 1.0e-3f;    // -60 dBFS: a swing counts only when the AC part crosses +-this
constexpr int kMinSwings = 4;             // two full cycles
constexpr float kSettleTimeConstants = 4.0f; // acPeak ignores the first 4 tau after priming (~64 ms at 10 Hz)
constexpr float kFlatRatio = 1.0e-5f;     // a snapshot block is flat when its deviation is under 1e-5 of its level
constexpr float kMaxSample = 1.0e6f;      // +120 dBFS: no audio; such a sample is broken, like NaN

/** A sample the AC taps use: finite and within +-kMaxSample. */
inline bool usable(float x) { return std::isfinite(x) && std::fabs(x) <= kMaxSample; }

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
 * 0 from the first sample. A tap that opens in the middle of a tone primes slightly off the tone's true mean; that
 * offset decays with tau = 1/(1-R) samples (~16 ms), so acPeak skips the first kSettleTimeConstants tau after a prime
 * (`settleLeft`); acRms and the swings count from the first sample. (A hit inside those first ~64 ms of a probe or
 * trace tap is in acRms and tone, not in acPeak.) acPeak is the peak of the blocker's OUTPUT: a sine keeps its peak, a
 * sawtooth's ramps tilt through any one-pole high-pass (a settled 55 Hz saw reads +1.5 dB, 110 Hz +0.8 dB), so acPeak
 * is not the input's peak for sharp waveforms; the raw peak is still there.
 */
struct DcBlocker {
    float r = coefficientFor(kDefaultRate);
    float x1 = 0.0f;
    float y1 = 0.0f;
    bool primed = false;
    int side = 0;              // the last side of +-kSwingLevel the output was on (0 = none yet)
    uint32_t settleLeft = 0;   // samples until acPeak counts again

    void reset(float coefficient)
    {
        r = (coefficient > 0.0f && coefficient < 1.0f) ? coefficient : coefficientFor(kDefaultRate);
        x1 = 0.0f;
        y1 = 0.0f;
        primed = false;
        side = 0;
        settleLeft = 0;
    }

    void prime(float dcEstimate)
    {
        primed = true;
        x1 = dcEstimate;
        y1 = 0.0f;
        side = 0;
        settleLeft = static_cast<uint32_t>(kSettleTimeConstants / std::max(1.0e-6f, 1.0f - r));
    }

    /** The AC part of a FINITE sample (callers skip non-finite ones). Unprimed, it primes with this sample. */
    float process(float x)
    {
        if (!primed)
            prime(x);
        float y = x - x1 + r * y1;
        if (!std::isfinite(y)) { // overflow (x - x1 past FLT_MAX): start again from this sample
            prime(x);
            return 0.0f;
        }
        x1 = x;
        if (std::fabs(y) < kDenormalFlush)
            y = 0.0f;
        y1 = y;
        return y;
    }
};

/**
 * What one tap accumulated: the AC part's sum of squares and peak, the signed sum of the input (its mean is the DC
 * level), the raw sum of squares and largest |x| (the snapshot's raw level and flatness), the AC swings and the finite
 * samples counted.
 */
struct AcStats {
    double acSumSq = 0.0;
    double sum = 0.0;
    float acPeak = 0.0f;
    uint64_t count = 0;
    uint64_t swings = 0;
    double rawSumSq = 0.0;
    float maxAbs = 0.0f;
    uint64_t active = 0;       // truth slice 2 (M3): samples whose AC part is at or past +-kSwingLevel (the swing rate's base)

    void add(AcStats const& other)
    {
        active += other.active;
        acSumSq += other.acSumSq;
        sum += other.sum;
        if (other.acPeak > acPeak) acPeak = other.acPeak;
        count += other.count;
        swings += other.swings;
        rawSumSq += other.rawSumSq;
        if (other.maxAbs > maxAbs) maxAbs = other.maxAbs;
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
            if (usable(vec[i])) {
                first += vec[i];
                ++finite;
            }
        if (finite > 0) {
            blocker.prime(static_cast<float>(first / finite));
            // Truth slice 2 (review H1): an exactly flat first block (silence, a constant) primes EXACTLY: nothing is
            // off to settle, so a hit right after it (a click, the first note on a node tap) counts in acPeak at once.
            float lo = 0.0f, hi = 0.0f;
            bool seen = false;
            for (int i = 0; i < n; ++i)
                if (usable(vec[i])) {
                    lo = seen ? std::min(lo, vec[i]) : vec[i];
                    hi = seen ? std::max(hi, vec[i]) : vec[i];
                    seen = true;
                }
            if (seen && lo == hi)
                blocker.settleLeft = 0;
        }
    }
    double acSq = 0.0, sum = 0.0, rawSq = 0.0;
    float peak = stats.acPeak, maxAbs = stats.maxAbs;
    uint64_t counted = 0, swings = 0, active = 0;
    for (int i = 0; i < n; ++i) {
        float const x = vec[i];
        if (!usable(x)) {
            if (std::isfinite(x)) blocker.primed = false; // huge: start again from the next usable sample
            continue;
        }
        float const y = blocker.process(x);
        acSq += static_cast<double>(y) * y;
        sum += x;
        rawSq += static_cast<double>(x) * x;
        maxAbs = std::max(maxAbs, std::fabs(x));
        if (blocker.settleLeft > 0)
            --blocker.settleLeft;
        else
            peak = std::max(peak, std::fabs(y));
        int const now = y >= kSwingLevel ? 1 : (y <= -kSwingLevel ? -1 : 0);
        if (now != 0) {
            if (blocker.side != 0 && now != blocker.side) ++swings;
            blocker.side = now;
            ++active;
        }
        ++counted;
    }
    stats.acSumSq += acSq;
    stats.sum += sum;
    stats.rawSumSq += rawSq;
    stats.maxAbs = maxAbs;
    stats.acPeak = peak;
    stats.count += counted;
    stats.swings += swings;
    stats.active += active;
}

/**
 * A one-block SNAPSHOT (no history): the block mean is the DC part, the rest the AC part. Pure DC reads exactly 0 AC; a
 * tone whose period is much longer than the block reads LOW, so its verdict is snapshotTone(), never levelsOf().tone.
 */
inline void accumulateBlockMean(AcStats& stats, float const* vec, int n)
{
    if (!vec || n <= 0) return;
    double sum = 0.0, rawSq = 0.0;
    float maxAbs = stats.maxAbs;
    uint64_t counted = 0;
    for (int i = 0; i < n; ++i)
        if (usable(vec[i])) {
            sum += vec[i];
            rawSq += static_cast<double>(vec[i]) * vec[i];
            maxAbs = std::max(maxAbs, std::fabs(vec[i]));
            ++counted;
        }
    if (counted == 0) return;
    double const mean = sum / static_cast<double>(counted);
    double acSq = 0.0;
    float peak = stats.acPeak;
    int side = 0;
    uint64_t swings = 0;
    for (int i = 0; i < n; ++i) {
        if (!usable(vec[i])) continue;
        double const y = vec[i] - mean;
        acSq += y * y;
        peak = std::max(peak, static_cast<float>(std::fabs(y)));
        int const now = y >= kSwingLevel ? 1 : (y <= -kSwingLevel ? -1 : 0); // the same swing rule as the blocker taps
        if (now != 0) {
            if (side != 0 && now != side) ++swings;
            side = now;
        }
    }
    stats.swings += swings;
    stats.acSumSq += acSq;
    stats.sum += sum;
    stats.rawSumSq += rawSq;
    stats.maxAbs = maxAbs;
    stats.acPeak = peak;
    stats.count += counted;
}

struct AcLevels {
    float acRmsDb = kFloorDb;
    float acPeakDb = kFloorDb;
    float dcDb = kFloorDb;
    bool tone = false;
    double swingsPerActive = 0.0; // truth slice 2 (M3): zero crossings per sample while the AC part is up

    /** The AC part's dominant swing rate (Hz): half the crossings per second of the time it was up; 0 = none. */
    double swingHz(double sampleRate) const { return swingsPerActive * sampleRate * 0.5; }
};

/** The levels from an AC RMS, an AC peak and a DC amplitude (all linear) and the swings seen. */
inline AcLevels levelsFrom(double acRms, double acPeak, double dcAmp, uint64_t swings)
{
    AcLevels out;
    out.acRmsDb = ampToDb(acRms);
    out.acPeakDb = ampToDb(acPeak);
    out.dcDb = ampToDb(std::fabs(dcAmp));
    out.tone = out.acRmsDb > kToneFloorDb && swings >= static_cast<uint64_t>(kMinSwings);
    return out;
}

/** The levels of one blocker tap; DC = |mean| of the input. Nothing counted: the floor, no tone. */
inline AcLevels levelsOf(AcStats const& s)
{
    if (s.count == 0) return {};
    double const n = static_cast<double>(s.count);
    auto lv = levelsFrom(std::sqrt(std::max(0.0, s.acSumSq) / n), s.acPeak, s.sum / n, s.swings);
    lv.swingsPerActive = s.active > 0 ? static_cast<double>(s.swings) / static_cast<double>(s.active) : 0.0;
    return lv;
}

/**
 * Several channels of one tap (the final output): each channel is judged on its own, so a mono voice on the left of a
 * 16-out device is not diluted by 15 silent channels. Levels = the loudest channel's (AC RMS, AC peak, |DC|); tone =
 * any channel has one.
 */
inline AcLevels levelsOfChannels(AcStats const* channels, int numChannels)
{
    AcLevels out;
    if (!channels) return out;
    for (int c = 0; c < numChannels; ++c) {
        auto const lv = levelsOf(channels[c]);
        if (channels[c].count == 0) continue;
        if (lv.acRmsDb > out.acRmsDb) out.swingsPerActive = lv.swingsPerActive; // the loudest channel's swing rate
        out.acRmsDb = std::max(out.acRmsDb, lv.acRmsDb);
        out.acPeakDb = std::max(out.acPeakDb, lv.acPeakDb);
        out.dcDb = std::max(out.dcDb, lv.dcDb);
        out.tone = out.tone || lv.tone;
    }
    return out;
}

/** The three-valued tone of a snapshot (see the top of this file): 1 tone, 0 no sound, -1 unknown. */
inline int snapshotTone(AcStats const& s)
{
    if (s.count == 0) return 0;
    double const n = static_cast<double>(s.count);
    bool const acUp = ampToDb(std::sqrt(std::max(0.0, s.acSumSq) / n)) > kToneFloorDb;
    if (acUp && s.swings >= 2) return 1;
    if (acUp) return -1; // AC without a full cycle in the block: a step or a low tone, one block cannot tell
    if (s.acPeak <= kFlatRatio * s.maxAbs || s.acPeak < 1.0e-9f) return 0; // exactly flat: DC, not a bass
    if (ampToDb(std::sqrt(std::max(0.0, s.rawSumSq) / n)) > kToneFloorDb) return -1;
    return 0;
}

} // namespace mcp::ac
