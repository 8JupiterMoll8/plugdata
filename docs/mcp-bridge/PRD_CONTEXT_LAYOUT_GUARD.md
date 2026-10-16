# PRD — Context-Aware Layout Guard (C++)

**Status:** proposed
**Owner:** MCP bridge (C++) + TS glue
**Depends on:** `8cf16e90d` (batch_atomic engine), the `/pd/layout` X-ray, the preflight/post-guard infrastructure
**One-liner:** Make layout a **guarantee in C++** — after any mutation, compute object **clusters** (context) and auto-fix **collisions / wire-occlusions** inline. Fast, free, and impossible for the AI to forget.

---

## 1. Motivation

Today the only *automatic* layout safety is a **single-object collision nudge** inside
`construct_patch_v4(move)`. Everything else is AI-driven and fragile:

| Need | Today | Cost |
|---|---|---|
| "Is the whole canvas overlapping?" | AI must call `analyze_patch(layout)` | 1 round-trip + tokens |
| "Fix it" | AI must decide + call `layout_patch`/`move` | 1 round-trip + tokens |
| "What belongs to what (voices/groups)?" | **not available** | — |
| Local collision | `move` auto-nudge (1 object) | free |

So correctness depends on the AI **remembering** — and it can forget. The bridge already
walks the graph for `diagnose` (cycles/dangling) and `layout` (bounds/collisions/occlusions)
in microseconds. **Grouping is the same kind of walk.** Move the guarantee out of the AI's
head and into C++.

---

## 2. Goals / Non-goals

**Goals**
- **G1 — Cluster fact:** expose `clusters` (connected components + kind) in the layout X-ray.
- **G2 — Auto-sanitize:** after any mutation, inline-fix collisions (and optionally occlusions).
- **G3 — Fast & free:** < 1 ms, no AI, no network round-trip, no tokens.
- **G4 — Minimal motion:** only move what is broken; preserve the AI's arrangement.

**Non-goals**
- Not a stylist — never imposes a house layout.
- Not a replacement for `layout_patch` algorithms (`dagre`/`flow`/`pillars`).
- Not a replacement for taste (`user_prefs`) or archetypes (skills).

---

## 3. Design

### 3.1 Cluster fact (context)
Compute **connected components** of the wire graph and classify each:

- **`signal`** — a chain with ≥1 signal source (`osc~`, `noise~`, `phasor~`, `adc~`, …) and/or
  terminating at `throw~`/`dac~`.
- **`bus`** — contains `catch~`/`dac~` (the master mix).
- **`control`** — no signal objects (messages, numbers, LFOs feeding inlets).
- **`unknown`** — conservative fallback.

Emitted shape:
```json
"clusters": [
  { "id": "c1", "kind": "signal",  "objects": ["osc","vca","shaper"] },
  { "id": "c2", "kind": "bus",     "objects": ["catch","dac"] },
  { "id": "c3", "kind": "control", "objects": ["hsl","sig_gain"] }
]
```

### 3.2 Auto-sanitize (post-guard)
After a mutation completes, in C++:

1. **Collision scan** — reuse the existing true-rect AABB scan (`/pd/collisions`).
2. If collisions → run the existing **`deoverlap`** (minimal push-apart).
3. If wire occlusions → optionally run **`tidy`** (configurable; default off to avoid
   over-moving).
4. Report what changed.

**Opt-out:** `autoLayout:false` per call (and a bridge/canvas setting for a global off).

---

## 4. Implementation

### 4.1 Files touched
| File | Change |
|---|---|
| `Source/Pd/MCPBridge.cpp` | add `computeClusters(...)`; add `sanitizeLayout(...)`; wire into `/pd/layout` X-ray + `batch_atomic` post-step |
| `Source/Pd/Interface.h` | (optional) shared graph-traversal helper |
| `mcp-server/src/tools/patching/transaction-v4.ts` | pass `autoLayout` through; surface `clusters` + `layoutSanitized` in `_v2meta` |
| `mcp-server/src/tools/layout-patch.ts` | surface `clusters` from the layout action |

### 4.2 `computeClusters(processor, cnv, canvasName)`
```cpp
// O(objects + wires). Union-find over gl_list connections.
std::vector<int> parent;                 // union-find
for (t_gobj* g = cnv->gl_list; g; g = g->g_next) parent.push_back(idx++);
// walk linetraverser: union(src, dst)
// classify each root by scanning member nodeClass:
//   has source~ && (ends dac~/throw~)  -> signal
//   has catch~/dac~                     -> bus
//   no signal objects                   -> control
//   else                                -> unknown
```
Returns the `clusters` JSON fragment. Reuse `mcpStableObjectMap` for tempIds (same as census).

