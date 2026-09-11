/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

#include "MCPBridge.h"
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Canvas.h"
#include "TabComponent.h"
#include "Object.h"
#include "Objects/ObjectBase.h"
#include "Objects/AllGuis.h" // t_fake_knob raw snd/rcv fields for screenshot labels
#include "Pd/Interface.h"
#include "Utility/Fonts.h"
#include "../../Libraries/fftw3/api/fftw3.h"

#include <set>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <cstdlib>
#include <thread>
#include <vector>
#include <deque>
struct BatchDedupEntry {
  uint64_t ts = 0;
  juce::OSCMessage reply;
  BatchDedupEntry() = delete;
  BatchDedupEntry(uint64_t t, juce::OSCMessage r) : ts(t), reply(std::move(r)) {}
  BatchDedupEntry(const BatchDedupEntry&) = default;
  BatchDedupEntry(BatchDedupEntry&&) = default;
  BatchDedupEntry& operator=(const BatchDedupEntry&) = default;
  BatchDedupEntry& operator=(BatchDedupEntry&&) = default;
};


extern "C" {
#include <m_pd.h>
#include <g_canvas.h>
#include <s_inter.h>
#include <g_all_guis.h> // t_iemgui x_snd/x_rcv for screenshot GUI-widget labels
#include <m_imp.h>      // obj_starttraverseoutlet/obj_nexttraverseoutlet (duplicate-wire check)

extern t_class *text_class;

// pd-else knob: snd/rcv are lazily parsed from the binbuf — the GUI calls these
// before reading x_snd_raw/x_rcv_raw (KnobObject.h); screenshot labels do the same.
extern "C" void knob_get_snd(void* x);
extern "C" void knob_get_rcv(void* x);

struct _outlet
{
    t_object *o_owner;
    struct _outlet *o_next;
    t_outconnect *o_connections;
    t_symbol *o_sym;
};
}

MCPBridge::MCPBridge(PluginProcessor* proc, int inPort, int outPort)
    : processor(proc)
    , listenPort(inPort)
    , sendPort(outPort)
    , probeManager(this)
{
    bootToken = juce::String(juce::Time::getMillisecondCounter()) + "-"
        + juce::String::toHexString(juce::Random::getSystemRandom().nextInt());
    start();
}

MCPBridge::~MCPBridge()
{
    stop();
}

bool MCPBridge::start()
{
    if (active.load()) return true;

    bool recvOk = receiver.connect(listenPort);
    if (recvOk) {
        receiver.addListener(this);
        active.store(true);
        reportedConnected = false;
        reportedLost = false;
        statusMessage = {};
        if (processor) {
            processor->logMessage("MCP bridge listening on UDP " + juce::String(listenPort));
        }
    } else {
        statusMessage = "port " + juce::String(listenPort) + " in use";
        if (processor) {
            processor->logError("MCP bridge: could not bind UDP " + juce::String(listenPort) + " (port in use?)");
        }
    }

    sender.connect("127.0.0.1", sendPort);

    return active.load();
}

void MCPBridge::stop()
{
    stopTimer();
    if (!active.load()) return;

    receiver.removeListener(this);
    receiver.disconnect();
    sender.disconnect();
    active.store(false);
    reportedConnected = false;
    reportedLost = false;
    statusMessage = {};
    if (processor) {
        processor->logMessage("MCP bridge stopped");
    }
}

bool MCPBridge::isConnected() const
{
    return active.load();
}

juce::String MCPBridge::getStatus() const
{
    if (!active.load()) return statusMessage.isNotEmpty() ? ("error: " + statusMessage) : juce::String("disabled");

    auto const last = lastServerActivity.load();
    if (last == 0) return "waiting for server";

    auto const elapsed = juce::Time::getMillisecondCounter() - last;
    if (elapsed > 15000) {
        // Server went quiet. Emit once (lazily — the Advanced panel polls
        // getStatus(), which is where an artist would look).
        if (!reportedLost && processor) {
            processor->logError("MCP server connection lost");
            reportedLost = true;
        }
        return "disconnected";
    }

    return "connected";
}

void MCPBridge::noteServerActivity()
{
    bool const first = (lastServerActivity.load() == 0);
    lastServerActivity.store(juce::Time::getMillisecondCounter());
    if (first && !reportedConnected && processor) {
        processor->logMessage("MCP server connected");
        reportedConnected = true;
    }
    reportedLost = false;
}

juce::String MCPBridge::normalizeCanvas(const juce::String& name)
{
    juce::String s = name.trim();
    if (s.isEmpty() || s == "main") return "pd-main";
    // PRD Phase 3.1: root-file tab symbols are the raw file name (the bound
    // gl_name, e.g. "phase3_roundtrip_test.pd") — pass through unmangled so
    // canvas resolution and identity map keys agree.
    if (s.endsWith(".pd")) return s;
    while (s.startsWith("pd-")) s = s.substring(3);
    if (s.isEmpty()) return "pd-main";
    return "pd-" + s;
}

static juce::String getArgString(const juce::OSCArgument& arg);
static float getArgFloat(const juce::OSCArgument& arg);

// The plugdata fork does not expose glist_nth; walk the glist ourselves.
static t_gobj* glistObjectAt(t_canvas* cnv, int index)
{
    if (!cnv || index < 0) return nullptr;
    int i = 0;
    for (t_gobj* y = cnv->gl_list; y; y = y->g_next) {
        if (i++ == index) return y;
    }
    return nullptr;
}

// Build the textual object representation PlugData's GUI expects, applying
// the same kind mapping used by mcp_create_batch_id in PluginProcessor.
static juce::String buildObjectText(const juce::String& kind, const juce::StringArray& tokens)
{
    juce::StringArray t = tokens;
    if (kind == "msg") {
        if (!t.isEmpty() && t[0] == "msg") t.remove(0);
        return "msg " + t.joinIntoString(" ");
    }
    if (kind == "text" || kind == "comment") {
        if (!t.isEmpty() && (t[0] == "text" || t[0] == "comment")) t.remove(0);
        return "comment " + t.joinIntoString(" ");
    }
    if (kind == "floatatom" || kind == "floatbox") {
        if (!t.isEmpty() && (t[0] == "floatatom" || t[0] == "floatbox")) t.remove(0);
        return "floatbox " + t.joinIntoString(" ");
    }
    if (kind == "symbolatom" || kind == "symbolbox") {
        if (!t.isEmpty() && (t[0] == "symbolatom" || t[0] == "symbolbox")) t.remove(0);
        return "symbolbox " + t.joinIntoString(" ");
    }
    return t.joinIntoString(" ");
}

// =========================================================================
// Zero-Dropout Paste Helpers
// =========================================================================

// Direct binbuf_eval paste — bypasses canvas_dopaste entirely.
// Skips: undo serialization, object selection, editmode switch,
// and the inner canvas_suspend_dsp/canvas_resume_dsp cycle.
static void pasteDirect(t_canvas* cnv, char const* buf)
{
    // Sanitise the paste buffer: drop top-level "#A" (array) lines.
    //
    // pasteDirect nulls sym_A->s_thing (below) because pasted object lines
    // never carry array data — but binbuf_eval still DISPATCHES any "#A" line
    // to that symbol. With s_thing == nullptr that is a null dereference →
    // segfault. GOP-module saves leak stray "#A saved" lines (ELSE savestate
    // objects inside the abstraction), so loading ANY patch that references a
    // GOP module crashed. Arrays are recreated by their "#X obj table/array"
    // lines, so dropping the stray "#A" rows is safe.
    juce::String filtered;
    {
        juce::StringArray lines;
        lines.addLines(juce::String(buf));
        for (auto& ln : lines) {
            if (ln.trimStart().startsWith("#A ")) continue;
            filtered += ln;
            filtered += "\n";
        }
    }

    size_t const len = filtered.getNumBytesAsUTF8();
    t_binbuf* b = binbuf_new();
    binbuf_text(b, filtered.toRawUTF8(), len);

    t_symbol* sym_X = gensym("#X");
    t_symbol* sym_N = gensym("#N");
    t_symbol* sym_A = gensym("#A");

    t_pd* saved_X = sym_X->s_thing;
    t_pd* saved_N = sym_N->s_thing;
    t_pd* saved_A = sym_A->s_thing;

    sym_A->s_thing = nullptr;
    sym_X->s_thing = &cnv->gl_pd;
    sym_N->s_thing = &pd_canvasmaker;

    binbuf_eval(b, 0, 0, 0);

    sym_A->s_thing = saved_A;
    sym_X->s_thing = saved_X;
    sym_N->s_thing = saved_N;

    binbuf_free(b);
}

// ── Undo-stack safety ──────────────────────────────────────────────
// Free a canvas's Pd undo queue after an MCP mutation that removes or
// replaces objects (delete / clear / load).
//
// Why: MCP mutation paths (pasteDirect for create, removeObjectsAudioThread
// for delete) and the wholesale clear/load paths intentionally do NOT
// register Pd undo actions. But Pd's undo entries reference their target
// objects by *index* (canvas_undo_apply stores u_index, resolved via
// glist_nth; create/recreate/cut do the same). Deleting or clearing objects
// shifts or frees those targets, leaving the queue holding stale
// t_undo_action nodes whose data/indices no longer match the live list. The
// next undo/redo walks them and dereferences freed memory — the observed
// "unsupported undo command <garbage int>" heap-corruption crash.
//
// Since MCP never contributes correct undo actions, the only safe state for
// the queue after such a mutation is EMPTY. canvas_undo_free() must run on
// the Pd scheduler thread, so this routes through
// receiveSysMessage("mcp_clear_undo") — the exact proven-safe path used by
// the /pd/clear_undo action (PluginProcessor mcp_clear_undo case).
static void resetCanvasUndo(PluginProcessor* proc, juce::String const& canvasName)
{
    if (!proc) return;
    SmallArray<pd::Atom> atoms;
    atoms.add(pd::Atom(proc->generateSymbol(canvasName)));
    proc->receiveSysMessage("mcp_clear_undo", atoms);
}

// Escape semicolons for Pd paste buffer format
static juce::String escapePdText(const juce::String& text)
{
    return text.replace(";", " \\;");
}

// Escape commas AND semicolons inside OBJECT box creation args so
// binbuf_text doesn't split them into separate messages. binbuf_text
// treats `,` as a message separator, so an unescaped `[expr pow($f1, 2)]`
// was created as `expr pow($f1` + a stray `2)` message. Backslash escapes
// are the Pd paste-buffer convention: `\,` → literal comma in the atom.
static juce::String escapePdObjArgs(const juce::String& text)
{
    juce::String out = text;
    out = out.replace("\\", "\\\\");
    out = out.replace(",", "\\,");
    out = out.replace(";", "\\;");
    return out;
}

// Convert MCP kind + tokens to Pd paste-format line
static juce::String formatAsPdLine(const juce::String& kind,
                                    const juce::StringArray& tokens,
                                    int x, int y)
{
    juce::StringArray t = tokens;

    if (kind == "msg" || kind == "message" || (!t.isEmpty() && t[0] == "msg")) {
        if (!t.isEmpty() && (t[0] == "msg" || t[0] == "message")) t.remove(0);
        while (!t.isEmpty() && t[0].isEmpty()) t.remove(0);
        return "#X msg " + juce::String(x) + " " + juce::String(y)
               + " " + escapePdText(t.joinIntoString(" ").trim()) + ";";
    }
    if (kind == "text" || kind == "comment" || (!t.isEmpty() && (t[0] == "text" || t[0] == "comment"))) {
        if (!t.isEmpty() && (t[0] == "text" || t[0] == "comment")) t.remove(0);
        while (!t.isEmpty() && t[0].isEmpty()) t.remove(0);
        return "#X text " + juce::String(x) + " " + juce::String(y)
               + " " + escapePdText(t.joinIntoString(" ").trim()) + ";";
    }
    if (kind == "floatatom" || kind == "floatbox") {
        if (!t.isEmpty() && (t[0] == "floatatom" || t[0] == "floatbox")) t.remove(0);
        // PlugData's FloatAtomObject has a GUI-component lifecycle bug that
        // crashes the renderer when a [floatatom] is created/destroyed via the
        // MCP mutation engine (reproduced by the GUI stress gauntlet). [numbox]
        // is the equivalent numeric display/input with a stable component and
        // identical gatom argument layout, so substitute it.
        t.insert(0, "numbox");
        return "#X obj " + juce::String(x) + " " + juce::String(y)
               + " " + escapePdObjArgs(t.joinIntoString(" ").trim()) + ";";
    }
    if (kind == "symbolatom" || kind == "symbolbox") {
        if (!t.isEmpty() && (t[0] == "symbolatom" || t[0] == "symbolbox")) t.remove(0);
        return "#X symbolatom " + juce::String(x) + " " + juce::String(y)
               + " " + t.joinIntoString(" ").trim() + ";";
    }

    if (!t.isEmpty() && t[0] == "+") t.set(0, "\\+");
    return "#X obj " + juce::String(x) + " " + juce::String(y)
           + " " + escapePdObjArgs(t.joinIntoString(" ").trim()) + ";";
}

// =========================================================================
// Offline Render (/pd/render)
// =========================================================================

static float estimateFrequency(const float* buf, int n, float sampleRate); // defined near ProbeManager

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

// Spectral analysis over the baked render buffer. Reuses the /meter/spectral
// feature set (Hann window + FFTW r2c on the final 1024 samples of the
// loudest channel) so the JSON keys match what the server already parses.
static juce::String renderSpectralJson(float const* data, int n, double sampleRate)
{
    constexpr int N = PROBE_RING_SIZE; // 1024
    if (n < N || !data || sampleRate <= 0.0)
        return {};

    // Hann window over the final N samples
    std::array<float, N> windowed {};
    for (int i = 0; i < N; ++i) {
        float const w = 0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * i / (N - 1)));
        windowed[i] = data[n - N + i] * w;
    }

    // RMS / peak over the full analyzed window (unwindowed)
    double sumSq = 0.0;
    float maxPeak = 0.0f;
    for (int i = n - N; i < n; ++i) {
        float const s = std::abs(data[i]);
        sumSq += (double)(data[i] * data[i]);
        if (s > maxPeak) maxPeak = s;
    }
    float const rms = (float)std::sqrt(sumSq / N);

    // FFT (single precision, same as the probe path)
    std::array<float, N> fftInput {};
    std::copy(windowed.begin(), windowed.end(), fftInput.begin());
    constexpr int NBINS = N / 2 + 1;
    std::vector<fftwf_complex> fftOutput(NBINS);
    fftwf_plan plan = fftwf_plan_dft_r2c_1d(N, fftInput.data(),
        reinterpret_cast<fftwf_complex*>(fftOutput.data()), FFTW_ESTIMATE);
    fftwf_execute(plan);
    fftwf_destroy_plan(plan);

    float const binHz = (float)sampleRate / (float)N;

    double sumWeightedFreq = 0.0, sumMag = 0.0;
    double logSum = 0.0;
    int magCount = 0;
    float maxMag = 0.0f;
    int maxBin = 0;
    for (int i = 1; i < NBINS; ++i) { // skip DC
        float const re = fftOutput[i][0];
        float const im = fftOutput[i][1];
        float const mag = std::sqrt(re * re + im * im);
        float const freq = i * binHz;
        sumWeightedFreq += (double)(mag * freq);
        sumMag += (double)mag;
        if (mag > 1e-10f) {
            logSum += std::log((double)mag);
            magCount++;
        }
        if (mag > maxMag) {
            maxMag = mag;
            maxBin = i;
        }
    }

    float const spectralCentroid = (sumMag > 1e-10) ? (float)(sumWeightedFreq / sumMag) : 0.0f;
    double const arithmeticMean = (magCount > 0) ? (sumMag / magCount) : 0.0;
    double const geometricMean = (magCount > 0) ? std::exp(logSum / magCount) : 0.0;
    float spectralFlatness = (arithmeticMean > 1e-10)
        ? (float)(geometricMean / arithmeticMean) : 0.0f;
    spectralFlatness = juce::jlimit(0.0f, 1.0f, spectralFlatness);

    // Rolloff (85% of spectral energy)
    double sumEnergy = 0.0;
    std::vector<double> binEnergy(NBINS, 0.0);
    for (int i = 1; i < NBINS; ++i) {
        float const re = fftOutput[i][0];
        float const im = fftOutput[i][1];
        binEnergy[i] = (double)(re * re + im * im);
        sumEnergy += binEnergy[i];
    }
    float rolloffFreq = 0.0f;
    if (sumEnergy > 1e-12) {
        double acc = 0.0;
        for (int i = 1; i < NBINS; ++i) {
            acc += binEnergy[i];
            if (acc >= 0.85 * sumEnergy) {
                rolloffFreq = i * binHz;
                break;
            }
        }
    }

    // Top-8 harmonic peaks (local maxima above 1% of max magnitude)
    juce::Array<juce::var> peaksArr;
    for (int i = 2; i < NBINS - 1 && peaksArr.size() < 8; ++i) {
        auto magAt = [&](int k) {
            return std::sqrt(fftOutput[k][0] * fftOutput[k][0] + fftOutput[k][1] * fftOutput[k][1]);
        };
        float const m = magAt(i);
        if (m > 0.01f * maxMag && m >= magAt(i - 1) && m >= magAt(i + 1)) {
            float const db = (m > 1e-7f) ? (20.0f * std::log10(m)) : -100.0f;
            auto* p = new juce::DynamicObject();
            p->setProperty("freq", i * binHz);
            p->setProperty("dB", db);
            peaksArr.add(juce::var(p));
        }
    }

    float const rmsDb = (rms > 1e-7f) ? (20.0f * std::log10(rms)) : -100.0f;
    float const peakDb = (maxPeak > 1e-7f) ? (20.0f * std::log10(maxPeak)) : -100.0f;
    float const crestFactor = (rms > 1e-7f) ? (maxPeak / rms) : 0.0f;
    float const peakFrequency = maxBin * binHz;

    // Fundamental via autocorrelation on the raw tail (same helper as probes)
    float const fundamental = estimateFrequency(data + (n - N), N, (float)sampleRate);

    auto* root = new juce::DynamicObject();
    root->setProperty("rmsDb", rmsDb);
    root->setProperty("peakDb", peakDb);
    root->setProperty("fundamental", fundamental);
    root->setProperty("peakFrequency", peakFrequency);
    root->setProperty("spectralCentroid", spectralCentroid);
    root->setProperty("spectralFlatness", spectralFlatness);
    root->setProperty("spectralRolloff", rolloffFreq);
    root->setProperty("crestFactor", crestFactor);
    root->setProperty("fftSize", N);
    root->setProperty("peaks", peaksArr);
    return juce::JSON::toString(juce::var(root), true);
}

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


// Compute native diagnostic graph facts under sys_lock(). Shared by standalone
// /pd/diagnose and inline conditional X-ray in batch_atomic.
juce::String MCPBridge::computeDiagnoseFacts(PluginProcessor* processor, t_canvas* cnv, const juce::String& canvasName)
{
    if (!processor || !cnv) return {};

    // 1. Objects: index, stable tempId (fallback: class#idx)
    std::vector<t_gobj*> objs;
    std::vector<juce::String> names;
    std::vector<juce::String> classNames;
    std::unordered_map<t_gobj*, juce::String> ptrToId;
    auto mapIt = processor->mcpStableObjectMap.find(canvasName.toStdString());
    if (mapIt != processor->mcpStableObjectMap.end())
        for (auto& [tid, ptr] : mapIt->second)
            if (ptr) ptrToId[ptr] = juce::String(tid);

    int idx = 0;
    for (t_gobj* y = cnv->gl_list; y; y = y->g_next, ++idx) {
        if (!y) continue;
        objs.push_back(y);
        t_class* cl = pd_class(&y->g_pd);
        const char* cName = cl ? class_getname(cl) : nullptr;
        classNames.push_back(juce::String(cName ? cName : ""));
        names.push_back(ptrToId.count(y)
            ? ptrToId[y]
            : (juce::String(cName ? cName : "unknown") + "#" + juce::String(idx)));
    }

    // 2. One linetraverser walk: signal edges + ALL wired inlets +
    //    rate-mismatched wires (PRD §2.2 `mismatched`)
    std::vector<std::pair<int, int>> sigEdges;         // (srcIdx, destIdx), signal only
    std::set<std::pair<int, int>> anyInletWired;       // (destIdx, inletNo), any type
    std::set<std::pair<int, int>> anyOutletWired;      // (srcIdx, outletNo), any type
    struct MismatchedWire { int src, srcOut, dest, destIn; bool srcIsSig; };
    std::vector<MismatchedWire> mismatchedWires;
    t_linetraverser lt;
    t_outconnect* oc = nullptr;
    linetraverser_start(&lt, cnv);
    while ((oc = linetraverser_next_nosize(&lt))) {
        int si = -1, di = -1;
        for (int i = 0; i < (int)objs.size(); ++i) {
            if (objs[i] == &lt.tr_ob->ob_g) si = i;
            if (objs[i] == &lt.tr_ob2->ob_g) di = i;
        }
        if (si < 0 || di < 0) continue;
        anyInletWired.insert({ di, lt.tr_inno });
        anyOutletWired.insert({ si, lt.tr_outno });
        bool srcIsSig = (lt.tr_outlet->o_sym == gensym("signal"));
        bool destIsSig = obj_issignalinlet(lt.tr_ob2, lt.tr_inno);
        if (srcIsSig != destIsSig)
            mismatchedWires.push_back({ si, lt.tr_outno, di, lt.tr_inno, srcIsSig });
        if (srcIsSig)
            sigEdges.push_back({ si, di });
    }

    // 3. DSP cycles: DFS over signal edges (white/gray/black)
    std::vector<std::vector<int>> adj(objs.size());
    for (auto& e : sigEdges) adj[e.first].push_back(e.second);
    std::vector<int> color(objs.size(), 0);
    std::vector<int> path;
    std::vector<std::vector<int>> cycles;
    std::function<void(int)> dfs = [&](int u) {
        color[u] = 1;
        path.push_back(u);
        for (int v : adj[u]) {
            if (color[v] == 1) {
                std::vector<int> cyc;
                for (int k = (int)path.size() - 1; k >= 0; --k) {
                    cyc.push_back(path[k]);
                    if (path[k] == v) break;
                }
                std::reverse(cyc.begin(), cyc.end());
                cycles.push_back(cyc);
            } else if (color[v] == 0) {
                dfs(v);
            }
        }
        path.pop_back();
        color[u] = 2;
    };
    for (int i = 0; i < (int)objs.size(); ++i)
        if (color[i] == 0) dfs(i);

    // 4. Main-signal-inlet check + zeroed-VCA check
    std::set<std::pair<int, int>> mainSigUnwired; // (idx, 0)
    std::set<std::pair<int, int>> zeroedVcas;     // (idx, 1)
    idx = 0;
    for (t_gobj* y = cnv->gl_list; y; y = y->g_next, ++idx) {
        if (!y) continue;
        t_object* ob = pd::Interface::checkObject(y);
        bool hasDspMethod = (ob != nullptr && zgetfn(&y->g_pd, gensym("dsp")) != nullptr);
        bool mainWired = anyInletWired.count({ idx, 0 }) > 0;
        if (hasDspMethod && ob && obj_issignalinlet(ob, 0) && !mainWired)
            mainSigUnwired.insert({ idx, 0 });

        if (!ob) continue;
        char* tb = nullptr; int tsz = 0;
        pd::Interface::getObjectText(ob, &tb, &tsz);
        juce::String text = (tb && tsz > 0)
            ? juce::String::fromUTF8(tb, static_cast<size_t>(tsz)).trim()
            : juce::String();
        if (tb) freebytes(tb, static_cast<size_t>(tsz) * sizeof(char));

        juce::String firstTok = text.upToFirstOccurrenceOf(" ", false, false);
        if (firstTok == "*~") {
            // Zeroed-VCA detection: multiplier is silent if it has NO float arg
            // and no inlet-1 wire (bare [*~]), OR an explicit arg of exactly 0
            // ([*~ 0] — a numeric arg parses as zero and locks the gain to silence).
            bool hasNonZeroFloatArg = false;
            bool hasZeroFloatArg = false;
            juce::String argTok = text
                .fromFirstOccurrenceOf(" ", false, false)
                .upToFirstOccurrenceOf(" ", false, false)
                .trim();
            if (argTok.isNotEmpty()) {
                char* endPtr = nullptr;
                const char* raw = argTok.toRawUTF8();
                double argVal = std::strtod(raw, &endPtr);
                if (endPtr != raw) { // numeric atom (symbol args like '*~' don't count)
                    if (argVal == 0.0) hasZeroFloatArg = true;
                    else hasNonZeroFloatArg = true;
                }
            }
            bool inlet1Wired = anyInletWired.count({ idx, 1 }) > 0;
            if ((!hasNonZeroFloatArg && !inlet1Wired) || (hasZeroFloatArg && !inlet1Wired))
                zeroedVcas.insert({ idx, 1 });
        }
    }

    // 4b. Structural completeness: orphans (zero wires on both sides) + dead-ends
    //     (inlet wired, output discarded). Wireless classes connect via send/receive
    //     SYMBOLS (no wire), GUI objects connect via their send/receive PROPERTIES,
    //     and comment/text/inlet/outlet are never wireable — all excluded, so the
    //     check fires only on a genuinely-dead DSP/control object (a forgotten wire),
    //     never on a legitimate wireless or cosmetic one.
    auto isWirelessOrCosmetic = [](const juce::String& c) {
        static const char* skip[] = {
            // wireless (symbol-addressed, no wire needed)
            "s", "r", "send", "receive", "s~", "r~", "send~", "receive~", "throw~", "catch~",
            // cosmetic / structural (never wireable)
            "comment", "text", "cnv", "canvas", "pd", "graph",
            // subpatch boundary ports
            "inlet", "inlet~", "outlet", "outlet~",
            // GUI (connect via send/receive properties, not wires)
            "bng", "tgl", "hsl", "vsl", "hradio", "vradio", "numbox", "floatatom", "symbolatom", "vu"
        };
        for (auto* k : skip) if (c == k) return true;
        return false;
    };
    auto isSink = [](const juce::String& c) {
        return c == "dac~" || c == "print" || c == "outlet" || c == "outlet~";
    };
    std::set<int> orphans;
    std::set<int> deadEnds;
    for (int i = 0; i < (int)objs.size(); ++i) {
        if (isWirelessOrCosmetic(classNames[(size_t)i])) continue;
        // Container (inline [pd] subpatch OR file-backed abstraction): a 0-port
        // container can still be alive via an internal [loadbang]/[metro]/[r], and
        // we cannot see inside from here — so NEVER flag a container as an orphan.
        // canvas_class covers BOTH; class-name strings miss abstractions, whose
        // class name is the abstraction's own name (not "pd").
        if (pd_class(&objs[(size_t)i]->g_pd) == canvas_class) continue;
        t_object* ob = pd::Interface::checkObject(objs[(size_t)i]);
        if (!ob) continue;
        int nin = obj_ninlets(ob), nout = obj_noutlets(ob);
        if (nin + nout <= 0) continue;
        bool hasIn = false, hasOut = false;
        for (auto& p : anyInletWired) if (p.first == i) { hasIn = true; break; }
        for (auto& p : anyOutletWired) if (p.first == i) { hasOut = true; break; }
        if (!hasIn && !hasOut) orphans.insert(i);
        else if (hasIn && !hasOut && nout > 0 && !isSink(classNames[(size_t)i])) deadEnds.insert(i);
    }

    // 5. Build JSON — cycles, unscheduled, dangling, zeroed
    auto nameOf = [&](int i) { return (i >= 0 && i < (int)names.size()) ? names[(size_t)i] : juce::String("?"); };
    juce::String json = "{\"canvas\":\"" + canvasName + "\",\"objectCount\":" + juce::String(objs.size());
    json += ",\"dsp_cycles\":[";
    for (size_t c = 0; c < cycles.size(); ++c) {
        if (c > 0) json += ",";
        json += "[";
        for (size_t k = 0; k < cycles[c].size(); ++k) {
            if (k > 0) json += ",";
            json += "\"" + nameOf(cycles[c][k]) + "\"";
        }
        json += "]";
    }
    json += "],\"unscheduled\":[";
    {
        std::set<int> unscheduled;
        for (auto& cyc : cycles) for (int m : cyc) unscheduled.insert(m);
        bool first = true;
        for (int m : unscheduled) {
            if (!first) json += ",";
            json += "\"" + nameOf(m) + "\"";
            first = false;
        }
    }
    json += "],\"dangling_main_sig\":[";
    {
        bool first = true;
        for (auto& z : mainSigUnwired) {
            if (!first) json += ",";
            json += "\"" + nameOf(z.first) + "\"";
            first = false;
        }
    }
    json += "],\"zeroed_vcgs\":[";
    {
        bool first = true;
        for (auto& z : zeroedVcas) {
            if (!first) json += ",";
            json += "{\"tempId\":\"" + nameOf(z.first) + "\",\"inlet\":" + juce::String(z.second) + "}";
            first = false;
        }
    }
    json += "],\"mismatched\":[";
    {
        bool first = true;
        for (auto& w : mismatchedWires) {
            if (!first) json += ",";
            json += "{\"srcId\":\"" + nameOf(w.src) + "\",\"srcOut\":" + juce::String(w.srcOut)
                  + ",\"destId\":\"" + nameOf(w.dest) + "\",\"destIn\":" + juce::String(w.destIn)
                  + ",\"dir\":\"" + (w.srcIsSig ? "sig->ctl" : "ctl->sig") + "\"}";
            first = false;
        }
    }
    json += "],\"orphans\":[";
    {
        bool first = true;
        for (int o : orphans) {
            if (!first) json += ",";
            json += "\"" + nameOf(o) + "\"";
            first = false;
        }
    }
    json += "],\"dead_ends\":[";
    {
        bool first = true;
        for (int o : deadEnds) {
            if (!first) json += ",";
            json += "\"" + nameOf(o) + "\"";
            first = false;
        }
    }
    json += "]}";
    return json;
}

MCPBridge::MasterMeterResult MCPBridge::computeMasterMeter(PluginProcessor* processor, t_canvas* cnv, const juce::String& canvasName)
{
    MasterMeterResult res;
    if (!processor || !cnv) return res;

    // Search for master sinks on this canvas in priority order:
    // 1. [dac~]
    // 2. [throw~]
    // 3. [catch~]
    // 4. [out~]
    std::vector<t_object*> dacObjs;
    std::vector<t_object*> throwObjs;
    std::vector<t_object*> catchObjs;
    std::vector<t_object*> outObjs;

    for (t_gobj* y = cnv->gl_list; y; y = y->g_next) {
        if (!y) continue;
        t_object* ob = pd::Interface::checkObject(y);
        if (!ob) continue;
        t_class* cl = pd_class(&y->g_pd);
        const char* cName = cl ? class_getname(cl) : nullptr;
        if (!cName) continue;
        juce::String cStr(cName);
        if (cStr == "dac~") dacObjs.push_back(ob);
        else if (cStr == "throw~") throwObjs.push_back(ob);
        else if (cStr == "catch~") catchObjs.push_back(ob);
        else if (cStr == "out~") outObjs.push_back(ob);
    }

    std::vector<t_object*> targetSinks;
    bool isOutgoingCatch = false;
    if (!dacObjs.empty()) {
        targetSinks = dacObjs;
        res.masterType = "dac~";
        res.masterFound = true;
    } else if (!throwObjs.empty()) {
        targetSinks = throwObjs;
        res.masterType = "throw~";
        res.masterFound = true;
    } else if (!catchObjs.empty()) {
        targetSinks = catchObjs;
        res.masterType = "catch~";
        res.masterFound = true;
        isOutgoingCatch = true;
    } else if (!outObjs.empty()) {
        targetSinks = outObjs;
        res.masterType = "out~";
        res.masterFound = true;
    } else {
        res.masterFound = false;
        res.masterType = "none";
        return res;
    }

    std::set<t_object*> sinkSet(targetSinks.begin(), targetSinks.end());

    // Traverse all wires and measure signal on connections to/from the target sinks
    float maxPeak = 0.0f;
    double totalSumSq = 0.0;
    int64_t totalSamples = 0;

    t_linetraverser lt;
    t_outconnect* oc = nullptr;
    linetraverser_start(&lt, cnv);
    while ((oc = linetraverser_next_nosize(&lt))) {
        bool match = false;
        if (isOutgoingCatch) {
            if (sinkSet.count(lt.tr_ob) && lt.tr_outno == 0) {
                match = true;
            }
        } else {
            if (sinkSet.count(lt.tr_ob2)) {
                match = true;
            }
        }

        if (!match) continue;

        t_signal* sig = outconnect_get_signal(oc);
        if (sig && sig->s_vec && sig->s_n > 0) {
            for (int i = 0; i < sig->s_n; ++i) {
                float s = sig->s_vec[i];
                float abs_s = std::abs(s);
                totalSumSq += (double)(s * s);
                if (abs_s > maxPeak) maxPeak = abs_s;
            }
            totalSamples += sig->s_n;
        }
    }

    float rms = (totalSamples > 0) ? (float)std::sqrt(totalSumSq / (double)totalSamples) : 0.0f;
    res.rmsDb = (rms > 1e-7f) ? (20.0f * std::log10(rms)) : -100.0f;
    res.peakDb = (maxPeak > 1e-7f) ? (20.0f * std::log10(maxPeak)) : -100.0f;

    return res;
}

juce::String MCPBridge::computeSignalTrace(PluginProcessor* processor, t_canvas* cnv, const juce::String& canvasName)
{
    if (!processor || !cnv) return "{\"error\":\"canvas not available\"}";

    // 1. Map objects and tempIds
    std::vector<t_gobj*> objs;
    std::vector<juce::String> names;
    std::vector<juce::String> classNames;
    std::unordered_map<t_gobj*, juce::String> ptrToId;
    auto mapIt = processor->mcpStableObjectMap.find(canvasName.toStdString());
    if (mapIt != processor->mcpStableObjectMap.end())
        for (auto& [tid, ptr] : mapIt->second)
            if (ptr) ptrToId[ptr] = juce::String(tid);

    int idx = 0;
    for (t_gobj* y = cnv->gl_list; y; y = y->g_next, ++idx) {
        if (!y) continue;
        objs.push_back(y);
        t_class* cl = pd_class(&y->g_pd);
        const char* cName = cl ? class_getname(cl) : nullptr;
        juce::String clStr = cName ? juce::String(cName) : juce::String("unknown");
        classNames.push_back(clStr);
        names.push_back(ptrToId.count(y)
            ? ptrToId[y]
            : (clStr + "#" + juce::String(idx)));
    }

    int numObjs = (int)objs.size();
    std::vector<float> maxInPeak(numObjs, 0.0f);
    std::vector<float> maxOutPeak(numObjs, 0.0f);
    std::vector<int> incomingSigWires(numObjs, 0);
    std::vector<int> outgoingSigWires(numObjs, 0);
    std::vector<std::unordered_map<int, float>> inletPeaks(numObjs);
    std::vector<std::unordered_map<int, std::vector<int>>> inletSources(numObjs);

    t_linetraverser lt;
    t_outconnect* oc = nullptr;
    linetraverser_start(&lt, cnv);
    while ((oc = linetraverser_next_nosize(&lt))) {
        int si = -1, di = -1;
        for (int i = 0; i < numObjs; ++i) {
            if (objs[i] == &lt.tr_ob->ob_g) si = i;
            if (objs[i] == &lt.tr_ob2->ob_g) di = i;
        }
        if (si < 0 || di < 0) continue;

        bool isSig = (lt.tr_outlet->o_sym == gensym("signal"));
        if (!isSig) continue;

        float wirePeak = 0.0f;
        t_signal* sig = outconnect_get_signal(oc);
        if (sig && sig->s_vec && sig->s_n > 0) {
            for (int k = 0; k < sig->s_n; ++k) {
                float val = std::abs(sig->s_vec[k]);
                if (val > wirePeak) wirePeak = val;
            }
        }

        outgoingSigWires[si]++;
        if (wirePeak > maxOutPeak[si]) maxOutPeak[si] = wirePeak;

        incomingSigWires[di]++;
        if (wirePeak > maxInPeak[di]) maxInPeak[di] = wirePeak;
        inletPeaks[di][lt.tr_inno] = std::max(inletPeaks[di][lt.tr_inno], wirePeak);
        inletSources[di][lt.tr_inno].push_back(si);
    }

    constexpr float ALIVE_THRESH = 1e-4f; // -80 dBFS
    std::vector<juce::String> liveSources;
    std::vector<juce::String> silentGenerators;
    std::vector<juce::String> silentEnvelopes;
    float loudestLivePeak = 0.0f; // peak of the loudest live source — real "input" level

    int dacIndex = -1;
    for (int i = 0; i < numObjs; ++i) {
        const juce::String& c = classNames[i];
        if (c == "dac~") dacIndex = i;

        bool isGen = (c == "osc~" || c == "phasor~" || c == "noise~" || c == "sig~" ||
                      c == "tabread4~" || c == "tabread~" || c == "tabplay~" || c == "adc~");
        bool isEnv = (c == "vline~" || c == "line~" || c == "adsr~" || c == "envgen~");

        if (isGen) {
            if (maxOutPeak[i] >= ALIVE_THRESH) {
                liveSources.push_back(names[i]);
                loudestLivePeak = std::max(loudestLivePeak, maxOutPeak[i]);
            } else if (outgoingSigWires[i] > 0) {
                silentGenerators.push_back(names[i]);
            }
        } else if (isEnv) {
            if (maxOutPeak[i] < ALIVE_THRESH && outgoingSigWires[i] > 0) {
                silentEnvelopes.push_back(names[i]);
            }
        } else if (incomingSigWires[i] == 0 && outgoingSigWires[i] > 0 && maxOutPeak[i] >= ALIVE_THRESH) {
            liveSources.push_back(names[i]);
            loudestLivePeak = std::max(loudestLivePeak, maxOutPeak[i]);
        }
    }

    int chokeIndex = -1;
    juce::String chokeReason;

    for (int i = 0; i < numObjs; ++i) {
        const juce::String& c = classNames[i];
        if (c == "dac~" || c == "throw~" || c == "out~") continue;

        if (maxInPeak[i] >= ALIVE_THRESH && maxOutPeak[i] < ALIVE_THRESH) {
            chokeIndex = i;
            if (c == "*~") {
                float inlet1Signal = inletPeaks[i].count(1) ? inletPeaks[i][1] : 0.0f;
                if (inletSources[i].count(1) && !inletSources[i][1].empty()) {
                    int srcI = inletSources[i][1][0];
                    if (inlet1Signal < ALIVE_THRESH) {
                        chokeReason = "multiplier control inlet 1 is zero from envelope '" + names[srcI] + "' (envelope did not trigger or [r gate] missing)";
                    } else {
                        chokeReason = "multiplier output is silent despite control signal on inlet 1";
                    }
                } else {
                    chokeReason = "multiplier control inlet 1 is zero or disconnected (silent VCA)";
                }
            } else if (c == "vcf~" || c == "lop~" || c == "hip~" || c == "bp~") {
                chokeReason = "filter attenuated signal to silence (cutoff may be at 0 Hz)";
            } else {
                float inDb = 20.0f * std::log10(maxInPeak[i] + 1e-7f);
                float outDb = 20.0f * std::log10(maxOutPeak[i] + 1e-7f);
                chokeReason = "signal died at this object (input " + juce::String(inDb, 1) + " dBFS, output " + juce::String(outDb, 1) + " dBFS)";
            }
            break;
        }
    }

    if (chokeIndex < 0) {
        if (!silentGenerators.empty() && liveSources.empty()) {
            chokeReason = "sound generator '" + silentGenerators[0] + "' is silent (check frequency or pitch receiver)";
            for (int i = 0; i < numObjs; ++i) {
                if (names[i] == silentGenerators[0]) { chokeIndex = i; break; }
            }
        } else if (!silentEnvelopes.empty() && liveSources.empty()) {
            chokeReason = "envelope generator '" + silentEnvelopes[0] + "' output is zero (envelope did not trigger)";
            for (int i = 0; i < numObjs; ++i) {
                if (names[i] == silentEnvelopes[0]) { chokeIndex = i; break; }
            }
        } else if (!liveSources.empty()) {
            if (dacIndex >= 0 && maxInPeak[dacIndex] < ALIVE_THRESH) {
                chokeReason = "signal generated by '" + liveSources[0] + "' never reaches dac~ (broken signal path)";
                chokeIndex = dacIndex;
            } else {
                // Healthy: a live source exists AND the signal is reaching the
                // master sink. Previously this branch emitted the alarming
                // "sound generated but not reaching master sink" reason even
                // though the dac~ was fed — a false choke report.
                chokeReason = "signal reaches master sink";
            }
        } else {
            chokeReason = "no active audio sources found on canvas";
        }
    }

    juce::String chokeName = (chokeIndex >= 0 && chokeIndex < numObjs) ? names[chokeIndex] : "none";
    juce::String chokeClass = (chokeIndex >= 0 && chokeIndex < numObjs) ? classNames[chokeIndex] : "none";
    // Report real levels. When there is no choke point, fall back to the
    // loudest live source (input) and the peak arriving at dac~ (output) so
    // the trace agrees with /meter/master instead of a hardcoded -100 dBFS.
    float inDb = -100.0f;
    float outDb = -100.0f;
    if (chokeIndex >= 0 && chokeIndex < numObjs) {
        inDb = maxInPeak[chokeIndex] > 1e-7f ? (20.0f * std::log10(maxInPeak[chokeIndex])) : -100.0f;
        outDb = maxOutPeak[chokeIndex] > 1e-7f ? (20.0f * std::log10(maxOutPeak[chokeIndex])) : -100.0f;
    } else if (dacIndex >= 0 && dacIndex < numObjs) {
        inDb = loudestLivePeak > 1e-7f ? (20.0f * std::log10(loudestLivePeak)) : -100.0f;
        outDb = maxInPeak[dacIndex] > 1e-7f ? (20.0f * std::log10(maxInPeak[dacIndex])) : -100.0f;
    }

    juce::String json = "{";
    json += "\"canvas\":\"" + canvasName + "\",";
    json += "\"chokePoint\":\"" + chokeName + "\",";
    json += "\"class\":\"" + chokeClass + "\",";
    json += "\"inputLevelDb\":" + juce::String(inDb, 1) + ",";
    json += "\"outputLevelDb\":" + juce::String(outDb, 1) + ",";
    json += "\"reason\":\"" + chokeReason.replace("\"", "\\\"") + "\",";
    json += "\"liveSources\":[";
    for (size_t i = 0; i < liveSources.size(); ++i) {
        if (i > 0) json += ",";
        json += "\"" + liveSources[i] + "\"";
    }
    json += "],\"silentEnvelopes\":[";
    for (size_t i = 0; i < silentEnvelopes.size(); ++i) {
        if (i > 0) json += ",";
        json += "\"" + silentEnvelopes[i] + "\"";
    }
    json += "]}";

    return json;
}

