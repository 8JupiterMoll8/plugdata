// The Copilot rack's data file (Abstractions/rack.json): more cards for the widgets tab without a rebuild. JUCE-free and pure: the
// functions take the file's text and return the cards to show plus the lines to log, so Tests/mcp/widget_rack_test.cpp runs them
// alone. Reading the file and showing the cards is Source/Sidebar/CopilotModuleRack.h.
//
//   { "version": 1,
//     "widgets": [
//       { "name": "ai.seq",   "purpose": "A note grid in key", "group": "MELODY", "object": "ai.seq bass" },
//       { "name": "groovebox", "purpose": "Clock, grids, bass, output", "group": "BLOCKS", "patch": "#X obj 0 0 gbx.clock;\n#X ..." } ] }
//
//  - name: 1..40 characters of [A-Za-z0-9._~-], the first a letter or digit (the card's title and its thumbnail's file name).
//  - purpose: one line, 1..160 characters, no control characters.
//  - group: IDEA KIT, RHYTHM, MELODY, SOUND, SYSTEM or BLOCKS (any case). BLOCKS has no built-in card; only this file fills it.
//  - object: ONE object text (what follows `#X obj 0 0 `): no `;`, no `$`, no line break, at most 200 characters. OR
//    patch: a whole Pd patch text (several objects and `#X connect` lines), pasted exactly as written on a drag: every statement
//    (ends at an unescaped `;`) must start with `#X `, `#N canvas ` or `#A `; at most 32 KB. One of the two, never both.
//  - override (optional, true): a name equal to a built-in card is skipped, unless this is true: then the card keeps its place and
//    group and takes this entry's purpose and object/patch.
// Anything else in the file is ignored. The built-in cards are always shown; the file only adds to them. Limits: 256 KB of
// text, 64 entries. A missing, broken or too big file, an unknown version or a bad entry never hides a built-in and never throws:
// the entry (or the file) is skipped and one line of `warnings` says why. Order: the groups in kRackGroups order; in a group the
// built-ins first (table order), then the file's entries (file order).
#pragma once

#include "McpRackJson.h"
#include "McpWidgetRack.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp {

inline constexpr std::size_t kRackFileMaxBytes = 256 * 1024;
inline constexpr std::size_t kRackFileMaxEntries = 64;
inline constexpr std::size_t kRackNameMax = 40;
inline constexpr std::size_t kRackPurposeMax = 160;
inline constexpr std::size_t kRackObjectMax = 200;
inline constexpr std::size_t kRackPatchMax = 32 * 1024;

// ---- one entry's rules ----------------------------------------------------------------------------------------------------

inline bool rackIsControl(char const c)
{
    return static_cast<unsigned char>(c) < 0x20 || c == 0x7f;
}

/** "" when `name` is a good card name, else why not. */
inline std::string rackNameProblem(std::string_view name)
{
    if (name.empty())
        return "the name is empty";
    if (name.size() > kRackNameMax)
        return "the name is longer than 40 characters";
    auto const alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
    if (!alnum(name.front()))
        return "the name must start with a letter or a digit";
    for (char const c : name)
        if (!alnum(c) && c != '.' && c != '_' && c != '~' && c != '-')
            return "the name may only use letters, digits and . _ ~ -";
    return {};
}

inline std::string rackPurposeProblem(std::string_view purpose)
{
    if (purpose.empty())
        return "the purpose is empty";
    if (purpose.size() > kRackPurposeMax)
        return "the purpose is longer than 160 characters";
    for (char const c : purpose)
        if (rackIsControl(c))
            return "the purpose has a control character or a line break";
    return {};
}

/** One object text for `#X obj 0 0 <object>;`: no `;` (it would end the statement), no `$`, no line break. */
inline std::string rackObjectProblem(std::string_view object)
{
    if (object.empty())
        return "the object is empty";
    if (object.size() > kRackObjectMax)
        return "the object is longer than 200 characters";
    for (char const c : object) {
        if (rackIsControl(c))
            return "the object has a control character or a line break";
        if (c == ';' || c == '$')
            return "the object has a ; or a $ (use patch for more than one object)";
    }
    return {};
}

