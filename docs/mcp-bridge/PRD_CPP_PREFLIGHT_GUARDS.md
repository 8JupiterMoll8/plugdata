# PRD: C++ Preflight Guards — Reject-at-Connect in batch_atomic

**Document Version:** 1.0.0 (DRAFT)
**Target Repos:** `plugdata-core` (C++ bridge) & `PlugData-MCP-Server` (TS mirror)
**Depends on:** PHASE 0 auto-reconcile · Phase A batch-facts · Phase B fault-injection guards (2026-09-09, UNCOMMITTED) · Diagnostic Layer PRD §2.2
**Author:** Jupiter Moll & opencode
**Status:** PROPOSED

---

## 1. Motivation — Four Silent-Failure Classes the Batch Still Lets Land

The 2026-09-09 Phase B gauntlet closed the *naming* gap (self-connection, port
range, duplicate wire are now rejected with named facts in the same reply).
Four classes remain where **the bad mutation lands on the canvas first** and is
only diagnosed afterwards — or never:

| # | Failure class | Today's path | Cost |
|---|---|---|---|
| 1 | Signal-outlet → control-inlet wire | Wire is created raw (`MCPBridge.cpp:2114` calls `obj_connect` directly, bypassing `Interface::canConnect` which contains the exact rate rule at `Interface.h:1094+`); diagnose X-ray reports it as `mismatched` AFTER the fact | Dead wire on canvas; agent needs a fix turn |
| 2 | Zero-delay signal cycle across a batch | All wires land; Pd's DSP compiler mutes the looped objects; ride-along diagnose names `dsp_cycles` in the same reply (verified 2026-09-03 gauntlet, ~18ms) | Graph exists in a broken state; audio muted until agent rewires |
| 3 | Retry duplication after lost reply | TS `batchAtomic` sends with `retries: 1` reusing the SAME corrId (`osc-client.ts:644–684`); if the reply is lost but the mutation applied, the retry re-pastes EVERY create. Canvas gets duplicate physical objects; the original becomes a `gui_*` orphan via PHASE 0 adoption | Corrupt canvas; identity churn; agent confusion |
| 4 | `[metro <10]` → GUI widget wire | Nothing in C++; the only guards are TS-side on paths v4 builds never reach (`lint.ts:86–107` on-demand tool; `gui-flood.ts` registry rule — symptom-triggered only) | **Environment killer:** GUI event flood can freeze PlugData itself — blast radius is the whole runtime, not the patch |

**Governing law (unchanged):** C++ is the ONLY source of truth. These guards
run inside the batch against live objects — no TS plan-checking, no doc-guessing.
TS keeps zero preflight; it only surfaces the new facts.

**Design stance — reject, don't auto-heal:** rate/cycle/flood violations are
REJECTED with a named reason + suggestion; the agent fixes in its next turn
(matching the established "Act on warnings in the NEXT call" harness loop).
Auto-splicing bridges (`sig~`/`snapshot~`) was considered and deferred (§8).

---

## 1.5 BASELINE — Phase B already shipped (2026-09-09) — DO NOT re-implement

All of this is live in the `batch_atomic` connect section (`MCPBridge.cpp:2050–2125`)
and MUST NOT be duplicated:

- **Self-connection** (src == dest) → named connectFailure (`:2068–2072`)
- **Src outlet out of range** → named, includes real `obj_noutlets` (`:2075–2081`)
- **Dest inlet out of range** → named, includes real `obj_ninlets` (`:2082–2088`)
- **Duplicate wire** (walks `obj_starttraverseoutlet`) → no-op, not counted (`:2090–2112`)
- **Unresolvable src/dest tempId** → named "src not found / dest not found" (`:2055–2060`)
- **Red-box creates** → paired to pendingCreates, named createFailure (`:1995–2040`)
- **line~/vline~ init seeding** at parse (`:1655–1674`)

This PRD adds guards R1–R4 **after** those checks, at the same site.

---

## 2. R1 — Rate Guard (signal→control REJECT, control→signal FACT)

### Rule

In the Phase 5 connect loop, after the duplicate-wire check and before
`obj_connect` (`MCPBridge.cpp:2114`):

