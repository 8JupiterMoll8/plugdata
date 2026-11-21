/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/
// The Copilot panel's talk list (design round 2026-11-19, step C1): the lines of the talk tab, as a notebook. JUCE-free, so
// Tests/mcp/copilot_talk_test.cpp compiles it alone; Source/Sidebar/CopilotPanel.h draws it.
//
//  - Roles in: `user` (the artist), `ai`, `detail` (the long answer, stored as `ai`), `error`, `waiting` and `thought`.
//    A line has the role `user`, `ai` or `error`; `waiting` is an `ai` line with the flag `waiting` set.
//  - `waiting` ("Claude Code has not answered yet") adds a line marked waiting. The next `ai`, `detail` or `error` line
//    REPLACES the newest waiting line in place (same position, same id): the answer takes the place of the waiting line, it is
//    not appended below it. A second waiting line while one is open replaces the first.
//  - A `user` line equal to the last user line is dropped (the echo of the artist's own prompt: the panel adds its own line
//    with the context first). A line equal to the newest line of the same role is dropped.
//  - `thought` (the live working status) never becomes a line; unknown roles and blank text neither.
//  - At most maxLines (400) lines, the oldest dropped.
//  - talkMark: the one mark a line wears, only where the software knows it: error -> Look, an AI line ending in `?` -> Ask.
//    A waiting line has none (it is drawn as a dotted grey line of text). "Measured" and "changed" are never guessed from the
//    words: the server says them.
//  - The server's mark (design round 2026-11-19, "who is who"): an `ai` / `detail` line may carry a third word on /chat/msg
//    that the server computed from the tool results of THAT turn (Source of truth: the server's services/talk-mark.ts):
//        changed=<n>   a construct of the turn returned ok, with n changes      changed   the same, count not known
//        explained     the turn only drew annotate / region tags                measured=<dB>   a reading after the last change
//    joined by a space. parseTalkWork reads it (a malformed token is ignored: no guess); the line keeps it as `work`; talkMark
//    turns it into the mark shape (changed = red circle, explained = blue outline square, measured = blue square) and
//    talkHeaderText into the grey line above the answer ("alphi · changed 4 · measured -14 dB").
//  - The widget marks (ai_widget plan, phase 3; the server's talk line per widget event, capability widget_event): `used` (the artist
//    took Alphi's proposal on a widget) and `refused` (the artist turned it down). Exact words only (`used=2`, `Used`, `unused` are no
//    mark); both at once say nothing about the artist (no guess); a change of the turn still leads, then the artist's answer, then a
//    guess, then a reading. The header says "alphi · used" / "alphi · refused".
//  - Who is who: talkGroups cuts the list into groups, one artist ask and the answers after it; talkHeaderShown says whether an
//    AI line carries its grey header (the first of a run, or a line that knows something new); talkContextLabel names the box an
//    artist line was about ("random 8", "ai.tap", "hsl +2").
//  - Canvas tags never become talk lines: the server draws tags with /pd/ai_annotate and sends them nowhere near /chat/msg, and
//    a click on a tag adds no line; only the roles above make a line, so a tag, a note card or the HUD arriving with any other
//    role (tag, annotation, hud, ...) is dropped here too.
//  - An answer is stored whole: nothing in this model cuts a line (the view wraps it); the pill's own one-line answer is cut
//    elsewhere (McpPillAnswer.h).
//
// Threading: no locks here. The panel owns one TalkList and writes it only on the message thread (CopilotPanel::receiveMessage,
// called by the bridge chat callback after MCPBridge::postChatMessage's MessageManager::callAsync, and by the canvas on the
// artist's own clicks).
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp {

/** What a turn proved, as the server says it (the third word of /chat/msg on an AI line); None = nothing is known. */
enum class TalkWorkKind { None, Changed, Explained, Measured, Used, Refused };

struct TalkWork {
    TalkWorkKind kind = TalkWorkKind::None;
    int changes = 0;      // Changed: how many changes, 0 = the count is not known
    bool hasDb = false;   // a reading of the turn
    double db = 0.0;      // its level, dBFS (-100 is a silent reading)

    bool operator==(TalkWork const& o) const { return kind == o.kind && changes == o.changes && hasDb == o.hasDb && db == o.db; }
    bool operator!=(TalkWork const& o) const { return !(*this == o); }
};

namespace talkdetail {

/** All of `s` is an unsigned decimal number of at most 5 digits (no sign, no exponent); false for anything else. */
inline bool parseCount(std::string_view s, int& out)
{
    if (s.empty() || s.size() > 5)
        return false;
    int n = 0;
    for (char c : s) {
        if (c < '0' || c > '9')
            return false;
        n = n * 10 + (c - '0');
    }
    out = n;
    return true;
}

/** `[-]digits[.digits]` inside [-200, 40]; no exponent, no nan or inf. Hand parsed: no locale. */
inline bool parseDb(std::string_view s, double& out)
{
    bool negative = false;
    std::size_t i = 0;
    if (i < s.size() && s[i] == '-') {
        negative = true;
        ++i;
    }
    std::size_t whole = 0;
    double value = 0.0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        value = value * 10.0 + static_cast<double>(s[i] - '0');
        ++i;
        ++whole;
    }
    if (whole == 0 || whole > 4)
        return false;
    if (i < s.size() && s[i] == '.') {
        ++i;
        double scale = 0.1;
        std::size_t frac = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            value += scale * static_cast<double>(s[i] - '0');
            scale *= 0.1;
            ++i;
            ++frac;
        }
        if (frac == 0 || frac > 3)
            return false;
    }
    if (i != s.size())
        return false;
    if (negative)
        value = -value;
    if (value < -200.0 || value > 40.0)
        return false;
    out = value;
    return true;
}

} // namespace talkdetail

