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
//    words: the server would have to say them.
//
// Threading: no locks here. The panel owns one TalkList and writes it only on the message thread (CopilotPanel::receiveMessage,
// called by the bridge chat callback after MCPBridge::postChatMessage's MessageManager::callAsync, and by the canvas on the
// artist's own clicks).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp {

struct TalkLine {
    std::uint64_t id = 0;  // unique and rising; a replaced line keeps its id (and its place)
    std::string role;      // "user", "ai" or "error"
    std::string text;      // trimmed
    std::string context;   // the widget an artist line was about ("ai.tap"), or empty
    bool waiting = false;  // an open "has not answered yet" line (role "ai")
};

enum class TalkChange { None, Appended, Replaced };

enum class TalkMark { None, Look, Ask };

class TalkList {
public:
    static constexpr std::size_t maxLines = 400;

    /** One chat line arrived. Says what happened to the list so the caller can repaint and decide on its "new line" dot. */
    TalkChange add(std::string_view role, std::string_view text, std::string_view context = {})
    {
        auto const trimmed = trim(text);
        if (trimmed.empty())
            return TalkChange::None;

        if (role == "user")
            return addUser(trimmed, context);
        if (role == "waiting")
            return addWaiting(trimmed, context);
        if (role == "ai" || role == "detail")
            return addAnswer("ai", trimmed, context);
        if (role == "error")
            return addAnswer("error", trimmed, context);
        return TalkChange::None; // thought, or a role nobody knows: never a line
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

    TalkChange addAnswer(char const* role, std::string const& text, std::string_view context)
    {
        auto const open = newestWaiting();
        if (open >= 0) {
            // The answer takes the waiting line's place.
            auto& line = lines_[static_cast<std::size_t>(open)];
            line.role = role;
            line.text = text;
            line.context = std::string(context);
            line.waiting = false;
            return TalkChange::Replaced;
        }
        if (!lines_.empty() && lines_.back().role == role && lines_.back().text == text)
            return TalkChange::None;
        return append(role, text, context, false);
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

/** The mark a line wears: error -> Look, an AI line that ends in `?` -> Ask, a waiting line or anything else -> None. */
inline TalkMark talkMark(TalkLine const& line)
{
    if (line.role == "error")
        return TalkMark::Look;
    if (line.waiting || line.role != "ai")
        return TalkMark::None;
    auto const text = TalkList::trim(line.text);
    return !text.empty() && text.back() == '?' ? TalkMark::Ask : TalkMark::None;
}

} // namespace mcp
