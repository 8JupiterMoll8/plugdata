# PRD: C++ Diagnostic Layer — Structured Facts from the Engine

**Document Version:** 1.0.0 (DRAFT)
**Target Repos:** `plugdata-core` (C++ bridge) & `PlugData-MCP-Server` (TS)
**Depends on:** Phases 1–3.1 (all shipped & live-verified 2026-09-02)
**Author:** Jupiter Moll & Antigravity
**Status:** PROPOSED

---

## 1. Motivation — Five Failures, One Pattern (2026-09-02 session evidence)

The full-session gauntlet + master-chain surgery exposed one structural truth:
**the engine knows why things break; TS only sees console prose, late and as text.**

| # | Failure tonight | What TS had to do | What C++ already knew |
|---|---|---|---|
| 1 | Connect silently no-op'd (`connected: 0`, master severed for seconds) | connect-count guard guessed WHICH wire failed | the batch knows the exact tempId that failed to resolve, and why |
| 2 | `compressor~` couldn't create | 80ms sleep + scrape console text (async print race — first attempt missed it) | the batch knows the object failed *at creation time, inside the lock* |
| 3 | DSP loop muted the graph | console prose ("DSP loop detected") + manual census + wire-tracing for several rounds | the signal compiler refuses looped objects — a graph cycle-detect over `sig` connections names the cycle directly |
| 4 | Bare `*~` with unconnected inlet 1 = master silence | manual probe chain (catch_bus hot → dc silent → break is the VCA) — NO error printed at all | a graph walk sees: signal `*~` with an unconnected audio inlet |
| 5 | Dangling/temp-ids churn made rewire blind | census → re-derive names → retry | the map IS the truth; failures can be named |

Every TS-side workaround built tonight (getErrorsSince scrape, 80ms settle,
connect-count guard) is a band-aid around this: **diagnosis by echo, racing
async print delivery.** The C++ diagnostic layer replaces echoes with facts.

## 1.5 BASELINE — What v4's TS layer already shipped (2026-09-02 night)

Before building this layer, know the interim state (all live-verified):