/** A whole patch text: only Pd's own statements (nothing that would send a message to Pd), each ended by an unescaped `;`. */
inline std::string rackPatchProblem(std::string_view patch)
{
    if (patch.empty())
        return "the patch is empty";
    if (patch.size() > kRackPatchMax)
        return "the patch is longer than 32 KB";
    for (char const c : patch)
        if (rackIsControl(c) && c != '\n' && c != '\r' && c != '\t')
            return "the patch has a control character";
    auto const trimmed = [](std::string_view t) {
        auto const first = t.find_first_not_of(" \t\r\n");
        return first == std::string_view::npos ? std::string_view() : t.substr(first);
    };
    std::size_t start = 0;
    int statements = 0;
    bool escaped = false;
    for (std::size_t i = 0; i < patch.size(); ++i) {
        char const c = patch[i];
        if (escaped) {
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == ';') {
            auto const st = trimmed(patch.substr(start, i - start));
            if (st.substr(0, 3) != "#X " && st.substr(0, 10) != "#N canvas " && st.substr(0, 3) != "#A ")
                return "a statement of the patch does not start with #X, #N canvas or #A";
            start = i + 1;
            ++statements;
        }
    }
    if (!trimmed(patch.substr(start)).empty())
        return "the patch does not end with ;";
    return statements > 0 ? std::string() : std::string("the patch has no statement");
}

// ---- the merge --------------------------------------------------------------------------------------------------------------

struct RackMerge {
    std::vector<RackEntry> entries;      // always the built-ins; then what the file added
    std::vector<std::string> warnings;   // one line per skipped entry or file problem (empty: all good)
};

enum class RackFileProblem : std::uint8_t { Missing, Unreadable };

inline std::vector<RackEntry> rackBuiltIns()
{
    std::vector<RackEntry> out;
    out.reserve(kRackWidgetCount);
    for (auto const& w : kRackWidgets)
        out.push_back(rackEntryOf(w));
    return out;
}

/** The built-ins alone, for a file the caller has no text of (a bigger one is handed over cut at 256 KB + 1 and refused by the merge). */
inline RackMerge rackBuiltInsOnly(RackFileProblem why)
{
    RackMerge m { rackBuiltIns(), {} };
    switch (why) {
    case RackFileProblem::Missing: m.warnings.push_back("rack.json: not found, only the built-in widgets are shown"); break;
    case RackFileProblem::Unreadable: m.warnings.push_back("rack.json: could not be read, skipped; only the built-in widgets are shown"); break;
    }
    return m;
}

