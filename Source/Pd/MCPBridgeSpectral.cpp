/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Spectral analysis over raw samples (Plan 3 P3.3): shared by the offline render, the /meter/spectral
// probe and the armed-capture path. Moved unchanged out of MCPBridge.cpp; the functions are no longer file-static.

#include "MCPBridgeCmdCommon.h"

// One Hann-windowed FFTW r2c over N (<= PROBE_RING_SIZE) samples: |X|^2 per bin (0..N/2), unnormalised.
static std::vector<double> framePower(float const* samples, int N)
{
    std::array<float, PROBE_RING_SIZE> windowed {};
    for (int i = 0; i < N; ++i) {
        float const w = 0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * i / (N - 1)));
        windowed[i] = samples[i] * w;
    }
    int const NBINS = N / 2 + 1;
    std::vector<fftwf_complex> fftOutput(NBINS);
    fftwf_plan plan = fftwf_plan_dft_r2c_1d(N, windowed.data(),
        reinterpret_cast<fftwf_complex*>(fftOutput.data()), FFTW_ESTIMATE);
    fftwf_execute(plan);
    fftwf_destroy_plan(plan);
    std::vector<double> power(NBINS, 0.0);
    for (int i = 0; i < NBINS; ++i)
        power[i] = (double)(fftOutput[i][0] * fftOutput[i][0] + fftOutput[i][1] * fftOutput[i][1]);
    return power;
}

// The spectral SHAPE fields from a power spectrum of an N-point frame (one frame, or a Welch average of several):
// peakFrequency, spectralCentroid, spectralFlatness, spectralRolloff, peaks. Magnitudes are normalised by N so bin
// levels are on the same scale as the live probe path (collectResults, which does `/ N`). Without this the two
// spectral surfaces disagreed by 20*log10(N) (~60 dB at 1024) for the SAME spectrum — measure_spectral reported
// positive "dB" peaks (e.g. +37.9) while analyze_spectrum/analyze_acoustics reported the same bins at ~-22 dB.
// Centroid/flatness/rolloff are scale-invariant, so only the peak dB changes.
static void setSpectralShape(juce::DynamicObject* root, std::vector<double> const& power, int N, double sampleRate)
{
    int const NBINS = N / 2 + 1;
    float const binHz = (float)sampleRate / (float)N;
    float const fftNorm = 1.0f / (float)N;
    auto magAt = [&](int k) { return (float)std::sqrt(power[k]) * fftNorm; };

    double sumWeightedFreq = 0.0, sumMag = 0.0;
    double logSum = 0.0;
    int magCount = 0;
    float maxMag = 0.0f;
    int maxBin = 0;
    for (int i = 1; i < NBINS; ++i) { // skip DC
        float const mag = magAt(i);
        float const freq = i * binHz;
        sumWeightedFreq += (double)(mag * freq);
        sumMag += (double)mag;
        if (mag > 1e-10f) {
            logSum += std::log((double)mag);
            magCount++;
        }
        if (mag > maxMag) {
            maxMag = mag;
            maxBin = i;
        }
    }

    float const spectralCentroid = (sumMag > 1e-10) ? (float)(sumWeightedFreq / sumMag) : 0.0f;
    double const arithmeticMean = (magCount > 0) ? (sumMag / magCount) : 0.0;
    double const geometricMean = (magCount > 0) ? std::exp(logSum / magCount) : 0.0;
    float spectralFlatness = (arithmeticMean > 1e-10)
        ? (float)(geometricMean / arithmeticMean) : 0.0f;
    spectralFlatness = juce::jlimit(0.0f, 1.0f, spectralFlatness);

    // Rolloff (85% of spectral energy)
    double sumEnergy = 0.0;
    for (int i = 1; i < NBINS; ++i)
        sumEnergy += power[i];
    float rolloffFreq = 0.0f;
    if (sumEnergy > 1e-12) {
        double acc = 0.0;
        for (int i = 1; i < NBINS; ++i) {
            acc += power[i];
            if (acc >= 0.85 * sumEnergy) {
                rolloffFreq = i * binHz;
                break;
            }
        }
    }

    // Top-8 harmonic peaks (local maxima above 1% of max magnitude)
    juce::Array<juce::var> peaksArr;
    for (int i = 2; i < NBINS - 1 && peaksArr.size() < 8; ++i) {
        float const m = magAt(i);
        if (m > 0.01f * maxMag && m >= magAt(i - 1) && m >= magAt(i + 1)) {
            float const db = (m > 1e-7f) ? (20.0f * std::log10(m)) : -100.0f;
            auto* p = new juce::DynamicObject();
            p->setProperty("freq", i * binHz);
            p->setProperty("dB", db);
            peaksArr.add(juce::var(p));
        }
    }

    root->setProperty("peakFrequency", maxBin * binHz);
    root->setProperty("spectralCentroid", spectralCentroid);
    root->setProperty("spectralFlatness", spectralFlatness);
    root->setProperty("spectralRolloff", rolloffFreq);
    root->setProperty("peaks", peaksArr);
}

