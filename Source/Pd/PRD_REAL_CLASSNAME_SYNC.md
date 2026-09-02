# PRD: Real Class Name Sync for Manual Canvas Objects (MERDA `.m~` & Abstractions)

**Document Version:** 1.1.0
**Target Repositories:** `plugdata-team/plugdata` (`plugdata-core`) & `8JupiterMoll8/PlugData-MCP-Server`
**Author:** Jupiter Moll & Antigravity
**Date:** 2026-09-02
**Status:** PROPOSED / ARCHITECTURE SPECIFICATION

### Revision Notes (v1.1.0)

This revision corrects factual errors found during implementation review:

| Issue | v1.0.0 | v1.1.0 |
|---|---|---|
| `gl_loaded` field | Referenced as the abstraction detection mechanism (§2.1) | **Does not exist in vanilla Pd.** Replaced with `canvas_isabstraction()` (`gl_env != NULL`) — the canonical Pd-internal check. |
| `gl_path` field | Referenced in §2.1 implementation sketch and API table | **Does not exist on `t_canvas`.** Removed. Path info lives in `gl_env->ce_dir` (irrelevant — `canvas_isabstraction()` is sufficient). |
| `/pd/typeof` for canvas-class | Claimed "already works" for abstraction name resolution | Returns `"pd"` for canvas-class objects (same as census). TS `getTypeOf()` fallback is dead code for the abstraction-name problem. Added §1.3 caveat and §2.3 decision tree. Recommended: enhance `typeof` handler to call `getAbstractionFileName`. |
| `type` vs `class` field inconsistency | Not documented | Two adoption paths store class name under different keys (`type` in `adoptIdentity`, `class` in telemetry auto-adoption). All downstream readers use `entry.type` — causes silent failures for telemetry-adopted objects. Added as A6 in §2.6.1 audit. |
| `isSignalObject` B8 | Claimed "auto-fixes for MERDA once `node_class` lands" | Does NOT auto-fix — `type === "pd"` branch returns `false` for MERDA modules. Added explicit `node_class` check specification. |
| B6/B7 `args[0]` semantic shift | Not noted | After `node_class`, `args[0]` for MERDA modules is the filename, not a user-given name. Subpatch matchers should guard with `!node_class` to restrict to inline subpatches. |
| §2.6.2 Option A blast radius | Claimed "all existing `type==='pd'` matchers keep working" | B6/B7/B8 require changes even under Option A. Corrected. |
| Effort estimate | ~1.5 developer-days | Revised to ~2 developer-days (A6 fix, B8 explicit, typeof enhancement, B6/B7 guard). |

---

## 1. Executive Summary & Problem Statement

### 1.1 The Issue

When an artist manually adds an object to the canvas in the PlugData GUI (e.g. a MERDA `crusher.m~` or `plate.rev.m~` module, or any abstraction) **outside** of the MCP `construct_patch_v4` pipeline, the TypeScript tooling reports it with an opaque, inconsistent identity:

- The **census** (`/pd/census` → `analyze_patch(action:'census')`) reports the object's type as `"pd"` — the generic abstraction/subpatch wrapper class — making it indistinguishable from any other `[pd foo]` subpatch.
- The **selection telemetry** (`/pd/ui/selection`) reports the **real** class name (`plate.rev.m~`) because the `Sidebar` resolves it via `object->getType(false)`.
- The **auto-adopted tempId** becomes `pd_main_90` (built from the `pd` wrapper class) instead of something readable like `plate_rev_main_90`.

**Result:** two different C++ code paths disagree about the same object's identity → the MCP tools that depend on these paths (census, `get_selection`, `probe`, port/wiring resolution) are **not in sync**, and manual additions are effectively "invisible" to the tooling.

**Governing principle this PRD enforces:** the C++ bridge is the ONLY source of truth for object identity. TS never guesses a class name; it mirrors what C++ reports. When the artist adds `crusher.m~` by hand in the GUI, it is exactly as real to C++ as one created via `construct_patch_v4` — and so it must be to every MCP tool. (See §2.0.)

### 1.2 Root Cause Analysis

Line-by-line inspection identified three compounding causes:

1. **Census path lacks abstraction resolution (`PluginProcessor.cpp:2762`):** the native census sets `type = className` but for `canvas_class` (subpatch / GOP / abstraction instance) it hard-codes `type = "pd"` and records only the subpatch name in `args[0]`. It never resolves whether the subpatch is actually a MERDA `.m~` abstraction instance whose *loaded file name* is `crusher.m~`.

2. **Two divergent class-name sources in C++:**
   - **Census** uses `class_getname(cl)` on the `gobj`'s pd class → yields `pd` for any subpatch/abstraction wrapper.
   - **Selection telemetry** uses `Sidebar::showParameters()` → `object->getType(false)` → yields the real abstraction name (`plate.rev.m~`) because the JUCE `Object` component resolves the GOP/abstraction file path.

3. **TS adoption picks the wrong source (`telemetry.ts:104` + `resolve-selected.ts:53`):** when minting an auto-adopted tempId, the TS `adoptIdentity` is fed the census type (`pd`) instead of the telemetry `className` (`plate.rev.m~`), so the readable `plate_rev_main_90` is never generated.

### 1.3 The `typeof` Endpoint Already Exists (with a caveat)

