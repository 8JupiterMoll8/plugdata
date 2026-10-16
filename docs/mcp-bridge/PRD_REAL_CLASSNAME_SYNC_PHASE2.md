# PRD: Phase 2 — Retire the TS Identity-Invention Layer (C++ Owns Identity)

**Document Version:** 0.1.0 (DRAFT)
**Target Repos:** `plugdata-core` (C++ bridge) & `PlugData-MCP-Server` (TS)
**Depends on:** Phase 1 (`PRD_REAL_CLASSNAME_SYNC.md`) — **SHIPPED & LIVE-VERIFIED 2026-09-02**
**Author:** Jupiter Moll & Antigravity
**Status:** PROPOSED

---

## 1. Why Phase 2, Now

Phase 1 made class-name identity C++-truth (`node_class`, enhanced `typeof`) and migrated every
type/identity DECISION to C++ sources. Live verification exposed the remaining architecture debt:

1. **TS still mints tempIds.** `adoptIdentity` / telemetry adoption invents `flanger_main_34`,
   then (post-fix) registers it into C++ via `/pd/register_id`. The *naming* is still a TS decision —
   backwards: C++ is the truth, but TS authors the truth.
2. **Two maps, two directions.** `mcpStableObjectMap` (C++, RAM) and `identities.json` (TS, disk)
   co-exist. Census fills `tempId` from C++ `id` first and falls back to the TS map — the mirror
   ordering is right, but both stores can disagree during the adoption window.
3. **The §2.7 stale window still exists.** Hand-add object → census shows `id:"none"` → TS adopts →
   registers. Between GUI-add and first MCP adoption, the C++ map has no entry (probe raced this
   live: "Object not found" until registration landed).
4. **~20 TS files read `identities.json`** for tempId→index lookup. Each reader is a place where TS
   disagrees with C++ under drift.

**Phase 2 goal (PRD §8.2):** C++ mints, owns, and persists identity. TS is a read-only mirror.
`identities.json` becomes a write-through cache of C++ truth — never a decision layer.

---

## 2. Design

### 2.1 C++: Native Auto-Adoption with Readable Names (the keystone)

Move TS's naming policy INTO the bridge. In the census walk / PHASE 0 reconcile, for every
canvas-class or regular object **not present in `mcpStableObjectMap`**:

```
tempId = sanitize(classResolution(gobj)) + "_" + canvasKey + "_" + index
```

- `classResolution`: reuse the Phase 1 helper — `getAbstractionFileName` for abstractions
  (`flanger.m~`), `class_getname` otherwise (`osc~`, `keyboard`).
- `sanitize`: C++ port of the TS `sanitizeTempIdSegment` (strip `.m~`/`.pd` extension → basename,
  non-alnum → `_`, trim `_`, `~` suffix → `_sig` for signal objects). Must produce IDENTICAL
  output to the TS version (test both sides against the same cases).
- Register into `mcpStableObjectMap`, bump `mcpIdentityVersion`. **Never overwrite** an existing
  entry (artist renames are sacred) and never mint for ephemeral rig objects.

**Consequence:** census ALWAYS emits a non-empty `id`. The adoption window (§2.7) disappears
entirely — a hand-added object is named, readable, and probeable on the FIRST census after creation.
`get_selection` for unregistered objects becomes a pure read.

**C++ census currently reports `type:"pd"` + `node_class` (Option A).** Phase 2 additionally emits
`tempId` natively, so the TS adoption branch in `telemetry.ts` / `analyzePatchFromCensus` /
`resolve-selected.ts` is dead code on current bridges.

### 2.2 C++: `rename_id` Reply (known gap, cheap)

AGENTS.md lists it: `/pd/rename_id` has no reply → TS waits a blind 20ms. Add the reply
(`/pd/mcp_rename_id/reply/<corr>` with success flag). Enables race-free renames and retires the
last TS-side rename bookkeeping (`renameIdentity` in identity.ts becomes cache-only).

### 2.3 TS: `identities.json` → Write-Through Cache

- **Write path:** after every `getAnalysis`, mirror the census result
  (`{ [obj.id]: { index, type: node_class||type, args } }`) into the persistent store. No TS-side
  adoption, no counters, no drift heuristics.
- **Read path:** ONLY as fallback when C++ is unreachable (legacy bridge / offline). Every reader
  first consults the live analysis (`objects.find(o => o.tempId === t)`); the store is consulted
  only when the analysis cache is cold and the bridge is down.
- **Restart semantics:** C++ map is RAM-only. On PlugData restart the canvas is empty → both stores
  reset naturally. On canvas `load`, PHASE 0 re-adopts from scratch with C++ names — no TS restore.

