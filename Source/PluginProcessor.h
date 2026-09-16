/*
 // Copyright (c) 2021-2025 Timothy Schoen
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

#pragma once
#include <unordered_map>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_dsp/juce_dsp.h>

#if PERFETTO
#    include <melatonin_perfetto/melatonin_perfetto.h>
#endif

#include "Utility/Config.h"
#include "Utility/Limiter.h"
#include "Utility/SettingsFile.h"
#include "Utility/AudioMidiFifo.h"
#include "Utility/SeqLock.h"
#include "Utility/MidiDeviceManager.h"

#include "Pd/Instance.h"
#include "Pd/Patch.h"
#include <juce_osc/juce_osc.h>

struct McpTransaction {
    juce::String canvasName;
    juce::OSCMessage forward;
    juce::OSCMessage inverse;
    // Which /pd action the forward/inverse messages are replayed through.
    // Defaults to batch_atomic (create/connect/edit/delete); moves use move_batch_id.
    juce::String replayAction = "batch_atomic";
};

class MCPBridge;

namespace pd {
class Library;
}

class PlugDataParameter;
class Autosave;
class InternalSynth;
class SettingsFile;
class StatusbarSource;
struct PlugDataLook;
class PluginEditor;
class ConnectionMessageDisplay;
class Object;
class PluginProcessor final : public AudioProcessor
    , public pd::Instance
    , public SettingsFileListener {
public:
    PluginProcessor();

    ~PluginProcessor() override;

    MCPBridge* getMCPBridge() const { return mcpBridge.get(); }
    juce::String getMcpBridgeStatus() const;
    void sendMCPReply(const String& replyAddr, const SmallArray<pd::Atom>& atoms);

    bool hasMcpTransaction(t_canvas* cnv) const;
    bool hasMcpRedoTransaction(t_canvas* cnv) const;
    void pushMcpTransaction(t_canvas* cnv, const juce::String& canvasName, const juce::OSCMessage& forward, const juce::OSCMessage& inverse, const juce::String& replayAction = "batch_atomic");
    void undoMcpTransaction(t_canvas* cnv);
    void redoMcpTransaction(t_canvas* cnv);
    void clearMcpTransactions(t_canvas* cnv);
    // Atomic: written on the message thread (undo/redo), read on the OSC thread
    // (batch_atomic dedup / push guards). Plain bool was a cross-thread race.
    std::atomic<bool> isExecutingMcpUndoRedo { false };

    static AudioProcessor::BusesProperties buildBusesProperties();

    void setOversampling(int amount);
    void setLimiterThreshold(int amount);
    void setEnableLimiter(bool enabled);
    bool getEnableLimiter();

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void numChannelsChanged() override;
    void releaseResources() override { }

    void updateAllEditorsLNF();

    void flushMessageQueue();
    void doubleFlushMessageQueue();

#ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported(BusesLayout const& layouts) const override;
#endif

    void processBlock(AudioBuffer<float>&, MidiBuffer&) override;

    void processBlockBypassed(AudioBuffer<float>& buffer, MidiBuffer&) override;

    AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    String const getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    String const getProgramName(int index) override;
    void changeProgramName(int index, String const& newName) override;

    void getStateInformation(MemoryBlock& destData) override;
    void setStateInformation(void const* data, int sizeInBytes) override;

    pd::Patch::Ptr findPatchInPluginMode(int editorIndex);

    void receiveNoteOn(int channel, int pitch, int velocity) override;
    void receiveControlChange(int channel, int controller, int value) override;
    void receiveProgramChange(int channel, int value) override;
    void receivePitchBend(int channel, int value) override;
    void receiveAftertouch(int channel, int value) override;
    void receivePolyAftertouch(int channel, int pitch, int value) override;
    void receiveMidiByte(int port, int byte) override;
    void receiveSysMessage(SmallString const& selector, SmallArray<pd::Atom> const& list) override;

    void addTextToTextEditor(uint64_t ptr, SmallString const& text) override;
    void hideTextEditorDialog(uint64_t ptr) override;
    void showTextEditorDialog(uint64_t ptr, SmallString const& title, std::function<void(String, uint64_t)> save, std::function<void(uint64_t)> close) override;
    void raiseTextEditorDialog(uint64_t ptr) override;
    void clearTextEditor(uint64_t ptr) override;
    bool isTextEditorDialogShown(uint64_t ptr) override;

    void updateConsole(int numMessages, bool newWarning) override;
    void onSelectionChanged(String const& selector, SmallArray<pd::Atom> const& list) override;
    void onConsoleMessage(String const& message, bool isError) override;

    void reloadAbstractions(File changedPatch, t_glist* except) override;

    void processConstant(dsp::AudioBlock<float>);
    void processVariable(dsp::AudioBlock<float>, MidiBuffer& midiBuffer);

    MidiDeviceManager& getMidiDeviceManager();

    bool canAddBus(bool isInput) const override
    {
        return true;
    }

    bool canRemoveBus(bool const isInput) const override
    {
        int const nbus = getBusCount(isInput);
        return nbus > 0;
    }

    void settingsFileReloaded() override;
    void settingsChanged(String const& name, var const& value) override;
    t_canvas* getCanvasBySymbol(const String& canvas_symbol);
    // Strict variant for mutations: resolves ONLY 'main'/'pd-main'/'' (focused
    // root) and genuinely registered canvas symbols. Returns nullptr for
    // unknown names instead of silently falling back to the first canvas
    // (fault gauntlet 2026-09-09 bug #11 — wrong-canvas injection).
    t_canvas* getCanvasBySymbolStrict(const String& canvas_symbol);
    void synchroniseCanvases();
    bool getIsProcessingAudio() const noexcept { return isProcessingAudio.load(std::memory_order_relaxed); }

    static bool initialiseFilesystem();
#if JUCE_IOS
    static void syncDirectoryFiles(File const& sourceDir, File const& targetDir, Time lastInitTime = Time(), bool deleteIfNotExists = false);
#endif
    void updateSearchPaths();

    void sendMidiBuffer(int device, MidiBuffer const& buffer);
    void sendPlayhead();
    void sendParameters();

    void updateEnabledParameters();
    SmallArray<PlugDataParameter*> getEnabledParameters();

    SmallArray<PluginEditor*> getEditors() const;

    void enableAudioParameter(SmallString const& name);
    void disableAudioParameter(SmallString const& name);
    void handleParameterMessage(SmallArray<pd::Atom> const& atoms) override;

    void performLatencyCompensationChange(float value) override;
    void sendParameterInfoChangeMessage();

    void fillDataBuffer(SmallArray<pd::Atom> const& list) override;
    void parseDataBuffer(XmlElement const& xml) override;
    std::unique_ptr<XmlElement> extraData;

    pd::Patch::Ptr loadPatch(String patch);
    pd::Patch::Ptr loadPatch(URL const& patchURL);

    void titleChanged() override;

    void setTheme(String themeToUse, bool force = false);

    void runBackupLoop();

    int lastUIWidth = 1000, lastUIHeight = 650;

    AtomicValue<float>* volume;
    ValueTree pluginModeTheme;
    float pluginModeScale = 1.0f;

    String currentThemeName;

    SettingsFile* settingsFile;

    std::unique_ptr<pd::Library> objectLibrary;

    File abstractions = ProjectInfo::versionDataDir.getChildFile("Abstractions");

    Value commandLocked = Value(var(false));

    std::unique_ptr<StatusbarSource> statusbarSource;

    struct AiMark { int state = 0; bool transient = false; };
    // PRD overlay: AI state marker for a gobj (0 = none). Used by Object::paint.
    // THREAD-SAFE: render runs on the VBlank thread, writers on the message
    // thread — all overlay access is guarded by mcpOverlayLock.
    int getAiOverlayState(t_gobj* g) const
    {
        juce::ScopedLock sl(mcpOverlayLock);
        auto it = mcpAiOverlay.find(g);
        return it != mcpAiOverlay.end() ? it->second.state : 0;
    }

    // PRD overlay: ghost/preview of PROPOSED (uncommitted) changes, drawn by the
    // canvas above the patch. Boxes are in patch coords; wires are patch-coord
    // endpoints. Written on the message thread; read on the VBlank render thread.
    struct McpGhost {
        int kind = 0;                              // 0 = box, 1 = wire
        float x = 0, y = 0, w = 0, h = 0;          // box
        float x1 = 0, y1 = 0, x2 = 0, y2 = 0;      // wire
        juce::String label;
    };
    std::vector<McpGhost> mcpGhosts;
    std::vector<McpGhost> getMcpGhosts() const { juce::ScopedLock sl(mcpOverlayLock); return mcpGhosts; }

    // PRD overlay: short in-place AI annotations ("why"/"what"), drawn as a
    // translucent tag near an object (patch coords). Explanation lives where it
    // belongs — on the patch, not in a chat window.
    struct McpAnnotation {
        float x = 0, y = 0; // patch coords (placed in the margin beside the target)
        juce::String text;
        juce::String targetId; // tempId this note is attached to (may be empty)
        juce::String kind;     // info | change | warn | artist  (drives the colour)
        double t = 0;          // creation time (s) — notes auto-fade after a TTL
        bool hasLeader = false;      // draw a thin line from the note to the object
        float leaderX = 0, leaderY = 0; // point on the object the line points to
    };
    std::vector<McpAnnotation> mcpAnnotations;
    std::vector<McpAnnotation> getMcpAnnotations() const { juce::ScopedLock sl(mcpOverlayLock); return mcpAnnotations; }

    // PRD overlay: titled REGIONS — translucent boxes grouping objects (e.g. "FX",
    // "Loops", "Drums"), drawn BEHIND the objects so the patch's structure is
    // visible at a glance. Bounds are in patch coords.
    struct McpRegion {
        float x = 0, y = 0, w = 0, h = 0;
        juce::String title;
        juce::String kind; // e.g. source | fx | loop | output | group
    };
    std::vector<McpRegion> mcpRegions;
    std::vector<McpRegion> getMcpRegions() const { juce::ScopedLock sl(mcpOverlayLock); return mcpRegions; }

    // PRD overlay lifecycle: wipe ALL AI overlay state (annotations, regions, ghosts,
    // and per-object marks). Called when the active patch changes (new patch, open,
    // close) so overlay never leaks onto the next patch — and so the gobj*-keyed marks
    // can never dangle onto deleted objects after a canvas is rebuilt. Message thread;
    // the render thread reads all of these under mcpOverlayLock.
    void clearAiOverlays()
    {
        juce::ScopedLock sl(mcpOverlayLock);
        mcpGhosts.clear();
        mcpAnnotations.clear();
        mcpRegions.clear();
        mcpAiOverlay.clear();
    }

    // Guards mcpAiOverlay / mcpGhosts / mcpAnnotations: written on the message
    // thread, read on the VBlank render thread.
    mutable juce::CriticalSection mcpOverlayLock;

    Value tailLength = Value(0.0f);

    // Just so we never have to deal with deleting the default LnF
    SharedResourcePointer<PlugDataLook> lnf;

    static constexpr int numParameters = 512;
    static constexpr int numInputBuses = 16;
    static constexpr int numOutputBuses = 16;

    // Protected mode value will decide if we apply clipping to output and remove non-finite numbers
    AtomicValue<bool> enableLimiter = true;

    // Zero means no oversampling
    AtomicValue<int> oversampling = 0;

    std::unique_ptr<InternalSynth> internalSynth;

    OwnedArray<PluginEditor> openedEditors;

    AtomicValue<ConnectionMessageDisplay*, Sequential> connectionListener = nullptr;
    std::unique_ptr<Autosave> autosave;

private:
    int customLatencySamples = 0;

    SmoothedValue<float> smoothedGain;

    AtomicValue<int> audioAdvancement = 0;

    bool variableBlockSize = false;
    AudioBuffer<float> audioBufferIn;
    AudioBuffer<float> audioBufferOut;
    AudioBuffer<float> bypassBuffer;

    HeapArray<float> audioVectorIn;
    HeapArray<float> audioVectorOut;

    std::unique_ptr<AudioMidiFifo> inputFifo;
    std::unique_ptr<AudioMidiFifo> outputFifo;

    MidiBuffer blockMidiBuffer;
    MidiBuffer midiBufferInternalSynth;

    MidiDeviceManager midiDeviceManager;

    AudioProcessLoadMeasurer cpuLoadMeasurer;

    bool midiByteIsSysex = false;
    uint8 midiByteBuffer[512] = {};
    size_t midiByteIndex = 0;

    SmallArray<pd::Atom> atoms_playhead;
    SmallArray<PlugDataParameter*> enabledParameters;

    int lastSetProgram = 0;

    Limiter limiter;
    std::unique_ptr<dsp::Oversampling<float>> oversampler;

    UnorderedMap<uint64_t, std::unique_ptr<Component>> textEditorDialogs;

#if PERFETTO
    std::unique_ptr<perfetto::TracingSession> tracingSession;
#endif

    static inline String const else_version = "ELSE v1.0-rc13";
    static inline String const cyclone_version = "cyclone v0.9-2";
    static inline String const heavylib_version = "heavylib v0.4";
    static inline String const gem_version = "Gem v0.94";
    // this gets updated with live version data later
    static String pdlua_version;

    class HostInfoUpdater final : public AsyncUpdater {
    public:
        explicit HostInfoUpdater(PluginProcessor* parentProcessor)
            : processor(*parentProcessor)
        {
        }

        void update()
        {
            if (ProjectInfo::isStandalone)
                return;
#if JUCE_IOS
            handleAsyncUpdate(); // iOS doesn't like it if we do this asynchronously
#else
            triggerAsyncUpdate();
#endif
        }

    private:
        void handleAsyncUpdate() override
        {
            auto const details = AudioProcessorListener::ChangeDetails {}.withParameterInfoChanged(true);
            processor.updateHostDisplay(details);
        }

        PluginProcessor& processor;
    };

    HostInfoUpdater hostInfoUpdater;

    int backupRunLoopInterval;
    TimedCallback backupRunLoop = TimedCallback([this] { runBackupLoop(); });
    CriticalSection backupLoopLock;
    std::atomic<bool> isProcessingAudio;
    t_gobj* resolveStableId(const String& canvasName, const String& objectId);

    // Full identity reconcile against the live canvas (gl_list): evict entries whose
    // gobj is gone, adopt untracked objects (gui_<class>_<n>), bump mcpIdentityVersion.
    // Caller must be on the message thread / hold the Pd lock (same context as
    // resolveStableId). Makes the tempId cache equal gl_list truth before any op that
    // resolves ids — so a stale id can never silently map to the wrong object.
    void reconcileIdentity(const String& canvasName);
    std::unordered_map<std::string, std::unordered_map<std::string, t_gobj*>> mcpStableObjectMap;
    std::unordered_map<t_gobj*, uint64_t> mcpStableSerialMap;
    // PRD overlay: per-object AI state for the canvas AI overlay
    // (state 1 = changed, 2 = proposed, 3 = error) + set-time so a *transient*
    // auto-clear can never erase a later *persistent* mark. Message-thread only.
    std::unordered_map<t_gobj*, AiMark> mcpAiOverlay;
    uint64_t mcpSerialCounter = 1;
    // Monotonic version counter — incremented on every identity mutation
    // (create, delete, rename, register, clear). Node.js can poll this cheaply
    // to decide whether a full snapshot fetch is needed.
    std::atomic<uint64_t> mcpIdentityVersion { 0 };
    int mcpSuspendedDspState = 0;
    std::unique_ptr<MCPBridge> mcpBridge;
    std::map<t_canvas*, std::vector<McpTransaction>> mcpUndoStack;
    std::map<t_canvas*, std::vector<McpTransaction>> mcpRedoStack;
    // Undo/redo stacks are pushed on the OSC thread and read/popped on the
    // message thread — mcpUndoLock guards all access. Held only for map ops,
    // never across the nested batch_atomic execution (avoids re-entry deadlock).
    mutable CriticalSection mcpUndoLock;

    // MCP zero-dropout WAV recorder — taps outputFifo directly in processBlock.
    // No canvas objects created, no DSP recompile, zero audio dropout.
    CriticalSection mcpRecorderLock;
    std::unique_ptr<juce::AudioFormatWriter> mcpWavWriter;
    std::atomic<bool> mcpRecording { false };
    juce::String mcpRecorderPath;

    // Armed peak-hold for trigger verification (/meter/arm → trigger → /meter/read).
    // Accumulated on the audio thread over the FINAL output, so a one-shot fired
    // between arm and read can NEVER be missed (window opens before the event).
    std::atomic<bool> mcpArmActive { false };
    std::atomic<float> mcpArmPeak { 0.0f };

    // Armed spectral capture (PRD_MEASUREMENT_WINDOWS Phase 5). From arm → read,
    // the FINAL output is appended to a preallocated mono buffer so a one-shot is
    // inside the window; read runs FFTW over the loudest 1024-sample region. This
    // is the spectral twin of the peak-hold — no more false negatives from a
    // window that opened after the hit. Written on the audio thread, drained on
    // the OSC thread via the armed/busy handshake (no locks on the audio path).
    std::atomic<bool> mcpSpecArmed { false };
    std::atomic<bool> mcpSpecBusy { false };
    std::atomic<int> mcpSpecWritePos { 0 };
    std::atomic<int> mcpSpecLimit { 0 };
    std::atomic<int> mcpSpecCapacity { 0 };
    std::vector<float> mcpSpecBuffer;

    // Writes the current output block into the active WAV writer, if recording.
    // Must be called from the audio thread on the final output buffer.
    void writeRecorderTap(dsp::AudioBlock<float> const& buffer);

    friend class MCPBridge;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginProcessor)
};
