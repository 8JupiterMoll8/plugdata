// Unit tests for the ONE sound verdict per meter read (truth slice 2, docs/review/truth-layers-audit-2026-11-08.md
// in the server repo, section 6): JUCE-free, run by scripts/test-bridge-safety.sh.
// The bridge says audible / silent / dc_only / unmeasured, the level that decided, the tone flag and the DC facts; the
// server only words it. Done-tests: a drone with DC, a quiet fading tail, a hard square.
#include "../../Source/Pd/MCPBridgeSoundVerdict.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

using namespace mcp;
using verdict::Kind;

namespace {

constexpr double kRate = 48000.0;
constexpr int kBlock = 64;
constexpr double kPi = 3.14159265358979323846;
int const kWindow = static_cast<int>(kRate / 2); // the ~500 ms window a construct / measure reads

// What a tap measures: the raw peak (dBFS) and the AC levels after the blocker, as the bridge gathers them.
struct Tap {
    float rawPeakDb;
    ac::AcLevels ac;
};

Tap tapOf(std::vector<float> const& signal)
{
    ac::DcBlocker blocker;
    blocker.reset(ac::coefficientFor(kRate));
    ac::AcStats stats;
    float peak = 0.0f;
    for (size_t at = 0; at < signal.size(); at += kBlock) {
        int const n = static_cast<int>(std::min<size_t>(kBlock, signal.size() - at));
        ac::accumulate(blocker, stats, signal.data() + at, n);
    }
    for (float s : signal) peak = std::max(peak, std::fabs(s));
    return { ac::ampToDb(peak), ac::levelsOf(stats) };
}

// [phasor~ hz] scaled by `amp`: 0..amp, half of it DC.
std::vector<float> phasor(double hz, float amp, int samples)
{
    std::vector<float> out(static_cast<size_t>(samples));
    double phase = 0.0;
    for (auto& s : out) {
        s = amp * static_cast<float>(phase);
        phase += hz / kRate;
        if (phase >= 1.0) phase -= 1.0;
    }
    return out;
}

// A sine of peak `amp` decaying with time constant `tauMs` (a piano tail).
std::vector<float> tail(double hz, float amp, double tauMs, int samples)
{
    std::vector<float> out(static_cast<size_t>(samples));
    for (int i = 0; i < samples; ++i) {
        double const t = i / kRate;
        out[static_cast<size_t>(i)] = amp * static_cast<float>(std::exp(-t * 1000.0 / tauMs) * std::sin(2.0 * kPi * hz * t));
    }
    return out;
}

std::vector<float> square(double hz, float amp, int samples)
{
    std::vector<float> out(static_cast<size_t>(samples));
    double phase = 0.0;
    for (auto& s : out) {
        s = phase < 0.5 ? amp : -amp;
        phase += hz / kRate;
        if (phase >= 1.0) phase -= 1.0;
    }
    return out;
}

bool has(std::string const& json, std::string const& part) { return json.find(part) != std::string::npos; }

} // namespace

