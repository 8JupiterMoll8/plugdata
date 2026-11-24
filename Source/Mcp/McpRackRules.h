// The rules one rack.json entry must meet before a card is made of it. JUCE-free (Tests/mcp/widget_rack_test.cpp). Why the patch rules
// are strict: a drag pastes the text through Patch::translatePatchAsString, which ends EVERY LINE with `;` and hands the lines to
// Pd's paste. `#X` is the canvas the card lands on, so `#X clear;` would empty it; a `,` repeats the target (`#X obj 0 0 f, clear`
// is `#X obj 0 0 f` then `#X clear`); a line break inside a statement makes its tail a statement of its own (`pd dsp 1`). So a patch is
// a list of whole lines, one statement each, and only the words that build a patch may follow `#X`.
#pragma once

#include "McpWidgetRack.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace mcp {

inline constexpr std::size_t kRackNameMax = 40;
inline constexpr std::size_t kRackPurposeMax = 160;
inline constexpr std::size_t kRackObjectMax = 200;
inline constexpr std::size_t kRackPatchMax = 32 * 1024;

inline bool rackIsControl(char const c)
{
    return static_cast<unsigned char>(c) < 0x20 || c == 0x7f;
}

/** "" when `name` is a good card name, else why not. */
inline std::string rackNameProblem(std::string_view name)
{
    if (name.empty())
        return "the name is empty";
    if (name.size() > kRackNameMax)
        return "the name is longer than 40 characters";
    auto const alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
    if (!alnum(name.front()))
        return "the name must start with a letter or a digit";
    for (char const c : name)
        if (!alnum(c) && c != '.' && c != '_' && c != '~' && c != '-')
            return "the name may only use letters, digits and . _ ~ -";
    return {};
}

inline std::string rackPurposeProblem(std::string_view purpose)
{
    if (purpose.empty())
        return "the purpose is empty";
    if (purpose.size() > kRackPurposeMax)
        return "the purpose is longer than 160 characters";
    for (char const c : purpose)
        if (rackIsControl(c))
            return "the purpose has a control character or a line break";
    return {};
}

/** The `#X` words a pasted patch may use (what a saved patch holds): `f` is the box width Pd writes as `, f 20`. */
inline constexpr std::string_view kRackStatementWords[] = { "obj", "msg", "text", "floatatom", "symbolatom", "listbox", "connect", "restore", "coords", "array", "declare", "f" };

/** The position of the first `target` that no backslash escapes, or npos. */
inline std::size_t rackFindUnescaped(std::string_view s, char const target)
{
    bool escaped = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (escaped)
            escaped = false;
        else if (s[i] == '\\')
            escaped = true;
        else if (s[i] == target)
            return i;
    }
    return std::string_view::npos;
}

inline std::string_view rackTrim(std::string_view t)
{
    auto const first = t.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    return t.substr(first, t.find_last_not_of(" \t\r\n") - first + 1);
}

inline bool rackAllDigits(std::string_view t)
{
    if (t.empty())
        return false;
    for (char const c : t)
        if (c < '0' || c > '9')
            return false;
    return true;
}

/** One object text for `#X obj 0 0 <object>;`: no `;` (it ends the statement), no `,` (it repeats the target), no `$`, no
 *  backslash (it could escape the closing `;`), no line break. */
inline std::string rackObjectProblem(std::string_view object)
{
    if (object.empty())
        return "the object is empty";
    if (object.size() > kRackObjectMax)
        return "the object is longer than 200 characters";
    for (char const c : object) {
        if (rackIsControl(c))
            return "the object has a control character or a line break";
        if (c == ';' || c == ',' || c == '$' || c == '\\')
            return "the object has a ; , $ or backslash (use patch for that)";
    }
    return {};
}

/** An unescaped `,` may only start the box width Pd writes after a box: `, f <digits>` to the end of the statement. */
inline bool rackCommaIsWidth(std::string_view statement)
{
    auto const comma = rackFindUnescaped(statement, ',');
    if (comma == std::string_view::npos)
        return true;
    auto tail = rackTrim(statement.substr(comma + 1));
    if (tail.size() < 3 || tail[0] != 'f' || (tail[1] != ' ' && tail[1] != '\t'))
        return false;
    return rackAllDigits(rackTrim(tail.substr(1)));
}