// ── /pd/clusters — context grouping for layout ──────────────────────────
// Connected components of the wire graph (signal + control), classified by
// kind. One union-find pass over gl_list connections — read-only, fast.
// Feeds context-aware layout (see PRD_CONTEXT_LAYOUT_GUARD.md).
// Shared: find the JUCE Canvas component for a Pd canvas (message thread).
static Canvas* mcpFindGuiCanvasFor(PluginProcessor* proc, t_canvas* c)
{
    if (!proc || !c) return nullptr;
    for (auto* editor : proc->getEditors()) {
        if (!editor) continue;
        for (auto* cnvItem : editor->getCanvases()) {
            if (cnvItem && cnvItem->patch.getPointer().get() == c) return cnvItem;
        }
    }
    return nullptr;
}

// Shared true-rect bounds (GUI-aware). MUST run on the JUCE message thread
// (reads Canvas object components). Used by the layout X-ray AND the inline
// layout guard so both agree on geometry.
static void mcpGetTrueObjectBounds(t_canvas* c, t_gobj* y, Canvas* guiCanvas, int* x, int* yy, int* w, int* h)
{
    *x = 0; *yy = 0; *w = 0; *h = 0;
    pd::Interface::getObjectBounds(c, y, x, yy, w, h);

    if (guiCanvas) {
        for (auto* obj : guiCanvas->objects) {
            if (obj && obj->getPointer() == y) {
                auto b = obj->getSelectableBounds();
                if (b.getWidth() > 0 && b.getHeight() > 0) {
                    *w = b.getWidth();
                    *h = b.getHeight();
                    return;
                }
                break;
            }
        }
    }

    if (pd::Interface::isTextObject(y) && (*w <= 10 || *h <= 10)) {
        t_text* textObj = reinterpret_cast<t_text*>(y);
        if (textObj && textObj->te_binbuf) {
            char* textBuf = nullptr;
            int textSize = 0;
            binbuf_gettext(textObj->te_binbuf, &textBuf, &textSize);
            if (textBuf && textSize > 0) {
                int fontWidth = glist_fontwidth(c);
                int fontHeight = glist_fontheight(c);
                if (fontWidth <= 0) fontWidth = 7;
                if (fontHeight <= 0) fontHeight = 14;
                juce::String fullText = juce::String::fromUTF8(textBuf, textSize).trim();
                auto lines = juce::StringArray::fromLines(fullText);
                int maxLineLen = 0;
                for (const auto& line : lines) maxLineLen = std::max(maxLineLen, line.length());
                int textW = maxLineLen * fontWidth + 12;
                int textH = std::max(1, lines.size()) * fontHeight + 7;
                if (*w <= 10) *w = textW;
                if (*h <= 10) *h = textH;
                freebytes(textBuf, textSize);
            }
        }
    }

    if (*w <= 0) *w = 60;
    if (*h <= 0) *h = 20;
}

// True-rect collision check (message thread) — same measurement as the
// /pd/collisions X-ray, so the guard and the verifier agree.
static bool mcpHasCollisions(PluginProcessor* processor, t_canvas* cnv, int pad)
{
    if (!processor || !cnv) return false;
    Canvas* gui = mcpFindGuiCanvasFor(processor, cnv);
    std::vector<t_gobj*> objs;
    for (t_gobj* y = cnv->gl_list; y; y = y->g_next) objs.push_back(y);
    struct R { int x, y, w, h; };
    std::vector<R> rects(objs.size());
    for (size_t i = 0; i < objs.size(); ++i) {
        int x = 0, y = 0, w = 0, h = 0;
        mcpGetTrueObjectBounds(cnv, objs[i], gui, &x, &y, &w, &h);
        rects[i] = { x, y, w, h };
    }
    for (size_t i = 0; i < rects.size(); ++i)
        for (size_t j = i + 1; j < rects.size(); ++j) {
            auto& a = rects[i]; auto& b = rects[j];
            if (a.x < b.x + b.w + pad && a.x + a.w + pad > b.x
             && a.y < b.y + b.h + pad && a.y + a.h + pad > b.y) return true;
        }
    return false;
}

juce::String MCPBridge::computeClusters(PluginProcessor* processor, t_canvas* cnv, const juce::String& canvasName)
{
    if (!processor || !cnv) return "{\"clusters\":[]}";

    std::vector<t_gobj*> objs;
    std::vector<juce::String> names;
    std::vector<juce::String> classNames;
    std::unordered_map<t_gobj*, int> ptrToIdx;

    std::unordered_map<t_gobj*, juce::String> ptrToId;
    auto mapIt = processor->mcpStableObjectMap.find(canvasName.toStdString());
    if (mapIt != processor->mcpStableObjectMap.end())
        for (auto& [tid, ptr] : mapIt->second)
            if (ptr) ptrToId[ptr] = juce::String(tid);

    int idx = 0;
    for (t_gobj* y = cnv->gl_list; y; y = y->g_next, ++idx) {
        ptrToIdx[y] = static_cast<int>(objs.size());
        objs.push_back(y);
        t_class* cl = pd_class(&y->g_pd);
        const char* cName = cl ? class_getname(cl) : nullptr;
        juce::String clStr = cName ? juce::String::fromUTF8(cName) : juce::String("unknown");
        classNames.push_back(clStr);
        names.push_back(ptrToId.count(y) ? ptrToId[y] : (clStr + "#" + juce::String(idx)));
    }

    int n = static_cast<int>(objs.size());
    if (n == 0) return "{\"clusters\":[]}";

    std::vector<int> parent(n);
    for (int i = 0; i < n; ++i) parent[i] = i;
    auto findRoot = [&](int a) {
        while (parent[a] != a) { parent[a] = parent[parent[a]]; a = parent[a]; }
        return a;
    };
    auto unite = [&](int a, int b) { int ra = findRoot(a), rb = findRoot(b); if (ra != rb) parent[ra] = rb; };

    t_linetraverser lt;
    t_outconnect* oc = nullptr;
    linetraverser_start(&lt, cnv);
    while ((oc = linetraverser_next_nosize(&lt))) {
        auto si = ptrToIdx.find(&lt.tr_ob->ob_g);
        auto di = ptrToIdx.find(&lt.tr_ob2->ob_g);
        if (si != ptrToIdx.end() && di != ptrToIdx.end()) unite(si->second, di->second);
    }

    std::unordered_map<int, std::vector<int>> groups;
    for (int i = 0; i < n; ++i) groups[findRoot(i)].push_back(i);

    juce::String json = "{\"clusters\":[";
    bool firstC = true;
    int cid = 0;
    for (auto& kv : groups) {
        auto& members = kv.second;
        bool hasSignal = false, hasBus = false;
        for (int m : members) {
            const juce::String& c = classNames[m];
            if (c.endsWithChar('~')) hasSignal = true;
            if (c == "catch~" || c == "dac~" || c == "out~") hasBus = true;
        }
        juce::String kind = hasBus ? "bus" : (hasSignal ? "signal" : "control");
        if (!firstC) json += ",";
        firstC = false;
        json += "{\"id\":\"c" + juce::String(++cid) + "\",\"kind\":\"" + kind + "\",\"objects\":[";
        for (size_t mi = 0; mi < members.size(); ++mi) {
            if (mi > 0) json += ",";
            json += "\"" + names[members[mi]] + "\"";
        }
        json += "]}";
    }
    json += "]}";
    return json;
}

// ── sanitizeLayout — inline minimal-deoverlap post-guard ────────────────
// Same algorithm as /pd/deoverlap (PAD 5, 10px snap, minimal-axis push) but
// using Pd-only bounds so it is safe to call inline under sys_lock from the
// batch_atomic path. Returns the number of objects moved.
int MCPBridge::sanitizeLayout(PluginProcessor* processor, t_canvas* cnv, int pad, int snap)
{
    if (!processor || !cnv) return 0;
    if (pad < 0) pad = 0;
    if (snap < 1) snap = 1;

    std::vector<t_gobj*> objs;
    for (t_gobj* y = cnv->gl_list; y; y = y->g_next) objs.push_back(y);
    if (objs.size() < 2) return 0;

    struct DR { t_gobj* g; int x, y, w, h; };
    std::vector<DR> rects;
    rects.reserve(objs.size());
    for (auto* g : objs) {
        int x = 0, y = 0, w = 0, h = 0;
        pd::Interface::getObjectBounds(cnv, g, &x, &y, &w, &h);
        rects.push_back({ g, x, y, w, h });
    }

    const int PAD = pad;
    const int MAXPASS = 24;
    auto snapTo = [snap](int v) { return (v / snap) * snap; };
    for (int pass = 0; pass < MAXPASS; pass++) {
        bool anyHit = false;
        for (size_t i = 0; i < rects.size(); i++) {
            for (size_t j = i + 1; j < rects.size(); j++) {
                auto& a = rects[i];
                auto& b = rects[j];
                bool hit = a.x < b.x + b.w + PAD && a.x + a.w + PAD > b.x
                        && a.y < b.y + b.h + PAD && a.y + a.h + PAD > b.y;
                if (!hit) continue;
                anyHit = true;
                int overlapX = std::min(a.x + a.w + PAD - b.x, b.x + b.w + PAD - a.x);
                int overlapY = std::min(a.y + a.h + PAD - b.y, b.y + b.h + PAD - a.y);
                if (overlapX <= overlapY) {
                    int acx = a.x + a.w / 2, bcx = b.x + b.w / 2;
                    int dx = std::max(snap, snapTo(overlapX + snap - 1));
                    b.x += (bcx >= acx ? dx : -dx);
                } else {
                    int acy = a.y + a.h / 2, bcy = b.y + b.h / 2;
                    int dy = std::max(snap, snapTo(overlapY + snap - 1));
                    b.y += (bcy >= acy ? dy : -dy);
                }
            }
        }
        if (!anyHit) break;
    }

    int moved = 0;
    for (auto& r : rects) {
        int ox = 0, oy = 0, ow = 0, oh = 0;
        pd::Interface::getObjectBounds(cnv, r.g, &ox, &oy, &ow, &oh);
        if (r.x != ox || r.y != oy) {
            pd::Interface::moveObject(cnv, r.g, r.x, r.y);
            moved++;
        }
    }
    if (moved > 0) canvas_dirty(cnv, 1);
    return moved;
}

// ── fixOcclusions — nudge boxes off wire paths ──────────────────────────
// Anchor-line vs box test (Liang-Barsky, same model as /pd/wire_occlusions).
// For each wire that passes through a box, shift that box horizontally (the
// minimal snapped amount) off the wire's x at the box's vertical centre.
// Bounded passes; geometry-only, no DSP touch.
int MCPBridge::fixOcclusions(PluginProcessor* processor, t_canvas* cnv, int pad, int snap)
{
    if (!processor || !cnv) return 0;
    if (snap < 1) snap = 1;

    auto lineHitsBox = [](double x1, double y1, double x2, double y2,
                          double bx, double by, double bw, double bh) {
        double minX = std::min(x1, x2), maxX = std::max(x1, x2);
        double minY = std::min(y1, y2), maxY = std::max(y1, y2);
        if (maxX < bx || minX > bx + bw || maxY < by || minY > by + bh) return false;
        double dx = x2 - x1, dy = y2 - y1, t0 = 0.0, t1 = 1.0;
        double p[4] = { -dx, dx, -dy, dy };
        double q[4] = { x1 - bx, bx + bw - x1, y1 - by, by + bh - y1 };
        for (int i = 0; i < 4; ++i) {
            if (p[i] == 0) { if (q[i] < 0) return false; }
            else {
                double t = q[i] / p[i];
                if (p[i] < 0) { if (t > t1) return false; if (t > t0) t0 = t; }
                else { if (t < t0) return false; if (t < t1) t1 = t; }
            }
        }
        return t0 < t1 && t0 > 0.05 && t1 < 0.95;
    };

    int totalMoved = 0;
    for (int pass = 0; pass < 3; pass++) {
        std::vector<t_gobj*> objs;
        for (t_gobj* y = cnv->gl_list; y; y = y->g_next) objs.push_back(y);
        if (objs.size() < 3) break;

        struct R { int x, y, w, h; };
        std::vector<R> rects(objs.size());
        std::unordered_map<t_gobj*, int> ptrToIdx;
        for (size_t i = 0; i < objs.size(); ++i) {
            int x = 0, yy = 0, w = 0, h = 0;
            pd::Interface::getObjectBounds(cnv, objs[i], &x, &yy, &w, &h);
            rects[i] = { x, yy, w, h };
            ptrToIdx[objs[i]] = static_cast<int>(i);
        }

        std::vector<std::pair<int, int>> edges;
        t_linetraverser lt;
        t_outconnect* oc = nullptr;
        linetraverser_start(&lt, cnv);
        while ((oc = linetraverser_next_nosize(&lt))) {
            auto si = ptrToIdx.find(&lt.tr_ob->ob_g);
            auto di = ptrToIdx.find(&lt.tr_ob2->ob_g);
            if (si == ptrToIdx.end() || di == ptrToIdx.end()) continue;
            edges.emplace_back(si->second, di->second);
        }

        auto snapUp = [snap](int v) { return ((v + snap - 1) / snap) * snap; };
        bool anyMove = false;
        for (auto& e : edges) {
            int si = e.first, di = e.second;
            double x1 = rects[si].x + rects[si].w / 2.0;
            double y1 = rects[si].y + rects[si].h;
            double x2 = rects[di].x + rects[di].w / 2.0;
            double y2 = rects[di].y;
            for (size_t k = 0; k < objs.size(); ++k) {
                if ((int)k == si || (int)k == di) continue;
                if (!lineHitsBox(x1, y1, x2, y2, rects[k].x, rects[k].y, rects[k].w, rects[k].h)) continue;
                double wyc = rects[k].y + rects[k].h / 2.0;
                double t = (y2 != y1) ? (wyc - y1) / (y2 - y1) : 0.5;
                t = std::max(0.0, std::min(1.0, t));
                double wx = x1 + t * (x2 - x1);
                int bx = rects[k].x, bw = rects[k].w;
                int by = rects[k].y, bh = rects[k].h;
                // Shift perpendicular to the wire: horizontal-ish wire → move the
                // box vertically; vertical-ish wire → move it horizontally.
                double dxw = x2 - x1, dyw = y2 - y1;
                int newbx = bx, newby = by;
                if (std::abs(dxw) >= std::abs(dyw)) {
                    double wxc = bx + bw / 2.0;
                    double tw = (dxw != 0.0) ? (wxc - x1) / dxw : 0.5;
                    tw = std::max(0.0, std::min(1.0, tw));
                    double wy = y1 + tw * dyw;
                    int shiftUp   = static_cast<int>(std::ceil((by + bh + pad) - wy)); // >0 → move up
                    int shiftDown = static_cast<int>(std::ceil(wy + pad - by));        // >0 → move down
                    if (shiftUp > 0 && (shiftUp <= shiftDown || shiftDown <= 0))
                        newby = by - snapUp(shiftUp);
                    else if (shiftDown > 0)
                        newby = by + snapUp(shiftDown);
                } else {
                int shiftLeft  = static_cast<int>(std::ceil((bx + bw + pad) - wx)); // >0 → move left
                int shiftRight = static_cast<int>(std::ceil(wx + pad - bx));        // >0 → move right
                if (shiftLeft > 0 && (shiftLeft <= shiftRight || shiftRight <= 0))
                    newbx = bx - snapUp(shiftLeft);
                else if (shiftRight > 0)
                    newbx = bx + snapUp(shiftRight);
                }
                if (newbx != bx || newby != by) { rects[k].x = newbx; rects[k].y = newby; anyMove = true; totalMoved++; }
            }
        }
        if (!anyMove) break;

        for (size_t k = 0; k < objs.size(); ++k) {
            int ox = 0, oy = 0, ow = 0, oh = 0;
            pd::Interface::getObjectBounds(cnv, objs[k], &ox, &oy, &ow, &oh);
            if (rects[k].x != ox || rects[k].y != oy) pd::Interface::moveObject(cnv, objs[k], rects[k].x, rects[k].y);
        }
        canvas_dirty(cnv, 1);
    }
    return totalMoved;
}

// ── composeLayout — P4 role-based composition ───────────────────────────
// Arranges the canvas by cluster role (PRD_CONTEXT_LAYOUT_GUARD P4):
//   bus/mix cluster -> left edge column
//   synth cluster   -> one column per cluster (left->right), signal objects
//                      stacked top->bottom, controls/gui in a gutter above
// On-demand (like dagre/flow), never forced. Returns objects moved.
int MCPBridge::composeLayout(PluginProcessor* processor, t_canvas* cnv, const juce::String& canvasName, int pad, int snap)
{
    if (!processor || !cnv) return 0;
    if (snap < 1) snap = 1;

    std::vector<t_gobj*> objs;
    std::unordered_map<t_gobj*, int> idx;
    int i = 0;
    for (t_gobj* y = cnv->gl_list; y; y = y->g_next) { idx[y] = i++; objs.push_back(y); }
    int n = static_cast<int>(objs.size());
    if (n < 2) return 0;

    std::vector<juce::String> cls(n);
    std::vector<int> ox(n), oy(n), ow(n), oh(n);
    for (int k = 0; k < n; ++k) {
        pd::Interface::getObjectBounds(cnv, objs[k], &ox[k], &oy[k], &ow[k], &oh[k]);
        t_class* cl = pd_class(&objs[k]->g_pd);
        const char* c = cl ? class_getname(cl) : nullptr;
        cls[k] = c ? juce::String::fromUTF8(c) : juce::String("unknown");
    }

    // union-find over wires
    std::vector<int> parent(n);
    for (int k = 0; k < n; ++k) parent[k] = k;
    auto findRoot = [&](int a) { while (parent[a] != a) { parent[a] = parent[parent[a]]; a = parent[a]; } return a; };
    auto unite = [&](int a, int b) { int ra = findRoot(a), rb = findRoot(b); if (ra != rb) parent[ra] = rb; };
    t_linetraverser lt; t_outconnect* oc = nullptr;
    std::vector<std::pair<int, int>> edges;
    linetraverser_start(&lt, cnv);
    while ((oc = linetraverser_next_nosize(&lt))) {
        auto si = idx.find(&lt.tr_ob->ob_g);
        auto di = idx.find(&lt.tr_ob2->ob_g);
        if (si != idx.end() && di != idx.end()) { unite(si->second, di->second); edges.emplace_back(si->second, di->second); }
    }

    std::unordered_map<int, std::vector<int>> groups;
    for (int k = 0; k < n; ++k) groups[findRoot(k)].push_back(k);

    auto isSignal = [](const juce::String& c) {
        return c.endsWithChar('~') || c == "catch~" || c == "dac~" || c == "throw~" || c == "out~" || c == "pd" || c == "table";
    };
    auto isGui = [](const juce::String& c) {
        return c == "bng" || c == "tgl" || c == "hsl" || c == "vsl" || c == "hradio" || c == "vradio"
            || c == "numbox" || c == "nbx" || c == "floatatom" || c == "symbolatom" || c == "vu"
            || c == "knob" || c == "slider" || c == "radio" || c == "toggle";
    };
    auto isSynth = [](const juce::String& c) {
        return c == "osc~" || c == "phasor~" || c == "noise~" || c == "sig~" || c.startsWith("bl.")
            || c.startsWith("plaits") || c == "tabread4~" || c == "tabread~" || c == "tabplay~";
    };

    struct Cl { std::vector<int> members; juce::String role; int minx; };
    std::vector<Cl> clusters;
    for (auto& kv : groups) {
        Cl c; c.members = kv.second;
        bool bus = false, synth = false, sig = false, allGui = true;
        int mx = INT_MAX;
        for (int k : c.members) {
            const juce::String& cc = cls[k];
            // The master bus is catch~/dac~/out~ ONLY. throw~ is a voice SEND.
            if (cc == "catch~" || cc == "dac~" || cc == "out~") bus = true;
            if (isSynth(cc)) synth = true;
            if (isSignal(cc)) sig = true;
            if (!isGui(cc)) allGui = false;
            mx = std::min(mx, ox[k]);
        }
        c.role = bus ? "bus" : (synth ? "synth" : (allGui ? "gui" : (sig ? "signal" : "control")));
        c.minx = mx;
        clusters.push_back(c);
    }

    std::sort(clusters.begin(), clusters.end(), [](const Cl& a, const Cl& b) {
        if ((a.role == "bus") != (b.role == "bus")) return a.role == "bus";
        return a.minx < b.minx;
    });

    const int COL_W = 300;
    const int COL_X = 30;
    const int GUTTER_Y = 40;
    const int ROW_GAP = 60;
    const int SIGNAL_TOP = 160;

    int moved = 0;
    int col = 0;
    for (auto& c : clusters) {
        int cx = COL_X + col * COL_W;
        std::vector<int> ctrl, sigv;
        for (int k : c.members) {
            if (isSignal(cls[k])) sigv.push_back(k);
            else ctrl.push_back(k);
        }
        // controls/gui in the gutter, left→right, spaced by their width
        int gx = cx;
        for (int k : ctrl) {
            pd::Interface::moveObject(cnv, objs[k], gx, GUTTER_Y);
            moved++;
            gx += ow[k] + 20;
        }
        // signal objects stacked top→bottom in SIGNAL-FLOW order (topological:
        // sources first, then processors, then the send/sink), fallback to y.
        std::unordered_set<int> inC(sigv.begin(), sigv.end());
        std::unordered_map<int, int> indeg;
        for (int k : sigv) indeg[k] = 0;
        for (auto& e : edges) if (inC.count(e.first) && inC.count(e.second)) indeg[e.second]++;
        std::vector<int> order;
        std::vector<int> remaining = sigv;
        while (!remaining.empty()) {
            bool progress = false;
            for (auto it = remaining.begin(); it != remaining.end(); ) {
                if (indeg[*it] <= 0) {
                    int node = *it;
                    order.push_back(node);
                    for (auto& e : edges) if (e.first == node && inC.count(e.second)) indeg[e.second]--;
                    it = remaining.erase(it);
                    progress = true;
                } else ++it;
            }
            if (!progress) {
                std::sort(remaining.begin(), remaining.end(), [&](int a, int b) { return oy[a] < oy[b]; });
                order.insert(order.end(), remaining.begin(), remaining.end());
                break;
            }
        }
        int yy = SIGNAL_TOP;
        for (int k : order) {
            pd::Interface::moveObject(cnv, objs[k], cx, yy);
            moved++;
            yy += ROW_GAP;
        }
        col++;
    }

    if (moved > 0) canvas_dirty(cnv, 1);
    (void)pad; (void)snap;
    return moved;
}

void MCPBridge::oscMessageReceived(const juce::OSCMessage& message)
{
    auto addr = message.getAddressPattern().toString();
    while (addr.startsWith("/")) addr = addr.substring(1);
    auto parts = juce::StringArray::fromTokens(addr, "/", "");
    parts.removeEmptyStrings();

    if (parts.isEmpty()) return;

    // Any inbound message means the MCP server is alive and talking to us.
    noteServerActivity();

    auto const domain = parts[0];

    if (domain == "pd") {
        auto action = parts.size() > 1 ? parts[1] : "";
        handlePdDomain(action, message);
    } else if (domain == "param") {
        auto paramName = parts.size() > 1 ? parts[1] : "";
        handleParamDomain(paramName, message);
    } else if (domain == "trigger") {
        auto triggerAction = parts.size() > 1 ? parts[1] : "";
        handleTriggerDomain(triggerAction, message);
    } else if (domain == "telemetry") {
        auto telAction = parts.size() > 1 ? parts[1] : "";
        handleTelemetryDomain(telAction, message);
    } else if (domain == "array") {
        // /array/write|/array/read are hierarchical; /array ["stats", ...] is flat.
        auto arrayAction = parts.size() > 1 ? parts[1] : (message.size() > 0 ? getArgString(message[0]) : "");
        handleArrayDomain(arrayAction, message);
    } else if (domain == "bridge") {
        auto bridgeAction = parts.size() > 1 ? parts[1] : (message.size() > 0 ? getArgString(message[0]) : "");
        handleBridgeDomain(bridgeAction, message);
    } else if (domain == "morph") {
        auto morphAction = parts.size() > 1 ? parts[1] : (message.size() > 0 ? getArgString(message[0]) : "");
        handleMorphDomain(morphAction, message);
    } else if (domain == "meter") {
        auto meterAction = parts.size() > 1 ? parts[1] : (message.size() > 0 ? getArgString(message[0]) : "");
        handleMeterDomain(meterAction, message);
    } else if (domain == "transport") {
        auto action = parts.size() > 1 ? parts[1] : (message.size() > 0 ? getArgString(message[0]) : "");
        handleTransportDomain(action, message);
    } else if (domain == "seq") {
        auto action = parts.size() > 1 ? parts[1] : (message.size() > 0 ? getArgString(message[0]) : "");
        handleSeqDomain(action, message);
    }
}

void MCPBridge::oscBundleReceived(const juce::OSCBundle& bundle)
{
    for (auto const& elem : bundle) {
        if (elem.isMessage()) {
            oscMessageReceived(elem.getMessage());
        }
    }
}

// Helper to extract string from OSC argument
static juce::String getArgString(const juce::OSCArgument& arg)
{
    if (arg.isString()) return arg.getString();
    if (arg.isFloat32()) return juce::String(arg.getFloat32());
    if (arg.isInt32()) return juce::String(arg.getInt32());
    return {};
}

// Helper to extract float from OSC argument
static float getArgFloat(const juce::OSCArgument& arg)
{
    if (arg.isFloat32()) return arg.getFloat32();
    if (arg.isInt32()) return static_cast<float>(arg.getInt32());
    if (arg.isString()) return arg.getString().getFloatValue();
    return 0.0f;
}

// ─── PRD Phase 3 §2.2: identity sidecar applier ──────────────────────────
// Re-registers sidecar identities (root + named subcanvases) for a freshly
// loaded/opened canvas. Caller must hold sys_lock. Returns tempIds restored.
int MCPBridge::mcpApplyIdentitySidecar(PluginProcessor* processor, juce::DynamicObject* sidecarObj,
                                   const juce::String& rootKey, t_canvas* cnv)
{
    if (!processor || !sidecarObj || !cnv) return 0;

    auto registerArray = [&](juce::var const& idArray, juce::String const& mapKey, t_canvas* target) -> int {
        const auto* arr = idArray.getArray();
        if (arr == nullptr) return 0;
        std::unordered_map<int, juce::String> idxToId;
        for (auto const& v : *arr) {
            if (auto* o = v.getDynamicObject()) {
                int i = static_cast<int>(static_cast<double>(o->getProperty("i")));
                juce::String id = o->getProperty("id").toString();
                if (id.isNotEmpty()) idxToId[i] = id;
            }
        }
        int restored = 0;
        int idx = 0;
        for (t_gobj* g = target->gl_list; g; g = g->g_next, ++idx) {
            auto it = idxToId.find(idx);
            if (it == idxToId.end()) continue;
            processor->mcpStableObjectMap[mapKey.toStdString()][it->second.toStdString()] = g;
            if (processor->mcpStableSerialMap.find(g) == processor->mcpStableSerialMap.end())
                processor->mcpStableSerialMap[g] = processor->mcpSerialCounter++;
            restored++;
        }
        return restored;
    };

    int restored = registerArray(sidecarObj->getProperty("root"), rootKey, cnv);

    // Named subcanvases: key "pd-<gl_name>" matches bridge map-key convention
    if (auto* subObj = sidecarObj->getProperty("sub").getDynamicObject()) {
        for (t_gobj* g = cnv->gl_list; g; g = g->g_next) {
            if (pd_class(&g->g_pd) != canvas_class) continue;
            auto* child = reinterpret_cast<t_canvas*>(g);
            if (!child->gl_name) continue;
            juce::String childName = juce::String::fromUTF8(child->gl_name->s_name);
            auto childIds = subObj->getProperty(juce::Identifier(childName));
            if (childIds.getArray() != nullptr && childIds.getArray()->size() > 0) {
                processor->mcpStableObjectMap[("pd-" + childName).toStdString()].clear();
                restored += registerArray(childIds, "pd-" + childName, child);
            }
        }
    }

    processor->mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
    return restored;
}

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

static float varToFloat(const juce::var& v, float def)
{
    if (v.isVoid() || v.isUndefined()) return def;
    if (v.isBool()) return v.toString().equalsIgnoreCase("true") ? 1.0f : 0.0f;
    return (float)(double)v;
}

static int varToInt(const juce::var& v, int def)
{
    if (v.isVoid() || v.isUndefined()) return def;
    return (int)(double)v;
}

static bool varToBool(const juce::var& v, bool def)
{
    if (v.isVoid() || v.isUndefined()) return def;
    if (v.isBool()) return v.toString().equalsIgnoreCase("true");
    return (double)v != 0.0;
}

static Canvas* getOrCreateCanvasComponent(PluginProcessor* proc, t_canvas* cnv)
{
    if (!proc || !cnv) return nullptr;
    Canvas* canvasComp = nullptr;
    for (auto* editor : proc->getEditors()) {
        if (!editor) continue;
        for (auto* c : editor->getCanvases()) {
            if (c && (c->patch.getUncheckedPointer() == cnv || c->patch.getRawPointer() == cnv || (c->patch.getPointer().get() == cnv))) {
                canvasComp = c;
                break;
            }
        }
        if (!canvasComp && editor->getCurrentCanvas()) {
            if (editor->getCurrentCanvas()->patch.getUncheckedPointer() == cnv || editor->getCurrentCanvas()->patch.getRawPointer() == cnv)
                canvasComp = editor->getCurrentCanvas();
        }
        if (!canvasComp) {
            canvasComp = editor->getTabComponent().openPatch(new pd::Patch(pd::WeakReference(cnv, proc), proc, false));
        }
        if (canvasComp) break;
    }
    return canvasComp;
}

