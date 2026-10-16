# PRD: Object-Scoped Get/Set (`/pd/obj_set`, `/pd/obj_get`)

**Status:** Phases 1–5 done and live-verified
**Files:** `Source/Pd/MCPBridge.cpp`, `mcp-server/src/tools/*`, `mcp-server/src/transport/osc-client.ts`

## 0. Verified live
- `obj_set`: `hsl` range `0 127` -> `1 127` -> `0 1`, persisted, wires intact.
- `construct_patch_v4.set[]`: create + set range in one atomic call.
- `obj_get` / `analyze_patch action:"props"`: `h1 (hsl) {min:0,max:1,...}`, `k1 (knob) {min:0,max:127,value:0,...}`.
- **P5a** `/pd/obj_set_batch`: two-entry `set[]` applied in one lock/reply.
- **P5b** structural classification: `resize` -> `obj_set h2: structural:resize (may trigger a DSP recompile — prefer edit)`.
- **P4** `/pd/param_get`: `analyze_patch props buses:["testbus"]` -> `{found:true, atoms:[42]}`.
- Dropout-free (no recompile) for scalar/property sets.

## 1. Problem

The bridge can create/delete/edit/connect objects and write **named buses**
(`/param <receiver> <value>`), and it has a bulk **array** channel
(`array_io` / `manage_pd_tables`). It has **no object-scoped get/set** by
`tempId`: there is no way to read or write a live property of a specific
object (e.g. a slider's `range`, a knob's `send`/`receive`, a DSP inlet
parameter).

Consequence (motivating bug): changing a slider's min/max required
`save` -> read `.pd` text -> `edit` (recreate), instead of a live set.

## 2. Key insight

PlugData's native GUI widgets already expose a live message handler that the
Inspector drives:

- `SliderObject::receiveObjectMessage` — `case hash("range")`, `lin`, `log`,
  `steady`, `orientation`, `color` (`Source/Objects/SliderObject.h:305/325`)
- `KnobObject` — `range`, `log`, `send`, `receive`, `init`, `ticks`, ...
- `ToggleObject`, `ButtonObject`, `RadioObject`, `FloatAtomObject`, ...

So a native bridge op only needs to deliver the right **selector** via
`pd_typedmess` (NOT a list, and NOT a recreate). Verified live: sending
`range 0 1` as a selector changed `hsl ... 0 127 ...` to `hsl ... 0 1 ...`
and it persisted to save.

## 3. Architecture: one engine, three surfaces

**Rule: the engine is `/pd/obj_set` + `/pd/obj_get`. Surfaces match intent —
no new standalone tool.**

| Intent | Surface | Engine call |
|---|---|---|
| Live set (jam, ride a knob) | `plugdata_send` (+ `object`, `selector`, `inlet`) | `/pd/obj_set` (realtime lane) |
| Set as one step of an atomic build | `construct_patch_v4.set[]` | `/pd/obj_set` (dispatched after batch, NOT batch_atomic) |
| Inspect a live property | `analyze_patch action:"props"` | `/pd/obj_get` |

Why not a dedicated `manage_object` tool: it fragments the surface and makes
three "set" paths with no clear owner. Fewer, clearer tools.

## 4. Scope (avoid redundancy)

| Data | Route |
|---|---|
| 1 atom / short list / a property | **object get/set** (this PRD) |
| Array scalar messages (`resize`, `bounds`, `const`, `sinesum`) | **object set** |
| Array bulk samples (read/write/transform/import) | **array_io** (`manage_pd_tables`) |
| Frozen creation args / class swap | **`edit`** (recreate) |
| Identity name | **`rename_id`** |

## 5. Dropout rules

- `get` — never dropouts.
- `set` scalar property/param — safe (no `canvas_update_dsp`, no alloc).
- `set` structural (`resize`, `send~`/`receive~`, `clone n`, `block~`,
  `switch~`, buffer sizes) — may trigger a graph recompile → caller routes
  through `batch_atomic`/`edit`.
- `set` hammered into a GUI widget — throttle (GUI-flood lint).

## 6. Protocol (engine)

### Set (Phase 1 — implemented)
```
/pd/obj_set <canvas> <corrId> <tempId> <inlet> <selector> <argc> <atoms...>
  inlet : <=0 = object inlet 0 (pd_typedmess); >=1 = physical inlet
  atoms : floats as-is; symbols tagged "s:name"
reply: /pd/obj_set/reply/<corrId> 1.0 | "error: ..."
```

### Get (Phase 3 — planned)
```
/pd/obj_get <canvas> <corrId> <tempId> [selector]
reply: /pd/obj_get/reply/<corrId> <json>
  { args:[...], props:{ min,max,value,send,receive,log,orientation,... } }
```
`args` from binbuf text (cheap); live `props` via small Objects-layer
accessors next to `knob_get_snd/rcv` (`MCPBridge.cpp:51`).

