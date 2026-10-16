# 🎛️ Product Requirements Document (PRD)

## Feature Title: Zero-Dropout Live DSP Hot-Patching & Dynamic Voice Allocation
- **Status**: Ready for Implementation (Specification Phase Complete)
- **Target Repositories**: 
  - `plugdata-team/plugdata` (`plugdata-core`)
  - `8JupiterMoll8/PlugData-MCP-Server` (`mcp-server`)
- **Target Audience**: Media Artists, Live Performers, Vibe Coders, Interactive Installation Designers
- **Author**: Jupiter Moll & Antigravity Pair Programming Team
- **Date**: August 24, 2026

---

## 1. Executive Summary & Creative Vision

### The Vision
When an artist or AI partner is live-patching during a performance, club set, or interactive gallery installation, **the music must never stop**. 

Currently, adding new synthesis voices, audio objects (`~`), or signal wires in Pure Data forces a synchronous audio thread freeze (`canvas_update_dsp`), creating an audible 10–30ms stutter. 

**Zero-Dropout Live DSP Hot-Patching** enables the AI and artist to dynamically construct entire synthesizer voices, insert audio effects, and connect signal paths in real time with **zero audio clicks, zero dropped frames, and zero tempo interruption**.

---

## 2. Problem Statement

### Current State
1. **The DSP Recompilation Bottleneck**: In standard Pure Data, calling `canvas_update_dsp()` reallocates and flattens the DSP graph on the main thread. While this happens, the audio callback is blocked.
2. **Performance Killer**: A running 130 BPM breakbeat or dense techno sequence experiences an audible gap whenever a new voice (e.g., glitch generator, bassline, reverb) is wired to `[dac~]`.
3. **Breaks Artist Flow**: The artist cannot freely experiment or vibe-code during a live take because every structural patch edit interrupts the groove.

---

## 3. Goals & Non-Goals

### Goals
- **Zero Audio Dropout**: Guarantee `0` dropped audio buffers when creating, wiring, editing, or deleting signal objects during playback.
- **Sub-Millisecond Voice Activation**: Newly added synthesis chains crossfade or phase-in at the next audio block boundary ($\le 1.4\text{ ms}$ at 44.1 kHz / 64 samples).
- **Atomic Double-Buffering**: Assemble and validate the updated DSP execution graph in background memory before atomically swapping the active execution pointer in `processBlock()`.
- **Seamless MCP Tool Integration**: `construct_patch_v2` automatically benefits from non-blocking DSP without requiring special user flags.

### Non-Goals
- Modifying offline file saving (`.pd` format remains 100% vanilla compatible).
- Eliminating hardware CPU limits (if a user creates 500 oscillators exceeding 100% CPU, normal audio buffer overruns will still occur).

---

## 4. User Journey & Core Use Cases

### Use Case A: Live "Vibe Coding" Jam
1. Artist starts a 124 BPM drum groove using `trigger_musical_events`.
2. Artist says: *"Now give me a dirty acid bassline."*
3. MCP builds a 15-object Reese bass synth (`[osc~]`, `[vcf~]`, `[tanh~]`, `[vline~]`) and wires it to `[dac~]`.
4. **Expected Result**: The bassline drops into the mix seamlessly on the next bar boundary. The kick, snare, and hi-hat do not stutter for even a single millisecond.

### Use Case B: Interactive Audio/Game Installations
1. PlugData runs inside a museum / gallery interactive installation.
2. Visitors trigger sensors or AI interactions that generate new generative audio nodes on the fly.
3. **Expected Result**: Continuous, smooth ambient soundscape without audio engine dropouts.

---

## 5. Technical Architecture & Implementation Strategy

```
                                  C++ CORE ARCHITECTURE
                                  
    ┌─────────────────────────────┐         ┌──────────────────────────────┐
    │   Audio Thread (JUCE)       │         │   Worker / MCP Thread (C++)  │
    │                             │         │                              │
    │  processBlock() (Realtime)  │         │  mcp_create_batch_id / wires │
    │  ─────────────────────────  │         │  ──────────────────────────  │
    │  Runs Active DSP Tree [A]   │         │  1. Builds objects in memory │
    │  (Playing Kick + Snare)     │         │  2. Resolves stable IDs      │
    │                             │         │  3. Compiles Shadow Tree [B] │
    │  [Pointer Swap Boundary]    │ ◄────── │  4. Signals Ready-to-Swap    │
    │  At sample block index 0    │         └──────────────────────────────┘
    │  Swaps: [A] ──► [B]         │
    │  Zero dropped samples!      │
    └─────────────────────────────┘
```

### Architecture Pillars:

#### Pillar 1: Pre-Allocated Dynamic Voice Matrix (Phase 1 — MCP Protocol Layer)
- Pre-allocate a low-overhead, 8-to-16 stereo channel internal summing bus in memory (`mcp_bus_1` ... `mcp_bus_16`).
- Routing uses `[throw~]` and `[catch~]` or internal direct gain stages.
- When new voices are spawned, they attach to an existing dynamic bus without tearing down global `[dac~]` wires.

#### Pillar 2: Double-Buffered DSP Graph Swapping (Phase 2 — Core Engine in `plugdata-core`)
- In `Source/Pd/` and `PluginProcessor.cpp`:
  1. Maintain two pointer lists: `t_int* active_dsp_list` and `t_int* staging_dsp_list`.
  2. Canvas mutations compile the `staging_dsp_list` on the background thread under mutex protection.
  3. Inside `PluginProcessor::processBlock()`, check an atomic flag `std::atomic<bool> dsp_swap_pending`.
  4. If true, swap `active_dsp_list = staging_dsp_list` at sample 0 of the audio buffer with an equal-power 32-sample micro-fade to prevent DC step clicks.

---

## 6. Success Metrics & Quality Gate

| Metric | Target | Measurement Method |
| :--- | :--- | :--- |
| **Audio Dropout on Mutation** | **0.0 ms (0 samples lost)** | Loopback recording + sample difference verification |
| **DSP Swap Latency** | **$< 2.0\text{ ms}$** | High-resolution timestamp from MCP call to first audio sample |
| **Glitch / Click Artifacts** | **$0\text{ dB}$ DC step ($<-90\text{ dBFS}$)** | FFT transient detector on voice insertion boundary |
| **Crash / Thread Safety** | **10,000 continuous mutations** | Stress gauntlet creating/deleting 50 voices/min |

---

## 7. Execution Roadmap (When Resuming)

1. **Step 1**: Implement Phase 1 in `mcp-server` (Dynamic Bus Pre-allocation for instant live-jamming benefit).
2. **Step 2**: Implement Phase 2 in `plugdata-core` (`PluginProcessor.cpp` lock-free DSP graph double-buffering).
3. **Step 3**: Run the live audio gauntlet to certify 0-dropout live patching.