void MCPBridge::handlePdDomain(const juce::String& action, const juce::OSCMessage& msg)
{
    if (action == "ping") {
        sendRawReply("/pd/pong");
        return;
    }

    if (action == "mcp_reload_lua") {
        auto correlationId = msg.size() >= 2 ? getArgString(msg[1]) : (msg.size() > 0 ? getArgString(msg[0]) : "0");
        sendReply("/pd/mcp_reload_lua/reply/" + correlationId, 1.0f);
        return;
    }

    if (action == "dsp") {
        if (msg.size() > 0) {
            float val = getArgFloat(msg[0]);
            if (processor) {
                if (val > 0.5f) processor->startDSP();
                else processor->releaseDSP();
            }
        }
        return;
    }

    if (action == "update_dsp") {
        // Full DSP graph recompile (stop+start). Needed after bulk
        // reconstruct operations (load/undo/redo) which create/connect
        // objects while the graph was compiled against an empty canvas.
        sys_lock();
        canvas_update_dsp();
        sys_unlock();
        return;
    }

    if (action == "vis") {
        if (msg.size() >= 2) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            float vis = getArgFloat(msg[1]);
            sys_lock();
            t_canvas* cnv = reinterpret_cast<t_canvas*>(pd_findbyclass(gensym(canvasName.toRawUTF8()), canvas_class));
            if (cnv) {
                t_atom a;
                SETFLOAT(&a, vis);
                pd_typedmess(reinterpret_cast<t_pd*>(cnv), gensym("vis"), 1, &a);
            }
            sys_unlock();
        }
        return;
    }

    if (action == "census") {
        if (msg.size() >= 1 && processor) {
            SmallArray<pd::Atom> atoms;
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";
            atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
            atoms.add(pd::Atom(processor->generateSymbol(correlationId)));
            processor->receiveSysMessage("mcp_census", atoms);
        }
        return;
    }

    if (action == "typeof") {
        // /pd/typeof <canvasName> <tempId_or_index> <correlationId>
        // Returns: /pd/typeof/reply/<corrId> <className> <objectText>
        // Reads the object's class name and binbuf text directly from structs.
        if (msg.size() >= 3 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto targetId = getArgString(msg[1]);
            auto correlationId = getArgString(msg[2]);

            sys_lock();
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

            if (!cnv) {
                sys_unlock();
                sendReply("/pd/typeof/error/" + correlationId, "Canvas not found: " + canvasName);
                return;
            }

            // Resolve by stableId first, fallback to numeric index
            t_gobj* gobj = processor->resolveStableId(canvasName, targetId);
            if (!gobj) {
                bool isNumber = targetId.isNotEmpty();
                for (int i = (targetId.startsWith("-") ? 1 : 0); i < targetId.length(); ++i) {
                    if (!juce::CharacterFunctions::isDigit(targetId[i])) {
                        isNumber = false;
                        break;
                    }
                }
                if (isNumber) {
                    gobj = glistObjectAt(cnv, targetId.getIntValue());
                }
            }

            if (!gobj) {
                sys_unlock();
                sendReply("/pd/typeof/error/" + correlationId, "Object not found: " + targetId);
                return;
            }

            // Get class name from the object's pd struct
            juce::String className = juce::String::fromUTF8(class_getname(pd_class(&gobj->g_pd)));

            // PRD §2.1: canvas-class objects (subpatch/abstraction wrappers).
            // NOTE: class_getname(canvas_class) is "canvas" in Pd, not "pd" —
            // normalize to the census vocabulary. Resolve the real loaded-file
            // name (MERDA .m~ / .pd abstraction) via the shared helper so
            // /pd/typeof is uniformly useful; inline [pd foo] stays "pd".
            if (pd_class(&gobj->g_pd) == canvas_class) {
                juce::String abstractName;
                if (pd::getAbstractionFileName(gobj, abstractName)) {
                    className = abstractName;
                } else {
                    className = "pd";
                }
            }

            // Get object text from binbuf (e.g. "osc~ 440", "vcf~ 1200 5")
            juce::String objectText;
            t_object* obj = pd::Interface::checkObject(gobj);
            if (obj) {
                char* text = nullptr;
                int len = 0;
                pd::Interface::getObjectText(obj, &text, &len);
                if (text && len > 0) {
                    objectText = juce::String::fromUTF8(text, len);
                    freebytes(text, len);
                }
            }

            // Get position
            int x = 0, y = 0, w = 0, h = 0;
            pd::Interface::getObjectBounds(cnv, gobj, &x, &y, &w, &h);

            sys_unlock();

            juce::OSCMessage reply { juce::OSCAddressPattern("/pd/typeof/reply/" + correlationId) };
            reply.addArgument(className);
            reply.addArgument(objectText);
            reply.addArgument(static_cast<int32>(x));
            reply.addArgument(static_cast<int32>(y));
            reply.addArgument(static_cast<int32>(w));
            reply.addArgument(static_cast<int32>(h));
            sender.send(reply);
        }
        return;
    }

    if (action == "ports") {
        // /pd/ports <canvasName> <tempId_or_index> <correlationId>
        // Returns: /pd/ports/reply/<corrId> <numInlets> <numOutlets> <inletTypes...> <outletTypes...>
        // inletTypes/outletTypes: "s" for signal, "c" for control
        if (msg.size() >= 3 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto targetId = getArgString(msg[1]);
            auto correlationId = getArgString(msg[2]);

            sys_lock();
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

            if (!cnv) {
                sys_unlock();
                sendReply("/pd/ports/error/" + correlationId, "Canvas not found: " + canvasName);
                return;
            }

            // Resolve by stableId first, fallback to numeric index
            t_gobj* gobj = processor->resolveStableId(canvasName, targetId);
            if (!gobj) {
                bool isNumber = targetId.isNotEmpty();
                for (int i = (targetId.startsWith("-") ? 1 : 0); i < targetId.length(); ++i) {
                    if (!juce::CharacterFunctions::isDigit(targetId[i])) {
                        isNumber = false;
                        break;
                    }
                }
                if (isNumber) {
                    gobj = glistObjectAt(cnv, targetId.getIntValue());
                }
            }

            if (!gobj) {
                sys_unlock();
                sendReply("/pd/ports/error/" + correlationId, "Object not found: " + targetId);
                return;
            }

            t_object* obj = pd::Interface::checkObject(gobj);
            if (!obj) {
                sys_unlock();
                sendReply("/pd/ports/error/" + correlationId, "Target is not a valid Pd object: " + targetId);
                return;
            }

            int numInlets = obj_ninlets(obj);
            int numOutlets = obj_noutlets(obj);

            // Build type strings: "s" = signal, "c" = control
            juce::String inletTypes;
            for (int i = 0; i < numInlets; i++) {
                inletTypes += obj_issignalinlet(obj, i) ? "s" : "c";
            }

            juce::String outletTypes;
            for (int i = 0; i < numOutlets; i++) {
                outletTypes += obj_issignaloutlet(obj, i) ? "s" : "c";
            }

            sys_unlock();

            juce::OSCMessage reply { juce::OSCAddressPattern("/pd/ports/reply/" + correlationId) };
            reply.addArgument(static_cast<int32>(numInlets));
            reply.addArgument(static_cast<int32>(numOutlets));
            reply.addArgument(inletTypes);
            reply.addArgument(outletTypes);
            sender.send(reply);
        }
        return;
    }

    if (action == "identity_snapshot") {
        // /pd/identity_snapshot <canvasName> <correlationId>
        // Returns a JSON string with the complete identity state:
        // { "version": N, "canvas": "pd-main", "entries": [
        //   { "id": "fm_carrier", "index": 3, "type": "osc~", "text": "osc~ 440" },
        //   ...
        // ]}
        // Single O(n) walk of gl_list builds a reverse pointer→index map, then
        // iterates the stable map. No per-entry O(n) scan. Total: O(n+m).
        if (msg.size() >= 2 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);

            sys_lock();
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

            if (!cnv) {
                sys_unlock();
                sendReply("/pd/identity_snapshot/error/" + correlationId, "Canvas not found: " + canvasName);
                return;
            }

            // Step 1: Build pointer→index map with ONE walk of gl_list (O(n))
            std::unordered_map<t_gobj*, int> ptrToIndex;
            int idx = 0;
            for (t_gobj* y = cnv->gl_list; y; y = y->g_next) {
                ptrToIndex[y] = idx++;
            }
            int totalObjects = idx;

            // Step 2: Walk the identity map and validate each entry (O(m))
            auto canvasStr = canvasName.toStdString();
            auto& canvasMap = processor->mcpStableObjectMap[canvasStr];
            uint64_t version = processor->mcpIdentityVersion.load(std::memory_order_relaxed);

            auto* rootObj = new juce::DynamicObject();
            rootObj->setProperty("version", static_cast<juce::int64>(version));
            rootObj->setProperty("canvas", canvasName);
            rootObj->setProperty("totalObjects", totalObjects);

            juce::Array<juce::var> entries;

            std::vector<std::string> toEvict;
            for (auto& [tempId, ptr] : canvasMap) {
                auto ptrIt = ptrToIndex.find(ptr);
                if (ptrIt == ptrToIndex.end()) {
                    // Object no longer on canvas — mark for eviction
                    toEvict.push_back(tempId);
                    continue;
                }

                // Serial verification (UAF defense)
                auto serialIt = processor->mcpStableSerialMap.find(ptr);
                if (serialIt == processor->mcpStableSerialMap.end()) {
                    toEvict.push_back(tempId);
                    continue;
                }

                auto* entry = new juce::DynamicObject();
                entry->setProperty("id", juce::String(tempId));
                entry->setProperty("index", ptrIt->second);

                // Read class name
                juce::String className = juce::String::fromUTF8(class_getname(pd_class(&ptr->g_pd)));
                entry->setProperty("class", className);

                // Read object text (type + args)
                t_object* obj = pd::Interface::checkObject(ptr);
                if (obj) {
                    char* text = nullptr;
                    int len = 0;
                    pd::Interface::getObjectText(obj, &text, &len);
                    if (text && len > 0) {
                        entry->setProperty("text", juce::String::fromUTF8(text, len));
                        freebytes(text, len);
                    }
                    entry->setProperty("inlets", obj_ninlets(obj));
                    entry->setProperty("outlets", obj_noutlets(obj));
                }

                entries.add(juce::var(entry));
            }

            // Evict stale entries
            for (auto& staleId : toEvict) {
                auto staleIt = canvasMap.find(staleId);
                if (staleIt != canvasMap.end()) {
                    processor->mcpStableSerialMap.erase(staleIt->second);
                    canvasMap.erase(staleIt);
                }
            }
            if (!toEvict.empty()) {
                processor->mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
                version = processor->mcpIdentityVersion.load(std::memory_order_relaxed);
                rootObj->setProperty("version", static_cast<juce::int64>(version));
                rootObj->setProperty("evicted", static_cast<int>(toEvict.size()));
            }

            rootObj->setProperty("entryCount", entries.size());
            rootObj->setProperty("entries", entries);

            juce::String jsonString = juce::JSON::toString(juce::var(rootObj), true);
            sys_unlock();

            juce::OSCMessage reply { juce::OSCAddressPattern("/pd/identity_snapshot/reply/" + correlationId) };
            reply.addArgument(jsonString);
            sender.send(reply);
        }
        return;
    }

    if (action == "identity_version") {
        // /pd/identity_version <correlationId>
        // Returns the current identity version counter.
        // Ultra-cheap: one atomic read, no locks, no canvas walk.
        // Node.js uses this to decide if a full snapshot fetch is needed.
        if (msg.size() >= 1 && processor) {
            auto correlationId = getArgString(msg[0]);
            uint64_t version = processor->mcpIdentityVersion.load(std::memory_order_relaxed);

            juce::OSCMessage reply { juce::OSCAddressPattern("/pd/identity_version/reply/" + correlationId) };
            reply.addArgument(static_cast<int32>(static_cast<int>(version & 0x7FFFFFFF)));
            sender.send(reply);
        }
        return;
    }

    if (action == "batch_atomic") {
        // /pd/batch_atomic <canvas> <corrId> <deleteCount> <disconnectCount> <editCount> <createCount> <connectCount> [data...]
        // Performs delete + disconnect + edit + create + connect in ONE sys_lock with exactly ONE DSP recompile.
        // Protocol:
        //   Deletes: [tempId] * deleteCount
        //   Disconnects: [srcId, srcOut, destId, destIn] * disconnectCount
        //   Edits: [tempId, newType, nargs, arg1..argN] * editCount
        //   Creates: [tempId, x, y, kind, type, nargs, arg1..argN] * createCount
        //   Connects: [srcId, srcOut, destId, destIn] * connectCount
        // Reply: /pd/batch_atomic/reply/<corrId> <deletedCount> <disconnectedCount> <editedCount> <createdCount> <connectedCount> [inline mappings...]
        if (msg.size() >= 5 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            // R3a — corrId dedup (PRD preflight): retry with same corrId must
            // not double-apply. LRU + TTL cache of full replies.
            static std::unordered_map<std::string, BatchDedupEntry> s_batchDedupCache;
            static std::deque<std::string> s_batchDedupOrder;
            static constexpr int BATCH_DEDUP_MAX = 128;
            static constexpr uint64_t BATCH_DEDUP_TTL_MS = 30000;
            if (!processor->isExecutingMcpUndoRedo) {
                std::string corrKey = correlationId.toStdString();
                uint64_t nowMs = juce::Time::currentTimeMillis();
                auto it = s_batchDedupCache.find(corrKey);
                if (it != s_batchDedupCache.end() && (nowMs - it->second.ts) < BATCH_DEDUP_TTL_MS) {
                    sender.send(it->second.reply);
                    return;
                }
                // Prune expired entries to bound memory.
                for (auto eit = s_batchDedupCache.begin(); eit != s_batchDedupCache.end();) {
                    if ((nowMs - eit->second.ts) >= BATCH_DEDUP_TTL_MS) {
                        auto oit = std::find(s_batchDedupOrder.begin(), s_batchDedupOrder.end(), eit->first);
                        if (oit != s_batchDedupOrder.end()) s_batchDedupOrder.erase(oit);
                        eit = s_batchDedupCache.erase(eit);
                    } else ++eit;
                }
            }
            
            int deleteCount = 0, disconnectCount = 0, editCount = 0, createCount = 0, connectCount = 0;
            int cursor = 2;

            if (msg.size() >= 7) {
                // 5-phase extended atomic protocol
                deleteCount = static_cast<int>(getArgFloat(msg[cursor++]));
                disconnectCount = static_cast<int>(getArgFloat(msg[cursor++]));
                editCount = static_cast<int>(getArgFloat(msg[cursor++]));
                createCount = static_cast<int>(getArgFloat(msg[cursor++]));
                connectCount = static_cast<int>(getArgFloat(msg[cursor++]));
            } else {
                // Legacy 3-phase protocol fallback (creates, connects, edits)
                createCount = static_cast<int>(getArgFloat(msg[cursor++]));
                connectCount = static_cast<int>(getArgFloat(msg[cursor++]));
                editCount = static_cast<int>(getArgFloat(msg[cursor++]));
            }

            int deleted = 0, disconnected = 0, edited = 0, created = 0, connected = 0;
            int reconcileEvicted = 0, reconcileAdopted = 0;
            int layoutSanitizedMoved = 0;
            int occlusionsFixed = 0;
            bool fallbackComposed = false;
            // Phase A (PRD diagnostic layer): create/connect failure facts —
            // named failures instead of silent skips (see PRD §2.1).
            struct CreateFailure { std::string tempId; std::string type; std::string reason; };
            struct ConnectFailure { std::string srcId; std::string destId; std::string reason; };
            std::vector<CreateFailure> createFailures;
            std::vector<ConnectFailure> connectFailures;
            std::vector<std::string> createdIds;
            std::vector<t_gobj*> createdPtrs;
            std::vector<int32> mappingIndices;

            // =========================================================================
            // PHASE 0: PRE-PARSE outside audio thread — zero contention
            // =========================================================================
            struct PreDelete { juce::String objectId; };
            std::vector<PreDelete> preDeletes;
            for (int d = 0; d < deleteCount && cursor < msg.size(); d++)
                preDeletes.push_back({ getArgString(msg[cursor++]) });

            struct PreDisconnect { juce::String srcId; int srcOut; juce::String destId; int destIn; };
            std::vector<PreDisconnect> preDisconnects;
            for (int dc = 0; dc < disconnectCount && cursor + 3 < msg.size(); dc++) {
                auto s = getArgString(msg[cursor++]); int so = static_cast<int>(getArgFloat(msg[cursor++]));
                auto d2 = getArgString(msg[cursor++]); int di = static_cast<int>(getArgFloat(msg[cursor++]));
                preDisconnects.push_back({ s, so, d2, di });
            }

            struct PreEdit { juce::String objectId; juce::String newText; };
            std::vector<PreEdit> preEdits;
            for (int e = 0; e < editCount && cursor < msg.size(); e++) {
                auto oid = getArgString(msg[cursor++]);
                auto nt = (cursor < msg.size()) ? getArgString(msg[cursor++]) : juce::String();
                int na = (cursor < msg.size()) ? static_cast<int>(getArgFloat(msg[cursor++])) : 0;
                juce::StringArray tk;
                if (nt != "msg" && nt.isNotEmpty()) tk.add(nt);
                for (int a = 0; a < na && cursor < msg.size(); a++) tk.add(getArgString(msg[cursor++]));
                preEdits.push_back({ oid, tk.joinIntoString(" ") });
            }

            struct PendingCreate {
                juce::String tempId;
                juce::String objType;
                juce::String kind;
                int x;
                int y;
                float initValue;
                bool seedInit;
            };
            std::vector<PendingCreate> pendingCreates;
            juce::String pastaBuffer;
            for (int o = 0; o < createCount && cursor < msg.size(); o++) {
                auto oid = getArgString(msg[cursor++]);
                float px = (cursor < msg.size()) ? getArgFloat(msg[cursor++]) : 0;
                float py = (cursor < msg.size()) ? getArgFloat(msg[cursor++]) : 0;
                auto kind = (cursor < msg.size()) ? getArgString(msg[cursor++]) : juce::String();
                auto ot = (cursor < msg.size()) ? getArgString(msg[cursor++]) : juce::String();
                int na = (cursor < msg.size()) ? static_cast<int>(getArgFloat(msg[cursor++])) : 0;
                juce::StringArray tk;
                if (ot.isNotEmpty()) tk.add(ot);
                for (int a = 0; a < na && cursor < msg.size(); a++) tk.add(getArgString(msg[cursor++]));

                // line~ in this fork takes NO creation arguments (line_tilde_new(void)),
                // so `[line~ 0.057]` is silently created at 0. Capture a numeric init
                // arg here and re-seed it onto the object right after paste, so
                // `[r x] -> [line~ 440] -> [osc~]` actually starts at 440 instead of 0.
                bool seedInit = false;
                float initValue = 0.0f;
                if ((ot == "line~" || ot == "vline~") && na >= 1 && tk.size() >= 2) {
                    juce::String firstArg = tk[1];
                    if (firstArg.isNotEmpty()) {
                        auto charptr = firstArg.getCharPointer();
                        auto ptr = charptr;
                        juce::CharacterFunctions::readDoubleValue(ptr);
                        if (ptr - charptr == firstArg.getNumBytesAsUTF8()) {
                            initValue = firstArg.getFloatValue();
                            seedInit = true;
                        }
                    }
                }

                pastaBuffer += formatAsPdLine(kind, tk, static_cast<int>(px), static_cast<int>(py)) + "\n";
                pendingCreates.push_back({ oid, ot, kind, static_cast<int>(px), static_cast<int>(py), initValue, seedInit });
            }

            // ALL connections go through obj_connect after paste (no #X connect in buffer)
            struct CrossConn { juce::String srcId; int srcOut; juce::String destId; int destIn; };
            std::vector<CrossConn> allConns;
            for (int c = 0; c < connectCount && cursor + 3 < msg.size(); c++) {
                auto s = getArgString(msg[cursor++]); int so = static_cast<int>(getArgFloat(msg[cursor++]));
                auto d2 = getArgString(msg[cursor++]); int di = static_cast<int>(getArgFloat(msg[cursor++]));
                allConns.push_back({ s, so, d2, di });
            }

            // Optional trailing flags (PRD_CONTEXT_LAYOUT_GUARD P2/P3/P3.1):
            // autoLayout (default ON), pad, grid snap, fixOcclusions (default ON).
            // Old clients omit them.
            bool autoLayout = true;
            int layoutPad = 5, layoutSnap = 10;
            bool autoFixOcclusions = true;
            if (cursor < msg.size()) autoLayout = getArgFloat(msg[cursor++]) > 0.5f;
            if (cursor < msg.size()) layoutPad = static_cast<int>(getArgFloat(msg[cursor++]));
            if (cursor < msg.size()) layoutSnap = static_cast<int>(getArgFloat(msg[cursor++]));
            if (cursor < msg.size()) autoFixOcclusions = getArgFloat(msg[cursor++]) > 0.5f;

            if (processor->isExecutingMcpUndoRedo) {
                autoLayout = false;
                autoFixOcclusions = false;
            }


            // =========================================================================
            // EXECUTE ON AUDIO THREAD — zero lock contention, zero dropout
            // =========================================================================

            // Storage for PHASE 0 reconcile results (filled inside lambda, read outside)
            struct DetectedConn { std::string srcId; int srcOut; std::string destId; int destIn; };
            std::vector<DetectedConn> detectedConnections;
            // Manual GUI patching has zero dropout because it enqueues mutations into
            // functionQueue, which runs on the audio thread during sendMessagesFromQueue()
            // BEFORE performDSP(). We do exactly the same thing here.

            extern int dsp_update_deferred;  // defined in m_obj.c
            juce::WaitableEvent done;
            t_canvas* cnv = nullptr;
            // Bug #11 (fault gauntlet 2026-09-09): strict canvas resolution.
            // Unknown names must NOT silently fall back to the focused/root
            // canvas — name the failure instead of mutating the wrong patch.
            bool canvasNotFound = false;

            processor->enqueueFunctionAsync([&]() {
                auto tLambdaStart = std::chrono::high_resolution_clock::now();
                cnv = processor->getCanvasBySymbolStrict(canvasName);
                if (!cnv && (canvasName == "pd-main" || canvasName == "main" || canvasName.isEmpty()))
                    cnv = pd_this->pd_canvaslist;
                if (!cnv) {
                    canvasNotFound = true;
                    post("batch_atomic: canvas '%s' not found — mutation refused", canvasName.toRawUTF8());
                }

                if (cnv) {
                    dsp_update_deferred = 1;

                    // PHASE 0: PRE-FLIGHT AUTO-RECONCILE
                    // Single walk of gl_list to sync identity map with live canvas.
                    // Detects manual GUI edits (user created/deleted objects) and
                    // fixes the map BEFORE any mutation — zero extra OSC round-trips,
                    // zero audio dropout (runs inside the same audio-thread lambda).
                    {
                        auto canvasStr = canvasName.toStdString();
                        auto& canvasMap = processor->mcpStableObjectMap[canvasStr];

                        // Step A: Build set of all live pointers from gl_list (O(n))
                        std::unordered_set<t_gobj*> livePointers;
                        for (t_gobj* y = cnv->gl_list; y; y = y->g_next)
                            livePointers.insert(y);

                        // Step B: Evict stale entries — user deleted objects via GUI
                        std::vector<std::string> toEvict;
                        for (auto& [tempId, ptr] : canvasMap) {
                            if (livePointers.find(ptr) == livePointers.end()) {
                                toEvict.push_back(tempId);
                                processor->mcpStableSerialMap.erase(ptr);
                            }
                        }
                        for (auto& id : toEvict) {
                            canvasMap.erase(id);
                            processor->mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
                            reconcileEvicted++;
                        }

                        // Step C: Adopt untracked objects — user created via GUI
                        // Build reverse set of tracked pointers for O(1) lookup
                        std::unordered_set<t_gobj*> trackedPointers;
                        for (auto& [_, ptr] : canvasMap)
                            trackedPointers.insert(ptr);

                        // Build set of tempIds being edited in this transaction
                        // so we can re-assign them instead of creating gui_* names.
                        std::unordered_set<std::string> editedIds;
                        for (auto& pe : preEdits)
                            editedIds.insert(pe.objectId.toStdString());

                        // Also build ordered list of evicted tempIds for re-use
                        // (Step B evicted them because their pointer changed after edit)
                        std::vector<std::string> evictedEditIds;
                        for (auto& id : toEvict) {
                            if (editedIds.count(id))
                                evictedEditIds.push_back(id);
                        }
                        size_t evictedEditIdx = 0;

                        int adoptIdx = 0;
                        for (t_gobj* y = cnv->gl_list; y; y = y->g_next) {
                            if (trackedPointers.find(y) == trackedPointers.end()) {
                                // Untracked object — auto-assign a tempId
                                // Read class name for a meaningful prefix
                                // If this untracked object corresponds to an edited
                                // tempId (pointer changed after renameObject), reuse
                                // the original tempId instead of assigning gui_*.
                                // This keeps the identity map stable across edits.
                                std::string autoId;
                                if (evictedEditIdx < evictedEditIds.size()) {
                                    autoId = evictedEditIds[evictedEditIdx++];
                                } else {
                                    juce::String className = juce::String::fromUTF8(
                                        class_getname(pd_class(&y->g_pd)));
                                    className = className.replace("~", "_t");
                                    autoId = ("gui_" + className + "_"
                                        + juce::String(adoptIdx++)).toStdString();
                                    while (canvasMap.count(autoId))
                                        autoId = ("gui_" + className + "_"
                                            + juce::String(adoptIdx++)).toStdString();
                                }
                                canvasMap[autoId] = y;
                                processor->mcpStableSerialMap[y] = processor->mcpSerialCounter++;
                                processor->mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
                                reconcileAdopted++;
                            }
                        }

                         // Step D: Connection scan — MOVED to post-PHASE-5 (bug #5,
                         // fault gauntlet 2026-09-09). Scanning inside PHASE 0 made
                         // detectedConnections lag one transaction: the receipt
                         // showed PRE-mutation wires; wires created by this very
                         // batch only appeared one call later. Scan now runs AFTER
                         // connect/edit/delete so the receipt is always current.
                         // (linetraverser walks every wire on the canvas in O(wires);
                         // both endpoints have tempIds — new creates are registered
                         // in mcpStableObjectMap by PHASE 4 before PHASE 5 wiring.)
                         // Build reverse ptr→tempId map (O(m)) — kept for the
                         // post-mutation Step D scan below (bug #5 move).
                    }

                    // PHASE 1: DISCONNECT — remove wires BEFORE objects are deleted
                    // so endpoints and stable IDs remain valid for clean unlinking.
                    for (auto& pdc : preDisconnects) {
                        t_gobj* sg = processor->resolveStableId(canvasName, pdc.srcId);
                        t_gobj* dg = processor->resolveStableId(canvasName, pdc.destId);
                        if (sg && dg) {
                            t_object* so = pd::Interface::checkObject(sg);
                            t_object* d_o = pd::Interface::checkObject(dg);
                            if (so && d_o) {
                                int si = 0, di2 = 0, idx = 0;
                                for (t_gobj* y = cnv->gl_list; y; y = y->g_next, idx++) {
                                    if (y == sg) si = idx;
                                    if (y == dg) di2 = idx;
                                }
                                t_atom ca[4];
                                SETFLOAT(&ca[0], static_cast<float>(si));
                                SETFLOAT(&ca[1], static_cast<float>(pdc.srcOut));
                                SETFLOAT(&ca[2], static_cast<float>(di2));
                                SETFLOAT(&ca[3], static_cast<float>(pdc.destIn));
                                pd_typedmess(reinterpret_cast<t_pd*>(cnv), gensym("disconnect"), 4, ca);
                                disconnected++;
                            }
                        }
                    }

                    // PHASE 2: DELETE — audio-thread-safe object removal (zero undo/GUI
                    // overhead; editor reconciles via trailing synchroniseCanvases).
                    SmallArray<t_gobj*> toDelete;
                    for (auto& pd : preDeletes) {
                        t_gobj* obj = processor->resolveStableId(canvasName, pd.objectId);
                        if (obj) {
                            processor->mcpStableObjectMap[canvasName.toStdString()].erase(pd.objectId.toStdString());
                            processor->mcpStableSerialMap.erase(obj);
                            processor->mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
                            toDelete.add(obj);
                            deleted++;
                        }
                    }
                    if (toDelete.size() > 0) {
                        pd::Interface::removeObjectsAudioThread(cnv, toDelete);
                    }

                    // PHASE 3: EDIT
                    for (auto& pe : preEdits) {
                        t_gobj* obj = processor->resolveStableId(canvasName, pe.objectId);
                        if (obj) {
                            t_object* o = pd::Interface::checkObject(obj);
                            if (o) {
                                pd::Interface::renameObject(cnv, obj, pe.newText.toRawUTF8(), pe.newText.length());
                                t_gobj* newObj = nullptr; bool still = false;
                                for (t_gobj* y = cnv->gl_list; y; y = y->g_next)
                                    if (y == obj) { still = true; break; }
                                newObj = still ? obj : pd::Interface::getNewest(cnv);
                                if (newObj) {
                                    processor->mcpStableObjectMap[canvasName.toStdString()][pe.objectId.toStdString()] = newObj;
                                    if (newObj != obj) processor->mcpStableSerialMap.erase(obj);
                                    processor->mcpStableSerialMap[newObj] = processor->mcpSerialCounter++;
                                    processor->mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
                                    edited++;
                                }
                            }
                        }
                    }

                    // PHASE 4: CREATE via pasteDirect
                    int preCreateCount = 0;
                    for (t_gobj* g = cnv->gl_list; g; g = g->g_next) preCreateCount++;

                    if (createCount > 0 && pastaBuffer.isNotEmpty()) {
                        pasteDirect(cnv, pastaBuffer.toRawUTF8());

                        std::vector<t_gobj*> allObjects;
                        for (t_gobj* g = cnv->gl_list; g; g = g->g_next)
                            allObjects.push_back(g);
                        int newCount = static_cast<int>(allObjects.size()) - preCreateCount;

                        struct NewObjInfo {
                            t_gobj* ptr = nullptr;
                            int glIndex = -1;
                            int x = 0;
                            int y = 0;
                            int type = -1;
                            juce::String firstWord;
                            juce::String className;
                            bool isRedBox = false;
                        };

                        std::vector<NewObjInfo> newInfos;
                        for (int i = 0; i < newCount; ++i) {
                            t_gobj* g = allObjects[preCreateCount + i];
                            NewObjInfo info;
                            info.ptr = g;
                            info.glIndex = preCreateCount + i;
                            t_object* ob = pd::Interface::checkObject(g);
                            if (ob) {
                                info.x = ob->te_xpix;
                                info.y = ob->te_ypix;
                                info.type = ob->te_type;
                                t_class* cl = pd_class(&g->g_pd);
                                if (cl) {
                                    const char* cName = class_getname(cl);
                                    if (cName) info.className = juce::String::fromUTF8(cName);
                                }
                                info.isRedBox = (ob->te_type == T_OBJECT && cl == text_class);
                                if (ob->te_binbuf) {
                                    char* tb = nullptr; int tsz = 0;
                                    binbuf_gettext(ob->te_binbuf, &tb, &tsz);
                                    if (tb && tsz > 0) {
                                        juce::String full = juce::String::fromUTF8(tb, tsz).trim();
                                        info.firstWord = full.upToFirstOccurrenceOf(" ", false, false);
                                        freebytes(tb, tsz);
                                    }
                                }
                            }
                            newInfos.push_back(info);
                        }

                        auto computeMatchScore = [](const PendingCreate& pc, const NewObjInfo& no) -> int {
                            int score = 0;
                            // Exact coordinate match is a strong structural anchor
                            if (pc.x == no.x && pc.y == no.y) score += 20;

                            // Red-box match: Pd creates text_class dummy, but te_binbuf preserves original type
                            if (no.isRedBox && no.firstWord == pc.objType) {
                                score += 25;
                                return score;
                            }

                            // Class name / binbuf first word match
                            if (no.firstWord.isNotEmpty() && no.firstWord == pc.objType) {
                                score += 20;
                            } else if (no.className.isNotEmpty() && no.className == pc.objType) {
                                score += 20;
                            }

                            // Kind match
                            if (pc.kind == "msg" && no.type == T_MESSAGE) score += 10;
                            else if (pc.kind == "text" && no.type == T_TEXT) score += 10;
                            else if ((pc.kind == "floatatom" || pc.kind == "symbolatom") &&
                                     (no.type == T_ATOM || no.className.containsIgnoreCase("atom"))) score += 10;
                            else if ((pc.kind == "obj" || pc.kind.isEmpty()) && no.type == T_OBJECT) score += 5;

                            return score;
                        };

                        // Monotonic alignment of created gobjs to pendingCreates.
                        // Since pasteDirect appends strictly in buffer order, newInfos
                        // is guaranteed to be a monotonic subsequence of pendingCreates.
                        int pCur = 0;
                        const int totalPending = static_cast<int>(pendingCreates.size());

                        // Red-box ghosts (failed creates) collected for rollback
                        // AFTER alignment, so the mapping scan reflects the final
                        // gl_list. A failed create must be a pure no-op (parity with
                        // the GOP-encapsulate path).
                        SmallArray<t_gobj*> redBoxes;

                        for (int o = 0; o < newCount; ++o) {
                            const auto& no = newInfos[o];
                            int bestP = -1;
                            int bestScore = 0;

                            int remainingObjs = newCount - 1 - o;
                            int maxSearchP = totalPending - remainingObjs;

                            for (int p = pCur; p < maxSearchP; ++p) {
                                int score = computeMatchScore(pendingCreates[p], no);
                                if (score > bestScore) {
                                    bestScore = score;
                                    bestP = p;
                                }
                            }

                            if (bestP < 0) {
                                bestP = pCur;
                            }

                            // All pendingCreates between pCur and bestP produced NO gobj on canvas
                            for (int p = pCur; p < bestP; ++p) {
                                createFailures.push_back({
                                    pendingCreates[p].tempId.toStdString(),
                                    pendingCreates[p].objType.toStdString(),
                                    "couldn't create"
                                });
                            }

                            auto& pc = pendingCreates[bestP];

                            // Check if this object is a dummy red box
                            if (no.isRedBox) {
                                // Roll the red box back so a failed create leaves
                                // NOTHING behind (parity with the GOP-encapsulate
                                // path). The ghost is still named in createFailures.
                                createFailures.push_back({
                                    pc.tempId.toStdString(),
                                    pc.objType.toStdString(),
                                    "couldn't create — red box rolled back (nothing left on canvas)"
                                });
                                redBoxes.add(no.ptr);
                            } else {
                                // Map the stable tempId to the live gobj pointer
                                processor->mcpStableObjectMap[canvasName.toStdString()][pc.tempId.toStdString()] = no.ptr;
                                processor->mcpStableSerialMap[no.ptr] = processor->mcpSerialCounter++;
                                processor->mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);

                                // Auto-seed line~/vline~ init value (fork ignores creation args)
                                if (pc.seedInit) {
                                    t_object* so = pd::Interface::checkObject(no.ptr);
                                    if (so) {
                                        t_atom sa;
                                        SETFLOAT(&sa, pc.initValue);
                                        pd_typedmess(reinterpret_cast<t_pd*>(so), gensym("float"), 1, &sa);
                                    }
                                }

                                createdIds.push_back(pc.tempId.toStdString());
                                createdPtrs.push_back(no.ptr);
                                mappingIndices.push_back(static_cast<int32>(no.glIndex));
                                created++;
                            }

                            pCur = bestP + 1;
                        }

                        // Any remaining pendingCreates after all newInfos were matched
                        // had no corresponding live object
                        for (int p = pCur; p < totalPending; ++p) {
                            createFailures.push_back({
                                pendingCreates[p].tempId.toStdString(),
                                pendingCreates[p].objType.toStdString(),
                                "couldn't create"
                            });
                        }

                        // Roll back red-box ghosts now that alignment is done.
                        // Audio-thread-safe removal (they were created moments ago in
                        // this same lambda; GUI has not synced them yet). Rebuild the
                        // index mappings against the post-delete gl_list so the reply's
                        // tempId→index pairs stay correct.
                        if (redBoxes.size() > 0) {
                            pd::Interface::removeObjectsAudioThread(cnv, redBoxes);
                            for (size_t mi = 0; mi < mappingIndices.size() && mi < createdPtrs.size(); ++mi) {
                                if (!createdPtrs[mi]) continue;
                                int idx = 0, found = -1;
                                for (t_gobj* y = cnv->gl_list; y; y = y->g_next, ++idx) {
                                    if (y == createdPtrs[mi]) { found = idx; break; }
                                }
                                if (found >= 0) mappingIndices[mi] = found;
                            }
                        }
                    }

                    // PHASE 5: CONNECT via obj_connect
                    // R2 — live signal adjacency for cycle detection (PRD preflight).
                    // Built once before the loop: signal edges only. Bounded: skip
                    // if graph too large (2000 objs / 4000 wires) — X-ray still reports.
                    std::unordered_map<t_gobj*, std::vector<t_gobj*>> liveSigAdj;
                    std::unordered_map<t_gobj*, std::vector<t_gobj*>> tentativeSigAdj;
                    bool cycleGuardActive = false;
                    {
                        int liveObjCount = 0;
                        for (t_gobj* y = cnv->gl_list; y; y = y->g_next) liveObjCount++;
                        if (liveObjCount <= 2000) {
                            t_linetraverser ltCount;
                            linetraverser_start(&ltCount, cnv);
                            int wireCount = 0;
                            while (linetraverser_next_nosize(&ltCount)) wireCount++;
                            if (wireCount <= 4000) {
                                cycleGuardActive = true;
                                t_linetraverser lt;
                                linetraverser_start(&lt, cnv);
                                t_outconnect* ocTmp = nullptr;
                                while ((ocTmp = linetraverser_next_nosize(&lt))) {
                                    t_gobj* sgTmp = &lt.tr_ob->ob_g;
                                    t_gobj* dgTmp = &lt.tr_ob2->ob_g;
                                    t_object* soTmp = pd::Interface::checkObject(sgTmp);
                                    if (soTmp && obj_issignaloutlet(soTmp, lt.tr_outno)) {
                                        liveSigAdj[sgTmp].push_back(dgTmp);
                                    }
                                }
                                tentativeSigAdj = liveSigAdj;
                            }
                        }
                    }
                    for (auto& cc : allConns) {
                        t_gobj* sg = processor->resolveStableId(canvasName, cc.srcId);
                        t_gobj* dg = processor->resolveStableId(canvasName, cc.destId);
                        if (!sg || !dg) {
                            // Phase A (PRD diagnostic layer): name the silent
                            // failure instead of skipping silently.
                            std::string reason;
                            if (!sg && !dg) reason = "src and dest not found";
                            else if (!sg)   reason = "src not found";
                            else            reason = "dest not found";
                            connectFailures.push_back({
                                cc.srcId.toStdString(), cc.destId.toStdString(), reason });
                            continue;
                        }
                        t_object* so = pd::Interface::checkObject(sg);
                        t_object* d_o = pd::Interface::checkObject(dg);
                        if (so && d_o) {
                            // Phase B (fault-injection gauntlet 2026-09-09):
                            // name self-connections, duplicate wires, and
                            // out-of-range ports instead of counting them as
                            // fresh successes.
                            if (sg == dg) {
                                connectFailures.push_back({
                                    cc.srcId.toStdString(), cc.destId.toStdString(),
                                    "self-connection (same object src==dest) — forbidden, breaks DSP scheduling" });
                                continue;
                            }
                            if (cc.srcOut < 0 || cc.srcOut >= obj_noutlets(so)) {
                                connectFailures.push_back({
                                    cc.srcId.toStdString(), cc.destId.toStdString(),
                                    "src outlet " + std::to_string(cc.srcOut) + " out of range (object has "
                                        + std::to_string(obj_noutlets(so)) + ")" });
                                continue;
                            }
                            if (cc.destIn < 0 || cc.destIn >= obj_ninlets(d_o)) {
                                connectFailures.push_back({
                                    cc.srcId.toStdString(), cc.destId.toStdString(),
                                    "dest inlet " + std::to_string(cc.destIn) + " out of range (object has "
                                        + std::to_string(obj_ninlets(d_o)) + ")" });
                                continue;
                            }
                            // Duplicate-wire check: Pd silently accepts re-connecting
                            // an existing wire; count it as a no-op, not a fresh success.
                            // Walk the source outlet's connection list (Pd traverse API).
                            bool alreadyWired = false;
                            {
                                t_outlet* srcOutlet = nullptr;
                                t_outconnect* oc = obj_starttraverseoutlet(so, &srcOutlet, cc.srcOut);
                                while (oc) {
                                    t_object* destObj = nullptr;
                                    t_inlet* destInl = nullptr;
                                    int which = -1;
                                    oc = obj_nexttraverseoutlet(oc, &destObj, &destInl, &which);
                                    if (destObj == d_o && which == cc.destIn) {
                                        alreadyWired = true;
                                        break;
                                    }
                                }
                            }
                            if (alreadyWired) {
                                connectFailures.push_back({
                                    cc.srcId.toStdString(), cc.destId.toStdString(),
                                    "duplicate wire (already connected) — no-op, not counted" });
                                continue;
                            }
                            // R1 — Rate guard (PRD preflight, 2026-09-09): signal outlet
                            // -> control inlet. Same rule as Interface::canConnect
                            // (return !obj_issignaloutlet || obj_issignalinlet). The
                            // GUI enforces it; batch_atomic previously bypassed it
                            // via raw obj_connect. Now REJECT with stable prefix.
                            {
                                bool srcSig  = obj_issignaloutlet(so,  cc.srcOut) != 0;
                                bool destSig = obj_issignalinlet(d_o, cc.destIn)  != 0;
                                if (srcSig && !destSig) {
                                    connectFailures.push_back({
                                        cc.srcId.toStdString(), cc.destId.toStdString(),
                                        "rate: signal outlet " + std::to_string(cc.srcOut)
                                          + " -> control inlet " + std::to_string(cc.destIn)
                                          + " — Pd drops the audio; use [snapshot~] or rewire via [*~]" });
                                    continue;
                                }
                            }
                            // R2 — would this wire close a zero-delay signal loop?
                            // Only signal-outlet wires can close signal cycles; control
                            // edges are not tracked. Tentative graph grows in batch order
                            // so the FIRST loop-closing wire is rejected.
                            if (cycleGuardActive && obj_issignaloutlet(so, cc.srcOut)) {
                                std::unordered_set<t_gobj*> visited;
                                std::vector<t_gobj*> stack;
                                stack.push_back(dg);
                                visited.insert(dg);
                                bool closesCycle = false;
                                while (!stack.empty() && !closesCycle) {
                                    t_gobj* cur = stack.back(); stack.pop_back();
                                    if (cur == sg) { closesCycle = true; break; }
                                    auto it = tentativeSigAdj.find(cur);
                                    if (it == tentativeSigAdj.end()) continue;
                                    for (t_gobj* nxt : it->second) {
                                        if (visited.insert(nxt).second) stack.push_back(nxt);
                                    }
                                }
                                if (closesCycle) {
                                    connectFailures.push_back({
                                        cc.srcId.toStdString(), cc.destId.toStdString(),
                                        "cycle: would close zero-delay signal loop — break it with [delwrite~]/[delread~] or [send~]/[receive~]" });
                                    continue;
                                }
                                tentativeSigAdj[sg].push_back(dg);
                            }
                            if (obj_connect(so, cc.srcOut, d_o, cc.destIn))
                                connected++;
                            else
                                connectFailures.push_back({
                                    cc.srcId.toStdString(), cc.destId.toStdString(),
                                    "obj_connect failed" });
                        } else {
                            connectFailures.push_back({
                                cc.srcId.toStdString(), cc.destId.toStdString(),
                                "not a patchable object" });
                        }
                    }

                    // Step D (moved, bug #5): post-mutation connection scan — runs
                    // AFTER PHASE 5 connect so the receipt reflects the wires this
                    // batch just created. PHASE 0 adoption already minted tempIds
                    // for GUI objects; PHASE 4 registered new creates; deletes
                    // were erased — every live wire endpoint resolves.
                    {
                        auto canvasStr = canvasName.toStdString();
                        auto& canvasMap = processor->mcpStableObjectMap[canvasStr];
                        std::unordered_map<t_gobj*, std::string> ptrToTempId;
                        for (auto& [tid, ptr] : canvasMap)
                            if (ptr) ptrToTempId[ptr] = tid;

                        t_linetraverser lt;
                        linetraverser_start(&lt, cnv);
                        t_outconnect* ltOc = nullptr;
                        while ((ltOc = linetraverser_next_nosize(&lt))) {
                            t_gobj* sg = &lt.tr_ob->ob_g;
                            t_gobj* dg = &lt.tr_ob2->ob_g;
                            auto sIt = ptrToTempId.find(sg);
                            auto dIt = ptrToTempId.find(dg);
                            if (sIt == ptrToTempId.end() || dIt == ptrToTempId.end()) continue;
                            detectedConnections.push_back({
                                sIt->second,
                                static_cast<int>(lt.tr_outno),
                                dIt->second,
                                static_cast<int>(lt.tr_inno)
                            });
                        }
                    }

                    canvas_dirty(cnv, 1);
                    dsp_update_deferred = 0;

                    auto t0 = std::chrono::high_resolution_clock::now();
                    canvas_update_dsp();
                    auto t1 = std::chrono::high_resolution_clock::now();
                    auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
                    post("batch_atomic: canvas_update_dsp took %lld us", (long long)us);
                }

                // Fire loadbangs
                for (auto* g : createdPtrs) {
                    if (g) {
                        if (pd_class(&g->g_pd) == canvas_class)
                            canvas_loadbang(reinterpret_cast<t_canvas*>(g));
                        else if (zgetfn(&g->g_pd, gensym("loadbang")))
                            vmess(&g->g_pd, gensym("loadbang"), "f", LB_LOAD);
                    }
                }

                // (layout guard + fallback run on the message thread below.)

                auto tLambdaEnd = std::chrono::high_resolution_clock::now();
                auto totalUs = std::chrono::duration_cast<std::chrono::microseconds>(tLambdaEnd - tLambdaStart).count();
                post("batch_atomic: TOTAL lambda took %lld us", (long long)totalUs);

                done.signal();
            }); // end enqueueFunctionAsync lambda

            // Wait for audio thread to complete (max 2000ms)
            auto tWaitStart = std::chrono::high_resolution_clock::now();
            bool waitOk = done.wait(2000);
            auto tWaitEnd = std::chrono::high_resolution_clock::now();
            auto waitUs = std::chrono::duration_cast<std::chrono::microseconds>(tWaitEnd - tWaitStart).count();
            post("batch_atomic: done.wait() took %lld us (%.1f ms)", (long long)waitUs, waitUs / 1000.0f);
            // R3b — if the audio thread did not finish in 2000ms, the batch
            // state is UNKNOWN (partial mutation). Do NOT send a success-shaped
            // reply (it would look like a normal result and the TS retry would
            // double-apply). Send an explicit error address.
            if (!waitOk) {
                juce::OSCMessage errReply { juce::OSCAddressPattern("/pd/batch_atomic/error/" + correlationId) };
                errReply.addArgument(juce::String("busy: audio thread did not finish in 2000ms — batch state UNKNOWN"));
                sender.send(errReply);
                return;
            }

            // PRD_CONTEXT_LAYOUT_GUARD (P2/P3/P3.1): layout guard on the JUCE
            // message thread. Deoverlap + occlusion rounds (fast, Pd coords),
            // then a GUARANTEED fallback: if true-rect collisions remain,
            // compose the canvas by role (proven collision-free).
            if (cnv && autoLayout) {
                auto runLayout = [&]() {
                    const int guardPad = layoutPad + 15;
                    for (int round = 0; round < 8; round++) {
                        sys_lock();
                        int c = sanitizeLayout(processor, cnv, guardPad, layoutSnap);
                        int o = autoFixOcclusions ? fixOcclusions(processor, cnv, guardPad, layoutSnap) : 0;
                        sys_unlock();
                        layoutSanitizedMoved += c;
                        occlusionsFixed += o;
                        if (c == 0 && o == 0) break;
                    }
                    if (mcpHasCollisions(processor, cnv, 5)) {
                        sys_lock();
                        int cm = composeLayout(processor, cnv, canvasName, layoutPad, layoutSnap);
                        sys_unlock();
                        layoutSanitizedMoved += cm;
                        fallbackComposed = true;
                    }
                    processor->synchroniseCanvases();
                };

                if (juce::MessageManager::getInstance()->isThisTheMessageThread()) {
                    runLayout();
                } else {
                    juce::WaitableEvent sanitizeDone;
                    juce::MessageManager::callAsync([&runLayout, &sanitizeDone]() {
                        runLayout();
                        sanitizeDone.signal();
                    });
                    sanitizeDone.wait(6000);
                }
            } else {
                if (juce::MessageManager::getInstance()->isThisTheMessageThread()) {
                    if (processor) processor->synchroniseCanvases();
                } else {
                    // Decoupled UI viewport sync - non-blocking async idle dispatch
                    juce::MessageManager::callAsync([p = processor] {
                        if (p) p->synchroniseCanvases();
                    });
                }
            }

            // MCP mutations never register correct Pd undo actions, yet they
            // still reorder/free objects that pre-existing index-based undo
            // entries point at (delete shifts indices; encapsulate/GOP/refactor
            // and clear/load replace objects outright). Any such entry left on
            // the queue makes the NEXT undo/redo dereference freed memory — the
            // recurring "unsupported undo command <garbage int>" segfault.
            // Flushing only on deletes was NOT enough: a create/edit/move after
            // a delete re-populates the queue with entries that a later
            // object-freeing op invalidates. The only provably-safe state for
            // the queue after ANY MCP mutation is EMPTY.
            bool undoReset = false;
            if (created > 0 || connected > 0 || edited > 0 || deleted > 0 || disconnected > 0 || layoutSanitizedMoved > 0 || occlusionsFixed > 0) {
                resetCanvasUndo(processor, canvasName);
                undoReset = true;
            }

            // Stage 2: Register with Unified C++ Transaction Engine for GUI Ctrl+Z / Undo
            if (processor && cnv && !processor->isExecutingMcpUndoRedo) {
                if (created > 0 || connected > 0 || disconnected > 0) {
                    juce::OSCMessage invMsg { juce::OSCAddressPattern("/pd/batch_atomic") };
                    invMsg.addArgument(canvasName);
                    invMsg.addArgument(juce::String("inv_" + correlationId));

                    // Header counts: deletes, disconnects, edits, creates, connects
                    invMsg.addArgument(static_cast<float>(pendingCreates.size())); // deleteCount
                    invMsg.addArgument(static_cast<float>(allConns.size()));        // disconnectCount
                    invMsg.addArgument(0.0f);                                      // editCount
                    invMsg.addArgument(0.0f);                                      // createCount
                    invMsg.addArgument(static_cast<float>(preDisconnects.size())); // connectCount

                    // 1. DELETES: delete all objects created in pendingCreates
                    for (auto const& pc : pendingCreates) {
                        invMsg.addArgument(pc.tempId);
                    }

                    // 2. DISCONNECTS: disconnect all connections made in allConns
                    for (auto const& c : allConns) {
                        invMsg.addArgument(c.srcId);
                        invMsg.addArgument(static_cast<float>(c.srcOut));
                        invMsg.addArgument(c.destId);
                        invMsg.addArgument(static_cast<float>(c.destIn));
                    }

                    // 3. EDITS: 0
                    // 4. CREATES: 0

                    // 5. CONNECTS: reconnect all wires disconnected in preDisconnects
                    for (auto const& pdc : preDisconnects) {
                        invMsg.addArgument(pdc.srcId);
                        invMsg.addArgument(static_cast<float>(pdc.srcOut));
                        invMsg.addArgument(pdc.destId);
                        invMsg.addArgument(static_cast<float>(pdc.destIn));
                    }

                    // Layout flags: no autolayout on undo
                    invMsg.addArgument(0.0f);
                    invMsg.addArgument(static_cast<float>(layoutPad));
                    invMsg.addArgument(static_cast<float>(layoutSnap));
                    invMsg.addArgument(0.0f);

                    processor->pushMcpTransaction(cnv, canvasName, msg, invMsg);
                }
            }

            // Build reply with counts and inline identity mappings
            juce::OSCMessage reply { juce::OSCAddressPattern("/pd/batch_atomic/reply/" + correlationId) };
            reply.addArgument(static_cast<int32>(created));
            reply.addArgument(static_cast<int32>(connected));
            reply.addArgument(static_cast<int32>(edited));
            reply.addArgument(static_cast<int32>(deleted));
            reply.addArgument(static_cast<int32>(disconnected));
            // PHASE 0 reconcile counts (appended — backward compatible)
            reply.addArgument(static_cast<int32>(reconcileEvicted));
            reply.addArgument(static_cast<int32>(reconcileAdopted));

            // Append inline mappings — built from data collected in lambda
            for (size_t i = 0; i < createdIds.size() && i < mappingIndices.size(); i++) {
                if (mappingIndices[i] >= 0) {
                    reply.addArgument(juce::String(createdIds[i]));
                    reply.addArgument(static_cast<int32>(mappingIndices[i]));
                }
            }

            // Append PHASE 0 detected connections as a compact JSON string.
            // Format: [{"s":"srcId","so":0,"d":"destId","di":0}, ...]
            // ALWAYS appended (empty "[]" when no wires) — the TS parser relies
            // on this marker to locate the failure-fact section that follows.
            {
                juce::String dcJson = "[";
                for (size_t i = 0; i < detectedConnections.size(); i++) {
                    auto& dc = detectedConnections[i];
                    if (i > 0) dcJson += ",";
                    dcJson += "{\"s\":\"" + juce::String(dc.srcId)
                           + "\",\"so\":" + juce::String(dc.srcOut)
                           + ",\"d\":\"" + juce::String(dc.destId)
                           + "\",\"di\":" + juce::String(dc.destIn) + "}";
                }
                dcJson += "]";
                reply.addArgument(dcJson);
            }

            // Phase A (PRD diagnostic layer): create/connect failure facts —
            // appended VERY LAST, after the detected-connections JSON. Old TS
            // clients stop parsing at the JSON and ignore these; new TS clients
            // read them as named failures (PRD diagnostic layer §2.1).
            reply.addArgument(static_cast<int32>(createFailures.size()));
            for (auto& cf : createFailures) {
                reply.addArgument(juce::String(cf.tempId));
                reply.addArgument(juce::String(cf.type));
                reply.addArgument(juce::String(cf.reason));
            }
            reply.addArgument(static_cast<int32>(connectFailures.size()));
            for (auto& cf : connectFailures) {
                reply.addArgument(juce::String(cf.srcId) + juce::String("->") + juce::String(cf.destId));
                reply.addArgument(juce::String(cf.reason));
            }

            // Inline conditional X-ray: if any created object has a signal type (~),
            // compute diagnose facts in the audio thread lambda and append to reply.
            bool hasSignalCreate = false;
            for (auto& pc : pendingCreates) {
                if (pc.objType.contains("~")) { hasSignalCreate = true; break; }
            }
            juce::String diagJson;
            if (hasSignalCreate && cnv) {
                sys_lock();
                diagJson = computeDiagnoseFacts(processor, cnv, canvasName);
                sys_unlock();
            }
            reply.addArgument(diagJson);

            // Bug #11 tail fact (appended LAST, after diagnose JSON): 1 when the
            // requested canvas symbol did not resolve and the mutation was
            // refused. Old TS clients ignore trailing args; new clients gate on it.
            reply.addArgument(static_cast<int32>(canvasNotFound ? 1 : 0));
            if (canvasNotFound)
                reply.addArgument(canvasName); // the name that failed, for the receipt

            // Undo-safety tail fact (appended LAST): 1 when this batch deleted
            // objects and therefore flushed the Pd undo queue (MCP deletes never
            // register undo actions, so the queue must be emptied to stay
            // crash-safe). Old TS clients ignore the extra trailing atom; new
            // clients surface it as _v2meta.undoReset so the AI knows native
            // undo cannot step back past this mutation.
            reply.addArgument(static_cast<int32>(undoReset ? 1 : 0));

            // Layout-sanitize tail fact (appended LAST, PRD_CONTEXT_LAYOUT_GUARD
            // P2): number of objects the inline deoverlap moved (0 = clean).
            reply.addArgument(static_cast<int32>(layoutSanitizedMoved));
            // Occlusion-fix tail fact (P3.1): objects nudged off wire paths.
            reply.addArgument(static_cast<int32>(occlusionsFixed));
            // Fallback tail fact: 1 if the guard fell back to compose.
            reply.addArgument(static_cast<int32>(fallbackComposed ? 1 : 0));

            sender.send(reply);
            // R3a — store reply in dedup cache (LRU + TTL, for retry idempotency).
            if (!processor->isExecutingMcpUndoRedo) {
                uint64_t nowMs = juce::Time::currentTimeMillis();
                std::string corrKey = correlationId.toStdString();
                s_batchDedupCache.insert_or_assign(corrKey, BatchDedupEntry{nowMs, reply});
                s_batchDedupOrder.push_back(corrKey);
                while ((int)s_batchDedupOrder.size() > BATCH_DEDUP_MAX) {
                    std::string oldest = s_batchDedupOrder.front();
                    s_batchDedupOrder.pop_front();
                    s_batchDedupCache.erase(oldest);
                }
            }
        }
        return;
    }

    if (action == "connections") {
        // /pd/connections <canvasName> <correlationId>
        // Returns JSON array of all connections on the canvas.
        // Uses linetraverser + single O(n) ptr→index map. No file dump.
        // Response: /pd/connections/reply/<corrId> <jsonString>
        // JSON: { "canvas":"pd-main", "count": N, "connections": [
        //   { "srcIndex":0, "srcOut":0, "destIndex":1, "destIn":0, "srcId":"osc", "destId":"dac" }, ...
        // ]}
        if (msg.size() >= 2 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);

            sys_lock();
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

            if (!cnv) {
                sys_unlock();
                sendReply("/pd/connections/error/" + correlationId, "Canvas not found: " + canvasName);
                return;
            }

            // Build ptr→index map with single O(n) walk
            std::unordered_map<t_gobj*, int> ptrToIndex;
            int idx = 0;
            for (t_gobj* y = cnv->gl_list; y; y = y->g_next) {
                ptrToIndex[y] = idx++;
            }

            // Build reverse identity map (ptr → tempId)
            auto canvasStr = canvasName.toStdString();
            std::unordered_map<t_gobj*, juce::String> ptrToId;
            if (processor->mcpStableObjectMap.count(canvasStr)) {
                for (auto const& [id, ptr] : processor->mcpStableObjectMap[canvasStr]) {
                    if (ptr) ptrToId[ptr] = juce::String(id);
                }
            }

            // Walk connections with linetraverser
            auto* rootObj = new juce::DynamicObject();
            rootObj->setProperty("canvas", canvasName);

            juce::Array<juce::var> connArray;
            int connCount = 0;

            t_linetraverser t;
            linetraverser_start(&t, cnv);
            t_outconnect* oc = nullptr;
            while ((oc = linetraverser_next_nosize(&t))) {
                t_gobj* srcGobj = &t.tr_ob->ob_g;
                t_gobj* destGobj = &t.tr_ob2->ob_g;

                auto srcIt = ptrToIndex.find(srcGobj);
                auto destIt = ptrToIndex.find(destGobj);
                if (srcIt == ptrToIndex.end() || destIt == ptrToIndex.end()) continue;

                bool srcIsSig = (t.tr_outlet && t.tr_outlet->o_sym == gensym("signal"));
                bool destIsSig = (t.tr_ob2 && obj_issignalinlet(t.tr_ob2, t.tr_inno) != 0);
                juce::String rate = (srcIsSig || destIsSig) ? "sig" : "ctl";

                auto* c = new juce::DynamicObject();
                c->setProperty("srcIndex", srcIt->second);
                c->setProperty("srcOut", t.tr_outno);
                c->setProperty("destIndex", destIt->second);
                c->setProperty("destIn", t.tr_inno);
                c->setProperty("srcId", ptrToId.count(srcGobj) ? ptrToId[srcGobj] : juce::String());
                c->setProperty("destId", ptrToId.count(destGobj) ? ptrToId[destGobj] : juce::String());
                c->setProperty("rate", rate);

                connArray.add(juce::var(c));
                connCount++;
            }

            rootObj->setProperty("count", connCount);
            rootObj->setProperty("connections", connArray);

            juce::String jsonString = juce::JSON::toString(juce::var(rootObj), true);
            sys_unlock();

            juce::OSCMessage reply { juce::OSCAddressPattern("/pd/connections/reply/" + correlationId) };
            reply.addArgument(jsonString);
            sender.send(reply);
        }
        return;
    }

    if (action == "dump") {
        if (msg.size() >= 3) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto dumpFile = getArgString(msg[1]);
            auto dumpDir = getArgString(msg[2]);
            auto correlationId = msg.size() > 3 ? getArgString(msg[3]) : "";

            sys_lock();
            t_canvas* cnv = processor ? processor->getCanvasBySymbol(canvasName) : nullptr;
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

            if (cnv) {
                t_binbuf* b = binbuf_new();
                canvas_savetemplatesto(cnv, b, 1);
                canvas_saveto(cnv, b);
                binbuf_write(b, dumpFile.toRawUTF8(), dumpDir.toRawUTF8(), 0);
                binbuf_free(b);
            }
            sys_unlock();

            juce::String replyAddress = correlationId.isNotEmpty() ? "/pd/dumped/" + correlationId : "/pd/dumped";
            sendReply(replyAddress, dumpFile);
        }
        return;
    }

    if (action == "load") {
        if (msg.size() >= 2 && processor) {
            auto file = getArgString(msg[0]);
            auto folder = getArgString(msg[1]);
            sys_lock();
            t_atom args[2];
            SETSYMBOL(&args[0], gensym(file.toRawUTF8()));
            SETSYMBOL(&args[1], gensym(folder.toRawUTF8()));
            pd_typedmess(gensym("pd")->s_thing, gensym("open"), 2, args);
            sys_unlock();
            sendRawReply("/pd/loaded");
        }
        return;
    }

    // /pd/save_content <canvasName> <destFilePath> [correlationId]
    // Serialises the live canvas to a .pd file using getCanvasContent() — pure
    // in-memory path, no binbuf_write overhead. Faster than /pd/dump and does
    // not require a separate dumpDir argument.
    // Reply: /pd/save_content/reply/<corrId>  arg0=destFilePath  (or error string)
    if (action == "save_content") {
        if (msg.size() >= 2 && processor) {
            auto canvasName    = normalizeCanvas(getArgString(msg[0]));
            auto destFilePath  = getArgString(msg[1]);
            auto correlationId = msg.size() > 2 ? getArgString(msg[2]) : "";

            juce::String content;
            sys_lock();
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
            if (cnv) {
                char* buf = nullptr;
                int   bufsize = 0;
                pd::Interface::getCanvasContent(cnv, &buf, &bufsize);
                if (buf) {
                    content = juce::String::fromUTF8(buf, static_cast<size_t>(bufsize));
                    freebytes(buf, static_cast<size_t>(bufsize) * sizeof(char));
                }
            }
            sys_unlock();

            juce::String replyAddr = correlationId.isNotEmpty()
                ? "/pd/save_content/reply/" + correlationId
                : "/pd/save_content/reply";

            if (content.isEmpty()) {
                sendReply(replyAddr, juce::String("error: canvas not found or empty"));
                return;
            }

            // Write content to destination path provided by TS
            juce::File destFile(destFilePath);
            if (!destFile.replaceWithText(content)) {
                sendReply(replyAddr, juce::String("error: could not write to " + destFilePath));
                return;
            }

            sendReply(replyAddr, destFilePath);
        }
        return;
    }

     // /pd/record_start <filePath> [corrId]
    // Zero-dropout WAV recorder — taps processBlock output buffer directly.
    // No canvas objects created, no DSP recompile, no audio dropout.
    // The WAV writer is created lazily on the audio thread (first tap) so its
    // channel count always matches the real output buffer — getTotalNumOutputChannels()
    // can report a bus layout (e.g. 32ch) that differs from the actual buffer (e.g. 2ch).
    // Reply: /pd/record_start/reply/<corrId>  1.0=ok, 0.0=failed
    if (action == "record_start") {
        if (msg.size() >= 1 && processor) {
            auto filePath      = getArgString(msg[0]);
            auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";
            juce::String replyAddr = "/pd/record_start/reply/" + correlationId;

            // Fail fast if we can't create the parent directory.
            juce::File destFile(filePath);
            auto parent = destFile.getParentDirectory();
            if (!parent.createDirectory().wasOk()) {
                post("MCP record_start: could not create directory %s", parent.getFullPathName().toRawUTF8());
                sendReply(replyAddr, 0.0f);
                return;
            }

            {
                const juce::ScopedLock sl(processor->mcpRecorderLock);
                processor->mcpWavWriter.reset();      // drop any stale writer
                processor->mcpRecorderPath = filePath;
                processor->mcpRecording.store(true, std::memory_order_release);
            }
            post("MCP record_start: armed (lazy writer) path=%s", filePath.toRawUTF8());
            sendReply(replyAddr, 1.0f);
        }
        return;
    }

    // /pd/record_stop [corrId]
    // Stop the zero-dropout WAV recorder and flush/close the file.
    // Reply: /pd/record_stop/reply/<corrId>  1.0=ok
    if (action == "record_stop") {
        auto correlationId = msg.size() > 0 ? getArgString(msg[0]) : "0";
        if (processor) {
            processor->mcpRecording.store(false, std::memory_order_release);
            {
                const juce::ScopedLock sl(processor->mcpRecorderLock);
                processor->mcpWavWriter.reset(); // flushes and closes the file
            }
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
    if (action == "render") {
        if (msg.size() >= 2 && processor) {
            auto filePath      = getArgString(msg[0]);
            float durationSec  = getArgFloat(msg[1]);
            bool analyze       = (msg.size() >= 3) ? (getArgFloat(msg[2]) > 0.5f) : false;
            auto correlationId = (msg.size() >= 4) ? getArgString(msg[3]) : "0";

            durationSec = juce::jlimit(0.1f, 60.0f, durationSec);

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


    // /pd/load_content <canvasName> <srcFilePath> [correlationId]
    // Clears the canvas and reconstructs it atomically from a .pd file using
    // binbuf_read + binbuf_eval — Pd's native file loader. Handles the full
    // .pd format (including #N canvas header) correctly.
    // DSP recompiles once at the end. Disk read is outside sys_lock.
    // Reply: /pd/load_content/reply/<corrId>  arg0=objectCount (float)
    if (action == "load_content") {
        if (msg.size() >= 2 && processor) {
            auto canvasName    = normalizeCanvas(getArgString(msg[0]));
            auto srcFilePath   = getArgString(msg[1]);
            auto correlationId = msg.size() > 2 ? getArgString(msg[2]) : "";

            juce::String replyAddr = correlationId.isNotEmpty()
                ? "/pd/load_content/reply/" + correlationId
                : "/pd/load_content/reply";

            juce::File srcFile(srcFilePath);
            if (!srcFile.existsAsFile()) {
                sendReply(replyAddr, -1.0f);
                return;
            }

            // Split into dir + filename for binbuf_read
            auto fileDir  = srcFile.getParentDirectory().getFullPathName();
            auto fileName = srcFile.getFileName();

            int objectCount = 0;

            // Stop all active probes before clearing — they hold t_outconnect*
            // pointers that become dangling after canvas clear.
            probeManager.stopAllProbes("load_content_reset");

            sys_lock();
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
            if (cnv) {
                // 1. Clear existing objects
                pd::Interface::clearCanvasAudioThread(cnv);

                // 2. Load .pd file — strip the #N canvas header line then
                //    use pasteDirect() which correctly handles both #X obj
                //    and #X connect lines via binbuf_eval with sym_X bound
                //    to the canvas. This is the same path batch_atomic uses.
                t_binbuf* b = binbuf_new();
                if (binbuf_read(b, fileName.toRawUTF8(), fileDir.toRawUTF8(), 0) == 0) {
                    // Find the end of the first statement (#N canvas ... ;)
                    // and build a new binbuf from the rest (#X lines only).
                    int natom = binbuf_getnatom(b);
                    t_atom* vec = binbuf_getvec(b);
                    int startIdx = 0;
                    for (int i = 0; i < natom; i++) {
                        if (vec[i].a_type == A_SEMI) { startIdx = i + 1; break; }
                    }
                    if (startIdx < natom) {
                        // Serialise remaining atoms back to text for pasteDirect
                        t_binbuf* body = binbuf_new();
                        binbuf_add(body, natom - startIdx, vec + startIdx);
                        char* textBuf = nullptr;
                        int   textLen = 0;
                        binbuf_gettext(body, &textBuf, &textLen);
                        if (textBuf && textLen > 0) {
                            // pasteDirect binds sym_X to canvas — handles both
                            // #X obj and #X connect correctly
                            pasteDirect(cnv, juce::String::fromUTF8(textBuf, textLen).toRawUTF8());
                            freebytes(textBuf, textLen);
                        }
                        binbuf_free(body);
                    }
                }
                binbuf_free(b);

                // 3. Fire loadbangs on any newly created subpatches
                for (t_gobj* g = cnv->gl_list; g; g = g->g_next) {
                    if (pd_class(&g->g_pd) == canvas_class)
                         canvas_loadbang(reinterpret_cast<t_canvas*>(g));
                }

                // 4. DSP graph recompile
                canvas_update_dsp();

                // Count objects
                for (t_gobj* g = cnv->gl_list; g; g = g->g_next)
                    objectCount++;
            }
            sys_unlock();

            // All prior objects were freed and replaced — their index-based undo
            // entries are now dangling. Drop the queue before anything can undo.
            resetCanvasUndo(processor, canvasName);

            // Synchronise canvas UI and restart DSP.
            // startDSP() is called directly here — same pattern as /pd/dsp
            // handler which also calls it from the OSC receiver thread.
            processor->enqueueFunctionAsync([p = processor] { p->synchroniseCanvases(); });

            if (objectCount > 0 && processor)
                processor->startDSP();

            // 5. Clear identity map — PHASE 0 reconcile fires on next batch_atomic
            SmallArray<pd::Atom> atoms;
            atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
            atoms.add(pd::Atom(processor->generateSymbol("0")));
            processor->receiveSysMessage("mcp_clear_ids", atoms);

            processor->enqueueFunctionAsync([p = processor] { p->synchroniseCanvases(); });

            // arg0 = objectCount, arg1 = undoReset (clear+rebuild flushed the
            // stale undo queue). Old TS clients read arg0 and ignore arg1.
            juce::Array<juce::var> loadReplyArgs;
            loadReplyArgs.add(static_cast<double>(objectCount));
            loadReplyArgs.add(1.0);
            sendReply(replyAddr, loadReplyArgs);
        }
        return;
    }

    // ── PRD Phase 3 §2.1: NATIVE SAVE WITH IDENTITY SIDECAR ─────────────
    // /pd/save_patch <canvas> <filePath> [corrId]
    // C++ owns the save: getCanvasContent writes the .pd, and a sidecar
    // (<file>.mcpids.json) carries the stable tempId map AT SAVE TIME —
    // index→tempId for the root canvas plus every named subcanvas. Identity
    // never crosses the TS boundary, so a later load can restore it exactly
    // (no positional guessing). Vanilla Pd sees a normal .pd file.
    // Reply: /pd/save_patch/reply/<corrId> <destFilePath> | "error: ..."
    if (action == "save_patch") {
        if (msg.size() >= 2 && processor) {
            auto canvasName    = normalizeCanvas(getArgString(msg[0]));
            auto destFilePath  = getArgString(msg[1]);
            auto correlationId = msg.size() > 2 ? getArgString(msg[2]) : "";

            juce::String replyAddr = correlationId.isNotEmpty()
                ? "/pd/save_patch/reply/" + correlationId
                : "/pd/save_patch/reply";

            juce::String content;
            juce::var sidecar;

            sys_lock();
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
            if (cnv) {
                char* buf = nullptr;
                int   bufsize = 0;
                pd::Interface::getCanvasContent(cnv, &buf, &bufsize);
                if (buf) {
                    content = juce::String::fromUTF8(buf, static_cast<size_t>(bufsize));
                    freebytes(buf, static_cast<size_t>(bufsize) * sizeof(char));
                }

                if (content.isNotEmpty()) {
                    // Identity sidecar: reverse map (gobj → tempId) per canvas
                    auto buildIdArray = [&](const juce::String& mapKey, t_canvas* c) -> juce::var {
                        juce::Array<juce::var> arr;
                        std::unordered_map<t_gobj*, juce::String> ptrToId;
                        auto mapIt = processor->mcpStableObjectMap.find(mapKey.toStdString());
                        if (mapIt != processor->mcpStableObjectMap.end()) {
                            for (auto const& [id, ptr] : mapIt->second)
                                if (ptr) ptrToId[ptr] = juce::String(id);
                        }
                        int idx = 0;
                        for (t_gobj* g = c->gl_list; g; g = g->g_next, ++idx) {
                            auto it = ptrToId.find(g);
                            if (it == ptrToId.end()) continue;
                            auto* o = new juce::DynamicObject();
                            o->setProperty("i", idx);
                            o->setProperty("id", it->second);
                            arr.add(juce::var(o));
                        }
                        return juce::var(arr);
                    };

                    auto* root = new juce::DynamicObject();
                    root->setProperty("format", juce::String("MCP-IDENTITY"));
                    root->setProperty("v", 1);
                    root->setProperty("identity_version",
                        (double)processor->mcpIdentityVersion.load(std::memory_order_relaxed));
                    root->setProperty("root", buildIdArray(canvasName, cnv));

                    // Named subcanvases: key "pd-<gl_name>" matches the map keys
                    // the bridge uses for subpatch census/mutations.
                    auto* subObj = new juce::DynamicObject();
                    for (t_gobj* g = cnv->gl_list; g; g = g->g_next) {
                        if (pd_class(&g->g_pd) != canvas_class) continue;
                        t_canvas* child = reinterpret_cast<t_canvas*>(g);
                        if (!child->gl_name) continue;
                        juce::String childName = juce::String::fromUTF8(child->gl_name->s_name);
                        auto childIds = buildIdArray("pd-" + childName, child);
                        if (childIds.getArray() != nullptr && childIds.getArray()->size() > 0)
                            subObj->setProperty(childName, childIds);
                    }
                    root->setProperty("sub", juce::var(subObj));
                    sidecar = juce::var(root);
                }
            }
            sys_unlock();

            if (content.isEmpty()) {
                sendReply(replyAddr, juce::String("error: canvas not found or empty"));
                return;
            }

            juce::File destFile(destFilePath);
            if (!destFile.replaceWithText(content)) {
                sendReply(replyAddr, juce::String("error: could not write to " + destFilePath));
                return;
            }
            // Sidecar next to the .pd — engine-private, invisible to vanilla Pd
            juce::File sidecarFile(destFilePath + ".mcpids.json");
            sidecarFile.replaceWithText(juce::JSON::toString(sidecar, true));

            // PRD Phase 3 §2.5 (save-binding): bind the file to the canvas the
            // way native Save-As does — tab title + current path update.
            // NOTE: deliberately setCurrentFile ONLY — Patch::savePatch(URL)
            // also runs reloadAbstractions() (recreates abstraction instances)
            // and crashed when a queued close_tab tore the canvas down mid-
            // reload. Title derives from currentFile; pd-side state untouched.
            processor->enqueueFunctionAsync([p = processor, cnv, destFilePath]() {
                juce::File f(destFilePath);
                for (auto* editor : p->getEditors()) {
                    if (!editor) continue;
                    for (auto* canvas : editor->getCanvases()) {
                        if (canvas && canvas->patch.getPointer().get() == cnv) {
                            canvas->patch.setCurrentFile(URL(f));
                            canvas->patch.setTitle(f.getFileName());
                            return;
                        }
                    }
                }
            });

            sendReply(replyAddr, destFilePath);
        }
        return;
    }

    // ── PRD Phase 3 §2.2: NATIVE LOAD WITH IDENTITY RESTORE ─────────────
    // /pd/load_patch <canvas> <filePath> [corrId]
    // Same canvas mechanics as load_content (clear + pasteDirect + loadbang +
    // canvas_update_dsp), then re-registers the sidecar tempIds DIRECTLY into
    // the stable map (root + named subcanvases) and bumps the version once.
    // Objects without a sidecar entry are auto-adopted by the Phase 2 census
    // naming on next census. No TS involvement, no positional guessing.
    // Reply: /pd/load_patch/reply/<corrId> <objectCount> (float, -1 = file missing)
    if (action == "load_patch") {
        if (msg.size() >= 2 && processor) {
            auto canvasName    = normalizeCanvas(getArgString(msg[0]));
            auto srcFilePath   = getArgString(msg[1]);
            auto correlationId = msg.size() > 2 ? getArgString(msg[2]) : "";

            juce::String replyAddr = correlationId.isNotEmpty()
                ? "/pd/load_patch/reply/" + correlationId
                : "/pd/load_patch/reply";

            juce::File srcFile(srcFilePath);
            if (!srcFile.existsAsFile()) {
                sendReply(replyAddr, -1.0f);
                return;
            }

            // Sidecar read OUTSIDE sys_lock (small file, best-effort)
            juce::var sidecar;
            juce::File sidecarFile(srcFilePath + ".mcpids.json");
            if (sidecarFile.existsAsFile()) {
                auto parsed = juce::JSON::parse(sidecarFile.loadFileAsString());
                if (auto* parsedObj = parsed.getDynamicObject()) {
                    if (parsedObj->getProperty("format") == juce::String("MCP-IDENTITY")) {
                        sidecar = parsed;
                    }
                }
            }

            auto fileDir  = srcFile.getParentDirectory().getFullPathName();
            auto fileName = srcFile.getFileName();

            // Prepare the paste body OFF the message thread (pure file I/O).
            juce::String bodyText;
            {
                t_binbuf* b = binbuf_new();
                if (binbuf_read(b, fileName.toRawUTF8(), fileDir.toRawUTF8(), 0) == 0) {
                    int natom = binbuf_getnatom(b);
                    t_atom* vec = binbuf_getvec(b);
                    int startIdx = 0;
                    for (int i = 0; i < natom; i++) {
                        if (vec[i].a_type == A_SEMI) { startIdx = i + 1; break; }
                    }
                    if (startIdx < natom) {
                        t_binbuf* body = binbuf_new();
                        binbuf_add(body, natom - startIdx, vec + startIdx);
                        char* textBuf = nullptr;
                        int   textLen = 0;
                        binbuf_gettext(body, &textBuf, &textLen);
                        if (textBuf && textLen > 0) {
                            bodyText = juce::String::fromUTF8(textBuf, textLen);
                            freebytes(textBuf, textLen);
                        }
                        binbuf_free(body);
                    }
                }
                binbuf_free(b);
            }

            probeManager.stopAllProbes("load_patch_reset");

            // The canvas mutation MUST run on the JUCE message thread:
            // pasteDirect creates Pd objects — GUI objects, abstractions and
            // GOP [cnv] — which instantiate JUCE Components. Creating those off
            // the message thread is illegal and was the intermittent load
            // crash. Same callAsync pattern as the native encapsulate handlers.
            juce::MessageManager::callAsync(
                [proc = processor, canvasName, bodyText, sidecar, replyAddr, bridge = this]() {
                    int objectCount = 0;
                    sys_lock();
                    t_canvas* cnv = proc->getCanvasBySymbol(canvasName);
                    if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
                    if (cnv) {
                        // 0. Reset identity for this canvas tree BEFORE re-registering
                        proc->mcpStableObjectMap[canvasName.toStdString()].clear();

                        // 1. Clear existing objects (message-thread-safe)
                        pd::Interface::clearCanvas(cnv);

                        // 2. Paste the prepared body
                        if (bodyText.isNotEmpty())
                            pasteDirect(cnv, bodyText.toRawUTF8());

                        // 3. Loadbangs on newly created subpatches
                        for (t_gobj* g = cnv->gl_list; g; g = g->g_next) {
                            if (pd_class(&g->g_pd) == canvas_class)
                                canvas_loadbang(reinterpret_cast<t_canvas*>(g));
                        }

                        // 4. DSP graph recompile
                        canvas_update_dsp();

                        // 5. Re-register sidecar identities
                        if (auto* sidecarObj = sidecar.getDynamicObject())
                            MCPBridge::mcpApplyIdentitySidecar(proc, sidecarObj, canvasName, cnv);

                        for (t_gobj* g = cnv->gl_list; g; g = g->g_next)
                            objectCount++;
                    }
                    sys_unlock();

                    proc->synchroniseCanvases();
                    if (objectCount > 0) proc->startDSP();
                    // Clear/free replaced every prior object — its index-based
                    // undo entries now dangle. Reset the queue to stay crash-safe.
                    resetCanvasUndo(proc, canvasName);

                    // arg0 = objectCount, arg1 = undoReset.
                    juce::Array<juce::var> loadReplyArgs;
                    loadReplyArgs.add(static_cast<double>(objectCount));
                    loadReplyArgs.add(1.0);
                    bridge->sendReply(replyAddr, loadReplyArgs);
                });
        }
        return;
    }

    // ── PRD Phase 3 §2.5: OPEN PATCH IN NEW TAB ─────────────────────────
    // /pd/open_patch <filePath> [corrId]
    // Opens a .pd file in a NEW editor tab (native File→Open path), then
    // pre-registers the sidecar identities if present — the tab comes up
    // fully named. Reply: /pd/open_patch/reply/<corrId> "<status text>"
    if (action == "open_patch") {
        if (msg.size() >= 2 && processor) {
            auto canvasName    = normalizeCanvas(getArgString(msg[0]));
            auto srcFilePath   = getArgString(msg[1]);
            auto correlationId = msg.size() > 2 ? getArgString(msg[2]) : "0";
            juce::String replyAddr = "/pd/open_patch/reply/" + correlationId;

            juce::File srcFile(srcFilePath);
            if (!srcFile.existsAsFile()) {
                sendReply(replyAddr, juce::String("error: file not found: " + srcFilePath));
                return;
            }

            // Phase 3.1 bugfix: the open lambda was observed to deliver twice
            // (two tabs for one request). Once-guard per file — any repeat
            // within 2s is ignored (idempotent open, root cause independent).
            static std::unordered_map<juce::String, juce::int64> lastOpenAtMs;
            auto nowMs = juce::Time::getMillisecondCounter();
            auto& lastRef = lastOpenAtMs[srcFile.getFullPathName()];
            if (nowMs - lastRef < 2000) {
                sendReply(replyAddr, juce::String("ignored: open already in progress for " + srcFilePath));
                return;
            }
            lastRef = nowMs;

            // PRD Phase 3 §2.5: duplicate-tab guard — PlugData focuses the
            // existing tab instead of duplicating a patch (by design). Reply
            // honestly instead of falling into the async no-reply path.
            bool alreadyOpen = false;
            {
                sys_lock();
                for (auto* editor : processor->getEditors()) {
                    if (!editor) continue;
                    for (auto* canvas : editor->getCanvases()) {
                        if (canvas && canvas->patch.getCurrentFile() == srcFile) {
                            alreadyOpen = true;
                            break;
                        }
                    }
                    if (alreadyOpen) break;
                }
                sys_unlock();
            }
            if (alreadyOpen) {
                sendReply(replyAddr, juce::String("already open: " + srcFile.getFullPathName() + " (focused existing tab)"));
                return;
            }

            // Sidecar read outside the message-thread hop
            juce::var sidecar;
            juce::File sidecarFile(srcFile.getFullPathName() + ".mcpids.json");
            if (sidecarFile.existsAsFile()) {
                auto parsed = juce::JSON::parse(sidecarFile.loadFileAsString());
                if (auto* parsedObj = parsed.getDynamicObject()) {
                    if (parsedObj->getProperty("format") == juce::String("MCP-IDENTITY"))
                        sidecar = parsed;
                }
            }

            // Acknowledge immediately — the tab-open runs on the message
            // thread (fire-and-forget); TS verifies via `tabs`/census.
            sendReply(replyAddr, juce::String("opening: " + srcFile.getFullPathName()));

            processor->enqueueFunctionAsync(
                [p = processor, fileStr = srcFile.getFullPathName(), sidecar]() {
                bool opened = false;
                for (auto* editor : p->getEditors()) {
                    if (!editor) continue;
                    editor->getTabComponent().openPatch(juce::URL(juce::File(fileStr)));
                    opened = true;
                    break;
                }
                if (!opened) {
                    p->logMessage("MCP open_patch: no editor");
                    return;
                }

                // Phase 3.1 bugfix: belt-and-suspenders dedup — if more than
                // one tab for this file exists, keep the first and close extras.
                juce::File f(fileStr);
                bool keptFirst = false;
                for (auto* editor : p->getEditors()) {
                    if (!editor) continue;
                    for (auto* canvas : editor->getCanvases()) {
                        if (!canvas || canvas->patch.getCurrentFile() != f) continue;
                        if (!keptFirst) { keptFirst = true; continue; }
                        editor->getTabComponent().closeTab(canvas);
                        p->logMessage("MCP open_patch: closed duplicate tab for " + fileStr);
                    }
                }

                // PRD Phase 3 §2.5: sidecar registration via the proven async
                // mcp_register_id path — NO sys_lock in this lambda (lock
                // inversion with the audio thread crashes the message thread).
                int queued = 0;
                if (auto* sidecarObj = sidecar.getDynamicObject()) {
                    auto queueRegisters = [&p, &queued](juce::var const& idArray, juce::String const& canvasKey, t_canvas* target) {
                        const auto* arr = idArray.getArray();
                        if (arr == nullptr) return;
                        int idx = 0;
                        for (t_gobj* g = target->gl_list; g; g = g->g_next, ++idx) {
                            // find the sidecar entry for this index
                            for (auto const& v : *arr) {
                                if (auto* o = v.getDynamicObject()) {
                                    if (static_cast<int>(static_cast<double>(o->getProperty("i"))) != idx) continue;
                                    juce::String id = o->getProperty("id").toString();
                                    if (id.isEmpty()) break;
                                    SmallArray<pd::Atom> atoms;
                                    atoms.add(pd::Atom(p->generateSymbol(canvasKey)));
                                    atoms.add(pd::Atom(p->generateSymbol("0")));
                                    atoms.add(pd::Atom(static_cast<float>(idx)));
                                    atoms.add(pd::Atom(p->generateSymbol(id)));
                                    p->receiveSysMessage("mcp_register_id", atoms);
                                    queued++;
                                    break;
                                }
                            }
                        }
                    };

                    juce::File f(fileStr);
                    for (auto* editor : p->getEditors()) {
                        if (!editor) continue;
                        for (auto* canvas : editor->getCanvases()) {
                            if (!canvas || canvas->patch.getCurrentFile() != f) continue;
                            if (auto* cnv = canvas->patch.getPointer().get()) {
                                // PRD Phase 3.1: register under the tab's bound
                                // name (gl_name = file name) so named-tab
                                // addressing finds these identities.
                                juce::String boundName = cnv->gl_name
                                    ? juce::String::fromUTF8(cnv->gl_name->s_name)
                                    : juce::String(f.getFileName());
                                queueRegisters(sidecarObj->getProperty("root"), boundName, cnv);
                                // named subcanvases
                                if (auto* subObj = sidecarObj->getProperty("sub").getDynamicObject()) {
                                    for (t_gobj* g = cnv->gl_list; g; g = g->g_next) {
                                        if (pd_class(&g->g_pd) != canvas_class) continue;
                                        auto* child = reinterpret_cast<t_canvas*>(g);
                                        if (!child->gl_name) continue;
                                        juce::String childName = juce::String::fromUTF8(child->gl_name->s_name);
                                        auto childIds = subObj->getProperty(juce::Identifier(childName));
                                        if (childIds.getArray() != nullptr && childIds.getArray()->size() > 0)
                                            queueRegisters(childIds, "pd-" + childName, child);
                                    }
                                }
                            }
                            break;
                        }
                        break;
                    }
                }

                p->logMessage(juce::String("MCP open_patch: ") + fileStr
                    + (queued > 0 ? " (" + juce::String(queued) + " identities queued)" : ""));
                (void)opened;
            });
        }
        return;
    }

    // ── PRD Phase 3 §2.5: LIST OPEN TABS ────────────────────────────────
    // /pd/list_tabs [corrId]
    // Enumerates every open root tab: title, file, focused flag, object
    // count. Reply: /pd/list_tabs/reply/<corrId> <json>
    if (action == "list_tabs") {
        auto correlationId = msg.size() > 0 ? getArgString(msg[0]) : "0";
        juce::String replyAddr = "/pd/list_tabs/reply/" + correlationId;

        processor->enqueueFunctionAsync([p = processor, bridge = this, replyAddr]() {
            juce::Array<juce::var> list;
            for (auto* editor : p->getEditors()) {
                if (!editor) continue;
                auto* focused = editor->getCurrentCanvas();
                for (auto* canvas : editor->getCanvases()) {
                    if (!canvas) continue;
                    auto* o = new juce::DynamicObject();
                    o->setProperty("title", canvas->patch.getTitle());
                    o->setProperty("file", canvas->patch.getCurrentFile().getFullPathName());
                    o->setProperty("focused", canvas == focused);
                    int count = 0;
                    sys_lock();
                    if (auto* cnv = canvas->patch.getPointer().get()) {
                        for (t_gobj* g = cnv->gl_list; g; g = g->g_next) count++;
                    }
                    sys_unlock();
                    o->setProperty("objects", count);
                    list.add(juce::var(o));
                }
            }
            auto* root = new juce::DynamicObject();
            root->setProperty("tabs", list);
            bridge->sendReply(replyAddr, juce::JSON::toString(juce::var(root)));
        });
        return;
    }

    // ── PRD Phase 3 §2.5: FOCUS TAB ─────────────────────────────────────
    // /pd/focus_tab <file-or-name> [corrId]
    // Makes the named tab the focused one — "main" then points at it.
    // Matches by full path or file name. Reply: .../reply/<corrId> "<status>"
    if (action == "focus_tab") {
        auto target       = msg.size() > 0 ? getArgString(msg[0]) : juce::String();
        auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";
        juce::String replyAddr = "/pd/focus_tab/reply/" + correlationId;

        processor->enqueueFunctionAsync([p = processor, bridge = this, target, replyAddr]() {
            juce::File f(target);
            for (auto* editor : p->getEditors()) {
                if (!editor) continue;
                for (auto* canvas : editor->getCanvases()) {
                    if (!canvas) continue;
                    bool match = canvas->patch.getCurrentFile() == f
                        || canvas->patch.getCurrentFile().getFileName() == target;
                    if (match) {
                        editor->getTabComponent().showTab(canvas, canvas->patch.splitViewIndex);
                        editor->getTabComponent().setActiveSplit(canvas);
                        bridge->sendReply(replyAddr, juce::String("focused: ") + canvas->patch.getTitle());
                        return;
                    }
                }
            }
            bridge->sendReply(replyAddr, juce::String("error: tab not found: " + target));
        });
        return;
    }

    // ── PRD Phase 3 §2.5: CLOSE TAB ─────────────────────────────────────
    // /pd/close_tab <file-or-name> [force] [corrId]
    // Closes the named tab. Refuses unsaved changes unless force=1.
    // Reply: .../reply/<corrId> "<status>"
    if (action == "close_tab") {
        auto target        = msg.size() > 0 ? getArgString(msg[0]) : juce::String();
        float force        = msg.size() > 1 ? getArgFloat(msg[1]) : 0.0f;
        auto correlationId = msg.size() > 2 ? getArgString(msg[2]) : "0";
        juce::String replyAddr = "/pd/close_tab/reply/" + correlationId;

        processor->enqueueFunctionAsync([p = processor, bridge = this, target, force, replyAddr]() {
            juce::File f(target);
            for (auto* editor : p->getEditors()) {
                if (!editor) continue;
                for (auto* canvas : editor->getCanvases()) {
                    if (!canvas) continue;
                    bool match = canvas->patch.getCurrentFile() == f
                        || canvas->patch.getCurrentFile().getFileName() == target;
                    if (match) {
                        if (canvas->patch.isDirty() && force < 0.5f) {
                            bridge->sendReply(replyAddr, juce::String(
                                "error: '" + canvas->patch.getTitle() + "' has unsaved changes — save first or pass force=1"));
                            return;
                        }
                        juce::String title = canvas->patch.getTitle();
                        editor->getTabComponent().closeTab(canvas);
                        bridge->sendReply(replyAddr, juce::String("closed: " + title));
                        return;
                    }
                }
            }
            bridge->sendReply(replyAddr, juce::String("error: tab not found: " + target));
        });
        return;
    }

    // ── PRD diagnostic layer §2.2: /pd/diagnose — THE GRAPH X-RAY ───────
    // /pd/diagnose <canvasName> [corrId]
    // Read-only structured facts, computed where the data lives (no console
    // scraping, no TS graph analysis):
    //   dsp_cycles   : signal-graph cycles, each named by objects (PRD §2.2)
    //   unscheduled  : members of detected cycles (Pd refuses to schedule them)
    //   dangling_main_sig : objects with a DSP method but nothing wired into
    //                      their main signal inlet (informational)
    //   zeroed_vcgs  : bare [*~] with no float arg and no inlet-1 wire —
    //                  outputs constant 0, silently killing the downstream
    //                  chain (the Audio-VCA law, checked natively)
    //   mismatched   : rate-mismatched wires (sig→ctl / ctl→sig) — mostly
    //                  blocked by Pd at connect-time, emitted when present
    // Reply: /pd/diagnose/reply/<corrId> <jsonString>
    if (action == "diagnose") {
        auto canvasName    = normalizeCanvas(getArgString(msg[0]));
        auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";
        juce::String replyAddr = "/pd/diagnose/reply/" + correlationId;

        juce::String json;
        bool hasCanvas = false;

        sys_lock();
        t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
        if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

        if (cnv) {
            hasCanvas = true;
            json = computeDiagnoseFacts(processor, cnv, canvasName);
        }
        sys_unlock();

        if (!hasCanvas) {
            sendReply(replyAddr, juce::String("error: canvas not found: " + canvasName));
            return;
        }
        sendReply(replyAddr, json);
        return;
    }

    // ── PRD layout-v2 Phase A: read-only layout facts ───────────────────
    // C++ is the truth of layout (same law as identity + failures):
    //   /pd/bounds         : true x/y/w/h per object (JUCE Object components / gobj_getrect)
    //   /pd/collisions     : AABB overlap pairs on true rects (5px pad)
    //   /pd/wire_occlusions: wire-vs-box hits on true rects; wire modeled as
    //                        src bottom-center → dest top-center anchor line
    //                        (true bezier/90° cord paths live in JUCE
    //                        Connection components — Phase B queries those).
    // All read-only under sys_lock, zero DSP touch, zero dropout.

    auto findGuiCanvasFor = [](PluginProcessor* proc, t_canvas* c) -> Canvas* {
        return mcpFindGuiCanvasFor(proc, c);
    };

    auto getTrueObjectBounds = [](t_canvas* c, t_gobj* y, Canvas* guiCanvas, int* x, int* yy, int* w, int* h) {
        mcpGetTrueObjectBounds(c, y, guiCanvas, x, yy, w, h);
    };

    // ── /pd/clusters — context grouping (PRD_CONTEXT_LAYOUT_GUARD) ──────
    // /pd/clusters <canvasName> [corrId]
    // Reply: /pd/clusters/reply/<corrId> {"clusters":[{"id","kind","objects":[...]}]}
    if (action == "clusters") {
        auto canvasName    = normalizeCanvas(getArgString(msg[0]));
        auto correlationId = msg.size() > 1 ? getArgString(msg[msg.size() - 1]) : "0";
        juce::String replyAddr = "/pd/clusters/reply/" + correlationId;

        juce::String json;
        sys_lock();
        t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
        if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
        json = cnv ? computeClusters(processor, cnv, canvasName)
                   : juce::String("{\"error\":\"canvas not found\"}");
        sys_unlock();

        sendReply(replyAddr, json);
        return;
    }

    if (action == "bounds") {
        auto canvasName    = normalizeCanvas(getArgString(msg[0]));
        auto correlationId = msg.size() > 1 ? getArgString(msg[msg.size() - 1]) : "0";
        juce::String replyAddr = "/pd/bounds/reply/" + correlationId;

        juce::String json;
        bool hasCanvas = false;

        sys_lock();
        t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
        if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

        if (cnv) {
            hasCanvas = true;
            std::unordered_map<t_gobj*, juce::String> ptrToId;
            auto mapIt = processor->mcpStableObjectMap.find(canvasName.toStdString());
            if (mapIt != processor->mcpStableObjectMap.end())
                for (auto& [tid, ptr] : mapIt->second)
                    if (ptr) ptrToId[ptr] = juce::String(tid);

            Canvas* guiCanvas = mcpFindGuiCanvasFor(processor, cnv);

            float zoom = 1.0f;
            int vx = 0, vy = 0, vw = 1000, vh = 800;
            if (guiCanvas) {
                zoom = getValue<float>(guiCanvas->zoomScale);
                if (zoom <= 0.001f) zoom = 1.0f;
                if (guiCanvas->viewport) {
                    auto va = guiCanvas->viewport->getViewArea();
                    vx = static_cast<int>(std::round(va.getX() / zoom));
                    vy = static_cast<int>(std::round(va.getY() / zoom));
                    vw = static_cast<int>(std::round(va.getWidth() / zoom));
                    vh = static_cast<int>(std::round(va.getHeight() / zoom));
                }
            }

            json = "{\"canvas\":\"" + canvasName + "\""
                 + ",\"zoom\":" + juce::String(zoom, 3)
                 + ",\"viewport\":{\"x\":" + juce::String(vx)
                 + ",\"y\":" + juce::String(vy)
                 + ",\"w\":" + juce::String(vw)
                 + ",\"h\":" + juce::String(vh) + "}"
                 + ",\"objects\":[";
            bool first = true;
            int idx = 0;
            for (t_gobj* y = cnv->gl_list; y; y = y->g_next, ++idx) {
                int x = 0, yy = 0, w = 0, h = 0;
                getTrueObjectBounds(cnv, y, guiCanvas, &x, &yy, &w, &h);
                juce::String tid = ptrToId.count(y)
                    ? ptrToId[y]
                    : (juce::String(class_getname(pd_class(&y->g_pd))) + "#" + juce::String(idx));

                t_class* cl = pd_class(&y->g_pd);
                const char* clName = class_getname(cl);
                juce::String className = clName ? juce::String::fromUTF8(clName) : juce::String("unknown");

                juce::String nodeClass = className;
                juce::String abstractName;

                if (cl == canvas_class) {
                    nodeClass = "pd";
                    if (pd::getAbstractionFileName(y, abstractName) && abstractName.isNotEmpty()) {
                        nodeClass = abstractName;
                    }
                } else if (cl == garray_class) {
                    nodeClass = "table";
                } else if (pd::Interface::isTextObject(y)) {
                    t_text* textObj = reinterpret_cast<t_text*>(y);
                    if (textObj->te_type == T_MESSAGE) {
                        nodeClass = "msg";
                    } else if (textObj->te_type == T_ATOM) {
                        nodeClass = className.containsIgnoreCase("symbol") ? "symbolatom" : "floatatom";
                    } else if (textObj->te_type == T_TEXT) {
                        nodeClass = "text";
                    } else {
                        char* textBuf = nullptr;
                        int textSize = 0;
                        binbuf_gettext(textObj->te_binbuf, &textBuf, &textSize);
                        if (textBuf && textSize > 0) {
                            juce::String fullText = juce::String::fromUTF8(textBuf, textSize).trim();
                            juce::String firstTok = fullText.upToFirstOccurrenceOf(" ", false, false);
                            if (firstTok.isNotEmpty()) nodeClass = firstTok;
                            freebytes(textBuf, textSize);
                        }
                    }
                }

                t_object* ob = pd::Interface::checkObject(y);
                bool sigIn0 = (ob != nullptr && obj_issignalinlet(ob, 0) != 0);

                if (!first) json += ",";
                json += "{\"tempId\":\"" + tid + "\""
                      + ",\"x\":" + juce::String(x)
                      + ",\"y\":" + juce::String(yy)
                      + ",\"w\":" + juce::String(w)
                      + ",\"h\":" + juce::String(h)
                      + ",\"sigIn0\":" + (sigIn0 ? "true" : "false")
                      + ",\"nodeClass\":\"" + juce::JSON::escapeString(nodeClass) + "\"}";
                first = false;
            }
            json += "]}";
        }
        sys_unlock();

        if (!hasCanvas) {
            sendReply(replyAddr, juce::String("error: canvas not found: " + canvasName));
            return;
        }
        sendReply(replyAddr, json);
        return;
    }

    if (action == "collisions") {
        auto canvasName    = normalizeCanvas(getArgString(msg[0]));
        auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";
        juce::String replyAddr = "/pd/collisions/reply/" + correlationId;

        juce::String json;
        bool hasCanvas = false;

        sys_lock();
        t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
        if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

        if (cnv) {
            hasCanvas = true;
            struct R { juce::String tid; int x, y, w, h; };
            std::vector<R> rects;
            std::unordered_map<t_gobj*, juce::String> ptrToId;
            auto mapIt = processor->mcpStableObjectMap.find(canvasName.toStdString());
            if (mapIt != processor->mcpStableObjectMap.end())
                for (auto& [tid, ptr] : mapIt->second)
                    if (ptr) ptrToId[ptr] = juce::String(tid);

            Canvas* guiCanvas = mcpFindGuiCanvasFor(processor, cnv);
            int idx = 0;
            for (t_gobj* y = cnv->gl_list; y; y = y->g_next, ++idx) {
                int x = 0, yy = 0, w = 0, h = 0;
                getTrueObjectBounds(cnv, y, guiCanvas, &x, &yy, &w, &h);
                juce::String tid = ptrToId.count(y)
                    ? ptrToId[y]
                    : (juce::String(class_getname(pd_class(&y->g_pd))) + "#" + juce::String(idx));
                rects.push_back({ tid, x, yy, w, h });
            }

            const int PAD = 5;
            json = "{\"canvas\":\"" + canvasName + "\",\"collisions\":[";
            bool first = true;
            for (size_t i = 0; i < rects.size(); ++i) {
                for (size_t j = i + 1; j < rects.size(); ++j) {
                    const auto& a = rects[i];
                    const auto& b = rects[j];
                    bool hit = a.x < b.x + b.w + PAD && a.x + a.w + PAD > b.x
                            && a.y < b.y + b.h + PAD && a.y + a.h + PAD > b.y;
                    if (hit) {
                        if (!first) json += ",";
                        json += "{\"a\":\"" + a.tid + "\",\"b\":\"" + b.tid + "\"}";
                        first = false;
                    }
                }
            }
            json += "]}";
        }
        sys_unlock();

        if (!hasCanvas) {
            sendReply(replyAddr, juce::String("error: canvas not found: " + canvasName));
            return;
        }
        sendReply(replyAddr, json);
        return;
    }

    if (action == "wire_occlusions") {
        auto canvasName    = normalizeCanvas(getArgString(msg[0]));
        auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";
        juce::String replyAddr = "/pd/wire_occlusions/reply/" + correlationId;

        juce::String json;
        bool hasCanvas = false;

        sys_lock();
        t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
        if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

        if (cnv) {
            hasCanvas = true;
            std::vector<t_gobj*> objs;
            std::vector<juce::String> names;
    std::vector<juce::String> classNames;
            std::unordered_map<t_gobj*, int> ptrToIdx;
            std::unordered_map<t_gobj*, juce::String> ptrToId;
            auto mapIt = processor->mcpStableObjectMap.find(canvasName.toStdString());
            if (mapIt != processor->mcpStableObjectMap.end())
                for (auto& [tid, ptr] : mapIt->second)
                    if (ptr) ptrToId[ptr] = juce::String(tid);

            int idx = 0;
            for (t_gobj* y = cnv->gl_list; y; y = y->g_next, ++idx) {
                objs.push_back(y);
                ptrToIdx[y] = idx;
                names.push_back(ptrToId.count(y)
                    ? ptrToId[y]
                    : (juce::String(class_getname(pd_class(&y->g_pd))) + "#" + juce::String(idx)));
            }

            Canvas* guiCanvas = mcpFindGuiCanvasFor(processor, cnv);
            struct R { int x, y, w, h; };
            std::vector<R> rects(objs.size());
            for (size_t i = 0; i < objs.size(); ++i) {
                int x = 0, yy = 0, w = 0, h = 0;
                getTrueObjectBounds(cnv, objs[i], guiCanvas, &x, &yy, &w, &h);
                rects[i] = { x, yy, w, h };
            }

            // Edges via one linetraverser walk (same as diagnose §2).
            std::vector<std::pair<int, int>> edges;
            t_linetraverser lt;
            t_outconnect* oc = nullptr;
            linetraverser_start(&lt, cnv);
            while ((oc = linetraverser_next_nosize(&lt))) {
                auto si = ptrToIdx.find(&lt.tr_ob->ob_g);
                auto di = ptrToIdx.find(&lt.tr_ob2->ob_g);
                if (si == ptrToIdx.end() || di == ptrToIdx.end()) continue;
                edges.emplace_back(si->second, di->second);
            }

            // Anchor-line vs box test (Liang-Barsky, same as TS rule so
            // results agree — only the rects are now true).
            auto lineHitsBox = [](double x1, double y1, double x2, double y2,
                                  double bx, double by, double bw, double bh) {
                double minX = std::min(x1, x2), maxX = std::max(x1, x2);
                double minY = std::min(y1, y2), maxY = std::max(y1, y2);
                if (maxX < bx || minX > bx + bw || maxY < by || minY > by + bh) return false;
                double dx = x2 - x1, dy = y2 - y1, t0 = 0.0, t1 = 1.0;
                double p[4] = { -dx, dx, -dy, dy };
                double q[4] = { x1 - bx, bx + bw - x1, y1 - by, by + bh - y1 };
                for (int i = 0; i < 4; ++i) {
                    if (p[i] == 0) { if (q[i] < 0) return false; }
                    else {
                        double t = q[i] / p[i];
                        if (p[i] < 0) { if (t > t1) return false; if (t > t0) t0 = t; }
                        else { if (t < t0) return false; if (t < t1) t1 = t; }
                    }
                }
                return t0 < t1 && t0 > 0.05 && t1 < 0.95;
            };

            json = "{\"canvas\":\"" + canvasName + "\",\"model\":\"anchor-line\",\"occlusions\":[";
            bool first = true;
            for (auto& [si, di] : edges) {
                double x1 = rects[si].x + rects[si].w / 2.0;
                double y1 = rects[si].y + rects[si].h;
                double x2 = rects[di].x + rects[di].w / 2.0;
                double y2 = rects[di].y;
                for (size_t k = 0; k < objs.size(); ++k) {
                    if ((int)k == si || (int)k == di) continue;
                    if (lineHitsBox(x1, y1, x2, y2, rects[k].x, rects[k].y, rects[k].w, rects[k].h)) {
                        if (!first) json += ",";
                        json += "{\"srcId\":\"" + names[si] + "\",\"destId\":\"" + names[di]
                              + "\",\"via\":\"" + names[k] + "\"}";
                        first = false;
                    }
                }
            }
            json += "]}";
        }
        sys_unlock();

        if (!hasCanvas) {
            sendReply(replyAddr, juce::String("error: canvas not found: " + canvasName));
            return;
        }
        sendReply(replyAddr, json);
        return;
    }

    if (action == "clear") {
        if (msg.size() >= 1 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";

            // Clear on the JUCE message thread. Deleting GUI/abstraction/GOP
            // objects off the message thread crashes (same class as the load
            // bug). clearCanvas() deselects the editor + glist_delete.
            juce::MessageManager::callAsync(
                [proc = processor, canvasName, correlationId, bridge = this]() {
                    sys_lock();
                    t_canvas* cnv = proc->getCanvasBySymbol(canvasName);
                    if (cnv) {
                        pd::Interface::clearCanvas(cnv);
                    }
                    sys_unlock();

                    // Clearing freed every object while Pd's index-based undo
                    // entries still point at them — drop the stale queue.
                    resetCanvasUndo(proc, canvasName);

                    SmallArray<pd::Atom> atoms;
                    atoms.add(pd::Atom(proc->generateSymbol(canvasName)));
                    atoms.add(pd::Atom(proc->generateSymbol("0")));
                    proc->receiveSysMessage("mcp_clear_ids", atoms);

                    proc->synchroniseCanvases();

                    bridge->sendRawReply("/pd/cleared");
                    if (correlationId != "0") {
                        juce::Array<juce::var> clearReplyArgs;
                        clearReplyArgs.add(1.0);
                        clearReplyArgs.add(1.0);
                        bridge->sendReply("/pd/clear/reply/" + correlationId, clearReplyArgs);
                    }
                });
        }
        return;
    }

    if (action == "clear_undo") {
        // /pd/clear_undo <canvasName>
        // Routes through receiveSysMessage → mcp_clear_undo case which runs
        // on the Pd scheduler thread — the only thread where canvas_undo_free
        // (calls canvas_suspend_dsp internally) is safe to call.
        if (msg.size() >= 1 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            SmallArray<pd::Atom> atoms;
            atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
            processor->receiveSysMessage("mcp_clear_undo", atoms);
            sendRawReply("/pd/clear_undo/reply");
        }
        return;
    }

    if (action == "clear_selection") {
        if (processor) {
            SmallArray<pd::Atom> atoms;
            processor->receiveSysMessage("mcp_clear_selection", atoms);
        }
        return;
    }

    if (action == "clear_ids") {
        if (msg.size() >= 2 && processor) {
            SmallArray<pd::Atom> atoms;
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
            atoms.add(pd::Atom(processor->generateSymbol(correlationId)));
            processor->receiveSysMessage("mcp_clear_ids", atoms);
        }
        return;
    }

    if (action == "create_id") {
        if (msg.size() >= 6 && processor) {
            SmallArray<pd::Atom> atoms;
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            auto objectId = getArgString(msg[2]);
            float x = getArgFloat(msg[3]);
            float y = getArgFloat(msg[4]);
            auto kind = getArgString(msg[5]);

            atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
            atoms.add(pd::Atom(processor->generateSymbol(correlationId)));
            atoms.add(pd::Atom(processor->generateSymbol(objectId)));
            atoms.add(pd::Atom(x));
            atoms.add(pd::Atom(y));
            atoms.add(pd::Atom(processor->generateSymbol(kind)));

            for (int i = 6; i < msg.size(); ++i) {
                atoms.add(pd::Atom(processor->generateSymbol(getArgString(msg[i]))));
            }
            processor->receiveSysMessage("mcp_create_id", atoms);
        }
        return;
    }

    if (action == "create_batch_id") {
        if (msg.size() >= 3 && processor) {
            SmallArray<pd::Atom> atoms;
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            float count = getArgFloat(msg[2]);

            atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
            atoms.add(pd::Atom(processor->generateSymbol(correlationId)));
            atoms.add(pd::Atom(count));

            for (int i = 3; i < msg.size(); ++i) {
                if (msg[i].isFloat32() || msg[i].isInt32()) {
                    atoms.add(pd::Atom(getArgFloat(msg[i])));
                } else {
                    atoms.add(pd::Atom(processor->generateSymbol(getArgString(msg[i]))));
                }
            }
            processor->receiveSysMessage("mcp_create_batch_id", atoms);
        }
        return;
    }

    if (action == "create") {
        if (msg.size() >= 5 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto kind = getArgString(msg[1]);
            float x = getArgFloat(msg[2]);
            float y = getArgFloat(msg[3]);

            juce::StringArray tokens;
            for (int i = 4; i < msg.size(); ++i) {
                tokens.add(getArgString(msg[i]));
            }

            sys_lock();
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (cnv) {
                pd::Patch patchWrapper(pd::WeakReference(cnv, processor), processor, false);
                patchWrapper.createObject(static_cast<int>(x), static_cast<int>(y), buildObjectText(kind, tokens));
            }
            sys_unlock();

            processor->enqueueFunctionAsync([p = processor] { p->synchroniseCanvases(); });
        }
        return;
    }

    if (action == "create_batch") {
        if (msg.size() >= 3 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            int count = static_cast<int>(getArgFloat(msg[2]));
            int cursor = 3;
            int created = 0;

            sys_lock();
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (cnv) {
                pd::Patch patchWrapper(pd::WeakReference(cnv, processor), processor, false);
                for (int o = 0; o < count && cursor < msg.size(); o++) {
                    auto kind = getArgString(msg[cursor++]);
                    float x = getArgFloat(msg[cursor++]);
                    float y = getArgFloat(msg[cursor++]);
                    int nargs = static_cast<int>(getArgFloat(msg[cursor++]));

                    juce::StringArray tokens;
                    for (int a = 0; a < nargs && cursor < msg.size(); a++) {
                        tokens.add(getArgString(msg[cursor++]));
                    }

                    if (patchWrapper.createObject(static_cast<int>(x), static_cast<int>(y), buildObjectText(kind, tokens))) {
                        created++;
                    }
                }
            }
            sys_unlock();

            processor->enqueueFunctionAsync([p = processor] { p->synchroniseCanvases(); });

            sendReply("/pd/create_batch/reply/" + correlationId, static_cast<float>(created));
        }
        return;
    }

    if (action == "connect_id") {
        if (msg.size() >= 6 && processor) {
            SmallArray<pd::Atom> atoms;
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            auto srcId = getArgString(msg[2]);
            float srcOut = getArgFloat(msg[3]);
            auto destId = getArgString(msg[4]);
            float destIn = getArgFloat(msg[5]);

            atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
            atoms.add(pd::Atom(processor->generateSymbol(correlationId)));
            atoms.add(pd::Atom(processor->generateSymbol(srcId)));
            atoms.add(pd::Atom(srcOut));
            atoms.add(pd::Atom(processor->generateSymbol(destId)));
            atoms.add(pd::Atom(destIn));

            processor->receiveSysMessage("mcp_connect_id", atoms);
        }
        return;
    }

    if (action == "connect_batch_id") {
        if (msg.size() >= 2 && processor) {
            SmallArray<pd::Atom> atoms;
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
            atoms.add(pd::Atom(processor->generateSymbol(correlationId)));

            for (int i = 2; i < msg.size(); ++i) {
                if (msg[i].isFloat32() || msg[i].isInt32()) {
                    atoms.add(pd::Atom(getArgFloat(msg[i])));
                } else {
                    atoms.add(pd::Atom(processor->generateSymbol(getArgString(msg[i]))));
                }
            }
            processor->receiveSysMessage("mcp_connect_batch_id", atoms);
        }
        return;
    }

    if (action == "connect_batch" || action == "connect") {
        if (msg.size() >= 3 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = action == "connect_batch" ? getArgString(msg[1]) : "0";
            int startIdx = action == "connect_batch" ? 3 : 1;
            int count = action == "connect_batch" ? static_cast<int>(getArgFloat(msg[2])) : 1;
            int done = 0;

            sys_lock();
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (cnv) {
                for (int c = 0; c < count && startIdx + 3 < msg.size(); c++) {
                    int srcIdx = static_cast<int>(getArgFloat(msg[startIdx++]));
                    int srcOut = static_cast<int>(getArgFloat(msg[startIdx++]));
                    int destIdx = static_cast<int>(getArgFloat(msg[startIdx++]));
                    int destIn = static_cast<int>(getArgFloat(msg[startIdx++]));

                    t_atom cArgs[4];
                    SETFLOAT(&cArgs[0], static_cast<float>(srcIdx));
                    SETFLOAT(&cArgs[1], static_cast<float>(srcOut));
                    SETFLOAT(&cArgs[2], static_cast<float>(destIdx));
                    SETFLOAT(&cArgs[3], static_cast<float>(destIn));
                    pd_typedmess(reinterpret_cast<t_pd*>(cnv), gensym("connect"), 4, cArgs);
                    done++;
                }
            }
            sys_unlock();

            processor->enqueueFunctionAsync([p = processor] { p->synchroniseCanvases(); });

            if (action == "connect_batch") {
                sendReply("/pd/connect_batch/reply/" + correlationId, static_cast<float>(done));
            }
        }
        return;
    }

    if (action == "disconnect_id") {
        if (msg.size() >= 6 && processor) {
            SmallArray<pd::Atom> atoms;
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            auto srcId = getArgString(msg[2]);
            float srcOut = getArgFloat(msg[3]);
            auto destId = getArgString(msg[4]);
            float destIn = getArgFloat(msg[5]);

            atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
            atoms.add(pd::Atom(processor->generateSymbol(correlationId)));
            atoms.add(pd::Atom(processor->generateSymbol(srcId)));
            atoms.add(pd::Atom(srcOut));
            atoms.add(pd::Atom(processor->generateSymbol(destId)));
            atoms.add(pd::Atom(destIn));

            processor->receiveSysMessage("mcp_disconnect_id", atoms);
        }
        return;
    }

    if (action == "disconnect_batch_id") {
        if (msg.size() >= 2 && processor) {
            SmallArray<pd::Atom> atoms;
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
            atoms.add(pd::Atom(processor->generateSymbol(correlationId)));

            for (int i = 2; i < msg.size(); ++i) {
                if (msg[i].isFloat32() || msg[i].isInt32()) {
                    atoms.add(pd::Atom(getArgFloat(msg[i])));
                } else {
                    atoms.add(pd::Atom(processor->generateSymbol(getArgString(msg[i]))));
                }
            }
            processor->receiveSysMessage("mcp_disconnect_batch_id", atoms);
        }
        return;
    }

    if (action == "disconnect") {
        if (msg.size() >= 5 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            int srcIdx = static_cast<int>(getArgFloat(msg[1]));
            int srcOut = static_cast<int>(getArgFloat(msg[2]));
            int destIdx = static_cast<int>(getArgFloat(msg[3]));
            int destIn = static_cast<int>(getArgFloat(msg[4]));

            sys_lock();
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (cnv) {
                t_object* src = pd::Interface::checkObject(glistObjectAt(cnv, srcIdx));
                t_object* dest = pd::Interface::checkObject(glistObjectAt(cnv, destIdx));
                if (src && dest) {
                    pd::Interface::removeConnection(cnv, src, srcOut, dest, destIn, nullptr);
                    canvas_dirty(cnv, 1);
                }
            }
            sys_unlock();

            processor->enqueueFunctionAsync([p = processor] { p->synchroniseCanvases(); });
        }
        return;
    }

    if (action == "disconnect_batch") {
        if (msg.size() >= 3 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            int count = static_cast<int>(getArgFloat(msg[2]));
            int cursor = 3;
            int done = 0;

            sys_lock();
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (cnv) {
                for (int c = 0; c < count && cursor + 3 < msg.size(); c++) {
                    int srcIdx = static_cast<int>(getArgFloat(msg[cursor++]));
                    int srcOut = static_cast<int>(getArgFloat(msg[cursor++]));
                    int destIdx = static_cast<int>(getArgFloat(msg[cursor++]));
                    int destIn = static_cast<int>(getArgFloat(msg[cursor++]));

                    t_object* src = pd::Interface::checkObject(glistObjectAt(cnv, srcIdx));
                    t_object* dest = pd::Interface::checkObject(glistObjectAt(cnv, destIdx));
                    if (src && dest) {
                        pd::Interface::removeConnection(cnv, src, srcOut, dest, destIn, nullptr);
                        done++;
                    }
                }
                if (done > 0) canvas_dirty(cnv, 1);
            }
            sys_unlock();

            processor->enqueueFunctionAsync([p = processor] { p->synchroniseCanvases(); });

            sendReply("/pd/disconnect_batch/reply/" + correlationId, static_cast<float>(done));
        }
        return;
    }

    if (action == "delete_id") {
        if (msg.size() >= 3 && processor) {
            SmallArray<pd::Atom> atoms;
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            auto objectId = getArgString(msg[2]);

            atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
            atoms.add(pd::Atom(processor->generateSymbol(correlationId)));
            atoms.add(pd::Atom(processor->generateSymbol(objectId)));

            processor->receiveSysMessage("mcp_delete_id", atoms);
        }
        return;
    }

    if (action == "delete_batch_id") {
        if (msg.size() >= 3 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            int count = static_cast<int>(getArgFloat(msg[2]));

            SmallArray<pd::Atom> atoms;
            processor->receiveSysMessage("mcp_suspend_dsp", atoms);

            int deleted = 0;
            for (int i = 0; i < count && 3 + i < msg.size(); i++) {
                auto id = getArgString(msg[3 + i]);
                if (id.isNotEmpty() && id != "nil") {
                    SmallArray<pd::Atom> delAtoms;
                    delAtoms.add(pd::Atom(processor->generateSymbol(canvasName)));
                    delAtoms.add(pd::Atom(processor->generateSymbol(correlationId)));
                    delAtoms.add(pd::Atom(processor->generateSymbol(id)));
                    processor->receiveSysMessage("mcp_delete_id", delAtoms);
                    deleted++;
                }
            }

            processor->receiveSysMessage("mcp_resume_dsp", atoms);
            sendReply("/pd/delete_batch_id/reply/" + correlationId, static_cast<float>(deleted));
        }
        return;
    }

    if (action == "delete_batch") {
        if (msg.size() >= 3 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            std::vector<int> indices;
            for (int i = 2; i < msg.size(); ++i) {
                indices.push_back(static_cast<int>(getArgFloat(msg[i])));
            }
            std::sort(indices.rbegin(), indices.rend());

            sys_lock();
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (cnv) {
                SmallArray<t_gobj*> toDelete;
                for (int idx : indices) {
                    t_gobj* obj = glistObjectAt(cnv, idx);
                    if (obj) toDelete.add(obj);
                }
                if (toDelete.size() > 0) {
                    pd::Interface::removeObjects(cnv, toDelete);
                    canvas_dirty(cnv, 1);
                }
            }
            sys_unlock();

            processor->enqueueFunctionAsync([p = processor] { p->synchroniseCanvases(); });

            sendReply("/pd/delete_batch/reply/" + correlationId, static_cast<float>(indices.size()));
        }
        return;
    }

    if (action == "delete") {
        if (msg.size() >= 2 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            int index = static_cast<int>(getArgFloat(msg[1]));

            sys_lock();
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (cnv) {
                t_gobj* obj = glistObjectAt(cnv, index);
                if (obj) {
                    SmallArray<t_gobj*> toDelete;
                    toDelete.add(obj);
                    pd::Interface::removeObjects(cnv, toDelete);
                    canvas_dirty(cnv, 1);
                }
            }
            sys_unlock();

            processor->enqueueFunctionAsync([p = processor] { p->synchroniseCanvases(); });
        }
        return;
    }

    if (action == "move_id" || action == "move_batch" || action == "move_batch_id") {
        if (msg.size() >= 3 && processor) {
            SmallArray<pd::Atom> atoms;
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
            atoms.add(pd::Atom(processor->generateSymbol(correlationId)));

            for (int i = 2; i < msg.size(); ++i) {
                if (msg[i].isFloat32() || msg[i].isInt32()) {
                    atoms.add(pd::Atom(getArgFloat(msg[i])));
                } else {
                    atoms.add(pd::Atom(processor->generateSymbol(getArgString(msg[i]))));
                }
            }

            if (action == "move_id") {
                processor->receiveSysMessage("mcp_move_id", atoms);
            } else if (action == "move_batch_id") {
                processor->receiveSysMessage("mcp_move_batch_id", atoms);
            } else {
                processor->receiveSysMessage("mcp_move_batch", atoms);
            }
        }
        return;
    }

    if (action == "rename_id") {
        if (msg.size() >= 4 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            auto oldId = getArgString(msg[2]).toStdString();
            auto newId = getArgString(msg[3]).toStdString();

            bool success = false;
            auto& map = processor->mcpStableObjectMap[canvasName.toStdString()];
            auto it = map.find(oldId);
            if (it != map.end()) {
                map[newId] = it->second;
                map.erase(it);
                processor->mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
                success = true;
            }

            sendReply("/pd/rename_id/reply/" + correlationId, success ? 1.0f : 0.0f);
        }
        return;
    }

    // /pd/rename_id_batch <canvas> <count> <old0> <new0> <old1> <new1> ... <corrId>
    // Batch rename with a reply — renames directly in mcpStableObjectMap
    // (inline, synchronous — no receiveSysMessage async queue).
    if (action == "rename_id_batch") {
        if (msg.size() >= 2 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            int count = static_cast<int>(getArgFloat(msg[1]));
            int cursor = 2;
            int renamed = 0;

            // Inline rename — directly mutates mcpStableObjectMap, no queue.
            auto& map = processor->mcpStableObjectMap[canvasName.toStdString()];
            for (int i = 0; i < count && (cursor + 1) < msg.size(); i++) {
                auto oldId = getArgString(msg[cursor++]).toStdString();
                auto newId = getArgString(msg[cursor++]).toStdString();
                auto it = map.find(oldId);
                if (it != map.end()) {
                    map[newId] = it->second;
                    map.erase(it);
                    renamed++;
                }
            }
            if (renamed > 0)
                processor->mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);

            auto correlationId = (cursor < msg.size()) ? getArgString(msg[cursor]) : "0";
            sendReply("/pd/rename_id_batch/reply/" + correlationId, static_cast<float>(renamed));
        }
        return;
    }

    if (action == "edit_id") {
        if (msg.size() >= 4 && processor) {
            SmallArray<pd::Atom> atoms;
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            auto objectId = getArgString(msg[2]);
            auto kind = getArgString(msg[3]);

            atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
            atoms.add(pd::Atom(processor->generateSymbol(correlationId)));
            atoms.add(pd::Atom(processor->generateSymbol(objectId)));
            atoms.add(pd::Atom(processor->generateSymbol(kind)));

            for (int i = 4; i < msg.size(); ++i) {
                atoms.add(pd::Atom(processor->generateSymbol(getArgString(msg[i]))));
            }
            processor->receiveSysMessage("mcp_edit_id", atoms);
        }
        return;
    }

    if (action == "get_mappings") {
        if (msg.size() >= 2 && processor) {
            SmallArray<pd::Atom> atoms;
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
            atoms.add(pd::Atom(processor->generateSymbol(correlationId)));
            processor->receiveSysMessage("mcp_get_mappings", atoms);
        }
        return;
    }

    if (action == "register_id") {
        if (msg.size() >= 4 && processor) {
            SmallArray<pd::Atom> atoms;
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = getArgString(msg[1]);
            float index = getArgFloat(msg[2]);
            auto objectId = getArgString(msg[3]);

            atoms.add(pd::Atom(processor->generateSymbol(canvasName)));
            atoms.add(pd::Atom(processor->generateSymbol(correlationId)));
            atoms.add(pd::Atom(index));
            atoms.add(pd::Atom(processor->generateSymbol(objectId)));

            processor->receiveSysMessage("mcp_register_id", atoms);
        }
        return;
    }

    if (action == "encapsulate") {
        if (msg.size() >= 3 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto subpatchName = getArgString(msg[1]);
            int count = static_cast<int>(getArgFloat(msg[2]));
            std::vector<juce::String> targetIds;
            int cursor = 3;
            for (int i = 0; i < count && cursor < msg.size(); i++) {
                targetIds.push_back(getArgString(msg[cursor++]));
            }
            auto correlationId = (cursor < msg.size()) ? getArgString(msg[cursor]) : "0";

            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

            if (cnv) {
                juce::MessageManager::callAsync([proc = processor, cnv, canvasName, subpatchName, targetIds, correlationId, bridge = this]() {
                    Canvas* canvasComp = getOrCreateCanvasComponent(proc, cnv);

                    if (!canvasComp) {
                        bridge->sendReply("/pd/encapsulate/reply/" + correlationId, 0.0f);
                        return;
                    }

                    canvasComp->patch.deselectAll();
                    int selCount = 0;
                    for (const auto& id : targetIds) {
                        t_gobj* g = proc->resolveStableId(canvasName, id);
                        if (g) {
                            for (auto* objComp : canvasComp->objects) {
                                if (objComp && objComp->getPointer() == g) {
                                    canvasComp->setSelected(objComp, true);
                                    selCount++;
                                    break;
                                }
                            }
                        }
                    }

                    canvasComp->encapsulateSelection(subpatchName);

                    t_gobj* newestObj = pd::Interface::getNewest(cnv);
                    if (newestObj) {
                        proc->mcpStableObjectMap[canvasName.toStdString()][subpatchName.toStdString()] = newestObj;
                        proc->mcpStableSerialMap[newestObj] = proc->mcpSerialCounter++;
                        proc->mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
                    }

                    // Encapsulation removed objects from the parent canvas and
                    // re-parented them into a fresh [pd] — index-based undo
                    // entries on the parent now dangle. Flush to keep undo safe.
                    resetCanvasUndo(proc, canvasName);

                    bridge->sendReply("/pd/encapsulate/reply/" + correlationId, 1.0f);
                });
            } else {
                sendReply("/pd/encapsulate/reply/" + correlationId, 0.0f);
            }
        }
        return;
    }

    if (action == "encapsulate_to_file") {
        if (msg.size() >= 4 && processor) {
            auto canvasName   = normalizeCanvas(getArgString(msg[0]));
            auto abstrName    = getArgString(msg[1]);
            auto filePath     = getArgString(msg[2]);
            int  count        = static_cast<int>(getArgFloat(msg[3]));
            std::vector<juce::String> targetIds;
            int cursor = 4;
            for (int i = 0; i < count && cursor < msg.size(); i++)
                targetIds.push_back(getArgString(msg[cursor++]));
            auto correlationId = (cursor < msg.size()) ? getArgString(msg[cursor]) : "0";

            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

            if (cnv) {
                juce::MessageManager::callAsync([proc = processor, cnv, canvasName, abstrName, filePath, targetIds, correlationId, bridge = this]() {
                    Canvas* canvasComp = getOrCreateCanvasComponent(proc, cnv);
                    if (!canvasComp) {
                        bridge->sendReply("/pd/encapsulate_to_file/reply/" + correlationId, 0.0f);
                        return;
                    }

                    // ── Step 1: Encapsulate selected objects ─────────────────
                    canvasComp->patch.deselectAll();
                    for (const auto& id : targetIds) {
                        t_gobj* g = proc->resolveStableId(canvasName, id);
                        if (g) {
                            for (auto* objComp : canvasComp->objects) {
                                if (objComp && objComp->getPointer() == g) {
                                    canvasComp->setSelected(objComp, true);
                                    break;
                                }
                            }
                        }
                    }
                    canvasComp->encapsulateSelection(abstrName);

                    // ── Step 2: Find the new [pd abstrName] subpatch ─────────
                    t_gobj* newestObj = pd::Interface::getNewest(cnv);
                    if (!newestObj) {
                        bridge->sendReply("/pd/encapsulate_to_file/reply/" + correlationId, 0.0f);
                        return;
                    }
                    t_canvas* subCnv = reinterpret_cast<t_canvas*>(newestObj);

                    // Record position before we replace it
                    int posX = subCnv->gl_obj.te_xpix;
                    int posY = subCnv->gl_obj.te_ypix;

                    // ── Step 3: Save subpatch content to file ────────────────
                    // getCanvasContent runs under sys_lock; disk write is outside.
                    juce::String content;
                    sys_lock();
                    {
                        char* buf = nullptr; int bufsize = 0;
                        pd::Interface::getCanvasContent(subCnv, &buf, &bufsize);
                        if (buf) {
                            content = juce::String::fromUTF8(buf, static_cast<size_t>(bufsize));
                            freebytes(buf, static_cast<size_t>(bufsize) * sizeof(char));
                        }
                        // getCanvasContent emits a SUBPATCH header
                        // ("#N canvas x y w h name mapped;") because subCnv still
                        // has an owner. An abstraction FILE must use the ROOT
                        // header ("#N canvas x y w h font;") — saving the wrong
                        // header makes Pd crash the instant [name] is
                        // instantiated (abstraction refs in load_patch / GUI).
                        if (content.startsWith("#N canvas"))
                        {
                            int semi = content.indexOfChar(';');
                            if (semi > 0)
                            {
                                juce::String header = "#N canvas "
                                    + juce::String(subCnv->gl_screenx1) + " "
                                    + juce::String(subCnv->gl_screeny1) + " "
                                    + juce::String(subCnv->gl_screenx2 - subCnv->gl_screenx1) + " "
                                    + juce::String(subCnv->gl_screeny2 - subCnv->gl_screeny1) + " "
                                    + juce::String(subCnv->gl_font) + ";";
                                content = header + content.substring(semi + 1);
                            }
                        }
                    }
                    sys_unlock();

                    if (content.isEmpty()) {
                        bridge->sendReply("/pd/encapsulate_to_file/reply/" + correlationId, 0.0f);
                        return;
                    }

                    juce::File targetFile(filePath);
                    targetFile.getParentDirectory().createDirectory();
                    bool writeOk = targetFile.replaceWithText(content);
                    if (!writeOk) {
                        bridge->sendReply("/pd/encapsulate_to_file/reply/" + correlationId, 0.0f);
                        return;
                    }

                    // ── Step 4: Replace [pd abstrName] with [abstrName] ref ──
                    // Delete the inline subpatch, create abstraction reference.
                    sys_lock();
                    {
                        // Delete the [pd abstrName] subpatch object
                        glist_delete(cnv, newestObj);
                        // Create [abstrName] abstraction reference at same position
                        // using pasteDirect — same path as batch_atomic PHASE 4
                        juce::String objLine = "#X obj " + juce::String(posX) + " " + juce::String(posY) + " " + abstrName + ";";
                        pasteDirect(cnv, objLine.toRawUTF8());
                        // Single DSP recompile
                        canvas_update_dsp();
                    }
                    sys_unlock();

                    // ── Step 5: Register new abstraction object in identity map
                    t_gobj* newObj = pd::Interface::getNewest(cnv);
                    if (newObj) {
                        proc->mcpStableObjectMap[canvasName.toStdString()][abstrName.toStdString()] = newObj;
                        proc->mcpStableSerialMap[newObj] = proc->mcpSerialCounter++;
                        proc->mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
                    }

                    // ── Step 6: Reload abstractions so Pd registers the file ─
                    proc->reloadAbstractions(juce::File(filePath), cnv);

                    proc->enqueueFunctionAsync([p = proc] { p->synchroniseCanvases(); });

                    // glist_delete + pasteDirect above freed/reordered objects
                    // on the parent canvas — flush the stale undo queue.
                    resetCanvasUndo(proc, canvasName);

                    bridge->sendReply("/pd/encapsulate_to_file/reply/" + correlationId, 1.0f);
                });
            } else {
                sendReply("/pd/encapsulate_to_file/reply/" + correlationId, 0.0f);
            }
        }
        return;
    }

    if (action == "encapsulate_gop") {
        // PRD GOP Module v2 Phase A: atomic parent-canvas swap for a TS-generated
        // MERDA abstraction. TS already generated + wrote the .pd file (file bytes
        // never cross OSC); C++ does delete-originals + create-[module] + rewire
        // parent boundary under ONE sys_lock + ONE canvas_update_dsp.
        // Args: [canvasName, moduleName, filePath, jsonSpec, correlationId]
        // jsonSpec: {"targetIds":["tid",...],
        //            "inbound":[{"src":"tid","out":0,"in":0}],   // parent→box
        //            "outbound":[{"out":0,"dest":"tid","in":0}], // box→parent
        //            "posX":123,"posY":45}
        // Reply: JSON string on /pd/encapsulate_gop/reply/<corrId>.
        // NOTE: native UndoSequence is Phase B; undo is via TS rollback snapshot.
        if (msg.size() >= 4 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto moduleName  = getArgString(msg[1]).trim();
            auto filePath    = getArgString(msg[2]);
            auto jsonSpec    = getArgString(msg[3]);
            auto correlationId = (msg.size() > 4) ? getArgString(msg[4]) : juce::String("0");
            auto replyAddr = "/pd/encapsulate_gop/reply/" + correlationId;

            juce::String safeName;
            for (auto c : moduleName) {
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')
                    safeName += c;
            }
            if (safeName.endsWith(".pd")) safeName = safeName.dropLastCharacters(3);

            // Pre-parse JSON outside the message-thread lambda (zero contention).
            juce::StringArray targetIds;
            struct GopIn { juce::String src; int srcOut; int boxIn; };
            struct GopOut { int boxOut; juce::String dest; int destIn; };
            std::vector<GopIn> inbound;
            std::vector<GopOut> outbound;
            int posX = 0, posY = 0;
            juce::String specError;
            {
                juce::var parsed = juce::JSON::parse(jsonSpec);
                if (auto* o = parsed.getDynamicObject()) {
                    if (auto* arr = o->getProperty("targetIds").getArray())
                        for (auto& v : *arr) targetIds.add(v.toString());
                    if (auto* arr = o->getProperty("inbound").getArray())
                        for (auto& e : *arr)
                            if (auto* w = e.getDynamicObject())
                                inbound.push_back({ w->getProperty("src").toString(),
                                    static_cast<int>(w->getProperty("out")),
                                    static_cast<int>(w->getProperty("in")) });
                    if (auto* arr = o->getProperty("outbound").getArray())
                        for (auto& e : *arr)
                            if (auto* w = e.getDynamicObject())
                                outbound.push_back({ static_cast<int>(w->getProperty("out")),
                                    w->getProperty("dest").toString(),
                                    static_cast<int>(w->getProperty("in")) });
                    posX = static_cast<int>(o->getProperty("posX"));
                    posY = static_cast<int>(o->getProperty("posY"));
                } else {
                    specError = "spec JSON parse failed";
                }
            }

            if (safeName.isEmpty()) {
                sendReply(replyAddr, juce::String("{\"ok\":false,\"error\":\"GOP_BAD_NAME\",\"detail\":\"empty module name\"}"));
            } else if (specError.isNotEmpty()) {
                sendReply(replyAddr, juce::String("{\"ok\":false,\"error\":\"GOP_BAD_SPEC\",\"detail\":\"" + specError + "\"}"));
            } else if (targetIds.isEmpty()) {
                sendReply(replyAddr, juce::String("{\"ok\":false,\"error\":\"GOP_BAD_SPEC\",\"detail\":\"no targetIds\"}"));
            } else {
                t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
                if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
                if (!cnv) {
                    sendReply(replyAddr, juce::String("{\"ok\":false,\"error\":\"GOP_NO_CANVAS\",\"detail\":\"" + canvasName + "\"}"));
                } else {
                    juce::MessageManager::callAsync([proc = processor, cnv, canvasName, safeName, filePath, targetIds, inbound, outbound, posX, posY, correlationId, replyAddr, bridge = this]() {
                        // Resolve everything BEFORE touching the canvas.
                        SmallArray<t_gobj*> toDelete;
                        juce::StringArray missing;
                        for (auto& tid : targetIds) {
                            if (t_gobj* g = proc->resolveStableId(canvasName, tid))
                                toDelete.add(g);
                            else
                                missing.add(tid);
                        }
                        struct ResWire { t_gobj* far; int farPort; int boxPort; };
                        std::vector<ResWire> inWires;
                        std::vector<ResWire> outWires;
                        for (auto& w : inbound) {
                            if (t_gobj* g = proc->resolveStableId(canvasName, w.src))
                                inWires.push_back({ g, w.srcOut, w.boxIn });
                            else
                                missing.add(w.src);
                        }
                        for (auto& w : outbound) {
                            if (t_gobj* g = proc->resolveStableId(canvasName, w.dest))
                                outWires.push_back({ g, w.destIn, w.boxOut });
                            else
                                missing.add(w.dest);
                        }
                        if (missing.size() > 0) {
                            bridge->sendReply(replyAddr, "{\"ok\":false,\"error\":\"GOP_TARGET_NOT_FOUND\",\"detail\":\"" + missing.joinIntoString(",") + "\"}");
                            return;
                        }

                        // Drop boundary wires whose far endpoint sits INSIDE the
                        // delete set (stale spec): after deletion those pointers
                        // are freed and obj_connect would UAF on them.
                        juce::StringArray staleErrs;
                        {
                            std::unordered_set<t_gobj*> doomed;
                            for (auto* g : toDelete) doomed.insert(g);
                            auto stale = [&](t_gobj* far) { return doomed.find(far) != doomed.end(); };
                            inWires.erase(std::remove_if(inWires.begin(), inWires.end(),
                                [&](auto& w) {
                                    if (stale(w.far)) { staleErrs.add("stale-in:" + juce::String(w.boxPort)); return true; }
                                    return false;
                                }), inWires.end());
                            outWires.erase(std::remove_if(outWires.begin(), outWires.end(),
                                [&](auto& w) {
                                    if (stale(w.far)) { staleErrs.add("stale-out:" + juce::String(w.boxPort)); return true; }
                                    return false;
                                }), outWires.end());
                        }

                        int connected = 0;
                        juce::StringArray connErrs;
                        juce::String newTempId;
                        sys_lock();
                        {
                            // 1. Create the abstraction box FIRST so a red-box
                            // failure leaves the originals untouched.
                            juce::String objLine = "#X obj " + juce::String(posX) + " " + juce::String(posY) + " " + safeName + ";";
                            pasteDirect(cnv, objLine.toRawUTF8());
                            t_gobj* newObj = pd::Interface::getNewest(cnv);
                            t_object* newOb = newObj ? pd::Interface::checkObject(newObj) : nullptr;
                            bool redBox = !newOb || (newOb->te_type == T_OBJECT && pd_class(&newObj->g_pd) == text_class);
                            if (redBox) {
                                // Roll the failed box back out; originals intact.
                                if (newObj) glist_delete(cnv, newObj);
                                canvas_update_dsp();
                                bridge->sendReply(replyAddr, "{\"ok\":false,\"error\":\"GOP_CREATE_FAILED\",\"detail\":\"[" + safeName + "] couldn't create (red box)\"}");
                            } else {
                                // 2. Delete originals via the message-thread-safe
                                // remover. removeObjects() clears selection first (noselect),
                                // opens UNDO_SEQUENCE_START, records UNDO_CUT, and suspends DSP around free.
                                for (auto* g : toDelete) {
                                    proc->mcpStableObjectMap[canvasName.toStdString()].erase(
                                        [&]() -> std::string {
                                            for (auto& [tid, ptr] : proc->mcpStableObjectMap[canvasName.toStdString()])
                                                if (ptr == g) return tid;
                                            return "";
                                        }());
                                    proc->mcpStableSerialMap.erase(g);
                                }
                                pd::Interface::removeObjectsAudioThread(cnv, toDelete);
                                proc->mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);

                                // 3. Rewire parent boundary through the new box with createConnection.
                                for (auto& w : inWires) {
                                    t_object* so = pd::Interface::checkObject(w.far);
                                    if (so && newOb && pd::Interface::createConnection(cnv, so, w.farPort, newOb, w.boxPort))
                                        connected++;
                                    else
                                        connErrs.add("in:" + juce::String(w.boxPort));
                                }
                                for (auto& w : outWires) {
                                    t_object* d_o = pd::Interface::checkObject(w.far);
                                    if (d_o && newOb && pd::Interface::createConnection(cnv, newOb, w.boxPort, d_o, w.farPort))
                                        connected++;
                                    else
                                        connErrs.add("out:" + juce::String(w.boxPort));
                                }
                                canvas_dirty(cnv, 1);
                                // Single DSP recompile for the whole swap.
                                canvas_update_dsp();

                                // Clear undo queue safely on Pd scheduler thread to prevent stale pointer crashes on Ctrl+Z.
                                SmallArray<pd::Atom> undoAtoms;
                                undoAtoms.add(pd::Atom(proc->generateSymbol(canvasName)));
                                proc->receiveSysMessage("mcp_clear_undo", undoAtoms);
                                for (auto& e : staleErrs) connErrs.add(e);
                                proc->mcpStableObjectMap[canvasName.toStdString()][safeName.toStdString()] = newObj;
                                proc->mcpSerialCounter++;
                                proc->mcpStableSerialMap[newObj] = proc->mcpSerialCounter;
                                proc->mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
                                newTempId = safeName;
                            }
                        }
                        sys_unlock();

                        if (newTempId.isEmpty()) return; // error reply already sent

                        // Sync GUI so plugdata displays the newly instantiated abstraction.
                        proc->enqueueFunctionAsync([p = proc] { p->synchroniseCanvases(); });

                        juce::String receipt = "{\"ok\":true,\"tempId\":\"" + newTempId
                            + "\",\"inlets\":" + juce::String(static_cast<int>(inWires.size()))
                            + ",\"outlets\":" + juce::String(static_cast<int>(outWires.size()))
                            + ",\"connected\":" + juce::String(connected)
                            + ",\"warnings\":[" + [&]() -> juce::String {
                                juce::StringArray q;
                                for (auto& e : connErrs) q.add("\"" + e + "\"");
                                return q.joinIntoString(",");
                            }() + "]}";
                        bridge->sendReply(replyAddr, receipt);
                    });
                }
            }
        }
        return;
    }

    if (action == "measure_text") {
        // /pd/measure_text <jsonStringsArray> <fontSize> <correlationId>
        // Fast font metrics batch using JUCE Font / Inter font.
        // Returns: /pd/measure_text/reply/<corrId> {"text": width, ...}
        if (msg.size() >= 2) {
            auto jsonArrayStr  = getArgString(msg[0]);
            float fontSize     = getArgFloat(msg[1]);
            if (fontSize <= 0.0f) fontSize = 12.0f;
            auto correlationId = (msg.size() > 2) ? getArgString(msg[2]) : "0";
            auto replyAddr     = "/pd/measure_text/reply/" + correlationId;

            juce::var parsed = juce::JSON::parse(jsonArrayStr);
            juce::Font font = Fonts::getCurrentFont().withHeight(fontSize);
            auto* resultObj = new juce::DynamicObject();

            if (auto* arr = parsed.getArray()) {
                for (const auto& item : *arr) {
                    auto str = item.toString();
                    resultObj->setProperty(str, font.getStringWidth(str));
                }
            } else if (parsed.isString()) {
                auto str = parsed.toString();
                resultObj->setProperty(str, font.getStringWidth(str));
            }

            juce::String jsonResult = juce::JSON::toString(juce::var(resultObj));
            sendReply(replyAddr, jsonResult);
        }
        return;
    }

    if (action == "tidy") {
        if (msg.size() >= 2 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            int count = static_cast<int>(getArgFloat(msg[1]));
            std::vector<juce::String> targetIds;
            int cursor = 2;
            for (int i = 0; i < count && cursor < msg.size(); i++) {
                targetIds.push_back(getArgString(msg[cursor++]));
            }
            auto correlationId = (cursor < msg.size()) ? getArgString(msg[cursor]) : "0";

            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

            if (cnv) {
                SmallArray<t_gobj*> targetObjs;
                if (!targetIds.empty()) {
                    for (const auto& id : targetIds) {
                        t_gobj* g = processor->resolveStableId(canvasName, id);
                        if (g) targetObjs.add(g);
                    }
                } else {
                    for (t_gobj* y = cnv->gl_list; y; y = y->g_next) {
                        targetObjs.add(y);
                    }
                }

                if (!targetObjs.empty()) {
                    pd::Interface::tidy(cnv, targetObjs);
                    processor->synchroniseCanvases();
                }
                sendReply("/pd/tidy/reply/" + correlationId, 1.0f);
            } else {
                sendReply("/pd/tidy/reply/" + correlationId, 0.0f);
            }
        }
        return;
    }

    if (action == "align") {
        if (msg.size() >= 3 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto alignStr = getArgString(msg[1]).toLowerCase();
            int count = static_cast<int>(getArgFloat(msg[2]));
            std::vector<juce::String> targetIds;
            int cursor = 3;
            for (int i = 0; i < count && cursor < msg.size(); i++) {
                targetIds.push_back(getArgString(msg[cursor++]));
            }
            auto correlationId = (cursor < msg.size()) ? getArgString(msg[cursor]) : "0";

            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

            if (cnv) {
                juce::MessageManager::callAsync([proc = processor, cnv, canvasName, alignStr, targetIds, correlationId, bridge = this]() {
                    Canvas* canvasComp = getOrCreateCanvasComponent(proc, cnv);

                    if (!canvasComp) {
                        bridge->sendReply("/pd/align/reply/" + correlationId, 0.0f);
                        return;
                    }

                    if (!targetIds.empty()) {
                        canvasComp->patch.deselectAll();
                        for (const auto& id : targetIds) {
                            t_gobj* g = proc->resolveStableId(canvasName, id);
                            if (g) {
                                for (auto* objComp : canvasComp->objects) {
                                    if (objComp && objComp->getPointer() == g) {
                                        canvasComp->setSelected(objComp, true);
                                        break;
                                    }
                                }
                            }
                        }
                    }

                    Align alignMode = Align::Left;
                    if (alignStr == "right") alignMode = Align::Right;
                    else if (alignStr == "hcentre" || alignStr == "hcenter") alignMode = Align::HCentre;
                    else if (alignStr == "hdistribute") alignMode = Align::HDistribute;
                    else if (alignStr == "top") alignMode = Align::Top;
                    else if (alignStr == "bottom") alignMode = Align::Bottom;
                    else if (alignStr == "vcentre" || alignStr == "vcenter") alignMode = Align::VCentre;
                    else if (alignStr == "vdistribute") alignMode = Align::VDistribute;

                    canvasComp->alignObjects(alignMode);
                    resetCanvasUndo(proc, canvasName);
                    bridge->sendReply("/pd/align/reply/" + correlationId, 1.0f);
                });
            } else {
                sendReply("/pd/align/reply/" + correlationId, 0.0f);
            }
        }
        return;
    }

    if (action == "zoom_to_fit") {
        if (msg.size() >= 1 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = (msg.size() >= 2) ? getArgString(msg[1]) : "0";

            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

            juce::MessageManager::callAsync([proc = processor, cnv, correlationId, bridge = this]() {
                Canvas* canvasComp = getOrCreateCanvasComponent(proc, cnv);

                if (canvasComp) {
                    canvasComp->zoomToFitAll();
                    bridge->sendReply("/pd/zoom_to_fit/reply/" + correlationId, 1.0f);
                } else {
                    bridge->sendReply("/pd/zoom_to_fit/reply/" + correlationId, 0.0f);
                }
            });
        }
        return;
    }

    // /pd/screenshot_canvas <canvas> <scale> <corrId>
    // Renders the focused JUCE Canvas component to a PNG temp file and replies
    // with the absolute file path. The MCP server reads the file, base64-encodes
    // it, and returns an MCP ImageContent block so the LLM sees the patch visually.
    //
    // Threading: createComponentSnapshot MUST run on the JUCE Message Thread.
    //            We use callAsync (same pattern as zoom_to_fit). DSP is untouched —
    //            zero dropout guaranteed.
    //
    // Scale: 0.25–1.0 (default 0.5). Canvas can be large; 50% captures structure
    //        without burning LLM vision token budget.
    if (action == "screenshot_canvas") {
        if (msg.size() >= 1 && processor) {
            auto canvasName     = normalizeCanvas(getArgString(msg[0]));
            float scale         = (msg.size() >= 2) ? static_cast<float>(getArgFloat(msg[1])) : 0.5f;
            auto  correlationId = (msg.size() >= 3) ? getArgString(msg[2]) : "0";
            bool  wantLabels    = (msg.size() >= 4) ? (getArgFloat(msg[3]) > 0.5f) : false;

            scale = juce::jlimit(0.1f, 2.0f, scale);

            // Resolve t_canvas — null is fine, we fall back to focused editor canvas
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);

            // ScreenshotLabel struct; collection happens inside callAsync after
            // canvasComp is resolved to avoid the null-cnv race (guard 'cnv' here
            // may be null when the named canvas falls back to focused editor).
            struct ScreenshotLabel {
                int x, y, w, h;       // pd coords
                juce::String text;    // "name · class" or "class"
            };

            juce::MessageManager::callAsync([proc = processor, cnv, scale, correlationId, bridge = this,
                                             wantLabels, canvasName]() {
                // Prefer named canvas; fall back to focused canvas in any open editor
                Canvas* canvasComp = cnv ? getOrCreateCanvasComponent(proc, cnv) : nullptr;
                if (!canvasComp && proc) {
                    for (auto* editor : proc->getEditors()) {
                        if (editor && editor->getCurrentCanvas()) {
                            canvasComp = editor->getCurrentCanvas();
                            break;
                        }
                    }
                }

                if (!canvasComp) {
                    bridge->sendReply("/pd/screenshot_canvas/reply/" + correlationId, juce::String("error:no_canvas"));
                    return;
                }

                // ── NVG surface capture ──
                // PlugData's Canvas is an NVGComponent: its JUCE paint() path is EMPTY
                // and all rendering goes to the GPU via PluginEditor::nvgSurface.
                // createComponentSnapshot therefore returns a blank image — verified
                // (3KB all-black PNGs). Correct path: force-render the target region
                // into the surface FBO, then pull pixels with renderFrameToImage.
                PluginEditor* editor = nullptr;
                for (auto* ed : proc->getEditors()) {
                    if (ed && ed->getCurrentCanvas() == canvasComp) { editor = ed; break; }
                }
                if (!editor) {
                    bridge->sendReply("/pd/screenshot_canvas/reply/" + correlationId, juce::String("error:no_editor"));
                    return;
                }

                // ── Native window capture (createSnapshotOfNativeWindow) ──
                // PlugData's Canvas is NVGComponent: JUCE paint() is empty, GPU-side
                // rendering. createSnapshotOfNativeWindow uses XGetImage on the X11
                // display connection JUCE already holds — no shell, no env issues.
                // It returns a juce::Image of the WHOLE editor window at native pixels.
                auto* peer = editor->getPeer();
                if (!peer) {
                    bridge->sendReply("/pd/screenshot_canvas/reply/" + correlationId, juce::String("error:no_peer"));
                    return;
                }

                // Canvas viewport + editor bounds in SCREEN coordinates.
                juce::Rectangle<int> viewportScreen;
                if (canvasComp->viewport != nullptr)
                    viewportScreen = canvasComp->viewport->getScreenBounds();
                else
                    viewportScreen = canvasComp->getScreenBounds();
                auto editorScreen = editor->getScreenBounds();

                // Grab the whole editor window via XGetImage (JUCE public API).
                // getNativeHandle() returns ::Window (XID) on Linux — same value
                // xwd -id uses, but via the existing X11 connection (no shell needed).
                auto img = juce::createSnapshotOfNativeWindow(peer->getNativeHandle());
                if (!img.isValid()) {
                    bridge->sendReply("/pd/screenshot_canvas/reply/" + correlationId, juce::String("error:snapshot_failed"));
                    return;
                }
                DBG("[screenshot_canvas] snapshot " + juce::String(img.getWidth()) + "x" + juce::String(img.getHeight())
                    + " editorScreen=" + editorScreen.toString()
                    + " viewportScreen=" + viewportScreen.toString());

                // createSnapshotOfNativeWindow returns the image at the display's
                // physical pixel scale (already divided by desktop scale inside JUCE).
                // editorScreen + viewportScreen are in logical pixels; no extra scale needed.
                // Crop to the viewport sub-rect within the window.
                juce::Rectangle<int> crop;
                crop.setX(viewportScreen.getX() - editorScreen.getX());
                crop.setY(viewportScreen.getY() - editorScreen.getY());
                crop.setWidth(viewportScreen.getWidth());
                crop.setHeight(viewportScreen.getHeight());
                crop = crop.getIntersection(img.getBounds());
                if (crop.isEmpty()) {
                    bridge->sendReply("/pd/screenshot_canvas/reply/" + correlationId,
                        juce::String("error:empty_crop w=") + juce::String(img.getWidth())
                        + " h=" + juce::String(img.getHeight())
                        + " crop=" + crop.toString());
                    return;
                }

                // Guard: at least 1% pixel variance (catches blank/hidden windows)
                {
                    juce::Image::BitmapData bd(img, juce::Image::BitmapData::readOnly);
                    long varying = 0, total = 0;
                    juce::uint8 firstR = 0, firstG = 0, firstB = 0;
                    bool haveFirst = false;
                    for (int row = crop.getY(); row < crop.getBottom(); row += 6) {
                        for (int col = crop.getX(); col < crop.getRight(); col += 6) {
                            auto px = img.getPixelAt(col, row);
                            if (!haveFirst) { firstR = px.getRed(); firstG = px.getGreen(); firstB = px.getBlue(); haveFirst = true; }
                            if (px.getRed() != firstR || px.getGreen() != firstG || px.getBlue() != firstB) varying++;
                            total++;
                        }
                    }
                    if (total > 0 && varying * 100 / total < 1) {
                        bridge->sendReply("/pd/screenshot_canvas/reply/" + correlationId,
                            juce::String("error:blank_window (uniform canvas — hidden or off-screen?)"));
                        return;
                    }
                }

                img = img.getClippedImage(crop);

                // Apply requested scale
                if (std::abs(scale - 1.0f) > 0.01f && img.getWidth() > 0 && img.getHeight() > 0) {
                    int targetW = juce::roundToInt(img.getWidth() * scale);
                    int targetH = juce::roundToInt(img.getHeight() * scale);
                    if (targetW > 0 && targetH > 0) {
                        juce::Image resized(juce::Image::ARGB, targetW, targetH, true);
                        juce::Graphics rg(resized);
                        rg.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
                        rg.drawImage(img, juce::Rectangle<float>(0, 0, (float)targetW, (float)targetH),
                            juce::RectanglePlacement::stretchToFit);
                        img = resized;
                    }
                }

                // ── Label collection (inside callAsync, on Message Thread) ──
                // Collect here after canvasComp is resolved so we always walk the
                // exact canvas that was rendered — never a stale/null pre-resolved cnv.
                // sys_lock from Message Thread is safe (same pattern as /pd/diagnose).
                std::vector<ScreenshotLabel> labels;
                if (wantLabels) {
                    static const std::unordered_set<juce::String> guiClasses = {
                        "knob", "vsl", "hsl", "vu", "tgl", "bng", "nbx",
                        "hradio", "vradio", "cnv"
                    };

                    // t_canvas* from resolved canvasComp; walk to root for pd-main
                    t_canvas* liveCnv = canvasComp->patch.getRawPointer();
                    if (canvasName == "pd-main" || canvasName == "main") {
                        auto* g = reinterpret_cast<t_glist*>(liveCnv);
                        while (g && g->gl_owner) g = g->gl_owner;
                        if (g) liveCnv = reinterpret_cast<t_canvas*>(g);
                    }

                    // ptrToId from mcpStableObjectMap for tempId fallback naming
                    std::unordered_map<t_gobj*, juce::String> ptrToId;
                    if (proc) {
                        auto mapIt = proc->mcpStableObjectMap.find(canvasName.toStdString());
                        if (mapIt != proc->mcpStableObjectMap.end())
                            for (auto& [tid, ptr] : mapIt->second)
                                if (ptr) ptrToId[ptr] = juce::String(tid);
                    }

                    sys_lock();
                    if (liveCnv) {
                        for (t_gobj* y = liveCnv->gl_list; y; y = y->g_next) {
                            t_class* cl = pd_class(&y->g_pd);
                            const char* clName = class_getname(cl);
                            if (!clName) continue;
                            juce::String className = juce::String::fromUTF8(clName);
                            if (!guiClasses.count(className)) continue;

                            juce::String semantic;
                            if (className == "knob") {
                                auto* knb = reinterpret_cast<t_fake_knob*>(y);
                                knob_get_rcv(knb);
                                knob_get_snd(knb);
                                auto valid = [](t_symbol* s) -> bool {
                                    return s && s->s_name && s->s_name[0]
                                        && juce::String::fromUTF8(s->s_name) != "empty";
                                };
                                if (valid(knb->x_rcv_raw))      semantic = juce::String::fromUTF8(knb->x_rcv_raw->s_name);
                                else if (valid(knb->x_snd_raw)) semantic = juce::String::fromUTF8(knb->x_snd_raw->s_name);
                            } else {
                                auto* iem = reinterpret_cast<t_iemgui*>(y);
                                auto valid = [](t_symbol* s) -> bool {
                                    return s && s != gensym("") && s->s_name && s->s_name[0]
                                        && juce::String::fromUTF8(s->s_name) != "empty";
                                };
                                // Guard with x_fsf flags — raw x_snd/x_rcv are garbage when not set (vu bug: "nosndno")
                                if (iem->x_fsf.x_rcv_able && valid(iem->x_rcv))      semantic = juce::String::fromUTF8(iem->x_rcv->s_name);
                                else if (iem->x_fsf.x_snd_able && valid(iem->x_snd)) semantic = juce::String::fromUTF8(iem->x_snd->s_name);
                            }
                            if (semantic.isEmpty()) {
                                auto it = ptrToId.find(y);
                                if (it != ptrToId.end()) semantic = it->second;
                            }
                            semantic = semantic.replace("\\ ", " ");

                            int x = 0, yy = 0, w = 0, h = 0;
                            pd::Interface::getObjectBounds(liveCnv, y, &x, &yy, &w, &h);
                            if (w <= 0) w = 60;
                            if (h <= 0) h = 20;

                            ScreenshotLabel lbl;
                            lbl.x = x; lbl.y = yy; lbl.w = w; lbl.h = h;
                            lbl.text = semantic.isNotEmpty()
                                ? semantic + " · " + className
                                : className;
                            labels.push_back(lbl);
                        }
                    }
                    sys_unlock();

                    // Diagnostic: label count + first label coords -> stderr (DBG)
                    DBG("[screenshot_labels] collected=" + juce::String((int)labels.size())
                        + " liveCnv=" + juce::String(liveCnv ? "ok" : "NULL")
                        + " img=" + juce::String(img.getWidth()) + "x" + juce::String(img.getHeight()));
                    if (!labels.empty()) {
                        float zoom0 = getValue<float>(canvasComp->zoomScale);
                        if (zoom0 <= 0.001f) zoom0 = 1.0f;
                        int vx0 = canvasComp->viewport ? canvasComp->viewport->getViewArea().getX() : 0;
                        int vy0 = canvasComp->viewport ? canvasComp->viewport->getViewArea().getY() : 0;
                        DBG("[screenshot_labels] first=" + labels[0].text
                            + " pd(" + juce::String(labels[0].x) + "," + juce::String(labels[0].y) + ")"
                            + " zoom=" + juce::String(zoom0, 2)
                            + " view=(" + juce::String(vx0) + "," + juce::String(vy0) + ")"
                            + " imgX=" + juce::String((labels[0].x * zoom0 - vx0) * scale, 1)
                            + " imgY=" + juce::String((labels[0].y * zoom0 - vy0) * scale, 1));
                    }
                }

                // ── Paint labels onto the captured bitmap (never the canvas) ──
                // Coordinate model (Object::updateBounds + Canvas transform):
                //   object in canvas-component px = canvasOrigin + pdPos
                //   viewport shows canvas at viewPos; zoom via component transform
                //   object in viewport px = zoom × (canvasOrigin + pdPos − viewPos)
                //   image px = (viewport-relative) × scale, since img IS the viewport
                if (!labels.empty()) {
                    float zoom = 1.0f;
                    int viewX = 0, viewY = 0;
                    if (canvasComp->viewport) {
                        zoom = getValue<float>(canvasComp->zoomScale);
                        if (zoom <= 0.001f) zoom = 1.0f;
                        viewX = canvasComp->viewport->getViewPositionX();
                        viewY = canvasComp->viewport->getViewPositionY();
                    }
                    auto const canvasOrigin = canvasComp->canvasOrigin;

                    juce::Graphics g(img);
                    juce::Font labelFont = Fonts::getCurrentFont()
                        .withHeight(juce::jlimit(9.0f, 16.0f, 12.0f * scale));
                    g.setFont(labelFont);

                    for (auto const& lbl : labels) {
                        // object top-left in viewport px
                        float vpX = zoom * static_cast<float>(canvasOrigin.x + lbl.x - viewX);
                        float vpY = zoom * static_cast<float>(canvasOrigin.y + lbl.y - viewY);
                        // → image coords (image = viewport × scale)
                        float imgX = vpX * scale;
                        float imgY = vpY * scale;
                        float objHImg = static_cast<float>(lbl.h) * zoom * scale;

                        float tw = labelFont.getStringWidthFloat(lbl.text);
                        float th = labelFont.getHeight();
                        float pad = 2.0f * scale;
                        float chipH = th + 2.0f * pad;
                        float chipW = tw + 3.0f * pad;

                        // Place chip above the object; flip below when clipped at top
                        float chipY = imgY - chipH - 1.0f;
                        if (chipY < 0.0f) chipY = imgY + objHImg + 1.0f;

                        // Skip labels fully outside the capture
                        if (imgX + chipW < 0.0f || imgX > static_cast<float>(img.getWidth())
                            || chipY + chipH < 0.0f || chipY > static_cast<float>(img.getHeight()))
                            continue;

                        g.setColour(juce::Colours::black.withAlpha(0.72f));
                        g.fillRoundedRectangle(juce::Rectangle<float>(imgX, chipY, chipW, chipH), 3.0f * scale);
                        g.setColour(juce::Colours::white.withAlpha(0.92f));
                        g.drawText(lbl.text, juce::Rectangle<float>(imgX + pad, chipY + pad, tw, th),
                            juce::Justification::centredLeft);
                    }
                }

                // Write PNG to a temp file — TS reads it synchronously (same machine)
                auto tmpFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                    .getChildFile("plugdata_canvas_" + correlationId + ".png");

                juce::FileOutputStream fos(tmpFile);
                if (!fos.openedOk()) {
                    bridge->sendReply("/pd/screenshot_canvas/reply/" + correlationId, juce::String("error:file_open"));
                    return;
                }

                juce::PNGImageFormat png;
                if (!png.writeImageToStream(img, fos)) {
                    bridge->sendReply("/pd/screenshot_canvas/reply/" + correlationId, juce::String("error:png_encode"));
                    return;
                }
                fos.flush();

                bridge->sendReply("/pd/screenshot_canvas/reply/" + correlationId,
                    tmpFile.getFullPathName());
            });
        }
        return;
    }


    if (action == "deoverlap") {
        // /pd/deoverlap <canvas> <count> <id…> <corrId>
        // PRD layout-v2 Phase B1: minimal-displacement push on TRUE rects
        // (gobj_getrect via getObjectBounds, AABB + 5px pad — same as collisions).
        // Visual-only: pd::Interface::moveObject (w_displacefn), same path as
        // mcp_move_batch_id — no DSP touch, zero dropout. Single undo sequence.
        if (msg.size() >= 2 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            int count = static_cast<int>(getArgFloat(msg[1]));
            std::vector<juce::String> targetIds;
            int cursor = 2;
            for (int i = 0; i < count && cursor < (int)msg.size(); i++) {
                targetIds.push_back(getArgString(msg[cursor++]));
            }
            auto correlationId = (cursor < (int)msg.size()) ? getArgString(msg[cursor]) : "0";

            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
            if (!cnv) {
                sendReply("/pd/deoverlap/reply/" + correlationId, 0.0f);
                return;
            }

            sys_lock();
            std::vector<t_gobj*> objs;
            if (!targetIds.empty()) {
                for (auto& id : targetIds) {
                    t_gobj* g = processor->resolveStableId(canvasName, id);
                    if (g) objs.push_back(g);
                }
            } else {
                for (t_gobj* y = cnv->gl_list; y; y = y->g_next) objs.push_back(y);
            }
            if (objs.empty()) {
                sys_unlock();
                sendReply("/pd/deoverlap/reply/" + correlationId, 1.0f);
                return;
            }

            Canvas* guiCanvas = mcpFindGuiCanvasFor(processor, cnv);
            struct DR { t_gobj* g; int x, y, w, h; };
            std::vector<DR> rects;
            rects.reserve(objs.size());
            for (auto* g : objs) {
                int x = 0, yy = 0, w = 0, h = 0;
                getTrueObjectBounds(cnv, g, guiCanvas, &x, &yy, &w, &h);
                rects.push_back({ g, x, yy, w, h });
            }

            const int PAD = 5;
            const int MAXPASS = 24;
            auto snap10 = [](int v) { return (v / 10) * 10; };
            for (int pass = 0; pass < MAXPASS; pass++) {
                bool anyHit = false;
                for (size_t i = 0; i < rects.size(); i++) {
                    for (size_t j = i + 1; j < rects.size(); j++) {
                        auto& a = rects[i];
                        auto& b = rects[j];
                        bool hit = a.x < b.x + b.w + PAD && a.x + a.w + PAD > b.x
                                && a.y < b.y + b.h + PAD && a.y + a.h + PAD > b.y;
                        if (!hit) continue;
                        anyHit = true;
                        int overlapX = std::min(a.x + a.w + PAD - b.x, b.x + b.w + PAD - a.x);
                        int overlapY = std::min(a.y + a.h + PAD - b.y, b.y + b.h + PAD - a.y);
                        // Keep relative order: push the later object (j) along the
                        // minimal axis, snapped to the 10px grid (min 10px step).
                        if (overlapX <= overlapY) {
                            int acx = a.x + a.w / 2, bcx = b.x + b.w / 2;
                            int dx = snap10(overlapX + 9);
                            if (dx < 10) dx = 10;
                            b.x += (bcx >= acx ? dx : -dx);
                        } else {
                            int acy = a.y + a.h / 2, bcy = b.y + b.h / 2;
                            int dy = snap10(overlapY + 9);
                            if (dy < 10) dy = 10;
                            b.y += (bcy >= acy ? dy : -dy);
                        }
                    }
                }
                if (!anyHit) break;
            }

            int moved = 0;
            for (auto& r : rects) {
                int ox = 0, oy = 0, ow = 0, oh = 0;
                pd::Interface::getObjectBounds(cnv, r.g, &ox, &oy, &ow, &oh);
                if (r.x != ox || r.y != oy) {
                    pd::Interface::moveObject(cnv, r.g, r.x, r.y);
                    moved++;
                }
            }
            if (moved > 0) {
                canvas_dirty(cnv, 1);
                resetCanvasUndo(processor, canvasName);
            }
            sys_unlock();

            processor->enqueueFunctionAsync([p = processor] { p->synchroniseCanvases(); });
            sendReply("/pd/deoverlap/reply/" + correlationId, 1.0f);
        }
        return;
    }

    // ── /pd/compose — P4 role-based composition ─────────────────────────
    // /pd/compose <canvas> <pad> <snap> <corrId>
    if (action == "compose") {
        auto canvasName    = normalizeCanvas(getArgString(msg[0]));
        int pad  = msg.size() > 1 ? static_cast<int>(getArgFloat(msg[1])) : 5;
        int snap = msg.size() > 2 ? static_cast<int>(getArgFloat(msg[2])) : 10;
        auto correlationId = msg.size() > 3 ? getArgString(msg[3]) : "0";
        juce::String replyAddr = "/pd/compose/reply/" + correlationId;

        t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
        if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
        if (!cnv) { sendReply(replyAddr, 0.0f); return; }

        sys_lock();
        int moved = composeLayout(processor, cnv, canvasName, pad, snap);
        if (moved > 0) resetCanvasUndo(processor, canvasName);
        sys_unlock();

        processor->enqueueFunctionAsync([p = processor] { p->synchroniseCanvases(); });
        sendReply(replyAddr, static_cast<float>(moved));
        return;
    }

    if (action == "pillars") {
        // /pd/pillars <canvas> [count id1 id2...] <corrId>
        // PRD layout-v2 Phase B: Eurorack modular column arrangement.
        // Assigns objects to columns at x = 50, 350, 650, 950... by floor(x/300),
        // with strictly monotonic Y ordering per pillar, snapped to 10px grid.
        if (msg.size() >= 1 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            int count = (msg.size() > 2) ? static_cast<int>(getArgFloat(msg[1])) : 0;
            std::vector<juce::String> targetIds;
            int cursor = 2;
            for (int i = 0; i < count && cursor < (int)msg.size(); i++) {
                targetIds.push_back(getArgString(msg[cursor++]));
            }
            auto correlationId = (cursor < (int)msg.size()) ? getArgString(msg[cursor])
                               : (msg.size() > 1 ? getArgString(msg[1]) : "0");

            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
            if (!cnv) {
                sendReply("/pd/pillars/reply/" + correlationId, 0.0f);
                return;
            }

            sys_lock();
            std::vector<t_gobj*> objs;
            if (!targetIds.empty()) {
                for (auto& id : targetIds) {
                    t_gobj* g = processor->resolveStableId(canvasName, id);
                    if (g) objs.push_back(g);
                }
            } else {
                for (t_gobj* y = cnv->gl_list; y; y = y->g_next) objs.push_back(y);
            }
            if (objs.empty()) {
                sys_unlock();
                sendReply("/pd/pillars/reply/" + correlationId, 1.0f);
                return;
            }

            Canvas* guiCanvas = mcpFindGuiCanvasFor(processor, cnv);
            struct PR { t_gobj* g; int x, y, w, h; int pillar; };
            std::vector<PR> rects;
            rects.reserve(objs.size());
            for (auto* g : objs) {
                int x = 0, yy = 0, w = 0, h = 0;
                getTrueObjectBounds(cnv, g, guiCanvas, &x, &yy, &w, &h);
                int p = std::max(0, (x + 100) / 300);
                rects.push_back({ g, x, yy, w, h, p });
            }

            auto snap10 = [](int v) { return (v / 10) * 10; };

            // Group into pillars and sort each pillar by original Y
            std::map<int, std::vector<size_t>> pillarGroups;
            for (size_t i = 0; i < rects.size(); ++i) {
                pillarGroups[rects[i].pillar].push_back(i);
            }

            for (auto& [pIdx, indices] : pillarGroups) {
                std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
                    return rects[a].y < rects[b].y;
                });
                int colX = 50 + pIdx * 300;
                int curY = 50;
                for (size_t idx : indices) {
                    rects[idx].x = colX;
                    rects[idx].y = curY;
                    curY = snap10(curY + rects[idx].h + 20);
                }
            }

            int moved = 0;
            for (auto& r : rects) {
                int ox = 0, oy = 0, ow = 0, oh = 0;
                pd::Interface::getObjectBounds(cnv, r.g, &ox, &oy, &ow, &oh);
                if (r.x != ox || r.y != oy) {
                    pd::Interface::moveObject(cnv, r.g, r.x, r.y);
                    moved++;
                }
            }
            if (moved > 0) {
                canvas_dirty(cnv, 1);
                resetCanvasUndo(processor, canvasName);
            }
            sys_unlock();

            processor->enqueueFunctionAsync([p = processor] { p->synchroniseCanvases(); });
            sendReply("/pd/pillars/reply/" + correlationId, 1.0f);
        }
        return;
    }

    if (action == "flow") {
        // /pd/flow <canvas> [count id1 id2...] <corrId>
        // PRD layout-v2 Phase B: Audio-semantic ranking.
        // Sources (no signal in) top rank -> DSP processing mid -> sinks (dac~/catch~) bottom.
        // Control objects placed in left gutter. 10px grid snapped, single undo sequence.
        if (msg.size() >= 1 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            int count = (msg.size() > 2) ? static_cast<int>(getArgFloat(msg[1])) : 0;
            std::vector<juce::String> targetIds;
            int cursor = 2;
            for (int i = 0; i < count && cursor < (int)msg.size(); i++) {
                targetIds.push_back(getArgString(msg[cursor++]));
            }
            auto correlationId = (cursor < (int)msg.size()) ? getArgString(msg[cursor])
                               : (msg.size() > 1 ? getArgString(msg[1]) : "0");

            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
            if (!cnv) {
                sendReply("/pd/flow/reply/" + correlationId, 0.0f);
                return;
            }

            sys_lock();
            std::vector<t_gobj*> objs;
            if (!targetIds.empty()) {
                for (auto& id : targetIds) {
                    t_gobj* g = processor->resolveStableId(canvasName, id);
                    if (g) objs.push_back(g);
                }
            } else {
                for (t_gobj* y = cnv->gl_list; y; y = y->g_next) objs.push_back(y);
            }
            if (objs.empty()) {
                sys_unlock();
                sendReply("/pd/flow/reply/" + correlationId, 1.0f);
                return;
            }

            auto snap10 = [](int v) { return (v / 10) * 10; };

            // 1. Identify connections & rate
            std::unordered_map<t_gobj*, std::vector<t_gobj*>> sigDownstream;
            std::unordered_map<t_gobj*, int> sigInCount;
            std::unordered_set<t_gobj*> hasSignalOut;
            std::unordered_set<t_gobj*> hasSignalIn;

            t_linetraverser lt;
            linetraverser_start(&lt, cnv);
            t_outconnect* oc = nullptr;
            while ((oc = linetraverser_next_nosize(&lt))) {
                t_gobj* sg = &lt.tr_ob->ob_g;
                t_gobj* dg = &lt.tr_ob2->ob_g;
                bool srcIsSig = (lt.tr_outlet && lt.tr_outlet->o_sym == gensym("signal"));
                bool destIsSig = (lt.tr_ob2 && obj_issignalinlet(lt.tr_ob2, lt.tr_inno) != 0);
                if (srcIsSig || destIsSig) {
                    sigDownstream[sg].push_back(dg);
                    sigInCount[dg]++;
                    hasSignalOut.insert(sg);
                    hasSignalIn.insert(dg);
                }
            }

            // 2. Classify objects
            struct FR {
                t_gobj* g;
                int x, y, w, h;
                int category; // 0 = control gutter, 1 = source, 2 = DSP, 3 = sink
                int rank;
                juce::String nodeClass;
            };
            std::vector<FR> rects;
            rects.reserve(objs.size());

            Canvas* guiCanvas = mcpFindGuiCanvasFor(processor, cnv);
            for (auto* g : objs) {
                int x = 0, yy = 0, w = 0, h = 0;
                getTrueObjectBounds(cnv, g, guiCanvas, &x, &yy, &w, &h);

                t_class* cl = pd_class(&g->g_pd);
                const char* clName = class_getname(cl);
                juce::String className = clName ? juce::String::fromUTF8(clName) : juce::String("unknown");
                juce::String nodeClass = className;
                juce::String abstractName;
                if (cl == canvas_class) {
                    nodeClass = "pd";
                    if (pd::getAbstractionFileName(g, abstractName) && abstractName.isNotEmpty())
                        nodeClass = abstractName;
                } else if (cl == garray_class) {
                    nodeClass = "table";
                } else if (pd::Interface::isTextObject(g)) {
                    t_text* textObj = reinterpret_cast<t_text*>(g);
                    if (textObj->te_type == T_MESSAGE) nodeClass = "msg";
                    else if (textObj->te_type == T_ATOM) nodeClass = className.containsIgnoreCase("symbol") ? "symbolatom" : "floatatom";
                    else if (textObj->te_type == T_TEXT) nodeClass = "text";
                    else {
                        char* tb = nullptr; int tsz = 0;
                        binbuf_gettext(textObj->te_binbuf, &tb, &tsz);
                        if (tb && tsz > 0) {
                            juce::String full = juce::String::fromUTF8(tb, tsz).trim();
                            nodeClass = full.upToFirstOccurrenceOf(" ", false, false);
                            freebytes(tb, tsz);
                        }
                    }
                }

                t_object* ob = pd::Interface::checkObject(g);
                bool isSigIn0 = (ob != nullptr && obj_issignalinlet(ob, 0) != 0);
                bool isSink = (nodeClass == "dac~" || nodeClass == "catch~" || nodeClass == "throw~"
                               || (hasSignalIn.count(g) > 0 && hasSignalOut.count(g) == 0));
                bool isSource = ((isSigIn0 == false && sigInCount[g] == 0 && (hasSignalOut.count(g) > 0 || nodeClass.endsWith("~")))
                                 || nodeClass == "noise~" || nodeClass == "r~" || nodeClass == "readsf~");

                int cat = 2; // default DSP
                if (nodeClass == "r" || nodeClass == "s" || nodeClass == "f" || nodeClass == "float"
                    || nodeClass == "pack" || nodeClass == "unpack" || nodeClass == "t" || nodeClass == "trigger"
                    || nodeClass == "bng" || nodeClass == "tgl" || nodeClass == "msg"
                    || nodeClass == "floatatom" || nodeClass == "symbolatom"
                    || (!nodeClass.endsWith("~") && hasSignalIn.count(g) == 0 && hasSignalOut.count(g) == 0)) {
                    cat = 0; // Control Left Gutter
                } else if (isSink) {
                    cat = 3; // Sink Bottom
                } else if (isSource) {
                    cat = 1; // Source Top
                }

                rects.push_back({ g, x, yy, w, h, cat, (cat == 1 ? 0 : (cat == 3 ? 3 : 1)), nodeClass });
            }

            // 3. Compute topological ranks for DSP objects (cat == 2)
            for (auto& r : rects) {
                if (r.category == 1) { // Source
                    std::vector<t_gobj*> queue = sigDownstream[r.g];
                    int rk = 1;
                    std::unordered_set<t_gobj*> visited;
                    while (!queue.empty() && rk < 3) {
                        std::vector<t_gobj*> nextQ;
                        for (auto* down : queue) {
                            if (visited.count(down)) continue;
                            visited.insert(down);
                            for (auto& target : rects) {
                                if (target.g == down && target.category == 2) {
                                    target.rank = std::max(target.rank, rk);
                                }
                            }
                            for (auto* nxt : sigDownstream[down]) nextQ.push_back(nxt);
                        }
                        queue = nextQ;
                        rk++;
                    }
                }
            }

            // 4. Place Control gutter (cat == 0) at x = 50
            int ctlY = 50;
            for (auto& r : rects) {
                if (r.category == 0) {
                    r.x = 50;
                    r.y = ctlY;
                    ctlY = snap10(ctlY + r.h + 20);
                }
            }

            // 5. Place Signal ranks at x >= 250
            std::map<int, std::vector<size_t>> rankGroups;
            for (size_t i = 0; i < rects.size(); ++i) {
                if (rects[i].category != 0) {
                    rankGroups[rects[i].rank].push_back(i);
                }
            }

            for (auto& [rank, indices] : rankGroups) {
                int rankY = 50 + rank * 150;
                int curX = 250;
                for (size_t idx : indices) {
                    rects[idx].x = curX;
                    rects[idx].y = rankY;
                    curX = snap10(curX + rects[idx].w + 30);
                }
            }

            // 6. Minimal deoverlap pass to ensure no collision
            const int PAD = 5;
            for (int pass = 0; pass < 12; pass++) {
                bool hit = false;
                for (size_t i = 0; i < rects.size(); i++) {
                    for (size_t j = i + 1; j < rects.size(); j++) {
                        auto& a = rects[i];
                        auto& b = rects[j];
                        if (a.x < b.x + b.w + PAD && a.x + a.w + PAD > b.x
                            && a.y < b.y + b.h + PAD && a.y + a.h + PAD > b.y) {
                            hit = true;
                            b.x = snap10(a.x + a.w + 20);
                        }
                    }
                }
                if (!hit) break;
            }

            int moved = 0;
            for (auto& r : rects) {
                int ox = 0, oy = 0, ow = 0, oh = 0;
                pd::Interface::getObjectBounds(cnv, r.g, &ox, &oy, &ow, &oh);
                if (r.x != ox || r.y != oy) {
                    pd::Interface::moveObject(cnv, r.g, r.x, r.y);
                    moved++;
                }
            }
            if (moved > 0) {
                canvas_dirty(cnv, 1);
                resetCanvasUndo(processor, canvasName);
            }
            sys_unlock();

            processor->enqueueFunctionAsync([p = processor] { p->synchroniseCanvases(); });
            sendReply("/pd/flow/reply/" + correlationId, 1.0f);
        }
        return;
    }

    if (action == "undo") {
        if (msg.size() >= 1 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";

            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && (canvasName == "pd-main" || canvasName == "main" || canvasName.isEmpty())) cnv = pd_this->pd_canvaslist;

            if (cnv) {
                juce::MessageManager::callAsync([proc = processor, cnv, correlationId, bridge = this]() {
                    Canvas* canvasComp = nullptr;
                    for (auto* editor : proc->getEditors()) {
                        if (!editor) continue;
                        for (auto* c : editor->getCanvases()) {
                            if (c && c->patch.getUncheckedPointer() == cnv) {
                                canvasComp = c;
                                break;
                            }
                        }
                        if (canvasComp) break;
                    }

                    if (proc->hasMcpTransaction(cnv)) {
                        proc->undoMcpTransaction(cnv);
                        proc->synchroniseCanvases();
                        bridge->sendReply("/pd/undo/reply/" + correlationId, 1.0f);
                    } else if (canvasComp) {
                        canvasComp->undo();
                        bridge->sendReply("/pd/undo/reply/" + correlationId, 1.0f);
                    } else {
                        pd::Interface::undo(cnv);
                        proc->synchroniseCanvases();
                        bridge->sendReply("/pd/undo/reply/" + correlationId, 1.0f);
                    }
                });
            } else {
                sendReply("/pd/undo/reply/" + correlationId, 0.0f);
            }
        }
        return;
    }

    if (action == "redo") {
        if (msg.size() >= 1 && processor) {
            auto canvasName = normalizeCanvas(getArgString(msg[0]));
            auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";

            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && (canvasName == "pd-main" || canvasName == "main" || canvasName.isEmpty())) cnv = pd_this->pd_canvaslist;

            if (cnv) {
                juce::MessageManager::callAsync([proc = processor, cnv, correlationId, bridge = this]() {
                    Canvas* canvasComp = nullptr;
                    for (auto* editor : proc->getEditors()) {
                        if (!editor) continue;
                        for (auto* c : editor->getCanvases()) {
                            if (c && c->patch.getUncheckedPointer() == cnv) {
                                canvasComp = c;
                                break;
                            }
                        }
                        if (canvasComp) break;
                    }

                    if (proc->hasMcpRedoTransaction(cnv)) {
                        proc->redoMcpTransaction(cnv);
                        proc->synchroniseCanvases();
                        bridge->sendReply("/pd/redo/reply/" + correlationId, 1.0f);
                    } else if (canvasComp) {
                        canvasComp->redo();
                        bridge->sendReply("/pd/redo/reply/" + correlationId, 1.0f);
                    } else {
                        pd::Interface::redo(cnv);
                        proc->synchroniseCanvases();
                        bridge->sendReply("/pd/redo/reply/" + correlationId, 1.0f);
                    }
                });
            } else {
                sendReply("/pd/redo/reply/" + correlationId, 0.0f);
            }
        }
        return;
    }
}