### 4.3 Layout X-ray integration
In the `/pd/layout` handler (the `analyze_patch(action:'layout')` backend), append
`"clusters": computeClusters(...)` to the JSON already returned (`layoutFacts`).

### 4.4 `batch_atomic` post-step
After the mutation lambda and **before** building the reply:
```cpp
bool autoLayout = /* parsed from the call, default true */;
if (autoLayout) {
    auto col = detectCollisions(cnv);              // existing true-rect scan
    if (!col.empty()) {
        int moved = deoverlap(cnv, /*preserveArrangement=*/true); // existing algo
        layoutSanitized.collisionsFixed = moved;
    }
}
// append layoutSanitized to the reply tail (backward-compatible, like undoReset)
```
Also run the same step after `/pd/clear` and `/pd/load_patch` (they replace objects).

### 4.5 Threading
Positions are geometry-only (no DSP impact). Run the sanitize on the **same execution
context** as the mutation:
- GUI/control batches (message thread) → inline, after the mutation.
- Signal-only batches (audio thread) → hop to the message thread for the geometry pass
  (the existing `synchroniseCanvases` hop), OR compute+apply under `sys_lock` since it only
  writes `te_xpix/te_ypix`.

### 4.6 Report in `_v2meta`
```json
"_v2meta": {
  "layoutSanitized": { "collisionsFixed": 2, "occlusionsFixed": 0, "moved": ["a","b"] },
  "clusters": [ ... ]
}
```
If nothing was broken → `"layoutSanitized": null` (zero movement, zero noise).

---

## 5. Performance budget
| Step | Cost | Notes |
|---|---|---|
| Cluster walk | O(n + e) | < 50 µs @ 100 objects |
| Collision scan | O(n²) AABB | switch to spatial hash above ~200 objects |
| Deoverlap | existing cost | minimal push-apart |
| **Total target** | **< 1 ms** | typical patches |

---

## 6. API / output (example)
```json
{
  "layoutFacts": {
    "bounds": [...],
    "collisions": [],
    "wireOcclusions": [],
    "clusters": [
      { "id": "c1", "kind": "signal",  "objects": ["smoke_osc","smoke_vca","wavefold_shaper","gen_out"] },
      { "id": "c2", "kind": "bus",     "objects": ["smoke_dac"] },
      { "id": "c3", "kind": "control", "objects": ["ctrl_freq","ctrl_gain","sig_gain"] }
    ]
  }
}
```

---

## 7. Testing
Add `scripts/layout-guard-gauntlet.ts` (live, port 19010, MCP server stopped):
1. **Overlap → auto-fixed:** build two objects at the same x/y → assert `layoutSanitized.collisionsFixed > 0` and `analyze_patch(layout).collisions == []`.
2. **Clean → no-op:** build a clean patch → assert `layoutSanitized == null` and positions unchanged.
3. **Opt-out:** `autoLayout:false` with overlap → assert overlaps preserved.
4. **Clusters correctness:** multi-voice patch → assert N signal clusters + 1 bus + 1 control.
5. **Perf:** time the post-step (< 1 ms) across 100 builds.

Also keep the existing suites green: `undo-redo-gauntlet`, `load-bisect`, `gui-stress-gauntlet`, `npm run validate`.

---

## 8. Risks & mitigations
| Risk | Mitigation |
|---|---|
| Auto-move surprises the artist | minimal `deoverlap` only; report in `_v2meta`; `autoLayout:false` |
| O(n²) on huge patches | spatial-hash threshold (~200 objects) |
| Cluster misclassification | conservative `unknown`; classify only by nodeClass + bus detection |
| Extra cost on every build | < 1 ms budget; skip entirely when no collisions |
| Thread-safety of position writes | geometry-only; same context as mutation / under `sys_lock` |

---

## 9. Phases (ship incrementally)
- **P1 — Cluster fact (read-only).** Add `computeClusters` + expose in the layout X-ray. No behavior change; verifiable immediately.
- **P2 — Auto-deoverlap.** Opt-in (`autoLayout:true` explicit) → test → then **default-on** with opt-out.
- **P3 — Occlusion handling + prefs.** Optional `tidy` for occlusions; read `user_prefs` layout (spacing, gutter) so the guard respects house style.
- **P4 — Cluster-aware composition (optional).** Let the guard *propose* per-cluster columns (voice = column, bus at edge) as a `layout_patch` preset — still AI-approvable, never forced.

---

## 10. Why this beats the AGENTS.md rule alone
| | Rule in AGENTS.md | Guard in C++ |
|---|---|---|
| Depends on AI memory | yes | no |
| Cost | tokens + round-trips | free, inline |
| Speed | seconds | < 1 ms |
| Consistency | model-dependent | deterministic |
| Context (clusters) | no | yes |

The AI keeps the **creativity**; C++ takes over the **guarantee**.