int main()
{
    // DONE-TEST 1a: a drone with DC. [phasor~ 55] * 0.4 runs 0..0.4: raw peak -8 dBFS, half of it a constant. The
    // tone decides (its AC peak, never the raw peak with the DC in it), and the DC near the tone is said.
    {
        auto const t = tapOf(phasor(55.0, 0.4f, kWindow));
        auto const v = verdict::judge(t.rawPeakDb, t.ac, nullptr);
        std::printf("  drone+dc: raw %.1f ac %.1f/%.1f dc %.1f -> %s level %.1f\n", t.rawPeakDb, t.ac.acRmsDb, t.ac.acPeakDb,
            t.ac.dcDb, verdict::kindName(v.kind), v.levelDb);
        CHECK(v.kind == Kind::Audible);
        CHECK(v.hasLevel);
        CHECK(v.tone);
        CHECK(!v.quiet);
        CHECK(v.levelDb < t.rawPeakDb - 3.0f); // the tone, not the raw peak with its DC
        CHECK(v.levelDb <= t.rawPeakDb);
        CHECK(v.dcFact == verdict::DcFact::WithTone);
        CHECK(std::fabs(v.dcDb - t.ac.dcDb) < 1e-4f);
        CHECK(!v.nearFullScale);
    }

    // DONE-TEST 1b: a drone that is ONLY DC (the 2026-11-06 bug: [phasor~] -> [lop~ 0]): loud raw peak, no tone at all.
    {
        float const dc = std::pow(10.0f, -22.0f / 20.0f);
        auto const t = tapOf(std::vector<float>(static_cast<size_t>(kWindow), dc));
        auto const v = verdict::judge(t.rawPeakDb, t.ac, nullptr);
        CHECK(v.kind == Kind::DcOnly);
        CHECK(!v.hasLevel);
        CHECK(!v.tone);
        CHECK(v.dcShare == verdict::DcShare::Dominant);
        CHECK(v.dcFact == verdict::DcFact::Alone);
        CHECK(std::string(v.reason) == "dc_dominant");
        auto const json = verdict::toJson(v, "final", nullptr);
        CHECK(has(json, "\"verdict\":\"dc_only\""));
        CHECK(has(json, "\"levelDb\":null"));
        CHECK(has(json, "\"rawPeakDb\":-22.0"));
    }

    // DONE-TEST 2a: a quiet fading tail (live jam 2026-11-07, item 12): 330 Hz from -50 dBFS, tau 60 ms. Its AC RMS over
    // the window is under the tone floor, so the engine's tone flag is 0, but it is a TONE: never "only DC".
    {
        float const amp = std::pow(10.0f, -50.0f / 20.0f);
        auto const t = tapOf(tail(330.0, amp, 60.0, kWindow));
        auto const v = verdict::judge(t.rawPeakDb, t.ac, nullptr);
        std::printf("  tail: raw %.1f ac %.1f/%.1f dc %.1f tone %d -> %s level %.1f\n", t.rawPeakDb, t.ac.acRmsDb, t.ac.acPeakDb,
            t.ac.dcDb, t.ac.tone, verdict::kindName(v.kind), v.levelDb);
        CHECK(!t.ac.tone);
        CHECK(v.kind != Kind::DcOnly);
        CHECK(v.kind == Kind::Audible); // its AC peak clears the floor
        CHECK(v.quiet);
        CHECK(!v.tone);
        CHECK(v.dcShare == verdict::DcShare::Below);
        CHECK(v.dcFact == verdict::DcFact::None);
        CHECK(v.levelDb > verdict::kAudibleFloorDb && v.levelDb <= t.rawPeakDb);
    }

    // DONE-TEST 2b: the same tail, quieter (-66 dBFS): silent, said as a quiet tone, still not DC.
    {
        float const amp = std::pow(10.0f, -66.0f / 20.0f);
        auto const t = tapOf(tail(330.0, amp, 60.0, kWindow));
        auto const v = verdict::judge(t.rawPeakDb, t.ac, nullptr);
        CHECK(v.kind == Kind::Silent);
        CHECK(v.hasLevel);
        CHECK(v.levelDb <= verdict::kAudibleFloorDb);
        CHECK(v.kind != Kind::DcOnly);
    }

    // DONE-TEST 2c: a fading tail ON a DC offset about its size: a quiet tone, and the DC is said with it.
    {
        float const amp = std::pow(10.0f, -50.0f / 20.0f);
        auto sig = tail(330.0, amp, 60.0, kWindow);
        float const dc = std::pow(10.0f, -64.0f / 20.0f);
        for (auto& s : sig) s += dc;
        auto const t = tapOf(sig);
        auto const v = verdict::judge(t.rawPeakDb, t.ac, nullptr);
        std::printf("  tail+dc: ac %.1f dc %.1f -> %s share %d\n", t.ac.acRmsDb, t.ac.dcDb, verdict::kindName(v.kind), static_cast<int>(v.dcShare));
        CHECK(v.kind == Kind::Audible);
        CHECK(v.quiet);
        CHECK(v.dcShare == verdict::DcShare::Comparable);
    }

    // DONE-TEST 3: a hard square (crest 1.0, what the old server's shape rule called "flat: DC or a square, not counted").
    // The blocker sees its edges: a tone, audible. The level never exceeds the raw peak (a one-pole high-pass overshoots
    // a square's edge: its AC peak reads above the input).
    {
        auto const t = tapOf(square(110.0, 0.5f, kWindow));
        auto const v = verdict::judge(t.rawPeakDb, t.ac, nullptr);
        std::printf("  square: raw %.1f acPeak %.1f -> %s level %.1f\n", t.rawPeakDb, t.ac.acPeakDb, verdict::kindName(v.kind), v.levelDb);
        CHECK(v.kind == Kind::Audible);
        CHECK(v.tone);
        CHECK(t.ac.acPeakDb > t.rawPeakDb); // the overshoot the clamp is for
        CHECK(std::fabs(v.levelDb - t.rawPeakDb) < 1e-4f);
        CHECK(v.dcFact == verdict::DcFact::None);
        CHECK(!v.nearFullScale);
        auto const full = tapOf(square(110.0, 1.0f, kWindow));
        CHECK(verdict::judge(full.rawPeakDb, full.ac, nullptr).nearFullScale);
    }

    // Silence: the read peak decides and is the level said; the floor names what turned it down (each from a read).
    {
        auto const t = tapOf(std::vector<float>(static_cast<size_t>(kWindow), 0.0f));
        auto const v = verdict::judge(t.rawPeakDb, t.ac, nullptr);
        CHECK(v.kind == Kind::Silent);
        CHECK(v.hasLevel && v.levelDb == ac::kFloorDb);
        CHECK(std::string(v.reason).empty());

        verdict::Floor zero;
        zero.read = true;
        zero.masterVolume = 0.0f;
        zero.pdOutPeakDb = -12.0f;
        CHECK(std::string(verdict::judge(t.rawPeakDb, t.ac, &zero).reason) == "master_volume_zero");

        verdict::Floor down;
        down.read = true;
        down.masterVolume = 0.0005f;
        down.pdOutPeakDb = -12.0f;
        down.preLimiterPeakDb = -78.0f;
        CHECK(std::string(verdict::judge(t.rawPeakDb, t.ac, &down).reason) == "master_volume_down");

        verdict::Floor breaker;
        breaker.read = true;
        breaker.breakerTrips = 1;
        breaker.preCeilingPeakDb = -3.0f;
        auto const vb = verdict::judge(t.rawPeakDb, t.ac, &breaker);
        CHECK(std::string(vb.reason) == "ceiling_breaker");
        auto const json = verdict::toJson(vb, "final", &breaker);
        CHECK(has(json, "\"verdict\":\"silent\""));
        CHECK(has(json, "\"breakerTrips\":1"));
        CHECK(has(json, "\"reason\":\"ceiling_breaker\""));
        CHECK(has(json, "\"tap\":\"final\""));
    }

    // Unmeasured: no audio block ran, or the peak is not a number. Never silent.
    {
        ac::AcLevels const none;
        auto const v = verdict::judge(-100.0f, none, nullptr, false);
        CHECK(v.kind == Kind::Unmeasured);
        CHECK(std::string(v.reason) == "no_audio");
        CHECK(!v.hasLevel);
        auto const nan = verdict::judge(std::numeric_limits<float>::quiet_NaN(), none, nullptr);
        CHECK(nan.kind == Kind::Unmeasured);
        CHECK(std::string(nan.reason) == "no_level");
        auto const json = verdict::toJson(nan, "node", nullptr);
        CHECK(has(json, "\"verdict\":\"unmeasured\""));
        CHECK(has(json, "\"rawPeakDb\":null")); // never a NaN in the JSON
        CHECK(!has(json, "nan"));
        CHECK(!has(json, "breakerTrips")); // a node tap carries no floor
    }

    // REVIEW H1 (blocker): a hit at the very start of a FRESH tap (a node tap opens with a new blocker) fell inside the
    // blocker's settle period, its AC peak was skipped, and a real hit read "silent quiet_tone" next to tone:true.
    // H1a: a 30 ms 1 kHz burst at -6 dBFS at tap start, then silence (a 500 ms window).
    {
        auto sig = std::vector<float>(static_cast<size_t>(kWindow), 0.0f);
        float const amp = std::pow(10.0f, -6.0f / 20.0f);
        for (int i = 0; i < static_cast<int>(0.030 * kRate); ++i)
            sig[static_cast<size_t>(i)] = amp * static_cast<float>(std::sin(2.0 * kPi * 1000.0 * i / kRate));
        auto const t = tapOf(sig);
        auto const v = verdict::judge(t.rawPeakDb, t.ac, nullptr);
        std::printf("  H1a burst: raw %.1f ac %.1f/%.1f tone %d -> %s level %.1f\n", t.rawPeakDb, t.ac.acRmsDb, t.ac.acPeakDb, t.ac.tone,
            verdict::kindName(v.kind), v.levelDb);
        CHECK(v.kind == Kind::Audible);
        CHECK(v.levelDb > -30.0f && v.levelDb <= t.rawPeakDb);
    }
    // H1b: a 30 Hz tone at -6 dBFS in a 50 ms node window (fewer than two cycles: no tone flag, all inside the settle).
    {
        int const n = static_cast<int>(0.050 * kRate);
        std::vector<float> sig(static_cast<size_t>(n));
        float const amp = std::pow(10.0f, -6.0f / 20.0f);
        for (int i = 0; i < n; ++i) sig[static_cast<size_t>(i)] = amp * static_cast<float>(std::sin(2.0 * kPi * 30.0 * i / kRate));
        auto const t = tapOf(sig);
        auto const v = verdict::judge(t.rawPeakDb, t.ac, nullptr);
        std::printf("  H1b 30 Hz/50 ms: raw %.1f ac %.1f/%.1f dc %.1f tone %d -> %s level %.1f\n", t.rawPeakDb, t.ac.acRmsDb, t.ac.acPeakDb,
            t.ac.dcDb, t.ac.tone, verdict::kindName(v.kind), v.levelDb);
        CHECK(v.kind == Kind::Audible);
        CHECK(v.levelDb > -20.0f && v.levelDb <= t.rawPeakDb);
    }
    // H1c: a single-sample click inside the settle period of a tap that opened on silence (an exactly flat first block
    // primes exactly: nothing to settle).
    {
        std::vector<float> sig(static_cast<size_t>(kWindow), 0.0f);
        sig[500] = 0.5f;
        auto const t = tapOf(sig);
        auto const v = verdict::judge(t.rawPeakDb, t.ac, nullptr);
        std::printf("  H1c click: raw %.1f ac %.1f/%.1f -> %s level %.1f\n", t.rawPeakDb, t.ac.acRmsDb, t.ac.acPeakDb, verdict::kindName(v.kind), v.levelDb);
        CHECK(v.kind == Kind::Audible);
        CHECK(v.levelDb > -10.0f && v.levelDb <= t.rawPeakDb);
    }
    // H1d: the armed spectral window: a 40 ms 220 Hz pluck at its start. The final tap's blockers run on every block,
    // so the window opens on a settled (here: silent) blocker; the per-channel stats decide, as the bridge reads them.
    {
        ac::DcBlocker blocker;
        blocker.reset(ac::coefficientFor(kRate));
        ac::AcStats warm;
        std::vector<float> silence(kBlock * 20, 0.0f);
        for (size_t at = 0; at < silence.size(); at += kBlock) ac::accumulate(blocker, warm, silence.data() + at, kBlock);
        std::vector<float> sig(static_cast<size_t>(kWindow), 0.0f);
        float const amp = std::pow(10.0f, -6.0f / 20.0f);
        for (int i = 0; i < static_cast<int>(0.040 * kRate); ++i)
            sig[static_cast<size_t>(i)] = amp * static_cast<float>(std::exp(-i / (0.015 * kRate)) * std::sin(2.0 * kPi * 220.0 * i / kRate));
        ac::AcStats window;
        for (size_t at = 0; at < sig.size(); at += kBlock) ac::accumulate(blocker, window, sig.data() + at, kBlock);
        auto const v = verdict::judgeChannels(&window, 1, kRate, true);
        std::printf("  H1d spectral pluck: -> %s level %.1f\n", verdict::kindName(v.kind), v.levelDb);
        CHECK(v.kind == Kind::Audible);
        CHECK(v.levelDb > -12.0f && v.levelDb <= -6.0f + 0.01f);
    }
    // M2: the spectral verdict reads each OUTPUT channel on its own: antiphase stereo (a mono sum of 0) is not silent.
    {
        auto const left = square(110.0, 0.5f, kWindow);
        std::vector<float> right(left.size());
        for (size_t i = 0; i < left.size(); ++i) right[i] = -left[i];
        ac::AcStats ch[2];
        ac::DcBlocker b[2];
        for (auto& x : b) x.reset(ac::coefficientFor(kRate));
        for (size_t at = 0; at < left.size(); at += kBlock) {
            ac::accumulate(b[0], ch[0], left.data() + at, kBlock);
            ac::accumulate(b[1], ch[1], right.data() + at, kBlock);
        }
        auto const v = verdict::judgeChannels(ch, 2, kRate, true);
        CHECK(v.kind == Kind::Audible);
        CHECK(verdict::judgeChannels(nullptr, 0, kRate, true).kind == Kind::Unmeasured);
        // delta review 4: a device with more than 16 outputs whose AC taps are all quiet: the untapped channels may
        // carry the sound, so the window is unjudged, never silent; a hot tapped channel is still judged.
        ac::AcStats quiet[2];
        std::vector<float> zeros(static_cast<size_t>(kWindow), 0.0f);
        for (auto& q : quiet) {
            ac::DcBlocker z;
            z.reset(ac::coefficientFor(kRate));
            for (size_t at = 0; at < zeros.size(); at += kBlock) ac::accumulate(z, q, zeros.data() + at, kBlock);
        }
        CHECK(verdict::judgeChannels(quiet, 2, kRate, true).kind == Kind::Silent);
        auto const wide = verdict::judgeChannels(quiet, 2, kRate, true, false);
        CHECK(wide.kind == Kind::Unmeasured);
        CHECK(std::string(wide.reason) == "channels_unjudged");
        CHECK(verdict::judgeChannels(ch, 2, kRate, true, false).kind == Kind::Audible);
    }
    // M3: an LFO wired into the output (2 Hz sine at -6 dBFS over 1 s) is said as sub-audio (a fact; the verdict keeps
    // its rule); a 220 Hz tone is not.
    {
        int const n = static_cast<int>(kRate);
        std::vector<float> lfo(static_cast<size_t>(n));
        float const amp = std::pow(10.0f, -6.0f / 20.0f);
        for (int i = 0; i < n; ++i) lfo[static_cast<size_t>(i)] = amp * static_cast<float>(std::sin(2.0 * kPi * 2.0 * i / kRate));
        auto const t = tapOf(lfo);
        auto const v = verdict::judge(t.rawPeakDb, t.ac, nullptr, true, kRate);
        std::printf("  M3 lfo: swingHz %.2f -> %s subAudio %d\n", t.ac.swingHz(kRate), verdict::kindName(v.kind), v.subAudio);
        CHECK(v.subAudio);
        CHECK(has(verdict::toJson(v, "final", nullptr), "\"subAudio\":true"));
        auto const tone220 = tapOf(tail(220.0, amp, 1.0e9, kWindow));
        CHECK(!verdict::judge(tone220.rawPeakDb, tone220.ac, nullptr, true, kRate).subAudio);
    }
    // L1: an audible level is never printed at or under the floor.
    {
        ac::AcLevels lv;
        lv.tone = true;
        lv.acPeakDb = -59.97f; // above the floor by a hair: printed at -59.9, never -60.0
        lv.acRmsDb = -70.0f;
        auto const v = verdict::judge(-59.96f, lv, nullptr);
        CHECK(v.kind == Kind::Audible);
        CHECK(v.levelDb >= -59.9f);
    }
    // L2: no tone, DC under the AC level (dcShare below): the DC is no fact worth saying.
    {
        ac::AcLevels lv;
        lv.acRmsDb = -30.0f;
        lv.acPeakDb = -27.0f;
        lv.dcDb = -40.0f;
        auto const v = verdict::judge(-25.0f, lv, nullptr);
        CHECK(v.dcShare == verdict::DcShare::Below);
        CHECK(v.dcFact == verdict::DcFact::None);
    }
    // L4: master volume 0 is the reason only when Pd itself made sound before it.
    {
        verdict::Floor zero;
        zero.read = true;
        zero.masterVolume = 0.0f;
        zero.pdOutPeakDb = -100.0f;
        CHECK(std::string(verdict::judge(-100.0f, ac::AcLevels {}, &zero).reason).empty());
    }
    // L3: a hot read whose loudest channel the AC taps do not cover (channel 17 and up) is unjudged, never silent.
    {
        auto const v = verdict::judge(-6.0f, ac::AcLevels {}, nullptr, true, kRate, false);
        CHECK(v.kind == Kind::Unmeasured);
        CHECK(std::string(v.reason) == "channels_unjudged");
        CHECK(verdict::judge(-80.0f, ac::AcLevels {}, nullptr, true, kRate, false).kind == Kind::Silent);
    }

    if (failures == 0) std::printf("sound_verdict_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