void MCPBridge::handleParamDomain(const juce::String& /*paramName*/, const juce::OSCMessage& msg)
{
    if (!processor || msg.size() < 1) return;

    // Lua-parity: /param <name> <value...> — the receiver name is the FIRST
    // argument, never the address path.
    auto paramName = getArgString(msg[0]);
    if (paramName.isEmpty() || paramName == "nil") return;

    if (msg.size() == 2) {
        auto const& v = msg[1];
        if (v.isFloat32() || v.isInt32()) {
            processor->sendFloat(paramName.toRawUTF8(), getArgFloat(v));
        } else {
            auto s = getArgString(v);
            if (s.startsWith("s:")) s = s.substring(2);
            processor->sendSymbol(paramName.toRawUTF8(), s.toRawUTF8());
        }
    } else if (msg.size() > 2) {
        SmallArray<pd::Atom> atoms;
        for (int i = 1; i < msg.size(); ++i) {
            auto const& arg = msg[i];
            if (arg.isFloat32() || arg.isInt32()) {
                atoms.add(pd::Atom(getArgFloat(arg)));
            } else {
                auto s = getArgString(arg);
                if (s.startsWith("s:")) s = s.substring(2);
                atoms.add(pd::Atom(processor->generateSymbol(s)));
            }
        }
        processor->sendList(paramName.toRawUTF8(), atoms);
    }
}

