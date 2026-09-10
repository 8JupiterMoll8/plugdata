# PRD — `construct_patch_v4` Truth Verdict + Fix Hints

**Date:** 2026-09-10
**Branch:** `feat/v4-zero-dropout-copilot`
**Status:** PROPOSED (design ready for implementation)
**Owner:** MCP bridge (C++) + MCP server (TS)
**Related:** `mcp-server/PRD-truth-gated-memory-harness.md` (the existing syntax-truth memory
harness this PRD extends with **structural** truth — see §4.4)

---

## 1. Problem

After a build, the AI must answer *"did it work, and if not what's wrong?"* Today the
facts exist but are **scattered** across the `_v2meta` reply:

- `createFailures` / `connectFailures` / `editFailures` / `deleteFailures`
- `diagnose` (zeroed_vcgs, dsp_cycles, dangling_main_sig, mismatched)
- `silenceReport` (chokePoint, reason)
- `reconcile.detectedConnections`
- `warnings[]` (prose)

Consequences:
1. The AI must **assemble** the picture itself → slow, and it sometimes gives up and
   asks the artist *"is it working? I can't hear it?"*
2. Fix guidance is **inconsistent** — some guards include a fix (rate/cycle/null-gain),
   others only name the problem.
3. Agents waste round-trips calling `analyze_patch(census)` + `monitor_signal_flow`
   to re-derive facts the build already computed (verified identical — see §7).

## 2. Goal

Every `construct_patch_v4` reply carries **one compact `health` verdict** — a single
line the AI can act on immediately — and **every issue carries an actionable `fix` hint**.

**No new detection.** This is aggregation of facts the C++ bridge already computes in the
same call. **Zero extra round-trips.**

### 2.1 Non-Negotiable Performance & Safety Invariants

Mirrors the memory-harness invariants (`mcp-server/PRD-truth-gated-memory-harness.md` §2.1).
Every implementation of this PRD MUST hold all of these:

1. **Zero audio dropouts.** The verdict adds **no new DSP work** — no graph recompile, no new
   nodes, no audio-thread mutation. It is assembled from facts the *same* `batch_atomic` call
   already produced. The X-ray stays read-only under `sys_lock`.
2. **Never block the audio thread.** Verdict synthesis runs on the existing batch path
   (C++ / OSC thread), never inside the audio callback. Memory operations stay in Node —
   **zero memory code touches the C++ audio thread.**
3. **The audio measurement is gated / optional.** The silence check needs ~60 ms to settle,
   so it runs **only** on `playNote` / `verifyAudio` (or when a `dac~` exists with no gate
   receiver). Plain builds must **not** wait for it. When not measured: `audible: null`, and
   the verdict is never `silent` by assumption.
4. **Microsecond budget.** Verdict synthesis = pure CPU/JSON in C++ (microseconds). Memory
   lookup = RAM hash (~50 ns). **No disk I/O** in the build fast path.
5. **Bounded reply size.** `health` stays compact; `issues` capped (default 8, with a
   `"+N more"` note). No raw census/monitor dumps in the reply.
6. **Backward compatible.** `health` is appended as a trailing field; older clients ignore
   it. No existing field changes shape.
7. **Fail-safe.** If health synthesis fails for any reason, the build reply still returns
   normally (health omitted) — it must never turn a successful mutation into an error.

## 3. Non-goals

- No auto-healing in Phase 1 (keeps the artist in control). See Phase 3.
- No judgement of *sound quality* / musical intent — the tool cannot hear "does it sound
  good", only "is sound present and is the graph structurally sound".
- No raw census dumps in the reply (token bloat).

## 4. Design

### 4.1 `health` object (new tail field on the batch_atomic reply)