**Shipped in v4's ride-along (TS-only band-aids):**
- self-connection warning · comma-in-msg warning · topology-rewire warning
- **silent-connect guard**: requested wires vs `connected` count — names every
  failed wire (v2's missingWires/unknownTempIds parity, count + first-5 names)
- console tail — **reads `ctx.getErrorsSince()`** (the REAL Pd console pipe)
- escalation: on console symptoms → silent-killer registry (27 rules) run on
  the live graph → cause analysis rides the same response

**Discoveries that shaped these band-aids (the C++ layer removes the causes):**
1. `LogManager` is an EMPTY SHELL — nothing ever calls `addLog`; Pd console
   prints never reach it. The real pipe is the errorManager (`getErrorsSince`).
2. Pd prints are delivered **asynchronously** — "couldn't create" posts during
   the batch but lands in the log ~80ms later. Hence the settle-wait race.
3. `resolveStableId` **silently no-ops** stale tempId connects (no error,
   no count) — the root of the silent master severance.
4. The bare-`*~` class **prints nothing at all** — only graph analysis
   (diagnose) or ears/probe can find it. THE remaining blind spot.

**Coverage tonight:** every failure class that PRINTS or breaks a wire is now
caught in the build response. The blind spot: silent structural faults (no
print, wires intact) — exactly what this PRD's `/pd/diagnose` solves.
**Dropout status:** all TS additions are read-only post-flight — builds may be
~10–90ms slower on creates; audio never gaps from verification. The only drop
surface remains signal-topology rewires (flagged `⚠ topology rewire`).

## 2. Design

### 2.1 Batch Reply Enrichment (kills failures #1 and #2)

`batch_atomic` reply gains, alongside created/connected/edited:

```
create_failures:  [tempId, type, reason, ...]   // objects that failed to create
connect_failures: [srcId, destId, reason, ...]  // connects whose tempId didn't resolve
```

- PHASE 4 (create): `pasteDirect` failures per object → `create_failures`
  (reason: "couldn't create"/"invalid class").
- PHASE 5 (connect): on `resolveStableId` returning null → `connect_failures`
  (reason: "tempId not found" / "stale identity").
- TS `_v2meta` maps these to `unknownTempIds` / `failedWires` — **the silent
  connect becomes a named failure in the same response**, zero scraping.

### 2.2 `/pd/diagnose <canvas> [corrId]` — the Graph X-Ray (kills failures #3 and #4)

One call, under `sys_lock`, walking `gl_list` + the signal connection graph:

```
{
  "dsp_cycles":    [ [tempId, tempId, ...], ... ],   // signal-graph cycles (DFS over sig connections)
  "unscheduled":   [tempId, ...],                    // signal objects excluded from the DSP pass
  "dangling_sig_inlets": [ {tempId, inlet}, ... ],   // signal inlets with no wire
  "zeroed_vcgs":   [ {tempId, inlet}, ... ],         // signal *~ with inlet-1 constant 0 / unconnected
  "mismatched":    [ {srcId, srcOut, destId, destIn}, ... ]  // sig→control / control→sig wires
}
```

- **Cycle detection** = DFS over the signal graph (outlet → connected inlets,
  signal-rate edges only). Names the exact object cycle — no more wire-tracing.
- **Unscheduled objects** = signal objects present in `gl_list` but absent from
  the DSP schedule after `canvas_update_dsp` (Pd refuses loop members; the set
  difference names them).
- **Dangling + zeroed** = the bare-`*~` law and its siblings, checked natively.
- TS: `analyze_patch(diagnostics)` action or auto-escalation target — the
  silent-killer TS registry then only formats/extends these facts.

### 2.3 Layer Split (facts native, judgment TS)

- **C++**: raw structured facts (loops, cycles, dangling, failures, levels).
- **TS silent-killer registry**: stays as the reporting/extension layer —
  rules that need `pdDocs` knowledge or artist-facing wording read the C++
  facts; rules whose fact is now native are demoted to thin formatters.
- **Ride-along escalation** (built 2026-09-02): on console symptoms → call
  `/pd/diagnose` instead of the TS registry run.

### 2.4 Console Pipe (secondary)

Pd console prints stay for human readability only — never again a diagnostic
data source. (The getErrorsSince scrape remains as legacy fallback for old
bridges.)

## 3. In Scope / Out of Scope

**In:** batch reply enrichment (create/connect failures), `/pd/diagnose` with
cycle detection + dangling/zeroed/mismatch facts, TS `_v2meta` mapping, ride-
along escalation switch to `/pd/diagnose`, caps entry, tests.
**Out:** DSP-schedule hooking internals beyond set-difference capture, audio
analysis (probe/meter already native), TS rule retirement (follow-up).

## 4. Acceptance Criteria

1. Build with an uncreatable object → reply `create_failures` names it; no
   console scraping anywhere in the v4 path.
2. Connect with a stale tempId → reply `connect_failures` names src/dest;
   `_v2meta` warns with the exact wire.
3. A patch with a signal self-loop → `/pd/diagnose` returns the cycle objects.
4. A bare `*~` with unconnected inlet 1 → `/pd/diagnose` flags `zeroed_vcgs`.
5. The artist's workflow: silence → one `diagnose` call → cause named. No
   multi-round manual hunting.
6. Old bridges: TS falls back to getErrorsSince scrape (current band-aid).
7. Zero audio-drop regression: diagnose is read-only under sys_lock.

## 5. Task Breakdown (est. ~1.5 developer-days)

| # | Task | File(s) | Effort |
|---|---|---|---|
| 1 | Batch reply: create_failures + connect_failures | `MCPBridge.cpp` batch arm | M |
| 2 | `/pd/diagnose`: sig-graph DFS cycle detect + dangling/zeroed/mismatch walk | `MCPBridge.cpp` | M |
| 3 | Caps: `diagnose` | `MCPBridge.cpp` | S |
| 4 | TS: `_v2meta` mapping + ride-along escalation switch to `/pd/diagnose` | `transaction-v4.ts` | S |
| 5 | TS: `analyze_patch(diagnostics)` action rendering the facts | `patching.ts` | S |
| 6 | Tests: failure naming, cycle naming, no-drop regression, legacy fallback | `__tests__` + live | M |

## 6. Verification Protocol

Deliberately create each failure class (bad object, stale connect, self-loop,
bare `*~`) → each must be **named in the response/diagnose output** without any
console scraping, while the beat keeps playing. Then: gap → one `diagnose` →
cause named → fix → confirm. The session-long hunt becomes one call.