The C++ bridge already provides `/pd/typeof` (`MCPBridge.cpp:429`) which reads the object's real class name from the pd struct via `class_getname(pd_class(&gobj->g_pd))` AND the full binbuf `objectText`. The TS client already wraps it in `oscClient.getTypeOf()` (`osc-client.ts:447`) — **but `getTypeOf()` is never called** during census enrichment or identity adoption. The plumbing exists on both ends; only the wiring is missing.

> **⚠ `typeof` has the same `"pd"` limitation as census:** `class_getname(pd_class(...))` returns `"pd"` for any canvas-class object, including loaded MERDA abstractions. The TS `getTypeOf()` fallback in §2.3 therefore cannot resolve real abstraction names on its own — it returns the same `"pd"` the census already reports. The `typeof` endpoint is useful for non-canvas objects (osc~, vcf~, etc.) but does NOT solve the abstraction-name problem. The authoritative fix is the C++ census `node_class` enrichment (§2.2). See §2.3 for the fallback decision tree.

---

## 2. Target Architecture & Specifications

### 2.0 Governing Principle — C++ is the ONLY Source of Truth for Object Identity

**The C++ bridge is the authoritative, non-negotiable source of truth for ALL object identity — class names, abstraction names, positions, ports, and indices.** TypeScript must NEVER derive, guess, or synthesize identity information about an object from its own cached analysis. It reports exactly what C++ states, nothing more.