void MCPBridge::handleTriggerDomain(const juce::String& triggerAction, const juce::OSCMessage& msg)
{
    if (!processor) return;

    if (triggerAction == "note" && msg.size() >= 3) {
        int ch = static_cast<int>(getArgFloat(msg[0]));
        int pitch = static_cast<int>(getArgFloat(msg[1]));
        int vel = static_cast<int>(getArgFloat(msg[2]));
        processor->sendNoteOn(ch, pitch, vel);
        return;
    }

    // Lua-parity: /trigger <name> — the receiver name is the FIRST argument.
    auto bangName = msg.size() > 0 ? getArgString(msg[0]) : juce::String();
    if (bangName.isEmpty() || bangName == "nil") return;
    processor->sendBang(bangName.toRawUTF8());
}

void MCPBridge::handleTelemetryDomain(const juce::String& /*telAction*/, const juce::OSCMessage& msg)
{
    if (!processor || msg.size() < 1) return;

    // Lua-parity: /telemetry <action> [args...] — the action is the FIRST argument.
    auto action = getArgString(msg[0]);

    if (action == "dsp" && msg.size() >= 2) {
        float val = getArgFloat(msg[1]);
        if (val > 0.5f) processor->startDSP();
        else processor->releaseDSP();
    } else if (action == "delete" || action == "delete_on") {
        // [telemetry, delete, index]           → canvas defaults to main
        // [telemetry, delete, canvas, index]   → explicit canvas
        int idx = -1;
        juce::String canvasName = "pd-main";
        if (msg.size() == 2) {
            idx = static_cast<int>(getArgFloat(msg[1]));
        } else if (msg.size() >= 3) {
            canvasName = normalizeCanvas(getArgString(msg[1]));
            idx = static_cast<int>(getArgFloat(msg[2]));
        }
        if (idx >= 0) {
            sys_lock();
            t_canvas* canvas = processor->getCanvasBySymbol(canvasName);
            if (canvas) {
                t_gobj* obj = glistObjectAt(canvas, idx);
                if (obj) {
                    SmallArray<t_gobj*> toDelete;
                    toDelete.add(obj);
                    pd::Interface::removeObjects(canvas, toDelete);
                    canvas_dirty(canvas, 1);
                }
            }
            sys_unlock();
        }
    }
}

