/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Bridge commands: transport clock, sequencer, morph and the bridge handshake (Plan 3 P3.3): handlers moved unchanged out of the handle<Domain>Domain functions.

#include "MCPBridgeCmdCommon.h"

// ─── Native sequencer helpers ────────────────────────────────────────────

static bool isPureDrumName(const juce::String& name)
{
    auto const lower = name.toLowerCase();
    static const char* const names[] = {
        "kick", "bd", "snare", "sd", "hh", "hat", "hihat", "openhat", "closedhat",
        "clap", "cp", "tom", "rim", "shaker", "cowbell", "cymbal", "crash", "ride", "perc"
    };
    for (auto const* nm : names) if (lower == nm) return true;
    return false;
}

static bool isBangTrack(const juce::String& name)
{
    if (!isPureDrumName(name)) return false;
    if (name.contains("_") || name.containsIgnoreCase("gate") || name.containsIgnoreCase("freq")
        || name.containsIgnoreCase("pitch") || name.containsIgnoreCase("cut")) return false;
    return true;
}

// Sample-accurate transport clock. Position derives from a running sample
// counter (advanced in audioTick()), so bar/beat/step and the next downbeat
// are exact — no Date.now()/wall-clock drift.
void MCPBridge::transportSetBpm(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 1) {
        double bpm = getArgFloat(msg[0]);
        if (bpm < 1.0) bpm = 1.0;
        if (bpm > 400.0) bpm = 400.0;
        double const sampleRate = (processor && processor->getSampleRate() > 0.0) ? processor->getSampleRate() : 44100.0;
        double const oldBpm = transport.bpm.load(std::memory_order_relaxed);
        double const samplesPerBeat = sampleRate * 60.0 / oldBpm;
        int64_t const counter = transport.sampleCounter.load(std::memory_order_relaxed);
        int64_t const anchorSample = transport.anchorSample.load(std::memory_order_relaxed);
        double const anchorBeat = transport.anchorBeat.load(std::memory_order_relaxed);
        // Anchor current position so a tempo change preserves the beat position.
        double const curBeat = samplesPerBeat > 0.0 ? anchorBeat + (double)(counter - anchorSample) / samplesPerBeat : anchorBeat;
        transport.anchorBeat.store(curBeat, std::memory_order_relaxed);
        transport.anchorSample.store(counter, std::memory_order_relaxed);
        transport.bpm.store((float)bpm, std::memory_order_relaxed);
    }
    return;
}

void MCPBridge::transportSetSubdivision(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 1) {
        int sub = (int)getArgFloat(msg[0]);
        if (sub < 1) sub = 1;
        if (sub > 64) sub = 64;
        transport.subdivision.store(sub, std::memory_order_relaxed);
    }
    return;
}

void MCPBridge::transportSetMode(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 1) {
        juce::String const mode = getArgString(msg[0]);
        transport.modeClock.store(mode == "clock", std::memory_order_relaxed);
    }
    return;
}

void MCPBridge::transportStart(const juce::String& action, const juce::OSCMessage& msg)
{
    transport.running.store(true, std::memory_order_relaxed);
    return;
}

void MCPBridge::transportPause(const juce::String& action, const juce::OSCMessage& msg)
{
    transport.running.store(false, std::memory_order_relaxed);
    return;
}

