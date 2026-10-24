// Unit tests for the output ceiling and its breaker (P9 Part B): JUCE-free, run by scripts/test-bridge-safety.sh
// at -O0 and again at the Release flags (-O3 -DNDEBUG), so the non-finite test also runs on an optimised build.
#include "../../Source/Mcp/McpOutputCeiling.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

void fillSquare(Block& b, float amplitude, int64_t& phase)
{
    for (int i = 0; i < b.samples(); ++i) {
        float const v = ((phase + i) / 50) % 2 == 0 ? amplitude : -amplitude;
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

void hotFor(OutputCeiling& c, int blocks, int64_t& phase, float amplitude = kPlus12Db)
{
    for (int k = 0; k < blocks; ++k) {
        Block b(2, kBlock);
        fillSquare(b, amplitude, phase);
        run(c, b, 0);
    }
}

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

    // A single over now and then never trips the breaker (and is never attenuated).
    {
        OutputCeiling c;
        turnOn(c);
        for (int k = 0; k < 400; ++k) { // ~4.6 s
            Block b(2, kBlock);
            if (k % 3 == 0) b.data[0][100] = 1.5f;
            b.data[1][200] = 0.25f;
            run(c, b, static_cast<uint32_t>(k * 12));
            CHECK(b.data[1][200] == 0.25f);
        }
        auto const t = c.read();
        CHECK(t.breakerTrips == 0);
        CHECK(!t.breakerEngaged);
        CHECK(t.clampedSamples > 100);
    }

    // A sustained hot square trips the breaker within ~150 ms (+ a block); the output then sits 20 dB down.
    // Silence for ~1 s releases it; after the ~200 ms ramp a quiet signal is bit-identical again.
    {
        OutputCeiling c;
        turnOn(c);
        int64_t phase = 0;
        int tripBlock = -1;
        uint32_t now = 0;
        for (int k = 0; k < 60; ++k, now += 12) {
            Block b(2, kBlock);
            fillSquare(b, kPlus12Db, phase);
            run(c, b, now);
            CHECK(b.peak() <= 1.0f);
            if (tripBlock < 0 && c.read().breakerEngaged) tripBlock = k;
            if (tripBlock >= 0 && k > tripBlock + 4) {
                CHECK(b.peak() <= OutputCeiling::kBreakerGain + 1e-6f); // ramped fully down after ~20 ms
            }
        }
        double const tripMs = (tripBlock + 1) * kBlock * 1000.0 / kRate;
        CHECK(tripBlock >= 0);
        CHECK(tripMs <= OutputCeiling::kBreakerWindowMs + 2.0 * kBlock * 1000.0 / kRate);
        auto t = c.read();
        CHECK(t.breakerTrips == 1); // one trip while it stays hot, not one per window
        CHECK(t.breakerEngaged);

        // Silence: still engaged before the hold time, released after it.
        int releaseBlock = -1;
        for (int k = 0; k < 200; ++k, now += 12) {
            Block b(2, kBlock);
            run(c, b, now);
            if (releaseBlock < 0 && !c.read().breakerEngaged) releaseBlock = k;
        }
        double const heldMs = (releaseBlock + 1) * kBlock * 1000.0 / kRate;
        CHECK(releaseBlock >= 0);
        CHECK(heldMs >= OutputCeiling::kBreakerHoldMs - 1.0);
        CHECK(heldMs <= OutputCeiling::kBreakerHoldMs + 2.0 * kBlock * 1000.0 / kRate);
        // The ramp back is over by now (200 blocks ~ 2.3 s): a quiet sine passes untouched.
        Block q(2, kBlock);
        int64_t p2 = 0;
        fillSine(q, 0.5f, 330.0, p2);
        Block const before = q;
        run(c, q, now);
        CHECK(q.bitEqual(before));
        CHECK(c.read().breakerTrips == 1);
    }

    // A +40 dB square (the mistake the clamp alone would turn into a full-scale square) ends up 20 dB down.
    {
        OutputCeiling c;
        turnOn(c);
        int64_t phase = 0;
        float lastPeak = 1.0f;
        for (int k = 0; k < 40; ++k) {
            Block b(2, kBlock);
            fillSquare(b, 100.0f, phase);
            run(c, b, static_cast<uint32_t>(k));
            lastPeak = b.peak();
        }
        CHECK(lastPeak <= OutputCeiling::kBreakerGain + 1e-6f);
        CHECK(c.read().breakerTrips == 1);
    }

    // The owner's reset releases the breaker at once (ramping back up), and a later sustained over trips it again.
    {
        OutputCeiling c;
        turnOn(c);
        int64_t phase = 0;
        hotFor(c, 30, phase);
        CHECK(c.read().breakerEngaged);
        c.requestBreakerReset();
        Block b(2, kBlock);
        run(c, b, 0);
        CHECK(!c.read().breakerEngaged);
        hotFor(c, 30, phase);
        CHECK(c.read().breakerEngaged);
        CHECK(c.read().breakerTrips == 2);
    }

    // Switching the ceiling off resets the breaker; back on starts at full gain.
    {
        OutputCeiling c;
        turnOn(c);
        int64_t phase = 0;
        hotFor(c, 30, phase);
        CHECK(c.read().breakerEngaged);
        c.setEnabled(false);
        Block off(2, kBlock);
        run(c, off, 0);
        CHECK(!c.read().breakerEngaged);
        c.setEnabled(true);
        Block q(2, kBlock);
        int64_t p2 = 0;
        fillSine(q, 0.5f, 330.0, p2);
        Block const before = q;
        run(c, q, 0);
        CHECK(q.bitEqual(before));
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

    std::printf(failures ? "output_ceiling_test: %d FAILED\n" : "output_ceiling_test: OK\n", failures);
    return failures ? 1 : 0;
}
