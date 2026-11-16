// A picture pasted onto a canvas (overnight queue 2026-11-16, Batch 2 item 6): it is saved as a PNG in the patch's `refs/`
// folder, next to the .pd, and shown by a [pic] box. This header is the JUCE-free part: the file name (never an existing one),
// the size the box gets, the pasted object text. The clipboard read, the write and the paste are CanvasMcpTools.cpp.
// Tested by Tests/mcp/refs_name_test.cpp.
//
//  - the name is `<patch>-<n>.png`: the patch's file name without its extension, lower case, only [a-z0-9_-] (anything else
//    becomes `-`, runs and edges are trimmed, at most 40 characters, `pasted` when nothing is left), n = the lowest number from 1
//    that `exists` does not claim: a file that is already there is never touched;
//  - the box is `[pic -open refs/<file>]`: ELSE's pic opens the file when it is created (and every time the patch loads),
//    relative to the patch's folder. A [loadbang] chain would not do: loadbang fires when a patch loads, not when objects are
//    pasted, so the picture would stay empty until the next load;
//  - a picture is stored at most `refsMaxEdge` pixels on its longest side (never enlarged): a pasted 4K screenshot would
//    otherwise be a 3840 px box on a patch.
#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace mcp {

inline constexpr int refsMaxEdge = 960;
inline constexpr std::size_t refsStemMax = 40;

/** The patch's name as a file-name stem: lower case, [a-z0-9_-], other runs become one `-`, trimmed, `pasted` when empty. */
inline std::string refsStem(std::string_view patchName)
{
    std::string out;
    bool pendingDash = false;
    for (char const raw : patchName) {
        char const c = (raw >= 'A' && raw <= 'Z') ? static_cast<char>(raw - 'A' + 'a') : raw;
        bool const plain = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!plain) {
            pendingDash = !out.empty();
            continue;
        }
        if (pendingDash) {
            out.push_back('-');
            pendingDash = false;
        }
        out.push_back(c);
    }
    if (out.size() > refsStemMax)
        out.resize(refsStemMax);
    while (!out.empty() && (out.back() == '-' || out.back() == '_'))
        out.pop_back();
    while (!out.empty() && (out.front() == '-' || out.front() == '_'))
        out.erase(out.begin());
    return out.empty() ? std::string("pasted") : out;
}

/** `<stem>-<n>.png` with the lowest n >= 1 that `exists` does not claim. */
inline std::string refsFileName(std::string_view patchName, std::function<bool(std::string const&)> const& exists)
{
    auto const stem = refsStem(patchName);
    for (int n = 1; n < 100000; ++n) {
        auto const name = stem + "-" + std::to_string(n) + ".png";
        if (!exists || !exists(name))
            return name;
    }
    return {}; // 100000 pictures of one patch: the caller refuses the paste
}

/** The size to store: the longest side at most `maxEdge`, the ratio kept, never below 1 px, never enlarged. */
inline std::pair<int, int> refsFitSize(int width, int height, int maxEdge = refsMaxEdge)
{
    if (width <= 0 || height <= 0)
        return { std::max(width, 0), std::max(height, 0) };
    int const longest = std::max(width, height);
    if (longest <= maxEdge)
        return { width, height };
    double const scale = static_cast<double>(maxEdge) / static_cast<double>(longest);
    return { std::max(1, static_cast<int>(width * scale + 0.5)), std::max(1, static_cast<int>(height * scale + 0.5)) };
}

/** The object text of the box: `pic -open refs/<file>` (relative to the patch's folder). */
inline std::string refsPicObject(std::string_view fileName)
{
    return "pic -open refs/" + std::string(fileName);
}

/** The patch text a paste takes: one `#X obj` line at the origin. */
inline std::string refsPatchText(std::string_view fileName)
{
    return "#X obj 0 0 " + refsPicObject(fileName) + ";\n";
}

} // namespace mcp
