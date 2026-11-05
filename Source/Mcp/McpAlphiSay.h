/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/
#pragma once

// Alphi drop 1, step W: what Alphi says. JUCE-free (Tests/mcp/alphi_say_test.cpp).
//
// The server sends /pd/alphi_say <json {text, kind, state, turn, ttlMs}> (replyMode none, class mutate, effect ui:
// no lease, no reply, no Pd lock, no patch access, no flow effect). The handler (MCPBridge::pdAlphiSay in
// CanvasMcpAlphiSay.cpp) is the only writer of the board; the canvas reads it to draw (F3). The TS zod type
// (src/services/alphi-bubble.ts) mirrors alphiSayFrom's checks.
//
// - kind plain|busy|fail picks the colour; state idle|building|measuring|failed drives sparks / dots / spiral.
//   busy goes with building|measuring only; plain|fail with idle|failed only.
// - turn: the server's id of the AI turn ([A-Za-z0-9_.:-]{1,40}); ttlMs: an integer 100..60000.
// - Plain text: control, line-separator and bidi characters become spaces, invalid UTF-8 is dropped, the ends are
//   trimmed, and the text is cut at 90 whole code points. Nothing in it is ever interpreted (no markup).
// - Two slots (delta review N4): a busy say sets only the activity slot; a final say (idle|failed) sets the line slot
//   and clears activity. A busy say of a turn whose final line came already (the last 8 turns) is dropped, so a late refresh never
//   brings the sparks back after the line and never touches the line. Each slot drops itself after its ttlMs.

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace mcp {

enum class AlphiSayKind { Plain, Busy, Fail };
enum class AlphiSayPhase { Idle, Building, Measuring, Failed };

struct AlphiSayLimits {
    static constexpr std::size_t maxCodePoints = 90;
    static constexpr std::size_t maxTurnChars = 40;
    static constexpr double minTtlMs = 100;
    static constexpr double maxTtlMs = 60000;
    static constexpr std::size_t finishedTurnsKept = 8;
};

struct AlphiSay {
    std::string text;
    AlphiSayKind kind = AlphiSayKind::Plain;
    AlphiSayPhase phase = AlphiSayPhase::Idle;
    std::string turn;
    std::int64_t ttlMs = 0;
};

struct AlphiSaySlot {
    std::string text;
    AlphiSayKind kind = AlphiSayKind::Plain;
    AlphiSayPhase phase = AlphiSayPhase::Idle;
    std::string turn;
    std::int64_t expiresAtMs = 0;
};

namespace alphisay_detail {

// Decodes one UTF-8 code point at s[i]; returns its length, or 0 when the bytes there are not valid UTF-8 (overlong,
// surrogate, past U+10FFFF, cut off).
inline std::size_t decode(std::string const& s, std::size_t i, char32_t& cp)
{
    auto const b = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    auto const cont = [&](std::size_t k) { return k < s.size() && (b(k) & 0xC0) == 0x80; };
    unsigned char const c = b(i);
    if (c < 0x80) { cp = c; return 1; }
    std::size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 ? 2 : 0;
    if (len == 0 || c > 0xF4) return 0;
    for (std::size_t k = 1; k < len; ++k)
        if (!cont(i + k)) return 0;
    cp = c & (0x7F >> len);
    for (std::size_t k = 1; k < len; ++k)
        cp = (cp << 6) | (b(i + k) & 0x3F);
    bool const overlong = (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000);
    bool const bad = overlong || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF;
    return bad ? 0 : len;
}

// Characters that would steer the layout or hide text: C0/C1 controls, DEL, line/paragraph separators, bidi marks,
// embeddings, overrides and isolates.
inline bool shownAsSpace(char32_t cp)
{
    return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp == 0x061C || cp == 0x200E || cp == 0x200F
        || (cp >= 0x2028 && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069);
}

} // namespace alphisay_detail

// The plain text Alphi may show: cleaned, trimmed, cut at `maxCodePoints` whole code points.
inline std::string alphiSayText(std::string const& raw, std::size_t maxCodePoints = AlphiSayLimits::maxCodePoints)
{
    std::string out;
    std::size_t count = 0;
    for (std::size_t i = 0; i < raw.size() && count < maxCodePoints;) {
        char32_t cp = 0;
        std::size_t const len = alphisay_detail::decode(raw, i, cp);
        if (len == 0) { ++i; continue; }
        bool const space = alphisay_detail::shownAsSpace(cp) || cp == ' ';
        if (!(space && out.empty())) {
            out += space ? std::string(" ") : raw.substr(i, len);
            ++count;
        }
        i += len;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

inline std::optional<AlphiSayKind> alphiSayKindOf(std::string const& w)
{
    if (w == "plain") return AlphiSayKind::Plain;
    if (w == "busy") return AlphiSayKind::Busy;
    if (w == "fail") return AlphiSayKind::Fail;
    return std::nullopt;
}

inline std::optional<AlphiSayPhase> alphiSayPhaseOf(std::string const& w)
{
    if (w == "idle") return AlphiSayPhase::Idle;
    if (w == "building") return AlphiSayPhase::Building;
    if (w == "measuring") return AlphiSayPhase::Measuring;
    if (w == "failed") return AlphiSayPhase::Failed;
    return std::nullopt;
}

inline bool alphiSayBusy(AlphiSayPhase p) { return p == AlphiSayPhase::Building || p == AlphiSayPhase::Measuring; }

inline bool alphiSayTurnOk(std::string const& t)
{
    if (t.empty() || t.size() > AlphiSayLimits::maxTurnChars) return false;
    for (char c : t) {
        bool const ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.'
            || c == ':' || c == '-';
        if (!ok) return false;
    }
    return true;
}

inline bool alphiSayTtlOk(double ttl)
{
    return std::isfinite(ttl) && std::floor(ttl) == ttl && ttl >= AlphiSayLimits::minTtlMs && ttl <= AlphiSayLimits::maxTtlMs;
}

// A checked say, or nullopt when any field breaks the contract (the say is then dropped whole).
inline std::optional<AlphiSay> alphiSayFrom(std::string const& text, std::string const& kind, std::string const& state,
    std::string const& turn, double ttlMs)
{
    auto const k = alphiSayKindOf(kind);
    auto const p = alphiSayPhaseOf(state);
    if (!k || !p || (*k == AlphiSayKind::Busy) != alphiSayBusy(*p)) return std::nullopt;
    if (!alphiSayTurnOk(turn) || !alphiSayTtlOk(ttlMs)) return std::nullopt;
    auto clean = alphiSayText(text);
    if (clean.empty()) return std::nullopt;
    return AlphiSay { std::move(clean), *k, *p, turn, static_cast<std::int64_t>(ttlMs) };
}

// The board's clock: steady, so a wall-clock jump never keeps a say up or drops it early. The handler and the
// canvas both read the board with it.
inline std::int64_t alphiSayNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The two slots. Any thread (own lock): the handler writes, the canvas reads.
class AlphiSayBoard {
public:
    // Applies a checked say at `nowMs`; true when what is visible changed (the caller asks for a repaint then).
    bool apply(AlphiSay const& say, std::int64_t nowMs)
    {
        std::lock_guard<std::mutex> const lock(mutex);
        AlphiSaySlot slot { say.text, say.kind, say.phase, say.turn, nowMs + say.ttlMs };
        if (alphiSayBusy(say.phase)) {
            if (finished(say.turn)) return false;
            bool const changed = !sameVisible(activitySlot, slot, nowMs);
            activitySlot = std::move(slot);
            return changed;
        }
        bool const changed = !sameVisible(lineSlot, slot, nowMs) || live(activitySlot, nowMs);
        if (!finished(say.turn)) finishedTurns[nextFinished++ % finishedTurns.size()] = say.turn;
        lineSlot = std::move(slot);
        activitySlot.reset();
        return changed;
    }

    std::optional<AlphiSaySlot> activity(std::int64_t nowMs) const { return shown(activitySlot, nowMs); }
    std::optional<AlphiSaySlot> line(std::int64_t nowMs) const { return shown(lineSlot, nowMs); }

private:
    bool finished(std::string const& turn) const
    {
        for (auto const& t : finishedTurns)
            if (t == turn) return true;
        return false;
    }

    static bool live(std::optional<AlphiSaySlot> const& s, std::int64_t nowMs) { return s && nowMs < s->expiresAtMs; }

    static bool sameVisible(std::optional<AlphiSaySlot> const& old, AlphiSaySlot const& next, std::int64_t nowMs)
    {
        return live(old, nowMs) && old->text == next.text && old->kind == next.kind && old->phase == next.phase;
    }

    std::optional<AlphiSaySlot> shown(std::optional<AlphiSaySlot> const& s, std::int64_t nowMs) const
    {
        std::lock_guard<std::mutex> const lock(mutex);
        return live(s, nowMs) ? s : std::nullopt;
    }

    mutable std::mutex mutex;
    std::optional<AlphiSaySlot> activitySlot;
    std::optional<AlphiSaySlot> lineSlot;
    // The turns whose final line came (the last few): their busy refreshes are dropped. Turn ids are never empty.
    std::array<std::string, AlphiSayLimits::finishedTurnsKept> finishedTurns {};
    std::size_t nextFinished = 0;
};

} // namespace mcp
