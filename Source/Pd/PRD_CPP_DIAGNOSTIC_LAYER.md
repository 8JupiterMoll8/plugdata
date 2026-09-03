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

**Shipped escalation (current state):** on console symptoms → the TS
silent-killer registry (27 rules) runs on the live graph → cause analysis
rides the same response. When the C++ `/pd/diagnose` lands, the escalation
switches to it (native facts, no TS graph analysis).

**Option — always-on diagnose (recommended addition):** for builds that CREATE
signal objects, run `/pd/diagnose` unconditionally (not just on symptoms) —
this closes the no-print silent class (bare `*~` = 0 printed NOTHING tonight;
only graph analysis catches it). Cost: one analysis round-trip per
creating-build (~10–50ms, read-only, zero drops). Recommended default ON.

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

---

## 7. IMPLEMENTATION PLAN — step-by-step (build tomorrow)

All touchpoints verified against the current code (2026-09-02 state).

### Phase A — batch facts (C++, ~2h) — kills failures #1 + #2

**A1. Track create failures** — `MCPBridge.cpp`, PHASE 4 CREATE block
(~line 1073–1105). Today: `newCount` objects map to `pendingCreates[i]`; a
failed create = the mismatch. Capture it:

```cpp
// after the newCount loop:
for (int i = static_cast<int>(newCount); i < static_cast<int>(pendingCreates.size()); ++i) {
    createFailures.push_back({ pendingCreates[i].tempId,
                               pendingCreates[i].type,
                               "couldn't create" });
}
```

**A2. Track connect failures** — PHASE 5 CONNECT block (~line 1107–1119).
Today: `resolveStableId` null → silent skip. Capture:

```cpp
for (auto& cc : allConns) {
    t_gobj* sg = processor->resolveStableId(canvasName, cc.srcId);
    t_gobj* dg = processor->resolveStableId(canvasName, cc.destId);
    if (!sg || !dg) {
        connectFailures.push_back({ cc.srcId, cc.destId,
            sg ? "" : "src not found", dg ? "" : "dest not found" });
        continue;   // loud failure instead of silent skip
    }
    ...
}
```

**A3. Reply enrichment** — reply building (~line 1161). OSC atoms are flat, so
append failures as paired strings:

```
reply.addArgument(int32 createFailCount);
for each failure: reply.addArgument(tempId); reply.addArgument(reason);
reply.addArgument(int32 connectFailCount);
for each failure: reply.addArgument(srcId + "->" + destId); reply.addArgument(reason);
```

**A4. TS parse** — `osc-client.ts batchAtomic()`: parse the new atoms,
return `{ ..., createFailures, connectFailures }`; `transaction-v4.ts`
maps them into `_v2meta.failedCreates` / `_v2meta.failedWires` (replacing the
count-based guard's guess with exact names).

**A5. Test gate:** build `compressor~` (known fail) → reply names it. Connect
to a stale tempId → reply names it. Beat keeps playing; zero drops.

### Phase B — /pd/diagnose (C++, ~3h) — kills failures #3 + #4

**B1. Signal-graph model.** Walk `linetraverser` once (same as census
connections): for each wire, if `lt.tr_ob`'s outlet is signal-rate
(`outlet->o_sym == gensym("signal")` — the check `resolveProbeTarget` already
uses), record edge `srcObj → destObj`. Build `adj: t_gobj* → vector<t_gobj*>`.

**B2. Cycle detection (DFS with visited-set):**

```cpp
// iterative DFS per connected component, signal edges only;
// on revisit of an in-progress node → record the path as a cycle
```

Output: `dsp_cycles: [[tempId, tempId, ...], ...]` — names tonight's
`osc_bass → sum_pitch → osc_bass` directly. Terminates always (visited set).

**B3. Unscheduled detection:** after `canvas_update_dsp()`, signal objects
present in `gl_list` but excluded from the DSP schedule. Simplest v1:
re-run the cycle DFS — members of detected cycles = the unscheduled set
(Pd refuses loop members). v2 (later): hook the signal compiler.

**B4. Dangling + zeroed:**
- dangling signal inlet: object has an audio inlet (`struct` check or
  `obj_siginlet`-style probe) with no wire → record `{tempId, inlet}`
- zeroed VCA: `*~` whose inlet 1 has no wire AND no creation float →
  outputs constant 0 → record (the bare-`*~` law, checked natively)

**B5. Reply JSON:** one OSC string arg containing
`{dsp_cycles, unscheduled, dangling_sig_inlets, zeroed_vcgs, mismatched}`.
TS parses and renders. Add `key` per object (bound name) for tab scoping.

### Phase C — TS glue (~1h)

**C1.** `osc-client.ts`: `saveDiagnose(canvas)` → `/pd/diagnose` + JSON parse.
**C2.** `transaction-v4.ts`: escalation (3b) switches from the TS registry
run to `/pd/diagnose` when cap `diagnose` present (TS registry stays as
fallback for legacy bridges).
**C3.** `analyze_patch` gains `action: "diagnostics"` rendering the facts for
the agent (human-readable, same style as audit).

### Phase D — Gates

1. `cmake --build` + `npm run build` (both clean)
2. Artist: reopen PlugData + reload server
3. Identity gauntlet re-run (I1–I9) — no regressions
4. Failure-class tests: each deliberately created → named in same response
5. Zero-drop check: beat playing through every test

### Risk notes

- Cycle DFS must only traverse SIGNAL edges (control wires would create false
  cycles) — filter by outlet signal-type during adjacency build.
- All walks under the existing `sys_lock()` in the batch lambda — same
  pattern as census; read-only, zero audio impact.
- diagnose is READ-ONLY — it must never mutate (the failure class that
  started the session: silent mutation).

