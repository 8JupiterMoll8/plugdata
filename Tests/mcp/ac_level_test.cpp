// Unit tests for the V1 AC taps (DC blocker + AC/DC levels): JUCE-free, run by scripts/test-bridge-safety.sh.
// The live bug (2026-11-06): [phasor~] -> [lop~ 0] put out pure DC; every meter said "-22 dBFS, audible".
#include "../../Source/Pd/MCPBridgeAcLevel.h"

#include <cmath>
#include <cstdio>
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

using namespace mcp::ac;

namespace {

constexpr double kRate = 48000.0;
constexpr int kBlock = 64; // a Pd block

// A naive saw in [-amp, amp] (RMS amp / sqrt 3), plus `dc`.
std::vector<float> saw(double hz, float amp, float dc, int samples, double rate = kRate)
{
    std::vector<float> out(static_cast<size_t>(samples));
    double phase = 0.0;
    for (auto& s : out) {
        s = dc + amp * static_cast<float>(2.0 * phase - 1.0);
        phase += hz / rate;
        if (phase >= 1.0) phase -= 1.0;
    }
    return out;
}

// Runs `signal` through a fresh blocker in Pd-sized blocks, as a tap sees it.
AcStats run(std::vector<float> const& signal, double rate = kRate)
{
    DcBlocker blocker;
    blocker.reset(coefficientFor(rate));
    AcStats stats;
    for (size_t at = 0; at < signal.size(); at += kBlock) {
        int const n = static_cast<int>(std::min<size_t>(kBlock, signal.size() - at));
        accumulate(blocker, stats, signal.data() + at, n);
    }
    return stats;
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

} // namespace

int main()
{
    int const window = static_cast<int>(kRate / 2); // the ~500 ms tap window

    // The corner sits near 10 Hz at 44.1 and 48 kHz; a missing rate falls back to 48 kHz.
    {
        for (double rate : { 44100.0, 48000.0, 96000.0 }) {
            float const r = coefficientFor(rate);
            double const fc = (1.0 - r) * rate / (2.0 * 3.14159265358979323846);
            CHECK(fc > 8.0 && fc < 12.0);
        }
        CHECK(coefficientFor(0.0) == coefficientFor(48000.0));
        CHECK(coefficientFor(std::numeric_limits<double>::quiet_NaN()) == coefficientFor(48000.0));
    }

    // The live bug: pure DC (the -22 dBFS of a [lop~ 0] behind a phasor) is no tone. AC at the floor, DC at -22.
    {
        float const dc = std::pow(10.0f, -22.0f / 20.0f);
        std::vector<float> signal(static_cast<size_t>(window), dc);
        auto const lv = levelsOf(run(signal));
        std::printf("  dc: ac %.1f / %.1f dB, dc %.2f dB, tone %d\n", lv.acRmsDb, lv.acPeakDb, lv.dcDb, lv.tone);
        CHECK(lv.acRmsDb <= kFloorDb);
        CHECK(lv.acPeakDb <= kFloorDb); // primed: no step transient at the first sample
        CHECK(near(lv.dcDb, -22.0, 0.05));
        CHECK(!lv.tone);
        // Negative DC too.
        std::vector<float> neg(static_cast<size_t>(window), -0.5f);
        auto const ln = levelsOf(run(neg));
        CHECK(!ln.tone && ln.acPeakDb <= kFloorDb && near(ln.dcDb, -6.02, 0.05));
    }

    // A 220 Hz saw is a tone, at its own level (amp 0.5: RMS 0.2887, -10.79 dB); no DC.
    {
        auto const lv = levelsOf(run(saw(220.0, 0.5f, 0.0f, window)));
        std::printf("  saw 220: ac %.2f / %.2f dB, dc %.1f dB, tone %d\n", lv.acRmsDb, lv.acPeakDb, lv.dcDb, lv.tone);
        CHECK(lv.tone);
        CHECK(near(lv.acRmsDb, -10.79, 0.3));
        CHECK(lv.acPeakDb < 0.0); // a tap opened mid-tone: the priming offset lifts the peak a little, never past 0 dB
        CHECK(lv.dcDb < -30.0);
    }

    // DC 0.5 plus a 55 Hz saw (amp 0.3: RMS 0.1732, -15.23 dB): a tone at the saw's level; the DC reads -6 dB.
    {
        auto const lv = levelsOf(run(saw(55.0, 0.3f, 0.5f, window)));
        std::printf("  dc+saw 55: ac %.2f / %.2f dB, dc %.2f dB, tone %d\n", lv.acRmsDb, lv.acPeakDb, lv.dcDb, lv.tone);
        CHECK(lv.tone);
        CHECK(near(lv.acRmsDb, -15.23, 0.5));
        CHECK(near(lv.dcDb, -6.02, 0.5));
        // At 44.1 kHz as well.
        auto const l44 = levelsOf(run(saw(55.0, 0.3f, 0.5f, 22050, 44100.0), 44100.0));
        CHECK(l44.tone && near(l44.acRmsDb, -15.23, 0.5));
    }

    // A quiet tone under the floor (-70 dB RMS) is not a tone; just above it is.
    {
        float const quiet = static_cast<float>(std::sqrt(3.0) * std::pow(10.0, -70.0 / 20.0));
        CHECK(!levelsOf(run(saw(220.0, quiet, 0.2f, window))).tone);
        float const audible = static_cast<float>(std::sqrt(3.0) * std::pow(10.0, -55.0 / 20.0));
        CHECK(levelsOf(run(saw(220.0, audible, 0.2f, window))).tone);
    }

    // Silence: everything at the floor, no tone, samples counted.
    {
        auto const stats = run(std::vector<float>(static_cast<size_t>(window), 0.0f));
        auto const lv = levelsOf(stats);
        CHECK(stats.count == static_cast<uint64_t>(window));
        CHECK(lv.acRmsDb <= kFloorDb && lv.acPeakDb <= kFloorDb && lv.dcDb <= kFloorDb && !lv.tone);
        // No samples at all: the floor, no tone.
        auto const none = levelsOf(AcStats {});
        CHECK(none.acRmsDb <= kFloorDb && !none.tone);
    }

    // NaN/inf: skipped (not counted, not in the state); the tone around them reads as before.
    {
        auto signal = saw(220.0, 0.5f, 0.0f, window);
        for (size_t i = 100; i < signal.size(); i += 997)
            signal[i] = (i % 2) ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
        auto const stats = run(signal);
        auto const lv = levelsOf(stats);
        CHECK(stats.count < static_cast<uint64_t>(window));
        CHECK(std::isfinite(stats.acSumSq) && std::isfinite(stats.sum) && std::isfinite(stats.acPeak));
        CHECK(lv.tone && near(lv.acRmsDb, -10.79, 0.5));
        // All NaN: nothing counted, the floor.
        std::vector<float> nan(static_cast<size_t>(kBlock), std::numeric_limits<float>::quiet_NaN());
        auto const ln = levelsOf(run(nan));
        CHECK(!ln.tone && ln.acRmsDb <= kFloorDb);
    }

    // Denormals: a tone that stops leaves no denormal state behind; denormal input stays at the floor.
    {
        DcBlocker blocker;
        blocker.reset(coefficientFor(kRate));
        AcStats stats;
        auto const tone = saw(220.0, 0.5f, 0.0f, 4800);
        accumulate(blocker, stats, tone.data(), static_cast<int>(tone.size()));
        std::vector<float> zeros(static_cast<size_t>(kRate * 2), 0.0f); // 2 s: the tail falls under the flush level
        accumulate(blocker, stats, zeros.data(), static_cast<int>(zeros.size()));
        CHECK(blocker.y1 == 0.0f || std::fpclassify(blocker.y1) == FP_NORMAL);
        CHECK(blocker.y1 == 0.0f); // the tail decayed below the flush threshold and was flushed
        std::vector<float> denorm(static_cast<size_t>(window), std::numeric_limits<float>::denorm_min() * 7.0f);
        auto const ld = levelsOf(run(denorm));
        CHECK(!ld.tone && ld.acRmsDb <= kFloorDb && ld.dcDb <= kFloorDb);
    }

    // The input is read, never written.
    {
        auto signal = saw(55.0, 0.3f, 0.5f, 1024);
        auto const copy = signal;
        run(signal);
        AcStats s;
        accumulateBlockMean(s, signal.data(), static_cast<int>(signal.size()));
        CHECK(signal == copy);
    }

    // Snapshot (block mean): pure DC is exactly no tone; a 220 Hz saw over a 64-sample block reads as a tone (low);
    // DC + a tone never reads louder than the tone.
    {
        std::vector<float> dc(static_cast<size_t>(kBlock), 0.0794f);
        AcStats s;
        accumulateBlockMean(s, dc.data(), kBlock);
        auto const lv = levelsOf(s);
        CHECK(snapshotTone(s) == 0 && lv.acPeakDb <= kFloorDb && near(lv.dcDb, -22.0, 0.05));

        auto const block = saw(1000.0, 0.5f, 0.5f, kBlock);
        AcStats t;
        accumulateBlockMean(t, block.data(), kBlock);
        auto const lt = levelsOf(t);
        CHECK(snapshotTone(t) == 1 && lt.acRmsDb <= -10.79 + 0.5);

        // Two wires summed into one snapshot (the master meter walks every wire into the sink).
        AcStats both;
        accumulateBlockMean(both, dc.data(), kBlock);
        accumulateBlockMean(both, block.data(), kBlock);
        CHECK(both.count == 2u * kBlock && snapshotTone(both) == 1);
        float nanBlock[4] = { std::numeric_limits<float>::quiet_NaN(), 0.25f, 0.25f, 0.25f };
        AcStats n;
        accumulateBlockMean(n, nanBlock, 4);
        CHECK(n.count == 3 && snapshotTone(n) == 0);
        AcStats silent;
        std::vector<float> zeros(kBlock, 0.0f);
        accumulateBlockMean(silent, zeros.data(), kBlock);
        CHECK(snapshotTone(silent) == 0 && snapshotTone(AcStats {}) == 0);
    }


    // Review F1 (HIGH): a snapshot (one 64-sample block) cannot see a bass whose period is longer than the block. A
    // real bass must never read "no tone" (0): it reads 1 or -1 (unknown, use an armed read), at every phase.
    {
        for (double hz : { 30.0, 55.0, 100.0, 200.0 })
            for (double db : { -40.0, -20.0 }) {
                double const amp = std::sqrt(2.0) * std::pow(10.0, db / 20.0);
                int zeros = 0, ones = 0;
                for (int ph = 0; ph < 64; ++ph) {
                    std::vector<float> v(kBlock);
                    for (int i = 0; i < kBlock; ++i)
                        v[static_cast<size_t>(i)] = static_cast<float>(amp * std::sin(2.0 * 3.14159265358979 * hz * i / kRate + ph * 2.0 * 3.14159265358979 / 64.0));
                    AcStats st;
                    accumulateBlockMean(st, v.data(), kBlock);
                    int const t = snapshotTone(st);
                    zeros += t == 0;
                    ones += t == 1;
                }
                std::printf("  snapshot sine %3.0f Hz %3.0f dB: tone=1 at %d/64 phases, tone=0 at %d\n", hz, db, ones, zeros);
                CHECK(zeros == 0);
            }
        // DC at the same levels stays 0 (exactly flat), however loud.
        for (float dc : { 0.01f, 0.1f, -0.7f }) {
            std::vector<float> v(kBlock, dc);
            AcStats st;
            accumulateBlockMean(st, v.data(), kBlock);
            CHECK(snapshotTone(st) == 0);
        }
    }

    // Review F2 (MEDIUM): a DC step inside a window (a gated DC voice, a cutoff snapping to 0) is no tone, even when its
    // blocker tail passes the RMS floor; a steady tone and a short noise hit still are.
    {
        auto settled = [](DcBlocker& b) {
            b.reset(coefficientFor(kRate));
            std::vector<float> z(static_cast<size_t>(kRate), 0.0f);
            AcStats warm;
            accumulate(b, warm, z.data(), static_cast<int>(z.size()));
        };
        for (double db : { -20.0, -30.0, -40.0, -45.0 }) {
            DcBlocker b;
            settled(b);
            std::vector<float> v(static_cast<size_t>(window), 0.0f);
            for (size_t i = v.size() / 2; i < v.size(); ++i) v[i] = static_cast<float>(std::pow(10.0, db / 20.0));
            AcStats st;
            for (size_t at = 0; at < v.size(); at += kBlock) accumulate(b, st, v.data() + at, kBlock);
            auto const lv = levelsOf(st);
            std::printf("  dc step %3.0f dB: acRms %.1f, swings %llu, tone %d\n", db, lv.acRmsDb, static_cast<unsigned long long>(st.swings), lv.tone);
            CHECK(!lv.tone);
        }
        // A gated DC pulse (on 100 ms, then off): up and down, one swing, no tone.
        {
            DcBlocker b;
            settled(b);
            std::vector<float> v(static_cast<size_t>(window), 0.0f);
            for (size_t i = 4800; i < 9600; ++i) v[i] = 0.1f;
            AcStats st;
            for (size_t at = 0; at < v.size(); at += kBlock) accumulate(b, st, v.data() + at, kBlock);
            CHECK(!levelsOf(st).tone);
        }
        // A steady tone with a settled blocker: tone.
        {
            DcBlocker b;
            settled(b);
            auto const v = saw(110.0, 0.05f, 0.3f, window);
            AcStats st;
            for (size_t at = 0; at < v.size(); at += kBlock) accumulate(b, st, v.data() + at, kBlock);
            CHECK(levelsOf(st).tone);
        }
        // A 30 ms noise hit (a hi-hat) in a 500 ms window: a tone (short hits must stay provable).
        {
            DcBlocker b;
            settled(b);
            std::vector<float> v(static_cast<size_t>(window), 0.0f);
            uint32_t seed = 12345;
            for (size_t i = 2000; i < 2000 + 1440; ++i) {
                seed = seed * 1664525u + 1013904223u;
                v[i] = 0.2f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
            }
            AcStats st;
            for (size_t at = 0; at < v.size(); at += kBlock) accumulate(b, st, v.data() + at, kBlock);
            CHECK(levelsOf(st).tone);
        }
    }

    // Review F3 (MEDIUM): channels are judged one by one: a mono voice on the left of a stereo (or 16-out) device is
    // not diluted by the silent ones.
    {
        auto const tone = saw(220.0, 0.1f * std::sqrt(3.0f), 0.0f, window); // RMS 0.1: -20 dBFS
        std::vector<float> silence(tone.size(), 0.0f);
        std::vector<float> dc(tone.size(), 0.25f);
        AcStats ch[16];
        DcBlocker b[16];
        for (int c = 0; c < 16; ++c) {
            b[c].reset(coefficientFor(kRate));
            auto const& src = c == 0 ? tone : (c == 1 ? dc : silence);
            accumulate(b[c], ch[c], src.data(), static_cast<int>(src.size()));
        }
        auto const stereo = levelsOfChannels(ch, 2);
        CHECK(stereo.tone && near(stereo.acRmsDb, -20.0, 0.5));
        CHECK(near(stereo.dcDb, -12.04, 0.1)); // the loudest channel's DC (the right one)
        auto const wide = levelsOfChannels(ch, 16);
        CHECK(wide.tone && near(wide.acRmsDb, -20.0, 0.5));
        // DC on the right alone: no tone on any channel.
        auto const dcOnly = levelsOfChannels(ch + 1, 15);
        CHECK(!dcOnly.tone && dcOnly.acRmsDb <= kFloorDb);
        CHECK(!levelsOfChannels(nullptr, 2).tone && !levelsOfChannels(ch + 2, 14).tone);
    }

    // Review F4: a finite input that overflows the filter, or sits past +120 dBFS, never poisons the blocker; the tone
    // after it reads as before (a 220 Hz sine, amp 0.5: -9.03 dBFS RMS).
    {
        std::vector<float> v(static_cast<size_t>(window));
        for (size_t i = 0; i < v.size(); ++i)
            v[i] = i < 64 ? ((i & 1) ? 3.0e38f : -3.0e38f) : 0.5f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 220.0 * i / kRate));
        auto const st = run(v);
        auto const lv = levelsOf(st);
        std::printf("  overflow then tone: ac %.2f / %.2f dB, tone %d\n", lv.acRmsDb, lv.acPeakDb, lv.tone);
        CHECK(st.count == v.size() - 64);
        CHECK(lv.tone && near(lv.acRmsDb, -9.03, 0.5) && lv.acPeakDb < 0.0);
        // The filter's own overflow (finite input, x - x1 past FLT_MAX) re-primes too.
        DcBlocker b;
        b.reset(coefficientFor(kRate));
        b.prime(-3.0e38f);
        CHECK(b.process(3.0e38f) == 0.0f && std::isfinite(b.y1));
    }

    // Review F5: a tap opened mid-tone primes off the tone's mean; acPeak skips the settling, so at every opening phase
    // it stays within 0.5 dB of what a SETTLED blocker reads. For a sine that is the true peak; a saw's ramps tilt
    // through any one-pole high-pass, so its settled acPeak sits above the input's (55 Hz: -4.5 vs -6.02 dBFS).
    {
        auto wave = [](bool sawShape, double ph0, double seconds = 0.5) {
            std::vector<float> v(static_cast<size_t>(kRate * seconds));
            double p = ph0;
            for (auto& x : v) {
                x = sawShape ? 0.5f * static_cast<float>(2.0 * p - 1.0) : 0.5f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * p));
                p += 55.0 / kRate;
                if (p >= 1.0) p -= 1.0;
            }
            return v;
        };
        for (bool sawShape : { true, false }) {
            DcBlocker settled;
            settled.reset(coefficientFor(kRate));
            AcStats warm;
            auto const longer = wave(sawShape, 0.0, 3.0); // 2.5 s to settle, then the last 0.5 s measured
            int const settleN = static_cast<int>(kRate * 2.5);
            accumulate(settled, warm, longer.data(), settleN);
            warm = {};
            accumulate(settled, warm, longer.data() + settleN, static_cast<int>(longer.size()) - settleN);
            double const steady = levelsOf(warm).acPeakDb;
            double worst = -200.0;
            for (int ph = 0; ph < 872; ph += 8)
                worst = std::max(worst, static_cast<double>(levelsOf(run(wave(sawShape, ph / 872.0))).acPeakDb));
            std::printf("  mid-open 55 Hz %s: worst acPeak %.2f dB, settled %.2f dB\n", sawShape ? "saw" : "sine", worst, steady);
            CHECK(worst <= steady + 0.5);
            if (!sawShape) CHECK(worst <= -6.02 + 0.5);
        }
    }

    // AcStats::add merges two taps.
    {
        AcStats a { 1.0, 2.0, 0.5f, 10, 3, 5.0, 0.9f }, b { 3.0, -4.0, 0.7f, 30, 4, 1.0, 0.2f };
        a.add(b);
        CHECK(a.acSumSq == 4.0 && a.sum == -2.0 && a.acPeak == 0.7f && a.count == 40);
        CHECK(a.swings == 7 && a.rawSumSq == 6.0 && a.maxAbs == 0.9f);
        // levelsFrom: the DC sign does not matter.
        CHECK(near(levelsFrom(0.0, 0.0, -0.5, 0).dcDb, -6.02, 0.01));
    }

    if (failures == 0)
        std::printf("ac_level_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
