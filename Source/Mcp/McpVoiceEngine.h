/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/
// The hidden voice engine of the ai.* widgets: one Pd patch, `<appDataDir>/Abstractions/ai-voices.pd`, opened at start
// with NO tab, so a widget that is dropped on a canvas sounds with nothing wired (a drum kit on [r ai-voice-tap], a
// 4-voice synth on [r ai-voice-note]; the patch is written in the mcp-server repo, .agents/skills/plugdata-ai-widgets).
//
// The engine is a root canvas that only Pd knows: it is NOT in PluginProcessor::patches, so no editor shows a tab for it,
// the quit check and autosave never see it, it is not in a DAW session, and "main" never resolves to it (the bridge's
// "main" is the focused editor's root, else the newest entry of `patches`). A widget or the AI reaches it by name
// (`[s ai-voice-tap]`), like any receiver.
//
// Lifecycle: McpHost::createBridge() opens it when the file exists; a 5 s timer (message thread) opens it later when the
// file appears, reopens it when something closed it (a DAW state restore closes every root; not more often than every
// 10 s), and swaps it when the file changed. A file younger than 2 s is left alone (a copy in progress is not loaded). A swap
// opens the new canvas first and frees the old one only when it came out whole (no box Pd could not create). A file that did
// not open is not tried again until it changes. McpHost::shutdown() closes it before the patches go.
// PLUGDATA_NO_VOICE_ENGINE=1 (any non-empty value but "0") keeps it closed. Everything runs on the message thread, but the
// registry is locked: a host may construct the processor elsewhere.
//
// The bridge must never address it: the identity walk (registerOpenCanvases) skips the root (isEngineRoot), so its subpatches
// take no wire keys from the artist's own [pd kick]; getCanvasBySymbol answers nothing for its name or its full path.
#pragma once

#include <juce_core/juce_core.h>

class PluginProcessor;
struct _glist; // t_canvas

namespace mcp::voice {

// The patch file's name; the one place that knows it.
inline constexpr char const* engineFileName = "ai-voices.pd";

// <appDataDir>/Abstractions/ai-voices.pd (where the artist's widgets and abstractions live).
juce::File engineFile();

// false when PLUGDATA_NO_VOICE_ENGINE is set.
bool engineEnabled();

// Registers the processor and opens the engine now when its file exists; starts the 5 s look. True when the engine is
// loaded after the call. A missing file is not an error (the look picks it up when it appears).
bool openEngine(PluginProcessor& processor);

// True when `canvas` is a root canvas made from the engine file (name and folder; an artist's own ai-voices.pd elsewhere is
// not). Reads the canvas only: no lock of its own, safe under the Pd lock.
bool isEngineRoot(_glist const* canvas);

// One look at the file now (the timer's job every 5 s): open, reopen or swap. True when the engine is loaded afterwards.
// Message thread. For tests; a file younger than 2 s is not opened.
bool lookNow(PluginProcessor& processor);

// True while the processor's engine canvas is alive in Pd.
bool engineLoaded(PluginProcessor const& processor);

// The engine's Pd root canvas, or nullptr when it is not loaded. MESSAGE THREAD only, and never while holding the Pd lock
// (it takes the registry lock, which the timer holds while it takes the Pd lock). Bridge code asks isEngineRoot instead.
_glist* engineRoot(PluginProcessor const& processor);

// Closes the engine and forgets the processor (before its patches are freed). Safe to call twice.
void closeEngine(PluginProcessor& processor);

} // namespace mcp::voice