- **Every object, every time, no matter who created it** (MCP `construct_patch_v4`, or the artist's mouse in the GUI) — C++ walks `gl_list` and is the only code that knows the real class. The artist adding an object by hand is *not* an edge case; it is the *default* state the system must serve.
- **TS is a read-only mirror of the canvas.** All class-name and type data in `getAnalysis`, census, `get_selection`, `probe`, port resolution, and auto-adoption must flow FROM the C++ runtime. TS never hard-codes `"pd"` as a fallback guess — it reads the C++-emitted `node_class` from the census payload (§2.2). For non-canvas objects, TS may call `/pd/typeof` as a supplementary source, but for canvas-class objects `typeof` also returns `"pd"` — the census `node_class` field is the only reliable C++ source for abstraction names (see §1.3 caveat).
- **One object, one identity, everywhere.** Because C++ is the only truth, every MCP tool shows the same class name (`crusher.m~`) and the same resolved tempId for the same object. There is exactly one allowed divergence point — the semantic tempId label — and even that is minted from the C++-provided class name, not guessed.
- **Sync follows truth, not the other way around.** The reason the system is "in sync" after this feature is not clever reconciliation — it is that C++ stopped emitting `pd` for abstractions and now emits `crusher.m~`. TS just faithfully forwards it. If C++ ever reports something, TS trusts it; if TS ever lacks it, TS asks C++ rather than inventing.

**Concrete contract:** the C++-emitted `node_class` (or `/pd/typeof` response) IS the identity. Any future tool or code path that needs to know "what is this object" must obtain it from C++ — never from a TS-side lookup table, string guess, or cached wrapper type.

### 2.1 Single C++ Class-Name Resolution Function

Both census and selection must resolve the real abstraction name from the **same C++ helper** — so there is literally one function, one behavior, on both paths. Introduce a shared helper in the bridge:

```cpp
// MCPBridge.h / Interface.h
bool getAbstractionFileName(t_gobj* gobj, juce::String& outName);
```

**Returns:** `true` + `outName` (e.g. `"crusher.m~"`) when the object is a subpatch/abstraction instance loaded from a file (MERDA `.m~`, abstraction `.pd`, or GOP). Returns `false` for inline `[pd foo]` subpatches (which genuinely are `"pd"`).

**Implementation sketch (agent — read this to write the actual code):**

```cpp
bool getAbstractionFileName(t_gobj* gobj, juce::String& outName) {
    // Step 1: only applies to canvas-class objects (subpatches / abstractions)
    if (pd_class(&gobj->g_pd) != canvas_class) return false;

    t_canvas* sub = reinterpret_cast<t_canvas*>(gobj);

    // Step 2: distinguish loaded-file abstractions from inline subpatches.
    // canvas_isabstraction() (g_canvas.c:1310) tests gl_env != NULL — the
    // canonical Pd-internal check used by all 20+ internal call sites.
    // gl_env is allocated by canvas_new() ONLY when THISGUI->i_newdirectory
    // is non-empty (set by glob_setfilename/binbuf_evalfile before loading
    // an abstraction from disk). Inline [pd foo] subpatches have gl_env == NULL.
    if (!canvas_isabstraction(sub)) return false;

    // Step 3: gl_name holds the name/symbol the canvas was bound to.
    // For a MERDA module loaded from disk: sub->gl_name->s_name == "crusher.m~"
    //   (just the filename, NOT the full path — Pd strips the path on load)
    // For an inline [pd foo]: never reached (canvas_isabstraction returns false)
    // For a loaded .pd abstraction: sub->gl_name->s_name == "myabs.pd"
    if (!sub->gl_name) return false;

    juce::String name = juce::String::fromUTF8(sub->gl_name->s_name);

    // Strip any leading path separator or directory prefix that Pd may leave:
    int lastSlash = name.lastIndexOfChar('/');
    if (lastSlash >= 0) name = name.substring(lastSlash + 1);

    outName = name;
    return true;
}
```

**Key API references for the implementer:**

| Symbol | Where defined | What it gives you |
|---|---|---|
| `canvas_class` | Pd core (`g_canvas.h:497`, `g_canvas.c:42`) | Sentinel class for all subpatch/abstraction wrappers — `pd_class(&gobj->g_pd) == canvas_class` |
| `canvas_isabstraction(sub)` | Pd core (`g_canvas.c:1310`) | Returns `true` when `gl_env != NULL` — i.e. the canvas was loaded from a file (abstraction). Returns `false` for inline `[pd foo]` subpatches. **This is the canonical Pd-internal check.** |
| `t_canvas::gl_name` | Pd core (`g_canvas.h`) | `t_symbol*` — the name the canvas was bound to. For loaded abstractions: the filename (e.g. `"crusher.m~"`). For inline `[pd foo]`: the user-given name (`"foo"`). |
| `t_canvas::gl_env` | Pd core (`g_canvas.h`) | `t_canvasenvironment*` — non-NULL for loaded-from-disk abstractions only. Contains `ce_dir` (directory path). NULL for inline subpatches. Do NOT confuse with the nonexistent `gl_path`. |
| `class_getname(pd_class(&gobj->g_pd))` | Pd core (`m_pd.h`) | Returns `"pd"` for any canvas-class object (this is the current census behavior, unchanged) |
| `obj_ninlets(obj)` / `obj_noutlets(obj)` | Pd core (`m_pd.h`) | Inlet/outlet counts — works on abstractions via their inlet/outlet objects |

**Census integration** (see §2.2): call `getAbstractionFileName(y_obj, abstractName)` inside the census loop at `PluginProcessor.cpp:2765`. The census loop runs under `sys_lock()` — the helper is safe to call there.

**Selection telemetry integration** (already works): `Sidebar::showParameters()` (`Sidebar.cpp:445-465`) calls `object->getType(false)` which internally resolves the abstraction filename via JUCE's `Object` component. No change needed here — selection already reports the real class name. The fix is purely on the census side (making census agree with selection).

**typeof endpoint** (limited): `/pd/typeof` at `MCPBridge.cpp:429-498` returns `class_getname(pd_class(&gobj->g_pd))` — for a canvas-class object this returns `"pd"`, same as census. **Does NOT resolve abstraction filenames** (see §1.3 caveat). The TS `getTypeOf()` fallback in §2.3 therefore cannot replace C++ census `node_class` for canvas-class objects — it is only useful for non-canvas objects or as a legacy-bridge supplementary source. **Recommended enhancement:** update the `typeof` handler to call `getAbstractionFileName` for canvas-class objects, returning the real name (e.g. `"crusher.m~"`) instead of `"pd"`. This makes the endpoint uniformly useful and the TS fallback truly functional. If this enhancement is deferred, document that `typeof` returns `"pd"` for abstractions and downstream code must not rely on it for canvas-class identity.

### 2.2 Census Enrichment (`PluginProcessor.cpp` `mcp_census` handler)

In the census builder loop (`PluginProcessor.cpp:2737`), for each object capture a new field when the object is an abstraction instance:

```cpp
String resolvedType = type;
String abstractName;
if (cl == canvas_class && getAbstractionFileName(y_obj, abstractName)) {
    o->setProperty("node_class", abstractName); // NEW field — the real abstraction name
    resolvedType = abstractName;                // Option A keeps "pd" in `type` below instead
}
o->setProperty("type", resolvedType);
```

> **§2.6.2 Option A (recommended):** keep `type="pd"` and only ADD the `node_class` property. TS renders/consumes `node_class` for human-facing identity; the ~17 structural `type==="pd"` matchers keep working unchanged. The drift fix lives entirely in the A/B-group sites consuming `node_class`, not in rewiring every subpatch test. If review picks **Option B** (change `type` to the real name), see the blast-radius table in §2.6.2 for the full matcher list that must be updated in lockstep.

**Backward-compatible:** keep `"pd"` in `args[0]` (subpatch name) so existing subpatch traversal still works; add the resolved abstraction name as a new `node_class` property. `type` remains `pd` (Option A) and `args[0]` still holds `pd crusher.m~` context.

### 2.3 TS Census Enrichment (`index.ts` `getAnalysis`)

As a belt-and-suspenders (and to cover older bridges without the C++ change), enrich `pd`-typed objects in `analyzePatchFromCensus`. **Decision tree:**

1. **Census has `node_class`** (current bridge after §2.2): use it directly — zero extra OSC calls. `node_class` is the real abstraction name (e.g. `"crusher.m~"`).
2. **Census lacks `node_class`** (legacy bridge): for each object whose `type === "pd"`, call `oscClient.getTypeOf(canvas, objIndex)`.
3. **`getTypeOf` returns `"pd"`** (canvas-class limitation, see §1.3): `typeof` uses the same `class_getname(pd_class(...))` which also returns `"pd"` for canvas-class objects. Cannot resolve further on old bridges. Log a warning and leave `type` as `"pd"`.
4. **`getTypeOf` returns a non-`"pd"` class name** (non-canvas objects): store as `obj.node_class` — **do NOT overwrite `obj.type`** (§2.6.2 Option A: `type` must stay `"pd"` for the structural matchers).

- This is the single choke point — every MCP tool that calls `getAnalysis` (census, lint, snapshot, state-manager, refactor, modulation, array-tools, audit) automatically sees the correct class name.

> **Performance note:** a full census calling `getTypeOf` per `pd` object is N OSC round-trips. Mitigate via the new `node_class` field from §2.2 (C++ resolves in one pass, zero extra round-trips) and only fall back to `getTypeOf` when `node_class` is absent (legacy bridge). On the current bridge, the N-roundtrip fallback is dead code for the abstraction-name problem (step 3 returns `"pd"` anyway) — the fix lives entirely in C++ §2.2.

### 2.4 Identity Adoption Uses Real Class Name (`telemetry.ts` + `resolve-selected.ts`)

Update both auto-adopt sites to prefer the resolved class name over the census `pd`:

- **`resolve-selected.ts:53`** — ✅ ALREADY DONE (uses `sel.class` from telemetry). Verified, keep.
- **`telemetry.ts:104`** — change `currentObjClass = found.type` to fall back to the telemetry `className` when `found.type === "pd"`. This mints `plate_rev_main_90` instead of `pd_main_90`.
- **`adoptIdentity`** (`identity.ts:136`) — sanitize `.m~` names into valid tempId segments (`plate.rev.m~` → `plate_rev`). Already handled by the non-alphanumeric strip, verify.

> **⚠ Field-name inconsistency (must fix):** Two adoption paths store the class name under different keys:
> - `adoptIdentity` (`identity.ts:152`) stores `{ index, type: className, adopted: true }` — the class is in `type`.
> - Telemetry auto-adoption (`tools/telemetry.ts:118`) stores `{ index, class: currentObjClass, args, adopted: true }` — the class is in `class`.
>
> Downstream consumers (`probe.ts:77,233`, `observe.ts:153`, `typeCompatibleVoEntry`) all read `entry.type`. If an object was only auto-adopted via the telemetry path (never via `adoptIdentity`), `entry.type` is `undefined` — the sink guard silently passes (no refusal), `isProbeableSource` returns false (object skipped), and `typeCompatibleVoEntry` fails the type check (object evicted on reconcile). **Fix:** make telemetry auto-adoption also store `type` (not just `class`), OR make all downstream readers check both fields. Do this BEFORE the Group A migrations (§2.6.1).

### 2.5 `probe` / Port / Wiring Resolution (`patching.ts:316`)

When resolving inlet/outlet contracts for an object whose type is `"pd"`, call `getTypeOf()` (and `getPorts()` if available) so the agent can wire the MERDA module's real ports (`crusher.m~`: 3 signal in / 1 signal out) instead of guessing from a blank `pd` wrapper.

---

### 2.6 `monitor_signal_flow` / `probe` / `observe` — C++-Truth Sink & Type Guard

**Current state (why this is a drift vector):** the actual signal measurement is already C++-truth-driven — `probe.ts` sends `srcRef` (tempId) straight to `/meter/spectral` (or `/meter/start`), and C++ resolves the tempId via its own stable map before tapping the outlet. **The measurement is correct.**

**But the pre-flight type guard is NOT:** `probe.ts:75-77` reads the object's type from the legacy TS `identitiesPreFlight[srcRef].type` (i.e. `identities.json`, the old invention layer) to decide whether the target is a sink (`dac~`, `throw~`, `s~`, etc.) and refuse early. That is stale TS data, not C++ truth. If the tempId→type map drifts (stale `identities.json`, index recycling, or a manual MERDA object seen as `pd`), the guard misjudges:
- A stale entry could wrongly refuse a valid probe.
- A `pd`-wrapped subpatch containing `dac~` slips past the sink guard.
- Most importantly, this is the **one remaining TS-side class lookup in the monitoring path** — exactly the kind of invented identity C++-as-truth (§2.0) forbids.

**Fix:** the sink guard must resolve the target's type from C++ (`/pd/typeof` / `node_class` / the same `getAbstractionFileName` helper) instead of `identities.json`.

**Acceptance:** index drift or a stale `identities.json` can never cause a probe to target the wrong object or misjudge a sink, because every type decision in the monitoring path comes from C++.

### 2.6.1 Complete Audit — every tool that reads legacy class/type data

Broken into three groups. **Group A** reads `identities.json` directly for a type/class DECISION → must migrate to `/pd/typeof`. **Group B** reads census `analysis.objects[].type` → auto-fixed by §2.3 `node_class` enrichment, but MUST be revisited because a `pd`-wrapped MERDA/abstraction is still invisible until the `.type === "pd"` matchers migrate. **Group C** is already C++-truth (no change).

| # | Tool / action | File:line | What it reads | Verdict |
|---|---|---|---|---|
| A0 | **`syncPersistentMap` legacy reconciler — type-compatibility matching** | `identity-sync.ts:200-203,263` (`typeCompatibleVoEntry`) | compares `canonicalType(vo.type)` (live census) vs `canonicalType(entry.type)` (stored `identities.json`) | **CRITICAL MIGRATE — after §2.3 census reports `crusher.m~` but stored entries say `pd` → every manual MERDA object becomes "type-incompatible" → rebound/evicted from its semantic identity on every reconcile.** Must compare `node_class` on BOTH sides (or accept stored entry when live `node_class` is a real abstraction name). This is the reconciliation layer everything else trusts — fix it FIRST. |
| A1 | `probe` SINK GUARD (refuses `dac~`/`throw~`/`s~` sinks) | `probe.ts:75-87` | `identities.json[srcRef].type` | **Migrate → `getTypeOf`** |
| A2 | `probe_control_msg` TYPE GUARD (refuses sinks + audio objects) | `probe.ts:230-246` | `identities.json[target].type` | **Migrate → `getTypeOf`** (NEW FIND) |
| A3 | `probe` fallback legacy capture rig (target type) | `probe.ts:231-233` | `identities.json[srcRef].type` | **Migrate → `getTypeOf`** |
| A4 | `observe` auto-probe-target picker (`isProbeableSource`: picks any `*~` not in NON_PROBEABLE) | `observe.ts:153-178` | `identities.json[id].type.endsWith("~")` | **Migrate → `getTypeOf`** (NEW FIND — a MERDA `*~.m~` seen as `pd` is skipped as "not probeable") |
| A5 | `transaction-v2.ts:3044` / `transaction.ts` acoustic auto-fix probes | `transaction-v2.ts:3044` | probe by tempId (C++ resolves) | Audit only — confirm guard path |
| A6 | **Telemetry auto-adoption stores `class` not `type` — all Group A/B consumers read `entry.type`** | `telemetry.ts:118` stores `class`; `probe.ts:77,233`, `observe.ts:153`, `identity-sync.ts:263` read `entry.type` | `identities.json` field mismatch | **CRITICAL — fix BEFORE other migrations.** Telemetry auto-adoption (`{ index, class: ..., args, adopted }`) never writes `entry.type`, so all downstream readers get `undefined`. Sink guard silently passes, `isProbeableSource` returns false, `typeCompatibleVoEntry` fails type match. Fix: store `type` in telemetry auto-adoption too (align with `adoptIdentity` shape). |
| B1 | `acoustics` preflight `hasDac` + dac~/tap discovery | `acoustics.ts:105-148,317,344` | census `analysis.objects.type` | Auto-fixed by §2.3; reopen `o.type === "dac~"` if MERDA internals matter |
| B2 | `acoustic_gate` dac detection | `acoustic-gate.ts:101` | census `analysis.objects.type` | Auto-fixed by §2.3 |
| B3 | `monitor_signal_flow` analyze/tap discovery | `observe.ts:116-165`, `acoustics.ts:308-363` | census `analysis.objects.type` | Auto-fixed by §2.3 |
| B4 | `lint` signal-vs-control checks | `lint.ts:27-89` | census `analysis.objects.type` | Auto-fixed by §2.3 |
| B5 | `error_report` drift detection | `patching.ts:339-341` (`obj.type !== entry.type`) | census type vs `identities.json` type | Migrate both sides to `node_class` comparison — NEW FIND |
| B6 | `modulation` subpatch lookup (`obj.type === "pd"` & args[0]===`modulation_map`) | `modulation.ts:325,458,569,600` | census `analysis.objects.type` | **Migrate `.type==="pd"` matchers to also match `node_class`** (NEW FIND). Note: after §2.2, `args[0]` for a MERDA module is its filename (`crusher.m~`), not a user-given subpatch name — the semantic meaning of `args[0]` changes for canvas-class objects. These matchers search by subpatch *name* (`args[0]`), which is correct for inline `[pd modulation_map]` but could theoretically false-positive if a MERDA module happened to be named `modulation_map.m~`. Guard with `!node_class` or `node_class === undefined` to restrict matching to true inline subpatches. |
| B7 | `refactor` subpatch lookup (`obj.type === "pd"` & args[0]===name) | `refactor.ts:216,254` | census `analysis.objects.type` | **Migrate `.type==="pd"` matchers to also match `node_class`** (NEW FIND). Same `args[0]` semantic-shift note as B6 — restrict to inline subpatches when `node_class` is present. |
| B8 | `isSignalObject` / `isSignalOutlet` / `isSignalInlet` — signal-rate detection used by audit/layout/dagre | `patch-analyzer.ts:164-210` | `type === "pd"` → relies on `subpatchSummary`; else `type.endsWith("~")` | **Must add `node_class` check.** After §2.2, a MERDA module like `crusher.m~` has `type === "pd"` but `node_class === "crusher.m~"`. The current `type === "pd"` branch returns `false` (non-signal) unless `subpatchSummary` exists — incorrectly classifying MERDA signal-rate modules as non-signal. **Fix:** when `type === "pd"`, check `(obj as any).node_class` — if it ends with `"~"` or `.m~`, treat as signal. Also handle MERDA modules without `~` suffix (e.g. mixer) by checking `node_class` presence as a secondary heuristic. |
| B9 | `delete` subpatch prune (`objData.type === "pd" || "clone"` → clears sub-canvas identities) | `transaction-v2.ts:1173-1181` | census `analysis.objects.type` | Audit — after node_class, deleting a MERDA module must still prune DOWNSTREAM named sub-canvases if any; `.type==="pd"` matcher must also match `node_class` containing `*~`/abstraction. (NEW FIND) |
| C1 | `inspect_object` fast path | `patching.ts:297-309` | `ctx.oscClient.getTypeOf` + `getPorts` | Already C++-truth ✅ |
| C2 | native probe spectral/meter measurement | `probe.ts:93-144` | sends tempId → `/meter/spectral`, C++ resolves | Already C++-truth ✅ |
| C3 | `get_selection` `classified` | `telemetry.ts` (get_selection) | `/pd/ui/selection` telemetry className | Already C++-truth ✅ |
| C4 | **`syncIdentitiesFromSnapshot` Phase 5 fast path** | `identity-sync.ts:48-72` | C++ `identity_snapshot` → `entry.class` (first token of text) | Already C++-truth ✅ (this is the path that avoids A0 entirely when the bridge supports `identity_snapshot` — verify `entry.class` carries real name) |

**Decision rule applied:** any code path that uses `.type` to (a) refuse a target, (b) pick a probe target, or (c) detect a named subpatch, and obtains `.type` from `identities.json` → must read `getTypeOf` / `node_class`. Census-driven checks (`B1-B7`) get the correct class automatically from §2.3, but the `.type === "pd"` subpatch matchers must also match `node_class` (a MERDA module's `node_class` is its real name, not `pd`).

**Phase 2 note:** `identities.json` group-A reads disappear entirely once Phase 2 makes the TS identity store a write-through C++ cache.

### 2.6.2 Blast Radius — `type` vs `node_class` (design decision REQUIRED)

**Confirmed C++ fork point:** `PluginProcessor.cpp:2765-2771` — `if (cl == canvas_class)` sets `kind="canvas"`, `type="pd"`, and `args = [sub->gl_name->s_name]`. For a MERDA abstraction, `sub->gl_name` holds the real file name (e.g. `crusher.m~`), and `canvas_isabstraction(sub)` returns `true` (because `gl_env != NULL`) — so **the real name is available at the same spot that fills `args`**, and the abstraction-vs-inline distinction is a one-line check. The C++ emit is a ~5-line change (add `getAbstractionFileName` helper + `node_class` property in the census loop).

**The trap:** dozens of TS code paths use `type === "pd"` as their *structural* test for "this is a subpatch/abstraction wrapper". If Phase 1 changes `type` from `"pd"` to `"crusher.m~"`, ALL of these silently stop treating MERDA modules as wrappers:

| Consumer | Use of `type === "pd"` |
|---|---|
| `transaction-v2.ts:1831-1853` | port expansion for subpatch connections (`srcOut`/`destIn` semantics) |
| `transaction-v2.ts:441` | skip-flattening wrappers with `subpatchSummary` |
| `canvas-reconstruction.ts:84,99,152,453` | recursion into subpatch internals for reconstruction |
| `gop-module.ts:1001,1011` | abstraction-file detection (`.pd` arg) |
| `hearing-guard.ts:52`, `validation.ts:491,518`, `rate-mismatch.ts:29` | subpatch bypass / rate rules |
| `rollback-manager.ts:204` | safe-recursion guard |
| `refactor.ts:216,254,259`, `modulation.ts:367` | find/create `[pd name]` wrappers |

**Decision (must be settled in review, NOT left implicit):**

- **Option A — keep `type="pd"`, ADD `node_class`:** Census keeps emitting `type="pd"` + `args=[name]` (structural, backward-compatible), and adds `node_class="crusher.m~"`. TS `getAnalysis` enriches `className` from `node_class`. All existing `type==="pd"` matchers keep working for their *structural* purpose (subpatch traversal, port expansion, GOP detection); only the ~10 drift-vector sites (§2.6.1 A/B groups) switch to preferring `node_class` for *human-facing/identity* purposes. **However, Option A is NOT zero-touch** — B6/B7 subpatch matchers must still be updated (a MERDA module's `args[0]` is now its filename, not a user-given name — the semantic meaning shifts), and B8 `isSignalObject` must check `node_class` when `type === "pd"` to correctly classify MERDA signal-rate modules. **Lowest risk; acceptance #1 changes to "census reports `type:"pd", node_class:"crusher.m~"`, and `analyze_patch` renders `crusher.m~`".**
- **Option B — change `type` to the real name:** full "one object, one class everywhere" purity, but requires updating **all ~17 `type==="pd"` matcher sites above** to `(type==="pd" || type===node_class)` and risks missing one. Only justified if the artist prioritizes seeing real names in *every* structural API over stability.

**Recommendation: Option A for the immediate Phase-1 fix** (the drift is fixed by `node_class` consumption in the A-group sites; `type` stays stable). Option B is the cleaner long-term shape and naturally pairs with the §8 Phase-2 retire-the-invention-layer work — revisit there.

### 2.7 The Identity Version Gap — GUI Edits Don't Bump `mcpIdentityVersion`

**An agent MUST understand this gap to reason about when identity is stale and why the fix still works.**

**Fact:** `mcpIdentityVersion` only increments inside C++ MCP *OSC handler arms* — `PluginProcessor.cpp` lines 2215, 2302, 2381, 2431, 2538, 2577, 2663. These fire when the TS MCP server sends a mutation via OSC (`mcp_batch_atomic`, `mcp_create`, `mcp_connect`, etc.). **Manual GUI edits (the artist dragging `crusher.m~` into the canvas by hand) never trigger these handlers → version stays unchanged.**

**Consequence — the stale window:**

```
Artist drags crusher.m~ into canvas (GUI)
  ↓
mcpIdentityVersion unchanged → syncIdentitiesFromSnapshot returns CACHED map
  ↓
Next MCP tool call (e.g. analyze_patch) → census walks live gl_list (FRESH)
  BUT identity map is still the cached version (no entry for crusher.m~)
  ↓
analyzePatchFromCensus auto-adopts the object → mints pd_main_N from type "pd"
```

**Why this is NOT a correctness problem (after the fix):**

1. **Census is always live** — `getAnalysis` in `index.ts:407-455` calls C++ `mcp_census` which walks `gl_list` fresh. The object IS seen.
2. **node_class is emitted in census** (§2.2) — `crusher.m~` appears as `node_class:"crusher.m~"` in the census JSON, not `type:"pd"`.
3. **Auto-adopt reads `node_class`** (§2.4) — `analyzePatchFromCensus` mints the tempId from the real class name (`crusher_main_N`), not from `pd`.
4. **The identity map is refreshed on the next MCP mutation** — PHASE 0 inside `batch_atomic` (the next time any tool creates/connects/edits an object) walks `gl_list`, adopts the GUI-created object into `mcpStableObjectMap`, and bumps `mcpIdentityVersion`. The snapshot endpoint then returns the updated map.

**Practical impact:** there is a brief window (GUI edit → next MCP tool call) where the identity map is stale. During this window, `$selected` and `get_selection` still work (they read live telemetry, not the identity map), but `probe`/`analyze` by name will auto-adopt on first access. **No data is lost; the real class name is emitted by census on every call.**

**Optional improvement (Phase 2):** add a C++ notification hook that bumps `mcpIdentityVersion` on any GUI canvas edit (not just MCP mutations). This eliminates the stale window entirely. Small ~10-line C++ sidecar fix. Not required for Phase 1 correctness.

---

## 3. In-Scope / Out-of-Scope

**In-scope:**
- C++ `getAbstractionFileName` helper (`canvas_isabstraction` + `gl_name`) + census `node_class` enrichment.
- C++ `typeof` endpoint enhancement: call `getAbstractionFileName` for canvas-class objects so `/pd/typeof` returns the real name instead of `"pd"` (makes the TS fallback truly functional).
- TS `getAnalysis` type enrichment (with `node_class` fast path + `getTypeOf` fallback decision tree per §2.3).
- **Telemetry auto-adoption `type` field fix** (A6): store `type` alongside `class` in `tools/telemetry.ts:118` so downstream readers (`probe.ts`, `observe.ts`, `typeCompatibleVoEntry`) don't get `undefined`. Fix FIRST.
- `telemetry.ts:104` adoption uses telemetry `className` and `node_class`.
- `probe`/port resolution calls `getTypeOf` for `pd` objects.
- `monitor_signal_flow`/`probe`/`observe` sink-guard & type-guard reads C++ truth (not `identities.json`) — §2.6/§2.6.1 Groups A1-A5.
- **`syncPersistentMap` legacy reconciler compares `node_class` on both sides so manual MERDA objects don't get rebound/evicted** — §2.6.1 Group A0.
- `modulation.ts`/`refactor.ts` `.type === "pd"` matchers also match `node_class` AND guard with `!node_class` for inline-subpatch-only semantics — §2.6.1 Group B6-B7.
- `isSignalObject` `node_class` check for MERDA signal-rate detection — §2.6.1 Group B8.
- `error_report` drift check compares `node_class` on both sides — §2.6.1 Group B5.

**Out-of-scope:**
- Deeper port-contract introspection (e.g. mapping `crusher.m~` knob symbol names). Use `lookup_pd_library(inspect_docs)` for that.
- Auto-renaming to a canonical `merda_*` prefix — user or agent may `rename` as desired.
- Behavioral change to `pd`-type detection for inline `[pd foo]` subpatches (still reported as `pd foo`).

---

## 4. Acceptance Criteria

1. Add `crusher.m~` manually in GUI → `analyze_patch(action:'census')` reports class `crusher.m~` (via `node_class`; `type` stays `pd` for structural backward-compat per §2.6.2 Option A).
2. Select `plate.rev.m~` → `get_selection` returns `class plate.rev.m~` AND a readable tempId `plate_rev_main_N`.
3. `probe` targets a MERDA module by its real class name and resolves its correct inlet/outlet counts (3-in/1-out for `crusher.m~`).
4. Inline `[pd foo]` subpatches still report `pd` + `foo` (unchanged).
5. No performance regression: enriched census completes in <10ms for a 100-object patch (C++ one-pass, no N-roundtrip fallback on current bridge).
6. Manual wire additions to the MERDA module are still discovered by PHASE 0 reconcile and probe correctly.
7. `manage_modulation(list_mappings / remove_mapping)` and `refactor_patch` still find `[pd modulation_map]` / named subpatches after census reports the real abstraction name for MERDA modules.
8. `error_report` no longer flags "drifted" on a manual MERDA object when `identities.json` type (`pd`) and census (`crusher.m~`) legitimately differ.
9. **Reconcile stability:** after 5 consecutive `analyze_patch(census)` calls (no mutations between), a manually-added `crusher.m~` keeps its same tempId every time (no rebind/evict churn from A0).

---

## 5. Effort Estimate & Task Breakdown

| # | Task | Repo / File | Effort |
|---|---|---|---|
| 0 | **Telemetry auto-adoption `type` field fix** (A6 — do FIRST) | `mcp-server/src/tools/telemetry.ts` | S |
| 1 | `getAbstractionFileName` C++ helper (`canvas_isabstraction` + `gl_name`) | `plugdata-core/Source/Pd` | S |
| 2 | Census `node_class` enrichment | `PluginProcessor.cpp` `mcp_census` | S |
| 3 | TS `getAnalysis` type enrichment (`CensusData` interface + `analyzePatchFromCensus`) | `mcp-server/src/analysis/patch-analyzer.ts`, `mcp-server/src/index.ts` | M |
| 4 | `telemetry.ts:104` adoption className fallback | `mcp-server/src/tools/telemetry.ts` | S |
| 5 | All Group A guards (`probe`/`probe_control_msg`/`observe`/legacy-rig) read `getTypeOf` | `mcp-server/src/tools/probe.ts`, `observe.ts`, `acoustics.ts` | M |
| 5b | Group B subpatch matchers: `.type==="pd"` also matches `node_class`; `isSignalObject` `node_class` check (B8); `error_report` `node_class` dual compare | `mcp-server/src/tools/modulation.ts`, `refactor.ts`, `patching.ts`, `analysis/patch-analyzer.ts` | M |
| 5c | **A0: `syncPersistentMap` reconciler accepts real `node_class` on live side (rebind guard)** | `mcp-server/src/tools/patching/identity-sync.ts` | M |
| 6 | Tests (census/lint/probe regression, sink-guard drift, subpatch lookup by `node_class`) | `mcp-server/src/tools/__tests__` | M |

**Total:** ~2 developer-days (revised up from ~1.5 to account for: A6 field-name fix, `isSignalObject` B8 explicit fix, `typeof` endpoint enhancement recommendation, B6/B7 `args[0]` semantic-shift guard).

---

## 6. Verification Protocol

Per the standard test protocol (AGENTS.md):
1. Rebuild standard 9-voice patch (`construct_patch_v4 clear:true`).
2. Start 128 BPM beat.
3. Manually add a MERDA module in GUI.
4. `analyze_patch(census)` → confirm real class name, no dropout.
5. Select it → `get_selection` → confirm className + readable tempId.
6. `tap_audio_signal` → > -20 dBFS, `read_pd_console` → zero errors.
7. Confirm tempIds stable (no unexpected `gui_*` / `pd_main_*` for the new object).

---

## 8. Phase 2 (Follow-up PRD): Retire the TS Identity-Invention Layer

### 8.1 Rationale
The pre-C++-bridge identity system (`identity.ts` `adoptIdentity`, `identities.json`, TS-side index↔tempId maps) was designed when **C++ had no identity awareness** — TS had to *invent* names from census `pd`-wrapped types and persist them to disk. Now that C++ owns `mcpStableObjectMap`, `mcpIdentityVersion`, and PHASE 0 auto-reconcile — and walks `gl_list` knowing the real class of every object — the TS invention layer is **redundant** and is the very source of the `pd_main_90` drift. This Phase 1 PRD fixes the symptom; Phase 2 inverts the authority.

### 8.2 Governing Shift
- **C++ owns** object identity, real class names, stable tempIds, version counter, and reconcile.
- **TS is a read-only mirror**: it forwards C++ truth (`node_class`, `/pd/typeof`, stable IDs) and only formats human-friendly label requests. It never guesses a class or mints identity from its own cache.

### 8.3 Proposed Changes
| TS identity piece | Action | Why |
|---|---|---|
| `adoptIdentity` (`identity.ts:136`) | **Demote to compat shim** — only for legacy bridge / offline fallback | C++ `resolveStableId` + PHASE 0 is the primary adapter |
| `identities.json` | **Keep, but write-through from C++ snapshots** | C++ RAM map doesn't survive restart; disk must be a cache of C++ truth, not a TS decision layer |
| TS index↔tempId maps | **Phase out** | Replaced by C++ `resolveStableId` / stable-ID in census (`node_class` + `id` already emitted) |
| `getAnalysis` census enrichment | **Keep** | Mirror that forwards C++ `node_class` (no TS-side class logic) |
| All TS class-name guessing / `"pd"` fallback | **Remove** | Single C++ `getAbstractionFileName` is the only source |

### 8.4 Acceptance (Phase 2)
1. Removing the TS adoption path causes **zero** regressions against the current bridge (all identity resolvable via C++ stable IDs).
2. `identities.json` regenerates exactly from a C++ `identity_snapshot` (drop → reload → identical).
3. No TS file contains a hard-coded `"pd"` class-name fallback or home-grown class resolution.
4. Artist hand-added objects and v4-created objects are indistinguishable to the tooling (same resolution path, same truth).

### 8.5 Effort (Phase 2)
Large refactor across `identity.ts`, `telemetry.ts`, `resolve-selected.ts`, `transaction-*.ts`, and analysis — separate PRD + test plan required. Phase 1 must ship first to establish `node_class` as the contract Phase 2 builds on.

---

## 9. Open Questions

1. Should the tempId for a MERDA module auto-prefix with `merda_` (e.g. `merda_crusher`) for clarity, or keep the class-derived `crusher_main_N`?
   **RESOLVED (this PRD):** `getAbstractionFileName` returns the real class name as-is (`crusher.m~`). `adoptIdentity` sanitizes it to `crusher`. The tempId becomes `crusher_main_N`. No `merda_` prefix — class names are already distinctive enough and an agent can read `crusher` directly.

2. Should `node_class` also be surfaced for GOP panels containing arbitrary subpatches, or only `.m~`/`.pd` abstraction file loads?
   **RESOLVED (this PRD):** `node_class` is surfaced for **all loaded-from-disk abstractions** — `.m~` MERDA modules, `.pd` abstractions, and GOP panels that loaded a file. It is **not** surfaced for inline `[pd foo]` subpatches (which have no file). The decision logic is `getAbstractionFileName` returning `true` (file was loaded) vs `false` (inline). This keeps the census path simple and uniform — there is no need to special-case MERDA vs other abstractions at the census level. The class name (`crusher.m~` vs `myabstraction.pd`) already tells the agent what it is.

3. Do we need a `getPorts`-style BPF for MERDA knob symbol names in future, or is `lookup_pd_library` sufficient for now?
   **RESOLVED:** `lookup_pd_library(inspect_docs)` is sufficient for Phase 1. MERDA knob introspection is out of scope (§3).
