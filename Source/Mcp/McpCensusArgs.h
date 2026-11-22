// The census's args for an abstraction box (live check 2026-11-21), the JUCE-free part. `[ai.loop~ a]` used to arrive as
// type "pd", node_class "ai.loop~.pd", args ["ai.loop~.pd"]: the creation args after the file name were lost, so `list`
// could not read a looper's name from the box. The args are now the file first (every consumer already reads args[0] as the
// file), then the box's own args, tokenised the way the census tokenises every other box (blanks split, a double-quoted span
// is one token with its quotes). A box with no args gives the file alone: the old answer, unchanged.
// Used by McpSysCommands.cpp (the census). Tested by Tests/mcp/census_args_test.cpp.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mcp {

/** The box text split into tokens: blanks (space, tab, CR, LF) separate, a double-quoted span is one token (quotes kept,
 *  an unclosed quote runs to the end of the text). */
inline std::vector<std::string> censusBoxTokens(std::string_view text)
{
    std::vector<std::string> tokens;
    std::string current;
    bool quoted = false;
    bool have = false;
    for (char const c : text) {
        if (quoted) {
            current += c;
            if (c == '"') quoted = false;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (have) tokens.push_back(std::move(current));
            current.clear();
            have = false;
            continue;
        }
        current += c;
        have = true;
        if (c == '"') quoted = true;
    }
    if (have) tokens.push_back(std::move(current));
    return tokens;
}

/** args for an abstraction row: `file`, then every token of the box text after its first word (the class). An empty file
 *  gives nothing (a plain [pd foo] keeps the caller's own answer). */
inline std::vector<std::string> abstractionCensusArgs(std::string_view file, std::string_view boxText)
{
    std::vector<std::string> args;
    if (file.empty()) return args;
    args.emplace_back(file);
    auto const tokens = censusBoxTokens(boxText);
    for (std::size_t i = 1; i < tokens.size(); ++i) args.push_back(tokens[i]);
    return args;
}

} // namespace mcp
