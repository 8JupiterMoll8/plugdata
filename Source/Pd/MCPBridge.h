/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_osc/juce_osc.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <readerwriterqueue.h>
#include <mutex>
#include <optional>
#include <array>
#include <functional>
#include <memory>
#include <type_traits>
#include "Utility/Config.h"
#include "Pd/Instance.h"
#include "Pd/MCPIdentity.h" // renameStableId (also used by PluginProcessor.cpp; finding C6)
#include "Pd/MCPBridgeRegistry.h" // verb table (Plan 3 P3.3)
#include "Pd/MCPBridgeWorker.h"   // command worker and reply queue (Plan 3 P3.4)
#include "Pd/MCPBridgeV2Server.h" // v2 TCP listener (Plan 4 P4.1)
#include "Pd/MCPBridgeV2Router.h" // v2 reply routing (P4.1 S3)

class PluginProcessor;
class MCPBridge;
namespace mcp { struct BatchJob; }

// PRD_MEASUREMENT_WINDOWS Phase 4 — armed CONTROL-message window. A native Pd
// receiver is bound to the requested name on arm; every message dispatched to
// that name accumulates into this buffer until read. Defined in MCPBridge.cpp.
struct McpControlWindow;

// PRD "Real Class Name Sync" §2.1 — the single shared class-name resolution
// helper. C++ is the ONLY source of truth for object identity: the census
// (PluginProcessor.cpp mcp_census) and the /pd/typeof endpoint both resolve
// abstraction names through this one function so every code path agrees.
//
// Returns true + outName (e.g. "crusher.m~") when gobj is a canvas-class
// object loaded from a file (MERDA .m~ module, .pd abstraction, or GOP panel
// loaded from disk). Returns false for inline [pd foo] subpatches, which
// genuinely are "pd". Caller must hold sys_lock() (both call sites do).
namespace pd {
inline bool getAbstractionFileName(t_gobj* gobj, juce::String& outName)
{
    // Step 1: only applies to canvas-class objects (subpatch/abstraction wrappers)
    if (pd_class(&gobj->g_pd) != canvas_class) return false;

    auto* sub = reinterpret_cast<t_canvas*>(gobj);

    // Step 2: canvas_isabstraction() (g_canvas.c) tests gl_env != NULL — the
    // canonical Pd-internal check. gl_env is allocated by canvas_new() ONLY
    // when the canvas was loaded from disk (glob_setfilename/binbuf_evalfile).
    // Inline [pd foo] subpatches have gl_env == NULL.
    if (!canvas_isabstraction(sub)) return false;

    // Step 3: gl_name holds the name the canvas was bound to. For a MERDA
    // module loaded from disk this is the file name (e.g. "crusher.m~"),
    // NOT the full path. Inline [pd foo] never reaches this point.
    if (!sub->gl_name) return false;

    juce::String name = juce::String::fromUTF8(sub->gl_name->s_name);

    // Strip any directory prefix Pd may leave on the bound name
    int lastSlash = name.lastIndexOfChar('/');
    if (lastSlash >= 0) name = name.substring(lastSlash + 1);

    // Normalize the .pd resolution suffix: Pd resolves the box name
    // "drive.m~" to the disk file "drive.m~.pd", so gl_name carries the
    // appended ".pd". The GUI box, the artist, and the selection telemetry
    // all say "drive.m~" — one object, one identity, everywhere (PRD §2.0)
    // means census must agree: strip the trailing ".pd" when what remains
    // is itself a MERDA module name ending in ".m~".
    if (name.endsWith(".pd")) {
        juce::String base = name.substring(0, name.length() - 3);
        if (base.endsWith(".m~")) {
            name = base;
        }
    }

    outName = name;
    return true;
}

// PRD Phase 2 §2.1: C++ port of the TS sanitizeTempIdSegment — the naming
// policy moves INTO the bridge so C++ mints tempIds itself and TS only
// mirrors. Must produce IDENTICAL output to the TS implementation
// (parity-tested): "crusher.m~" → "crusher", "drive.m~.pd" → "drive",
// "plate.rev.m~" → "plate_rev", "myabs.pd" → "myabs", "osc~" → "osc_sig".
inline juce::String sanitizeClassNameForTempId(const juce::String& className)
{
    auto sanitizeBase = [](const juce::String& s) {
        juce::String out;
        for (auto ch : s) {
            if (juce::CharacterFunctions::isLetterOrDigit(ch) || ch == '_')
                out << ch;
            else
                out << '_';
        }
        while (out.startsWith("_")) out = out.substring(1);
        while (out.endsWith("_")) out = out.dropLastCharacters(1);
        return out;
    };

    juce::String raw = className.trim();
    if (raw.isEmpty()) return "obj";

    // Normalize "X.m~.pd" → "X.m~" (Pd file-resolution suffix, mirrors TS)
    if (raw.endsWith(".pd") && raw.dropLastCharacters(3).endsWith(".m~"))
        raw = raw.dropLastCharacters(3);

    int lastDot = raw.lastIndexOfChar('.');
    if (lastDot > 0) {
        juce::String ext = raw.substring(lastDot + 1);
        if (ext == "m~" || ext == "pd") {
            juce::String base = sanitizeBase(raw.substring(0, lastDot));
            return base.isNotEmpty() ? base : juce::String("abstraction");
        }
    }

    // Vanilla/ELSE signal objects: trailing "~" → "_sig"
    juce::String body = raw.endsWith("~") ? raw.dropLastCharacters(1) + "_sig" : raw;
    juce::String out = sanitizeBase(body);
    return out.isNotEmpty() ? out : juce::String("obj");
}
} // namespace pd

