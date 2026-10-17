/*
 // The MCP host (Plan 3 P3.2 S4): owns the MCPBridge and the McpSession, and the
 // bridge-only functions that used to be PluginProcessor members (identity lookup,
 // canvas lookup, transactions, recorder and monitor taps). PluginProcessor holds one
 // std::unique_ptr<McpHost> and no MCP state.
 */
#pragma once

#include "Mcp/McpSession.h"
#include "Mcp/McpGuiHost.h"
#include <juce_dsp/juce_dsp.h>

#include "Pd/Instance.h"

class PluginProcessor;
class MCPBridge;

class McpHost {
public:
    explicit McpHost(PluginProcessor& processor);
    ~McpHost();

    // Reads the mcp_bridge_* settings and creates the bridge (was inline in the
    // PluginProcessor constructor, at the same point of startup).
    void createBridge();

    MCPBridge* getMCPBridge() const { return mcpBridge.get(); }
    juce::String getMcpBridgeStatus() const;
    // Reverse identity lookup (C++ truth): the stable tempId for a live pd object,
    // read straight from the bridge's own stable map — never from the TS mirror.
    juce::String getStableId(t_gobj* g) const;
    juce::String getOrAdoptStableId(t_canvas* canvas, t_gobj* g);
    void sendMCPReply(const String& replyAddr, const SmallArray<pd::Atom>& atoms);

    bool hasMcpTransaction(t_canvas* cnv) const;
    bool hasMcpRedoTransaction(t_canvas* cnv) const;
    void pushMcpTransaction(t_canvas* cnv, const juce::String& canvasName, const juce::OSCMessage& forward, const juce::OSCMessage& inverse, const juce::String& replayAction = "batch_atomic");
    void undoMcpTransaction(t_canvas* cnv);
    void redoMcpTransaction(t_canvas* cnv);
    void clearMcpTransactions(t_canvas* cnv);
    bool settingsChanged(String const& name, var const& value);
    void mcpSettingsChanged(String const& name, var const& value);
    t_canvas* getCanvasBySymbol(const String& canvas_symbol);
    // Strict variant for mutations: resolves ONLY 'main'/'pd-main'/'' (focused
    // root) and genuinely registered canvas symbols. Returns nullptr for
    // unknown names instead of silently falling back to the first canvas
    // (fault gauntlet 2026-09-09 bug #11 — wrong-canvas injection).
    t_canvas* getCanvasBySymbolStrict(const String& canvas_symbol);
    void synchroniseCanvases();

    t_gobj* resolveStableId(const String& canvasName, const String& objectId);
    // Full identity reconcile against the live canvas (gl_list): evict entries whose
    // gobj is gone, adopt untracked objects (gui_<class>_<n>), bump mcpIdentityVersion.
    // Caller must be on the message thread / hold the Pd lock (same context as
    // resolveStableId). Makes the tempId cache equal gl_list truth before any op that
    // resolves ids — so a stale id can never silently map to the wrong object.
    void reconcileIdentity(const String& canvasName);
    // Open/close the WAV take. Never call these on the audio thread.
    bool mcpOpenRecorder(juce::String const& path);
    void mcpCloseRecorder();
    // Writes the current output block into the active WAV writer, if recording.
    // Must be called from the audio thread on the final output buffer.
    void writeRecorderTap(dsp::AudioBlock<float> const& buffer);
    // Applies the solo/mute monitor tap to ONE Pd sub-block of the Pd output
    // (audio thread; runs per sub-block so it works for any JUCE block size).
    // No-op when no tap is active. Non-destructive: the patch is untouched.
    void applyMonitorTap(float* out, int numSamples, int numChannels);

    // Audio hooks (Source/Mcp/McpAudio.cpp); each runs where its block used to be.
    void prepare(double sampleRate, int samplesPerBlock);
    void audioBlockStarted();
    void midiTick(MidiBuffer& midiInputHistory, AudioBuffer<float>& buffer, double sampleRate);
    void audioBlockFinished(AudioBuffer<float>& buffer);
    void pdBlock(float* audioVectorIn, float* audioVectorOut, int pdBlockSize, int numChannels);

    // Telemetry hooks (Source/Mcp/McpTelemetry.cpp).
    void selectionChanged(SmallArray<juce::Component*>& objects);
    void sendSelectionTelemetry(String const& selector, SmallArray<pd::Atom> const& list);
    void consoleMessage(String const& message, bool isError);

    // The mcp_* Pd system messages (Source/Mcp/McpSysCommands.cpp). Returns false for
    // every other selector, which PluginProcessor::receiveSysMessage then handles.
    bool handleSysMessage(SmallString const& selector, SmallArray<pd::Atom> const& list);
    void dispatchSysMessage(SmallString const& selector, SmallArray<pd::Atom> const& list);

    // The bridge's only way to the GUI (Source/Mcp/GuiHost.cpp, S6).
    McpGuiHost& gui() { return *guiHost; }

    PluginProcessor& processor;
    McpSession session;
    // Declared before the bridge, so it outlives it.
    std::unique_ptr<McpGuiHost> guiHost;
    // Declared after the session, so it is destroyed first: its threads stop
    // before the state they touch goes away.
    std::unique_ptr<MCPBridge> mcpBridge;

    JUCE_DECLARE_NON_COPYABLE(McpHost)
};
