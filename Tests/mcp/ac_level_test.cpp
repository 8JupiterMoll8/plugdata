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
        for (double rate : { 44100.0, 48000.0 }) {
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
        CHECK(!lv.tone && lv.acPeakDb <= kFloorDb && near(lv.dcDb, -22.0, 0.05));

        auto const block = saw(1000.0, 0.5f, 0.5f, kBlock);
        AcStats t;
        accumulateBlockMean(t, block.data(), kBlock);
        auto const lt = levelsOf(t);
        CHECK(lt.tone && lt.acRmsDb <= -10.79 + 0.5);

        // Two wires summed into one snapshot (the master meter walks every wire into the sink).
        AcStats both;
        accumulateBlockMean(both, dc.data(), kBlock);
        accumulateBlockMean(both, block.data(), kBlock);
        CHECK(both.count == 2u * kBlock && levelsOf(both).tone);
        float nanBlock[4] = { std::numeric_limits<float>::quiet_NaN(), 0.25f, 0.25f, 0.25f };
        AcStats n;
        accumulateBlockMean(n, nanBlock, 4);
        CHECK(n.count == 3 && !levelsOf(n).tone);
    }

    // AcStats::add merges two taps.
    {
        AcStats a { 1.0, 2.0, 0.5f, 10 }, b { 3.0, -4.0, 0.7f, 30 };
        a.add(b);
        CHECK(a.acSumSq == 4.0 && a.sum == -2.0 && a.acPeak == 0.7f && a.count == 40);
        // levelsFrom: the DC sign does not matter.
        CHECK(near(levelsFrom(0.0, 0.0, -0.5).dcDb, -6.02, 0.01));
    }

    if (failures == 0)
        std::printf("ac_level_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