```jsonc
"health": {
  "verdict": "ok" | "warn" | "silent" | "error",
  "audible": true | false | null,          // null = not measured this call
  "chokePoint": "z" | null,                // exact object where signal died
  "reason": "multiplier inlet 1 is zero or disconnected (silent VCA)" | null,
  "issues": [
    { "kind": "null-gain", "tempId": "z", "severity": "warn",
      "message": "bare [*~] with unwired inlet 1 outputs 0",
      "fix": "give it a numeric arg (*~ 0.5) or wire a signal into inlet 1" }
  ],
  "counts": { "objects": 3, "wires": 2,
              "createFailed": 0, "connectFailed": 0, "editFailed": 0, "deleteFailed": 0 }
}
```

### 4.2 Verdict logic (deterministic, C++)

| Condition | verdict |
|---|---|
| any `*Failures` non-empty OR `canvasNotFound` | `error` |
| `audioVerified === false` (measured this call) | `silent` |
| any diagnose issue (zeroed VCA / cycle / mismatched) | `warn` |
| otherwise | `ok` |

`audible`/`silent` only reflects a **measured** check. Gated patches (envelope waiting on
`[r gate]`) are legitimately silent at rest → when not measured, `audible: null`,
`verdict` never `silent` from assumption.

### 4.3 Fix-hint table (kind → `fix`)

| kind | fix |
|---|---|
| `null-gain` (zeroed VCA) | give `[*~]` a numeric arg or wire a signal into its right inlet |
| `dsp-cycle` | break the loop with `[delwrite~]`/`[delread~]` or `[send~]`/`[receive~]` |
| `rate-mismatch` | insert `[line~]` (or `[sig~]`) between the control and signal object |
| `dangling-source` | info only — sources are allowed; ignore unless unintended |
| `create-failed` | object name not found — verify via `lookup_pd_library(find_objects)` |
| `self-connection` / `dup-wire` / `port-range` | (specific, already present) |
| `canvas-not-found` | list real canvases via `analyze_patch(census)` |
| `silent chokePoint` | per-class hint (null-gain / untriggered envelope / broken path) |

### 4.4 Integration with the existing Truth-Gated Memory Harness (structural-fix memory)

This is **not a new system** — it completes the harness that already exists
(`mcp-server/PRD-truth-gated-memory-harness.md`, `learning-manager.ts`,
`circuit-breaker.ts`, `memory.json`).

Today that harness remembers **syntax truth**: object-name aliases
(`flanger~ → flanger.m~`), sealed only after a clean C++ receipt.

This PRD extends the **same** harness with **structural truth** — a new memory class that
maps a **problem signature → the fix that made it go away**, sealed by the **verdict**:

```
build  → verdict: silent @ 'z' (null-gain)
   │ AI applies fix
build  → verdict: ok
   │ TRUTH-GATE: verdict flipped (silent/warn/error → ok) on the same canvas
seal   { signature: "null-gain@*~", fix: "give [*~] a numeric arg or wire inlet 1" }
   │ next occurrence
receipt carries the remembered fix as a hint — no reasoning needed
```

**Rules (mirror the existing anti-poisoning law):**
- Seal **only** when the verdict improves (`silent`/`warn`/`error` → `ok`) **and** the named
  object still exists **and** C++ reports zero failures. C++ verdict is the only judge.
- **Problem signature must be stable** — e.g. `null-gain@*~`, `rate-mismatch@ctl→sig`,
  `dsp-cycle`. **Never** use the `tempId` (it churns).
- **Bounded** entries (same cap policy as Tier 1).
- **Never overwrite** an existing mapping without explicit confirmation.
- **Quarantine** a structural fix if applying it twice still fails (reuse the 2-strike
  breaker + `quarantined` map).

**Roles:**
- The static fix-hint table (§4.3) is the **seed / bootstrap**.
- The memory harness makes it **compound** — the AI stops re-deriving the same fix.

**Storage:** a new `structural_fixes` key in `memory.json` (or a sibling file), reusing the
read-before-write + truth-gate helpers already in `learning-manager.ts`.

## 5. Implementation plan

