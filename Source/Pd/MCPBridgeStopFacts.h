/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Truth slice 3 (F3): the trace's `stopFacts` block (capability trace_stop_facts). Builds the stop walk's graph
// (MCPBridgeStopWalk.h) from one pass over the canvas's wires and the armed window's per-tap verdicts, and words each
// row as facts read under the same Pd lock: the inlets (MCPBridgeInletValues.h), the names the object listens on
// (MCPBridgeBound.h), a red box (MCPBridgeBroken.h), [switch~] off, who made it, and for a GUI that feeds a row whether
// a hand moved it (McpHost::aiGuiValue). No advice, no ranking beyond the walk's fixed order.
//
// Caller holds the Pd lock (message thread or the command worker). Never on the audio thread: it reads the window's
// readings, which the probes collected; it reads no signal buffer itself.
#pragma once

#include "MCPBridgeStopWalk.h"
#include "MCPBridgeSoundVerdict.h"

#include <juce_core/juce_core.h>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include <m_pd.h>
#include <g_canvas.h>
}

class PluginProcessor;

namespace mcp::stops {

/** The canvas's objects as the trace names them (tempId, else "class#index"), in gl_list order. */
struct CanvasObjects {
    std::vector<t_gobj*> const& objs;
    std::vector<juce::String> const& names;
    std::vector<juce::String> const& classNames;
};

/** A tap's level from its sound verdict (slice 2's judge): audible / silent / dc_only / unmeasured. */
inline Level levelOf(mcp::verdict::Verdict const& v)
{
    switch (v.kind) {
    case mcp::verdict::Kind::Audible: return Level::Audible;
    case mcp::verdict::Kind::Silent: return Level::Silent;
    case mcp::verdict::Kind::DcOnly: return Level::DcOnly;
    case mcp::verdict::Kind::Unmeasured: return Level::Unmeasured;
    }
    return Level::Unmeasured;
}

/** What the walk found, for the trace's chokePoint / reason / levels (no window: the last block's raw peaks). */
struct WalkSummary {
    Result result;
    Graph graph;
};

/**
 * The walk over `cnv`. `verdicts` maps "<name>:<outlet>" to the window's verdict of that outlet; an outlet missing
 * from it is unread. Caller holds the Pd lock.
 */
WalkSummary walkCanvas(t_canvas* cnv, CanvasObjects const& objects,
    std::unordered_map<std::string, mcp::verdict::Verdict> const& verdicts);

/** The `stopFacts` JSON object of a walk (rows worded from live reads). Caller holds the Pd lock. */
juce::String stopFactsJson(PluginProcessor* processor, t_canvas* cnv, CanvasObjects const& objects, WalkSummary const& walk);

} // namespace mcp::stops
