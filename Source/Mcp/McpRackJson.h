// A small strict JSON reader for the Copilot rack's data file (Source/Mcp/McpRackFile.h). JUCE-free, no exceptions: RFC 8259 (no
// comments, no trailing commas), UTF-8 checked, nested at most kRackJsonMaxDepth deep, so any text ends in a value or an error line.
// Tests/mcp/widget_rack_test.cpp.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mcp {

inline constexpr int kRackJsonMaxDepth = 16;

namespace rackjson {

/** One JSON value; objects keep their keys in `keys`, parallel to `items`. */
struct Value {
    enum class Kind : std::uint8_t { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool boolean = false;
    std::string text; // a string's characters, a number's text
    std::vector<std::string> keys;
    std::vector<Value> items;

    Value const* find(std::string_view key) const
    {
        for (std::size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == key)
                return &items[i];
        return nullptr;
    }
};

/** True when `s` is well-formed UTF-8 (no overlong forms, no surrogates, nothing above U+10FFFF). */
inline bool isValidUtf8(std::string_view s)
{
    std::size_t i = 0;
    while (i < s.size()) {
        auto const c = static_cast<unsigned char>(s[i]);
        std::size_t extra = 0;
        std::uint32_t cp = 0;
        if (c < 0x80) {
            ++i;
            continue;
        } else if (c >= 0xC2 && c <= 0xDF) {
            extra = 1;
            cp = c & 0x1Fu;
        } else if (c >= 0xE0 && c <= 0xEF) {
            extra = 2;
            cp = c & 0x0Fu;
        } else if (c >= 0xF0 && c <= 0xF4) {
            extra = 3;
            cp = c & 0x07u;
        } else {
            return false;
        }
        if (i + extra >= s.size())
            return false;
        for (std::size_t k = 1; k <= extra; ++k) {
            auto const cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if ((extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += extra + 1;
    }
    return true;
}

inline void appendUtf8(std::string& out, std::uint32_t cp)
{
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

/** A small strict JSON reader (RFC 8259: no comments, no trailing commas), nested at most kRackJsonMaxDepth deep. */
class Parser {
public:
    explicit Parser(std::string_view text)
        : s(text)
    {
    }

    /** True and `out` filled when the whole text is one JSON value; else false and `error` says where and why. */
    bool parse(Value& out, std::string& error)
    {
        if (s.substr(0, 3) == "\xEF\xBB\xBF") // a byte order mark, as some editors write
            pos = 3;
        skipSpace();
        if (value(out, 0)) {
            skipSpace();
            if (pos == s.size())
                return true;
            fail("text after the end of the JSON");
        }
        error = err;
        return false;
    }

private:
    std::string_view s;
    std::size_t pos = 0;
    std::string err;

    bool fail(char const* why)
    {
        if (err.empty()) {
            auto const line = 1 + std::count(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(std::min(pos, s.size())), '\n');
            err = std::string("line ") + std::to_string(line) + ": " + why;
        }
        return false;
    }

    void skipSpace()
    {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r'))
            ++pos;
    }

    bool literal(std::string_view word)
    {
        if (s.substr(pos, word.size()) != word)
            return fail("unknown word");
        pos += word.size();
        return true;
    }

    bool digits()
    {
        auto const start = pos;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9')
            ++pos;
        return pos > start;
    }

    bool number(Value& out)
    {
        auto const start = pos;
        if (pos < s.size() && s[pos] == '-')
            ++pos;
        if (pos < s.size() && s[pos] == '0')
            ++pos;
        else if (!digits())
            return fail("a number was expected");
        if (pos < s.size() && s[pos] == '.') {
            ++pos;
            if (!digits())
                return fail("digits were expected after the point");
        }
        if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
            ++pos;
            if (pos < s.size() && (s[pos] == '+' || s[pos] == '-'))
                ++pos;
            if (!digits())
                return fail("digits were expected in the exponent");
        }
        out.kind = Value::Kind::Number;
        out.text = std::string(s.substr(start, pos - start));
        return true;
    }

    bool hex4(std::uint32_t& cp)
    {
        if (pos + 4 > s.size())
            return fail("a \\u escape is cut short");
        cp = 0;
        for (int i = 0; i < 4; ++i) {
            char const c = s[pos++];
            cp <<= 4;
            if (c >= '0' && c <= '9')
                cp |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                cp |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                cp |= static_cast<std::uint32_t>(c - 'A' + 10);
            else
                return fail("a \\u escape needs four hex digits");
        }
        return true;
    }

    bool unicodeEscape(std::string& out)
    {
        std::uint32_t cp = 0;
        if (!hex4(cp))
            return false;
        if (cp >= 0xD800 && cp <= 0xDBFF) { // a high surrogate needs its low half
            std::uint32_t low = 0;
            if (s.substr(pos, 2) != "\\u")
                return fail("a lone surrogate in a \\u escape");
            pos += 2;
            if (!hex4(low))
                return false;
            if (low < 0xDC00 || low > 0xDFFF)
                return fail("a lone surrogate in a \\u escape");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return fail("a lone surrogate in a \\u escape");
        }
        if (cp == 0)
            return fail("\\u0000 is not allowed");
        appendUtf8(out, cp);
        return true;
    }

    bool escape(std::string& out)
    {
        if (pos >= s.size())
            return fail("a string is cut short");
        char const c = s[pos++];
        switch (c) {
        case '"': out.push_back('"'); return true;
        case '\\': out.push_back('\\'); return true;
        case '/': out.push_back('/'); return true;
        case 'b': out.push_back('\b'); return true;
        case 'f': out.push_back('\f'); return true;
        case 'n': out.push_back('\n'); return true;
        case 'r': out.push_back('\r'); return true;
        case 't': out.push_back('\t'); return true;
        case 'u': return unicodeEscape(out);
        default: return fail("an unknown escape in a string");
        }
    }

    bool string(std::string& out)
    {
        ++pos; // the opening quote
        while (pos < s.size()) {
            auto const c = static_cast<unsigned char>(s[pos++]);
            if (c == '"')
                return true;
            if (c < 0x20)
                return fail("a control character in a string");
            if (c == '\\') {
                if (!escape(out))
                    return false;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
        return fail("a string is not closed");
    }

    bool array(Value& out, int depth)
    {
        out.kind = Value::Kind::Array;
        ++pos;
        skipSpace();
        if (pos < s.size() && s[pos] == ']') {
            ++pos;
            return true;
        }
        for (;;) {
            out.items.emplace_back();
            if (!value(out.items.back(), depth + 1))
                return false;
            skipSpace();
            if (pos < s.size() && s[pos] == ',') {
                ++pos;
                skipSpace();
            } else if (pos < s.size() && s[pos] == ']') {
                ++pos;
                return true;
            } else {
                return fail("a , or ] was expected");
            }
        }
    }

    bool object(Value& out, int depth)
    {
        out.kind = Value::Kind::Object;
        ++pos;
        skipSpace();
        if (pos < s.size() && s[pos] == '}') {
            ++pos;
            return true;
        }
        for (;;) {
            if (pos >= s.size() || s[pos] != '"')
                return fail("a key in quotes was expected");
            out.keys.emplace_back();
            if (!string(out.keys.back()))
                return false;
            skipSpace();
            if (pos >= s.size() || s[pos] != ':')
                return fail("a : was expected");
            ++pos;
            skipSpace();
            out.items.emplace_back();
            if (!value(out.items.back(), depth + 1))
                return false;
            skipSpace();
            if (pos < s.size() && s[pos] == ',') {
                ++pos;
                skipSpace();
            } else if (pos < s.size() && s[pos] == '}') {
                ++pos;
                return true;
            } else {
                return fail("a , or } was expected");
            }
        }
    }

    bool value(Value& out, int depth)
    {
        if (depth >= kRackJsonMaxDepth)
            return fail("nested too deep");
        if (pos >= s.size())
            return fail("a value was expected");
        char const c = s[pos];
        if (c == '{')
            return object(out, depth);
        if (c == '[')
            return array(out, depth);
        if (c == '"') {
            out.kind = Value::Kind::String;
            return string(out.text);
        }
        if (c == 't') {
            out.kind = Value::Kind::Bool;
            out.boolean = true;
            return literal("true");
        }
        if (c == 'f') {
            out.kind = Value::Kind::Bool;
            return literal("false");
        }
        if (c == 'n')
            return literal("null");
        return number(out);
    }
};

} // namespace rackjson

} // namespace mcp
