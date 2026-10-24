/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Bridge commands: meters, probes and armed captures (Plan 3 P3.3): handlers moved unchanged out of MCPBridge::handleMeterDomain.

#include "MCPBridgeCmdCommon.h"
#include "MCPBridgeTraceTaps.h"

#include <limits>


void MCPBridge::meterStart(const juce::String& meterAction, const juce::OSCMessage& msg)
{
    if (!processor) return;
    if (msg.size() < 2) return;
    auto canvasName = normalizeCanvas(getArgString(msg[0]));
    auto tempId = getArgString(msg[1]);
    int outletIndex = 0;
    int durationMs = 500;
    juce::String correlationId = "0";

    if (msg.size() == 3) {
        correlationId = getArgString(msg[2]);
    } else if (msg.size() == 4) {
        outletIndex = static_cast<int>(getArgFloat(msg[2]));
        correlationId = getArgString(msg[3]);
    } else if (msg.size() >= 5) {
        outletIndex = static_cast<int>(getArgFloat(msg[2]));
        durationMs = static_cast<int>(getArgFloat(msg[3]));
        correlationId = getArgString(msg[4]);
    }

    activateProbing();

    sys_lock();
    juce::String errorOut;
    t_outconnect* oc = resolveProbeTarget(canvasName, tempId, outletIndex, errorOut);
    int const epoch = ugen_getsortno(); // P3.4 S5: read with the resolve, under the same lock
    sys_unlock();

    if (!oc) {
        sendReply("/meter/error/" + correlationId, errorOut);
        return;
    }

    int probeId = probeManager.startProbe(oc, epoch, canvasName, tempId, outletIndex, correlationId, durationMs);
    if (probeId <= 0) {
        sendReply("/meter/error/" + correlationId, "Max active probes exceeded (limit: " + juce::String(MAX_PROBES) + ")");
        return;
    }

    if (!isTimerRunning()) {
        startTimer(15);
    }

    juce::OSCMessage rep { juce::OSCAddressPattern("/meter/start/reply/" + correlationId) };
    rep.addArgument(static_cast<int32>(probeId));
    sender.send(rep);
    return;
}

void MCPBridge::meterStop(const juce::String& meterAction, const juce::OSCMessage& msg)
{
    if (!processor) return;
    if (msg.size() < 1) return;
    uint32_t probeId = static_cast<uint32_t>(getArgFloat(msg[0]));
    juce::String correlationId = msg.size() > 1 ? getArgString(msg[1]) : "";
    // P3.4 S5: on the message thread, where the timer collects results: one consumer per result queue
    // (TSan in S0: try_dequeue raced between the command thread and the timer).
    postMessage([this, probeId, correlationId] { probeManager.stopProbe(probeId, correlationId); });
    return;
}

void MCPBridge::meterStopAll(const juce::String& meterAction, const juce::OSCMessage& msg)
{
    if (!processor) return;
    juce::String correlationId = msg.size() > 0 ? getArgString(msg[0]) : "";
    probeManager.stopAllProbes(correlationId);
    return;
}