### 2.4 TS: Migrate the ~20 Reader Files (mechanical)

| Class of reader | Migration |
|---|---|
| tempId→index lookups (array-tools, modulation, control, checkpoints, file-io, gop-module) | `analysis.objects.find(o => o.tempId === t)?.id` (census `id` = C++ truth) |
| Adoption sites (telemetry `get_selection`, `resolve-selected.ts`, `identity-sync.ts`) | Delete entirely on bridges with `census` + auto-adopt cap; keep a thin legacy shim gated on `hasCap("census") === false` |
| Reconcilers (`syncPersistentMap`, `typeCompatibleVoEntry` machinery, pendingReconcile/eviction) | Deleted for modern bridges — C++ PHASE 0 + auto-adopt is the reconciler. Legacy shim keeps the old code path. |
| `adoptIdentity` / `renameIdentity` (identity.ts) | Compat shim, legacy-only |
| Class-name fallbacks (`|| "pd"`, sanitizer usage outside legacy shim) | Removed (§8.4 acceptance 3) |

### 2.5 Undo/Redo Interplay (risk area)

`undo-v4.ts` topology remap currently re-anchors `gui_*` ids after undo. With C++ auto-adoption:
- C++ PHASE 0 must **re-adopt post-undo objects with their previous C++-minted names when the
  topology match is unambiguous** (serial map survives; else mint fresh).
- The TS remap layer becomes redundant on modern bridges — gate it off, keep for legacy.

### 2.6 Bridge Capability Gate

Advertise `auto_adopt` in the C++ caps list. TS checks `hasCap("auto_adopt")`:
- **true** → new path (mirror-only, zero adoption code).
- **false** → legacy path (current behavior, including Phase 1 fixes). One session, two bridges,
  zero breakage.

---

## 3. In Scope / Out of Scope

**In:** §2.1 auto-adopt naming (C++), §2.2 rename reply, §2.3 write-through store, §2.4 reader
migration + shim gating, §2.5 undo re-adoption, caps gate, tests for all of it.
**Out:** deeper port introspection for MERDA knobs (unchanged), multi-document canvas maps beyond
the existing per-canvas keys, offline editing (no bridge = read-only tools only).

---

## 4. Acceptance Criteria

1. Hand-add any object → FIRST `analyze_patch(census)` already shows readable `tempId`
   (`flanger_main_34`), C++-minted; `probe` on it works with **zero** TS involvement.
2. `identities.json` deleted from disk → every tool works identically (store regenerates from
   census as pure mirror).
3. Rename an object → census + probe + snapshot all reflect the new name after one mutation
   (no 20ms settle needed).
4. Undo a create → the recreated object keeps a readable C++-minted name; no `gui_*` visible in
   census `id` on the modern path.
5. Legacy bridge (caps without `auto_adopt`) → behavior identical to post-Phase-1 today.
6. Zero regression across the full Phase-1 live battery (§6 protocol + 5×census stability).
7. `grep` proof: no TS file contains a class-name fallback (`"pd"` literal as identity guess)
   outside the legacy shim module.

---

## 5. Task Breakdown (est. ~2 developer-days)

| # | Task | File(s) | Effort |
|---|---|---|---|
| 1 | C++ `sanitizeClassName` port + auto-adopt in census/PHASE 0 (never overwrite) | `MCPBridge.h/.cpp`, `PluginProcessor.cpp` | M |
| 2 | `auto_adopt` caps entry | `MCPBridge.cpp` caps | S |
| 3 | `rename_id` reply | `PluginProcessor.cpp` handler | S |
| 4 | Undo re-adoption of C++-minted names | `PluginProcessor.cpp` (PHASE 0 post-undo), `undo-v4.ts` gating | M |
| 5 | TS write-through mirror in `getAnalysis` | `index.ts` | S |
| 6 | Delete/gate TS adoption (telemetry, resolve-selected, identity-sync, adoptIdentity shim) | 4 files | M |
| 7 | Migrate tempId→index readers to census-`id` lookups | ~10 files | M |
| 8 | Legacy shim module (`legacy-identity.ts`) + `hasCap` gates | new + call sites | M |
| 9 | Parity tests (TS↔C++ sanitize), restart test, undo test, live §6 battery | `__tests__` + live | M |

## 6. Verification Protocol

Same as Phase 1 §6, plus: delete `identities.json` mid-session → census/probe/rename/undo all
still correct; restart PlugData → re-add object → first census carries C++ tempId.
