// Unit tests for the output ceiling and its loudness breaker (P9 Part B, review round 1): JUCE-free, run by scripts/test-bridge-safety.sh
// at -O0 and again at the Release flags (-O3 -DNDEBUG), so the non-finite test also runs on an optimised build.
#include "../../Source/Mcp/McpOutputCeiling.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

using mcp::OutputCeiling;

namespace {

constexpr double kRate = 44100.0;
constexpr int kBlock = 512;
constexpr double kPi = 3.14159265358979323846;
constexpr float kPlus12Db = 3.98107f; // +12 dBFS

// A multichannel block, channel-major, with a pointer table like JUCE's AudioBuffer.
struct Block {
    std::vector<std::vector<float>> data;
    std::vector<float*> ptrs;
    Block(int channels, int samples)
        : data(static_cast<size_t>(channels), std::vector<float>(static_cast<size_t>(samples), 0.0f))
    {
        for (auto& ch : data)
            ptrs.push_back(ch.data());
    }
    Block(Block const& o)
        : data(o.data)
    {
        for (auto& ch : data)
            ptrs.push_back(ch.data());
    }
    int channels() const { return static_cast<int>(data.size()); }
    int samples() const { return data.empty() ? 0 : static_cast<int>(data[0].size()); }
    float peak() const
    {
        float p = 0.0f;
        for (auto const& ch : data)
            for (float x : ch)
                p = std::fmax(p, std::fabs(x));
        return p;
    }
    bool allFinite() const
    {
        for (auto const& ch : data)
            for (float x : ch)
                if (!std::isfinite(x)) return false;
        return true;
    }
    bool bitEqual(Block const& o) const
    {
        for (size_t c = 0; c < data.size(); ++c)
            if (std::memcmp(data[c].data(), o.data[c].data(), data[c].size() * sizeof(float)) != 0) return false;
        return true;
    }
};

void fillSine(Block& b, float amplitude, double freq, int64_t& phase)
{
    for (int i = 0; i < b.samples(); ++i) {
        float const v = amplitude * static_cast<float>(std::sin(2.0 * kPi * freq * static_cast<double>(phase + i) / kRate));
        for (auto& ch : b.data)
            ch[static_cast<size_t>(i)] = v;
    }
    phase += b.samples();
}

void run(OutputCeiling& c, Block& b, uint32_t nowMs)
{
    c.process(b.ptrs.data(), b.channels(), b.samples(), kRate, nowMs);
}

void turnOn(OutputCeiling& c)
{
    c.setStandalone(true);
    c.setEnabled(true);
}

// ── Signal generators and a block feeder for the breaker tests ──
using Gen = std::function<float(int64_t)>;
Gen silence() { return [](int64_t) { return 0.0f; }; }
Gen square(float a) { return [a](int64_t n) { return (n / 50) % 2 == 0 ? a : -a; }; }
Gen sine(float a, double f)
{
    return [a, f](int64_t n) { return a * static_cast<float>(std::sin(2.0 * kPi * f * static_cast<double>(n) / kRate)); };
}
Gen tanhSine(float gain, double f)
{
    return [gain, f](int64_t n) { return std::tanh(gain * static_cast<float>(std::sin(2.0 * kPi * f * static_cast<double>(n) / kRate))); };
}
// Music-like: a 0.7-peak mix of three partials, and an over of `over` every 997 samples.
Gen music(float peak, float over)
{
    return [peak, over](int64_t n) {
        if (n % 997 == 0) return (n / 997) % 2 == 0 ? over : -over;
        double const t = static_cast<double>(n) / kRate;
        double const v = 0.43 * std::sin(2.0 * kPi * 110.0 * t) + 0.36 * std::sin(2.0 * kPi * 277.0 * t)
            + 0.21 * std::sin(2.0 * kPi * 1250.0 * t);
        return peak * static_cast<float>(v);
    };
}
Gen saw(float a, double f)
{
    return [a, f](int64_t n) {
        double const ph = std::fmod(f * static_cast<double>(n) / kRate, 1.0);
        return a * static_cast<float>(2.0 * ph - 1.0);
    };
}
// Full-scale pulses: `duty` of every 200 samples at +-a.
Gen clicks(float a, double duty)
{
    return [a, duty](int64_t n) {
        int64_t const k = n % 200;
        if (static_cast<double>(k) >= duty * 200.0) return 0.0f;
        return (n / 200) % 2 == 0 ? a : -a;
    };
}

constexpr double kExpectedTripMs = 270.0;     // ~248 ms from silence to -2.5 dBFS RMS for a full-scale square (tau 300 ms)
constexpr double kExpectedReleaseMs = 1400.0; // the 1 s hold after the loudness falls under -6 dBFS RMS, + its fall

// Feeds blocks of `block` frames from a generator to channel `hot` (others silent), or to every channel (hot < 0).
struct Feed {
    OutputCeiling& ceiling;
    int channels, block;
    int64_t pos = 0;
    float lastPeak = 0.0f;
    bool lastBitEqual = false;
    bool everyBlockAtUnity = true; // output == clamp(input) on every block so far
    double rate = kRate;           // the sample rate passed to process()

