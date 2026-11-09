# Truth slice 3: reply-shape changes against docs/plans/truth-slice3-steps.md (Track F → Track S)

Everything in the plan's "Fixed reply shapes" holds. Below are the additions and the points the plan left open,
as the fork now answers them. Nothing in the plan's shapes was removed or renamed.

## `/meter/trace … read` → `stopFacts` (capability `trace_stop_facts`)
1. **`nearestStop.kind` has two more measured kinds, `silent_source` and `bus_end`** (review M4). `bus_end`: the walk
   ended at a silent catch~ / receive~ whose senders are not on this canvas. `silent_source`: the walk ended at a node whose output is silent
   (or dc_only) and into which no signal wire comes (e.g. `[sig~ 0]` → `[*~]` → dac~). Order at one hop is fixed:
   `dies_here`, then `open_inlet`, then `silent_source`. `nearestStop` is the LOWEST-hop candidate, so an open inlet
   at hop 1 wins over a dying node at hop 2 (the L2 case: `sig_8` hop 1 beats `dust_g` hop 2). `unread` only when an
   untapped node lies nearer than every measured candidate (or none was measured).
2. **An output with no signal wire at all** is `open_inlet` at the output itself, `hop: 0`, `inlet: 0`.
3. **`kind: "none"`**: `nearestStop.tempId` is the output's tempId, `hop: 0`, no `inlet`.
4. **No output on the canvas** (no dac~ / out~): `"nearestStop": null`; the trace's `reason` is `"no_output"`.
5. `nearestStop.inlet` is present only for `open_inlet`.
6. **Live-end rows**: after the walk's rows (≤ 30), every live end that is not on the walk is appended as a row with
   `"hop": 0` and `"liveEnd": true` (so `lp_lop`'s facts are there). Walk rows have no `liveEnd` key.
   `rowsTruncated` counts walk rows only. Output sinks (dac~/out~) are never rows; they are in `outputs`.
7. **Wire ids carry the outlet**: `sig[].from[].id` and `ctl[].from[].id` are both `"<srcTempId>:<outlet>"`
   (e.g. `"noise:0"`, `"mo_sl:0"`). A source without a tempId is named `"<class>#<index>"`.
8. `out` = `{verdict, levelDb}`; `verdict` is `audible | silent | dc_only | unmeasured | unread` (unread: no tap on
   it); `levelDb` is null for `dc_only`, `unmeasured`, `unread`. `sig[].from[]` uses the same verdict words.
9. `sig[]` lists EVERY signal inlet (wired or not). `ctl[]` lists an inlet when it has a control wire, or when it
   holds a value (`floatSlot`/`scalar`/`symbolSlot`) and no signal wire. `ctl[].value`/`source` are the inlet's own
   copy (null when Pd keeps none). `ctl[].by` = who sent the inlet's last recorded message (`ai`/`other`), null when
   nothing was recorded since the patch loaded.
10. `ctl[].from[].value`: a GUI source's own value; else the first atom the wire last carried; else null.
    `ctl[].from[].by`: who sent the wire's last message; null when none recorded. `handMoved`: GUI sources only
    (hsl, vsl, tgl, hradio, vradio, nbx, knob, float atom); `false` when the GUI holds the AI's last value (a bridge
    set, or the value it had right after the AI created it); `true` when it differs AND the GUI has no wire in and no
    receive name; `null` otherwise (no AI value recorded, a wire or a receive name could have moved it, not a GUI).
11. `made`: `"ai"` when the object is the AI's and still as the AI left it (`McpHost::aiIntact`), else `"other"`.
12. `names`: the receive names the object itself is bound to (checked against Pd's binding).
13. `switchedOff`: `[switch~]` off on the row's canvas chain (for a subpatch box: its own chain).

## Trace top level (no window and window)
14. `chokePoint` = `nearestStop.tempId`, `reason` = the kind code (`dies_here | open_inlet | silent_source | bus_end |
    unread | no_output`). **Review H3:** when the output sounds (kind `none`) the old pair stays exactly:
    `chokePoint: "none"`, `reason: "signal reaches master sink"` (an older server reads it right); the output's id is
    only in `stopFacts.nearestStop.tempId`. The prose
    reasons, the gate-sel note and `stages[].note` are gone. The plain (unarmed) trace runs the same walk on the
    last block's raw peaks (audible above -60 dBFS, the judge's floor) but has NO `stopFacts` block.