void MCPBridge::meterTap(const juce::String& meterAction, const juce::OSCMessage& msg)
{
    if (!processor) return;
    // /meter/tap <canvasName> <tempId> <mode: solo|mute|off> [outletIndex] [holdMs] <correlationId>
    // NON-DESTRUCTIVE monitor tap: the audio thread keeps only this node
    // (solo) or removes it (mute) in the final output. Patch untouched,
    // RAM-only, expires (default 30s, max 120s).
    if (msg.size() < 3) return;
    auto canvasName = normalizeCanvas(getArgString(msg[0]));
    auto tempId = getArgString(msg[1]);
    juce::String modeStr = getArgString(msg[2]);
    int outletIndex = 0;
    float holdMs = 30000.0f;
    juce::String correlationId = "0";
    size_t i = 3;
    if (i < msg.size() && !msg[i].isString()) { outletIndex = static_cast<int>(getArgFloat(msg[i])); ++i; }
    if (i < msg.size() && !msg[i].isString()) { holdMs = getArgFloat(msg[i]); ++i; }
    if (i < msg.size()) correlationId = getArgString(msg[i]);

    if (!processor) {
        sendReply("/meter/tap/reply/" + correlationId, juce::String("error: processor unavailable"));
        return;
    }
    if (modeStr == "off") {
        processor->getMcp()->session.mcpTapMode.store(McpSession::mcpTapOff, std::memory_order_relaxed);
        processor->getMcp()->session.mcpTapSig.store(nullptr, std::memory_order_relaxed);
        mcpTapTempId.clear();
        sendReply("/meter/tap/reply/" + correlationId, juce::String("off"));
        return;
    }

    int mode = (modeStr == "mute") ? McpSession::mcpTapMute : McpSession::mcpTapSolo;
    sys_lock();
    juce::String errorOut;
    t_outconnect* oc = resolveProbeTarget(canvasName, tempId, outletIndex, errorOut);
    t_signal* signal = oc ? outconnect_get_signal(oc) : nullptr;
    float* vec = (signal && signal->s_vec) ? signal->s_vec : nullptr;
    int n = (signal && signal->s_vec) ? signal->s_n : 0;
    int const tapEpoch = ugen_getsortno(); // P3.4 S5: with the resolve, under the same lock
    sys_unlock();

    if (!vec || n <= 0) {
        processor->getMcp()->session.mcpTapMode.store(McpSession::mcpTapOff, std::memory_order_relaxed);
        processor->getMcp()->session.mcpTapSig.store(nullptr, std::memory_order_relaxed);
        mcpTapTempId.clear();
        sendReply("/meter/tap/reply/" + correlationId,
                  juce::String("error: ") + (errorOut.isNotEmpty() ? errorOut : juce::String("signal not ready (is DSP running?)")));
        return;
    }

    mcpTapCanvas = canvasName;
    mcpTapTempId = tempId;
    mcpTapOutlet = outletIndex;
    auto const until = juce::Time::getMillisecondCounter()
                     + static_cast<juce::uint32>(juce::jlimit(1000.0f, 120000.0f, holdMs));
    processor->getMcp()->session.mcpTapUntilMs.store(until, std::memory_order_relaxed);
    processor->getMcp()->session.mcpTapEpoch.store(tapEpoch, std::memory_order_relaxed);
    processor->getMcp()->session.mcpTapSig.store(static_cast<void*>(signal), std::memory_order_release);
    processor->getMcp()->session.mcpTapMode.store(mode, std::memory_order_release); // publish LAST
    processor->getMcp()->session.mcpTapGeneration.fetch_add(1, std::memory_order_relaxed);
    if (!isTimerRunning())
        startTimer(15); // P3.4 S6: the timer re-resolves the tap after a DSP rebuild
    sendReply("/meter/tap/reply/" + correlationId,
              juce::String(mode == McpSession::mcpTapMute ? "mute " : "solo ") + tempId
              + " " + juce::String(static_cast<int>(juce::jlimit(1000.0f, 120000.0f, holdMs))) + "ms");
    return;
}

void MCPBridge::meterQuery(const juce::String& meterAction, const juce::OSCMessage& msg)
{
    if (!processor) return;
    if (msg.size() < 2) return;
    auto canvasName = normalizeCanvas(getArgString(msg[0]));
    auto tempId = getArgString(msg[1]);
    int outletIndex = 0;
    juce::String correlationId = "0";

    if (msg.size() == 3) {
        correlationId = getArgString(msg[2]);
    } else if (msg.size() >= 4) {
        outletIndex = static_cast<int>(getArgFloat(msg[2]));
        correlationId = getArgString(msg[3]);
    }

    bool justActivated = activateProbing();
    if (justActivated) {
        juce::Thread::sleep(5);
    }

    sys_lock();
    juce::String errorOut;
    t_outconnect* oc = resolveProbeTarget(canvasName, tempId, outletIndex, errorOut);
    if (!oc) {
        sys_unlock();
        sendReply("/meter/error/" + correlationId, errorOut);
        return;
    }

    t_signal* signal = outconnect_get_signal(oc);
    if (!signal || !signal->s_vec) {
        sys_unlock();
        sendReply("/meter/error/" + correlationId, "Signal not ready (is DSP running?)");
        return;
    }

    int n = signal->s_n;
    int nchans = signal->s_nchans;
    float* vec = signal->s_vec;
    float sumSq = 0.0f, peak = 0.0f;
    for (int i = 0; i < n; i++) {
        float s = vec[i];
        float abs_s = std::abs(s);
        sumSq += s * s;
        if (abs_s > peak) peak = abs_s;
    }
    sys_unlock();

    float rms = (n > 0) ? std::sqrt(sumSq / static_cast<float>(n)) : 0.0f;
    float rmsDb = (rms > 1e-7f) ? (20.0f * std::log10(rms)) : -100.0f;
    float peakDb = (peak > 1e-7f) ? (20.0f * std::log10(peak)) : -100.0f;

    juce::OSCMessage rep { juce::OSCAddressPattern("/meter/query/reply/" + correlationId) };
    rep.addArgument(rmsDb);
    rep.addArgument(peakDb);
    rep.addArgument(static_cast<int32>(nchans));
    rep.addArgument(static_cast<int32>(n));
    sender.send(rep);
    return;
}