/** Reads the server's word: space separated tokens `changed=<n>`, `changed`, `explained`, `measured=<dB>`, `used`, `refused`, in any
 *  order. A token that does not parse is ignored (never guessed). A change beats everything; then the artist's answer (`used` or
 *  `refused`; both at once are no answer), then an explanation; with none of them, a reading alone is Measured. */
inline TalkWork parseTalkWork(std::string_view wire)
{
    TalkWork out;
    bool changed = false;
    bool explained = false;
    bool used = false;
    bool refused = false;
    int changes = 0;
    std::size_t i = 0;
    while (i < wire.size()) {
        while (i < wire.size() && (wire[i] == ' ' || wire[i] == '\t' || wire[i] == '\n' || wire[i] == '\r'))
            ++i;
        std::size_t const start = i;
        while (i < wire.size() && !(wire[i] == ' ' || wire[i] == '\t' || wire[i] == '\n' || wire[i] == '\r'))
            ++i;
        auto const token = wire.substr(start, i - start);
        if (token.empty())
            continue;
        if (token == "changed") {
            changed = true;
        } else if (token.substr(0, 8) == "changed=") {
            int n = 0;
            if (talkdetail::parseCount(token.substr(8), n) && n > 0) {
                changed = true;
                changes = n;
            }
        } else if (token == "explained") {
            explained = true;
        } else if (token == "used") {
            used = true;
        } else if (token == "refused") {
            refused = true;
        } else if (token.substr(0, 9) == "measured=") {
            double db = 0.0;
            if (talkdetail::parseDb(token.substr(9), db)) {
                out.hasDb = true;
                out.db = db;
            }
        }
    }
    if (changed) {
        out.kind = TalkWorkKind::Changed;
        out.changes = changes;
    } else if (used != refused) {
        out.kind = used ? TalkWorkKind::Used : TalkWorkKind::Refused;
    } else if (explained) {
        out.kind = TalkWorkKind::Explained;
    } else if (out.hasDb) {
        out.kind = TalkWorkKind::Measured;
    }
    return out;
}

struct TalkLine {
    std::uint64_t id = 0;  // unique and rising; a replaced line keeps its id (and its place)
    std::string role;      // "user", "ai" or "error"
    std::string text;      // trimmed
    std::string context;   // the box an artist line was about ("random 8", "ai.tap"), or empty
    bool waiting = false;  // an open "has not answered yet" line (role "ai")
    TalkWork work;         // what the server's tool results proved for this answer (role "ai" only), None = unknown
};

enum class TalkChange { None, Appended, Replaced };

enum class TalkMark { None, Look, Ask, Changed, Explained, Measured, Used, Refused };

class TalkList {
public:
    static constexpr std::size_t maxLines = 400;

    /** One chat line arrived. Says what happened to the list so the caller can repaint and decide on its "new line" dot.
     *  `workWire` is the server's third word (parseTalkWork); only an ai / detail line keeps it. */
    TalkChange add(std::string_view role, std::string_view text, std::string_view context = {}, std::string_view workWire = {})
    {
        auto const trimmed = trim(text);
        if (trimmed.empty())
            return TalkChange::None;

        if (role == "user")
            return addUser(trimmed, context);
        if (role == "waiting")
            return addWaiting(trimmed, context);
        if (role == "ai" || role == "detail")
            return addAnswer("ai", trimmed, context, parseTalkWork(workWire));
        if (role == "error")
            return addAnswer("error", trimmed, context, TalkWork {});
        return TalkChange::None; // thought, a canvas tag (tag, annotation, hud, ...), or a role nobody knows: never a line
    }

