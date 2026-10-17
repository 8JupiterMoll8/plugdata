/*
 // JSON string escaping for the MCP bridge's hand-built replies (Plan 3 P3.2 S1, finding C6).
 // Kept free of JUCE and Pd so it can be unit-tested with plain g++:
 // see Tests/mcp/json_escape_test.cpp and scripts/test-bridge-safety.sh.
 */
#pragma once

#include <cstdio>
#include <string>
#include <string_view>

namespace mcp::json {

// Escapes `utf8` for use between the quotes of a JSON string: `"`, `\`, \n, \r, \t and every
// other control byte (as \u00xx). Bytes >= 0x80 are UTF-8 and pass through unchanged; an
// embedded NUL is data, so it is escaped too.
inline std::string jsonEscape(std::string_view utf8)
{
    std::string out;
    out.reserve(utf8.size() + 8);
    for (char const raw : utf8) {
        auto const c = static_cast<unsigned char>(raw);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += raw;
                }
        }
    }
    return out;
}

// Pd symbols and C strings: null is an empty string.
inline std::string jsonEscape(char const* s)
{
    return s ? jsonEscape(std::string_view(s)) : std::string();
}

} // namespace mcp::json
