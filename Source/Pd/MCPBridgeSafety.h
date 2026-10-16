/*
 // Security helpers for the MCP bridge (Plan 1 Phase 3, Report F6).
 // Kept free of JUCE and Pd so they can be unit-tested with plain g++:
 // see Tests/mcp/bridge_safety_test.cpp and scripts/test-bridge-safety.sh.
 */
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace mcp::safety {

// True for hosts that can only be reached from this machine. An empty host
// means "use the default", which the bridge treats as 127.0.0.1.
inline bool isLoopbackHost(std::string_view host)
{
    if (host.empty() || host == "localhost" || host == "::1")
        return true;

    // Strict dotted-quad 127.a.b.c with every octet 0..255.
    int octets = 0;
    int value = -1;
    int first = -1;
    for (char const c : host) {
        if (c >= '0' && c <= '9') {
            value = (value < 0 ? 0 : value * 10) + (c - '0');
            if (value > 255)
                return false;
        } else if (c == '.') {
            if (value < 0)
                return false;
            if (octets == 0)
                first = value;
            ++octets;
            value = -1;
        } else {
            return false;
        }
    }
    if (value < 0)
        return false;
    if (octets == 0)
        first = value;
    return octets == 3 && first == 127;
}

// Resolve a client-supplied output file path against `root`. A relative path
// lands inside root; an absolute path must already be inside it. Returns
// nullopt for anything that escapes root (`..`, other directories, a sibling
// sharing root's name as a prefix, symlinks pointing out) or that names root
// itself.
inline std::optional<std::filesystem::path> resolveOutputPath(std::filesystem::path const& root,
                                                              std::string const& requested)
{
    namespace fs = std::filesystem;
    if (requested.empty())
        return std::nullopt;

    fs::path const rootNorm = (root / "").lexically_normal().parent_path();
    fs::path const req(requested);
    fs::path const candidate = (req.is_absolute() ? req : rootNorm / req).lexically_normal();

    auto inside = [](fs::path const& base, fs::path const& p) {
        fs::path const rel = p.lexically_relative(base);
        return !rel.empty() && rel != "." && *rel.begin() != "..";
    };
    if (!inside(rootNorm, candidate))
        return std::nullopt;

    // Follow symlinks for the parts that already exist on disk.
    std::error_code ec;
    fs::path const realRoot = fs::weakly_canonical(rootNorm, ec);
    if (ec)
        return std::nullopt;
    fs::path const realCandidate = fs::weakly_canonical(candidate, ec);
    if (ec || !inside(realRoot, realCandidate))
        return std::nullopt;

    return candidate;
}

} // namespace mcp::safety
