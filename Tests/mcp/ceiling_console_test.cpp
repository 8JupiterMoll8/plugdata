// Unit tests for the output ceiling's console rate limit (P9 review round 2): at most one line per kind (trip,
// release) per 10 s, then a summary "Output ceiling: breaker tripped N times in the last Xs". JUCE-free.
#include "../../Source/Mcp/McpCeilingConsole.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

using mcp::CeilingConsole;

namespace {
bool has(std::string const& s, char const* part) { return s.find(part) != std::string::npos; }
// The N of a summary line ("... breaker tripped N times ..."), 1 for a single line.
uint64_t countIn(std::string const& line)
{
    auto const at = line.find(" times in the last ");
    if (at == std::string::npos) return 1;
    auto start = line.rfind(' ', at - 1);
    return std::strtoull(line.c_str() + start + 1, nullptr, 10);
}
} // namespace

int main()
{
    // One trip and one release: each said once, as the plain lines; nothing more while nothing new happens.
    {
        CeilingConsole c;
        c.start(0, 0, 0);
        auto l = c.poll(1, 0, 250);
        CHECK(l.size() == 1 && l[0] == CeilingConsole::kTripLine);
        l = c.poll(1, 1, 1700);
        CHECK(l.size() == 1 && l[0] == CeilingConsole::kReleaseLine);
        for (int64_t t = 2000; t < 30000; t += 250)
            CHECK(c.poll(1, 1, t).empty());
    }

    // The baseline: what happened before start() is not said.
    {
        CeilingConsole c;
        c.start(7, 6, 0);
        CHECK(c.poll(7, 6, 250).empty());
        CHECK(c.poll(8, 6, 500).size() == 1);
    }

    // A rhythmic burst pattern: a trip every 2 s and its release 1.5 s later, for 60 s, polled every 250 ms. At most
    // one line per kind per 10 s; after it stops, the summaries account for every trip and release.
    {
        CeilingConsole c;
        c.start(0, 0, 0);
        uint64_t trips = 0, releases = 0;
        std::vector<std::pair<int64_t, std::string>> said;
        for (int64_t t = 0; t <= 90000; t += 250) {
            if (t <= 60000 && t % 2000 == 0 && t > 0) ++trips;
            if (t <= 61500 && t % 2000 == 1500 && trips > releases) ++releases;
            for (auto const& line : c.poll(trips, releases, t))
                said.emplace_back(t, line);
        }
        uint64_t tripSaid = 0, releaseSaid = 0;
        int64_t lastTrip = -100000, lastRelease = -100000;
        int tripLines = 0, releaseLines = 0;
        for (auto const& [t, line] : said) {
            bool const isTrip = has(line, "turned down 20 dB") || has(line, "breaker tripped");
            bool const isRelease = has(line, "breaker released");
            CHECK(isTrip != isRelease);
            if (isTrip) {
                CHECK(t - lastTrip >= CeilingConsole::kMinGapMs);
                lastTrip = t;
                tripSaid += countIn(line);
                ++tripLines;
            } else {
                CHECK(t - lastRelease >= CeilingConsole::kMinGapMs);
                lastRelease = t;
                releaseSaid += countIn(line);
                ++releaseLines;
            }
            if (has(line, " times in the last ")) {
                CHECK(has(line, "Output ceiling: breaker"));
                CHECK(line.back() == 's');
            }
        }
        CHECK(tripSaid == trips);
        CHECK(releaseSaid == releases);
        CHECK(trips == 30 && releases == 30);
        CHECK(tripLines >= 6 && tripLines <= 8);       // one at the start, then one per 10 s
        CHECK(releaseLines >= 6 && releaseLines <= 8);
        bool sawSummary = false;
        for (auto const& [t, line] : said)
            if (has(line, "Output ceiling: breaker tripped 5 times in the last 10s")) sawSummary = true;
        CHECK(sawSummary);
    }

    // Several trips in one poll at the start: the first is said at once, the rest in the next summary.
    {
        CeilingConsole c;
        c.start(0, 0, 0);
        auto l = c.poll(3, 0, 250);
        CHECK(l.size() == 1 && l[0] == CeilingConsole::kTripLine);
        CHECK(c.poll(3, 0, 5000).empty());
        l = c.poll(3, 0, 10250);
        CHECK(l.size() == 1 && has(l[0], "breaker tripped 2 times in the last 10s"));
    }

    // Counters that fall (a new ceiling): a new baseline, nothing said for the drop.
    {
        CeilingConsole c;
        c.start(5, 5, 0);
        CHECK(c.poll(0, 0, 250).empty());
        CHECK(c.poll(1, 0, 500).size() == 1);
    }

    std::printf(failures ? "ceiling_console_test: %d FAILED\n" : "ceiling_console_test: OK\n", failures);
    return failures ? 1 : 0;
}
