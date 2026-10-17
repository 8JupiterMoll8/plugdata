// Unit tests for mcp::CommandTable and the verb table (Plan 3 P3.3).
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Pd/MCPBridgeRegistry.h"

#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

template<typename Fn>
static bool throws(Fn&& fn)
{
    try {
        fn();
    } catch (std::logic_error const&) {
        return true;
    }
    return false;
}

using mcp::VerbSource;
using Table = mcp::CommandTable<std::string>; // the handler is its own name

int main()
{
    // 1. The table itself.
    {
        Table t;
        t.setSource("pd", VerbSource::Path);
        t.setSource("chat", VerbSource::PathOrArg0);
        t.add("pd", "ping", "pdPing");
        t.add("pd", "doc_get", "pdDocGet");
        t.add("pd", "doc", "pdDocGet"); // alias: a second row, same handler
        t.addDomain("chat", "chatMsg");

        CHECK(*t.find("pd", "ping") == "pdPing");
        CHECK(*t.find("pd", "doc") == *t.find("pd", "doc_get"));
        CHECK(t.find("pd", "nope") == nullptr);   // unknown verb
        CHECK(t.find("zzz", "ping") == nullptr);  // unknown domain
        CHECK(*t.find("chat", "msg") == "chatMsg"); // verb-agnostic domain takes any verb
        CHECK(*t.find("chat", "") == "chatMsg");
        CHECK(t.hasDomain("pd") && !t.hasDomain("zzz"));
        CHECK(t.size() == 4);

        auto const keys = t.keys();
        CHECK((keys == std::vector<std::string> { "pd/doc", "pd/doc_get", "pd/ping" }));
        CHECK((t.domainKeys() == std::vector<std::string> { "chat" }));

        // Registering twice, or in the wrong shape, is a startup failure.
        CHECK(throws([&] { t.add("pd", "ping", "other"); }));
        CHECK(throws([&] { t.add("pd", "", "x"); }));
        CHECK(throws([&] { t.add("nodomain", "v", "x"); }));
        CHECK(throws([&] { t.addDomain("pd", "x"); }));       // verb rows already there
        CHECK(throws([&] { t.add("chat", "msg", "x"); }));    // domain handler already there
        CHECK(throws([&] { t.addDomain("chat", "x"); }));
        CHECK(throws([&] { t.setSource("pd", VerbSource::Arg0); }));
        CHECK(*t.find("pd", "ping") == "pdPing"); // a refused row changed nothing
    }

    // 2. Where the verb comes from.
    {
        CHECK(mcp::resolveVerb(VerbSource::Path, "note", "x") == "note");
        CHECK(mcp::resolveVerb(VerbSource::Path, "", "x").empty());
        CHECK(mcp::resolveVerb(VerbSource::PathOrArg0, "read", "x") == "read");
        CHECK(mcp::resolveVerb(VerbSource::PathOrArg0, "", "stats") == "stats"); // /array ["stats", ...]
        CHECK(mcp::resolveVerb(VerbSource::PathOrArg0, "", "").empty());
        CHECK(mcp::resolveVerb(VerbSource::Arg0, "ignored", "dsp") == "dsp");
        CHECK(mcp::resolveVerb(VerbSource::Arg0, "ignored", "").empty());
    }

    // 3. The committed verb table (MCPBridgeVerbs.inc), read with test macros.
    {
        struct Row { std::string domain, verb, handler; };
        std::vector<Row> verbRows, aliasRows, domainRows;
        std::map<std::string, std::string> sources;
#define MCP_SOURCE(domain, source) sources[#domain] = #source;
#define MCP_VERB(domain, verb, handler) verbRows.push_back({ #domain, verb, #handler });
#define MCP_ALIAS(domain, verb, handler) aliasRows.push_back({ #domain, verb, #handler });
#define MCP_DOMAIN(domain, handler) domainRows.push_back({ #domain, "", #handler });
#include "../../Source/Pd/MCPBridgeVerbs.inc"

        // Every wire domain is declared, with the source oscMessageReceived always used.
        std::map<std::string, std::string> const expected {
            { "pd", "Path" }, { "param", "Path" }, { "trigger", "Path" }, { "telemetry", "Arg0" },
            { "array", "PathOrArg0" }, { "bridge", "PathOrArg0" }, { "morph", "PathOrArg0" },
            { "meter", "PathOrArg0" }, { "transport", "PathOrArg0" }, { "seq", "PathOrArg0" },
            { "chat", "PathOrArg0" },
        };
        CHECK(sources == expected);

        // The rows build into a table without a duplicate or a misplaced row.
        Table t;
        bool built = true;
        try {
            for (auto const& s : sources)
                t.setSource(s.first, s.second == "Path" ? VerbSource::Path : s.second == "Arg0" ? VerbSource::Arg0 : VerbSource::PathOrArg0);
            for (auto const& r : verbRows) t.add(r.domain, r.verb, r.handler);
            for (auto const& r : aliasRows) t.add(r.domain, r.verb, r.handler);
            for (auto const& r : domainRows) t.addDomain(r.domain, r.handler);
        } catch (std::logic_error const& e) {
            std::printf("  table error: %s\n", e.what());
            built = false;
        }
        CHECK(built);

        // One handler per verb row: a second name for a handler is an MCP_ALIAS.
        std::set<std::string> handlers;
        for (auto const& r : verbRows) CHECK(handlers.insert(r.handler).second);
        for (auto const& r : domainRows) CHECK(handlers.insert(r.handler).second);
        for (auto const& r : aliasRows) {
            bool found = false;
            for (auto const& v : verbRows) found = found || (v.domain == r.domain && v.handler == r.handler);
            CHECK(found); // an alias names a verb row of its own domain
        }
        std::printf("  verb table: %zu verbs, %zu aliases, %zu domain handlers\n", verbRows.size(), aliasRows.size(), domainRows.size());
    }

    std::printf(failures ? "registry_test: %d FAILED\n" : "registry_test: OK\n", failures);
    return failures ? 1 : 0;
}