```cpp
bool srcSig  = obj_issignaloutlet(so,  cc.srcOut) != 0;
bool destSig = obj_issignalinlet(d_o, cc.destIn)   != 0;
if (srcSig && !destSig) {
    connectFailures.push_back({
        cc.srcId.toStdString(), cc.destId.toStdString(),
        "rate: signal outlet " + std::to_string(cc.srcOut)
          + " -> control inlet " + std::to_string(cc.destIn)
          + " — Pd drops the audio; use [snapshot~] (meter) or rewire via [*~]" });
    continue;
}
```

- **signal → control: BLOCK.** This is exactly `Interface::canConnect`'s own
  rule (`return !obj_issignaloutlet(src, nout) || obj_issignalinlet(sink, nin);`
  — `Interface.h`, `canConnect`, ~line 1094). The GUI connection tool enforces
  it; the batch path currently bypasses it by calling `obj_connect` raw.
- **control → signal: ALLOW, emit fact.** Pd semantics legitimately convert
  floats to constant signal on signal inlets (vanilla behavior; `canConnect`
  permits it). It remains an *artistic* hazard (no ramps → zipper noise), so it
  stays a **diagnose X-ray `mismatched` fact**, non-blocking. Do NOT block —
  that would break `[r freq] → [osc~]` float-setting which is valid Pd.
- Do not modify the diagnose handler's `mismatched` collection
  (`MCPBridge.cpp:594–613`): after R1 ships, `sig->ctl` entries there shrink to
  GUI-drawn wires only, which is correct (X-ray sees all truth, batch guards
  only its own path).

### Reason-string contract

All new rejection reasons carry a **stable prefix** so TS can classify without
fragile prose matching: `rate:`, `cycle:`, `gui-flood:`, `idempotent:`,
`busy:`. Free text follows the prefix.

---

## 3. R2 — Cycle Guard (zero-delay signal loop REJECT-at-wire)

### Rule

A wire that would close a zero-delay signal cycle is rejected BEFORE
`obj_connect`. Rationale: Pd has NO legal zero-delay signal cycles (feedback
only via `delwrite~/delread~`, `send~/receive~` — which are NOT wire cycles,
so no false positives by construction).

### Algorithm (inside the audio lambda, Phase 5, before the connect loop)

1. **Live signal adjacency — reuse Step D.** PHASE 0 Step D already walks all
   wires via `linetraverser` (`MCPBridge.cpp:1785+`). While building
   `detectedConnections`, also record signal edges: `lt.tr_outlet->o_sym ==
   gensym("signal")` (same predicate the diagnose handler uses, `:612`) into
   `liveSigAdj: map<gobj, vector<gobj>>`.
2. **Tentative apply, in batch order.** For each candidate wire in `allConns`
   (both endpoints resolved): if the src outlet is signal (`obj_issignaloutlet`),
   DFS from `dest` — if `src` is reachable, the wire closes a cycle:
   ```cpp
   connectFailures.push_back({
       cc.srcId.toStdString(), cc.destId.toStdString(),
       "cycle: would close zero-delay signal loop — break it with "
       "[delwrite~]/[delread~] or [send~]/[receive~]" });
   continue;  // do NOT add to tentative graph
   ```
   Otherwise add the edge to the tentative adjacency.
3. **Bound the DFS:** if `objects > 2000 || wires > 4000`, skip R2 entirely and
   append a reply note `cycle-guard: skipped (graph too large) — X-ray still
   reports`. Never let a safety check stall the audio thread.

**Ordering note:** candidate wires must be processed in `allConns` order so the
FIRST loop-closing wire is rejected and later independent wires still land.

### What changes vs. today

Today the 3-node cycle from the 2026-09-03 gauntlet *lands*, mutes audio, and
gets named by the X-ray in the same reply. After R2 the wire **never lands** —
`dsp_cycles` in the X-ray stays empty, canvas stays clean. Same information,
zero cleanup turn.

---

## 4. R3 — Idempotency (exactly-once batch replies)

### R3a — corrId dedup cache (ship first)

**Problem:** TS `sendAndAwait(..., { retries: 1 })` re-sends the identical
`/pd/batch_atomic` args (corrId included, built once at `osc-client.ts:644`)
when the reply is lost. If the first delivery applied, the retry re-applies the
whole batch → duplicate objects.