15a. **Buses (review M4):** the walk crosses `throw~ N` -> `catch~ N` and `send~/s~ N` -> `receive~/r~ N` on the same
    canvas (checked against Pd's binding). A bus receiver's row gets `bus: [{id: "<senderId>:bus", verdict, levelDb}]`
    (the sender's level is its input's); a receiver with no sender on the canvas gets `bus: []` and, when the walk ends
    there, kind `bus_end`. Bus edges count for hops; a throw~/send~'s bus outlet is not counted in `outWires`.

## `/pd/inlet_values` (capability `inlet_last`)
15. `last` = `{selector, atoms, seq, by, via}` plus `truncated: <full atom count>` when more than 4 atoms came.
    A pointer atom reads `"<pointer>"`. `via` is `"inlet"` (the inlet's own record: every message it got, by wire or
    sent to it; inlets ≥ 1 only) or `"wire:<srcId>:<outlet>"` (inlet 0, or an inlet ≥ 1 without its own record).
16. `signalWindowMs <ms>` (clamped 20..1000) after the ids. Each signal-wired inlet gets
    `signal: {feeders, blocks, mean, min, max[, reason]}`. `mean` is the sum of the feeders' means (Pd sums them);
    `min`/`max` only with exactly one feeder (else null); `mean` null with a `reason` when not every feeder could be
    tapped (≤ 4 feeders per call, the free probe slots) or a feeder ran no block. The root echoes `signalWindowMs`.

## `/pd/obj_set_batch` (capability `set_delivery`)
17. Each result has `delivered`, `console` (array of line texts from that object during delivery) and `readBack`
    `{value, source, reason}`; `source` is `gui | scalar | floatSlot | symbolSlot | last | null`. `last` is used only
    when THIS dispatch made the inlet's record (osc~'s forwarded phase inlet reads back as `last`). A failed entry
    (`ok:false`) has `delivered:false, console:[], readBack:null`. A stale tempId (object gone) is now `ok:false`
    ("unknown tempId"), never a dangling read.
18. `/pd/obj_set` (single) keeps its old OSC reply (`1.0 [warning]`); it records the AI range (so `by` is right) but
    carries no delivery block. Use `obj_set_batch` for delivered/readBack.

## `/pd/send_checked` (capability `send_checked`)
19. Shape as planned. `refused` is a sentence (empty when not refused); refused also for an empty name. `console` =
    every ERROR line printed during delivery (any object) plus any line a receiver printed, as `{tempId, text}`
    (`tempId` "" when the printer has none). `receivers[].readBack.source`: `gui` (a GUI bound to the name) or
    `last` (the record this send left on the receiver's outlet wire, e.g. `[r f]` → hsl) or null with a reason.
    Receivers are objects only; at most 64. **Review M6, fail closed:** also refused: names starting with `.`, and any
    name that also reaches something that is not a box (a canvas named by namecanvas, an array, a dialog stub):
    `refused` names what it reaches, nothing is sent. The bridge's own probes are skipped.
19a. **`delivered` counts differently (review L9):** obj_set_batch counts any line the SET object printed during
    delivery; send_checked counts every ERROR line during delivery (any object) plus any line a receiver printed.
    Neither sees a line Pd prints in a later tick.

## `by` limits (truth)
20a. **Review H2:** `by` (in `last`, `ctl[].by`, `ctl[].from[].by`) is `"ai"`, `"other"` or **null** = not known: the
    seq is older than the oldest AI range the bridge still keeps (a ring of 1024 ranges; back-to-back AI dispatches
    merge into one). Word null as "not known", never as the artist's.
20. `by: "ai"` covers the bridge's own dispatches: `obj_set`, `obj_set_batch`, `send_checked`. Realtime `/param` and
    `/trigger` go through PlugData's message queue and are NOT marked: a value the AI sent with `/param` reads
    `by: "other"`. Word it as "not marked as the AI's", or move value sends to `send_checked`.

## Known gaps of F5 (ids after the artist's own delete; review L9)
21. Not journalled, so the ids are lost as before: encapsulate, triggerize, the children of a deleted subpatch, and a
    cut -> paste -> undo -> redo chain (the paste makes new boxes). An editor-delete id comes back only with the undo
    of that very delete (review H1), never by place alone.
