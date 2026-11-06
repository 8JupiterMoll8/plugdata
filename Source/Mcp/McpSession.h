/*
 // MCP session state (Plan 3 P3.2 S4): everything the bridge keeps between commands —
 // overlay model, identity maps, transactions, recorder/meter/tap state. It used to live
 // in PluginProcessor.h; moved verbatim. Owned by McpHost (Source/Mcp/McpHost.h).
 */
#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <concurrentqueue.h>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_osc/juce_osc.h>

#include "Utility/Config.h"
#include "Mcp/McpOutputCeiling.h"
#include "Mcp/McpApprovalState.h"
#include "Mcp/McpFlowMode.h"
#include "Mcp/McpAlphi.h"
#include "Mcp/McpAlphiSay.h"
#include "Pd/MCPBridgeAcLevel.h" // V1 AC taps (JUCE-free)

extern "C" {
#include <m_pd.h>
#include <g_canvas.h>
}

// P4.4: an id whose object an undo or redo took away, kept so the object that comes back (same text, same place)
// gets the id again. Pd context only.
struct McpUndoGhost {
    std::string id;
    std::string text;
    int x = 0, y = 0, index = -1;
};

struct McpSession {

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
        juce::StringArray targetIds; // multi-selection targets
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
        juce::StringArray targetIds;
    };
    std::vector<McpRegion> mcpRegions;
    std::vector<McpRegion> getMcpRegions() const { juce::ScopedLock sl(mcpOverlayLock); return mcpRegions; }

    // PRD Phase 2: pasted reference images (hardware photos, schematics) living on
    // the canvas glass behind the DSP layer. x/y/w/h are PD patch coords so they map
    // 1:1 to OSC /pd/reference/image and construct_patch_v4 placement.
    struct McpReferenceImage {
        juce::String id;       // content hash
        juce::String filePath; // cached PNG under <appData>/references/
        juce::String title;    // display tag (file name)
        juce::String targetId; // surgical lane: stable tempId this image is attached to ("" = loose moodboard)
        float x = 0.0f;        // top-left corner, PD patch coords
        float y = 0.0f;
        float width = 300.0f;
        float height = 200.0f;
        float alpha = 0.88f;
        bool isLocked = false;
        bool visible = true; // Visible-Only Ingestion: hidden images are not rendered nor sent to the AI
    };
    std::vector<McpReferenceImage> mcpReferenceImages;
    std::vector<McpReferenceImage> getMcpReferenceImages() const { juce::ScopedLock sl(mcpOverlayLock); return mcpReferenceImages; }
    void addMcpReferenceImage(McpReferenceImage img)
    {
        juce::ScopedLock sl(mcpOverlayLock);
        for (auto& existing : mcpReferenceImages) {
            if (existing.id == img.id) {
                existing.x = img.x;
                existing.y = img.y;
                return; // re-pasting the same image just re-centers it
            }
        }
        mcpReferenceImages.push_back(std::move(img));
    }
    void moveMcpReferenceImage(const juce::String& id, float x, float y)
    {
        juce::ScopedLock sl(mcpOverlayLock);
        for (auto& img : mcpReferenceImages) {
            if (img.id == id) {
                img.x = x;
                img.y = y;
                return;
            }
        }
    }
    void setMcpReferenceLock(const juce::String& id, bool locked)
    {
        juce::ScopedLock sl(mcpOverlayLock);
        for (auto& img : mcpReferenceImages) {
            if (img.id == id) {
                img.isLocked = locked;
                return;
            }
        }
    }
    void setMcpReferenceVisible(const juce::String& id, bool visible)
    {
        juce::ScopedLock sl(mcpOverlayLock);
        for (auto& img : mcpReferenceImages) {
            if (img.id == id) {
                img.visible = visible;
                return;
            }
        }
    }
    void setMcpReferenceTarget(const juce::String& id, const juce::String& targetId)
    {
        juce::ScopedLock sl(mcpOverlayLock);
        for (auto& img : mcpReferenceImages) {
            if (img.id == id) {
                img.targetId = targetId;
                return;
            }
        }
    }

    // Phase 3 (slice): persisted voice takes — the beatbox/melody recordings become
    // first-class moodboard artifacts (WAV on disk + structured transcript).
    struct McpVoiceTake {
        juce::String id;      // take_<timestamp>
        juce::String wavPath; // ~/Documents/plugdata/recordings/take_….wav
        juce::String json;    // structured analysis (mode, bpm, hits, notes)
        juce::String mode;    // "beatbox" | "melody"
        int bpm = 0;
        int hitCount = 0;
        int noteCount = 0;
        double durationSec = 0.0;
        bool visible = true;
        // On-glass presence (PD patch coords) + mini-waveform envelope.
        float x = 0.0f;
        float y = 0.0f;
        bool positioned = false;
        std::vector<float> peaks; // 64 peak buckets for the chip
    };
    std::vector<McpVoiceTake> mcpVoiceTakes;
    std::vector<McpVoiceTake> getMcpVoiceTakes() const { juce::ScopedLock sl(mcpOverlayLock); return mcpVoiceTakes; }
    void addMcpVoiceTake(McpVoiceTake take)
    {
        juce::ScopedLock sl(mcpOverlayLock);
        mcpVoiceTakes.push_back(std::move(take));
    }
    void removeMcpVoiceTake(const juce::String& id)
    {
        juce::ScopedLock sl(mcpOverlayLock);
        std::erase_if(mcpVoiceTakes, [&id](McpVoiceTake const& t) { return t.id == id; });
    }
    void setMcpVoiceTakeVisible(const juce::String& id, bool visible)
    {
        juce::ScopedLock sl(mcpOverlayLock);
        for (auto& t : mcpVoiceTakes) {
            if (t.id == id) {
                t.visible = visible;
                return;
            }
        }
    }
    void setMcpVoiceTakePos(const juce::String& id, float x, float y)
    {
        juce::ScopedLock sl(mcpOverlayLock);
        for (auto& t : mcpVoiceTakes) {
            if (t.id == id) {
                t.x = x;
                t.y = y;
                t.positioned = true;
                return;
            }
        }
    }

    // MIDI melodic intent — captured from a MIDI keyboard: EXACT notes (no transcription).
    // A MIDI take is pure data, so it carries a `voice` binding (canvas | voice_bank:<name> | agent).
    struct McpMidiTake {
        juce::String id;    // midi_<timestamp>
        juce::String json;  // structured (mode, bpm, notes[{pitch,start,duration,vel}], voice)
        juce::String voice; // "canvas" | "voice_bank:name" | "agent"
        int bpm = 0;
        int noteCount = 0;
        double durationSec = 0.0;
        bool visible = true;
        float x = 0.0f;
        float y = 0.0f;
        bool positioned = false;
    };
    std::vector<McpMidiTake> mcpMidiTakes;
    std::vector<McpMidiTake> getMcpMidiTakes() const { juce::ScopedLock sl(mcpOverlayLock); return mcpMidiTakes; }
    void addMcpMidiTake(McpMidiTake take)
    {
        juce::ScopedLock sl(mcpOverlayLock);
        mcpMidiTakes.push_back(std::move(take));
    }
    void removeMcpMidiTake(const juce::String& id)
    {
        juce::ScopedLock sl(mcpOverlayLock);
        std::erase_if(mcpMidiTakes, [&id](McpMidiTake const& t) { return t.id == id; });
    }
    void setMcpMidiTakeVisible(const juce::String& id, bool visible)
    {
        juce::ScopedLock sl(mcpOverlayLock);
        for (auto& t : mcpMidiTakes) {
            if (t.id == id) {
                t.visible = visible;
                return;
            }
        }
    }
    void setMcpMidiTakePos(const juce::String& id, float x, float y)
    {
        juce::ScopedLock sl(mcpOverlayLock);
        for (auto& t : mcpMidiTakes) {
            if (t.id == id) {
                t.x = x;
                t.y = y;
                t.positioned = true;
                return;
            }
        }
    }
    void removeMcpReferenceImage(const juce::String& id)
    {
        juce::ScopedLock sl(mcpOverlayLock);
        std::erase_if(mcpReferenceImages, [&id](McpReferenceImage const& img) { return img.id == id; });
    }

    // Cinematic Viewport HUD & Subtitle overlay for video capture & tutorials.
    // Rendered in screen space (unscaled/unpanned) — zero cord collisions.
    struct McpHud {
        bool active = false;
        juce::String chapter;   // e.g. "▶ CHAPTER 01: THE ZERO-LATENCY BUILD"
        juce::String tool;      // e.g. "construct_patch_v4"
        juce::String latency;   // e.g. "<1ms" or "4ms"
        juce::String prompt;    // e.g. "Start me off with a warm analog saw"
        juce::String receipt;   // e.g. "Wired 24dB ladder filter + [tanh~]"
        juce::String telemetry; // e.g. "128 BPM · 48kHz"
    };
    McpHud mcpHud;

    // P12c: the canvas approval chips (one per pending record of the server, keyed by its nonce). Set ONLY by
    // /pd/approval_show and /pd/approval_clear; HUD text never shows chips. Own lock (any thread).
    mcp::ApprovalState approvals;
    // Alphi drop 1: what Alphi says (activity + line slots). Written ONLY by /pd/alphi_say (CanvasMcpAlphiSay.cpp);
    // display only, no flow effect. Own lock (any thread); read with mcp::alphiSayNowMs().
    mcp::AlphiSayBoard alphiSay;
    // P12a A3: flow mode, off at every start; on only through the artist's click on Alphi (P12a.1, McpFlowMode.h).
    mcp::FlowMode flowMode;
    McpHud getMcpHud() const { juce::ScopedLock sl(mcpOverlayLock); return mcpHud; }

    // Spatial / Targeted Overlays: restricts visual lenses (connection activity,
    // order, object activity) to specific objects/cords instead of canvas-wide.
    std::unordered_set<t_gobj*> mcpOverlayTargets;
    bool mcpOverlayStrictCables = false;

    void setMcpOverlayTargets(std::unordered_set<t_gobj*> targets, bool strictCables = false)
    {
        juce::ScopedLock sl(mcpOverlayLock);
        mcpOverlayTargets = std::move(targets);
        mcpOverlayStrictCables = strictCables;
    }
    void clearMcpOverlayTargets()
    {
        juce::ScopedLock sl(mcpOverlayLock);
        mcpOverlayTargets.clear();
        mcpOverlayStrictCables = false;
    }
    bool hasMcpOverlayTargets() const
    {
        juce::ScopedLock sl(mcpOverlayLock);
        return !mcpOverlayTargets.empty();
    }
    size_t getMcpOverlayTargetsCount() const
    {
        juce::ScopedLock sl(mcpOverlayLock);
        return mcpOverlayTargets.size();
    }
    bool isMcpTargeted(t_gobj* g) const
    {
        juce::ScopedLock sl(mcpOverlayLock);
        return mcpOverlayTargets.find(g) != mcpOverlayTargets.end();
    }
    bool isMcpConnectionTargeted(t_gobj* out, t_gobj* in) const
    {
        juce::ScopedLock sl(mcpOverlayLock);
        if (mcpOverlayTargets.empty()) return true;
        bool const outTargeted = out && mcpOverlayTargets.find(out) != mcpOverlayTargets.end();
        bool const inTargeted = in && mcpOverlayTargets.find(in) != mcpOverlayTargets.end();
        if (mcpOverlayStrictCables) {
            return outTargeted && inTargeted;
        }
        return outTargeted || inTargeted;
    }

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
        mcpReferenceImages.clear();
        mcpVoiceTakes.clear();
        mcpAiOverlay.clear();
        mcpOverlayTargets.clear();
        mcpOverlayStrictCables = false;
        mcpHud = McpHud();
    }

    // Guards mcpAiOverlay / mcpGhosts / mcpAnnotations / mcpOverlayTargets: written on the message
    // thread, read on the VBlank render thread.
    mutable juce::CriticalSection mcpOverlayLock;


    // PRD overlay: per-object AI state for the canvas AI overlay
    // (state 1 = changed, 2 = proposed, 3 = error) + set-time so a *transient*
    // auto-clear can never erase a later *persistent* mark. Message-thread only.
    std::unordered_map<t_gobj*, AiMark> mcpAiOverlay;
    int mcpSuspendedDspState = 0;
    // P4.4: per canvas, the ids undo/redo took away (newest last, bounded), with the canvas's liveness check.
    // Pd context only.
    struct UndoJournal {
        std::function<bool()> alive;
        std::vector<McpUndoGhost> ghosts;
    };
    std::map<t_canvas*, UndoJournal> undoJournal;

    // MCP zero-dropout WAV recorder — taps outputFifo directly in processBlock.
    // No canvas objects created, no DSP recompile, zero audio dropout.
    CriticalSection mcpRecorderLock;
    // Report F3: the audio thread only pushes samples into the ThreadedWriter's
    // FIFO; file creation and disk writes happen off the audio thread.
    // Declared before the writer so the writer is destroyed first.
    juce::TimeSliceThread mcpRecorderThread { "MCP recorder" };
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> mcpThreadedWriter;
    std::atomic<bool> mcpRecording { false };
    juce::String mcpRecorderPath;

    // Armed peak-hold for trigger verification (/meter/arm → trigger → /meter/read).
    // Accumulated on the audio thread over the FINAL output, so a one-shot fired
    // between arm and read can NEVER be missed (window opens before the event).
    std::atomic<bool> mcpArmActive { false };
    std::atomic<float> mcpArmPeak { 0.0f };
    // P9: the same window seen BEFORE the output ceiling (finite samples), and the ceiling's clamp count at arm, so
    // /meter/read can say "hot, clipped at the ceiling" instead of "audible".
    std::atomic<float> mcpArmPreCeilingPeak { 0.0f };
    std::atomic<uint64_t> mcpArmClampedAtArm { 0 };
    std::atomic<uint64_t> mcpArmNonFiniteAtArm { 0 }; // round 1: the window's NaN/inf count
    std::atomic<uint64_t> mcpArmTripsAtArm { 0 };     // round 1: breaker trips in the window
    // P11 V2 (J3b): the same window at two more taps. PRE-LIMITER = processBlock after PlugData's master gain and
    // before its limiter (McpHost::midiTick, the existing hook there): peak and mean square. FINAL: the mean square
    // of what leaves the app (the peak is mcpArmPeak). The limiter's largest gain reduction in the window (dB, per
    // block: pre-limiter peak over the peak after the limiter). PD OUT: Pd's own output before the master gain
    // (McpHost::pdBlock). Single writer (the audio thread); read after the window closed.
    std::atomic<float> mcpArmPreLimiterPeak { 0.0f };
    std::atomic<double> mcpArmPreLimiterSumSq { 0.0 };
    std::atomic<uint64_t> mcpArmPreLimiterCount { 0 };
    std::atomic<double> mcpArmFinalSumSq { 0.0 };
    std::atomic<uint64_t> mcpArmFinalCount { 0 };
    std::atomic<float> mcpArmLimiterReductionDbMax { 0.0f };
    std::atomic<float> mcpArmPdOutPeak { 0.0f };
    float mcpBlockPreLimiterPeak = -1.0f; // audio thread only: this block's pre-limiter peak (-1 = not taken)
    // V1 AC taps (2026-11-06: a DC drone read "-22 dBFS, audible"): the FINAL tap through a DC blocker per output
    // channel. The blockers run on every block (read only, a few flops per sample), so a window opens on a settled
    // blocker; the sums below count only while armed. Channels past kAcChannels are not in the AC sums. DC = the mean
    // of each channel over the window (RMS over the channels). Single writer (the audio thread); read after the window
    // closed, emptied with the others at a new arm epoch.
    static constexpr int kAcChannels = 16;
    std::array<mcp::ac::DcBlocker, kAcChannels> mcpAcBlockers {}; // audio thread only
    double mcpAcRate = 0.0;                                       // audio thread only: the rate the poles are for
    std::atomic<double> mcpArmFinalAcSumSq { 0.0 };
    std::atomic<float> mcpArmFinalAcPeak { 0.0f };
    std::atomic<uint64_t> mcpArmFinalAcCount { 0 };
    std::array<std::atomic<double>, kAcChannels> mcpArmFinalChSum {};
    std::array<std::atomic<uint64_t>, kAcChannels> mcpArmFinalChCount {};
    void resetArmAc() // the window's AC sums (arm and the audio thread's epoch reset)
    {
        mcpArmFinalAcSumSq.store(0.0, std::memory_order_relaxed);
        mcpArmFinalAcPeak.store(0.0f, std::memory_order_relaxed);
        mcpArmFinalAcCount.store(0, std::memory_order_relaxed);
        for (auto& v : mcpArmFinalChSum) v.store(0.0, std::memory_order_relaxed);
        for (auto& v : mcpArmFinalChCount) v.store(0, std::memory_order_relaxed);
    }
    mcp::ac::AcLevels readArmAc() const // the window's AC levels (after it closed)
    {
        uint64_t const n = mcpArmFinalAcCount.load(std::memory_order_relaxed);
        if (n == 0) return {};
        double dcSq = 0.0;
        int channels = 0;
        for (int c = 0; c < kAcChannels; ++c)
            if (uint64_t const cn = mcpArmFinalChCount[static_cast<size_t>(c)].load(std::memory_order_relaxed); cn > 0) {
                double const mean = mcpArmFinalChSum[static_cast<size_t>(c)].load(std::memory_order_relaxed) / static_cast<double>(cn);
                dcSq += mean * mean;
                ++channels;
            }
        return mcp::ac::levelsFrom(std::sqrt(std::max(0.0, mcpArmFinalAcSumSq.load(std::memory_order_relaxed)) / static_cast<double>(n)),
            mcpArmFinalAcPeak.load(std::memory_order_relaxed), channels > 0 ? std::sqrt(dcSq / channels) : 0.0);
    }
    // Review L3: every arm bumps the epoch; the audio thread empties the window's accumulators itself at the first
    // block of a new epoch, so a sum it stored for the old window after the arm's reset cannot leak into the new one.
    std::atomic<uint32_t> mcpArmEpoch { 0 };
    uint32_t mcpArmEpochSeen = 0; // audio thread only
    // Review L4: the audio thread must never take a lock for these.
    static_assert(std::atomic<double>::is_always_lock_free, "atomic<double> must be lock-free on the audio thread");
    static_assert(std::atomic<uint64_t>::is_always_lock_free, "atomic<uint64_t> must be lock-free on the audio thread");

    // P9 Part B: the output ceiling and its breaker on the final output (McpHost::audioBlockFinished). Its switch
    // mirrors the owner setting `mcp_bridge_ai_ceiling` (read at bridge creation and in the MCP settings path; never
    // SettingsFile on the audio thread). Off only from the settings panel or at startup.
    mcp::OutputCeiling outputCeiling;

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

    // --- Monitor tap: solo / mute (NON-DESTRUCTIVE) --------------------------
    // The audio thread reads the target node's outlet buffer (resolved once on
    // the message thread via the stable object map) and either keeps ONLY it
    // (solo) or removes it (mute) in the FINAL output. The patch is never
    // touched, state is RAM-only, and it EXPIRES (default 30 s) — a forgotten
    // tap can never leave the artist's mix dead. mcpTapVec is validated by a
    // message-thread timer; a dead/null vec turns the tap off in the callback.
    static constexpr int mcpTapOff = 0, mcpTapSolo = 1, mcpTapMute = 2;
    std::atomic<int> mcpTapMode { 0 };
    // Opaque t_signal* (resolved/published by the bridge). Pd swaps each
    // outlet's s_vec per DSP block, so the audio thread must re-read
    // sig->s_vec every block — a captured s_vec pointer goes stale.
    std::atomic<void*> mcpTapSig { nullptr };
    // P3.4 S5: ugen_getsortno() when mcpTapSig was resolved (under the Pd lock); the audio thread applies the
    // tap only while the two agree. Pd frees signals only on a DSP rebuild, which bumps that number.
    std::atomic<int> mcpTapEpoch { 0 };
    std::atomic<float> mcpTapGain { 1.0f }; // unity — the master chain applies the level
    std::atomic<juce::uint32> mcpTapUntilMs { 0 };
    std::atomic<juce::uint32> mcpTapGeneration { 0 }; // bumped on every set/clear

    // --- Armed-capture DIAGNOSTICS (#83) -------------------------------------
    // Evidence for the "armed reads silent while live reads audible" failure.
    // On arm we snapshot mcpAudioBlocks and zero the two window counters; the
    // audio thread bumps them at the processBlock tail. On read the elapsed
    // counts split the three candidate causes:
    //   blocksElapsed == 0                    → the audio thread isn't ticking
    //   blocksElapsed > 0, armedTailRuns == 0 → the armed tail isn't reached
    //   armedTailRuns > 0, tailPeak ~ 0       → the JUCE output buffer is silent
    std::atomic<uint64_t> mcpAudioBlocks { 0 };      // ++ every processBlock
    // #83 (P3.4 S10): getMillisecondCounter() at the last processBlock. Pd's signal vectors keep the last
    // block when the callback stops, so the Pd-side meters check this before trusting them.
    std::atomic<juce::uint32> mcpLastBlockMs { 0 };
    // P3.4 S11 test-only (/pd/debug_stall_audio): the next audio block holds the Pd lock and sleeps this long.
    std::atomic<int> mcpDebugStallMs { 0 };
    std::atomic<uint64_t> mcpArmBlocksAtArm { 0 };   // snapshot taken on arm
    std::atomic<uint64_t> mcpArmedTailRuns { 0 };    // armed-tail executions this window
    std::atomic<float>    mcpArmedTailPeak { 0.0f }; // peak seen at the tail this window

    // --- The MCP Pd queue (Plan 3 P3.4 S2) ------------------------------------
    // Every Pd job of the bridge (batch_atomic's mutation, the legacy verbs, layout moves, ...) is queued
    // here and run by McpHost::drainPdQueue() on the message thread UNDER sys_lock (S3: plugdata's own Pd
    // editing context). Upstream's functionQueue stays upstream's.
    moodycamel::ConcurrentQueue<std::function<void()>> mcpPdQueue { 256 };
    std::atomic<bool> mcpPdDrainPosted { false }; // a drainPdQueue callAsync is pending
    std::atomic<bool> mcpShuttingDown { false };  // P3.4 S8: McpHost::shutdown ran; no more Pd jobs

    // P3.4 S3: the focused editor's true root canvas, written only by the message thread
    // (McpHost::refreshFocusedRoot); readers validate it against pd_canvaslist.
    std::atomic<t_canvas*> mcpFocusedRoot { nullptr };
};