**Fix (C++, entry of the batch_atomic handler, BEFORE pre-parse):**

```cpp
// LRU + TTL dedup: corrId -> full reply message
static std::unordered_map<juce::String, juce::OSCMessage> s_dedupCache; // bridge-lifetime
static std::deque<juce::String> s_dedupOrder;                          // LRU order
static constexpr int DEDUP_MAX = 128;
static constexpr uint64_t DEDUP_TTL_MS = 30'000;
```

- On entry: if `corrId` is in the cache and age < TTL → **re-send the cached
  reply verbatim** and return. (The retry exists precisely because the reply
  was lost — resend, don't recompute.)
- After building the reply at the end of the handler: store it in the cache,
  evict LRU beyond `DEDUP_MAX`.
- Cache lives in bridge (PlugData-process) memory: a PlugData restart clears
  it naturally — no boot-token plumbing needed on the C++ side.

**TS-side companion (required, see §6):** `nextCorrelationId()` is a monotonic
counter that RESETS on TS restart (`osc-client.ts:472–477`) — a restarted TS
could re-emit `corrId 1` and hit a stale cache entry (false dedup). Fix: for
`batchAtomic` only, compose the corrId as
`"${sessionNonce}-${counter}"` where `sessionNonce` is a random 6-char string
generated once per `OscClient` construction. Wire-compatible: the reply
address is string-built on both sides; no numeric arg assumptions exist.

**Capability:** announce `batch-dedup` in `/bridge/capabilities/reply`
(add one `reply.addArgument(juce::String("batch-dedup"));` at the caps block,
`MCPBridge.cpp:5894–5965`). TS gates the nonce scheme on this cap so an old
bridge (no dedup) gets plain counters as before.

### R3b — done.wait timeout must NOT emit a success-shaped reply

Today, if the audio lambda exceeds `done.wait(2000)` (`MCPBridge.cpp`, the
wait after the lambda), the handler proceeds to build and send a reply from
**partially-filled storage variables** — a mid-mutation snapshot that looks
like a normal success/failure reply. A TS retry then double-applies.

**Fix:** if `done.wait(2000)` returns `false`, send
`/pd/batch_atomic/error/<corrId>` with `"busy: audio thread did not finish in 2000ms — batch state UNKNOWN"` and DO NOT send the normal reply. Cache nothing.

**TS-side companion (required):** `batchAtomic` treats an error-address reply
as a special `null`-like result with `stateUnknown: true`. `construct_patch_v4`'s
catch path currently AUTO-UNDOS on failure (`transaction-v4.ts:1062–1068`) —
for `stateUnknown` undo is WRONG (the batch may have applied; undo would revert
a half-known state on top of an unknown one). Instead: skip auto-undo, run
`getAnalysis(canvas, true)` (census reconcile) and tell the agent
"suggestedNext: state unknown — census ran, trust its objects list".

### R3c — same-tempId create skip (follow-up phase, optional)

Agent-driven re-calls (new corrId, same tempIds) still duplicate. After R3a,
extend the create path: inside the lambda AFTER PHASE 0 Steps B/C (live truth),
if `canvasMap.count(tempId)` and the mapped pointer is live and the class name
matches the pending create → skip the paste entry and report it in the new
`idempotentSkips` reply section (§5). If the class differs → named
createFailure `"idempotent: tempId already live with different type — use edit or delete first"`.
Requires moving `pastaBuffer` assembly into the lambda (string concat, µs) so
the filtered set is pasted. **Defer until R3a is verified live** — R3a already
covers the protocol-level retry case.

---

## 5. R4 — GUI-Flood Guard (metro-rate REJECT)

### Rule

In the Phase 5 connect loop, in addition to R1: a direct wire from a `metro`
object whose **first creation arg is numeric and < 10 (ms)** to a GUI-class
object is rejected:

```cpp
// src class check: pd_class(&sg->g_pd) == gensym("metro") — read first atom
// from ((t_text*)so)->te_binbuf (numeric?) — interval ms = atom value
// GUI sink set (PlugData class names, superset of TS lint.ts:90–95 / gui-flood.ts:22–25):
//   bng, tgl, toggle, nbx, hsl, vsl, hradio, vradio, knob, vu, cnv,
//   slider, number, floatatom, symbolatom, canvas
if (isRapidMetro && isGuiSink) {
    connectFailures.push_back({
        cc.srcId.toStdString(), cc.destId.toStdString(),
        "gui-flood: [metro " + interval + "] (<10ms) direct into GUI object — "
        "insert [speedlim 50] or [change] to throttle" });
    continue;
}
```

- **Block threshold: interval < 10ms** (unambiguously pathological). Intervals
  10–50ms into GUI sinks: ALLOW but append a reply note (advisory), matching
  TS `lint.ts`'s 20ms warning spirit without breaking legitimate fast UIs.
- **Direct edge only** — same scope as the TS `gui-flood.ts` rule (immediate
  src → immediate dest). Path-based analysis stays in the X-ray (follow-up,
  see §8).
- Reading the metro arg: if the binbuf atom isn't numeric or the object is
  pre-existing without readable args, degrade to allow + advisory note — never
  block on a read failure.
- This is the only R-guard where the protected asset is the **runtime itself**
  (GUI event flood freezes PlugData), justifying block severity.

---

## 6. Wire Format — Backward-Compatible Reply Extension

Current arg order after the counts (TS parser: `osc-client.ts:678–741`):
`[mappings pairs]* [detectedConnections JSON] [cfCount, facts...] [wfCount,
facts...] [diagnose JSON]`.

**New sections append AFTER the diagnose JSON** — old TS parsers stop reading
there, so nothing breaks:

```
... diagnose JSON (may be "{}") ...
int32 idempotentSkipCount          // R3c (0 until R3c ships)
  (tempId: String, reason: String) × idempotentSkipCount
int32 advisoryCount                 // R4 soft notes, R2 skip notes, R1 ctl->sig facts
  (note: String) × advisoryCount
```

Rejected wires from R1/R2/R4 ride the **existing `connectFailures`** section —
no format change, just new reasons with the stable prefixes (§2).

**Capability announcement:** add `preflight-guards` to the caps reply
(`MCPBridge.cpp:5894–5965`, one line next to `batch-facts` at `:5965`). TS uses
it to (a) trust the exhaustive guard coverage, (b) parse the new trailing
sections.

---

## 7. TS Mirror Changes (minimal — facts in, zero preflight)

Per the governing law, TS adds NO preflight. Changes:

1. **`osc-client.ts` `batchAtomic`:**
   - corrId = `${sessionNonce}-${counter}` gated on `batch-dedup` cap (§4 R3a).
   - Handle `/pd/batch_atomic/error/<corrId>` → return `{ stateUnknown: true }`
     sentinel instead of throwing; `retries` stays 1 (dedup makes it safe).
   - Parse `idempotentSkips` + `advisories` trailing sections when
     `preflight-guards` cap present.
2. **`transaction-v4.ts`:**
   - `_v2meta`: surface `idempotentSkips` and `advisories` verbatim (receipt
     truth); classify `connectFailures` reasons by prefix for
     `suggestedNext` (`rate:` → "rewire via [*~] or [snapshot~]", `cycle:` →
     "break with delwrite~/delread~", `gui-flood:` → "insert [speedlim 50]").
   - Error path: if `stateUnknown` → **skip auto-undo**, run census, receipt
     says "state unknown — census reconciled". (Undo on unknown state is
     forbidden.)
3. **`undo` interplay:** rejected wires never called `obj_connect`, so no undo
   entries are created for them — undo depth stays meaningful. No change
   needed; document in code comment.
4. **AGENTS.md harness rule:** schema descriptions for `construct_patch_v4`
   gain one line ("connect guards: rate/cycle/gui-flood rejected with named
   reasons; idempotent replies on retry"). A guard invisible in the schema is
   invisible to the AI.
5. **Docs:** update the tool description of `construct_patch_v4` in
   `patching.ts:659` with the same one-liner.

---

## 8. Out of Scope / Deferred

- **Auto-adapter splice** (`sig~`/`snapshot~` inserted by the bridge on rate
  rejection) — deferred: the harness loop is receipt-driven; the agent heals
  next turn. Auto-splicing mutates the artist's graph unrequested.
- **Control-rate cycle blocking** — infinite control loops freeze Pd, but
  intentional control loops with `[delay]`/`pipe]` breaks are legal. Stay
  fact-only (X-ray `unscheduled` + TS `circular-logic` warn).
- **Path-based GUI-flood analysis** (metro → x → … → GUI through intermediates)
  — X-ray follow-up; direct-edge block ships first.
- **v2 path wiring** — v2 mutates via its own connect calls; porting R1/R2/R4
  to the v2 handlers is mechanical but out of scope (v4 is the only sanctioned
  mutation engine).

---

## 9. Acceptance Gauntlet (extend the 2026-09-09 Phase B protocol)

Run on a dedicated window (`PLUGDATA_RECEIVE_PORT=19021` retarget) with the
standard 33-object test patch rebuilt per AGENTS.md. Beat running at 128 BPM;
**zero dropouts permitted in ALL runs**.

| # | Test | Expected |
|---|---|---|
| 1 | Batch connecting `[osc~ 440]` outlet → control inlet (e.g. `[print]`) | `connectFailures` names wire with `rate:` prefix; wire absent; X-ray `mismatched` for it: none from batch path |
| 2 | Batch creating 3-node signal cycle (`a→b→c→a`, all `~`) | First loop-closing wire rejected with `cycle:`; other wires land; X-ray `dsp_cycles` EMPTY; audio un-muted |
| 3 | Batch with `delwrite~`/`delread~` feedback loop | ALLOWED (not a wire cycle) — no false positive |
| 4 | Kill reply delivery (raw-OSC script, retargeted port), TS retry fires with same corrId | Object exists exactly ONCE; retry reply byte-identical to first (counts+mappings); PHASE 0 reports zero `gui_*` adoption |
| 5 | Force `done.wait` timeout (block audio thread >2s) | `/pd/batch_atomic/error/…` with `busy:` reason; TS does NOT auto-undo; census truth surfaces in receipt |
| 6 | `[metro 5]` → `[bng]` direct | Rejected, `gui-flood:` prefix, suggestion names `[speedlim 50]` |
| 7 | `[metro 500]` → `[bng]` and `[metro 30]` → `[bng]` | Both allowed; 30ms case carries advisory note |
| 8 | `[r freq]` → `[osc~]` (control→signal) | ALLOWED (float→signal is valid Pd); advisory fact only |
| 9 | Clean rebuild of standard patch (no faults) | Zero new warnings, zero guard cost visible: executionMs delta < 2ms vs. pre-guard baseline |
| 10 | Old TS binary against new bridge | All existing sections parse unchanged (trailing sections ignored); dedup inert until TS sends nonce corrIds |
| 11 | New TS against old bridge (cap `preflight-guards` absent) | TS skips trailing-section parsing, nonce scheme off — full backward compat |

**Regression watch:** I9 tripwire (v4-undo object loss) after any change to
the connect loop — undo/re-undo cycles must still restore every tempId.

---

## 10. Performance Budget

- **R1:** two `obj_issignal*` calls per wire — nanoseconds each.
- **R2:** reuses the Step D wire walk (already running); DFS bounded O(W×(V+E))
  with hard bail-out at 2000 objects / 4000 wires → worst case skipped with a
  note. On the gauntlet patch (33 objects, ~40 wires): < 1ms.
- **R3a:** one hash lookup on entry, one insert on exit.
- **R4:** class-symbol compare + binbuf atom read per metro-sourced wire.
- All checks run where the duplicate-wire walk already runs — pure CPU, no
  syscalls, no additional sys_lock scopes, no change to the audio-dropout
  profile. Total added on clean builds: **< 2ms** (acceptance #9 enforces).

---

## 11. Implementation Order

1. **R1 rate + R4 flood** (same site, same pattern as Phase B — smallest diff)
2. **R2 cycle** (needs Step D edge collection + tentative graph)
3. **R3a corrId dedup + R3b busy-reply** (wire-format + TS companions together)
4. **Trailing reply sections + caps + TS surfacing** (ships with R3a)
5. **R3c same-tempId skip** (only after R3a verified live in the gauntlet)

Each step: compile (`cmake --build . --target plugdata_standalone -j8`),
reopen PlugData, reload MCP server, capabilities re-negotiated, then run the
gauntlet rows relevant to that step.
