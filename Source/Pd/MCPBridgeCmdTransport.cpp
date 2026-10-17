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

    {
        juce::ScopedLock sl(seqLock);
        for (auto& existing : seqJobs) {
            if (existing.jobId == jobId) {
                existing.active = false;
                existing = job;
                sendReply(replyAddr, 1.0f);
                return;
            }
        }
        seqJobs.push_back(job);
    }
    sendReply(replyAddr, 1.0f);
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
        auto correlationId = msg.size() > 0 ? getArgString(msg[0]) : "0";
        juce::OSCMessage reply { juce::OSCAddressPattern("/bridge/capabilities/reply") };
        reply.addArgument(juce::String("11.18"));
        reply.addArgument(juce::String("create_batch"));
        reply.addArgument(juce::String("delete_batch"));
        reply.addArgument(juce::String("connect_batch"));
        reply.addArgument(juce::String("disconnect_batch"));
        reply.addArgument(juce::String("ping"));
        reply.addArgument(juce::String("clear"));
        reply.addArgument(juce::String("dump"));
        reply.addArgument(juce::String("load"));
        reply.addArgument(juce::String("param"));
        reply.addArgument(juce::String("param_symbol"));
        reply.addArgument(juce::String("param_list"));
        reply.addArgument(juce::String("obj_set"));
        reply.addArgument(juce::String("obj_set_batch"));
        reply.addArgument(juce::String("obj_get"));
        reply.addArgument(juce::String("obj_get_batch"));
        reply.addArgument(juce::String("param_get"));
        reply.addArgument(juce::String("params"));
        reply.addArgument(juce::String("param_set_name"));
        // Rewrite a text object's label without recreating it (live set + truthful number)
        reply.addArgument(juce::String("obj_relabel"));
        reply.addArgument(juce::String("trigger"));
        reply.addArgument(juce::String("telemetry"));
        reply.addArgument(juce::String("array_io"));
        reply.addArgument(juce::String("morph"));
        reply.addArgument(juce::String("array_stats"));
        reply.addArgument(juce::String("get_mappings"));
        reply.addArgument(juce::String("move_batch_id"));
        reply.addArgument(juce::String("connect_batch_id"));
        reply.addArgument(juce::String("disconnect_batch_id"));
        reply.addArgument(juce::String("create_batch_id"));
        reply.addArgument(juce::String("delete_batch_id"));
        reply.addArgument(juce::String("meter"));
        reply.addArgument(juce::String("meter_query"));
        reply.addArgument(juce::String("meter_master"));
        reply.addArgument(juce::String("meter_arm"));
        reply.addArgument(juce::String("meter_control"));
        reply.addArgument(juce::String("meter_spectral_arm"));
        reply.addArgument(juce::String("meter_trace"));
        reply.addArgument(juce::String("array_bulk"));
        reply.addArgument(juce::String("census"));
        reply.addArgument(juce::String("typeof"));
        reply.addArgument(juce::String("ports"));
        reply.addArgument(juce::String("identity_snapshot"));
        reply.addArgument(juce::String("identity_version"));
        // PRD Phase 2 §2.6: the bridge mints readable tempIds natively
        // (census auto-adopt). TS is a read-only mirror on bridges that
        // advertise this; legacy bridges keep the TS adoption path.
        reply.addArgument(juce::String("auto_adopt"));
        // PRD Phase 3 §2.6: C++-native save/load with identity sidecar —
        // tempIds travel with the file and are re-registered by the engine.
        reply.addArgument(juce::String("save_patch"));
        reply.addArgument(juce::String("load_patch"));
        // PRD Phase 3 §2.5: multi-tab — open in new tab + enumerate tabs
        reply.addArgument(juce::String("open_patch"));
        reply.addArgument(juce::String("list_tabs"));
        reply.addArgument(juce::String("focus_tab"));
        reply.addArgument(juce::String("close_tab"));
        // PRD diagnostic layer: the graph X-ray
        reply.addArgument(juce::String("diagnose"));
        // PRD 1.2: CPU/DSP cost introspection (live load + weighted census)
        reply.addArgument(juce::String("perf"));
        // PRD GOP Module v2 Phase A: atomic parent-canvas swap for TS-generated
        // MERDA abstractions (single delete+create+rewire under one DSP update)
        reply.addArgument(juce::String("encapsulate_gop"));
        // PRD GOP Module v2 Phase B: font measurement for pixel-perfect GOP layout
        reply.addArgument(juce::String("measure_text"));
        // PRD layout-v2 Phase A: read-only layout facts (C++ truth of layout)
        reply.addArgument(juce::String("bounds"));
        reply.addArgument(juce::String("collisions"));
        reply.addArgument(juce::String("clusters"));
        reply.addArgument(juce::String("wire_occlusions"));
        // PRD layout-v2 Phase B1/B2: native writers on the zero-drop move path
        reply.addArgument(juce::String("deoverlap"));
        reply.addArgument(juce::String("layout_guard"));
        reply.addArgument(juce::String("compose"));
        reply.addArgument(juce::String("flow"));
        // PRD Multimodal Studio: the moodboard rail — one query returns the whole
        // multimodal brief (images + ink + notes + voice takes + selection).
        reply.addArgument(juce::String("moodboard"));
        reply.addArgument(juce::String("pillars"));
        reply.addArgument(juce::String("connections"));
        reply.addArgument(juce::String("zoom_to_fit"));
        reply.addArgument(juce::String("spectral"));
        reply.addArgument(juce::String("batch_atomic"));
        reply.addArgument(juce::String("batch-facts"));
        reply.addArgument(juce::String("batch-dedup"));
        reply.addArgument(juce::String("preflight-guards"));
        reply.addArgument(juce::String("transport"));
        reply.addArgument(juce::String("seq"));
        // Canvas screenshot 2192 LLM vision: render Canvas to PNG temp file, MCP returns ImageContent
        reply.addArgument(juce::String("screenshot_canvas"));
        // PRD 1.4: native GEM render-window capture (no patch objects)
        reply.addArgument(juce::String("gem_capture"));
        // PRD 1.5: programmatic GUI selection by tempId (highlight)
        reply.addArgument(juce::String("select"));
        // PRD overlay: per-object AI state markers drawn on the canvas
        // C++ Ground Truth Documentation & Native Dialogs
        reply.addArgument(juce::String("doc_get"));
        reply.addArgument(juce::String("doc_search"));
        reply.addArgument(juce::String("doc_dump"));
        reply.addArgument(juce::String("library_info"));
        reply.addArgument(juce::String("ui_open_reference"));
        reply.addArgument(juce::String("ui_open_help"));
        reply.addArgument(juce::String("viewport"));
        reply.addArgument(juce::String("ai_overlay"));
        // PRD overlay: ghost/preview of proposed (uncommitted) changes
        reply.addArgument(juce::String("ai_ghost"));
        // PRD overlay: short in-place AI annotations (explain on the patch)
        reply.addArgument(juce::String("ai_annotate"));
        // PRD overlay: titled regions grouping objects (structure at a glance)
        reply.addArgument(juce::String("ai_region"));
        // Cinematic Viewport HUD & Subtitle overlay for video/tutorial capture
        reply.addArgument(juce::String("ai_hud"));
        // Native & AI overlay control: query and set overlay flags from MCP
        reply.addArgument(juce::String("overlays_control"));
        // Labeled screenshots: GUI-widget name chips painted on the bitmap only
        reply.addArgument(juce::String("screenshot_labels"));
        // Offline faster-than-realtime render to WAV (background DSP bake)
        reply.addArgument(juce::String("render"));
        reply.addArgument(juce::String("boot:" + bootToken));
        sender.send(reply);
    }
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
