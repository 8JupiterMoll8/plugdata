// The canvas pill's answer line (overnight queue 2026-11-16, Batch 2 item 3 follow-up): the newest AI chat line
// (`/chat/msg ai <text>`), shown for a few seconds as ONE short line where the artist typed, instead of only in the
// Copilot sidebar. JUCE-free, so Tests/mcp/pill_answer_test.cpp compiles it alone.
//
//  - only role `ai` sets the line: `detail` (the long answer, sidebar only), `thought` (the working status) and
//    `error` leave it alone; role `user` (the artist sent the next prompt) clears it;
//  - the line is plain text on one line: control characters and line breaks become spaces, runs of spaces collapse,
//    backticks (markdown code marks) are dropped, and it is cut at `maxCodePoints` whole code points with "...";
//    the drawing code cuts again by pixel width;
//  - the line holds `holdMs`, then fades out over `fadeMs` (about 8 s in all); a new line restarts the clock.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace mcp {

class PillAnswer {
public:
    static constexpr std::int64_t holdMs = 7000;
    static constexpr std::int64_t fadeMs = 1000;
    static constexpr std::size_t maxCodePoints = 160;

    /** One chat line arrived. True when what the pill shows changed (the caller repaints and runs the fade clock). */
    bool onChat(std::string_view role, std::string_view text, std::int64_t nowMs)
    {
        if (role == "user") {
            if (line_.empty())
                return false;
            line_.clear();
            return true;
        }
        if (role != "ai")
            return false;
        line_ = oneLine(text, maxCodePoints);
        shownAtMs_ = nowMs;
        return true;
    }

    /** The text to draw (empty: nothing to draw). */
    std::string const& line() const { return line_; }

    /** 1 while the line holds, falling to 0 over the fade, 0 when there is no line or it is over. */
    float alpha(std::int64_t nowMs) const
    {
        if (line_.empty())
            return 0.0f;
        std::int64_t const age = nowMs - shownAtMs_;
        if (age < 0 || age <= holdMs)
            return 1.0f;
        if (age >= holdMs + fadeMs)
            return 0.0f;
        return 1.0f - static_cast<float>(age - holdMs) / static_cast<float>(fadeMs);
    }

    bool visible(std::int64_t nowMs) const { return alpha(nowMs) > 0.0f; }

    void clear() { line_.clear(); }

    /** Plain one-line text, cut at `limit` whole code points (the cut gets "..."). */
    static std::string oneLine(std::string_view text, std::size_t limit)
    {
        std::string out;
        out.reserve(text.size());
        bool pendingSpace = false;
        bool cut = false;
        std::size_t points = 0;
        for (std::size_t i = 0; i < text.size();) {
            std::size_t const len = codePointLength(text, i);
            auto const piece = text.substr(i, len);
            i += len;
            if (isSpaceLike(piece)) {
                pendingSpace = true;
                continue;
            }
            if (piece == "`")
                continue;
            std::size_t const need = (pendingSpace && !out.empty()) ? 2 : 1;
            if (points + need > limit) {
                cut = true;
                break;
            }
            if (need == 2)
                out.push_back(' ');
            out.append(piece);
            points += need;
            pendingSpace = false;
        }
        if (cut)
            out += "...";
        return out;
    }

private:
    /** Bytes of the code point at `i` (a cut-off sequence at the end keeps its bytes together; never reads past the end). */
    static std::size_t codePointLength(std::string_view text, std::size_t i)
    {
        unsigned char const c = static_cast<unsigned char>(text[i]);
        std::size_t len = 1;
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        return i + len > text.size() ? text.size() - i : len;
    }

    /** ASCII whitespace and controls, DEL, the C1 controls and the line / paragraph separators U+2028 / U+2029. */
    static bool isSpaceLike(std::string_view piece)
    {
        unsigned char const c = static_cast<unsigned char>(piece[0]);
        if (piece.size() == 1)
            return c <= 0x20 || c == 0x7F;
        if (piece.size() == 2 && c == 0xC2)
            return static_cast<unsigned char>(piece[1]) >= 0x80 && static_cast<unsigned char>(piece[1]) <= 0x9F;
        if (piece.size() == 3 && c == 0xE2 && static_cast<unsigned char>(piece[1]) == 0x80)
            return static_cast<unsigned char>(piece[2]) == 0xA8 || static_cast<unsigned char>(piece[2]) == 0xA9;
        return false;
    }

    std::string line_;
    std::int64_t shownAtMs_ = 0;
};

} // namespace mcp