---

## 7. Phase 2 — Surface spec

All three gate on capabilities (`obj_set` / `obj_get`); if absent, return a
clear `"bridge too old: rebuild plugdata-core"` error.

### 7A. `plugdata_send` — live object set

Extend `SEND_SCHEMA` (`mcp-server/src/tools/params.ts:19`):
```ts
action: z.enum(["float", "bang", "message", "object"])   // + "object"
object: z.string().max(128).optional()   // tempId (required when action="object")
selector: z.string().max(64).optional()  // required when action="object"
inlet: z.number().int().optional()       // default 0
atoms: z.array(z.union([z.number(), z.string()])).max(1024).optional()
```
Dispatch:
```
canvas = active tab
atoms: numbers as-is; strings -> "s:<str>"
/pd/obj_set [canvas, corrId, object, inlet ?? 0, selector, atoms.length, ...atoms]
```
- Use the **realtime lane** (`sendRealtime`, same as `/param`) — no `withOscLock`.
- Return `_v2meta`; surface Pd console errors (reuse the existing error-scan in `sendHandler`).
- Multiple objects later: `objects: string[]` fan-out (same selector/atoms).

### 7B. `construct_patch_v4` — atomic `set[]`

Schema addition:
```ts
set: z.array(z.object({
  tempId:  z.string(),
  selector:z.string(),
  inlet:   z.number().int().optional(),          // default 0
  atoms:   z.array(z.union([z.number(), z.string()])).optional(),
})).max(64).optional()
```
Dispatch order (critical):
1. Run existing `batch_atomic` (create/connect/edit/delete/move).
2. Parse reply mappings (new tempIds -> indices) — Phase-1 kill-polling already returns them.
3. For each `set[]` entry, resolve `tempId` (mapped) and send `/pd/obj_set`.
   - **Do NOT** fold into `batch_atomic` (would force a needless recompile).
   - v1: sequential `obj_set` calls (one round-trip each).
   - P5: add `/pd/obj_set_batch` for a single lock + reply.
4. Merge any `obj_set` errors into the `_v2meta.health` receipt.

Guard: if `selector` is a **structural** selector (Section 5), warn in the
receipt and recommend `edit` instead.

### 7C. `analyze_patch` — `action:"props"`

Extend the `analyze_patch` action enum (`mcp-server/src/tools/patching.ts`):
```ts
action: "props"
target:  z.string().optional()          // tempId
targets: z.array(z.string()).optional() // batch read
selector:z.string().optional()          // filter a single prop
```
Dispatch: `/pd/obj_get <canvas> <corrId> <tempId>` per target (or a
`/pd/obj_get_batch` later). Returns live `{ args, props }` per object.
- Read-only → no `withOscLock` needed; safe to call anytime (even mid-jam).

### 7D. Dispatch matrix

| Caller | Sends | Engine | Lane |
|---|---|---|---|
| `plugdata_send` action=object | `/pd/obj_set` | obj_set | realtime (unlocked) |
| `construct_patch_v4.set[]` | `/pd/obj_set` (after batch) | obj_set | locked (post-batch) |
| `analyze_patch` action=props | `/pd/obj_get` | obj_get | unlocked (read) |

### 7E. Error contract

Structured errors (not silent): `unknown tempId`, `inlet N out of range`,
`unsupported selector for class`, `bridge missing obj_set cap`. Each reply
carries a machine code + human hint, folded into `_v2meta.warnings`.

---

## 8. Phases

- **P0** capability handshake: add `obj_set` / `obj_get` flags. *(obj_set flag added in P1 commit)*
- **P1** `/pd/obj_set` handler in `handlePdDomain` — **DONE**.
- **P2** surfaces: `plugdata_send` object mode (7A), `construct_patch_v4.set[]` (7B), `analyze_patch` props (7C).
- **P3** `/pd/obj_get` + Objects-layer read accessors.
- **P4** `/param_get` (read named bus).
- **P5** `/pd/obj_set_batch` + structural-selector classification -> route to `batch_atomic`.

## 9. Tests

1. `obj_set hsl_drive range 0 1` -> save shows `0 1`.
2. `plugdata_send action=object object=hsl_drive selector=range atoms=[0,1]` -> verified.
3. `construct_patch_v4 {create:[hsl], set:[{tempId, selector:"range", atoms:[0,1]}]}` -> one call, range applied, receipt clean.
4. `analyze_patch action=props target=hsl_drive` -> `{min:0,max:1}`.
5. GUI selector matrix (send/receive/log/orientation/color).
6. Dropout: loop playing -> burst scalar sets -> tap shows no gaps.
7. Old-bridge fallback: missing cap -> clear error.
8. Regression: `/param` bus path unchanged.

## 10. Non-goals

- Bulk array transfer stays in `array_io`.
- Frozen creation args stay `edit`.
- Identity rename stays `rename_id`.
