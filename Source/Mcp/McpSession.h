/*
 // MCP session state (Plan 3 P3.2 S4): everything the bridge keeps between commands —
 // overlay model, identity maps, transactions, recorder/meter/tap state. It used to live
 // in PluginProcessor.h; moved verbatim. Owned by McpHost (Source/Mcp/McpHost.h).
 */
#pragma once

#include <atomic>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_osc/juce_osc.h>

#include "Utility/Config.h"

extern "C" {
#include <m_pd.h>
#include <g_canvas.h>
}

struct McpTransaction {
    juce::String canvasName;
    juce::OSCMessage forward;
    juce::OSCMessage inverse;
    // Which /pd action the forward/inverse messages are replayed through.
    // Defaults to batch_atomic (create/connect/edit/delete); moves use move_batch_id.
    juce::String replayAction = "batch_atomic";
};

struct McpSession {
    // Atomic: written on the message thread (undo/redo), read on the OSC thread
    // (batch_atomic dedup / push guards). Plain bool was a cross-thread race.
    std::atomic<bool> isExecutingMcpUndoRedo { false };

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
    std::map<t_canvas*, std::vector<McpTransaction>> mcpUndoStack;
    std::map<t_canvas*, std::vector<McpTransaction>> mcpRedoStack;
    // Undo/redo stacks are pushed on the OSC thread and read/popped on the
    // message thread — mcpUndoLock guards all access. Held only for map ops,
    // never across the nested batch_atomic execution (avoids re-entry deadlock).
    mutable CriticalSection mcpUndoLock;

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
    std::atomic<uint64_t> mcpArmBlocksAtArm { 0 };   // snapshot taken on arm
    std::atomic<uint64_t> mcpArmedTailRuns { 0 };    // armed-tail executions this window
    std::atomic<float>    mcpArmedTailPeak { 0.0f }; // peak seen at the tail this window
};
