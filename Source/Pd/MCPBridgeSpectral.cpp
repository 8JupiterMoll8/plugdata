/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Spectral analysis over raw samples (Plan 3 P3.3): shared by the offline render, the /meter/spectral
// probe and the armed-capture path. Moved unchanged out of MCPBridge.cpp; the functions are no longer file-static.

#include "MCPBridgeCmdCommon.h"

// Spectral analysis over exactly N raw samples (Hann window + FFTW r2c on
// mono data). Shared by the render path (final 1024 samples), the /meter/spectral
// probe path, and the Phase-5 armed-capture path. JSON keys match what the
// server already parses.
juce::var spectralVarFromSamples(float const* samples, int N, double sampleRate)
{
    if (!samples || N <= 1 || sampleRate <= 0.0)
        return {};
    if (N > PROBE_RING_SIZE) N = PROBE_RING_SIZE;

    // Hann window over the N samples
    std::array<float, PROBE_RING_SIZE> windowed {};
    for (int i = 0; i < N; ++i) {
        float const w = 0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * i / (N - 1)));
        windowed[i] = samples[i] * w;
    }

    // RMS / peak over the full analyzed window (unwindowed)
    double sumSq = 0.0;
    float maxPeak = 0.0f;
    for (int i = 0; i < N; ++i) {
        float const s = std::abs(samples[i]);
        sumSq += (double)(samples[i] * samples[i]);
        if (s > maxPeak) maxPeak = s;
    }
    float const rms = (float)std::sqrt(sumSq / N);

    // FFT (single precision, same as the probe path)
    std::array<float, PROBE_RING_SIZE> fftInput {};
    std::copy(windowed.begin(), windowed.begin() + N, fftInput.begin());
    int const NBINS = N / 2 + 1;
    std::vector<fftwf_complex> fftOutput(NBINS);
    fftwf_plan plan = fftwf_plan_dft_r2c_1d(N, fftInput.data(),
        reinterpret_cast<fftwf_complex*>(fftOutput.data()), FFTW_ESTIMATE);
    fftwf_execute(plan);
    fftwf_destroy_plan(plan);

    float const binHz = (float)sampleRate / (float)N;

    // Normalize magnitudes by N so bin levels are on the same scale as the
    // live probe path (collectResults, which does `/ N`). Without this the two
    // spectral surfaces disagreed by 20*log10(N) (~60 dB at 1024) for the SAME
    // spectrum — measure_spectral reported positive "dB" peaks (e.g. +37.9) while
    // analyze_spectrum/analyze_acoustics reported the same bins at ~-22 dB.
    // Centroid/flatness/rolloff are scale-invariant, so only the peak dB changes.
    float const fftNorm = 1.0f / (float)N;

    double sumWeightedFreq = 0.0, sumMag = 0.0;
    double logSum = 0.0;
    int magCount = 0;
    float maxMag = 0.0f;
    int maxBin = 0;
    for (int i = 1; i < NBINS; ++i) { // skip DC
        float const re = fftOutput[i][0];
        float const im = fftOutput[i][1];
        float const mag = std::sqrt(re * re + im * im) * fftNorm;
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
    std::vector<double> binEnergy(NBINS, 0.0);
    for (int i = 1; i < NBINS; ++i) {
        float const re = fftOutput[i][0];
        float const im = fftOutput[i][1];
        binEnergy[i] = (double)(re * re + im * im);
        sumEnergy += binEnergy[i];
    }
    float rolloffFreq = 0.0f;
    if (sumEnergy > 1e-12) {
        double acc = 0.0;
        for (int i = 1; i < NBINS; ++i) {
            acc += binEnergy[i];
            if (acc >= 0.85 * sumEnergy) {
                rolloffFreq = i * binHz;
                break;
            }
        }
    }

    // Top-8 harmonic peaks (local maxima above 1% of max magnitude)
    juce::Array<juce::var> peaksArr;
    for (int i = 2; i < NBINS - 1 && peaksArr.size() < 8; ++i) {
        auto magAt = [&](int k) {
            return std::sqrt(fftOutput[k][0] * fftOutput[k][0] + fftOutput[k][1] * fftOutput[k][1]) * fftNorm;
        };
        float const m = magAt(i);
        if (m > 0.01f * maxMag && m >= magAt(i - 1) && m >= magAt(i + 1)) {
            float const db = (m > 1e-7f) ? (20.0f * std::log10(m)) : -100.0f;
            auto* p = new juce::DynamicObject();
            p->setProperty("freq", i * binHz);
            p->setProperty("dB", db);
            peaksArr.add(juce::var(p));
        }
    }

    float const rmsDb = (rms > 1e-7f) ? (20.0f * std::log10(rms)) : -100.0f;
    float const peakDb = (maxPeak > 1e-7f) ? (20.0f * std::log10(maxPeak)) : -100.0f;
    float const crestFactor = (rms > 1e-7f) ? (maxPeak / rms) : 0.0f;
    float const peakFrequency = maxBin * binHz;

    // Fundamental via autocorrelation on the raw samples (same helper as probes)
    float const fundamental = estimateFrequency(samples, N, (float)sampleRate);

    auto* root = new juce::DynamicObject();
    root->setProperty("rms", rms);
    root->setProperty("maxPeak", maxPeak);
    root->setProperty("rmsDb", rmsDb);
    root->setProperty("peakDb", peakDb);
    root->setProperty("fundamental", fundamental);
    root->setProperty("peakFrequency", peakFrequency);
    root->setProperty("spectralCentroid", spectralCentroid);
    root->setProperty("spectralFlatness", spectralFlatness);
    root->setProperty("spectralRolloff", rolloffFreq);
    root->setProperty("crestFactor", crestFactor);
    root->setProperty("fftSize", N);
    root->setProperty("binHz", binHz);
    root->setProperty("peaks", peaksArr);
    return juce::var(root);
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

// Spectral analysis over the baked render buffer (final 1024 samples).
juce::String renderSpectralJson(float const* data, int n, double sampleRate)
{
    constexpr int N = PROBE_RING_SIZE; // 1024
    if (n < N || !data || sampleRate <= 0.0)
        return {};
    return spectralJsonFromSamples(data + (n - N), N, sampleRate);
}

// Phase 5 armed-capture analysis: the captured window may hold a one-shot
// anywhere, so center the analysis on the loudest sample instead of blindly
// taking the tail. Reports silence/short honestly if there is nothing to read.
juce::String armedSpectralJson(float const* data, int n, double sampleRate)
{
    constexpr int N = PROBE_RING_SIZE; // 1024
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
    return spectralJsonFromSamples(data + start, N, sampleRate);
}
