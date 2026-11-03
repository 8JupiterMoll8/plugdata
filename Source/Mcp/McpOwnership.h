/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/
#pragma once

// P12a A2: what counts as the AI's own object (C++ is the truth; the TS gate reads it fresh, /pd/ownership, at check
// time and never keeps it in its mirror). JUCE-free, unit-tested in Tests/mcp/flow_provenance_test.cpp.
//
// Owned = the object's lifetime was made by the AI in this run (identity origin Ai) AND it is not a container
// (subpatch, abstraction, clone) AND not a data holder (anything that keeps recorded or typed content an undo cannot
// bring back) AND no wire into it comes from (or out of it goes to) an object that is not the AI's AND its text and GUI
// properties are still as the AI left them (review M1) AND its tab name is not ambiguous.
// Every "not" answers why. Containers and data holders always ask, whoever made them (review round 1, H2).

#include <string>

namespace mcp::ownership {

enum class Why {
    AiOwned,       // the AI's own: flow mode may change it without a question
    NotFound,      // no live object under this id on this canvas
    OriginUnknown, // made by the artist, pasted, loaded, retyped by hand, restored by undo: the artist's
    EditedInPlace, // made by the AI, then its text or GUI properties changed outside the AI's own edits (review M1)
    AmbiguousTab,  // another open root tab has the same name (until the TS keys tabs uniquely, P11.6 F10)
    Container,     // subpatch, abstraction, clone
    DataHolder,    // table, array, text, coll, buffer...
    ArtistInWire,  // a wire into it comes from an object that is not the AI's own
    ArtistOutWire, // a wire out of it goes into an object that is not the AI's own (review M1)
};

inline char const* whyName(Why w)
{
    switch (w) {
    case Why::AiOwned: return "ai_own";
    case Why::NotFound: return "not_found";
    case Why::OriginUnknown: return "origin_unknown";
    case Why::EditedInPlace: return "edited_in_place";
    case Why::AmbiguousTab: return "ambiguous_tab";
    case Why::Container: return "container";
    case Why::DataHolder: return "data_holder";
    case Why::ArtistInWire: return "artist_in_wire";
    case Why::ArtistOutWire: return "artist_out_wire";
    }
    return "unknown";
}

struct Facts {
    bool found = false;
    bool originAi = false;
    bool ambiguousTab = false;
    bool container = false;
    bool dataHolder = false;
    bool nonAiInWire = false;
    bool nonAiOutWire = false;
    bool editedInPlace = false;
};

// The first reason that makes it not the AI's own, or AiOwned.
inline Why decide(Facts const& f)
{
    if (!f.found) return Why::NotFound;
    if (!f.originAi) return Why::OriginUnknown;
    if (f.editedInPlace) return Why::EditedInPlace;
    if (f.ambiguousTab) return Why::AmbiguousTab;
    if (f.container) return Why::Container;
    if (f.dataHolder) return Why::DataHolder;
    if (f.nonAiInWire) return Why::ArtistInWire;
    if (f.nonAiOutWire) return Why::ArtistOutWire;
    return Why::AiOwned;
}

namespace detail {
inline std::string stripLibrary(std::string w)
{
    for (char const* p : { "cyclone/", "else/" })
        if (w.rfind(p, 0) == 0) return w.substr(std::string(p).size());
    return w;
}
inline std::string word(std::string const& text, int n)
{
    size_t i = 0;
    for (int k = 0;; ++k) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == ';')) ++i;
        size_t const start = i;
        while (i < text.size() && text[i] != ' ' && text[i] != '\t' && text[i] != '\n' && text[i] != ';') ++i;
        if (k == n) return text.substr(start, i - start);
        if (i >= text.size()) return {};
    }
}
inline bool holderWord(std::string const& w)
{
    for (char const* c : { "table", "garray", "qlist", "textfile", "coll", "buffer~", "buffer", "funbuff", "mtr",
             "seq", "capture", "capture~", "bag", "sfload" })
        if (w == c) return true;
    return false;
}
inline bool holderPair(std::string const& a, std::string const& b)
{
    bool const define = b == "define" || b == "d";
    if ((a == "array" || a == "text") && define) return true;
    return a == "list" && b == "store";
}
} // namespace detail

// A data holder, from Pd's class name and the object's text. A superset of the TS isTableClass (core/mutation-budget.ts:
// [table], [array define]), pinned by the unit test.
inline bool isDataHolder(std::string const& className, std::string const& text)
{
    using namespace detail;
    auto const cls = stripLibrary(className);
    if (holderWord(cls)) return true;
    if (cls == "array define" || cls == "text define" || cls == "list store") return true;
    auto const w0 = stripLibrary(word(text, 0));
    auto const w1 = word(text, 1);
    if (holderWord(w0)) return true;
    return holderPair(w0, w1);
}

} // namespace mcp::ownership
