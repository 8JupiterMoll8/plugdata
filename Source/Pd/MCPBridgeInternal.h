/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Small stateless helpers shared by MCPBridge.cpp and the per-domain command
// files MCPBridgeCmd<Domain>.cpp (Plan 3 P3.3). Bodies are the ones that used to
// be file-static in MCPBridge.cpp, unchanged; `inline` replaces `static` so every
// translation unit shares one definition. Helpers with state, or tied to one
// domain (render, spectral, the control window), move with their domain's
// handlers instead of landing here.

#pragma once

#include <juce_core/juce_core.h>
#include <juce_osc/juce_osc.h>

#include "MCPJson.h" // jsonEscape for hand-built JSON (finding C6)

extern "C" {
#include <m_pd.h>
#include <g_canvas.h>
}

// The bridge reads raw object text with the caller freeing the buffer (the old
// pd::Interface::getObjectText contract). Upstream's version now returns a
// SmallString; keep the raw form here so long message boxes stay intact.
inline void mcpGetObjectText(t_object const* ob, char** text, int* size)
{
    *text = nullptr;
    *size = 0;
    binbuf_gettext(ob->te_binbuf, text, size);
}

inline void copyTrunc(char* dst, int dstSize, const char* src)
{
    if (!src) { if (dstSize > 0) dst[0] = 0; return; }
    int i = 0;
    for (; i < dstSize - 1 && src[i]; ++i) dst[i] = src[i];
    if (dstSize > 0) dst[i] = 0;
}

// JSON text for a value that goes between quotes in a hand-built reply (finding C6).
// UTF-8 passes through unchanged; see MCPJson.h and Tests/mcp/json_escape_test.cpp.
inline juce::String jsonEscape(const char* s)
{
    return juce::String::fromUTF8(mcp::json::jsonEscape(s).c_str());
}

inline juce::String jsonEscape(juce::String const& s)
{
    return jsonEscape(s.toRawUTF8());
}

// Helper to extract string from OSC argument
inline juce::String getArgString(const juce::OSCArgument& arg)
{
    if (arg.isString()) return arg.getString();
    if (arg.isFloat32()) return juce::String(arg.getFloat32());
    if (arg.isInt32()) return juce::String(arg.getInt32());
    return {};
}

// Helper to extract float from OSC argument
inline float getArgFloat(const juce::OSCArgument& arg)
{
    if (arg.isFloat32()) return arg.getFloat32();
    if (arg.isInt32()) return static_cast<float>(arg.getInt32());
    if (arg.isString()) return arg.getString().getFloatValue();
    return 0.0f;
}

// The plugdata fork does not expose glist_nth; walk the glist ourselves.
inline t_gobj* glistObjectAt(t_canvas* cnv, int index)
{
    if (!cnv || index < 0) return nullptr;
    int i = 0;
    for (t_gobj* y = cnv->gl_list; y; y = y->g_next) {
        if (i++ == index) return y;
    }
    return nullptr;
}

inline float varToFloat(const juce::var& v, float def)
{
    if (v.isVoid() || v.isUndefined()) return def;
    if (v.isBool()) return v.toString().equalsIgnoreCase("true") ? 1.0f : 0.0f;
    return (float)(double)v;
}

inline int varToInt(const juce::var& v, int def)
{
    if (v.isVoid() || v.isUndefined()) return def;
    return (int)(double)v;
}

inline bool varToBool(const juce::var& v, bool def)
{
    if (v.isVoid() || v.isUndefined()) return def;
    if (v.isBool()) return v.toString().equalsIgnoreCase("true");
    return (double)v != 0.0;
}
