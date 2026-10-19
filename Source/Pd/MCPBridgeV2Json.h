/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Minimal JSON for the v2 frames (Plan 4 P4.1 S2). JUCE-free so the codec can be unit-tested with g++
// (Tests/mcp/v2_codec_test.cpp). The input is untrusted (any local process can connect before the token check),
// so the parser is strict: RFC 8259 grammar only, valid UTF-8 only (a juce::String built from bad UTF-8 asserts),
// no duplicate object keys, nesting at most kMaxDepth, at most `maxNodes` values per document (a Value is ~100
// bytes: the budget bounds memory and time, S2 review H1/H2). Numbers ignore the C locale (Pd and JUCE may change
// LC_NUMERIC): std::from_chars / std::to_chars, or classic-locale streams where libc++ lacks the floating-point
// overloads (macOS before 13.3; MCP_V2_PORTABLE_FLOAT forces that path for testing on Linux).
#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace mcp::v2::json {

inline constexpr int kMaxDepth = 64;
inline constexpr std::size_t kMaxMembers = 1024; // per object; envelopes have a dozen
inline constexpr std::size_t kMaxNodes = 262144;  // per document: ~25 MB of Values at most

#if defined(__APPLE__) || defined(MCP_V2_PORTABLE_FLOAT)
#define MCP_V2_STREAM_FLOAT 1
#endif

struct Value {
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Value> items;     // Array elements, or Object values
    std::vector<std::string> keys; // Object keys, parallel to `items`

    bool is(Kind k) const { return kind == k; }

    // The member `key` of an object, or nullptr.
    Value const* find(std::string_view key) const
    {
        if (kind != Kind::Object)
            return nullptr;
        for (std::size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == key)
                return &items[i];
        return nullptr;
    }
};

// Length of the UTF-8 sequence starting at s[i] (1..4), or 0 if it is not a valid, shortest-form scalar value.
inline std::size_t utf8SequenceLength(std::string_view s, std::size_t i)
{
    auto const b = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    auto const cont = [&](std::size_t k) { return k < s.size() && (b(k) & 0xC0) == 0x80; };
    unsigned char const c = b(i);
    if (c < 0x80)
        return 1;
    if (c >= 0xC2 && c <= 0xDF)
        return cont(i + 1) ? 2 : 0;
    if (c >= 0xE0 && c <= 0xEF) {
        if (!cont(i + 1) || !cont(i + 2))
            return 0;
        if (c == 0xE0 && b(i + 1) < 0xA0)
            return 0; // overlong
        if (c == 0xED && b(i + 1) >= 0xA0)
            return 0; // UTF-16 surrogate
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (!cont(i + 1) || !cont(i + 2) || !cont(i + 3))
            return 0;
        if (c == 0xF0 && b(i + 1) < 0x90)
            return 0; // overlong
        if (c == 0xF4 && b(i + 1) >= 0x90)
            return 0; // above U+10FFFF
        return 4;
    }
    return 0;
}

inline bool validUtf8(std::string_view s)
{
    for (std::size_t i = 0; i < s.size();) {
        auto const n = utf8SequenceLength(s, i);
        if (n == 0)
            return false;
        i += n;
    }
    return true;
}

// At most `maxBytes` of `s`, never ending inside a UTF-8 sequence (s is valid UTF-8, or the cut stops at bad bytes).
inline std::string truncateUtf8(std::string_view s, std::size_t maxBytes)
{
    std::size_t end = 0;
    while (end < s.size()) {
        auto const n = utf8SequenceLength(s, end);
        if (n == 0 || end + n > maxBytes)
            break;
        end += n;
    }
    return std::string(s.substr(0, end));
}