struct ProbeResult {
    uint32_t probeId = 0;
    float rms = 0.0f;
    float peak = 0.0f;
    int blockSize = 0;
};

static constexpr int PROBE_RING_SIZE = 1024;
static constexpr int MAX_PROBES = 16;

struct ActiveProbe {
    std::atomic<bool> active { false };
    std::atomic<t_outconnect*> outconnect { nullptr };
    // P3.4 S5: the DSP-sort number (ugen_getsortno) when `outconnect` was resolved, under the Pd lock. Pd frees
    // connections and signals only with a DSP rebuild, which bumps that number; the audio thread reads the probe
    // only while the two agree (and under the lock), else it asks the timer to resolve it again.
    std::atomic<int> epoch { 0 };
    std::atomic<bool> needsResolve { false };
    uint32_t probeId = 0;
    juce::String correlationId;
    juce::String canvasName;
    juce::String tempId;
    int outletIndex = 0;
    int durationMs = 500;
    juce::int64 startTimeMs = 0;
    bool spectral = false; // If true, run FFT on completion instead of simple RMS/peak

    moodycamel::ReaderWriterQueue<ProbeResult> resultQueue { 256 };

    // P3.4 S11 (TSan): written by the audio tick while the timer may copy it: relaxed atomics, no tearing UB.
    std::array<std::atomic<float>, PROBE_RING_SIZE> ringBuffer {};
    std::atomic<int> ringWritePos { 0 };

    float accRms = 0.0f;
    float accPeak = 0.0f;
    int accBlocks = 0;
};

class ProbeManager {
public:
    explicit ProbeManager(MCPBridge* owner);

    void audioTick();
    int startProbe(t_outconnect* oc, int epoch, const juce::String& canvasName, const juce::String& tempId, int outletIndex, const juce::String& correlationId, int durationMs);
    void stopProbe(uint32_t probeId, const juce::String& correlationId = {});
    void stopAllProbes(const juce::String& correlationId = {});
    void collectResults();

    int getActiveProbeCount() const;
    bool isDebugEnabledByUs() const { return debugWasEnabledByUs; }
    void setDebugEnabledByUs(bool v) { debugWasEnabledByUs = v; }

    ActiveProbe probes[MAX_PROBES];

private:
    MCPBridge* bridge = nullptr;
    std::atomic<uint32_t> nextProbeId { 1 };
    bool debugWasEnabledByUs = false;
};

// The reply queue (Plan 3 P3.4 S1; Report F3). juce::OSCSender is not
// thread-safe and the bridge replies from the command worker, the message
// thread, timers and the render thread. Every thread posts here; one thread
// owns the OSCSender and sends in post order. connect/disconnect are queued
// too, so a reconnect stays ordered with the replies around it. Nothing on the
// audio thread may post (posting copies the message).
class ReplyQueue {
public:
    ReplyQueue()
    {
        worker.start([this](Op op) { run(std::move(op)); });
    }
    ~ReplyQueue() { worker.stop(); } // replies still queued at teardown are dropped

    bool connect(juce::String const& host, int port) { return worker.post(Op { Op::Connect, {}, host, port, {} }); }
    bool disconnect() { return worker.post(Op { Op::Disconnect, {}, {}, 0, {} }); }
    // P4.1 S3: the reply carries the origin of the code that sent it (mcp::v2::currentOrigin() on this thread).
    bool send(juce::OSCMessage const& message) { return worker.post(Op { Op::Send, message, {}, 0, mcp::v2::currentOrigin() }); }
    // P4.1 S3: posted when a request's work token goes (the handler and every hop it posted finished), so it is
    // queued behind every reply they sent and the router sees them first.
    bool handlerDone(mcp::v2::OriginPtr origin) { return worker.post(Op { Op::HandlerDone, {}, {}, 0, std::move(origin) }); }
    bool tick() { return worker.post(Op { Op::Tick, {}, {}, 0, {} }); }

