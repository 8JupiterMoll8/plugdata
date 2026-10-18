/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Bridge commands: recording and offline render (Plan 3 P3.3): handlers moved unchanged out of MCPBridge::handlePdDomain.

#include "MCPBridgeCmdCommon.h"

// =========================================================================
// Offline Render (/pd/render)
// =========================================================================

// ── Output-path guard for /pd/record_start and /pd/render (Report F6) ──
// Files may only be written below the render root: $PLUGDATA_MCP_RENDER_DIR
// when set to an absolute path, else ~/Documents/plugdata/mcp-renders.
static juce::File mcpRenderRoot()
{
    auto const env = juce::SystemStats::getEnvironmentVariable("PLUGDATA_MCP_RENDER_DIR", {});
    if (env.isNotEmpty() && juce::File::isAbsolutePath(env))
        return juce::File(env);
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("plugdata")
        .getChildFile("mcp-renders");
}

// The safe absolute path for a client-supplied one, or empty when it escapes
// the render root (the caller must refuse the request).
static juce::String resolveMcpOutputPath(juce::String const& requested)
{
    auto const root = mcpRenderRoot();
    if (!root.createDirectory().wasOk())
        return {};
    auto const resolved = mcp::safety::resolveOutputPath(
        std::filesystem::path(root.getFullPathName().toStdString()), requested.toStdString());
    return resolved ? juce::String(resolved->string()) : juce::String();
}

// Single-flight guard — one render at a time. File-static: the OSC thread
// (claim/release around launch) and the render thread (release at exit) are
// the only touchers.
static std::atomic<bool> mcpRenderActive { false };

// RAII: ensures live audio is resumed + the single-flight flag is cleared on
// every exit path of the render thread (including early failures).
struct RenderGuard {
    PluginProcessor* proc;
    ~RenderGuard()
    {
        if (proc)
            proc->suspendProcessing(false);
        mcpRenderActive.store(false);
    }
};