void MCPBridge::meterSpectral(const juce::String& meterAction, const juce::OSCMessage& msg)
{
    if (!processor) return;
    // /meter/spectral <canvasName> <tempId> [outletIndex] [durationMs] <correlationId>
    // Starts a probe that captures audio into the ring buffer, then runs FFT
    // on completion. Returns spectral centroid, flatness, RMS, peak, fundamental,
    // crest factor, and top frequency bins.
    if (msg.size() < 2) return;
    auto canvasName = normalizeCanvas(getArgString(msg[0]));
    auto tempId = getArgString(msg[1]);
    int outletIndex = 0;
    int durationMs = 200; // ~4400 samples at 44.1kHz, fills ring buffer multiple times
    juce::String correlationId = "0";

    if (msg.size() == 3) {
        correlationId = getArgString(msg[2]);
    } else if (msg.size() == 4) {
        outletIndex = static_cast<int>(getArgFloat(msg[2]));
        correlationId = getArgString(msg[3]);
    } else if (msg.size() >= 5) {
        outletIndex = static_cast<int>(getArgFloat(msg[2]));
        durationMs = static_cast<int>(getArgFloat(msg[3]));
        correlationId = getArgString(msg[4]);
    }

    if (durationMs < 50) durationMs = 50;
    if (durationMs > 2000) durationMs = 2000;

    activateProbing();

    sys_lock();
    juce::String errorOut;
    t_outconnect* oc = resolveProbeTarget(canvasName, tempId, outletIndex, errorOut);
    int const epoch = ugen_getsortno(); // P3.4 S5: read with the resolve, under the same lock
    sys_unlock();

    if (!oc) {
        sendReply("/meter/spectral/error/" + correlationId, errorOut);
        return;
    }

    int probeId = probeManager.startProbe(oc, epoch, canvasName, tempId, outletIndex, correlationId, durationMs);
    if (probeId <= 0) {
        sendReply("/meter/spectral/error/" + correlationId, "Max active probes exceeded");
        return;
    }

    // Mark this probe as spectral so collectResults runs FFT
    for (auto& probe : probeManager.probes) {
        if (probe.probeId == static_cast<uint32_t>(probeId)) {
            probe.spectral = true;
            break;
        }
    }

    if (!isTimerRunning()) {
        startTimer(15);
    }

    juce::OSCMessage rep { juce::OSCAddressPattern("/meter/spectral/started/" + correlationId) };
    rep.addArgument(static_cast<int32>(probeId));
    sender.send(rep);
    return;
}

void MCPBridge::meterStatus(const juce::String& meterAction, const juce::OSCMessage& msg)
{
    if (!processor) return;
    juce::String correlationId = msg.size() > 0 ? getArgString(msg[0]) : "0";
    int activeCount = probeManager.getActiveProbeCount();
    int debugEnabled = plugdata_debugging_enabled();

    juce::OSCMessage rep { juce::OSCAddressPattern("/meter/status/reply/" + correlationId) };
    rep.addArgument(static_cast<int32>(activeCount));
    rep.addArgument(static_cast<int32>(debugEnabled));
    sender.send(rep);
    return;
}

void MCPBridge::meterArm(const juce::String& meterAction, const juce::OSCMessage& msg)
{
    if (!processor) return;
    // /meter/arm <correlationId> — open the measurement window: reset + start the
    // master peak-hold. Call this BEFORE triggering, so a one-shot is inside it.
    auto correlationId = msg.size() > 0 ? getArgString(msg[0]) : juce::String("0");
    if (processor) {
        processor->getMcp()->session.mcpArmPeak.store(0.0f, std::memory_order_relaxed);
        // #83 diagnostics: snapshot the audio-block counter and zero the
        // window counters so `read` can split the failure causes.
        processor->getMcp()->session.mcpArmBlocksAtArm.store(processor->getMcp()->session.mcpAudioBlocks.load(std::memory_order_relaxed), std::memory_order_relaxed);
        processor->getMcp()->session.mcpArmedTailRuns.store(0, std::memory_order_relaxed);
        processor->getMcp()->session.mcpArmedTailPeak.store(0.0f, std::memory_order_relaxed);
        // P9: the window before the output ceiling, and the ceiling's clamp count at arm
        processor->getMcp()->session.mcpArmPreCeilingPeak.store(0.0f, std::memory_order_relaxed);
        processor->getMcp()->session.mcpArmClampedAtArm.store(processor->getMcp()->session.outputCeiling.read().clampedSamples, std::memory_order_relaxed);
        processor->getMcp()->session.mcpArmActive.store(true, std::memory_order_release);
    }
    sendRawReply("/meter/arm/reply/" + correlationId);
    return;
}