static t_garray* findGArrayInCanvas(t_canvas* cnv, t_symbol* nameSym)
{
    if (!cnv) return nullptr;
    for (t_gobj* y = cnv->gl_list; y; y = y->g_next) {
        if (pd_class(&y->g_pd) == garray_class) {
            t_garray* ga = reinterpret_cast<t_garray*>(y);
            t_symbol* sym = nullptr;
            if (garray_getname(ga, &sym) && sym == nameSym) {
                return ga;
            }
        } else if (pd_class(&y->g_pd) == canvas_class) {
            t_canvas* sub = reinterpret_cast<t_canvas*>(y);
            t_garray* ga = findGArrayInCanvas(sub, nameSym);
            if (ga) return ga;
        }
    }
    return nullptr;
}

void MCPBridge::handleArrayDomain(const juce::String& arrayAction, const juce::OSCMessage& msg)
{
    if (!processor || msg.size() < 2) return;

    // Two conventions:
    //   write/read: /array/<action> <name> <subpatch> <corrId> ...  (name = msg[0])
    //   stats:      /array ["stats", name, sampleRate, corrId]      (name = msg[1])
    auto arrayName = arrayAction == "stats" ? getArgString(msg[1]) : getArgString(msg[0]);
    auto canvasName = normalizeCanvas(getArgString(msg[1]));

    // /array/load <name> <subpatch> <filePath> <corrId>
    // Loads a WAV/AIFF from disk into the array natively (JUCE AudioFormatReader).
    // One message, no [soundfiler] object, no bang-and-wait dance. The disk read
    // happens OUTSIDE sys_lock; only resize+fill run under sys_lock. Reply arg0 = size,
    // or a negative error code.
    if (arrayAction == "load") {
        auto filePath      = msg.size() > 2 ? getArgString(msg[2]) : "";
        auto correlationId = msg.size() > 3 ? getArgString(msg[3]) : "0";
        juce::String replyAddr = "/array/load/reply/" + correlationId;

        juce::File file(filePath);
        if (!file.existsAsFile()) { sendReply(replyAddr, -2.0f); return; }

        juce::AudioFormatManager formatManager;
        formatManager.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
        if (!reader) { sendReply(replyAddr, -3.0f); return; }

        int const numChannels = static_cast<int>(reader->numChannels);
        int const numSamples  = static_cast<int>(reader->lengthInSamples);
        if (numChannels <= 0 || numSamples <= 0) { sendReply(replyAddr, -4.0f); return; }

        // Mono downmix (JUCE downmixes stereo→mono when the destination is 1ch).
        juce::AudioBuffer<float> fileBuf(1, numSamples);
        if (!reader->read(&fileBuf, 0, numSamples, 0, true, true)) { sendReply(replyAddr, -5.0f); return; }
        const float* src = fileBuf.getReadPointer(0);

        sys_lock();
        auto nameSym = gensym(arrayName.toRawUTF8());
        t_garray* garray = reinterpret_cast<t_garray*>(pd_findbyclass(nameSym, garray_class));
        if (!garray) {
            for (t_canvas* c = pd_this->pd_canvaslist; c; c = c->gl_next) {
                garray = findGArrayInCanvas(c, nameSym);
                if (garray) break;
            }
        }

        // Auto-create the array if it doesn't exist yet.
        if (!garray) {
            t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
            if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
            if (cnv) {
                int nobj = 0;
                for (t_gobj* y = cnv->gl_list; y; y = y->g_next) nobj++;
                int stagger = (nobj * 14) % 420;
                juce::String pasta = "#N canvas 0 0 450 300 (subpatch) 0;\n#X array "
                    + arrayName + " " + juce::String(numSamples) + " float 2;\n#X coords 0 1 "
                    + juce::String(numSamples > 1 ? numSamples - 1 : 1) + " -1 200 140 1 0 0;\n#X restore "
                    + juce::String(60 + stagger) + " " + juce::String(60 + stagger) + " graph;";
                pd::Interface::paste(cnv, pasta.toRawUTF8());
                garray = reinterpret_cast<t_garray*>(pd_findbyclass(nameSym, garray_class));
                if (!garray) {
                    for (t_canvas* c = pd_this->pd_canvaslist; c; c = c->gl_next) {
                        garray = findGArrayInCanvas(c, nameSym);
                        if (garray) break;
                    }
                }
                if (garray) {
                    canvas_dirty(cnv, 1);
                    processor->enqueueFunctionAsync([p = processor] { p->synchroniseCanvases(); });
                }
            }
        }

        if (!garray) { sys_unlock(); sendReply(replyAddr, -6.0f); return; }

        garray_resize_long(garray, numSamples);
        int size = 0;
        t_word* vec = nullptr;
        if (!garray_getfloatwords(garray, &size, &vec) || !vec) {
            sys_unlock();
            sendReply(replyAddr, -7.0f);
            return;
        }

        int const copy = size < numSamples ? size : numSamples;
        for (int i = 0; i < copy; ++i) vec[i].w_float = src[i];
        garray_redraw(garray);
        sys_unlock();

        sendReply(replyAddr, static_cast<float>(copy));
        return;
    }

    sys_lock();
    auto nameSym = gensym(arrayName.toRawUTF8());
    t_garray* garray = reinterpret_cast<t_garray*>(pd_findbyclass(nameSym, garray_class));
    if (!garray) {
        for (t_canvas* c = pd_this->pd_canvaslist; c; c = c->gl_next) {
            garray = findGArrayInCanvas(c, nameSym);
            if (garray) break;
        }
    }

    // PlugData instantiates arrays through the GUI; arrays created via the
    // MCP text-object path may never bind their garray. If a write targets a
    // missing array, create it as a graph-on-parent via paste (the same
    // mechanism pd::Patch::createObject uses for arrays).
    if (!garray && (arrayAction == "write" || arrayAction == "write_bulk") && msg.size() >= 4) {
        int reqSize = 2048;
        if (arrayAction == "write" && msg.size() >= 5) {
            int chunkIndex = static_cast<int>(getArgFloat(msg[3]));
            int dataCount = msg.size() - 5;
            reqSize = std::max(2048, chunkIndex * 128 + dataCount);
        } else if (arrayAction == "write_bulk" && msg.size() >= 4) {
            int offset = static_cast<int>(getArgFloat(msg[3]));
            int dataCount = msg.size() - 4;
            reqSize = std::max(2048, offset + dataCount);
        }

        t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
        if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
        if (cnv) {
            // Stagger each auto-created array so they don't stack on top of
            // each other in the top-left corner.
            int nobj = 0;
            for (t_gobj* y = cnv->gl_list; y; y = y->g_next) nobj++;
            int stagger = (nobj * 14) % 420;
            juce::String pasta = "#N canvas 0 0 450 300 (subpatch) 0;\n#X array "
                + arrayName + " " + juce::String(reqSize) + " float 2;\n#X coords 0 1 "
                + juce::String(reqSize > 1 ? reqSize - 1 : 1) + " -1 200 140 1 0 0;\n#X restore "
                + juce::String(60 + stagger) + " " + juce::String(60 + stagger) + " graph;";
            pd::Interface::paste(cnv, pasta.toRawUTF8());
            garray = reinterpret_cast<t_garray*>(pd_findbyclass(nameSym, garray_class));
            if (!garray) {
                for (t_canvas* c = pd_this->pd_canvaslist; c; c = c->gl_next) {
                    garray = findGArrayInCanvas(c, nameSym);
                    if (garray) break;
                }
            }
            if (garray) {
                canvas_dirty(cnv, 1);
                processor->enqueueFunctionAsync([p = processor] { p->synchroniseCanvases(); });
            }
        }
    }

    if (!garray) {
        sys_unlock();
        return;
    }

    int size = 0;
    t_word* vec = nullptr;
    if (!garray_getfloatwords(garray, &size, &vec) || !vec) {
        sys_unlock();
        return;
    }

    if (arrayAction == "write_bulk" && msg.size() >= 4) {
        // [name, subpatch, corrId, offset, data...]
        auto correlationId = getArgString(msg[2]);
        int offset = static_cast<int>(getArgFloat(msg[3]));
        int dataStart = 4;
        int dataCount = msg.size() - dataStart;

        int requiredSize = offset + dataCount;
        if (requiredSize > size) {
            garray_resize_long(garray, requiredSize);
            if (!garray_getfloatwords(garray, &size, &vec) || !vec) {
                sys_unlock();
                juce::OSCMessage errReply { juce::OSCAddressPattern("/array/write_bulk/reply/" + correlationId) };
                errReply.addArgument(static_cast<int32>(-1));
                errReply.addArgument(static_cast<int32>(0));
                sender.send(errReply);
                return;
            }
        }

        int written = 0;
        for (int i = dataStart; i < msg.size() && (offset + written) < size; ++i) {
            vec[offset + written].w_float = getArgFloat(msg[i]);
            written++;
        }
        garray_redraw(garray);
        sys_unlock();

        juce::OSCMessage reply { juce::OSCAddressPattern("/array/write_bulk/reply/" + correlationId) };
        reply.addArgument(static_cast<int32>(written));
        reply.addArgument(static_cast<int32>(size));
        sender.send(reply);
        return;
    } else if (arrayAction == "read_bulk") {
        // Format: [arrayName, subpatch, correlationId, offset, limit]
        auto correlationId = msg.size() > 2 ? getArgString(msg[2]) : "0";
        int offset = msg.size() > 3 ? static_cast<int>(getArgFloat(msg[3])) : 0;
        int limit = msg.size() > 4 ? static_cast<int>(getArgFloat(msg[4])) : size;

        int readStart = std::max(0, std::min(offset, size - 1));
        int readCount = std::min(limit, size - readStart);

        // Cap at 8192 samples per single reply (32KB OSC payload)
        readCount = std::min(readCount, 8192);

        juce::OSCMessage reply { juce::OSCAddressPattern("/array/read_bulk/reply/" + correlationId) };
        reply.addArgument(arrayName);
        reply.addArgument(static_cast<int32>(size));         // total array size
        reply.addArgument(static_cast<int32>(readStart));    // actual start
        reply.addArgument(static_cast<int32>(readCount));    // actual count
        for (int i = readStart; i < readStart + readCount; ++i) {
            reply.addArgument(vec[i].w_float);
        }
        sys_unlock();
        sender.send(reply);
        return;
    } else if (arrayAction == "write" && msg.size() >= 5) {
        // [name, subpatch, corrId, chunkIndex, totalChunks, data...]
        auto correlationId = getArgString(msg[2]);
        int chunkIndex = static_cast<int>(getArgFloat(msg[3]));
        int const CHUNK = 128;
        int offset = chunkIndex * CHUNK;
        int dataStart = 5;

        for (int i = dataStart; i < msg.size() && (offset + i - dataStart) < size; ++i) {
            vec[offset + i - dataStart].w_float = getArgFloat(msg[i]);
        }
        garray_redraw(garray);
        sys_unlock();

        sendReply("/array/write/reply/" + correlationId, static_cast<float>(size));
    } else if (arrayAction == "read") {
        auto correlationId = msg.size() > 2 ? getArgString(msg[2]) : "0";
        int offset = msg.size() > 3 ? static_cast<int>(getArgFloat(msg[3])) : 0;
        int limit = msg.size() > 4 ? static_cast<int>(getArgFloat(msg[4])) : 512;

        int readStart = std::max(0, std::min(offset, size - 1));
        int readCount = std::min(limit, size - readStart);

        int const CHUNK = 4096;
        int totalChunks = (readCount + CHUNK - 1) / CHUNK;

        for (int c = 0; c < totalChunks; ++c) {
            int start = readStart + c * CHUNK;
            int stop = std::min(start + CHUNK, readStart + readCount);

            juce::OSCMessage chunkMsg { juce::OSCAddressPattern("/array/read/chunk/" + correlationId) };
            chunkMsg.addArgument(static_cast<int32>(c));
            chunkMsg.addArgument(static_cast<int32>(totalChunks));
            for (int i = start; i < stop; ++i) {
                chunkMsg.addArgument(vec[i].w_float);
            }
            sender.send(chunkMsg);
        }
        sys_unlock();

        juce::OSCMessage doneMsg { juce::OSCAddressPattern("/array/read/done/" + correlationId) };
        doneMsg.addArgument(arrayName);
        doneMsg.addArgument(static_cast<int32>(size));
        sender.send(doneMsg);
    } else if (arrayAction == "stats") {
        float sampleRate = msg.size() > 2 ? getArgFloat(msg[2]) : 44100.0f;
        auto correlationId = msg.size() > 3 ? getArgString(msg[3]) : "0";

        float minVal = 0.0f, maxVal = 0.0f, sumSq = 0.0f;
        int zeroCrossings = 0;
        float prevVal = 0.0f;

        if (size > 0) {
            minVal = vec[0].w_float;
            maxVal = vec[0].w_float;
            for (int i = 0; i < size; ++i) {
                float v = vec[i].w_float;
                if (v < minVal) minVal = v;
                if (v > maxVal) maxVal = v;
                sumSq += v * v;

                if (i > 0) {
                    if ((prevVal < 0 && v >= 0) || (prevVal > 0 && v <= 0)) {
                        zeroCrossings++;
                    }
                }
                prevVal = v;
            }
        }
        sys_unlock();

        float rms = size > 0 ? std::sqrt(sumSq / static_cast<float>(size)) : 0.0f;
        float estimatedPitch = size > 0 ? (static_cast<float>(zeroCrossings) / 2.0f) * (sampleRate / static_cast<float>(size)) : 0.0f;

        juce::OSCMessage reply { juce::OSCAddressPattern("/array/stats/reply/" + correlationId) };
        reply.addArgument(arrayName);
        reply.addArgument(static_cast<int32>(size));
        reply.addArgument(minVal);
        reply.addArgument(maxVal);
        reply.addArgument(rms);
        reply.addArgument(static_cast<int32>(zeroCrossings));
        reply.addArgument(estimatedPitch);
        sender.send(reply);
    } else if (arrayAction == "mutate") {
        // /array/mutate <name> <subpatch> <corrId> <reverse|invert|crush>
        // In-place array mutation under a single sys_lock — zero OSC data round-trip,
        // so it never floods the bridge queue (the cause of live audio drops when
        // the TS server shuttles 427k floats through ~150 chunked messages).
        auto correlationId = msg.size() > 2 ? getArgString(msg[2]) : "0";
        auto op = msg.size() > 3 ? getArgString(msg[3]) : "reverse";

        if (op == "reverse") {
            for (int i = 0; i < size / 2; ++i) {
                float tmp = vec[i].w_float;
                vec[i].w_float = vec[size - 1 - i].w_float;
                vec[size - 1 - i].w_float = tmp;
            }
        } else if (op == "invert") {
            for (int i = 0; i < size; ++i) vec[i].w_float = -vec[i].w_float;
        } else if (op == "crush") {
            int const bits = 8;
            float const q = 1.0f / static_cast<float>(1 << (bits - 1));
            for (int i = 0; i < size; ++i) {
                float v = vec[i].w_float;
                vec[i].w_float = std::round(v / q) * q;
            }
        }

        garray_redraw(garray);
        sys_unlock();
        sendReply("/array/mutate/reply/" + correlationId, static_cast<float>(size));
        return;
    } else {
        sys_unlock();
    }
}

