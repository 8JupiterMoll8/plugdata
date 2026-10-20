// Unit tests for the MCP bridge security helpers (Plan 1 Phase 3, Report F6).
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Pd/MCPBridgeSafety.h"

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

namespace fs = std::filesystem;
using mcp::safety::isLoopbackHost;
using mcp::safety::resolveOutputPath;

int main()
{
    // ── loopback ──
    CHECK(isLoopbackHost("127.0.0.1"));
    CHECK(isLoopbackHost("127.1.2.3"));
    CHECK(isLoopbackHost("localhost"));
    CHECK(isLoopbackHost("::1"));
    CHECK(isLoopbackHost(""));          // empty means "default", which is loopback
    CHECK(!isLoopbackHost("192.168.1.20"));
    CHECK(!isLoopbackHost("10.0.0.1"));
    CHECK(!isLoopbackHost("0.0.0.0"));
    CHECK(!isLoopbackHost("127.0.0.1.evil.com"));
    CHECK(!isLoopbackHost("localhost.evil.com"));
    CHECK(!isLoopbackHost("1270.0.0.1"));

    // ── output paths ──
    fs::path const root = "/home/u/Documents/plugdata/mcp-renders";
    auto inRoot = [&](char const* req, char const* expect) {
        auto r = resolveOutputPath(root, req);
        return r.has_value() && *r == fs::path(expect);
    };
    CHECK(inRoot("take.wav", "/home/u/Documents/plugdata/mcp-renders/take.wav"));
    CHECK(inRoot("sub/take.wav", "/home/u/Documents/plugdata/mcp-renders/sub/take.wav"));
    CHECK(inRoot("/home/u/Documents/plugdata/mcp-renders/a.wav", "/home/u/Documents/plugdata/mcp-renders/a.wav"));
    CHECK(inRoot("./x/../b.wav", "/home/u/Documents/plugdata/mcp-renders/b.wav"));
    CHECK(!resolveOutputPath(root, "../escape.wav"));
    CHECK(!resolveOutputPath(root, "sub/../../escape.wav"));
    CHECK(!resolveOutputPath(root, "/etc/cron.d/evil"));
    CHECK(!resolveOutputPath(root, "/home/u/Documents/plugdata/mcp-renders-evil/a.wav")); // prefix trick
    CHECK(!resolveOutputPath(root, "/home/u/Documents/plugdata/mcp-renders"));             // the root itself is not a file
    CHECK(!resolveOutputPath(root, ""));
    CHECK(inRoot("a.wav", "/home/u/Documents/plugdata/mcp-renders/a.wav"));

    // ── symlink escape (real files on disk) ──
    {
        fs::path const tmp = fs::temp_directory_path() / "mcp_safety_test";
        fs::remove_all(tmp);
        fs::create_directories(tmp / "renders");
        fs::create_directories(tmp / "outside");
        fs::create_directory_symlink(tmp / "outside", tmp / "renders" / "link");
        CHECK(!resolveOutputPath(tmp / "renders", "link/evil.wav"));
        CHECK(resolveOutputPath(tmp / "renders", "ok.wav").has_value());
        fs::remove_all(tmp);
    }

    if (failures == 0) std::printf("bridge_safety_test: all passed\n");
    else std::printf("bridge_safety_test: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