void MCPBridge::meterRead(const juce::String& meterAction, const juce::OSCMessage& msg)
{
    if (!processor) return;
    // /meter/read <correlationId> — close the window; reply with peak-since-arm.
    // peakDb > -60 → the trigger produced sound. Never misses a one-shot because
    // the window was armed before the event and accumulated every block since.
    auto correlationId = msg.size() > 0 ? getArgString(msg[0]) : juce::String("0");
    if (processor) processor->getMcp()->session.mcpArmActive.store(false, std::memory_order_release);
    float const peak = processor ? processor->getMcp()->session.mcpArmPeak.load(std::memory_order_relaxed) : 0.0f;
    float const peakDb = (peak > 1e-7f) ? (20.0f * std::log10(peak)) : -100.0f;
    // #83 diagnostics — split the three causes of "armed read silent":
    //   blocksElapsed == 0                    → audio thread isn't ticking
    //   blocksElapsed > 0, armedTailRuns == 0 → the armed tail isn't reached
    //   armedTailRuns > 0, tailPeak ~ 0       → JUCE output buffer is silent
    int64_t blocksElapsed = 0, armedTailRuns = 0;
    float tailPeak = 0.0f;
    if (processor) {
        blocksElapsed = (int64_t)(processor->getMcp()->session.mcpAudioBlocks.load(std::memory_order_relaxed)
                                  - processor->getMcp()->session.mcpArmBlocksAtArm.load(std::memory_order_relaxed));
        armedTailRuns = (int64_t)processor->getMcp()->session.mcpArmedTailRuns.load(std::memory_order_relaxed);
        tailPeak = processor->getMcp()->session.mcpArmedTailPeak.load(std::memory_order_relaxed);
        mcpLogLine(*this, processor, juce::String::formatted("MCP-ARM-DIAG read: peak=%.6f blocksElapsed=%lld armedTailRuns=%lld tailPeak=%.6f",
             (double)peak, (long long)blocksElapsed, (long long)armedTailRuns, (double)tailPeak));
    }
    juce::OSCMessage rep { juce::OSCAddressPattern("/meter/read/reply/" + correlationId) };
    rep.addArgument(peakDb);
    rep.addArgument(peak);
    rep.addArgument(static_cast<int32>(blocksElapsed));
    rep.addArgument(static_cast<int32>(armedTailRuns));
    rep.addArgument(tailPeak);
    // P9: the window before the output ceiling (args 5, 6): its peak, and how many samples the ceiling clamped.
    float preCeilingPeak = 0.0f;
    uint64_t clamped = 0;
    if (processor) {
        preCeilingPeak = processor->getMcp()->session.mcpArmPreCeilingPeak.load(std::memory_order_relaxed);
        clamped = processor->getMcp()->session.outputCeiling.read().clampedSamples
            - processor->getMcp()->session.mcpArmClampedAtArm.load(std::memory_order_relaxed);
    }
    rep.addArgument(preCeilingPeak);
    rep.addArgument(static_cast<int32>(std::min<uint64_t>(clamped, static_cast<uint64_t>(std::numeric_limits<int32>::max()))));
    sender.send(rep);
    return;
}

void MCPBridge::meterArmControl(const juce::String& meterAction, const juce::OSCMessage& msg)
{
    if (!processor) return;
    // /meter/arm_control <receiverName> <correlationId> — open a CONTROL
    // window: bind a native receiver to <receiverName> and accumulate every
    // atom dispatched to it until read. Call BEFORE sending the message.
    if (msg.size() < 1) return;
    auto name = getArgString(msg[0]);
    juce::String const correlationId = msg.size() > 1 ? getArgString(msg[1]) : juce::String("0");
    if (name.isEmpty()) {
        sendReply("/meter/arm_control/error/" + correlationId, "receiver name required");
        return;
    }
    if (!controlWindow) controlWindow = std::make_unique<McpControlWindow>();

    sys_lock();
    // Re-arm replaces any previous window cleanly (frees its binding).
    controlWindow->unbindReceiver();
    ensureMcpCtrlReceiverClass();
    auto* rec = reinterpret_cast<McpCtrlReceiver*>(pd_new(mcpCtrlReceiverClass));
    rec->sym = gensym(name.toRawUTF8());
    rec->bridge = this;
    pd_bind(&rec->obj.ob_pd, rec->sym);
    controlWindow->name = name;
    controlWindow->boundName = rec->sym;
    controlWindow->receiver = rec;
    controlWindow->count.store(0, std::memory_order_relaxed);
    controlWindow->dropped.store(0, std::memory_order_relaxed);
    controlWindow->armed.store(true, std::memory_order_release);
    sys_unlock();

    sendRawReply("/meter/arm_control/reply/" + correlationId);
    return;
}