### C++ (`plugdata-core/Source/Pd/MCPBridge.cpp`)
1. Add `computeHealthFacts(...)` that takes the already-collected failure vectors +
   `diagnoseJson` + `silenceReport` + `detectedConnections.size()` and returns a compact
   `health` JSON string. Called at the end of the `batch_atomic` handler (reply build).
2. Add `fix` strings to the issue objects emitted by `computeDiagnoseFacts` (zeroed VCA,
   cycles, mismatched) and to the null-gain silent-killer rule.
3. Append `health` as a **new trailing arg** after `undoReset` (backward compatible —
   old TS clients ignore trailing atoms).

### TS (`mcp-server/src/transport/osc-client.ts`)
4. Parse the trailing `health` JSON in `batchAtomic` → `health` field.

### TS (`mcp-server/src/tools/patching/transaction-v4.ts`)
5. Surface `health` in the top-level output + `_v2meta`.
6. Lead the human-readable message with the verdict line
   (e.g. `⚠ SILENT — signal dies at 'z': give [*~] a gain or wire inlet 1`).

### Tests
7. Reconciliation test: build a broken patch → assert `health` equals the standalone
   `analyze_patch(diagnostics)` + `monitor_signal_flow(master)` values (§7).

### Structural-fix memory (extends the existing harness — §4.4)
8. `learning-manager.ts`: add `structural_fixes` store + `sealStructuralFix(signature, fix)`
   and `suggestStructuralFix(signature)`, reusing the read-before-write + truth-gate helpers.
9. `transaction-v4.ts`: after each build, compare this call's `health.verdict` with the
   previous verdict for the canvas; on improvement, seal the (signature → applied fix).
   Inject remembered fixes into `health.issues[].fix` (marked `source: "memory"`).

## 6. Phasing

- **Phase 1 — Verdict + fix hints (seed).** Aggregation of existing C++ facts + the static
  fix-hint table. Low risk, no new detection.
- **Phase 2 — Structural-fix memory (compound).** Extend the existing truth-gated memory
  harness (§4.4) to learn problem→fix pairs from verdict improvements. Reuses
  `learning-manager.ts` + `circuit-breaker.ts`; nothing new to trust.
- **Phase 3 (optional, gated flag) — Auto-heal.** Apply *mechanical, unambiguous* fixes
  automatically (auto-insert `[line~]` for a rejected rate wire, auto-give a bare `*~` a
  default gain). Must never silently change artistic intent; off by default.

## 7. Evidence (reconciliation already verified live)

On `osc~ → bare *~ → dac~`, the inline v4 facts matched the standalone tools exactly:

| Fact | v4 inline | standalone |
|---|---|---|
| object count | 3 | census: 3 |
| wire count | 2 | census: 2 |
| zeroed VCA | `[{z, inlet 1}]` | diagnostics: `[{z, inlet 1}]` |
| dangling inlet | `["o"]` | diagnostics: `["o"]` |
| silent? | `audioVerified:false` | master: Silent -100 dB |
| choke point | `silenceReport.chokePoint: "z"` | diagnostics names `z` |

→ The verdict is the **same C++ truth**, just stated once.

## 8. Risks / mitigations

- **Reply size** → keep `health` compact; `issues` capped (e.g. 8) with a "+N more" note.
- **False "silent"** on gated patches → only set `silent` from a *measured* check.
- **Backward compat** → append as trailing atom; old clients ignore.
- **God-tool creep** → Phase 1 adds **no** new detection; it only summarizes.

## 9. Success criteria

1. One build → one verdict line; the AI needs **no** follow-up census/monitor call to know
   what's wrong.
2. Every issue includes an actionable `fix`.
3. Verdict values match standalone census/diagnostics/monitor (reconciliation test).
4. Zero extra OSC round-trips vs today.
5. **(Phase 2)** A fix that improves the verdict is sealed into the memory harness and
   re-suggested automatically on the next occurrence — solving **compounds** instead of
   being re-derived.