void MCPBridge::transportStatus(const juce::String& action, const juce::OSCMessage& msg)
{
    auto const correlationId = msg.size() > 0 ? getArgString(msg[0]) : "0";
    double const sampleRate = (processor && processor->getSampleRate() > 0.0) ? processor->getSampleRate() : 44100.0;
    double const bpm = transport.bpm.load(std::memory_order_relaxed);
    int const subdivision = transport.subdivision.load(std::memory_order_relaxed);
    int const beatsPerBar = transport.beatsPerBar.load(std::memory_order_relaxed);
    bool const running = transport.running.load(std::memory_order_relaxed);
    bool const modeClock = transport.modeClock.load(std::memory_order_relaxed);
    int64_t const counter = transport.sampleCounter.load(std::memory_order_relaxed);
    int64_t const anchorSample = transport.anchorSample.load(std::memory_order_relaxed);
    double const anchorBeat = transport.anchorBeat.load(std::memory_order_relaxed);

    double const samplesPerBeat = sampleRate * 60.0 / bpm;
    double const totalBeats = samplesPerBeat > 0.0 ? anchorBeat + (double)(counter - anchorSample) / samplesPerBeat : anchorBeat;
    double const barD = std::floor(totalBeats / beatsPerBar);
    int const bar = (int)barD;
    int const beatInBar = (int)std::floor(totalBeats) - bar * beatsPerBar;
    int const stepInBeat = (int)std::floor((totalBeats - std::floor(totalBeats)) * subdivision);
    int const stepsPerBar = beatsPerBar * subdivision;
    int const stepInBar = ((int)std::floor(totalBeats * subdivision)) % stepsPerBar;

    double const nextBarBeat = (barD + 1.0) * beatsPerBar;
    int64_t const samplesToNextBar = (int64_t)((nextBarBeat - totalBeats) * samplesPerBeat);

    juce::OSCMessage reply { juce::OSCAddressPattern("/transport/status/" + correlationId) };
    reply.addArgument((float)bpm);
    reply.addArgument(subdivision);
    reply.addArgument(beatsPerBar);
    reply.addArgument(bar);
    reply.addArgument(beatInBar);
    reply.addArgument(stepInBeat);
    reply.addArgument(stepInBar);
    reply.addArgument((float)samplesToNextBar);
    reply.addArgument((float)sampleRate);
    reply.addArgument(running ? 1 : 0);
    reply.addArgument(modeClock ? juce::String("clock") : juce::String("free"));
    sender.send(reply);
    return;
}

    // /seq/start <jobId> <corrId> <json>
    // /seq/stop  <jobId>
void MCPBridge::seqStart(const juce::String& action, const juce::OSCMessage& msg)
{
    auto const jobId = msg.size() > 0 ? getArgString(msg[0]) : juce::String();
    auto const correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";
    auto const jsonStr = msg.size() > 2 ? getArgString(msg[2]) : juce::String();
    juce::String const replyAddr = "/seq/start/reply/" + correlationId;

    if (jobId.isEmpty() || jsonStr.isEmpty()) { sendReply(replyAddr, 0.0f); return; }

    juce::var const parsed = juce::JSON::parse(jsonStr);
    if (parsed.isVoid()) { sendReply(replyAddr, 0.0f); return; }

    SeqJob job;
    job.jobId = jobId;
    job.bpm = varToFloat(parsed.getProperty("bpm", 120.0), 120.0f);
    job.subdivision = varToInt(parsed.getProperty("subdivision", 4.0), 4);
    job.swing = varToFloat(parsed.getProperty("swing", 0.0), 0.0f);
    job.syncToClock = varToBool(parsed.getProperty("syncToClock", true), true);
    job.drift = varToFloat(parsed.getProperty("drift", 0.0), 0.0f);
    job.nextStepSample = -1;  // sentinel: anchor to current position on first tick
    job.jobSampleCounter = 0;
    job.stepIndex = 0;
    job.active = true;

    if (auto* tracksObj = parsed.getProperty("tracks", juce::var()).getDynamicObject()) {
        int maxLen = 0;
        for (auto const& kv : tracksObj->getProperties()) {
            juce::String const name = kv.name.toString();
            SeqTrack track;
            track.name = name;
            track.isBang = isBangTrack(name);
            if (auto* arr = kv.value.getArray()) {
                for (auto const& stepVar : *arr) {
                    if (stepVar.isBool()) track.steps.push_back(stepVar.toString().equalsIgnoreCase("true") ? 1.0f : 0.0f);
                    else track.steps.push_back(varToFloat(stepVar, 0.0f));
                }
            }
            maxLen = std::max(maxLen, (int)track.steps.size());
            job.tracks.push_back(track);
        }
        job.patternLength = maxLen;
    }

    if (job.patternLength <= 0 || job.tracks.empty()) { sendReply(replyAddr, 0.0f); return; }

    // P3.4 S6: the receive symbols are made here, under the Pd lock (gensym is not thread-safe), so the audio
    // thread only follows pointers.
    if (!processor) { sendReply(replyAddr, 0.0f); return; }
    processor->setThis();
    sys_lock();
    for (auto& track : job.tracks)
        track.sym = gensym(track.name.toRawUTF8());
    sys_unlock();

    {
        juce::ScopedLock sl(seqLock);
        bool replaced = false;
        for (auto& existing : seqJobs) {
            if (existing.jobId == jobId) {
                existing.active = false;
                existing = job;
                replaced = true;
                break;
            }
        }
        if (!replaced)
            seqJobs.push_back(job);
    }
    sendReply(replyAddr, 1.0f); // P3.4 S6: after the lock (it was sent while holding seqLock)
    return;
}