    // P4.1 S3: every message passes the v2 router first (nullptr = none). The router outlives this queue.
    void setRouter(mcp::v2::Router* r) { router.store(r); }
    // Off while the bridge does not own its UDP port: what is left after routing is dropped, so another app's
    // MCP server on the reply port never gets this bridge's messages.
    void setUdpEnabled(bool enabled) { udpEnabled.store(enabled); }

    // Tests (Tests/mcp/bridge_handler_tests.cpp): with a sink set, every message goes to it instead of the
    // socket, on the reply thread.
    void setSink(std::function<void(juce::OSCMessage const&)> newSink)
    {
        std::lock_guard<std::mutex> const l(sinkLock);
        sink = std::move(newSink);
    }

private:
    struct Op {
        enum Kind { Send, Connect, Disconnect, HandlerDone, Tick } kind = Send;
        std::optional<juce::OSCMessage> message;
        juce::String host;
        int port = 0;
        mcp::v2::OriginPtr origin;
    };

    static std::int64_t nowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    static mcp::v2::OutMessage toOut(juce::OSCMessage const& message)
    {
        mcp::v2::OutMessage out;
        out.address = message.getAddressPattern().toString().toStdString();
        out.args.reserve(static_cast<size_t>(message.size()));
        for (auto const& a : message) {
            mcp::v2::Arg arg;
            if (a.isInt32()) {
                arg.type = 'i';
                arg.i = a.getInt32();
            } else if (a.isFloat32()) {
                arg.type = 'f';
                arg.f = a.getFloat32();
            } else if (a.isString()) {
                arg.s = a.getString().toStdString();
            } else if (a.isBlob()) {
                arg.s = a.getBlob().toBase64Encoding().toStdString(); // no reply sends one today
            } else {
                continue; // colours, T/F/nil: not used by the bridge
            }
            out.args.push_back(std::move(arg));
        }
        return out;
    }

    void run(Op op)
    {
        switch (op.kind) {
        case Op::Connect:
            inner.connect(op.host, op.port);
            return;
        case Op::Disconnect:
            inner.disconnect();
            return;
        case Op::HandlerDone:
            if (auto* r = router.load())
                r->handlerDone(op.origin, nowMs());
            return;
        case Op::Tick:
            if (auto* r = router.load())
                r->tick(nowMs());
            return;
        case Op::Send: {
            if (auto* r = router.load(); r && (r->active() || op.origin))
                if (!r->route(toOut(*op.message), op.origin.get(), nowMs()))
                    return; // a v2 session's reply or an artist event for v2: not for UDP
            if (!udpEnabled.load())
                return;
            std::function<void(juce::OSCMessage const&)> s;
            {
                std::lock_guard<std::mutex> const l(sinkLock);
                s = sink;
            }
            if (s)
                s(*op.message);
            else
                inner.send(*op.message);
            return;
        }
        }
    }

    juce::OSCSender inner; // touched only on the reply thread
    std::atomic<mcp::v2::Router*> router { nullptr };
    std::atomic<bool> udpEnabled { false };
    std::mutex sinkLock;
    std::function<void(juce::OSCMessage const&)> sink;
    mcp::SerialWorker<Op> worker; // last: stopped (joined) first, before `inner` goes
};

