# PRD — Native Sample-Accurate Sequencer, Transport, and Curve Engine

**Status:** Spec (not yet implemented)
**Owner:** MCP bridge (C++) + `trigger_musical_events` (TS)
**Branch:** `feat/v4-zero-dropout-copilot`

---

## 1. Problem

Every action in `trigger_musical_events` — `play_drum_pattern`, `play_musical_event`,
`orchestrate_automation`, `orchestrate_sequence`, and `transport` — is currently
**timed by TypeScript** (`Date.now()` + `setTimeout`), then dispatches each event as a
separate OSC round-trip to the C++ bridge.

Consequences:

1. **Timing jitter** — Node timers fire at event-loop granularity (~1–5ms, worse under
   GC/load). Percussion hits do not land on the sample grid.
2. **Per-event network cost** — a dense pattern shuttles dozens of `/trigger` + `/param`
   messages per second over UDP. A per-step `{v,p,d}` object fires up to 3 messages.
3. **No audio-thread sync** — the sequencer clock is wall-clock; the C++ bridge already
   owns a real sample clock (`processBlock`) that is never used for scheduling.

The **dispatch** (`/trigger`, `/param`) is already C++. The **scheduler** is the gap.

This is the same class of fix already applied to `/array/mutate`, `/array/load`, and the
recorder: move the hot loop out of TS and into one C++ audio-thread job, so the music
never flinches.

---

## 2. Goals

1. Sample-accurate scheduling of drum hits, notes, automation steps, and LFO curves.
2. One-message "upload the pattern" — zero per-event OSC round-trips.
3. Preserve every existing creative feature: swing, nudge, probability, humanize, flam,
   per-step params, mutations, `shiftEachLoop`, `bpmCurve`, per-track polyrhythms.
4. Keep the two timing modes the artist already knows:
   - **locked** (`syncToClock`) — shared sample-clock grid, dead-tight downbeats.
   - **free / drift** — each job its own sample-clock timeline, clean phasing.
5. Atomic **hot-swap** (`update_*`) — change a running pattern at the next bar, no stop.

---

## 3. Architecture

Three native endpoints replace the TS scheduler:

| Endpoint | Replaces (TS) | Responsibility |
|---|---|---|
| `/transport/*` | `TransportClock` (Date.now) | Master sample-clock: bpm, subdivision, mode, position |
| `/seq/*` | `play_drum_pattern` + `update_drum_pattern` | Drum/note step schedule |
| `/curve/*` | `orchestrate_automation` + `orchestrate_sequence` | Parameter automation + LFO |

Each job runs on the audio thread, advances by **sample count** (not wall clock), and
fires events **in-process** by resolving the receiver symbol (`gensym` → `pd_typedmess`)
— no OSC round-trip per event.

---

## 4. Timing Model (the important part)

### 4.1 Locked mode
All `syncToClock` jobs read the **shared transport sample counter**. Their steps align
to the same bar grid at sample resolution. Separate jobs land on the *same* downbeat.

### 4.2 Free mode
A free job owns a private tempo accumulator (still sample-count based). It does not read
the shared transport. It is **internally sample-tight** but does not share a downbeat with
other jobs.

### 4.3 Drift (intentional de-sync)
`drift` is a % offset applied to a job's *own* accumulator rate:
`effectiveRate = baseRate * (1 + drift/100)`.
Because the rate is applied to a sample-count accumulator (not `Date.now()`), drift is a
**clean phase**, not jitter. Two drifting loops slide past each other like two perfectly
tuned metronomes at slightly different speeds, then re-align.

> Free + drift is a *creative* feature, not sloppiness. The native version removes the
> random jitter that currently buries the intended phase.

### 4.4 Humanize is deliberate
`humanizeVelocity` / `humanizeTiming` are computed on the audio thread with a **seeded
PRNG** (deterministic per job seed). Timing humanize nudges a hit within its step slot
(never overlapping the next event). The result is a *played* feel, not random slop.

---

## 5. Message Formats

Schedules are nested, so they ship as a **single JSON string argument** (parsed C++-side
with `juce::JSON`). This keeps feature parity with the existing TS payloads while collapsing
N messages into 1.

### 5.1 Transport

```
/transport/set_bpm <bpm>
/transport/set_subdivision <n>
/transport/set_mode <"clock"|"free">
/transport/start
/transport/pause
/transport/status <corrId>            → reply: /transport/status/<corrId> <bpm> <bar> <step> <nextBarSampleOffset>
```