void MCPBridge::meterReadControl(const juce::String& meterAction, const juce::OSCMessage& msg)
{
    if (!processor) return;
    // /meter/read_control <correlationId> — close the CONTROL window and
    // reply with every message captured since arm. Unbinds the receiver.
    juce::String const correlationId = msg.size() > 0 ? getArgString(msg[0]) : juce::String("0");
    if (!controlWindow) {
        sendReply("/meter/read_control/reply/" + correlationId,
                  "{\"error\":\"no control window armed\"}");
        return;
    }
    sys_lock();
    // exchange(false) makes read idempotent: a second read (or a read with
    // no armed window) reports "not armed" instead of returning stale
    // messages from a previous window.
    bool const wasArmed = controlWindow->armed.exchange(false, std::memory_order_acq_rel);
    if (!wasArmed) {
        sys_unlock();
        sendReply("/meter/read_control/reply/" + correlationId,
                  "{\"error\":\"no control window armed\"}");
        return;
    }
    int const n = controlWindow->count.load(std::memory_order_acquire);
    juce::String const json = controlWindow->toJson(n);
    controlWindow->unbindReceiver();
    sys_unlock();
    sendReply("/meter/read_control/reply/" + correlationId, json);
    return;
}

void MCPBridge::meterArmSpectral(const juce::String& meterAction, const juce::OSCMessage& msg)
{
    if (!processor) return;
    // /meter/arm_spectral <durationMs> <correlationId> — open an ARMED
    // SPECTRAL window: accumulate the final output so a one-shot fired
    // before read lands inside the FFT analysis. Phase 5 twin of /meter/arm.
    double durationMs = msg.size() > 0 ? (double)getArgFloat(msg[0]) : 500.0;
    juce::String const correlationId = msg.size() > 1 ? getArgString(msg[1]) : juce::String("0");
    if (!processor) {
        sendReply("/meter/arm_spectral/reply/" + correlationId, "{\"error\":\"no processor\"}");
        return;
    }
    int const cap = processor->getMcp()->session.mcpSpecCapacity.load(std::memory_order_relaxed);
    if (cap <= 0) {
        sendReply("/meter/arm_spectral/reply/" + correlationId,
                  "{\"error\":\"spectral capture buffer not ready (DSP not prepared)\"}");
        return;
    }
    double const sr = processor->getSampleRate() > 0.0 ? processor->getSampleRate() : 44100.0;
    if (durationMs < 50.0)   durationMs = 50.0;
    if (durationMs > 2000.0) durationMs = 2000.0;
    int target = static_cast<int>(std::ceil(sr * durationMs / 1000.0));
    if (target > cap) target = cap;
    if (target < PROBE_RING_SIZE) target = std::min(cap, PROBE_RING_SIZE);

    processor->getMcp()->session.mcpSpecArmed.store(false, std::memory_order_release);
    processor->getMcp()->session.mcpSpecWritePos.store(0, std::memory_order_relaxed);
    processor->getMcp()->session.mcpSpecLimit.store(target, std::memory_order_relaxed);
    processor->getMcp()->session.mcpSpecArmed.store(true, std::memory_order_release);
    sendRawReply("/meter/arm_spectral/reply/" + correlationId);
    return;
}

void MCPBridge::meterReadSpectral(const juce::String& meterAction, const juce::OSCMessage& msg)
{
    if (!processor) return;
    // /meter/read_spectral <correlationId> — close the window and reply the
    // spectrum of the loudest PROBE_RING_SIZE-sample region captured since arm.
    juce::String const correlationId = msg.size() > 0 ? getArgString(msg[0]) : juce::String("0");
    if (!processor) {
        sendReply("/meter/read_spectral/reply/" + correlationId, "{\"error\":\"no processor\"}");
        return;
    }
    processor->getMcp()->session.mcpSpecArmed.store(false, std::memory_order_release);
    // Wait out any in-flight audio append (a few ms at most) before copying.
    for (int spin = 0; spin < 200; ++spin) {
        if (!processor->getMcp()->session.mcpSpecBusy.load(std::memory_order_acquire)) break;
        juce::Thread::sleep(1);
    }
    int const n = processor->getMcp()->session.mcpSpecWritePos.load(std::memory_order_acquire);
    double const sr = processor->getSampleRate() > 0.0 ? processor->getSampleRate() : 44100.0;
    // #83 diagnostics — same split as /meter/read (audio thread / armed tail / buffer).
    int64_t blocksElapsed = (int64_t)(processor->getMcp()->session.mcpAudioBlocks.load(std::memory_order_relaxed)
                              - processor->getMcp()->session.mcpArmBlocksAtArm.load(std::memory_order_relaxed));
    int64_t armedTailRuns = (int64_t)processor->getMcp()->session.mcpArmedTailRuns.load(std::memory_order_relaxed);
    float tailPeak = processor->getMcp()->session.mcpArmedTailPeak.load(std::memory_order_relaxed);
    juce::String diag = ",\"blocksElapsed\":" + juce::String((int)blocksElapsed)
                      + ",\"armedTailRuns\":" + juce::String((int)armedTailRuns)
                      + ",\"tailPeak\":" + juce::String(tailPeak, 6);
    mcpLogLine(*this, processor, juce::String::formatted("MCP-ARM-DIAG read_spectral: samples=%d blocksElapsed=%lld armedTailRuns=%lld tailPeak=%.6f",
         n, (long long)blocksElapsed, (long long)armedTailRuns, (double)tailPeak));
    juce::String json = armedSpectralJson(processor->getMcp()->session.mcpSpecBuffer.data(), n, sr);
    processor->getMcp()->session.mcpSpecWritePos.store(0, std::memory_order_relaxed);
    if (json.isEmpty()) {
        sendReply("/meter/read_spectral/reply/" + correlationId,
                  juce::String("{\"error\":\"insufficient capture\",\"samples\":") + juce::String(n) + diag + "}");
        return;
    }
    if (json.endsWithChar('}')) json = json.dropLastCharacters(1) + diag + "}";
    sendReply("/meter/read_spectral/reply/" + correlationId, json);
    return;
}