void MCPBridge::seqStop(const juce::String& action, const juce::OSCMessage& msg)
{
    auto const jobId = msg.size() > 0 ? getArgString(msg[0]) : juce::String();
    juce::ScopedLock sl(seqLock);
    for (auto& job : seqJobs)
        if (job.jobId == jobId || jobId.isEmpty())
            job.active = false;
    if (jobId.isEmpty()) seqJobs.clear();
    return;
}

void MCPBridge::handleBridgeDomain(const juce::String& bridgeAction, const juce::OSCMessage& msg)
{
    if (bridgeAction == "connect") {
        if (msg.size() >= 2) {
            auto ip = getArgString(msg[0]);
            int port = static_cast<int>(getArgFloat(msg[1]));
            // Report F6: never redirect replies/telemetry off this machine.
            if (!mcp::safety::isLoopbackHost(ip.toStdString())) {
                if (processor)
                    processor->logError("MCP bridge: refused /bridge/connect to non-loopback host " + ip);
                return;
            }
            auto const host = (ip.isEmpty() || ip == "localhost" || ip == "::1") ? juce::String("127.0.0.1") : ip;
            if (port > 0 && port < 65536 && (host != senderHost || port != senderPort)) {
                sender.disconnect();
                sender.connect(host, port);
                senderHost = host;
                senderPort = port;
            }
        }
        sendReply("/bridge/connect/ack", "127.0.0.1");
    } else if (bridgeAction == "capabilities") {
        juce::OSCMessage reply { juce::OSCAddressPattern("/bridge/capabilities/reply") };
        reply.addArgument(juce::String("11.18"));
        for (auto const& cap : capabilityList())
            reply.addArgument(juce::String(cap));
        reply.addArgument(juce::String("boot:" + bootToken));
        sender.send(reply);
    }
}