    Feed(OutputCeiling& c, int ch, int b)
        : ceiling(c)
        , channels(ch)
        , block(b)
    {
    }

    void oneBlock(Gen const& g, int hot, int frames)
    {
        Block b(channels, frames);
        for (int i = 0; i < frames; ++i) {
            float const v = g(pos + i);
            for (int ch = 0; ch < channels; ++ch)
                b.data[static_cast<size_t>(ch)][static_cast<size_t>(i)] = (hot < 0 || ch == hot) ? v : 0.0f;
        }
        Block const before = b;
        ceiling.process(b.ptrs.data(), b.channels(), b.samples(), rate, static_cast<uint32_t>(pos * 1000 / static_cast<int64_t>(kRate)));
        pos += frames;
        lastPeak = b.peak();
        lastBitEqual = b.bitEqual(before);
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < frames; ++i) {
                float const x = before.data[static_cast<size_t>(ch)][static_cast<size_t>(i)];
                float const want = std::isfinite(x) ? std::fmax(-1.0f, std::fmin(1.0f, x)) : 0.0f;
                if (b.data[static_cast<size_t>(ch)][static_cast<size_t>(i)] != want) everyBlockAtUnity = false;
            }
    }
    void runSamples(Gen const& g, int hot, int64_t samples)
    {
        while (samples > 0) {
            int const n = static_cast<int>(std::min<int64_t>(samples, block));
            oneBlock(g, hot, n);
            samples -= n;
        }
    }
    void run(Gen const& g, int hot, double ms) { runSamples(g, hot, static_cast<int64_t>(ms * kRate / 1000.0)); }
    // Milliseconds fed until the breaker state equals `engaged` (end of that block), or -1 within `maxMs`.
    double until(Gen const& g, int hot, double maxMs, bool engaged)
    {
        int64_t const start = pos;
        while (static_cast<double>(pos - start) * 1000.0 / kRate < maxMs) {
            oneBlock(g, hot, block);
            if (ceiling.read().breakerEngaged == engaged) return static_cast<double>(pos - start) * 1000.0 / kRate;
        }
        return -1.0;
    }
    double untilEngaged(Gen const& g, int hot, double maxMs) { return until(g, hot, maxMs, true); }
    double untilReleased(Gen const& g, int hot, double maxMs) { return until(g, hot, maxMs, false); }
};

} // namespace

