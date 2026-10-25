/*
 // The MCP host (Plan 3 P3.2 S4): owns the MCPBridge and the McpSession, and the
 // bridge-only functions that used to be PluginProcessor members (identity lookup,
 // canvas lookup, transactions, recorder and monitor taps). PluginProcessor holds one
 // std::unique_ptr<McpHost> and no MCP state.
 */
#pragma once

#include "Mcp/McpSession.h"
#include "Mcp/McpIdentity.h"
#include "Mcp/McpGuiHost.h"
#include "Mcp/McpSysList.h"
#include <juce_dsp/juce_dsp.h>

#include "Pd/Instance.h"
#include "Pd/MCPBridgeRegistry.h"
#include "Pd/MCPBridgeHandoff.h"
#include "Pd/MCPBridgeV2Router.h" // P4.1 S3: the request's work token travels with each hop
#include "Mcp/McpAudit.h"
#include "Mcp/McpCeilingConsole.h"
#include <optional>
#include <type_traits>

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
    int xrunCount() const;
    juce::String getMcpBridgeStatus() const;
    // P3.5 S2: identity state is touched only under the Pd lock (sys_lock, recursive). Scoped lock for any thread.
    // Never hold it while waiting for another context (THREADING.md rule 2); take it before mcpOverlayLock (rule 10).
    class PdLockScope {
    public:
        explicit PdLockScope(PluginProcessor& p);
        ~PdLockScope();
        PdLockScope(PdLockScope const&) = delete;
        PdLockScope& operator=(PdLockScope const&) = delete;
    };

    // The identity methods below take the Pd lock themselves (any thread; they hand out values, not references).
    // Reverse identity lookup (C++ truth): the stable tempId for a live pd object,
    // read straight from the bridge's own stable map — never from the TS mirror.
    juce::String getStableId(t_gobj* g) const;
    juce::String getOrAdoptStableId(t_canvas* canvas, t_gobj* g);
    // P3.4 S7: straight to OSC (floats as floats, strings as strings, as before). The Pd-side
    // "mcp_telemetry_reply" message it also sent had no receiver anywhere (the Lua bridge was retired).
    void sendMCPReply(const String& replyAddr, mcp::SysList const& values);

    // P4.4: one undo history per canvas, Pd's own queue, for AI and manual edits. One step back (or forward); false
    // when there was nothing to take. Ids of objects the step takes away come back with the objects (undo journal).
    // Takes the Pd lock; any thread that may lock Pd.
    bool undoStep(t_canvas* cnv, bool redo);
    // The undo journal of a canvas (Pd context); cleared when its queue is flushed.
    std::vector<McpUndoGhost>& undoJournalFor(t_canvas* cnv);
    void clearUndoJournal(t_canvas* cnv);
    // An MCP edit is about to take `obj` (named `id`) away in an undoable step: its id goes to the journal, so an
    // undo that brings the object back gives it the id again. Pd context (lock held).
    void journalId(t_canvas* cnv, std::string const& id, t_gobj* obj);
    bool settingsChanged(String const& name, var const& value);
    // P9: the owner's switch for the output ceiling, from the settings panel only (the one way to switch it off
    // while PlugData runs; on also resets the breaker). Message thread.
    void setOutputCeilingFromGui(bool on);
    // P9 round 1: only a literal JSON `false` switches the ceiling off; anything else (malformed) is on.
    static bool ceilingSettingIsOn(juce::var const& value);
    // P9 round 1: read the setting into the ceiling as at startup (createBridge): a malformed value is on and is
    // written back as true; off posts a loud console line. Message thread.
    void applyOutputCeilingSettingAtStartup();
    // P9 round 1: posts each breaker trip and release once to the console (the ceiling timer, every 250 ms); round 2:
    // at most one line per kind per 10 s, then a summary (Mcp/McpCeilingConsole.h).
    // Message thread; never the audio thread.
    void pollOutputCeiling();
    // P9: the `ceiling` object of the /pd/limiter reply (JSON). Any thread.
    juce::String outputCeilingJson() const;
    void mcpSettingsChanged(String const& name, var const& value);
    t_canvas* getCanvasBySymbol(const String& canvas_symbol);
    // P3.4 S3: the true root of the focused editor's current canvas, published by the message thread
    // (refreshFocusedRoot: a 50 ms timer and every MCP tab change). getCanvasBySymbol("main") reads it, so
    // no command thread touches an editor. Message thread only.
    void refreshFocusedRoot();

    // P3.4 S8: callAsync that runs `fn` only while this host still exists (checked on the message thread,
    // where the host is destroyed). Every callback the host posts goes through it.
    void postMessage(std::function<void()> fn);
    // P3.4 S8: called first in ~PluginProcessor (MCP-HOOK(teardown)), before the patches are freed: stops the
    // bridge (receiver, command worker, background jobs) and drops the queued Pd jobs.
    void shutdown();

    // P3.4 S3: run `fn` on the JUCE message thread and return its result (inline when already there).
    // Empty when the message thread did not start it within timeoutMs; it then never runs, so `fn` may
    // capture the caller's frame by reference. NEVER call it while holding sys_lock: the message thread may
    // be waiting for that lock, and the audio thread needs it every block (the audit reports such a call).
    template<typename F>
    auto onMessageThread(F&& fn, int timeoutMs, char const* where) -> std::optional<std::invoke_result_t<F&>>
    {
        using R = std::invoke_result_t<F&>;
        auto* mm = juce::MessageManager::getInstanceWithoutCreating();
        if (mm == nullptr)
            return std::nullopt;
        if (mm->isThisTheMessageThread())
            return std::optional<R>(fn());
        if (mcp::audit::holdsPdLock())
            mcp::audit::reportWaitUnderLock(where);
        auto handoff = std::make_shared<mcp::OneShotHandoff>();
        std::optional<R> result;
        mm->callAsync([handoff, &fn, &result, work = mcp::v2::captureWork()] {
            if (!handoff->begin())
                return; // the caller gave up: its frame may be gone
            mcp::OneShotHandoff::FinishGuard finishGuard(*handoff);
            mcp::v2::WorkScope const scope(work);
            result.emplace(fn());
        });
        if (handoff->waitOrAbandon(std::chrono::milliseconds(timeoutMs)) != mcp::OneShotHandoff::Result::Finished)
            return std::nullopt;
        return result;
    }
    // Strict variant for mutations: resolves ONLY 'main'/'pd-main'/'' (focused
    // root) and genuinely registered canvas symbols. Returns nullptr for
    // unknown names instead of silently falling back to the first canvas
    // (fault gauntlet 2026-09-09 bug #11 — wrong-canvas injection).
    t_canvas* getCanvasBySymbolStrict(const String& canvas_symbol);
    void synchroniseCanvases();
    // P3.5 S3: makes an identity record for every open canvas (roots oldest first, subcanvases depth-first, so
    // an earlier canvas keeps the plain wire key and a later duplicate gets "#2") and drops dead ones. Takes the
    // Pd lock.
    void registerOpenCanvases();
    // P11 V2: the linear gain PlugData's master volume applies now (its slider mapped as in processBlock: 0.8 is
    // unity, below it (v / 0.8) ^ 2.5, above it 1 + (v - 0.8) / 0.2). Pinned by runMeterTapCases.
    float masterGain() const;
    static float masterGainFromSlider(float slider);
    // P3.5 S3: a canvas's wire key before any "#n" (root: file name; subcanvas: pd-<name>; unnamed: pd-anon).
    static juce::String identityBaseKey(t_canvas* cnv);
    // P3.5 S5 (L9): tombstones for the ids of every instance of abstraction `fileName` (call before a reload),
    // and their rebind to the recreated instances (registerOpenCanvases calls it; so does encapsulate_to_file).
    void tombstoneAbstractionInstances(juce::String const& fileName);
    void rebindTombstones();
    // P3.5 S4: the prefix of a minted tempId (owner decision O1: type + number).
    static juce::String mintPrefix(t_gobj* g);

    t_gobj* resolveStableId(const String& canvasName, const String& objectId);
    // Full identity reconcile against the live canvas (gl_list): evict entries whose
    // gobj is gone, adopt untracked objects (minted <type>_<n>, S4), bump the identity version.
    // Takes the Pd lock itself (P3.5 S2). Makes the tempId cache equal gl_list truth before any op that
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

    // The Pd context (Plan 3 P3.4 S2/S3): the message thread under sys_lock (see McpAudio.cpp).
    // enqueuePd: any thread, never waits; posts a drain. pdWait: the same, then waits up to timeoutMs; false =
    // abandoned (the job never ran and never will, so it may capture the caller's frame by reference); on the
    // message thread it runs the job inline under sys_lock. drainPdQueue: posted by enqueuePd only.
    void enqueuePd(std::function<void()> job);
    // In the Pd context now: inline under sys_lock when called on the message thread (an MCP undo/redo
    // replays batch_atomic there; queuing would wait for a drain that thread itself must run), else queued.
    void runPd(std::function<void()> job);
    bool pdWait(std::function<void()> const& job, int timeoutMs);
    void drainPdQueue();

    // Audio hooks (Source/Mcp/McpAudio.cpp); each runs where its block used to be.
    void prepare(double sampleRate, int samplesPerBlock);
    void audioBlockStarted();
    void midiTick(MidiBuffer& midiInputHistory, AudioBuffer<float>& buffer, double sampleRate);
    void audioBlockFinished(AudioBuffer<float>& buffer);
    void pdBlock(float* audioVectorIn, float* audioVectorOut, int pdBlockSize, int numChannels);

    // Telemetry hooks (Source/Mcp/McpTelemetry.cpp).
    void selectionChanged(SmallArray<juce::Component*>& objects);
    void sendSelectionTelemetry(String const& selector, SmallArray<pd::Atom> const& list);
    // `object` is the Pd object that printed the line (null when none): an error line is pushed with its tempId (T2b).
    void consoleMessage(String const& message, bool isError, void* object = nullptr);

    // The mcp_* Pd system messages (Source/Mcp/McpSysCommands.cpp). Returns false for
    // every other selector, which PluginProcessor::receiveSysMessage then handles.
    bool handleSysMessage(SmallString const& selector, SmallArray<pd::Atom> const& list);
    // P5.1 sink floor (McpSysCommands.cpp): a patch's `__param` message for the master `volume` that would widen
    // its range past 0..1 or set it above 1 is refused or clamped. True = consumed (plugdata's handler skips it).
    bool guardParameterMessage(SmallArray<pd::Atom> const& atoms);
    // P3.4 S7: the bridge's way into an mcp_* command: plain strings and numbers, no Pd symbols made.
    bool callSys(SmallString const& selector, mcp::SysList const& list);