std::vector<std::string> MCPBridge::capabilityList() const
{
    std::vector<std::string> caps {
        "create_batch",
        "delete_batch",
        "connect_batch",
        "disconnect_batch",
        "ping",
        "clear",
        "dump",
        "load",
        "param",
        "param_symbol",
        "param_list",
        "obj_set",
        "obj_set_batch",
        "obj_get",
        "obj_get_batch",
        "param_get",
        "params",
        "param_set_name",
        // Rewrite a text object's label without recreating it (live set + truthful number)
        "obj_relabel",
        "trigger",
        "telemetry",
        "array_io",
        "morph",
        "array_stats",
        "get_mappings",
        "move_batch_id",
        "meter",
        "meter_query",
        "meter_master",
        "meter_arm",
        "meter_control",
        "meter_spectral_arm",
        "meter_trace",
        // T2b: /meter/trace ... arm <windowMs> [focus] / read: the trace of a window's peaks (a triggered voice)
        "meter_trace_armed",
        // T2b: /pd/error carries the tempId of the object that raised the line, as a second argument
        "console_object",
        "array_bulk",
        "census",
        "typeof",
        "ports",
        "identity_snapshot",
        "identity_version",
        // PRD Phase 2 §2.6: the bridge mints readable tempIds natively
        // (census auto-adopt). TS is a read-only mirror on bridges that
        // advertise this; legacy bridges keep the TS adoption path.
        "auto_adopt",
        // PRD Phase 3 §2.6: C++-native save/load with identity sidecar —
        // tempIds travel with the file and are re-registered by the engine.
        "save_patch",
        "load_patch",
        // PRD Phase 3 §2.5: multi-tab — open in new tab + enumerate tabs
        "open_patch",
        "list_tabs",
        "focus_tab",
        "close_tab",
        // PRD diagnostic layer: the graph X-ray
        "diagnose",
        // PRD 1.2: CPU/DSP cost introspection (live load + weighted census)
        "perf",
        // PRD GOP Module v2 Phase A: atomic parent-canvas swap for TS-generated
        // MERDA abstractions (single delete+create+rewire under one DSP update)
        "encapsulate_gop",
        // PRD GOP Module v2 Phase B: font measurement for pixel-perfect GOP layout
        "measure_text",
        // PRD layout-v2 Phase A: read-only layout facts (C++ truth of layout)
        "bounds",
        "collisions",
        "clusters",
        "wire_occlusions",
        // PRD layout-v2 Phase B1/B2: native writers on the zero-drop move path
        "deoverlap",
        "layout_guard",
        "compose",
        "flow",
        // PRD Multimodal Studio: the moodboard rail — one query returns the whole
        // multimodal brief (images + ink + notes + voice takes + selection).
        "moodboard",
        "pillars",
        "connections",
        "zoom_to_fit",
        "spectral",
        "batch_atomic",
        "batch-facts",
        "batch-dedup",
        "preflight-guards",
        "transport",
        "seq",
        // Canvas screenshot 2192 LLM vision: render Canvas to PNG temp file, MCP returns ImageContent
        "screenshot_canvas",
        // PRD 1.4: native GEM render-window capture (no patch objects)
        "gem_capture",
        // PRD 1.5: programmatic GUI selection by tempId (highlight)
        "select",
        // PRD overlay: per-object AI state markers drawn on the canvas
        // C++ Ground Truth Documentation & Native Dialogs
        "doc_get",
        "doc_search",
        "doc_dump",
        "library_info",
        "ui_open_reference",
        "ui_open_help",
        "viewport",
        "ai_overlay",
        // PRD overlay: ghost/preview of proposed (uncommitted) changes
        "ai_ghost",
        // PRD overlay: short in-place AI annotations (explain on the patch)
        "ai_annotate",
        // PRD overlay: titled regions grouping objects (structure at a glance)
        "ai_region",
        // Cinematic Viewport HUD & Subtitle overlay for video/tutorial capture
        "ai_hud",
        // Native & AI overlay control: query and set overlay flags from MCP
        "overlays_control",
        // Labeled screenshots: GUI-widget name chips painted on the bitmap only
        "screenshot_labels",
        // Offline faster-than-realtime render to WAV (background DSP bake)
        "render",
        // An address the bridge has no verb for is answered with /bridge/unknown_verb [address, domain, verb]
        "unknown_verb_reply",
        // P3.5: per-canvas identity_version, snapshot canvasKey/canvasVersion, rename_id error, sidecar v2.
        "identity_v2",
        // P5.1 sink floor S1: /pd/limiter <corrId> answers whether PlugData's master limiter runs (read-only)
        "limiter",
    };
    // P4.1 S2: the v2 listener is up (TCP 127.0.0.1, port and token in the discovery file).
    if (v2Server.running())
        caps.push_back("protocol_v2");
    return caps;
}

void MCPBridge::handleMorphDomain(const juce::String& morphAction, const juce::OSCMessage& msg)
{
    if (morphAction == "run" && msg.size() >= 5) {
        auto correlationId = getArgString(msg[0]);
        int durationMs = static_cast<int>(getArgFloat(msg[1]));
        int steps = static_cast<int>(getArgFloat(msg[2]));
        int paramCount = static_cast<int>(getArgFloat(msg[3]));

        if (steps <= 0) steps = 20;
        int interval = std::max(1, durationMs / steps);

        MorphJob job;
        job.correlationId = correlationId;
        job.totalSteps = steps;
        job.currentStep = 0;
        job.intervalMs = interval;

        int cursor = 4;
        for (int i = 0; i < paramCount && cursor + 2 < msg.size(); i++) {
            MorphParam p;
            p.name = getArgString(msg[cursor++]);
            p.from = getArgFloat(msg[cursor++]);
            p.to = getArgFloat(msg[cursor++]);
            job.params.push_back(p);
        }

        {
            const juce::ScopedLock sl(morphLock);
            morphJobs.push_back(job);
        }

        if (!isTimerRunning()) {
            startTimer(15);
        }
    }
}