// Spectral analysis over exactly N raw samples (Hann window + FFTW r2c on
// mono data). Shared by the render path (final PROBE_RING_SIZE samples), the /meter/spectral
// probe path, and the Phase-5 armed-capture path. JSON keys match what the
// server already parses.
juce::var spectralVarFromSamples(float const* samples, int N, double sampleRate)
{
    if (!samples || N <= 1 || sampleRate <= 0.0)
        return {};
    if (N > PROBE_RING_SIZE) N = PROBE_RING_SIZE;

    // RMS / peak over the full analyzed window (unwindowed)
    double sumSq = 0.0;
    float maxPeak = 0.0f;
    for (int i = 0; i < N; ++i) {
        float const s = std::abs(samples[i]);
        sumSq += (double)(samples[i] * samples[i]);
        if (s > maxPeak) maxPeak = s;
    }
    float const rms = (float)std::sqrt(sumSq / N);
    float const rmsDb = (rms > 1e-7f) ? (20.0f * std::log10(rms)) : -100.0f;
    float const peakDb = (maxPeak > 1e-7f) ? (20.0f * std::log10(maxPeak)) : -100.0f;
    float const crestFactor = (rms > 1e-7f) ? (maxPeak / rms) : 0.0f;

    // Fundamental via autocorrelation on the raw samples (same helper as probes)
    float const fundamental = estimateFrequency(samples, N, (float)sampleRate);

    auto* root = new juce::DynamicObject();
    root->setProperty("rms", rms);
    root->setProperty("maxPeak", maxPeak);
    root->setProperty("rmsDb", rmsDb);
    root->setProperty("peakDb", peakDb);
    root->setProperty("fundamental", fundamental);
    setSpectralShape(root, framePower(samples, N), N, sampleRate);
    root->setProperty("crestFactor", crestFactor);
    root->setProperty("fftSize", N);
    root->setProperty("binHz", (float)sampleRate / (float)N);
    return juce::var(root);
}

// YIN (de Cheveigné & Kawahara, 2002) over [minHz, maxHz]: difference function -> cumulative-mean normalised
// difference -> absolute threshold 0.15 -> parabolic interpolation. Hz (0 = unvoiced) and a confidence. Needs
// n >= n/2 + sampleRate/minHz. The voice-take pipeline uses it from 55 Hz (MCPBridge.cpp); J7 from 30 Hz.
float estimatePitchYinRange(const float* buf, int n, float sampleRate, float minHz, float maxHz, float& outConfidence)
{
    outConfidence = 0.0f;
    if (!buf || n < 256 || sampleRate <= 0.0f || minHz <= 0.0f) return 0.0f;

    int const maxTau = std::min(n / 2, static_cast<int>(sampleRate / minHz));
    int const minTau = std::max(2, maxHz > 0.0f ? static_cast<int>(sampleRate / maxHz) : 2);
    if (maxTau < 8 || minTau >= maxTau - 1) return 0.0f;
    int const halfN = n / 2;

    std::vector<float> dp(maxTau, 1.0f);
    float running = 0.0f;
    for (int tau = 1; tau < maxTau; ++tau) {
        float sum = 0.0f;
        for (int i = 0; i < halfN; ++i) {
            float const diff = buf[i] - buf[i + tau];
            sum += diff * diff;
        }
        running += sum;
        dp[tau] = running > 0.0f ? (sum * static_cast<float>(tau)) / running : 1.0f;
    }

    constexpr float threshold = 0.15f;
    int tauEstimate = -1;
    for (int tau = minTau; tau < maxTau; ++tau) {
        if (dp[tau] < threshold) {
            while (tau + 1 < maxTau && dp[tau + 1] < dp[tau]) ++tau;
            tauEstimate = tau;
            break;
        }
    }
    if (tauEstimate < 0) {
        int best = minTau;
        for (int tau = minTau + 1; tau < maxTau; ++tau) {
            if (dp[tau] < dp[best]) best = tau;
        }
        if (dp[best] > 0.35f) return 0.0f; // too aperiodic → unvoiced
        tauEstimate = best;
    }

    outConfidence = juce::jlimit(0.0f, 1.0f, 1.0f - dp[tauEstimate] * 1.6f);

    float betterTau = static_cast<float>(tauEstimate);
    if (tauEstimate > 1 && tauEstimate < maxTau - 1) {
        float const s0 = dp[tauEstimate - 1];
        float const s1 = dp[tauEstimate];
        float const s2 = dp[tauEstimate + 1];
        float const denom = 2.0f * (2.0f * s1 - s2 - s0);
        if (std::abs(denom) > 1e-9f) betterTau = static_cast<float>(tauEstimate) + (s2 - s0) / denom;
    }
    return betterTau > 0.0f ? sampleRate / betterTau : 0.0f;
}

// JSON string form of the SAME analysis (render + armed-capture callers).
// One FFT implementation lives in spectralVarFromSamples; this is a thin
// wrapper so no caller carries a second copy that can drift (see #78).
juce::String spectralJsonFromSamples(float const* samples, int N, double sampleRate)
{
    auto v = spectralVarFromSamples(samples, N, sampleRate);
    if (v.isVoid())
        return {};
    return juce::JSON::toString(v, true);
}