// The offline render itself. Runs on a detached background thread; owns the
// DSP graph for its whole lifetime (live audio suspended via RAII guard).
static void runOfflineRender(PluginProcessor* processor, juce::String const& filePath,
    float durationSec, bool analyze, juce::String const& correlationId, MCPBridge* bridge)
{
    RenderGuard guard { processor };

    auto const t0 = juce::Time::getMillisecondCounterHiRes();

    int const pdBlockSize = pd::Instance::getBlockSize();
    double const sampleRate = (processor->getSampleRate() > 0.0) ? processor->getSampleRate() : 44100.0;

    // Stereo cap + channel count consistent with the live recorder tap.
    int const numCh = juce::jlimit(1, 2,
        std::max(processor->getTotalNumInputChannels(), processor->getTotalNumOutputChannels()));
    int const maxChannels = std::max(processor->getTotalNumInputChannels(), processor->getTotalNumOutputChannels());

    int64 const totalSamples = (int64)std::ceil(durationSec * sampleRate);
    int64 const totalBlocks = (totalSamples + pdBlockSize - 1) / pdBlockSize;

    // Interleaved render accumulation buffer. 60s stereo @44.1k ~ 21MB (bounded).
    std::vector<float> renderBuf((size_t)(numCh * totalSamples), 0.0f);

    // Suspend live audio FIRST — the render thread becomes the sole graph
    // runner (standalone player skips callbacks while suspended).
    processor->suspendProcessing(true);

    // Input vectors: pre-sized maxChannels * pdBlockSize by prepareToPlay.
    // Zero once — [adc~] reads silence, correct offline semantics.
    auto const inSize = (size_t)(maxChannels * pdBlockSize);
    std::vector<float> inVec(inSize, 0.0f);
    std::vector<float> outVec(inSize, 0.0f);

    bool renderOk = true;
    juce::String failReason;

    for (int64 b = 0; b < totalBlocks && renderOk; ++b) {
        int64 const sampleOff = b * pdBlockSize;
        int const samplesThisBlock = (int)std::min<int64>(pdBlockSize, totalSamples - sampleOff);

        // Mirror processConstant per block: message phase then DSP phase.
        processor->setThis();
        processor->sendParameters();
        processor->getMcp()->drainPdQueue(); // the bridge's Pd jobs (P3.4 S2), as audioBlockStarted does live
        processor->sendMessagesFromQueue(); // drains OSC fires + loadbangs

        std::fill(outVec.begin(), outVec.end(), 0.0f);
        processor->performDSP(inVec.data(), outVec.data()); // sys_lock inside

        bridge->audioTick(); // probes + native transport + sequencer advance

        // De-interleave out-vector → renderBuf (processConstant copy pattern)
        for (int ch = 0; ch < numCh; ++ch) {
            float const* src = outVec.data() + ch * pdBlockSize;
            float* dst = renderBuf.data() + (size_t)(ch * totalSamples + sampleOff);
            std::copy(src, src + samplesThisBlock, dst);
        }
    }

    auto const t1 = juce::Time::getMillisecondCounterHiRes();

    // Write the WAV — 16-bit PCM stereo-cap, recorder conventions, render
    // thread owns the file exclusively (no lazy audio-thread writer).
    if (renderOk) {
        juce::File destFile(filePath);
        auto outStream = std::unique_ptr<juce::FileOutputStream>(destFile.createOutputStream());
        if (!outStream) {
            renderOk = false;
            failReason = "could not create output stream";
        } else {
            juce::WavAudioFormat wavFormat;
            auto writer = std::unique_ptr<juce::AudioFormatWriter>(
                wavFormat.createWriterFor(outStream.get(), sampleRate, numCh, 16, {}, 0));
            if (!writer) {
                renderOk = false;
                failReason = "WAV writer creation failed";
            } else {
                outStream.release(); // writer owns the stream now
                // juce::AudioBuffer over the interleaved planes — needs split
                // channels, so wrap per-channel pointers.
                juce::AudioBuffer<float> tmpBuf(numCh, (int)totalSamples);
                for (int ch = 0; ch < numCh; ++ch)
                    tmpBuf.copyFrom(ch, 0, renderBuf.data() + (size_t)(ch * totalSamples), (int)totalSamples);
                writer->writeFromAudioSampleBuffer(tmpBuf, 0, (int)totalSamples);
                writer->flush();
            }
        }
    }

    if (!renderOk) {
        bridge->sendReply("/pd/render/error/" + correlationId, failReason.isNotEmpty() ? failReason : juce::String("render failed"));
        return;
    }

    auto const wallClockMs = t1 - t0;

    // Build the completion JSON.
    auto* root = new juce::DynamicObject();
    root->setProperty("path", filePath);
    root->setProperty("renderedMs", (int)(durationSec * 1000.0f));
    root->setProperty("wallClockMs", (int)wallClockMs);
    root->setProperty("speedFactor", (wallClockMs > 1.0) ? (double)(durationSec * 1000.0f) / wallClockMs : 0.0);
    root->setProperty("sampleRate", sampleRate);
    root->setProperty("numChannels", numCh);
    root->setProperty("pdBlockSize", pdBlockSize);
    root->setProperty("blocks", (int64)totalBlocks);

    if (analyze && numCh >= 1 && totalSamples >= PROBE_RING_SIZE) {
        // Analyze the loudest channel (robust when a voice is panned).
        int bestCh = 0;
        double bestRms = -1.0;
        for (int ch = 0; ch < numCh; ++ch) {
            double sumSq = 0.0;
            float const* p = renderBuf.data() + (size_t)(ch * totalSamples);
            for (int64 i = 0; i < totalSamples; ++i)
                sumSq += (double)(p[i] * p[i]);
            double const rms = std::sqrt(sumSq / (double)totalSamples);
            if (rms > bestRms) {
                bestRms = rms;
                bestCh = ch;
            }
        }
        auto analysisJson = renderSpectralJson(renderBuf.data() + (size_t)(bestCh * totalSamples),
            (int)totalSamples, sampleRate);
        if (analysisJson.isNotEmpty())
            root->setProperty("analysis", juce::JSON::parse(analysisJson));
    }

    bridge->sendReply("/pd/render/reply/" + correlationId, juce::JSON::toString(juce::var(root), true));
}

     // /pd/record_start <filePath> [corrId]
    // Zero-dropout WAV recorder — taps processBlock output buffer directly.
    // No canvas objects created, no DSP recompile, no audio dropout.
    // The WAV writer is created lazily on the audio thread (first tap) so its
    // channel count always matches the real output buffer — getTotalNumOutputChannels()
    // can report a bus layout (e.g. 32ch) that differs from the actual buffer (e.g. 2ch).
    // Reply: /pd/record_start/reply/<corrId>  1.0=ok, 0.0=failed
