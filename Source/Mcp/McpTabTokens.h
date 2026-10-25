/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/
// P11.5 V24: the tab token, the receiver namespace of an open tab (`[r kick_p48]`), computed in C++ and answered in
// every /pd/list_tabs row as `ns`. JUCE-free (Tests/mcp/tab_token_test.cpp).
//
// The default is the stem hash the TS used before (src/core/tab-namespace.ts tabSuffix): the key's last path element,
// a trailing ".pd" dropped (any case), every UTF-16 unit that is not an ASCII letter or digit replaced by "_", FNV-1a
// 32 over the result, "p" + (hash mod 100). Patches saved with `_pNN` receiver names therefore keep their names.
// Only when two OPEN tabs end up with the same token does the later one (in the order given: oldest first) get the
// next free "pNN"; that token is remembered while the tab stays open and the caller persists it in the tab's identity
// sidecar (`ns`), which is passed back here as `persisted` on the next open.
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace mcp {

// "p" followed by one or more digits: the only shape a token has (the TS strips `_p\d+` suffixes).
inline bool isTabToken(std::string const& s)
{
    if (s.size() < 2 || s[0] != 'p')
        return false;
    for (size_t i = 1; i < s.size(); ++i)
        if (s[i] < '0' || s[i] > '9')
            return false;
    return true;
}

// The stem as the TS hashes it: UTF-8 in, one "_" per non-alphanumeric UTF-16 unit (two for a code point above
// U+FFFF, a surrogate pair), ASCII letters and digits kept.
inline std::string tabStem(std::string const& key)
{
    std::string base = key.empty() ? std::string("tab") : key;
    auto const slash = base.find_last_of('/');
    if (slash != std::string::npos)
        base = base.substr(slash + 1);
    if (base.empty())
        base = "tab";
    if (base.size() >= 3) {
        auto const tail = base.substr(base.size() - 3);
        if (tail[0] == '.' && (tail[1] == 'p' || tail[1] == 'P') && (tail[2] == 'd' || tail[2] == 'D'))
            base = base.substr(0, base.size() - 3);
    }
    std::string out;
    for (size_t i = 0; i < base.size();) {
        auto const c = static_cast<unsigned char>(base[i]);
        if (c < 0x80) {
            bool const alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            out += alnum ? static_cast<char>(c) : '_';
            ++i;
            continue;
        }
        int len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        if (i + static_cast<size_t>(len) > base.size())
            len = 1; // truncated sequence: one unit
        out += (len == 4) ? "__" : "_";
        i += static_cast<size_t>(len);
    }
    return out;
}

inline std::string defaultTabToken(std::string const& key)
{
    uint32_t h = 0x811c9dc5u;
    for (unsigned char const c : tabStem(key)) {
        h ^= c;
        h *= 0x01000193u;
    }
    return "p" + std::to_string(h % 100u);
}

class TabTokens {
public:
    struct Tab {
        void const* id = nullptr; // the tab's root canvas
        std::string key;          // its canonical key (file name) or title
        std::string persisted;    // the token its identity sidecar holds ("" none)
    };
    struct Row {
        void const* id = nullptr;
        std::string token;
        bool collided = false;    // the default (or persisted) token was taken by an older open tab
        bool fromSidecar = false; // the persisted token was used
    };

    // `tabs`: every open tab, oldest first. A tab seen before (same id and key) keeps its token; tabs not in the list
    // are forgotten.
    std::vector<Row> assign(std::vector<Tab> const& tabs)
    {
        std::map<void const*, Memo> kept;
        std::set<std::string> taken;
        for (auto const& t : tabs) {
            auto it = memo.find(t.id);
            if (it != memo.end() && it->second.key == t.key && taken.count(it->second.row.token) == 0) {
                kept[t.id] = it->second;
                taken.insert(it->second.row.token);
            }
        }
        std::vector<Row> rows;
        for (auto const& t : tabs) {
            if (auto k = kept.find(t.id); k != kept.end()) {
                rows.push_back(k->second.row);
                continue;
            }
            Row row;
            row.id = t.id;
            bool const usePersisted = isTabToken(t.persisted);
            std::string want = usePersisted ? t.persisted : defaultTabToken(t.key);
            row.fromSidecar = usePersisted;
            if (taken.count(want) != 0) {
                row.collided = true;
                want = nextFree(want, taken);
            }
            row.token = want;
            taken.insert(want);
            kept[t.id] = Memo { t.key, row };
            rows.push_back(row);
        }
        memo = std::move(kept);
        return rows;
    }

private:
    struct Memo {
        std::string key;
        Row row;
    };
    std::map<void const*, Memo> memo;

    static std::string nextFree(std::string const& from, std::set<std::string> const& taken)
    {
        unsigned long n = 0;
        try {
            n = std::stoul(from.substr(1));
        } catch (...) {
            n = 0;
        }
        // Inside p0..p99 first (the next numbers after the wanted one, wrapping), then p100 and up.
        if (n < 100)
            for (unsigned long step = 1; step < 100; ++step)
                if (auto const c = "p" + std::to_string((n + step) % 100); taken.count(c) == 0)
                    return c;
        for (unsigned long m = std::max(100ul, n + 1);; ++m)
            if (auto const c = "p" + std::to_string(m); taken.count(c) == 0)
                return c;
    }
};

} // namespace mcp