// Spectral analysis over the baked render buffer (final PROBE_RING_SIZE samples).
juce::String renderSpectralJson(float const* data, int n, double sampleRate)
{
    constexpr int N = PROBE_RING_SIZE;
    if (n < N || !data || sampleRate <= 0.0)
        return {};
    return spectralJsonFromSamples(data + (n - N), N, sampleRate);
}

// Phase 5 armed-capture analysis. P11 V7 (J7): the captured window may hold a one-shot anywhere. The level fields
// (rms, maxPeak, rmsDb, peakDb, crestFactor) still come from one frame centred on the loudest sample, as before. The
// PITCH and the spectral SHAPE come from the body: hop-spaced PROBE_RING_SIZE frames from the onset (the first sample
// within 20 dB of the peak) + 25 ms on, at most kBodyFrames, the quiet ones (26 dB under the loudest) left out. The
// pitch is the energy-weighted median of YIN (30 Hz up) over the voiced frames; the shape is a Welch average of the
// frames' power spectra. Extra fields: voicedFrames, bodyFrames, attackPitchHz (the old estimate on the peak frame,
// which J7 read as 0 Hz / 108 Hz for an 808 kick), pitchMethod. A capture too short for a body frame falls back to
// the peak frame for both. Reports silence/short honestly if there is nothing to read.
juce::String armedSpectralJson(float const* data, int n, double sampleRate)
{
    constexpr int N = PROBE_RING_SIZE;
    constexpr int kBodyFrames = 24;
    if (n < N || !data || sampleRate <= 0.0)
        return {};

    int peakIdx = 0;
    float peak = 0.0f;
    for (int i = 0; i < n; ++i) {
        float const a = std::abs(data[i]);
        if (a > peak) { peak = a; peakIdx = i; }
    }
    int start = peakIdx - N / 2;
    if (start < 0) start = 0;
    if (start + N > n) start = n - N;
    auto v = spectralVarFromSamples(data + start, N, sampleRate);
    auto* root = v.getDynamicObject();
    if (!root)
        return {};
    float const attackPitch = static_cast<float>(root->getProperty("fundamental"));

    // The body frames.
    int onset = 0;
    while (onset < n && std::abs(data[onset]) < 0.1f * peak)
        ++onset;
    std::vector<int> frames;
    int const from = onset + static_cast<int>(0.025 * sampleRate);
    if (peak > 0.0f && from + N <= n) {
        int const span = n - N - from;
        int const hop = std::max(N / 4, span / (kBodyFrames - 1) + 1);
        for (int f = from; f + N <= n && static_cast<int>(frames.size()) < kBodyFrames; f += hop)
            frames.push_back(f);
    }
    if (frames.empty())
        frames.push_back(start);

    std::vector<double> energy(frames.size(), 0.0);
    double loudest = 0.0;
    for (size_t k = 0; k < frames.size(); ++k) {
        for (int i = 0; i < N; ++i)
            energy[k] += static_cast<double>(data[frames[k] + i]) * data[frames[k] + i];
        loudest = std::max(loudest, energy[k]);
    }
    std::vector<double> welch(N / 2 + 1, 0.0);
    int used = 0;
    struct Voiced { float hz; double weight; };
    std::vector<Voiced> voiced;
    for (size_t k = 0; k < frames.size(); ++k) {
        if (loudest <= 0.0 || energy[k] < loudest * 0.0025) // 26 dB under the loudest body frame
            continue;
        auto const power = framePower(data + frames[k], N);
        for (size_t b = 0; b < welch.size(); ++b)
            welch[b] += power[b];
        ++used;
        float confidence = 0.0f;
        float const hz = estimatePitchYinRange(data + frames[k], N, static_cast<float>(sampleRate), 30.0f, 4000.0f, confidence);
        if (hz > 0.0f)
            voiced.push_back({ hz, energy[k] });
    }
    if (used > 0) {
        for (auto& b : welch)
            b /= used;
        setSpectralShape(root, welch, N, sampleRate);
    }
    float fundamental = 0.0f;
    if (!voiced.empty()) {
        std::sort(voiced.begin(), voiced.end(), [](Voiced const& a, Voiced const& b) { return a.hz < b.hz; });
        double total = 0.0;
        for (auto const& x : voiced) total += x.weight;
        double acc = 0.0;
        fundamental = voiced.back().hz;
        for (auto const& x : voiced) {
            acc += x.weight;
            if (acc >= 0.5 * total) { fundamental = x.hz; break; }
        }
    }
    root->setProperty("fundamental", fundamental);
    root->setProperty("attackPitchHz", attackPitch);
    root->setProperty("voicedFrames", static_cast<int>(voiced.size()));
    root->setProperty("bodyFrames", used);
    root->setProperty("pitchMethod", "yin_median_body");
    return juce::JSON::toString(v, true);
}
