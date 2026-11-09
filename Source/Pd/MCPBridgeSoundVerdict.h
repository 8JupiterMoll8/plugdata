/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Truth slice 2: ONE sound verdict per meter read. The server used to decide audible / silent / "only DC" itself from
// the raw peak, the AC levels and (for older forks) the SHAPE of a reading (crest, spectral rolloff), in about ten
// places that each picked their own number (server docs/review/truth-layers-audit-2026-11-08.md). The bridge now says
// it once, from the samples it holds, and the server only words it.
//
// The block (JSON, `"sound"` in the trace / spectral replies, /meter/read arg 21, /meter/result arg 8; capability
// meter_verdict):
//   verdict      audible | silent | dc_only | unmeasured
//   levelDb      the level that DECIDED: the tone's AC peak (never above the raw peak), or the raw peak when silent;
//                null for dc_only and unmeasured
//   tap          final (the device output) | node (one outlet)
//   tone         the AC taps' tone flag (AC RMS above the floor AND two full cycles)
//   quiet        no tone flag, but not DC either: a quiet or fading tone (its AC peak decides)
//   rawPeakDb, acRmsDb, acPeakDb, dcDb   the facts it was judged from (dB, -100 floor; rawPeakDb null when unread)
//   dcShare      the DC against the AC RMS: below | comparable (under +6 dB) | dominant (+6 dB and more)
//   dcFact       a DC offset worth saying: none | alone (no tone) | with_tone (within 6 dB of the tone's level or above)
//   nearFullScale  the raw peak is at or above -1 dBFS
//   reason       "" | no_audio | no_level | dc_dominant | quiet_tone | master_volume_zero | master_volume_down |
//                ceiling_breaker
//   final tap only: clamped, nonFinite, breakerTrips, breakerEngaged, limiterDb, masterVolume (the window's floor)
//
// The rules (what the server decided until now, with the bridge's own AC taps; MCPBridgeAcLevel.h):
//   no audio block in the window         unmeasured (no_audio); a non-finite peak: unmeasured (no_level)
//   raw peak at or under -60 dBFS         silent at the raw peak; the floor names what turned it down
//   no tone and DC >= AC RMS + 6 dB       dc_only (a DC offset is no sound)
//   else                                  level = min(AC peak, raw peak): audible above -60, else silent (quiet_tone)
// The -60 dBFS floor is the server's truth-policy floor (TS MASTER_AUDIBLE_DBFS); keep the two equal.
//
// JUCE-free and Pd-free (Tests/mcp/sound_verdict_test.cpp runs it with g++ alone). Not for the audio thread: toJson
// allocates; judge() alone is O(1).
#pragma once

#include "MCPBridgeAcLevel.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace mcp::verdict {

constexpr float kAudibleFloorDb = -60.0f;
constexpr float kDcDominanceDb = 6.0f;
constexpr float kDcNearToneDb = 6.0f;
constexpr float kNearFullScaleDb = -1.0f;

enum class Kind { Audible, Silent, DcOnly, Unmeasured };
enum class DcShare { Below, Comparable, Dominant };
enum class DcFact { None, Alone, WithTone };

/** The final tap's window floor (/meter/read args 5-16). A node tap has none. */
struct Floor {
    bool read = false;
    float masterVolume = 1.0f;          // linear gain of PlugData's master slider
    float pdOutPeakDb = ac::kFloorDb;   // Pd's own output before the master volume
    float preLimiterPeakDb = ac::kFloorDb;
    float preCeilingPeakDb = ac::kFloorDb;
    uint64_t clamped = 0;
    uint64_t nonFinite = 0;
    uint64_t breakerTrips = 0;
    bool breakerEngaged = false;
    float limiterReductionDb = 0.0f;
};

struct Verdict {
    Kind kind = Kind::Unmeasured;
    bool hasLevel = false;
    float levelDb = ac::kFloorDb;
    bool tone = false;
    bool quiet = false;
    bool rawRead = false;
    float rawPeakDb = ac::kFloorDb;
    float acRmsDb = ac::kFloorDb;
    float acPeakDb = ac::kFloorDb;
    float dcDb = ac::kFloorDb;
    DcShare dcShare = DcShare::Below;
    DcFact dcFact = DcFact::None;
    bool nearFullScale = false;
    char const* reason = "";
};

inline char const* kindName(Kind k)
{
    switch (k) {
    case Kind::Audible: return "audible";
    case Kind::Silent: return "silent";
    case Kind::DcOnly: return "dc_only";
    case Kind::Unmeasured: return "unmeasured";
    }
    return "unmeasured";
}

inline char const* shareName(DcShare s)
{
    return s == DcShare::Dominant ? "dominant" : s == DcShare::Comparable ? "comparable" : "below";
}

inline char const* factName(DcFact f)
{
    return f == DcFact::WithTone ? "with_tone" : f == DcFact::Alone ? "alone" : "none";
}

inline bool hot(float db) { return std::isfinite(db) && db > kAudibleFloorDb; }

/** What silenced a read the floor saw (each only from a read; nothing inferred). */
inline char const* silentReason(Floor const* floor)
{
    if (!floor || !floor->read) return "";
    if (floor->masterVolume == 0.0f) return "master_volume_zero";
    if (hot(floor->pdOutPeakDb) && !hot(floor->preLimiterPeakDb)) return "master_volume_down";
    if ((floor->breakerTrips > 0 || floor->breakerEngaged) && hot(floor->preCeilingPeakDb)) return "ceiling_breaker";
    return "";
}

