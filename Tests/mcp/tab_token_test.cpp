// P11.5 V24: the tab token (the receiver namespace of a tab) computed in C++. By default it is the TS stem hash it
// replaces (src/core/tab-namespace.ts tabSuffix: FNV-1a 32 over the file stem's UTF-16 units, non-alphanumerics as
// "_", mod 100), so patches saved with `_pNN` names keep their names; only a collision between OPEN tabs gives the
// later tab a new token, which the caller persists in that tab's identity sidecar. JUCE-free.
//   g++ -std=c++17 -Wall -Wextra -Werror -o /tmp/tab_token_test Tests/mcp/tab_token_test.cpp && /tmp/tab_token_test
#include "../../Source/Mcp/McpTabTokens.h"

#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

int main()
{
    // The TS reference values (node, src/core/tab-namespace.ts tabSuffix, 2026-10-25).
    struct Ref { char const* key; char const* token; };
    for (auto const& r : { Ref { "beat_b.pd", "p24" }, Ref { "/x/patches/beat_b.pd", "p24" }, Ref { "drums.pd", "p98" },
             Ref { "Untitled-1", "p84" }, Ref { "song.PD", "p72" }, Ref { "tab", "p76" }, Ref { "", "p76" },
             Ref { "a/b/", "p76" }, Ref { "kick \xC3\xA9.pd", "p89" }, Ref { "main.pd", "p92" }, Ref { "x.pd.pd", "p72" },
             Ref { "\xF0\x9F\x98\x80.pd", "p63" } }) {
        auto const got = mcp::defaultTabToken(r.key);
        if (got != r.token) std::printf("  %s -> %s (want %s)\n", r.key, got.c_str(), r.token);
        CHECK(got == r.token);
    }

    mcp::TabTokens tokens;
    int a = 0, b = 0, c = 0, d = 0;
    // No collision: every tab keeps its default token.
    {
        auto rows = tokens.assign({ { &a, "drums.pd", "" }, { &b, "beat_b.pd", "" } });
        CHECK(rows.size() == 2 && rows[0].token == "p98" && rows[1].token == "p24");
        CHECK(!rows[0].collided && !rows[1].collided);
    }
    // song0 and song39 both hash to p48: the older tab keeps it, the later one gets the next free token.
    mcp::TabTokens fresh;
    {
        auto rows = fresh.assign({ { &a, "song0.pd", "" }, { &b, "song39.pd", "" } });
        CHECK(rows.size() == 2 && rows[0].token == "p48" && !rows[0].collided);
        CHECK(rows[1].token == "p49" && rows[1].collided);
        // Stable while both stay open, also in another order.
        auto again = fresh.assign({ { &b, "song39.pd", "" }, { &a, "song0.pd", "" } });
        CHECK(again.size() == 2 && again[0].token == "p49" && again[1].token == "p48");
        // A third tab whose default is the token the collision took (song200 -> p26 is free; song189 -> p24 free).
        auto three = fresh.assign({ { &a, "song0.pd", "" }, { &b, "song39.pd", "" }, { &c, "song178.pd", "" } });
        CHECK(three.size() == 3 && three[2].token == "p50" && three[2].collided);
    }
    // The older tab closes: the later one keeps its token (its receivers carry it).
    {
        auto rows = fresh.assign({ { &b, "song39.pd", "" } });
        CHECK(rows.size() == 1 && rows[0].token == "p49");
    }
    // A token persisted in the sidecar wins over the default (the file's receivers were built with it) ...
    {
        mcp::TabTokens t;
        auto rows = t.assign({ { &a, "song39.pd", "p49" } });
        CHECK(rows.size() == 1 && rows[0].token == "p49" && rows[0].fromSidecar && !rows[0].collided);
        // ... unless another open tab already holds it: then the later tab gets a new one.
        auto both = t.assign({ { &a, "song39.pd", "p49" }, { &d, "other.pd", "p49" } });
        CHECK(both.size() == 2 && both[0].token == "p49" && both[1].token != "p49" && both[1].collided);
        // A malformed persisted token is ignored (the default is used).
        mcp::TabTokens u;
        auto bad = u.assign({ { &c, "drums.pd", "kick" } });
        CHECK(bad.size() == 1 && bad[0].token == "p98" && !bad[0].fromSidecar);
    }
    // The same address reused by another file is a new tab (its key changed).
    {
        mcp::TabTokens t;
        t.assign({ { &a, "song0.pd", "" }, { &b, "song39.pd", "" } });
        auto rows = t.assign({ { &b, "drums.pd", "" } });
        CHECK(rows.size() == 1 && rows[0].token == "p98" && !rows[0].collided);
    }
    CHECK(mcp::isTabToken("p0") && mcp::isTabToken("p99") && mcp::isTabToken("p123"));
    CHECK(!mcp::isTabToken("p") && !mcp::isTabToken("q1") && !mcp::isTabToken("p1x") && !mcp::isTabToken(""));

    std::printf(failures ? "tab_token_test: %d FAILED\n" : "tab_token_test: OK\n", failures);
    return failures ? 1 : 0;
}