void MCPBridge::pdRecordStart(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 1 && processor) {
        auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";
        juce::String replyAddr = "/pd/record_start/reply/" + correlationId;
        auto const filePath = resolveMcpOutputPath(getArgString(msg[0]));
        if (filePath.isEmpty()) {
            post("MCP record_start: refused, path is outside %s", mcpRenderRoot().getFullPathName().toRawUTF8());
            sendReply(replyAddr, 0.0f);
            return;
        }

        // Fail fast if we can't create the parent directory.
        juce::File destFile(filePath);
        auto parent = destFile.getParentDirectory();
        if (!parent.createDirectory().wasOk()) {
            post("MCP record_start: could not create directory %s", parent.getFullPathName().toRawUTF8());
            sendReply(replyAddr, 0.0f);
            return;
        }

        // Report F3: open the file and writer here, never on the audio thread.
        if (!processor->getMcp()->mcpOpenRecorder(filePath)) {
            sendReply(replyAddr, 0.0f);
            return;
        }
        processor->getMcp()->session.mcpRecording.store(true, std::memory_order_release);
        post("MCP record_start: recording path=%s", filePath.toRawUTF8());
        sendReply(replyAddr, 1.0f);
    }
    return;
}

    // /pd/record_stop [corrId]
    // Stop the zero-dropout WAV recorder and flush/close the file.
    // Reply: /pd/record_stop/reply/<corrId>  1.0=ok
void MCPBridge::pdRecordStop(const juce::String& action, const juce::OSCMessage& msg)
{
    auto correlationId = msg.size() > 0 ? getArgString(msg[0]) : "0";
    if (processor) {
        processor->getMcp()->mcpCloseRecorder(); // flushes and closes the file
    }
    sendReply("/pd/record_stop/reply/" + correlationId, 1.0f);
    return;
}

    // /pd/render <filePath> <durationSec> [analyze 0|1] [corrId]
    // Offline faster-than-realtime render: bakes the current patch to WAV on a
    // background thread by looping performDSP without the audio device clock.
    // Mirrors processConstant exactly (setThis/sendParameters/sendMessagesFromQueue/
    // performDSP/audioTick) so a render is what you'd hear live — sequencer jobs
    // and transport advance inside the loop via audioTick(). Live audio is
    // suspended for the render wall-time (standalone player skips the callback
    // while suspended, so no dropouts — a brief mute blip only).
    // Replies:
    //   /pd/render/started/<corrId>          (immediate ack, OSC thread)
    //   /pd/render/reply/<corrId> <json>     (completion: path, timings, analysis?)
    //   /pd/render/error/<corrId> <reason>   (failure after the ack)
void MCPBridge::pdRender(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 2 && processor) {
        auto const filePath = resolveMcpOutputPath(getArgString(msg[0]));
        float durationSec  = getArgFloat(msg[1]);
        bool analyze       = (msg.size() >= 3) ? (getArgFloat(msg[2]) > 0.5f) : false;
        auto correlationId = (msg.size() >= 4) ? getArgString(msg[3]) : "0";

        durationSec = juce::jlimit(0.1f, 60.0f, durationSec);

        if (filePath.isEmpty()) {
            sendReply("/pd/render/error/" + correlationId,
                "path is outside the render folder " + mcpRenderRoot().getFullPathName());
            return;
        }

        // Single-flight: one render at a time.
        if (mcpRenderActive.exchange(true)) {
            sendReply("/pd/render/error/" + correlationId, juce::String("render already in progress"));
            return;
        }

        juce::File destFile(filePath);
        auto parent = destFile.getParentDirectory();
        if (!parent.createDirectory().wasOk()) {
            mcpRenderActive.store(false);
            post("MCP render: could not create directory %s", parent.getFullPathName().toRawUTF8());
            sendReply("/pd/render/error/" + correlationId, juce::String("could not create directory"));
            return;
        }

        sendRawReply("/pd/render/started/" + correlationId);

        // Detached one-shot render thread — owns the graph for its lifetime.
        std::thread([proc = processor, filePath = filePath, durationSec, analyze,
                     correlationId = correlationId, bridge = this]() {
            runOfflineRender(proc, filePath, durationSec, analyze, correlationId, bridge);
        }).detach();
    } else if (processor) {
        auto correlationId = (msg.size() >= 4) ? getArgString(msg[3]) : "0";
        sendReply("/pd/render/error/" + correlationId, juce::String("usage: /pd/render <path> <durationSec> [analyze] [corrId]"));
    }
    return;
}