/** The verdict of one tap: its raw peak (dBFS), its AC levels, the floor (final tap only), and whether audio ran. */
inline Verdict judge(float rawPeakDb, ac::AcLevels const& lv, Floor const* floor, bool audioRan = true)
{
    Verdict v;
    v.tone = lv.tone;
    v.acRmsDb = lv.acRmsDb;
    v.acPeakDb = lv.acPeakDb;
    v.dcDb = lv.dcDb;
    float const dcOverAc = lv.dcDb - lv.acRmsDb;
    v.dcShare = dcOverAc >= kDcDominanceDb ? DcShare::Dominant : dcOverAc > 0.0f ? DcShare::Comparable : DcShare::Below;
    if (!audioRan) {
        v.reason = "no_audio";
        return v;
    }
    if (!std::isfinite(rawPeakDb)) {
        v.reason = "no_level";
        return v;
    }
    v.rawRead = true;
    v.rawPeakDb = std::max(rawPeakDb, ac::kFloorDb);
    v.nearFullScale = rawPeakDb >= kNearFullScaleDb;
    if (!hot(rawPeakDb)) {
        v.kind = Kind::Silent;
        v.hasLevel = true;
        v.levelDb = v.rawPeakDb;
        v.reason = silentReason(floor);
        return v;
    }
    if (!lv.tone && v.dcShare == DcShare::Dominant) {
        v.kind = Kind::DcOnly;
        v.reason = "dc_dominant";
        v.dcFact = hot(lv.dcDb) ? DcFact::Alone : DcFact::None;
        return v;
    }
    v.quiet = !lv.tone;
    v.hasLevel = true;
    v.levelDb = std::min(lv.acPeakDb, v.rawPeakDb);
    v.kind = hot(v.levelDb) ? Kind::Audible : Kind::Silent;
    if (v.kind == Kind::Silent) v.reason = "quiet_tone";
    if (hot(lv.dcDb)) {
        if (!lv.tone) v.dcFact = DcFact::Alone;
        else if (lv.dcDb >= v.levelDb - kDcNearToneDb) v.dcFact = DcFact::WithTone;
    }
    return v;
}

/** The verdict of one captured buffer (the armed spectral window): the blocker runs over it in Pd-sized blocks. */
inline Verdict judgeSamples(float const* data, int n, double sampleRate)
{
    if (!data || n <= 0) return judge(std::nanf(""), ac::AcLevels {}, nullptr);
    ac::DcBlocker blocker;
    blocker.reset(ac::coefficientFor(sampleRate));
    ac::AcStats stats;
    for (int at = 0; at < n; at += 64)
        ac::accumulate(blocker, stats, data + at, std::min(64, n - at));
    if (stats.count == 0) return judge(std::nanf(""), ac::AcLevels {}, nullptr);
    return judge(ac::ampToDb(stats.maxAbs), ac::levelsOf(stats), nullptr);
}

inline std::string dbJson(bool read, float db)
{
    if (!read || !std::isfinite(db)) return "null";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", static_cast<double>(std::max(db, ac::kFloorDb)));
    return buf;
}

/** The block as one JSON object; `tap` is "final" or "node"; the floor fields only with a floor. */
inline std::string toJson(Verdict const& v, char const* tap, Floor const* floor)
{
    auto const b = [](bool x) { return std::string(x ? "true" : "false"); };
    std::string out = "{\"verdict\":\"" + std::string(kindName(v.kind)) + "\""
        + ",\"levelDb\":" + dbJson(v.hasLevel, v.levelDb)
        + ",\"tap\":\"" + std::string(tap ? tap : "node") + "\""
        + ",\"tone\":" + b(v.tone) + ",\"quiet\":" + b(v.quiet)
        + ",\"rawPeakDb\":" + dbJson(v.rawRead, v.rawPeakDb)
        + ",\"acRmsDb\":" + dbJson(true, v.acRmsDb) + ",\"acPeakDb\":" + dbJson(true, v.acPeakDb)
        + ",\"dcDb\":" + dbJson(true, v.dcDb)
        + ",\"dcShare\":\"" + shareName(v.dcShare) + "\",\"dcFact\":\"" + factName(v.dcFact) + "\""
        + ",\"nearFullScale\":" + b(v.nearFullScale)
        + ",\"reason\":\"" + std::string(v.reason ? v.reason : "") + "\"";
    if (floor) {
        char buf[200];
        std::snprintf(buf, sizeof buf, ",\"clamped\":%llu,\"nonFinite\":%llu,\"breakerTrips\":%llu,\"breakerEngaged\":%s,\"limiterDb\":%.1f,\"masterVolume\":%.4f",
            static_cast<unsigned long long>(floor->clamped), static_cast<unsigned long long>(floor->nonFinite),
            static_cast<unsigned long long>(floor->breakerTrips), floor->breakerEngaged ? "true" : "false",
            std::isfinite(floor->limiterReductionDb) ? static_cast<double>(floor->limiterReductionDb) : 0.0,
            std::isfinite(floor->masterVolume) ? static_cast<double>(floor->masterVolume) : 0.0);
        out += buf;
    }
    return out + "}";
}

} // namespace mcp::verdict