/** `#A` array data: numbers and nothing else (a word first would be a message to the array). */
inline bool rackIsArrayData(std::string_view rest)
{
    bool any = false;
    std::size_t pos = 0;
    while (pos < rest.size()) {
        auto const end = rest.find_first_of(" \t", pos);
        auto const token = rest.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
        pos = end == std::string_view::npos ? rest.size() : end + 1;
        if (token.empty())
            continue;
        if (token.find_first_not_of("0123456789+-.eE") != std::string_view::npos || token.find_first_of("0123456789+-.") != 0)
            return false;
        any = true;
    }
    return any;
}

/** One line of a patch, without its closing `;`. `first`: nothing came before; `depth`: the `#N canvas` still open. */
inline std::string rackStatementProblem(std::string_view st, bool const first, int& depth)
{
    if (st.substr(0, 10) == "#N canvas ") {
        if (rackFindUnescaped(st, ',') != std::string_view::npos)
            return "a , in the patch may only come before f <width>";
        ++depth;
        return {};
    }
    if (st.substr(0, 3) == "#X ") {
        auto rest = st.substr(3);
        rest.remove_prefix(std::min(rest.find_first_not_of(' '), rest.size()));
        auto const word = rest.substr(0, rest.find_first_of(" \t"));
        bool allowed = false;
        for (auto const w : kRackStatementWords)
            allowed = allowed || word == w;
        if (!allowed)
            return "a line of the patch uses a #X word that is not allowed (obj msg text floatatom symbolatom listbox connect restore coords array declare f)";
        if (word == "restore") {
            if (depth == 0)
                return "the patch has a #X restore without its #N canvas";
            --depth;
        }
        return rackCommaIsWidth(st) ? std::string() : std::string("a , in the patch may only come before f <width>");
    }
    if (!first && st.substr(0, 3) == "#A ")
        return rackIsArrayData(st.substr(3)) ? std::string() : std::string("#A array data may only hold numbers");
    return first ? "the patch must start with #X or #N canvas" : "a line of the patch does not start with #X, #N canvas or #A";
}

/**
 * A whole patch text, as the drop will treat it: every non-blank LINE is one statement that ends with an unescaped `;` (no line
 * break inside a statement, one statement per line, so the drop's `;` per line adds nothing); only `#X <allowed word>`, `#N canvas`
 * and numeric `#A` lines; the first line starts with `#X ` or `#N canvas `; `#N canvas` and `#X restore` pair up; an unescaped `,`
 * only as `, f <width>`. An escaped `\;` inside a msg box stays: it runs only when the artist clicks that box.
 */
inline std::string rackPatchProblem(std::string_view patch)
{
    if (patch.empty())
        return "the patch is empty";
    if (patch.size() > kRackPatchMax)
        return "the patch is longer than 32 KB";
    for (char const c : patch)
        if (rackIsControl(c) && c != '\n' && c != '\r' && c != '\t')
            return "the patch has a control character";
    int depth = 0;
    int statements = 0;
    std::size_t pos = 0;
    while (pos < patch.size()) {
        auto const end = patch.find_first_of("\r\n", pos);
        auto const line = rackTrim(patch.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos));
        pos = end == std::string_view::npos ? patch.size() : end + 1;
        if (line.empty())
            continue;
        if (rackFindUnescaped(line, ';') != line.size() - 1)
            return "a line of the patch must be one whole statement ending with ; (no line break inside one, one per line)";
        auto const problem = rackStatementProblem(line.substr(0, line.size() - 1), statements == 0, depth);
        if (!problem.empty())
            return problem;
        ++statements;
    }
    if (statements == 0)
        return "the patch has no statement";
    return depth == 0 ? std::string() : std::string("the patch has a #N canvas without its #X restore");
}

} // namespace mcp
