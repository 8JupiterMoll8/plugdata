// The Copilot sidebar's module rack (overnight queue 2026-11-16, Batch 2 item 5): one card per ai.* widget, in groups, drag to
// patch. This header is the JUCE-free part: the table (name, one-line purpose, group, what the drag creates) and the small pure
// helpers (thumbnail file name, a free looper name), so Tests/mcp/widget_rack_test.cpp compiles it alone. The cards and the list
// are Source/Sidebar/CopilotModuleRack.h.
//
// The purposes are the gallery's comments (Alphi Ideas/gallery/ai-widgets.gallery.pd), shortened to one line. The widget names are
// the artist-facing `ai.<word>` names (ai.note is ELSE's [note] sticky note under a name of its own). Names with a `~` map to a
// plain file name for the optional thumbnail (`ai.loop~` -> `ai.loop_.png`), so a thumbs folder works on every file system.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace mcp {

enum class RackGroup : std::uint8_t { IdeaKit, Rhythm, Melody, Sound, System };

inline constexpr RackGroup kRackGroups[] = { RackGroup::IdeaKit, RackGroup::Rhythm, RackGroup::Melody, RackGroup::Sound, RackGroup::System };

constexpr std::string_view rackGroupTitle(RackGroup g)
{
    switch (g) {
    case RackGroup::IdeaKit: return "IDEA KIT";
    case RackGroup::Rhythm: return "RHYTHM";
    case RackGroup::Melody: return "MELODY";
    case RackGroup::Sound: return "SOUND";
    case RackGroup::System: return "SYSTEM";
    }
    return "";
}

/** 0xAARRGGBB of the group's dot. */
constexpr std::uint32_t rackGroupColour(RackGroup g)
{
    switch (g) {
    case RackGroup::IdeaKit: return 0xffffc107; // amber
    case RackGroup::Rhythm: return 0xffff7043;  // coral
    case RackGroup::Melody: return 0xffb388ff;  // violet (the MIDI looper's colour from before the rack)
    case RackGroup::Sound: return 0xff4dd0e1;   // cyan
    case RackGroup::System: return 0xff90a4ae;  // blue grey
    }
    return 0xff90a4ae;
}

struct RackWidget {
    std::string_view name;    // what the card shows: ai.<word>
    std::string_view purpose; // one line
    RackGroup group;
    std::string_view object;  // the object text a drag creates (after "#X obj 0 0 "); "{name}" = a free looper name
};

/** In the order the rack shows them: grouped, groups in kRackGroups order. */
inline constexpr RackWidget kRackWidgets[] = {
    { "ai.sketch", "Pencil pad for ideas; the AI can suggest a stroke", RackGroup::IdeaKit, "ai.sketch" },
    { "ai.note", "A sticky note; double-click to write, the AI reads it", RackGroup::IdeaKit, "note 280 16 Inter empty 0 18 18 18 0 247 194 20 1 0 sticky note" },
    { "ai.paint", "Paint the music with colours and brushes", RackGroup::IdeaKit, "ai.paint" },
    { "ai.grid", "Compose a Bauhaus poster; shapes are sounds", RackGroup::IdeaKit, "ai.grid" },
    { "ai.tap", "Tap a beat on three pads; keeps your feel, the AI adds ghost hits", RackGroup::Rhythm, "ai.tap" },
    { "ai.loop~", "A looper you hit; free or sync, the AI suggests layers", RackGroup::Rhythm, "ai.loop~ {name}" },
    { "ai.draw", "Draw a melody line; the AI answers with a line", RackGroup::Melody, "ai.draw" },
    { "ai.midi", "Play notes; the AI hears your take", RackGroup::Melody, "ai.midi 240 64" },
    { "ai.voice", "Sing or speak into the mic capsule", RackGroup::Melody, "ai.voice 240 72" },
    { "ai.listen", "Call and response; play a phrase and it answers", RackGroup::Melody, "ai.listen" },
    { "ai.knob", "One big knob; you name its two ends (calm / wild)", RackGroup::Sound, "ai.knob" },
    { "ai.dice", "Roll a die; four variations, keep one", RackGroup::Sound, "ai.dice" },
    { "ai.strip", "Arrange sections by moving cards", RackGroup::System, "ai.strip" },
    { "ai.jobs", "Shows what the AI's background jobs are playing", RackGroup::System, "ai.jobs" },
};

inline constexpr std::size_t kRackWidgetCount = sizeof(kRackWidgets) / sizeof(kRackWidgets[0]);

/** The patch text a drag pastes: `#X obj 0 0 <object>;\n`, with `{name}` replaced by `looperName` (ai.loop~ needs one). */
inline std::string rackPatchText(RackWidget const& w, std::string_view looperName)
{
    std::string object(w.object);
    constexpr std::string_view token = "{name}";
    if (auto const at = object.find(token); at != std::string::npos)
        object.replace(at, token.size(), looperName.empty() ? std::string_view("loop1") : looperName);
    return "#X obj 0 0 " + object + ";\n";
}

/** `<name>.png` with every character outside [A-Za-z0-9._-] replaced by `_` ("" for an empty name). */
inline std::string rackThumbFileName(std::string_view name)
{
    if (name.empty())
        return {};
    std::string out;
    out.reserve(name.size() + 4);
    for (char const c : name) {
        bool const plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        out.push_back(plain ? c : '_');
    }
    return out + ".png";
}

/**
 * The lowest `loop<n>` (n from 1) that `taken` does not claim. A looper's name is its shared bus (`ai.loop_<name>`), so two
 * loopers with one name would play into each other: the rack picks a name no open tab uses.
 */
inline std::string rackFreeLooperName(std::function<bool(std::string const&)> const& taken)
{
    for (int n = 1; n < 10000; ++n) {
        auto const candidate = "loop" + std::to_string(n);
        if (!taken || !taken(candidate))
            return candidate;
    }
    return "loop" + std::to_string(10000);
}

} // namespace mcp