private:
    // Selector table (McpSysVerbs.inc): one private member function per row, built at
    // startup like the bridge's verb table.
    using SysHandler = void (McpHost::*)(SmallString const& selector, mcp::SysList const& list);
    using SysTable = mcp::CommandTable<SysHandler>;
    static SysTable const& sysTable();

#define MCP_SYS(name, handler) void handler(SmallString const& selector, mcp::SysList const& list);
#include "Mcp/McpSysVerbs.inc"

public:

    // The bridge's only way to the GUI (Source/Mcp/GuiHost.cpp, S6).
    McpGuiHost& gui() { return *guiHost; }

    PluginProcessor& processor;
    McpSession session;
    // tempIds, serials and the identity version (P3.5: the only identity state).
    mcp::IdentityService identity;
    // Declared before the bridge, so it outlives it.
    std::unique_ptr<McpGuiHost> guiHost;
    // Declared after the session, so it is destroyed first: its threads stop
    // before the state they touch goes away.
    std::unique_ptr<MCPBridge> mcpBridge;

private:
    // Calls refreshFocusedRoot every 50 ms on the message thread (started in createBridge).
    struct FocusTimer final : juce::Timer {
        explicit FocusTimer(McpHost& h) : host(h) {}
        void timerCallback() override { host.refreshFocusedRoot(); }
        McpHost& host;
    };
    FocusTimer focusTimer { *this };
    // P9 round 1: calls pollOutputCeiling every 250 ms on the message thread (started in createBridge).
    struct CeilingTimer final : juce::Timer {
        explicit CeilingTimer(McpHost& h) : host(h) {}
        void timerCallback() override { host.pollOutputCeiling(); }
        McpHost& host;
    };
    CeilingTimer ceilingTimer { *this };
    mcp::CeilingConsole ceilingConsole; // round 2: rate-limited trip / release lines; message thread

public:
    JUCE_DECLARE_WEAK_REFERENCEABLE(McpHost)
    JUCE_DECLARE_NON_COPYABLE(McpHost)
};