void MCPBridge::meterMaster(const juce::String& meterAction, const juce::OSCMessage& msg)
{
    if (!processor) return;
    // /meter/master <canvasName> <correlationId>
    if (msg.size() < 1) return;
    auto canvasName = normalizeCanvas(getArgString(msg[0]));
    juce::String correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";

    bool justActivated = activateProbing();
    if (justActivated) {
        juce::Thread::sleep(5);
    }

    t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
    if (!cnv) {
        juce::OSCMessage rep { juce::OSCAddressPattern("/meter/master/reply/" + correlationId) };
        rep.addArgument(-100.0f);
        rep.addArgument(-100.0f);
        rep.addArgument(static_cast<int32>(0));
        rep.addArgument(juce::String("canvas_not_found"));
        sender.send(rep);
        return;
    }

    sys_lock();
    MasterMeterResult res;
    try {
        res = computeMasterMeter(processor, cnv, canvasName);
    } catch (...) {
        res.peakDb = -100.0f;
        res.rmsDb = -100.0f;
        res.masterFound = false;
        res.masterType = "error";
    }
    sys_unlock();

    // Bug #83 (P3.4 S10): this reads the dac~'s Pd signal vectors, which keep the last block when the audio
    // callback stops (device lost, a second instance on the device). That stale level was reported as live
    // while every armed read, which needs processBlock, was silent. No block for 250 ms (or four blocks of a
    // larger host buffer): say so instead. Also true during an offline render (it suspends live processing).
    double const blockMs = processor->getSampleRate() > 0.0 ? 1000.0 * processor->juce::AudioProcessor::getBlockSize() / processor->getSampleRate() : 0.0;
    auto const limitMs = static_cast<juce::uint32>(std::max(250.0, 4.0 * blockMs));
    auto const lastBlockMs = processor->getMcp()->session.mcpLastBlockMs.load(std::memory_order_relaxed);
    bool const stalled = lastBlockMs == 0 || juce::Time::getMillisecondCounter() - lastBlockMs > limitMs;
    static std::atomic<bool> reportedStall { false }; // log the transition only (the meter is polled)
    if (stalled && !reportedStall.exchange(true))
        processor->logWarning("MCP meter: the audio callback is not running, the master level is not live");
    if (!stalled)
        reportedStall = false;
    if (stalled && res.masterFound) {
        res.peakDb = -100.0f;
        res.rmsDb = -100.0f;
        res.masterType = "audio_not_running";
    }

    juce::OSCMessage rep { juce::OSCAddressPattern("/meter/master/reply/" + correlationId) };
    rep.addArgument(res.peakDb);
    rep.addArgument(res.rmsDb);
    rep.addArgument(static_cast<int32>(res.masterFound ? 1 : 0));
    rep.addArgument(res.masterType);
    sender.send(rep);
    return;
}