inline void appendUtf8(std::string& out, std::uint32_t cp)
{
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

namespace detail {

class Parser {
public:
    Parser(std::string_view text, std::size_t nodeBudget)
        : s(text)
        , nodesLeft(nodeBudget)
    {
    }

    std::optional<Value> run(std::string& error)
    {
        Value v;
        skipSpace();
        if (!value(v, 0) || (skipSpace(), pos != s.size())) {
            if (message.empty())
                message = "trailing characters";
            error = message + " at byte " + std::to_string(pos);
            return std::nullopt;
        }
        return v;
    }

private:
    bool fail(char const* what)
    {
        if (message.empty())
            message = what;
        return false;
    }

    void skipSpace()
    {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r'))
            ++pos;
    }

    bool literal(char const* word)
    {
        auto const n = std::strlen(word);
        if (s.substr(pos, n) != word)
            return fail("bad literal");
        pos += n;
        return true;
    }

    bool value(Value& v, int depth)
    {
        if (depth > kMaxDepth)
            return fail("nested too deeply");
        if (nodesLeft == 0)
            return fail("too many values");
        --nodesLeft;
        if (pos >= s.size())
            return fail("unexpected end");
        switch (s[pos]) {
        case '{':
            return object(v, depth);
        case '[':
            return array(v, depth);
        case '"':
            v.kind = Value::Kind::String;
            return string(v.string);
        case 't':
            v.kind = Value::Kind::Bool;
            v.boolean = true;
            return literal("true");
        case 'f':
            v.kind = Value::Kind::Bool;
            return literal("false");
        case 'n':
            return literal("null");
        default:
            v.kind = Value::Kind::Number;
            return number(v.number);
        }
    }

    bool object(Value& v, int depth)
    {
        v.kind = Value::Kind::Object;
        ++pos;
        skipSpace();
        if (pos < s.size() && s[pos] == '}') {
            ++pos;
            return true;
        }
        for (;;) {
            skipSpace();
            std::string key;
            if (pos >= s.size() || s[pos] != '"' || !string(key))
                return fail("expected a key");
            if (v.keys.size() >= kMaxMembers)
                return fail("too many members");
            skipSpace();
            if (pos >= s.size() || s[pos] != ':')
                return fail("expected ':'");
            ++pos;
            skipSpace();
            Value member;
            if (!value(member, depth + 1))
                return false;
            v.keys.push_back(std::move(key));
            v.items.push_back(std::move(member));
            skipSpace();
            if (pos < s.size() && s[pos] == ',') {
                ++pos;
                continue;
            }
            if (pos < s.size() && s[pos] == '}') {
                ++pos;
                return uniqueKeys(v.keys);
            }
            return fail("expected ',' or '}'");
        }
    }

    // Sorted copy, adjacent compare: O(k log k) per object, so the whole document stays O(n log n).
    bool uniqueKeys(std::vector<std::string> const& keys)
    {
        if (keys.size() < 2)
            return true;
        std::vector<std::string_view> sorted(keys.begin(), keys.end());
        std::sort(sorted.begin(), sorted.end());
        if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
            return fail("duplicate key");
        return true;
    }

    bool array(Value& v, int depth)
    {
        v.kind = Value::Kind::Array;
        ++pos;
        skipSpace();
        if (pos < s.size() && s[pos] == ']') {
            ++pos;
            return true;
        }
        for (;;) {
            skipSpace();
            Value item;
            if (!value(item, depth + 1))
                return false;
            v.items.push_back(std::move(item));
            skipSpace();
            if (pos < s.size() && s[pos] == ',') {
                ++pos;
                continue;
            }
            if (pos < s.size() && s[pos] == ']') {
                ++pos;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    bool hex4(std::uint32_t& out)
    {
        if (pos + 4 > s.size())
            return fail("bad \\u escape");
        out = 0;
        for (int k = 0; k < 4; ++k) {
            char const c = s[pos++];
            out <<= 4;
            if (c >= '0' && c <= '9')
                out |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                out |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                out |= static_cast<std::uint32_t>(c - 'A' + 10);
            else
                return fail("bad \\u escape");
        }
        return true;
    }

    bool string(std::string& out)
    {
        ++pos; // opening quote
        for (;;) {
            if (pos >= s.size())
                return fail("unterminated string");
            auto const c = static_cast<unsigned char>(s[pos]);
            if (c == '"') {
                ++pos;
                return true;
            }
            if (c < 0x20)
                return fail("control character in string");
            if (c != '\\') {
                auto const n = utf8SequenceLength(s, pos);
                if (n == 0)
                    return fail("invalid UTF-8");
                out.append(s.substr(pos, n));
                pos += n;
                continue;
            }
            if (++pos >= s.size())
                return fail("unterminated string");
            char const e = s[pos++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                std::uint32_t cp = 0;
                if (!hex4(cp))
                    return false;
                if (cp >= 0xDC00 && cp <= 0xDFFF)
                    return fail("lone low surrogate");
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    std::uint32_t lo = 0;
                    if (pos + 2 > s.size() || s[pos] != '\\' || s[pos + 1] != 'u')
                        return fail("lone high surrogate");
                    pos += 2;
                    if (!hex4(lo))
                        return false;
                    if (lo < 0xDC00 || lo > 0xDFFF)
                        return fail("lone high surrogate");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                appendUtf8(out, cp);
                break;
            }
            default:
                return fail("bad escape");
            }
        }
    }

    bool number(double& out)
    {
        auto const start = pos;
        auto const digit = [&](std::size_t k) { return k < s.size() && s[k] >= '0' && s[k] <= '9'; };
        if (pos < s.size() && s[pos] == '-')
            ++pos;
        if (!digit(pos))
            return fail("bad number");
        if (s[pos] == '0') {
            ++pos;
            if (digit(pos))
                return fail("leading zero");
        } else {
            while (digit(pos))
                ++pos;
        }
        if (pos < s.size() && s[pos] == '.') {
            ++pos;
            if (!digit(pos))
                return fail("bad number");
            while (digit(pos))
                ++pos;
        }
        if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
            ++pos;
            if (pos < s.size() && (s[pos] == '+' || s[pos] == '-'))
                ++pos;
            if (!digit(pos))
                return fail("bad number");
            while (digit(pos))
                ++pos;
        }
#if MCP_V2_STREAM_FLOAT
        std::istringstream in(std::string(s.substr(start, pos - start)));
        in.imbue(std::locale::classic());
        in >> out;
        if (in.fail() || !std::isfinite(out))
            return fail("number out of range");
#else
        auto const [end, ec] = std::from_chars(s.data() + start, s.data() + pos, out);
        if (ec != std::errc() || end != s.data() + pos || !std::isfinite(out))
            return fail("number out of range");
#endif
        return true;
    }

    std::string_view s;
    std::size_t pos = 0;
    std::size_t nodesLeft;
    std::string message;
};

} // namespace detail

// The parsed document, or nullopt with `error` set ("<what> at byte <n>").
inline std::optional<Value> parse(std::string_view text, std::string& error, std::size_t maxNodes = kMaxNodes)
{
    return detail::Parser(text, maxNodes).run(error);
}

// A JSON string literal for UTF-8 text: '"', '\\' and control characters are escaped, the rest passes through.
inline std::string quote(std::string_view text)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(text.size() + 2);
    out += '"';
    for (char ch : text) {
        auto const c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                out += "\\u00";
                out += hex[c >> 4];
                out += hex[c & 0xF];
            } else {
                out += ch;
            }
        }
    }
    out += '"';
    return out;
}

// Shortest text that reads back as the same float32; null for NaN and infinity (JSON has no such numbers).
inline std::string floatText(float f)
{
    if (!std::isfinite(f))
        return "null";
#if MCP_V2_STREAM_FLOAT
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(9) << f; // 9 significant digits read back as the same float32
    return out.str();
#else
    char buf[32];
    auto const [end, ec] = std::to_chars(buf, buf + sizeof buf, f);
    return ec == std::errc() ? std::string(buf, end) : std::string("null");
#endif
}

} // namespace mcp::v2::json