namespace rackdetail {

inline std::string stringField(rackjson::Value const& obj, char const* key, std::string& out, bool required)
{
    auto const* v = obj.find(key);
    if (!v)
        return required ? std::string("\"") + key + "\" is missing" : std::string();
    if (v->kind != rackjson::Value::Kind::String)
        return std::string("\"") + key + "\" must be text";
    out = v->text;
    return {};
}

/** Fills `out` from one array element; "" when it is a good entry, else why not. `overrides` is its "override" flag. */
inline std::string readEntry(rackjson::Value const& v, RackEntry& out, bool& overrides)
{
    if (v.kind != rackjson::Value::Kind::Object)
        return "not an object";
    std::string groupText;
    if (auto p = stringField(v, "name", out.name, true); !p.empty())
        return p;
    if (auto p = rackNameProblem(out.name); !p.empty())
        return p;
    if (auto p = stringField(v, "purpose", out.purpose, true); !p.empty())
        return p;
    if (auto p = rackPurposeProblem(out.purpose); !p.empty())
        return p;
    if (auto p = stringField(v, "group", groupText, true); !p.empty())
        return p;
    auto const group = rackGroupFromTitle(groupText);
    if (!group)
        return "\"group\" is not one of IDEA KIT, RHYTHM, MELODY, SOUND, SYSTEM, BLOCKS";
    out.group = *group;
    if (auto p = stringField(v, "object", out.object, false); !p.empty())
        return p;
    if (auto p = stringField(v, "patch", out.patch, false); !p.empty())
        return p;
    bool const hasObject = v.find("object") != nullptr;
    bool const hasPatch = v.find("patch") != nullptr;
    if (hasObject == hasPatch)
        return hasObject ? "it has both \"object\" and \"patch\"" : "it needs \"object\" or \"patch\"";
    if (auto p = hasObject ? rackObjectProblem(out.object) : rackPatchProblem(out.patch); !p.empty())
        return p;
    overrides = false;
    if (auto const* o = v.find("override")) {
        if (o->kind != rackjson::Value::Kind::Bool)
            return "\"override\" must be true or false";
        overrides = o->boolean;
    }
    out.fromFile = true;
    return {};
}

inline std::size_t groupIndex(RackGroup g)
{
    for (std::size_t i = 0; i < sizeof(kRackGroups) / sizeof(kRackGroups[0]); ++i)
        if (kRackGroups[i] == g)
            return i;
    return 0;
}

inline std::string entryLabel(std::size_t index, rackjson::Value const& v)
{
    std::string label = "rack.json: entry " + std::to_string(index + 1);
    auto const* name = v.kind == rackjson::Value::Kind::Object ? v.find("name") : nullptr;
    if (name && name->kind == rackjson::Value::Kind::String && !name->text.empty() && name->text.size() <= kRackNameMax) {
        label += " (";
        for (char const c : name->text) // only printable ASCII goes into the log line
            label.push_back(c >= 0x20 && c < 0x7f ? c : '?');
        label += ")";
    }
    return label;
}

/** Adds one good entry to `entries`: a new card, or (override) a built-in's new text; "" or the reason it is skipped. */
inline std::string addEntry(std::vector<RackEntry>& entries, RackEntry&& e, bool overrides)
{
    for (auto& have : entries) {
        if (have.name != e.name)
            continue;
        if (have.fromFile)
            return "the name is used twice";
        if (!overrides)
            return "the name is a built-in widget (add \"override\": true to replace it)";
        have.purpose = std::move(e.purpose);
        have.object = std::move(e.object);
        have.patch = std::move(e.patch);
        have.fromFile = true;
        return {};
    }
    entries.push_back(std::move(e));
    return {};
}

} // namespace rackdetail

/**
 * The cards to show for rack.json's text: the built-ins, then the file's good entries, grouped and ordered as the header says.
 * `warnings` names every skipped entry (or the whole file) in one line each; it never throws on bad input.
 */
inline RackMerge mergeRackFile(std::string_view text)
{
    RackMerge m { rackBuiltIns(), {} };
    auto const whole = [&m](std::string const& why) {
        m.warnings.push_back("rack.json: " + why + "; only the built-in widgets are shown");
        return m;
    };
    if (text.size() > kRackFileMaxBytes)
        return whole("bigger than 256 KB, skipped");
    if (!rackjson::isValidUtf8(text))
        return whole("not valid UTF-8, skipped");
    rackjson::Value root;
    std::string error;
    if (!rackjson::Parser(text).parse(root, error))
        return whole("not valid JSON (" + error + "), skipped");
    if (root.kind != rackjson::Value::Kind::Object)
        return whole("the top level must be an object { \"widgets\": [...] }, skipped");
    if (auto const* version = root.find("version"); version && (version->kind != rackjson::Value::Kind::Number || version->text != "1"))
        return whole("\"version\" must be 1, skipped");
    auto const* widgets = root.find("widgets");
    if (!widgets || widgets->kind != rackjson::Value::Kind::Array)
        return whole("\"widgets\" must be a list, skipped");

    std::size_t const considered = std::min(widgets->items.size(), kRackFileMaxEntries);
    for (std::size_t i = 0; i < considered; ++i) {
        auto const& item = widgets->items[i];
        RackEntry entry;
        bool overrides = false;
        auto problem = rackdetail::readEntry(item, entry, overrides);
        if (problem.empty())
            problem = rackdetail::addEntry(m.entries, std::move(entry), overrides);
        if (!problem.empty())
            m.warnings.push_back(rackdetail::entryLabel(i, item) + ": " + problem + ", skipped");
    }
    if (widgets->items.size() > kRackFileMaxEntries)
        m.warnings.push_back("rack.json: more than 64 entries, the rest are skipped");
    std::stable_sort(m.entries.begin(), m.entries.end(),
        [](RackEntry const& a, RackEntry const& b) { return rackdetail::groupIndex(a.group) < rackdetail::groupIndex(b.group); });
    return m;
}

} // namespace mcp