void MCPBridge::meterTrace(const juce::String& meterAction, const juce::OSCMessage& msg)
{
    if (!processor) return;
    // /meter/trace <canvasName> <correlationId>                             the trace of the last audio block
    // /meter/trace <canvasName> <correlationId> arm <windowMs> [focusCsv]  T2b: open a window of taps (armSignalTrace)
    // /meter/trace <canvasName> <correlationId> read                       T2b: close it, answer the trace of its peaks
    if (msg.size() < 1) return;
    auto canvasName = normalizeCanvas(getArgString(msg[0]));
    juce::String correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";
    juce::String const traceMode = msg.size() > 2 ? getArgString(msg[2]) : juce::String();
    if (traceMode == "arm") {
        int const windowMs = msg.size() > 3 ? static_cast<int>(getArgFloat(msg[3])) : 500;
        armSignalTrace(canvasName, correlationId, windowMs, msg.size() > 4 ? getArgString(msg[4]) : juce::String());
        return;
    }
    if (traceMode == "read") {
        readSignalTrace(correlationId);
        return;
    }

    bool justActivated = activateProbing();
    if (justActivated) {
        juce::Thread::sleep(5);
    }

    t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
    if (!cnv) {
        sendReply("/meter/trace/reply/" + correlationId, "{\"error\":\"canvas not found: " + jsonEscape(canvasName) + "\"}");
        return;
    }

    sys_lock();
    juce::String json;
    try {
        json = computeSignalTrace(processor, cnv, canvasName);
    } catch (...) {
        json = "{\"error\":\"trace failed: exception under sys_lock\"}";
    }
    sys_unlock();

    sendReply("/meter/trace/reply/" + correlationId, json);
    return;
}

// ── T2b: the armed signal trace ──────────────────────────────────────────────────────────────────────────────────
// A triggered voice is silent between notes, so the trace of the last block cannot see it. arm opens a tap on the
// signal outlets of the canvas (probe slots in window mode: each keeps the largest block peak since it started) and
// returns after the first audio block; the caller fires the note; read closes the window and answers the same trace
// JSON as the plain verb, computed from the window's peaks, plus `window` (blocks, taps, cap, sampled, what was left
// out) and `levels` (the peak of every tapped outlet). More signal outlets than free slots: the taps are spread along
// the signal path and `focus` objects are always tapped (MCPBridgeTraceTaps.h).

namespace {
std::vector<std::string> splitFocus(juce::String const& csv)
{
    juce::StringArray tokens;
    tokens.addTokens(csv, ",", "");
    std::vector<std::string> out;
    for (auto const& t : tokens) {
        auto const name = t.trim();
        if (name.isNotEmpty()) out.emplace_back(name.toRawUTF8());
    }
    return out;
}
} // namespace

void MCPBridge::releaseTraceWindow()
{
    std::unique_ptr<TraceWindow> old;
    {
        juce::ScopedLock const lock(traceWindowLock);
        old = std::move(traceWindow);
    }
    if (!old) return;
    for (auto const& tap : old->taps) {
        float peak = 0.0f;
        int blocks = 0;
        probeManager.finishWindowTap(tap.probeId, peak, blocks);
    }
}

void MCPBridge::armSignalTrace(const juce::String& canvasName, const juce::String& correlationId, int windowMs, const juce::String& focusCsv)
{
    auto const replyAddress = "/meter/trace/reply/" + correlationId;
    windowMs = juce::jlimit(50, 2000, windowMs);

    t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
    if (!cnv) {
        sendReply(replyAddress, "{\"armed\":false,\"error\":\"canvas not found: " + jsonEscape(canvasName) + "\"}");
        return;
    }
    releaseTraceWindow(); // a new arm replaces an open window (its slots are free for this one)

    if (activateProbing())
        juce::Thread::sleep(5);

    int const cap = MAX_PROBES - probeManager.getActiveProbeCount();
    if (cap <= 0) {
        sendReply(replyAddress, "{\"armed\":false,\"cap\":0,\"error\":\"no free probe slot (" + juce::String(MAX_PROBES) + " probes are running)\"}");
        return;
    }

    auto window = std::make_unique<TraceWindow>();
    window->canvasName = canvasName;
    window->windowMs = windowMs;
    window->cap = cap;
    window->armedAtMs = juce::Time::getMillisecondCounter();

    sys_lock();
    auto const plan = planTraceTaps(processor, cnv, splitFocus(focusCsv), cap);
    int const epoch = ugen_getsortno();
    for (auto const& tap : plan.taps) {
        int const id = probeManager.startProbe(tap.oc, epoch, canvasName, tap.target, tap.outlet, juce::String(), windowMs + ProbeManager::WINDOW_TAP_GRACE_MS, true);
        if (id > 0) window->taps.push_back({ static_cast<uint32_t>(id), tap.name, tap.outlet, tap.cls });
    }
    sys_unlock();
    window->untapped = plan.untapped;
    window->signalOutlets = plan.signalOutlets;
    window->sampled = plan.sampled || window->taps.size() < plan.taps.size();

    if (window->taps.empty()) {
        sendReply(replyAddress, "{\"armed\":false,\"cap\":" + juce::String(cap) + ",\"signalOutlets\":" + juce::String(plan.signalOutlets)
                  + ",\"error\":\"no signal outlet with a connection to tap on this canvas\"}");
        return;
    }
    if (!isTimerRunning())
        startTimer(15); // the timer ends a window nobody reads

    // Settle: wait for the first audio block to reach a tap, so the caller's note lands inside the window.
    auto sawBlock = [this, &window] {
        for (auto const& tap : window->taps)
            for (auto& probe : probeManager.probes)
                if (probe.probeId == tap.probeId && probe.windowBlocks.load(std::memory_order_acquire) > 0)
                    return true;
        return false;
    };
    bool settled = sawBlock();
    for (int waited = 0; waited < 120 && !settled; waited += 4) {
        juce::Thread::sleep(4);
        settled = sawBlock();
    }

    juce::String const json = "{\"armed\":true,\"canvas\":\"" + jsonEscape(canvasName) + "\",\"windowMs\":" + juce::String(windowMs)
        + ",\"taps\":" + juce::String(static_cast<int>(window->taps.size())) + ",\"cap\":" + juce::String(cap)
        + ",\"signalOutlets\":" + juce::String(window->signalOutlets) + ",\"sampled\":" + (window->sampled ? "true" : "false")
        + ",\"settled\":" + (settled ? "true" : "false") + "}";
    {
        juce::ScopedLock const lock(traceWindowLock);
        traceWindow = std::move(window);
    }
    sendReply(replyAddress, json);
}

