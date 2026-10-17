// Unit tests for the bridge's JSON string escaper (Plan 3 P3.2 S1, finding C6).
// Build + run: scripts/test-bridge-safety.sh
//
// Build with -DJSON_ESCAPE_LEGACY to run the same cases against a copy of the
// pre-S1 function (MCPBridge.cpp jsonEscape, which appended every byte as its
// own juce_wchar): it fails on every multi-byte case, which is the defect.
#include "../../Source/Pd/MCPJson.h"

#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

#ifdef JSON_ESCAPE_LEGACY
// What the old code produced once juce::String turned its wchars back into UTF-8:
// each byte >= 0x80 became the 2-byte encoding of U+0080..U+00FF. Stops at NUL.
static std::string escape(std::string_view in)
{
    std::string out;
    for (char const raw : in) {
        auto const c = static_cast<unsigned char>(raw);
        if (c == 0) break;
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else if (c < 0x80) {
                    out += static_cast<char>(c);
                } else {
                    out += static_cast<char>(0xC0 | (c >> 6));
                    out += static_cast<char>(0x80 | (c & 0x3F));
                }
        }
    }
    return out;
}
#else
static std::string escape(std::string_view in) { return mcp::json::jsonEscape(in); }
#endif

// Decodes what escape() produced (only the escapes it can emit). Returns false on a raw
// control byte, a raw quote, or an unknown escape: such output is not a valid JSON string body.
static bool unescape(std::string const& in, std::string& out)
{
    out.clear();
    for (size_t i = 0; i < in.size(); ++i) {
        auto const c = static_cast<unsigned char>(in[i]);
        if (c < 0x20 || c == '"') return false;
        if (c != '\\') { out += static_cast<char>(c); continue; }
        if (++i >= in.size()) return false;
        switch (in[i]) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                if (i + 4 >= in.size()) return false;
                out += static_cast<char>(std::stoi(in.substr(i + 1, 4), nullptr, 16));
                i += 4;
                break;
            }
            default: return false;
        }
    }
    return true;
}

int main()
{
    // ── the escapes ──
    CHECK(escape("") == "");
    CHECK(escape("plain~ 440") == "plain~ 440");
    CHECK(escape("a\"b") == "a\\\"b");
    CHECK(escape("a\\b") == "a\\\\b");
    CHECK(escape("\n") == "\\n");
    CHECK(escape("\r") == "\\r");
    CHECK(escape("\t") == "\\t");
    CHECK(escape("\x01") == "\\u0001");
    CHECK(escape("\x1f") == "\\u001f");
    CHECK(escape("\x7f") == "\x7f"); // DEL is legal unescaped in JSON

    // ── the tempId from the C6 report ──
    CHECK(escape("q\"x\\y") == "q\\\"x\\\\y");

    // ── UTF-8 passes through unchanged (the second C6 defect: it used to be mangled) ──
    CHECK(escape("\xC3\xBC") == "\xC3\xBC");                 // u-umlaut, 2 bytes
    CHECK(escape("\xE2\x82\xAC") == "\xE2\x82\xAC");         // euro sign, 3 bytes
    CHECK(escape("\xF0\x9F\x98\x80") == "\xF0\x9F\x98\x80"); // emoji, 4 bytes
    CHECK(escape("caf\xC3\xA9 \"x\"") == "caf\xC3\xA9 \\\"x\\\"");

    // ── an embedded NUL is data, not the end ──
    CHECK(escape(std::string_view("a\0b", 3)) == "a\\u0000b");

    // ── every ASCII byte round-trips and never leaves a raw control byte or quote ──
    for (int b = 0; b < 0x80; ++b) {
        std::string const in(1, static_cast<char>(b));
        std::string back;
        CHECK(unescape(escape(in), back));
        CHECK(back == in);
    }

#ifndef JSON_ESCAPE_LEGACY
    // ── the const char* overload (callers pass Pd symbols, which can be null) ──
    CHECK(mcp::json::jsonEscape(static_cast<char const*>(nullptr)) == "");
    CHECK(mcp::json::jsonEscape("a\"b") == "a\\\"b");
#endif

    if (failures == 0) std::printf("json_escape_test: ok\n");
    return failures == 0 ? 0 : 1;
}