int main()
{
    // The hot sine: +12 dBFS in, nothing above 0 dBFS out, on 1, 2 and 8 channels; overs counted.
    for (int channels : { 1, 2, 8 }) {
        OutputCeiling c;
        turnOn(c);
        Block b(channels, kBlock);
        int64_t phase = 0;
        fillSine(b, kPlus12Db, 440.0, phase);
        run(c, b, 1000);
        CHECK(b.peak() <= 1.0f);
        CHECK(b.peak() > 0.99f); // clamped, not muted
        auto const t = c.read();
        CHECK(t.clampedSamples > 0);
        CHECK(t.clampedSamples % static_cast<uint64_t>(channels) == 0); // every channel carries the same sine
        CHECK(t.clampedBlocks == 1);
        CHECK(t.nonFiniteSamples == 0);
        CHECK(t.lastClampMs == 1000);
    }

    // Exactly +-1.0 passes; the next float above is clamped.
    {
        OutputCeiling c;
        turnOn(c);
        Block b(1, 4);
        b.data[0] = { 1.0f, -1.0f, std::nextafter(1.0f, 2.0f), -std::nextafter(1.0f, 2.0f) };
        run(c, b, 5);
        CHECK(b.data[0][0] == 1.0f && b.data[0][1] == -1.0f);
        CHECK(b.data[0][2] == 1.0f && b.data[0][3] == -1.0f);
        CHECK(c.read().clampedSamples == 2);
    }

    // Non-finite in, 0 out (this file also runs at -O3 -DNDEBUG: the check must survive optimisation).
    {
        OutputCeiling c;
        turnOn(c);
        Block b(2, 6);
        float const nan = std::numeric_limits<float>::quiet_NaN();
        float const inf = std::numeric_limits<float>::infinity();
        for (auto& ch : b.data)
            ch = { nan, inf, -inf, 0.5f, -nan, 2.0f };
        run(c, b, 7);
        CHECK(b.allFinite());
        for (auto const& ch : b.data) {
            CHECK(ch[0] == 0.0f && ch[1] == 0.0f && ch[2] == 0.0f);
            CHECK(ch[3] == 0.5f);
            CHECK(ch[4] == 0.0f);
            CHECK(ch[5] == 1.0f);
        }
        auto const t = c.read();
        CHECK(t.nonFiniteSamples == 8);
        CHECK(t.clampedSamples == 2);
        CHECK(t.clampedBlocks == 1);
    }

    // A signal inside +-1 is bit-identical (no gain change below 0 dBFS).
    {
        OutputCeiling c;
        turnOn(c);
        int64_t phase = 0;
        for (int k = 0; k < 20; ++k) {
            Block b(2, kBlock);
            fillSine(b, 0.999f, 997.0, phase);
            Block const before = b;
            run(c, b, static_cast<uint32_t>(k * 12));
            CHECK(b.bitEqual(before));
        }
        auto const t = c.read();
        CHECK(t.clampedSamples == 0 && t.clampedBlocks == 0 && t.lastClampMs < 0);
    }

    // Setting off: bit-identical, hot and non-finite included, nothing counted.
    {
        OutputCeiling c;
        c.setStandalone(true);
        c.setEnabled(false);
        Block b(2, kBlock);
        int64_t phase = 0;
        fillSine(b, kPlus12Db, 440.0, phase);
        b.data[1][3] = std::numeric_limits<float>::quiet_NaN();
        Block before = b;
        run(c, b, 1);
        CHECK(b.data[1][3] != b.data[1][3]); // the NaN is still there
        b.data[1][3] = before.data[1][3] = 0.0f;
        CHECK(b.bitEqual(before));
        CHECK(!c.isActive());
        auto const t = c.read();
        CHECK(t.clampedSamples == 0 && t.nonFiniteSamples == 0 && t.clampedBlocks == 0);
    }

    // Not the standalone app (a plugin): untouched, whatever the setting.
    {
        OutputCeiling c;
        c.setStandalone(false);
        c.setEnabled(true);
        Block b(2, kBlock);
        int64_t phase = 0;
        fillSine(b, kPlus12Db, 440.0, phase);
        Block const before = b;
        run(c, b, 1);
        CHECK(b.bitEqual(before));
        CHECK(!c.isActive());
    }

    // Toggled while running: on clamps, off passes, on clamps again; counters only grow.
    {
        OutputCeiling c;
        turnOn(c);
        int64_t phase = 0;
        uint64_t last = 0;
        for (int k = 0; k < 9; ++k) {
            bool const on = (k / 3) != 1;
            c.setEnabled(on);
            Block b(2, kBlock);
            fillSine(b, kPlus12Db, 220.0, phase);
            Block const before = b;
            run(c, b, static_cast<uint32_t>(100 + k));
            if (on) {
                CHECK(b.peak() <= 1.0f);
            } else {
                CHECK(b.bitEqual(before));
            }
            auto const t = c.read();
            CHECK(t.clampedSamples >= last); // monotonic
            CHECK(on ? t.clampedSamples > last : t.clampedSamples == last);
            last = t.clampedSamples;
        }
    }

    // ── The breaker (review round 1): sustained LOUDNESS of the breaker's input (after the clamp), per channel. ──

    // The thresholds are what their names say.
    CHECK(std::fabs(OutputCeiling::kTripMeanSquare - std::pow(10.0, OutputCeiling::kTripRmsDb / 10.0)) < 1e-9);
    CHECK(std::fabs(OutputCeiling::kReleaseMeanSquare - std::pow(10.0, OutputCeiling::kReleaseRmsDb / 10.0)) < 1e-9);
    CHECK(OutputCeiling::kReleaseRmsDb < OutputCeiling::kTripRmsDb);

    // Full-scale material that never exceeds +-1.0 (what [clip~ -1 1] or tanh~ make of a +40 dB mistake) trips it,
    // on 2, 10, 32 and 72 channels with only ONE of them hot (the last; past 64 it shares the last loudness slot), at
    // several block sizes.
    {
        struct Case {
            char const* name;
            Gen gen;
        };
        Case const cases[] = {
            { "pre-clipped +-1 square", square(1.0f) },
            { "0.9999 square", square(0.9999f) },
            { "tanh(+40 dB sine)", tanhSine(100.0f, 440.0) },
            { "+12 dB square (clamped)", square(kPlus12Db) },
        };
        for (auto const& c : cases)
            for (int channels : { 2, 10, 32, 72 })
                for (int block : { 64, 512, 1000 }) {
                    OutputCeiling ceil;
                    turnOn(ceil);
                    Feed f(ceil, channels, block);
                    int const hot = channels - 1;
                    double const tripMs = f.untilEngaged(c.gen, hot, 2000.0);
                    if (!(tripMs >= 0.0 && tripMs <= kExpectedTripMs + 2.0 * block * 1000.0 / kRate))
                        std::printf("  %s, %d ch, block %d: trip at %.1f ms\n", c.name, channels, block, tripMs);
                    CHECK(tripMs >= 0.0);
                    CHECK(tripMs <= kExpectedTripMs + 2.0 * block * 1000.0 / kRate);
                    CHECK(tripMs >= kExpectedTripMs * 0.8); // sustained: not on the first loud block
                    // 40 ms later the output sits at the breaker gain, on every channel.
                    f.run(c.gen, hot, 40.0);
                    CHECK(f.lastPeak <= OutputCeiling::kBreakerGain + 1e-6f);
                    CHECK(ceil.read().breakerTrips == 1);
                }
    }

    // A burst slightly longer than the trip time trips it wherever it falls against the block grid (sliding measure).
    for (int block : { 64, 512, 4096 })
        for (int offset : { 0, 1, 17, 200, 511, 4000 }) {
            OutputCeiling ceil;
            turnOn(ceil);
            Feed f(ceil, 2, block);
            f.runSamples(silence(), -1, offset);
            f.runSamples(square(1.0f), 0, static_cast<int64_t>((kExpectedTripMs + 30.0) * kRate / 1000.0));
            f.run(silence(), -1, 50.0);
            if (ceil.read().breakerTrips != 1) std::printf("  burst: block %d offset %d not caught\n", block, offset);
            CHECK(ceil.read().breakerTrips == 1);
        }

    // Round 2 (trip at -2.5 dBFS RMS): a +0.2 dB hot sine (-2.85 dBFS RMS) is clamped, never tripped.
    {
        OutputCeiling ceil;
        turnOn(ceil);
        Feed f(ceil, 2, 512);
        f.run(sine(1.0233f, 440.0), -1, 4000.0);
        CHECK(ceil.read().clampedSamples > 0);
        CHECK(ceil.read().breakerTrips == 0);
    }

    // Round 2: 0 dBFS sines never trip at any frequency (at -3 dBFS the one-pole ripple tripped those under ~110 Hz).
    for (double hz : { 20.0, 30.0, 50.0, 80.0, 100.0, 440.0, 1000.0 }) {
        OutputCeiling ceil;
        turnOn(ceil);
        Feed f(ceil, 2, 512);
        f.run(sine(1.0f, hz), -1, 4000.0);
        if (ceil.read().breakerTrips != 0) std::printf("  0 dBFS sine at %.0f Hz tripped\n", hz);
        CHECK(ceil.read().breakerTrips == 0);
    }

    // Round 2: a loud master at -4 to -5 dBFS short-term RMS does not trip: a 0.6 square (-4.4 dBFS RMS) and a dense
    // mix scaled to about -4.5 dBFS RMS with its peaks clamped.
    {
        OutputCeiling ceil;
        turnOn(ceil);
        Feed f(ceil, 2, 512);
        f.run(square(0.6f), -1, 4000.0);
        f.run(music(1.45f, 1.2f), -1, 4000.0);
        CHECK(ceil.read().clampedSamples > 0);
        CHECK(ceil.read().breakerTrips == 0);
    }

    // Round 2: more than 64 channels: channel 63 and channel 70 of 72 (both in the shared last slot) each trip it.
    for (int hot : { 63, 70 }) {
        OutputCeiling ceil;
        turnOn(ceil);
        Feed f(ceil, 72, 512);
        double const tripMs = f.untilEngaged(square(1.0f), hot, 2000.0);
        CHECK(tripMs >= kExpectedTripMs * 0.8 && tripMs <= kExpectedTripMs + 2.0 * 512 * 1000.0 / kRate);
    }

    // Round 2: the loudness starts afresh on a channel-count change (mono -> stereo): what was measured before does
    // not shorten the trip after.
    {
        OutputCeiling ceil;
        turnOn(ceil);
        Feed mono(ceil, 1, 512);
        mono.run(square(1.0f), 0, 200.0); // ~49 % of the way to the trip, under it
        CHECK(!ceil.read().breakerEngaged);
        Feed stereo(ceil, 2, 512);
        double const tripMs = stereo.untilEngaged(square(1.0f), 0, 2000.0);
        CHECK(tripMs >= kExpectedTripMs * 0.8);
    }
    // Round 3 (MEDIUM): repeated prepares (`; pd oversample 1/0` -> prepareToPlay) no longer reset the loudness; at
    // this level they are sample-rate changes every ~30, 80 and 235 ms during a full-scale square: it still trips, and
    // once engaged it is not released while the square plays. (The host side: bridge_handler_tests, McpHost::prepare.)
    for (double everyMs : { 30.0, 80.0, 235.0 }) {
        OutputCeiling ceil;
        turnOn(ceil);
        Feed f(ceil, 2, 512);
        bool engaged = false, releasedWhileHot = false;
        for (int k = 0; k < 60; ++k) { // 60 x everyMs, at least 1.8 s
            f.rate = (k % 2 == 0) ? 44100.0 : 48000.0;
            f.run(square(1.0f), -1, everyMs);
            bool const now = ceil.read().breakerEngaged;
            releasedWhileHot = releasedWhileHot || (engaged && !now);
            engaged = engaged || now;
        }
        CHECK(engaged);
        CHECK(!releasedWhileHot);
        CHECK(ceil.read().breakerTrips == 1);
    }

    // Hot music-like material (a 0.7 peak mix with a 1.05 over now and then) never trips it and is never turned down.
    {
        OutputCeiling ceil;
        turnOn(ceil);
        Feed f(ceil, 2, 512);
        f.run(music(0.7f, 1.05f), -1, 6000.0);
        auto const t = ceil.read();
        CHECK(t.breakerTrips == 0);
        CHECK(!t.breakerEngaged);
        CHECK(t.clampedSamples > 0); // the overs were clamped
        CHECK(f.everyBlockAtUnity);  // nothing else changed
    }

    // Residue, pinned: full-scale waveforms whose short-term RMS stays under -2.5 dBFS are only clamped: a saw
    // (-4.8 dBFS RMS) and a pulse train at 54 % duty (-2.7 dBFS RMS).
    {
        OutputCeiling ceil;
        turnOn(ceil);
        Feed f(ceil, 2, 512);
        f.run(saw(1.0f, 110.0), -1, 4000.0);
        f.run(clicks(1.0f, 0.54), -1, 4000.0);
        CHECK(ceil.read().breakerTrips == 0);
    }

    // Held loud: engaged the whole time, one trip (not one per window); the output stays 20 dB down.
    // Calm (silence): released after the loudness falls under -6 dBFS RMS and stays there 1 s; a release is counted.
    {
        OutputCeiling ceil;
        turnOn(ceil);
        Feed f(ceil, 2, 512);
        f.run(square(1.0f), -1, 3000.0);
        auto t = ceil.read();
        CHECK(t.breakerEngaged && t.breakerTrips == 1 && t.breakerReleases == 0);
        CHECK(f.lastPeak <= OutputCeiling::kBreakerGain + 1e-6f);
        double const releasedMs = f.untilReleased(silence(), -1, 4000.0);
        CHECK(releasedMs >= OutputCeiling::kBreakerHoldMs);
        CHECK(releasedMs <= kExpectedReleaseMs + 300.0); // from full scale the loudness takes ~420 ms to fall to -6 dB
        t = ceil.read();
        CHECK(!t.breakerEngaged && t.breakerReleases == 1);
        // The ramp back up is over after ~200 ms: a quiet sine is bit-identical.
        f.run(sine(0.5f, 330.0), -1, 300.0);
        CHECK(f.lastBitEqual);
    }

    // Released while a moderate signal keeps playing (-10 dBFS sine), not only on silence.
    {
        OutputCeiling ceil;
        turnOn(ceil);
        Feed f(ceil, 2, 512);
        f.run(square(1.0f), 0, 1000.0);
        CHECK(ceil.read().breakerEngaged);
        double const releasedMs = f.untilReleased(sine(0.45f, 220.0), -1, 3000.0);
        CHECK(releasedMs >= 0.0 && releasedMs <= kExpectedReleaseMs + 300.0);
    }

    // A +40 dB square (the mistake the clamp alone turns into a full-scale square) ends up 20 dB down.
    {
        OutputCeiling ceil;
        turnOn(ceil);
        Feed f(ceil, 2, 512);
        f.run(square(100.0f), -1, 600.0);
        CHECK(f.lastPeak <= OutputCeiling::kBreakerGain + 1e-6f);
        CHECK(ceil.read().breakerTrips == 1);
    }

    // The owner's reset releases the breaker at once and clears the loudness; held loud, it trips again later.
    {
        OutputCeiling ceil;
        turnOn(ceil);
        Feed f(ceil, 2, 512);
        f.run(square(1.0f), -1, 600.0);
        CHECK(ceil.read().breakerEngaged);
        ceil.requestBreakerReset();
        f.run(square(1.0f), -1, 12.0);
        CHECK(!ceil.read().breakerEngaged);
        f.run(square(1.0f), -1, 600.0);
        CHECK(ceil.read().breakerEngaged);
        CHECK(ceil.read().breakerTrips == 2);
    }

    // Switching the ceiling off resets the breaker; back on starts at full gain with a fresh loudness measure.
    {
        OutputCeiling ceil;
        turnOn(ceil);
        Feed f(ceil, 2, 512);
        f.run(square(1.0f), -1, 600.0);
        CHECK(ceil.read().breakerEngaged);
        ceil.setEnabled(false);
        f.run(silence(), -1, 12.0);
        CHECK(!ceil.read().breakerEngaged);
        ceil.setEnabled(true);
        f.run(sine(0.5f, 330.0), -1, 12.0);
        CHECK(f.lastBitEqual);
    }

    // startedAtMs: set once, at construction (a restart gives a new one).
    {
        OutputCeiling a;
        auto const t = a.read();
        CHECK(t.startedAtMs > 1600000000000LL); // a wall-clock millisecond time
        CHECK(a.read().startedAtMs == t.startedAtMs);
    }

    // Zero channels or samples: nothing happens.
    {
        OutputCeiling c;
        turnOn(c);
        c.process(nullptr, 0, 512, kRate, 0);
        Block b(2, 0);
        run(c, b, 0);
        CHECK(c.read().clampedBlocks == 0);
    }

    // Alphi drop 1 F1 (D2): once per block the ceiling publishes the block mean square of what leaves it (after the
    // clamp AND the breaker gain), the loudest channel, plus a block counter; the inactive ceiling publishes 0 and
    // counts nothing (= stale: Alphi calm, "nothing measured").
    {
        OutputCeiling c;
        turnOn(c);
        CHECK(c.readPulse().blocks == 0);
        CHECK(c.readPulse().meanSquare == 0.0f);
        Feed f(c, 2, kBlock);
        f.oneBlock(sine(0.5f, 441.0), 0, kBlock); // ch 0 at 0.5 peak (mean square 0.125), ch 1 silent
        auto p = c.readPulse();
        CHECK(p.blocks == 1);
        CHECK(std::fabs(p.meanSquare - 0.125f) < 0.003f); // the loudest channel, not the mean of the two
        f.oneBlock(sine(0.25f, 441.0), -1, kBlock);
        p = c.readPulse();
        CHECK(p.blocks == 2);
        CHECK(std::fabs(p.meanSquare - 0.03125f) < 0.001f); // this block only (no smoothing on the audio thread)
        f.oneBlock(silence(), -1, kBlock);
        CHECK(c.readPulse().meanSquare == 0.0f);
        CHECK(c.readPulse().blocks == 3);
        // A hot block: what leaves the ceiling is clamped to +-1, so the mean square never exceeds 1.
        f.oneBlock(square(kPlus12Db), -1, kBlock);
        CHECK(c.readPulse().meanSquare <= 1.0f && c.readPulse().meanSquare > 0.95f);
    }
    {
        // After the breaker gain: engaged, the output (and so the pulse) is ~-20 dB under the full-scale input.
        OutputCeiling c;
        turnOn(c);
        Feed f(c, 2, kBlock);
        CHECK(f.untilEngaged(square(1.0f), -1, 2000.0) > 0.0);
        f.run(square(1.0f), -1, 100.0); // past the 20 ms attack
        float const g2 = OutputCeiling::kBreakerGain * OutputCeiling::kBreakerGain;
        CHECK(std::fabs(c.readPulse().meanSquare - g2) < 0.002f);
        CHECK(f.lastPeak <= OutputCeiling::kBreakerGain + 1e-6f); // the breaker itself is unchanged
    }
    {
        // Inactive (switched off, or a plugin build): 0 and no new block; null channels publish nothing.
        OutputCeiling c;
        turnOn(c);
        Feed f(c, 1, kBlock);
        f.oneBlock(sine(0.5f, 441.0), -1, kBlock);
        CHECK(c.readPulse().meanSquare > 0.1f);
        auto const before = c.readPulse().blocks;
        c.setEnabled(false);
        f.oneBlock(sine(0.5f, 441.0), -1, kBlock);
        CHECK(c.readPulse().meanSquare == 0.0f);
        CHECK(c.readPulse().blocks == before);
        CHECK(f.lastBitEqual); // inactive: the output is untouched, as before
        c.setEnabled(true);
        c.setStandalone(false);
        f.oneBlock(sine(0.5f, 441.0), -1, kBlock);
        CHECK(c.readPulse().meanSquare == 0.0f && c.readPulse().blocks == before);
        c.setStandalone(true);
        c.process(nullptr, 2, kBlock, kRate, 0);
        CHECK(c.readPulse().blocks == before);
        f.oneBlock(sine(0.5f, 441.0), -1, kBlock);
        CHECK(c.readPulse().blocks == before + 1);
        // Idle breaker: a signal inside +-1 still leaves bit-identical with the pulse publish in the loop.
        CHECK(f.lastBitEqual);
    }

    std::printf(failures ? "output_ceiling_test: %d FAILED\n" : "output_ceiling_test: OK\n", failures);
    return failures ? 1 : 0;
}