void MCPBridge::handleBridgeDomain(const juce::String& bridgeAction, const juce::OSCMessage& msg)
{
    if (bridgeAction == "connect") {
        if (msg.size() >= 2) {
            auto ip = getArgString(msg[0]);
            int port = static_cast<int>(getArgFloat(msg[1]));
            if (port > 0) {
                sender.disconnect();
                sender.connect(ip.isNotEmpty() ? ip : "127.0.0.1", port);
            }
        }
        sendReply("/bridge/connect/ack", "127.0.0.1");
    } else if (bridgeAction == "capabilities") {
        auto correlationId = msg.size() > 0 ? getArgString(msg[0]) : "0";
        juce::OSCMessage reply { juce::OSCAddressPattern("/bridge/capabilities/reply") };
        reply.addArgument(juce::String("11.8"));
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
        reply.addArgument(juce::String("trigger"));
        reply.addArgument(juce::String("telemetry"));
        reply.addArgument(juce::String("array_io"));
        reply.addArgument(juce::String("morph"));
        reply.addArgument(juce::String("adaptive_dump"));
        reply.addArgument(juce::String("finalize"));
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
        reply.addArgument(juce::String("meter_trace"));
        reply.addArgument(juce::String("inline_mappings"));
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
        reply.addArgument(juce::String("compose"));
        reply.addArgument(juce::String("flow"));
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
        // Labeled screenshots: GUI-widget name chips painted on the bitmap only
        reply.addArgument(juce::String("screenshot_labels"));
        // Offline faster-than-realtime render to WAV (background DSP bake)
        reply.addArgument(juce::String("render"));
        reply.addArgument(juce::String("boot:" + bootToken));
        sender.send(reply);
    }
}