void MCPBridge::readSignalTrace(const juce::String& correlationId)
{
    auto const replyAddress = "/meter/trace/reply/" + correlationId;
    std::unique_ptr<TraceWindow> window;
    {
        juce::ScopedLock const lock(traceWindowLock);
        window = std::move(traceWindow);
    }
    if (!window) {
        sendReply(replyAddress, "{\"error\":\"no armed trace window (arm one first: /meter/trace <canvas> <id> arm <windowMs>)\"}");
        return;
    }

    TraceWindowView view;
    int maxBlocks = 0;
    juce::String levels;
    juce::String unread;
    for (auto const& tap : window->taps) {
        float peak = 0.0f;
        int blocks = 0;
        bool const got = probeManager.finishWindowTap(tap.probeId, peak, blocks);
        if (!levels.isEmpty()) levels += ",";
        levels += "{\"tempId\":\"" + jsonEscape(juce::String(tap.name)) + "\",\"class\":\"" + jsonEscape(tap.cls) + "\",\"outlet\":" + juce::String(tap.outlet);
        if (got && blocks > 0) {
            view.peaks[tap.name + ":" + std::to_string(tap.outlet)] = peak;
            maxBlocks = std::max(maxBlocks, blocks);
            levels += ",\"peakDb\":" + juce::String(mcp::trace::peakToDb(peak), 1) + ",\"blocks\":" + juce::String(blocks) + "}";
        } else {
            if (!unread.isEmpty()) unread += ",";
            unread += "\"" + jsonEscape(juce::String(tap.name)) + ":" + juce::String(tap.outlet) + "\"";
            levels += ",\"peakDb\":null,\"blocks\":0}";
        }
    }
    juce::String untapped;
    int shown = 0;
    for (auto const& name : window->untapped) {
        if (shown++ >= 80) break; // enough to name; the count says the rest
        if (!untapped.isEmpty()) untapped += ",";
        untapped += "\"" + jsonEscape(juce::String(name)) + "\"";
    }
    juce::String const windowJson = "\"window\":{\"armedMs\":" + juce::String(window->windowMs)
        + ",\"ms\":" + juce::String(static_cast<int>(juce::Time::getMillisecondCounter() - window->armedAtMs))
        + ",\"blocks\":" + juce::String(maxBlocks) + ",\"taps\":" + juce::String(static_cast<int>(window->taps.size()))
        + ",\"cap\":" + juce::String(window->cap) + ",\"signalOutlets\":" + juce::String(window->signalOutlets)
        + ",\"sampled\":" + (window->sampled ? "true" : "false") + ",\"unread\":[" + unread + "],\"untapped\":[" + untapped
        + "],\"untappedCount\":" + juce::String(static_cast<int>(window->untapped.size())) + "}";
    if (maxBlocks == 0) {
        sendReply(replyAddress, "{\"error\":\"no audio block ran in the window (is DSP on and the audio device running?)\"," + windowJson + ",\"levels\":[" + levels + "]}");
        return;
    }
    view.extraJson = "," + windowJson + ",\"levels\":[" + levels + "]";

    t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(window->canvasName);
    if (!cnv) {
        sendReply(replyAddress, "{\"error\":\"canvas not found: " + jsonEscape(window->canvasName) + "\"," + windowJson + "}");
        return;
    }
    sys_lock();
    juce::String json;
    try {
        json = computeSignalTrace(processor, cnv, window->canvasName, &view);
    } catch (...) {
        json = "{\"error\":\"trace failed: exception under sys_lock\"}";
    }
    sys_unlock();
    sendReply(replyAddress, json);
}