class MCPBridge final : public juce::OSCReceiver::Listener<juce::OSCReceiver::RealtimeCallback>
                      , private juce::Timer
{
public:
    explicit MCPBridge(PluginProcessor* processor, int listenPort = 9000, int sendPort = 19010);
    ~MCPBridge() override;

    // Identity sidecar (<file>.mcpids.json), P3.5 S7: format v2 with each entry's class and the SHA-256 of the
    // .pd written beside it (see MCPBridge.cpp). Written by the MCP save and by plugdata's GUI save hook.
    struct SidecarStats {
        int restored = 0; // names bound again
        int renamed = 0;  // objects whose saved name no longer fits (fresh name), or saved indices out of range
        int skipped = 0;  // saved names already used by another live object (#38)
        bool fileMatches = false;
    };
    // The one applier (load_patch and open_patch). Caller holds the Pd lock.
    static SidecarStats mcpApplyIdentitySidecar(PluginProcessor* processor, juce::DynamicObject* sidecarObj,
                                                t_canvas* cnv, juce::File const& pdFile);
    // Reads and checks <pdFile>.mcpids.json; void (with `note` set) when it is truncated or unreadable.
    static juce::var readIdentitySidecar(juce::File const& pdFile, juce::String& note);
    static juce::String sidecarReport(juce::File const& pdFile, SidecarStats const& stats, juce::String const& note);
    static juce::String sidecarPdHash(juce::File const& pdFile);
    // buildIdentitySidecar: CALLER HOLDS the Pd lock.
    static juce::var buildIdentitySidecar(PluginProcessor* processor, t_canvas* cnv);
    // Adds the hash of the .pd on disk and writes the sidecar (temp + rename). "" on success, else an error.
    static juce::String writeIdentitySidecarFile(juce::var sidecar, juce::File const& pdFile);
    // Takes the Pd lock itself. Returns "" on success, an error string otherwise.
    static juce::String writeIdentitySidecar(PluginProcessor* processor, t_canvas* cnv,
                                             const juce::String& destFilePath);

    bool start();
    void stop();
    bool isConnected() const;
    // P3.4 S8: new ports on the SAME object (it used to be destroyed and rebuilt while the audio thread could be
    // inside audioTick/midiInputTick). Restarts only when it was running.
    void rebind(int newListenPort, int newSendPort);
    // P3.4 S8: before the processor frees its patches: stop receiving, finish or cancel the background jobs
    // (offline render, voice analysis), and drop what is still to be posted. Idempotent; the destructor calls it.
    void shutdown();
    bool isShuttingDown() const { return shuttingDown.load(std::memory_order_acquire); }
    // P3.4 S8: callAsync that runs `fn` only while this bridge still exists (checked on the message thread,
    // where the bridge is destroyed, so the check cannot race the destruction).
    void postMessage(std::function<void()> fn);
    // Background work owned by the bridge (was std::thread(...).detach()); joined by shutdown().
    void runJob(std::function<void()> job);

    // Tests only (Tests/mcp/bridge_handler_tests.cpp): replies go to `sink` instead of a socket, and the bridge
    // counts as active so sendReply sends. Call stop() first; nothing is bound.
    void setReplySinkForTests(std::function<void(juce::OSCMessage const&)> sink);
    // Tests only: starts the v2 listener alone (no UDP), discovery file under the current HOME. False if it could
    // not start. stop() stops it.
    bool startV2ForTests();

    // Status for the GUI / console. Returns a short human-readable state:
    // "connected" | "listening" | "stopped" | "error: <detail>".
    juce::String getStatus() const;
    int getListenPort() const { return listenPort; }
    int getSendPort() const { return sendPort; }

    // Mark server activity (called on any inbound OSC message) so the
    // bridge can distinguish "listening" from "server connected".
    void noteServerActivity();

    // OSC Inbound Callbacks
    void oscMessageReceived(const juce::OSCMessage& message) override;
    void oscBundleReceived(const juce::OSCBundle& bundle) override;

    // Audio thread hook for zero-dropout passive signal metering
    void audioTick();
    ProbeManager& getProbeManager() { return probeManager; }

    // Phase 4 (PRD_MEASUREMENT_WINDOWS): entry point for the native armed
    // control receiver bound during `arm_control`. Runs while the Pd lock is
    // held by the dispatching thread, so no lock is taken here. No-op unless a
    // control window is armed.
    void mcpCtrlCapture(t_symbol* selector, int argc, t_atom* argv);

    // Native Telemetry Dispatch (Port 19010)
    void sendSelectionTelemetry(const juce::String& selector, const SmallArray<pd::Atom>& list);

    // PRD overlay: forward an artist-typed note (from an inline note editor) to
    // the MCP as /pd/ui/note <targetTempId> <text>.
    void sendArtistNote(const juce::String& targetId, const juce::String& text);
    void sendConsoleLog(const juce::String& message, bool isError);
    void sendPrompt(const juce::String& promptText);
    // PRD Copilot: prompt from the floating selection pill, carrying the selected
    // objects' stable tempIds (resolved in C++ — the identity source of truth) so
    // the MCP server never has to guess the selection from a stale mirror.
    void sendSelectionPrompt(const juce::String& promptText, const juce::StringArray& targetTempIds, bool queue = false);
    void sendSketchStroke(const juce::String& subpatch, const juce::String& jsonPoints, bool isFastPath = false);
    // Phase 1B/Moodboard Chisel: the artist hit [⚡ Build] on the sketch pill —
    // ships the full multi-stroke bundle + bbox (PD patch coords) + optional
    // prompt as /pd/sketch/build so the MCP server becomes the visual compiler.
    void sendSketchBuild(const juce::String& subpatch, const juce::String& jsonStrokes, const juce::String& jsonBBox, const juce::String& prompt);
    // PRD Phase 2: a reference image was pinned to the canvas glass —
    // /pd/reference/image <path> <x> <y> <w> <h> in PD patch coords.
    void sendReferenceImage(const juce::String& path, float x, float y, float w, float h);
    // PRD Copilot: the active creative Lens (Doctor / Jam Partner / Genesis /
    // Modular Tech). Sent to the MCP server, which prepends the lens brief to
    // subsequent agent turns. "free" clears it.
    void sendLens(const juce::String& lens);
    // AI Copilot Chat callback: role ("user", "ai", "thought", "error"), text
    void setChatCallback(std::function<void(const juce::String& role, const juce::String& text)> cb);
    void postChatMessage(const juce::String& role, const juce::String& text);
    // Voice & Beatbox Capture Engine. mode: 0 = auto-classify, 1 = melody
    // (artist declared "sing"), 2 = beat (artist declared "beatbox").
    // maxSeconds = safety ceiling only — the artist decides the real length by
    // tapping stop; a full buffer now stages the take instead of discarding it.
    void startVoiceCapture(int maxSeconds = 60, int mode = 0);
    void stopVoiceCaptureAndAnalyze();
    void analyzeCapturedVoice();
    // Voice take preview — out-of-patch audition through the main audio output.
    // Tapping a playing take's ▶ again stops it.
    void playVoiceTake(const juce::String& takeId, const juce::String& wavPath);
    void stopVoiceTake();
    juce::String getPlayingVoiceTakeId();
    bool isVoiceCapturing() const { return voiceCapturing.load(std::memory_order_relaxed); }
    float getVoiceLiveLevel() const { return voiceLiveLevel.load(std::memory_order_relaxed); }
    int getVoiceCaptureMode() const { return voiceCaptureMode.load(std::memory_order_relaxed); }
    void voiceInputTick(float const* inputSamples, int numSamples);
    void parseVoiceBufferToJson(float const* buffer, int totalSamples, double sampleRate, int forcedMode);

    // MIDI Melodic-Intent Capture. The artist plays a MIDI keyboard; we log exact
    // note-on/off events (no transcription) and stage a take with a voice binding.
    // maxSeconds = safety ceiling; the artist decides the real length by stopping.
    void startMidiCapture(int maxSeconds = 120, const juce::String& voiceBinding = "canvas");
    void stopMidiCaptureAndStage();
    void midiInputTick(const juce::MidiBuffer& midi, int numSamples, juce::AudioBuffer<float>* audioBuffer = nullptr, double sampleRate = 48000.0);
    void setMidiMonitorThrough(bool enabled) { monitorThroughEnabled.store(enabled); }
    bool isMidiMonitorThrough() const { return monitorThroughEnabled.load(); }
    bool isMidiCapturing() const { return midiCapturing.load(std::memory_order_relaxed); }
    int getMidiCaptureNoteCount() const;
    juce::String getMidiCaptureVoice() const { return midiCaptureVoice; }
    void playMidiTake(const juce::String& takeId, const juce::String& jsonStr);
    void stopMidiTake();
    bool isMidiPlaying() const { return midiPlaying.load(std::memory_order_relaxed); }
    juce::String getPlayingMidiTakeId() const { return playingMidiTakeId; }

    void sendReply(const juce::String& addressPattern, const juce::Array<juce::var>& args);
    void sendReply(const juce::String& addressPattern, float val);
    void sendReply(const juce::String& addressPattern, const juce::String& str);
    void sendRawReply(const juce::String& addressPattern);

    // Canvas & Table Helpers
    static juce::String normalizeCanvas(const juce::String& name);
    static juce::String canonicalCanvasKey(t_canvas* cnv);

    friend class ProbeManager;
    friend class McpHost; // replays undo/redo transactions through replayPd
    friend struct mcp::BatchJob; // batch_atomic stages (P3.4 S9) use the layout and diagnose helpers

private:
    // Verb table (MCPBridgeVerbs.inc): one private member function per row, built
    // into a command table at startup. A verb with a row is served from there;
    // every handler is declared from the table.
    using VerbHandler = void (MCPBridge::*)(const juce::String& action, const juce::OSCMessage& msg);
    using VerbTable = mcp::CommandTable<VerbHandler>;
    static VerbTable const& commandTable();
    bool dispatchRegistered(const juce::String& domain, const juce::String& pathVerb, const juce::OSCMessage& msg);
    // A message no row serves gets /bridge/unknown_verb [address, domain, verb] (capability unknown_verb_reply).
    void replyUnknownVerb(const juce::String& domain, const juce::String& pathVerb, const juce::OSCMessage& msg);
    // McpHost undo/redo replays a recorded /pd transaction (batch_atomic, move_batch_id) by its verb.
    void replayPd(const juce::String& action, const juce::OSCMessage& msg);
    // P3.4 S11: /pd/debug_stall_audio <ms> <corrId>, a table row only with PLUGDATA_MCP_AUDIT=1 (see commandTable).
    void pdDebugStallAudio(const juce::String& action, const juce::OSCMessage& msg);

#define MCP_VERB(domain, verb, handler) void handler(const juce::String& action, const juce::OSCMessage& msg);
#define MCP_DOMAIN(domain, handler) void handler(const juce::String& action, const juce::OSCMessage& msg);
#include "Pd/MCPBridgeVerbs.inc"

    void advanceSequencer(int blockSize);
    void enqueueSeqFire(bool isBang, t_symbol* sym, float value);
    void flushSeqFires();
    struct SeqFire {
        t_symbol* sym = nullptr;
        float value = 0.0f;
        bool isBang = true;
    };
    static constexpr int kMaxSeqFires = 256;
    std::array<SeqFire, kMaxSeqFires> seqPendingFires {}; // audio thread only
    int seqPendingCount = 0;                               // audio thread only
    std::atomic<uint32_t> seqFiresDropped { 0 };

    t_outconnect* resolveProbeTarget(const juce::String& canvasName, const juce::String& targetId, int outletIndex, juce::String& errorOut);
    bool activateProbing();

    static juce::String computeDiagnoseFacts(PluginProcessor* processor, t_canvas* cnv, const juce::String& canvasName);

    struct MasterMeterResult {
        float peakDb = -100.0f;
        float rmsDb = -100.0f;
        bool masterFound = false;
        juce::String masterType = "none";
    };

    static MasterMeterResult computeMasterMeter(PluginProcessor* processor, t_canvas* cnv, const juce::String& canvasName);
    static juce::String computeSignalTrace(PluginProcessor* processor, t_canvas* cnv, const juce::String& canvasName);

    // Context-aware layout (PRD_CONTEXT_LAYOUT_GUARD): connected components of
    // the wire graph, classified as signal / bus / control. Read-only, fast.
    static juce::String computeClusters(PluginProcessor* processor, t_canvas* cnv, const juce::String& canvasName);

    // PRD 1.2: CPU/DSP cost introspection. Live host load + a per-subpatch
    // cost ESTIMATE from a weighted census of DSP objects (read-only).
    static juce::String computePerfFacts(PluginProcessor* processor, t_canvas* cnv, const juce::String& canvasName);

    // Inline layout sanitize (PRD_CONTEXT_LAYOUT_GUARD P2/P3): minimal-deoverlap
    // of overlapping objects after a mutation, using the artist's preferred pad
    // and grid snap. Returns objects moved.
    static int sanitizeLayout(PluginProcessor* processor, t_canvas* cnv, int pad = 5, int snap = 10);

    // Inline occlusion fix (PRD_CONTEXT_LAYOUT_GUARD P3.1): nudge boxes that a
    // wire passes through, off the wire's path. Returns objects moved.
    static int fixOcclusions(PluginProcessor* processor, t_canvas* cnv, int pad = 5, int snap = 10);

    // P4 compose (PRD_CONTEXT_LAYOUT_GUARD): arrange clusters into role-based
    // zones — bus/mix at the edge, synth clusters as columns, gui/control in a
    // gutter. On-demand layout (like dagre/flow), not a per-mutation guard.
    // Returns objects moved.
    static int composeLayout(PluginProcessor* processor, t_canvas* cnv, const juce::String& canvasName, int pad = 5, int snap = 10);

    void timerCallback() override;

    struct MorphParam {
        juce::String name;
        float from = 0.0f;
        float to = 0.0f;
    };

    struct MorphJob {
        juce::String correlationId;
        int totalSteps = 20;
        int currentStep = 0;
        int intervalMs = 50;
        std::vector<MorphParam> params;
    };

    // Native sample-accurate transport clock. Advanced on the audio thread via
    // audioTick() by a running sample counter; bar/beat/step and the next downbeat
    // are derived from that counter (not Date.now()), so they are sample-accurate.
    struct TransportState {
        std::atomic<bool> running { false };
        std::atomic<float> bpm { 120.0f };
        std::atomic<int> subdivision { 4 };   // steps per beat
        std::atomic<int> beatsPerBar { 4 };
        std::atomic<bool> modeClock { false }; // clock vs free (Phase 2 consumes it)
        std::atomic<int64_t> sampleCounter { 0 };
        std::atomic<int64_t> anchorSample { 0 }; // sampleCounter at last tempo anchor
        std::atomic<double> anchorBeat { 0.0 };  // beat position at last tempo anchor
    };

    // One track of a native sequencer job. `steps` holds per-step values:
    // bang track: 0 = rest, 1/number = hit (velocity); param track: raw float.
    struct SeqTrack {
        juce::String name;
        t_symbol* sym = nullptr; // P3.4 S6: resolved by seqStart under the Pd lock, never on the audio thread
        bool isBang = true;
        std::vector<float> steps;
    };

    // Native sample-accurate drum/note sequencer. Scheduling state is advanced in
    // audioTick() (after a Pd sub-block); the step's fires are sent right there,
    // before the next performDSP, as the old functionQueue route did. P3.4 S6: no
    // blocking lock and no allocation on the audio thread: the job list is read
    // under a try-lock (busy: the steps fire one sub-block later), fires collect in
    // a fixed array and go out under a try-locked Pd lock (busy: next sub-block).
    struct SeqJob {
        juce::String jobId;
        std::vector<SeqTrack> tracks;
        float bpm = 120.0f;
        int subdivision = 4;
        float swing = 0.0f;
        bool syncToClock = true;
        float drift = 0.0f;
        int patternLength = 0;
        int stepIndex = 0;
        int64_t nextStepSample = 0;
        int64_t jobSampleCounter = 0;
        bool active = false;  // guarded by seqLock
    };

    PluginProcessor* processor = nullptr;
    int listenPort = 9000;
    int sendPort = 19010;

    // Bound to 127.0.0.1 only (Report F6): OSCReceiver::connect(port) would
    // listen on every interface. Must outlive the receiver's connection.
    std::unique_ptr<juce::DatagramSocket> listenSocket;
    juce::OSCReceiver receiver;
    // P4.1 S2: the v2 listener (loopback TCP, frames, token handshake). Started by start() even when the UDP port is
    // taken (v2 has its own port and token); stopped first in stop(), before the command worker. S3: the router
    // decides, for every message `sender` sends, whether a v2 session gets it. Both are declared before `sender`
    // so they outlive its thread.
    mcp::v2::Server v2Server;
    mcp::v2::Router v2Router { v2Server };
    ReplyQueue sender;
    // Current reply target, so /bridge/connect only reconnects on a change.
    juce::String senderHost { "127.0.0.1" };
    int senderPort = 0;
    std::atomic<bool> active { false };

    std::atomic<juce::int64> lastServerActivity { 0 };
    mutable bool reportedConnected = false;
    mutable bool reportedLost = false;
    juce::String statusMessage;
    // Unique per-process boot token. The MCP server compares it across
    // capability handshakes to detect a PlugData restart and clear its stale
    // identity cache (tempId -> index mappings from the previous session).
    juce::String bootToken;
    bool startV2();
    void stopV2();
    // The v1 capability names (the capabilities reply without its version and boot entries); the v2 welcome sends
    // the same list. `protocol_v2` is in it only while this bridge's v2 listener runs.
    std::vector<std::string> capabilityList() const;
    // P4.1 S3: a v2 request from a V2Read thread: admitted (posted to the worker with its origin) or refused.
    std::optional<mcp::v2::RequestAnswer> admitV2(int session, mcp::v2::Request request);
    // Replies may be sent: the UDP socket is ours, or a v2 listener runs (UDP output is then dropped by `sender`).
    bool canReply() const { return active.load() || v2Server.running(); }

    std::vector<MorphJob> morphJobs;
    juce::CriticalSection morphLock;

    TransportState transport;

    std::vector<SeqJob> seqJobs;
    juce::CriticalSection seqLock;

    ProbeManager probeManager;

    // Monitor tap (solo/mute): the target is re-resolved on the timer so a
    // deleted/rewired node can never leave a dangling s_vec on the audio thread.
    juce::String mcpTapCanvas;
    juce::String mcpTapTempId;
    int mcpTapOutlet { 0 };

    // Phase 4 armed control-message window (native Pd receiver + atom buffer).
    std::unique_ptr<McpControlWindow> controlWindow;

    // Voice & Beatbox capture state
    static constexpr int MAX_VOICE_CAPTURE_SAMPLES = 48000 * 6; // 6 seconds at 48k
    std::vector<float> voiceCaptureBuffer;
    std::atomic<bool> voiceCapturing { false };
    std::atomic<int> voiceCaptureWritePos { 0 };
    std::atomic<float> voiceLiveLevel { 0.0f };
    std::atomic<int> voiceCaptureMode { 0 }; // 0 = auto, 1 = melody, 2 = beat

    // MIDI capture state (audio thread logs; message thread stages)
    struct MidiNoteEvent {
        int pitch = 0;
        int velocity = 0;
        int channel = 1;
        juce::int64 sampleTime = 0; // absolute samples since capture start
        double durationSec = 0.0;   // filled on note-off
        bool closed = false;
    };
    // Option B: Auto-Monitor-Through synth voice bank (warm EPiano tone for blank canvas)
    struct MonitorVoice {
        int pitch = -1;
        float velocity = 0.0f;
        float phase = 0.0f;
        float phaseInc = 0.0f;
        float env = 0.0f;
        bool active = false;
        bool released = false;
    };
    static constexpr int kMaxMonitorVoices = 16;
    MonitorVoice monitorVoices[kMaxMonitorVoices];
    std::atomic<bool> monitorThroughEnabled { true };

    std::vector<MidiNoteEvent> midiCaptureEvents;
    juce::CriticalSection midiCaptureLock;
    // P3.4 S6: the audio thread only queues raw note on/off here (preallocated: it never allocates or blocks);
    // drainMidiCaptureRing() pairs them into midiCaptureEvents, under midiCaptureLock, off the audio thread.
    struct MidiRawEvent {
        bool on = true;
        int pitch = 0;
        int velocity = 0;
        int channel = 1;
        juce::int64 sampleTime = 0;
    };
    moodycamel::ReaderWriterQueue<MidiRawEvent> midiCaptureRing { 4096 };
    std::atomic<uint32_t> midiCaptureDropped { 0 };
    void drainMidiCaptureRing(); // caller holds midiCaptureLock
    // Raised by the audio thread instead of a callAsync there; the bridge timer acts on them.
    std::atomic<bool> midiCaptureAutoStopPending { false };
    std::atomic<bool> voiceAnalyzePending { false };
    std::atomic<bool> midiCapturing { false };
    std::atomic<juce::int64> midiCaptureSamples { 0 }; // running sample clock
    std::atomic<int> midiCaptureMaxSamples { 0 };
    juce::String midiCaptureVoice { "canvas" };
    int midiCaptureStartBpm = 0;

    // MIDI playback state (auditioning staged takes on canvas)
    struct MidiPlaybackNote {
        int pitch = 0;
        float velocity = 0.8f;
        juce::int64 startSample = 0;
        juce::int64 endSample = 0;
    };
    std::vector<MidiPlaybackNote> midiPlaybackNotes;
    std::atomic<bool> midiPlaying { false };
    std::atomic<juce::int64> midiPlaybackPos { 0 };
    std::atomic<juce::int64> midiPlaybackEndSample { 0 };
    juce::String playingMidiTakeId;
    juce::CriticalSection midiPlaybackLock;
    // Preview transport state (message thread; audio callback is the player)
    std::unique_ptr<juce::AudioFormatManager> previewFormatManager;
    std::unique_ptr<juce::AudioTransportSource> previewTransport;
    std::unique_ptr<juce::AudioSourcePlayer> previewPlayer;
    juce::CriticalSection previewLock;
    juce::String previewTakeId;

    std::function<void(const juce::String&, const juce::String&)> chatCallback;
    juce::CriticalSection chatLock;

    std::atomic<bool> shuttingDown { false };
    juce::ThreadPool jobPool { 2 }; // P3.4 S8: offline render, voice analysis

    // Plan 3 P3.4 S1: the network thread only posts; every command runs here, one at a time, in arrival
    // order (the order the network thread used to run them in). Declared last: destroyed first.
    // P4.1 S3: a v1 datagram (no origin) or a v2 request (its origin), in one queue: arrival order across both.
    struct Command {
        juce::OSCMessage message;
        mcp::v2::WorkPtr work; // null for a v1 datagram
    };
    // A request's work token reports "done" through this; the bridge's destructor turns it off, so a token released
    // later (a posted lambda dropped after the bridge went) does nothing.
    struct V2DoneGate {
        std::mutex lock;
        ReplyQueue* queue = nullptr;
        void done(mcp::v2::OriginPtr const& origin)
        {
            std::lock_guard<std::mutex> const l(lock);
            if (queue)
                queue->handlerDone(origin);
        }
    };
    std::shared_ptr<V2DoneGate> v2Done = std::make_shared<V2DoneGate>();
    mcp::SerialWorker<Command> commandWorker;
    void runCommand(Command command);
    void handleMessage(const juce::OSCMessage& message);
    // P3.4 S3: handlers that create, delete or rewire Pd objects run in the Pd context (the message thread
    // under sys_lock), where plugdata's GUI expects every structural edit. Called first in such a handler:
    // true = it has been run there now (or could not be: logged), the caller returns; false = already there.
    using CommandHandler = void (MCPBridge::*)(const juce::String&, const juce::OSCMessage&);
    bool runInPdContext(CommandHandler handler, const juce::String& action, const juce::OSCMessage& msg);
    void startCommandWorker();

    JUCE_DECLARE_WEAK_REFERENCEABLE(MCPBridge)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MCPBridge)
};