Position is derived from the running sample counter → sample-accurate bar/step/downbeat.

### 5.2 Sequencer

```
/seq/start   <jobId> <corrId> <json>
/seq/update  <jobId> <corrId> <json>   # atomic hot-swap at next bar
/seq/stop    <jobId>
```

`json` schedule (field-for-field parity with `play_drum_pattern`):

```json
{
  "bpm": 128, "subdivision": 4,
  "syncToClock": true, "drift": 0,
  "swing": 0.08,
  "humanizeVelocity": 0, "humanizeTiming": 0,
  "loopMutations": false,
  "tracks": { "kick": [1,0,0,0,1,0,0,0], "hh": [1,0,1,0] },
  "probability": { "hh": 0.5 },
  "nudge": { "snare": { "4": 3, "12": -2 } },
  "mutations": [ { "atBar": 4, "swing": 0.4, "tracks": { "kick": [1,0,1,0] } } ],
  "bpmCurve": [ { "atBar": 0, "bpm": 120 }, { "atBar": 4, "bpm": 60 } ],
  "automations": [ { "target": "cutoff", "curve": [ {"atBar":0,"val":200}, {"atBar":4,"val":2000} ] } ],
  "shiftEachLoop": { "hh": 1 }
}
```

Firing is identical to the current `dispatchDrum` semantics:
- `1` / `true` → `/trigger <track> [vel]`
- number → velocity (0.1–1.0) or param track
- `"f8"` → flam (ghost + main 8ms apart, scheduled natively — no TS `setTimeout`)
- `{v,p,d,n,r}` → per-step object: trigger/param + `_decay` + `_pitch`

### 5.3 Curve / LFO engine

```
/curve/start  <jobId> <corrId> <json>
/curve/update <jobId> <corrId> <json>
/curve/stop   <jobId>
```

```json
{
  "target": "cutoff",
  "mode": "once" | "loop",
  "syncToClock": true, "drift": 0,
  "points": [ { "at": 0, "val": 200 }, { "at": 4, "val": 2000 } ],
  "shape": "linear" | "exponential" | "sine" | "saw" | "square",
  "rate": 0.5,          // LFO: cycles/sec (or /bar when synced)
  "depth": 500, "center": 1200, "phase": 0,
  "ramp": 20            // per-segment smoothing ms (anti-zipper)
}
```

Interpolation runs on the audio thread → butter-smooth sweeps, no stepping. Values are
written directly to the receiver (in-process), never re-routed through UDP.

---

## 6. Feature Parity Checklist (must not regress)

- [ ] Per-track polyrhythm (tracks of different lengths loop independently, shared phase)
- [ ] Swing (odd-step offset)
- [ ] Probability (per-track hit chance)
- [ ] Nudge (per-track per-step ±ms)
- [ ] Flam (`f<n>` ghost + main)
- [ ] Per-step param objects `{v,p,d,n,r}`
- [ ] `velocityMode`: `bang` vs `param`
- [ ] Mutations at bar boundaries (merge semantics)
- [ ] `loopMutations` (cycle via `loopCount % cycleLength`)
- [ ] `bpmCurve` (linear interpolation between bar points)
- [ ] `automations` synced to the drum clock
- [ ] `shiftEachLoop` (per-track step rotation)
- [ ] Humanize velocity + timing (deliberate, seeded)
- [ ] `syncToClock` / `free` / `drift` (sample-accurate)

---

## 7. Phased Implementation

1. **`/transport`** — native sample-clock transport (bpm/subdivision/mode/start/pause/status).
2. **`/seq`** — drum pattern schedule + hot-swap. Wire `dispatchDrum` semantics into the
   audio-thread fire routine. Keep TS as a thin client that only uploads JSON.
3. **`/curve`** — automation + LFO schedule, reuse the existing `morph` job for smoothing.
4. **TS client** — `osc-client.ts` gains `seqStart/seqUpdate/seqStop`, `curveStart/...`,
   `transport*`; `performance.ts` delegates to them when the `seq`/`curve` capabilities are
   advertised (fall back to the TS scheduler on older bridges).

Each phase ships independently and is drop-tested against the running beat (zero dropout,
> -20 dBFS, zero console errors) before the next.

---

## 8. Capabilities

The bridge adds to `/bridge/capabilities/reply`:
```
transport
seq
curve
```

TS gates the native path on `hasCap("seq")` / `hasCap("curve")` / `hasCap("transport")`
and keeps the current TS scheduler as the legacy fallback.