    std::vector<TalkLine> const& lines() const { return lines_; }
    std::size_t size() const { return lines_.size(); }
    bool empty() const { return lines_.empty(); }
    bool hasOpenWaiting() const { return newestWaiting() >= 0; }

    void clear() { lines_.clear(); }

    static std::string trim(std::string_view s)
    {
        auto const isBlank = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
        std::size_t b = 0;
        std::size_t e = s.size();
        while (b < e && isBlank(s[b]))
            ++b;
        while (e > b && isBlank(s[e - 1]))
            --e;
        return std::string(s.substr(b, e - b));
    }

private:
    TalkChange addUser(std::string const& text, std::string_view context)
    {
        for (auto it = lines_.rbegin(); it != lines_.rend(); ++it) {
            if (it->role == "user") {
                if (it->text == text)
                    return TalkChange::None; // the echo of a line the panel already has
                break;
            }
        }
        return append("user", text, context, false);
    }

    TalkChange addWaiting(std::string const& text, std::string_view context)
    {
        auto const open = newestWaiting();
        if (open < 0)
            return append("ai", text, context, true);
        auto& line = lines_[static_cast<std::size_t>(open)];
        if (line.text == text)
            return TalkChange::None;
        line.text = text;
        line.context = std::string(context);
        return TalkChange::Replaced;
    }

    TalkChange addAnswer(char const* role, std::string const& text, std::string_view context, TalkWork const& work)
    {
        auto const open = newestWaiting();
        if (open >= 0) {
            // The answer takes the waiting line's place.
            auto& line = lines_[static_cast<std::size_t>(open)];
            line.role = role;
            line.text = text;
            line.context = std::string(context);
            line.waiting = false;
            line.work = work;
            return TalkChange::Replaced;
        }
        if (!lines_.empty() && lines_.back().role == role && lines_.back().text == text) {
            // The same words again: dropped, unless this copy knows what the turn proved and the first one did not.
            if (work.kind != TalkWorkKind::None && lines_.back().work.kind == TalkWorkKind::None) {
                lines_.back().work = work;
                return TalkChange::Replaced;
            }
            return TalkChange::None;
        }
        auto const change = append(role, text, context, false);
        lines_.back().work = work;
        return change;
    }

    TalkChange append(char const* role, std::string const& text, std::string_view context, bool waiting)
    {
        TalkLine line;
        line.id = ++lastId_;
        line.role = role;
        line.text = text;
        line.context = std::string(context);
        line.waiting = waiting;
        lines_.push_back(std::move(line));
        if (lines_.size() > maxLines)
            lines_.erase(lines_.begin(), lines_.begin() + static_cast<std::ptrdiff_t>(lines_.size() - maxLines));
        return TalkChange::Appended;
    }

    /** Index of the newest open waiting line, -1 when none. */
    long newestWaiting() const
    {
        for (std::size_t i = lines_.size(); i > 0; --i)
            if (lines_[i - 1].waiting)
                return static_cast<long>(i - 1);
        return -1;
    }

    std::vector<TalkLine> lines_;
    std::uint64_t lastId_ = 0;
};

/** The mark a line wears. What the server proved leads (changed = red circle, explained = blue outline square, measured = blue
 *  square, used / refused = the artist's answer to a widget proposal); then: error -> Look, an AI line that ends in `?` -> Ask; a waiting line or anything else -> None. */
inline TalkMark talkMark(TalkLine const& line)
{
    if (line.role == "ai" && !line.waiting) {
        switch (line.work.kind) {
        case TalkWorkKind::Changed: return TalkMark::Changed;
        case TalkWorkKind::Explained: return TalkMark::Explained;
        case TalkWorkKind::Measured: return TalkMark::Measured;
        case TalkWorkKind::Used: return TalkMark::Used;
        case TalkWorkKind::Refused: return TalkMark::Refused;
        case TalkWorkKind::None: break;
        }
    }
    if (line.role == "error")
        return TalkMark::Look;
    if (line.waiting || line.role != "ai")
        return TalkMark::None;
    auto const text = TalkList::trim(line.text);
    return !text.empty() && text.back() == '?' ? TalkMark::Ask : TalkMark::None;
}