void MCPBridge::handleTransportDomain(const juce::String& action, const juce::OSCMessage& msg)
{
    // Sample-accurate transport clock. Position derives from a running sample
    // counter (advanced in audioTick()), so bar/beat/step and the next downbeat
    // are exact — no Date.now()/wall-clock drift.

    if (action == "set_bpm") {
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

    if (action == "set_subdivision") {
        if (msg.size() >= 1) {
            int sub = (int)getArgFloat(msg[0]);
            if (sub < 1) sub = 1;
            if (sub > 64) sub = 64;
            transport.subdivision.store(sub, std::memory_order_relaxed);
        }
        return;
    }

    if (action == "set_mode") {
        if (msg.size() >= 1) {
            juce::String const mode = getArgString(msg[0]);
            transport.modeClock.store(mode == "clock", std::memory_order_relaxed);
        }
        return;
    }

    if (action == "start") {
        transport.running.store(true, std::memory_order_relaxed);
        return;
    }

    if (action == "pause") {
        transport.running.store(false, std::memory_order_relaxed);
        return;
    }

    if (action == "status") {
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
}

void MCPBridge::handleSeqDomain(const juce::String& action, const juce::OSCMessage& msg)
{
    // /seq/start <jobId> <corrId> <json>
    // /seq/stop  <jobId>
    if (action == "start") {
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

    if (action == "stop") {
        auto const jobId = msg.size() > 0 ? getArgString(msg[0]) : juce::String();
        juce::ScopedLock sl(seqLock);
        for (auto& job : seqJobs)
            if (job.jobId == jobId || jobId.isEmpty())
                job.active = false;
        if (jobId.isEmpty()) seqJobs.clear();
        return;
    }
}

void MCPBridge::advanceSequencer(int blockSize)
{
    double const sampleRate = (processor && processor->getSampleRate() > 0.0) ? processor->getSampleRate() : 44100.0;

    juce::ScopedLock sl(seqLock);
    for (auto& job : seqJobs) {
        if (!job.active) continue;

        // Free jobs advance their own sample counter; locked jobs read the transport.
        job.jobSampleCounter += blockSize;
        int64_t const pos = job.syncToClock
            ? transport.sampleCounter.load(std::memory_order_relaxed)
            : job.jobSampleCounter;

        if (job.nextStepSample < 0)
            job.nextStepSample = pos;  // first tick: anchor to now

        double const effBpm = job.bpm * (1.0 + job.drift / 100.0);
        double const stepSamples = sampleRate * 60.0 / (effBpm * job.subdivision);

        int guard = 0;
        while (job.nextStepSample <= pos && guard++ < 64) {
            // Fire every non-rest step value across all tracks for this step.
            for (auto const& track : job.tracks) {
                if (track.steps.empty()) continue;
                int const ti = job.stepIndex % (int)track.steps.size();
                float const val = track.steps[ti];
                if (val == 0.0f) continue;
                enqueueSeqFire(track.isBang, track.name, val);
            }

            job.stepIndex = (job.stepIndex + 1) % job.patternLength;

            double len = stepSamples;
            if (job.swing > 0.0f && (job.stepIndex % 2) == 1)
                len += stepSamples * job.swing * 0.5;
            job.nextStepSample += (int64_t)len;
        }
    }
}

void MCPBridge::enqueueSeqFire(bool isBang, const juce::String& name, float value)
{
    PluginProcessor* p = processor;
    std::string const n = name.toStdString();
    // Defer the actual libpd dispatch to the message phase (sendMessagesFromQueue)
    // before the next block's performDSP — safe + block-accurate, zero UDP.
    p->enqueueFunctionAsync([p, isBang, n, value] {
        if (isBang) p->sendBang(n.c_str());
        else p->sendFloat(n.c_str(), value);
    });
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

void MCPBridge::audioTick()
{
    probeManager.audioTick();

    // Advance the native transport clock — one PD block per call, so the running
    // sample counter gives sample-accurate bar/beat/step position.
    int const blockSize = pd::Instance::getBlockSize();
    if (blockSize > 0) {
        if (transport.running.load(std::memory_order_relaxed))
            transport.sampleCounter.fetch_add(blockSize, std::memory_order_relaxed);
        advanceSequencer(blockSize);
    }
}

static float estimateFrequency(const float* buf, int n, float sampleRate)
{
    if (!buf || n < 128 || sampleRate <= 0.0f) return 0.0f;

    int minLag = std::max(2, static_cast<int>(sampleRate / 4000.0f));  // max freq 4kHz
    int maxLag = std::min(n / 2, static_cast<int>(sampleRate / 40.0f));  // min freq 40Hz

    if (minLag >= maxLag) return 0.0f;

    std::vector<float> corrs(maxLag + 1, 0.0f);

    for (int lag = minLag; lag <= maxLag; lag++) {
        float sumCross = 0.0f;
        float sumSq0 = 0.0f;
        float sumSqLag = 0.0f;
        int count = n - lag;
        for (int i = 0; i < count; i++) {
            float a = buf[i];
            float b = buf[i + lag];
            sumCross += a * b;
            sumSq0 += a * a;
            sumSqLag += b * b;
        }
        float denom = std::sqrt(sumSq0 * sumSqLag);
        if (denom > 1e-6f) {
            corrs[lag] = sumCross / denom;
        }
    }

    int peakLag = 0;
    for (int lag = minLag + 1; lag < maxLag; lag++) {
        if (corrs[lag] > 0.75f && corrs[lag] >= corrs[lag - 1] && corrs[lag] >= corrs[lag + 1]) {
            peakLag = lag;
            break;
        }
    }

    if (peakLag > 0) {
        float alpha = corrs[peakLag - 1];
        float beta = corrs[peakLag];
        float gamma = corrs[peakLag + 1];
        float denom = (alpha - 2.0f * beta + gamma);
        float delta = (std::abs(denom) > 1e-9f) ? (0.5f * (alpha - gamma) / denom) : 0.0f;
        float trueLag = static_cast<float>(peakLag) + delta;
        return (trueLag > 0.0f) ? (sampleRate / trueLag) : 0.0f;
    }

    return 0.0f;
}

ProbeManager::ProbeManager(MCPBridge* owner)
    : bridge(owner)
{
}

void ProbeManager::audioTick()
{
    for (auto& probe : probes) {
        if (!probe.active.load(std::memory_order_relaxed)) continue;

        auto* oc = probe.outconnect.load(std::memory_order_acquire);
        if (!oc) continue;

        auto* signal = outconnect_get_signal(oc);
        if (!signal || !signal->s_vec) continue;

        int n = signal->s_n;
        if (n <= 0) continue;
        float* vec = signal->s_vec;

        float sumSq = 0.0f, peak = 0.0f;
        for (int i = 0; i < n; i++) {
            float s = vec[i];
            float abs_s = std::abs(s);
            sumSq += s * s;
            if (abs_s > peak) peak = abs_s;
        }
        float rms = std::sqrt(sumSq / static_cast<float>(n));

        ProbeResult result { probe.probeId, rms, peak, n };
        probe.resultQueue.try_enqueue(result);

        int writePos = probe.ringWritePos.load(std::memory_order_relaxed);
        for (int i = 0; i < n; i++) {
            probe.ringBuffer[(writePos + i) % PROBE_RING_SIZE] = vec[i];
        }
        probe.ringWritePos.store((writePos + n) % PROBE_RING_SIZE, std::memory_order_release);
    }
}

int ProbeManager::startProbe(t_outconnect* oc, const juce::String& canvasName, const juce::String& tempId, int outletIndex, const juce::String& correlationId, int durationMs)
{
    for (auto& probe : probes) {
        if (!probe.active.load(std::memory_order_relaxed)) {
            ProbeResult discard;
            while (probe.resultQueue.try_dequeue(discard)) {}

            probe.probeId = nextProbeId.fetch_add(1);
            if (probe.probeId == 0) probe.probeId = nextProbeId.fetch_add(1);
            probe.correlationId = correlationId;
            probe.canvasName = canvasName;
            probe.tempId = tempId;
            probe.outletIndex = outletIndex;
            probe.durationMs = std::max(20, durationMs);
            probe.startTimeMs = juce::Time::getMillisecondCounter();
            probe.accRms = 0.0f;
            probe.accPeak = 0.0f;
            probe.accBlocks = 0;
            probe.ringWritePos.store(0, std::memory_order_relaxed);
            probe.ringBuffer.fill(0.0f);
            probe.outconnect.store(oc, std::memory_order_release);
            probe.active.store(true, std::memory_order_release);
            return static_cast<int>(probe.probeId);
        }
    }
    return -1;
}

void ProbeManager::stopProbe(uint32_t probeId, const juce::String& correlationId)
{
    for (auto& probe : probes) {
        if (probe.active.load(std::memory_order_relaxed) && probe.probeId == probeId) {
            ProbeResult item;
            while (probe.resultQueue.try_dequeue(item)) {
                probe.accRms += item.rms * item.rms;
                if (item.peak > probe.accPeak) probe.accPeak = item.peak;
                probe.accBlocks++;
            }

            float meanRms = (probe.accBlocks > 0) ? std::sqrt(probe.accRms / static_cast<float>(probe.accBlocks)) : 0.0f;
            float peakVal = probe.accPeak;
            float rmsDb = (meanRms > 1e-7f) ? (20.0f * std::log10(meanRms)) : -100.0f;
            float peakDb = (peakVal > 1e-7f) ? (20.0f * std::log10(peakVal)) : -100.0f;

            float sampleRate = 44100.0f;
            if (bridge && bridge->processor) {
                sampleRate = static_cast<float>(bridge->processor->getSampleRate());
            }
            float freq = estimateFrequency(probe.ringBuffer.data(), PROBE_RING_SIZE, sampleRate);

            auto corr = correlationId.isNotEmpty() ? correlationId : probe.correlationId;
            if (corr.isNotEmpty() && bridge) {
                juce::OSCMessage rep { juce::OSCAddressPattern("/meter/result/" + corr) };
                rep.addArgument(rmsDb);
                rep.addArgument(peakDb);
                rep.addArgument(freq);
                rep.addArgument(static_cast<int32>(probe.accBlocks));
                bridge->sender.send(rep);
            }

            probe.active.store(false, std::memory_order_release);
            probe.outconnect.store(nullptr, std::memory_order_release);
            return;
        }
    }
}

void ProbeManager::stopAllProbes(const juce::String& correlationId)
{
    for (auto& probe : probes) {
        if (probe.active.load(std::memory_order_relaxed)) {
            probe.active.store(false, std::memory_order_release);
            probe.outconnect.store(nullptr, std::memory_order_release);
            if (probe.correlationId.isNotEmpty() && bridge) {
                bridge->sendReply("/meter/error/" + probe.correlationId, "Probe cancelled");
            }
        }
    }
    if (correlationId.isNotEmpty() && bridge) {
        bridge->sendRawReply("/meter/stop_all/reply/" + correlationId);
    }
}

void ProbeManager::collectResults()
{
    auto now = juce::Time::getMillisecondCounter();
    float sampleRate = 44100.0f;
    if (bridge && bridge->processor) {
        sampleRate = static_cast<float>(bridge->processor->getSampleRate());
    }

    for (auto& probe : probes) {
        if (!probe.active.load(std::memory_order_relaxed)) continue;

        ProbeResult item;
        while (probe.resultQueue.try_dequeue(item)) {
            probe.accRms += item.rms * item.rms;
            if (item.peak > probe.accPeak) probe.accPeak = item.peak;
            probe.accBlocks++;
        }

        if (now - probe.startTimeMs >= static_cast<juce::int64>(probe.durationMs)) {
            if (probe.accBlocks == 0) {
                juce::String errCorr = probe.correlationId;
                bool wasSpectral = probe.spectral;
                probe.spectral = false;
                probe.active.store(false, std::memory_order_release);
                probe.outconnect.store(nullptr, std::memory_order_release);
                if (bridge) {
                    juce::String errAddr = wasSpectral ? "/meter/spectral/error/" : "/meter/error/";
                    bridge->sendReply(errAddr + errCorr, "Timeout: No audio blocks processed (is DSP running?)");
                }
                continue;
            }

            float meanRms = std::sqrt(probe.accRms / static_cast<float>(probe.accBlocks));
            float peakVal = probe.accPeak;
            float rmsDb = (meanRms > 1e-7f) ? (20.0f * std::log10(meanRms)) : -100.0f;
            float peakDb = (peakVal > 1e-7f) ? (20.0f * std::log10(peakVal)) : -100.0f;
            std::array<float, PROBE_RING_SIZE> linearBuf;
            int wPos = probe.ringWritePos.load(std::memory_order_acquire);
            for (int i = 0; i < PROBE_RING_SIZE; i++) {
                linearBuf[i] = probe.ringBuffer[(wPos + i) % PROBE_RING_SIZE];
            }
            float freq = estimateFrequency(linearBuf.data(), PROBE_RING_SIZE, sampleRate);

            if (probe.spectral && bridge) {
                // === SPECTRAL ANALYSIS (Phase 7) ===
                // Apply Hann window
                constexpr int N = PROBE_RING_SIZE;
                std::array<float, N> windowed;
                for (int i = 0; i < N; i++) {
                    float w = 0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * static_cast<float>(i) / static_cast<float>(N - 1)));
                    windowed[i] = linearBuf[i] * w;
                }

                // Run real FFT via FFTW3
                constexpr int NBINS = N / 2 + 1;
                std::array<float, N> fftInput;
                std::copy(windowed.begin(), windowed.end(), fftInput.begin());

                // Use fftwf (single precision)
                std::array<fftwf_complex, NBINS> fftOutput;
                fftwf_plan plan = fftwf_plan_dft_r2c_1d(N, fftInput.data(),
                    reinterpret_cast<fftwf_complex*>(fftOutput.data()), FFTW_ESTIMATE);
                fftwf_execute(plan);
                fftwf_destroy_plan(plan);

                // Compute magnitude spectrum (dB)
                std::array<float, NBINS> magnitudes;
                float binHz = sampleRate / static_cast<float>(N);
                float sumMag = 0.0f;
                float sumWeightedFreq = 0.0f;
                float sumLogMag = 0.0f;
                float maxMag = 0.0f;
                int maxBin = 0;

                for (int i = 0; i < NBINS; i++) {
                    float re = fftOutput[i][0];
                    float im = fftOutput[i][1];
                    float mag = std::sqrt(re * re + im * im) / static_cast<float>(N);
                    magnitudes[i] = mag;

                    if (i > 0) { // Skip DC bin for spectral features
                        sumMag += mag;
                        sumWeightedFreq += mag * (static_cast<float>(i) * binHz);
                        if (mag > 1e-10f) sumLogMag += std::log(mag);
                        else sumLogMag += std::log(1e-10f);
                        if (mag > maxMag) { maxMag = mag; maxBin = i; }
                    }
                }

                // Spectral centroid (Hz)
                float spectralCentroid = (sumMag > 1e-10f) ? (sumWeightedFreq / sumMag) : 0.0f;

                // Spectral flatness (0 = tonal, 1 = noise)
                int numBins = NBINS - 1; // exclude DC
                float geometricMean = std::exp(sumLogMag / static_cast<float>(numBins));
                float arithmeticMean = sumMag / static_cast<float>(numBins);
                float spectralFlatness = (arithmeticMean > 1e-10f) ? (geometricMean / arithmeticMean) : 0.0f;
                spectralFlatness = std::min(1.0f, std::max(0.0f, spectralFlatness));

                // Crest factor (peak / RMS)
                float crestFactor = (meanRms > 1e-7f) ? (peakVal / meanRms) : 0.0f;

                // Peak frequency bin
                float peakFreq = static_cast<float>(maxBin) * binHz;

                // Spectral rolloff (frequency below which 85% of energy lives)
                float totalEnergy = 0.0f;
                for (int i = 1; i < NBINS; i++) totalEnergy += magnitudes[i] * magnitudes[i];
                float rolloffThreshold = totalEnergy * 0.85f;
                float accumEnergy = 0.0f;
                float rolloffFreq = 0.0f;
                for (int i = 1; i < NBINS; i++) {
                    accumEnergy += magnitudes[i] * magnitudes[i];
                    if (accumEnergy >= rolloffThreshold) {
                        rolloffFreq = static_cast<float>(i) * binHz;
                        break;
                    }
                }

                // Top 8 frequency peaks (for harmonic analysis)
                struct FreqPeak { float freq; float magDb; };
                std::array<FreqPeak, 8> topPeaks {};
                std::array<float, NBINS> magCopy;
                std::copy(magnitudes.begin(), magnitudes.end(), magCopy.begin());
                for (int p = 0; p < 8; p++) {
                    int best = 1;
                    for (int i = 2; i < NBINS - 1; i++) {
                        if (magCopy[i] > magCopy[best]) best = i;
                    }
                    if (magCopy[best] < 1e-10f) break;
                    topPeaks[p].freq = static_cast<float>(best) * binHz;
                    topPeaks[p].magDb = 20.0f * std::log10(magCopy[best]);
                    // Zero out neighborhood to find next peak
                    for (int k = std::max(1, best - 3); k <= std::min(NBINS - 1, best + 3); k++) {
                        magCopy[k] = 0.0f;
                    }
                }

                // Build JSON response
                auto* rootObj = new juce::DynamicObject();
                rootObj->setProperty("rmsDb", rmsDb);
                rootObj->setProperty("peakDb", peakDb);
                rootObj->setProperty("fundamental", freq);
                rootObj->setProperty("spectralCentroid", spectralCentroid);
                rootObj->setProperty("spectralFlatness", spectralFlatness);
                rootObj->setProperty("spectralRolloff", rolloffFreq);
                rootObj->setProperty("crestFactor", crestFactor);
                rootObj->setProperty("peakFrequency", peakFreq);
                rootObj->setProperty("sampleRate", sampleRate);
                rootObj->setProperty("fftSize", N);
                rootObj->setProperty("binHz", binHz);
                rootObj->setProperty("blocks", probe.accBlocks);

                juce::Array<juce::var> peaksArray;
                for (int p = 0; p < 8 && topPeaks[p].freq > 0.0f; p++) {
                    auto* pk = new juce::DynamicObject();
                    pk->setProperty("freq", topPeaks[p].freq);
                    pk->setProperty("dB", topPeaks[p].magDb);
                    peaksArray.add(juce::var(pk));
                }
                rootObj->setProperty("peaks", peaksArray);

                juce::String jsonString = juce::JSON::toString(juce::var(rootObj), true);

                juce::OSCMessage rep { juce::OSCAddressPattern("/meter/spectral/result/" + probe.correlationId) };
                rep.addArgument(jsonString);
                bridge->sender.send(rep);

                probe.spectral = false;
            } else if (bridge) {
                juce::OSCMessage rep { juce::OSCAddressPattern("/meter/result/" + probe.correlationId) };
                rep.addArgument(rmsDb);
                rep.addArgument(peakDb);
                rep.addArgument(freq);
                rep.addArgument(static_cast<int32>(probe.accBlocks));
                bridge->sender.send(rep);
            }

            probe.active.store(false, std::memory_order_release);
            probe.outconnect.store(nullptr, std::memory_order_release);
        }
    }
}

int ProbeManager::getActiveProbeCount() const
{
    int count = 0;
    for (auto const& probe : probes) {
        if (probe.active.load(std::memory_order_relaxed)) {
            count++;
        }
    }
    return count;
}

bool MCPBridge::activateProbing()
{
    if (!plugdata_debugging_enabled()) {
        set_plugdata_debugging_enabled(1);
        sys_lock();
        canvas_update_dsp();
        sys_unlock();
        probeManager.setDebugEnabledByUs(true);
        return true;
    }
    return false;
}

t_outconnect* MCPBridge::resolveProbeTarget(const juce::String& canvasName, const juce::String& targetId, int outletIndex, juce::String& errorOut)
{
    if (!processor) {
        errorOut = "Processor unavailable";
        return nullptr;
    }

    t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
    if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
    if (!cnv) {
        errorOut = "Canvas not found: " + canvasName;
        return nullptr;
    }

    t_gobj* gobj = processor->resolveStableId(canvasName, targetId);

    if (!gobj) {
        bool isNumber = targetId.isNotEmpty();
        for (int i = (targetId.startsWith("-") ? 1 : 0); i < targetId.length(); ++i) {
            if (!juce::CharacterFunctions::isDigit(targetId[i])) {
                isNumber = false;
                break;
            }
        }
        if (isNumber) {
            int idx = targetId.getIntValue();
            gobj = glistObjectAt(cnv, idx);
        }
    }

    if (!gobj) {
        errorOut = "Object not found: " + targetId;
        return nullptr;
    }

    t_object* obj = pd::Interface::checkObject(gobj);
    if (!obj) {
        errorOut = "Target is not a valid Pd object: " + targetId;
        return nullptr;
    }

    t_outlet* outlet = obj->ob_outlet;
    for (int i = 0; i < outletIndex && outlet; i++) {
        outlet = outlet->o_next;
    }
    if (!outlet) {
        errorOut = "Outlet " + juce::String(outletIndex) + " not found on object " + targetId;
        return nullptr;
    }

    if (outlet->o_sym != gensym("signal")) {
        errorOut = "Outlet " + juce::String(outletIndex) + " is not a signal outlet (control rate)";
        return nullptr;
    }

    if (!outlet->o_connections) {
        errorOut = "Signal outlet has no connections";
        return nullptr;
    }

    return outlet->o_connections;
}

void MCPBridge::handleMeterDomain(const juce::String& meterAction, const juce::OSCMessage& msg)
{
    if (!processor) return;

    if (meterAction == "start") {
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
        sys_unlock();

        if (!oc) {
            sendReply("/meter/error/" + correlationId, errorOut);
            return;
        }

        int probeId = probeManager.startProbe(oc, canvasName, tempId, outletIndex, correlationId, durationMs);
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

    if (meterAction == "stop") {
        if (msg.size() < 1) return;
        uint32_t probeId = static_cast<uint32_t>(getArgFloat(msg[0]));
        juce::String correlationId = msg.size() > 1 ? getArgString(msg[1]) : "";
        probeManager.stopProbe(probeId, correlationId);
        return;
    }

    if (meterAction == "stop_all") {
        juce::String correlationId = msg.size() > 0 ? getArgString(msg[0]) : "";
        probeManager.stopAllProbes(correlationId);
        return;
    }

    if (meterAction == "query") {
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

    if (meterAction == "spectral") {
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
        sys_unlock();

        if (!oc) {
            sendReply("/meter/spectral/error/" + correlationId, errorOut);
            return;
        }

        int probeId = probeManager.startProbe(oc, canvasName, tempId, outletIndex, correlationId, durationMs);
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

    if (meterAction == "status") {
        juce::String correlationId = msg.size() > 0 ? getArgString(msg[0]) : "0";
        int activeCount = probeManager.getActiveProbeCount();
        int debugEnabled = plugdata_debugging_enabled();

        juce::OSCMessage rep { juce::OSCAddressPattern("/meter/status/reply/" + correlationId) };
        rep.addArgument(static_cast<int32>(activeCount));
        rep.addArgument(static_cast<int32>(debugEnabled));
        sender.send(rep);
        return;
    }

    if (meterAction == "master") {
        // /meter/master <canvasName> <correlationId>
        if (msg.size() < 1) return;
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        juce::String correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";

        bool justActivated = activateProbing();
        if (justActivated) {
            juce::Thread::sleep(5);
        }

        t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
        if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
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

        juce::OSCMessage rep { juce::OSCAddressPattern("/meter/master/reply/" + correlationId) };
        rep.addArgument(res.peakDb);
        rep.addArgument(res.rmsDb);
        rep.addArgument(static_cast<int32>(res.masterFound ? 1 : 0));
        rep.addArgument(res.masterType);
        sender.send(rep);
        return;
    }

    if (meterAction == "trace") {
        // /meter/trace <canvasName> <correlationId>
        if (msg.size() < 1) return;
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        juce::String correlationId = msg.size() > 1 ? getArgString(msg[1]) : "0";

        bool justActivated = activateProbing();
        if (justActivated) {
            juce::Thread::sleep(5);
        }

        t_canvas* cnv = processor->getCanvasBySymbol(canvasName);
        if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;
        if (!cnv) {
            sendReply("/meter/trace/reply/" + correlationId, "{\"error\":\"canvas not found: " + canvasName + "\"}");
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
}

void MCPBridge::timerCallback()
{
    probeManager.collectResults();

    std::vector<MorphJob> activeJobs;
    std::vector<juce::String> completedIds;

    {
        const juce::ScopedLock sl(morphLock);
        for (auto& job : morphJobs) {
            job.currentStep++;
            float t = static_cast<float>(job.currentStep) / static_cast<float>(job.totalSteps);
            if (t > 1.0f) t = 1.0f;

            for (auto const& p : job.params) {
                float val = p.from + (p.to - p.from) * t;
                if (processor) {
                    processor->sendFloat(p.name.toRawUTF8(), val);
                }
            }

            if (job.currentStep >= job.totalSteps) {
                completedIds.push_back(job.correlationId);
            } else {
                activeJobs.push_back(job);
            }
        }
        morphJobs = activeJobs;
    }

    for (auto const& id : completedIds) {
        sendRawReply("/morph/run/done/" + id);
    }

    {
        const juce::ScopedLock sl(morphLock);
        if (morphJobs.empty() && probeManager.getActiveProbeCount() == 0) {
            stopTimer();
        }
    }
}

void MCPBridge::sendSelectionTelemetry(const juce::String& selector, const SmallArray<pd::Atom>& list)
{
    if (!active.load()) return;

    juce::String addr;
    if (selector.startsWith("/")) addr = selector;
    else if (selector == "selected_object") addr = "/pd/ui/selection";
    else if (selector == "selection_count") addr = "/pd/ui/count";
    else if (selector == "selection_indices") addr = "/pd/ui/indices";
    else addr = "/pd/ui/" + selector;

    juce::OSCMessage msg { juce::OSCAddressPattern(addr) };
    for (int i = 0; i < list.size(); ++i) {
        if (list[i].isFloat()) {
            msg.addArgument(list[i].getFloat());
        } else if (list[i].isSymbol()) {
            msg.addArgument(juce::String::fromUTF8(list[i].getSymbol()->s_name));
        }
    }
    sender.send(msg);
}

void MCPBridge::sendConsoleLog(const juce::String& message, bool isError)
{
    if (!active.load()) return;

    juce::OSCMessage msg { juce::OSCAddressPattern(isError ? "/pd/error" : "/pd/log") };
    msg.addArgument(message);
    sender.send(msg);
}

void MCPBridge::sendPrompt(const juce::String& promptText)
{
    if (!active.load()) return;

    juce::OSCMessage msg { juce::OSCAddressPattern("/pd/mcp_prompt") };
    msg.addArgument(promptText);
    sender.send(msg);
}

void MCPBridge::sendReply(const juce::String& addressPattern, const juce::Array<juce::var>& args)
{
    if (!active.load()) return;

    juce::OSCMessage msg { juce::OSCAddressPattern(addressPattern) };
    for (auto const& v : args) {
        if (v.isDouble() || v.isInt() || v.isInt64()) {
            msg.addArgument(static_cast<float>(v));
        } else {
            msg.addArgument(v.toString());
        }
    }
    sender.send(msg);
}

void MCPBridge::sendReply(const juce::String& addressPattern, float val)
{
    if (!active.load()) return;

    juce::OSCMessage msg { juce::OSCAddressPattern(addressPattern) };
    msg.addArgument(val);
    sender.send(msg);
}

void MCPBridge::sendReply(const juce::String& addressPattern, const juce::String& str)
{
    if (!active.load()) return;

    juce::OSCMessage msg { juce::OSCAddressPattern(addressPattern) };
    msg.addArgument(str);
    sender.send(msg);
}

void MCPBridge::sendRawReply(const juce::String& addressPattern)
{
    if (!active.load()) return;

    juce::OSCMessage msg { juce::OSCAddressPattern(addressPattern) };
    sender.send(msg);
}