/** One group of the talk: an artist ask and the lines that follow it, up to the next ask. A run of lines before any ask (a
 *  saved picture, an old answer) is a group without an ask. `first` and `count` index TalkList::lines(). */
struct TalkGroup {
    std::size_t first = 0;
    std::size_t count = 0;
    bool hasAsk = false;
};

inline std::vector<TalkGroup> talkGroups(std::vector<TalkLine> const& lines)
{
    std::vector<TalkGroup> groups;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        bool const ask = lines[i].role == "user";
        if (ask || groups.empty()) {
            TalkGroup g;
            g.first = i;
            g.hasAsk = ask;
            groups.push_back(g);
        }
        ++groups.back().count;
    }
    return groups;
}

/** A level as the header says it: whole dB when it is whole, else one decimal ("-18", "-14.2"); a silent reading says so. */
inline std::string talkDbText(double db)
{
    if (db <= -99.5)
        return "silent";
    long const tenths = std::lround(db * 10.0);
    long const magnitude = tenths < 0 ? -tenths : tenths;
    std::string out = tenths < 0 ? "-" : "";
    out += std::to_string(magnitude / 10);
    if (magnitude % 10 != 0)
        out += "." + std::to_string(magnitude % 10);
    return out + " dB";
}

/** The grey line above an AI answer: "alphi", then what the turn proved ("changed 4", "explained", "measured -14 dB") joined by
 *  a middle dot; "asks" for a line that ends in a question, "look here" for an error. Nothing extra when nothing is known. */
inline std::string talkHeaderText(TalkLine const& line)
{
    std::string const dot = " \xC2\xB7 ";
    std::string out = "alphi";
    if (line.role == "error")
        return out + dot + "look here";
    switch (line.work.kind) {
    case TalkWorkKind::Changed:
        out += dot + "changed";
        if (line.work.changes > 0)
            out += " " + std::to_string(line.work.changes);
        break;
    case TalkWorkKind::Explained: out += dot + "explained"; break;
    case TalkWorkKind::Used: out += dot + "used"; break;
    case TalkWorkKind::Refused: out += dot + "refused"; break;
    case TalkWorkKind::Measured: break;
    case TalkWorkKind::None:
        if (talkMark(line) == TalkMark::Ask)
            out += dot + "asks";
        break;
    }
    if (line.work.hasDb)
        out += dot + "measured " + talkDbText(line.work.db);
    return out;
}

/** Does the line at `index` carry its grey header? Only an AI or error line, never a waiting one (it is a dotted lead). The first
 *  line after an ask (or at the head) does; so does a line of another role than the one before it; a later line of the same run
 *  only when it knows something the one before did not. */
inline bool talkHeaderShown(std::vector<TalkLine> const& lines, std::size_t index)
{
    if (index >= lines.size())
        return false;
    auto const& line = lines[index];
    if (line.role == "user" || line.waiting)
        return false;
    if (index == 0)
        return true;
    auto const& before = lines[index - 1];
    if (before.role == "user" || before.waiting || before.role != line.role)
        return true;
    return line.work.kind != TalkWorkKind::None && line.work != before.work;
}

/** The label under which an artist line names the box it was about, from the text of the first selected box and how many are
 *  selected: the whole text when it is short ("random 8"), else its class ("hsl"); an ai.* widget is always its name; "+n" for
 *  the other boxes of a bigger selection. Empty when there is no text. */
inline std::string talkContextLabel(std::string_view objectText, std::size_t selectedCount)
{
    std::vector<std::string> tokens;
    std::size_t i = 0;
    while (i < objectText.size()) {
        while (i < objectText.size() && (objectText[i] == ' ' || objectText[i] == '\t' || objectText[i] == '\n' || objectText[i] == '\r'))
            ++i;
        std::size_t const start = i;
        while (i < objectText.size() && !(objectText[i] == ' ' || objectText[i] == '\t' || objectText[i] == '\n' || objectText[i] == '\r'))
            ++i;
        if (i > start)
            tokens.emplace_back(objectText.substr(start, i - start));
    }
    if (tokens.empty())
        return {};
    std::string joined;
    for (auto const& t : tokens)
        joined += (joined.empty() ? "" : " ") + t;
    bool const widget = tokens[0].rfind("ai.", 0) == 0;
    std::string label = (!widget && tokens.size() <= 3 && joined.size() <= 24) ? joined : tokens[0];
    if (selectedCount > 1)
        label += " +" + std::to_string(selectedCount - 1);
    return label;
}

} // namespace mcp
