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
3. **Option D (third review), facts only.** NEW `stopFacts.kind` (always present when there is an output): the stop
   kind. **When every output sounds: `kind: "none"` and `nearestStop: null`** (no stop is chosen; no path rule, no
   sum detection). The walk still lists every node upstream of the outputs as rows with their OWN measured verdicts.
   **When some output is silent:** `nearestStop` is the measured walk from the SILENT outputs only (dies_here,
   open_inlet, silent_source, bus_end at the lowest hop, or unread); sounding outputs are never searched for stops.
   **Every row has `diesHere: true|false`**: some signal input audible and the output silent or dc_only (a measured
   fact; the reader decides whether a dead lead into a sounding mixer, or a [sig~] bias, matters).
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
    unread | no_output`). **Review H3 / option D:** whenever EVERY output sounds the old pair stays exactly:
    `chokePoint: "none"`, `reason: "signal reaches master sink"` (an older server reads it right), and
    `stopFacts.kind: "none"`, `stopFacts.nearestStop: null`. The prose
    reasons, the gate-sel note and `stages[].note` are gone. The plain (unarmed) trace runs the same walk on the
    last block's raw peaks (audible above -60 dBFS, the judge's floor) but has NO `stopFacts` block.

15a. **Buses (review M4; `$0` names realised, delta 4):** the walk crosses `throw~ N` -> `catch~ N` and `send~/s~ N` -> `receive~/r~ N` on the same
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
    Receivers are objects only; at most 64. **Review M6 / server review B, fail closed:** also refused: names starting with `.`
    (a sentence in `refused`), and any name that also reaches something that is not a box (a canvas named by
    namecanvas, an array, a [v] value, a dialog stub): `refused: "non-object binder"` exactly, plus
    `refusedClass: "<Pd class>"`; nothing is sent.
    **Server review A:** a delivered send also writes the /param shadow, so `/pd/param_get <name>` reads it.
    **Server review D:** when the Pd context does not answer in time: `bound: null` (not known), `delivered: false`,
    `error: "the Pd context did not answer in time"`. The bridge's own probes are skipped.
19a. **`delivered` counts differently (review L9):** obj_set_batch counts any line the SET object printed during
    delivery; send_checked counts every ERROR line during delivery (any object) plus any line a receiver printed.
    Neither sees a line Pd prints in a later tick.

19b. **pd-lua `pd.Receive` proxies are listeners (overnight 2026-11-16).** A binder whose class is named `pdlua proxy receive` is
    no box and is never read as a `t_object` (review H5); it used to be refused as `non-object binder`. Now it is counted:
    the reply has a new integer `proxies` (always present, 0 when none). `bound` is true when there are boxes or proxies; the
    send goes out when there is at least one of either; only the boxes are read back (`receivers[]`), so a name with proxies
    alone is `bound: true, proxies: N, receivers: []`, and `delivered` is true when no console line came. Every other binder
    that is not a box is still refused as before. `obj_set` / `obj_set_batch`: ONLY a pd-lua object (a class plugdata's own pd-lua
    registry knows, `name` or its `name:gfx` shadow class, and CLASS_NOINLET) numbers its inlets from the first Lua inlet: inlet k is
    `ob_inlet[k]`, inlet 0 stays the object itself when it has no `ob_inlet` entry at all. A negative inlet is the object, always.
    Every other class, the CLASS_NOINLET ones included (the canvas of an abstraction or a [pd] with [inlet]s, [clone], an unnamed
    [receive], a gatom, ELSE [receiver], [outlet~]), keeps the numbering it always had: inlet 0 is the object, inlet k >= 1 is
    `ob_inlet[k-1]`, so the server's "physical inlet + 1" for abstraction sets and its canvas selectors (vis, clear, loadbang,
    dsp, coords) on inlet 0 are unchanged. Only pd-lua objects changed; no server change. (Commit a9b265338 first applied the Lua
    numbering to every CLASS_NOINLET class: wrong for the classes above, fixed 2026-11-17.)

## `/pd/widget_ask` (capability `widget_ask`)
19c. The `<canvas>` argument is the tab the widget sits in: the identity wire key of the open root canvas whose patch (subpatches
    and abstractions included) holds a box of the widget's class (`ai.tap`, pd-lua's `ai.tap:gfx` shadow class, or an
    abstraction file `ai.tap.pd`), the `key` of that tab's `/pd/list_tabs` row. `main` (the focused tab, as before) when the widget
    cannot be placed (a looper, a box not yet placed, a clone instance) or sits in more than one tab. Same argument count.
    All the widgets of one pump tick are looked up in ONE walk of the open patches (budget 20000 boxes); a walk that runs out of
    budget claims nothing: `main`. Roots no tab shows (none are opened by plugdata itself) count like tabs.

## `/chat/msg` roles
19d. Role `detail` (a long answer) is shown in the Copilot sidebar as an AI message. The only reader of `/chat/msg` is the sidebar
    panel (`CopilotPanel::receiveMessage`), so it appears nowhere else; roles `ai`, `error`, `user`, `thought` are unchanged.
19e. Role `ai` is also shown on the canvas: as ONE short line under the place of the selection pill (its last frame: the pill
    hides on a send, the answer arrives after), about 8 s (7 s hold, 1 s fade), cleared early by the artist's next prompt
    (role `user`). The text is one plain line (breaks and control characters become spaces, markdown backticks dropped, cut at
    160 characters with "...", then cut again by width). `detail`, `thought` and `error` are not shown there. The sidebar still
    receives every role first: the canvas is one more reader of the bridge's chat lines (`MCPBridge::addChatListener`, keyed by
    its owner), `setChatCallback` stays the sidebar's single slot. No wire change.

## `/chat/lane` (capability `pill_lane`) and the pill's QUICK / DEEP chip
19f. NEW inbound `/chat/lane <deep|quick>`: the server's sticky pill lane, pushed after every successful bridge handshake (first
    connect, reconnect, a PlugData restart with a new boot token, a late handshake, a cap refresh) and at every switch. State only:
    it sets the lane the chip on the selection pill shows (`MCPBridge::isPillDeepLane`) and posts NO chat line. When the word
    CHANGES the lane, the fork posts one repaint of every canvas (`McpGuiHost::repaintAll`) so the chip shows it at once (review
    2026-11-17, finding 8); the same word again repaints nothing. It is a verb of
    the `chat` domain, which takes every verb: no verb-table row, no schema row, no new hash. Any other word, or none, is
    ignored; an older fork reads a one-argument `/chat/...` as nothing (it needs two). The server sends it only to a bridge
    with capability `pill_lane`.
19g. The chip (QUICK / DEEP, between the prompt field and the send arrow; the pill is 46 px wider, the field keeps its width)
    flips the lane on a click. DEEP makes a typed prompt go out with mode `queue` on `/pd/ui/prompt` (the existing wire: the
    server's deep lane), QUICK with `spawn`. A click also sends the lane word as a prompt, `/pd/ui/prompt deep|quick spawn 0`
    (no chat echo, no selection), the words the artist can type: the server switches its sticky lane and pushes it back with
    `/chat/lane`, so the chip and the word agree. Palette actions and the sidebar input are not changed by the chip.

## Thread phase 2: live widget history and ai.midi's source (capability `widget_state`, no new schema hash)
19k. NEW kind `state` of the existing event `/pd/widget_ask`: `/pd/widget_ask state main <widget> <rev>` (widget `ai.<word>`, rev a whole
    number 0..9999999; the canvas is always `main`: the server reads a widget by its name, no walk of the patches is made for it).
    Same address as the ask chip's `ask` / `hello` (events are listed by ADDRESS in the schema's `events` table, so the kind word is free:
    no schema row, no new schema hash; an address that is not in the table would reach UDP only, never a v2 session). While the
    bridge serves, a native CLASS_PD receiver is bound to the state name of every watched widget: `ai-tap-state`, `ai-draw-state`,
    `ai-loop_0-state`, `ai-loop-state` (the legacy looper), `ai-sketch-state`, `ai-knob-state`, `ai-dice-state`, `ai-grid-state`,
    `ai-paint-state`, `ai-strip-state`, `ai-listen-state`, and, once a named looper `ai.loop_<name>` (name `[a-z0-9_]{1,12}`) has said
    hello on `ai-ask`, its `ai-loop_<name>-state` (at most 16 named loopers: a 17th pushes out the one idle longest, freeing its receiver, and
    says so as one console line at most once a second; the names held survive a stop / start of the bridge, because a looper says hello only
    once, at load). It coexists with the widget's own `[r]` / `pd.Receive` and with an armed control window on the same name (Pd keeps a bind list).
    The receiver looks at ONE thing: a message with the selector `end` and a number (a widget's state frames end with `end <rev>`);
    every other frame is one pointer compare. A widget whose last `end` is 1 s old is ONE event, only when its rev is not the one last
    forwarded, and at most one event per widget per 2 s (a change inside the gap waits for it). The pump is the ask pump (message thread,
    every 50 ms); it pops nothing while no server can be told. The rev is the only identity of a state: two instances of one widget in
    different tabs share one state name, and a widget rebuilt at rev 0 after rev 0 is not reported again until its rev moves. The producer side (Pd's dispatch, the audio thread too) is atomics only: no lock, no allocation, no JUCE
    (`Source/Mcp/McpWidgetState.h`, `Source/Pd/MCPBridgeWidgetState.h`; `scripts/test-bridge-safety.sh` greps both).
19l. NEW inbound `/chat/widget_watch <on|off>` (capability `widget_state`): the server switches the history on or off. A verb of the `chat`
    domain (takes every verb: no verb-table row, no schema row), state only, nothing is posted to the chat. The watch is OFF at start and
    after every stop (also an `on` still in flight when the bridge stops), so an older server (which never sends `on`) never sees a `state` event; any other word, or none, is ignored.
    `MCPBridge::setWidgetStateWatch` / `widgetStateWatching` (tests). Receivers are bound at `bindWidgetAsk` and freed at `unbindWidgetAsk`.
19m. ai.midi's send (`AiMidiObject::transmitToAi`) names itself: `MCPBridge::sendMidiTakePrompt` sends the same `/pd/ui/prompt <prompt>
    spawn 0` as before (and the chat echo), plus the source of the take after the (zero) selection ids: `... ai.midi <take id> <note count>
    <bpm>`. An older server reads exactly `count` ids and ignores the rest; the new server reads the tag first and keeps the wording
    match ("Incorporate MIDI take (...) into the patch.") as the fallback for a build without it.
    Tests: `Tests/mcp/widget_state_test.cpp` (the book: bounds, quiet, rev, gap, off), `runWidgetStateCases` in `bridge_handler_tests.cpp`
    (receivers bound, burst = one event, same rev none, gap, named looper by hello, off, `sendMidiTakePrompt`, stop unbinds).

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

## The Copilot module rack (sidebar), no wire change
19h. The sidebar's "MODULE RACK" (it held two cards, ai.midi and ai.voice) lists one card per ai.* widget in groups: IDEA KIT
    (ai.sketch, ai.note, ai.paint, ai.grid), RHYTHM (ai.tap, ai.loop~), MELODY (ai.draw, ai.midi, ai.voice, ai.listen), SOUND
    (ai.knob, ai.dice), SYSTEM (ai.strip, ai.jobs). A card shows the group's colour dot, the name, a one-line purpose (the gallery's
    comments) and an optional thumbnail; dragging it to a patch creates `ai.<word>` (`ai.midi 240 64`, `ai.voice 240 72`;
    `ai.loop~ loop<n>` with the lowest n no open tab uses, since a looper's name is its shared bus; ai.note is ELSE's
    `note 280 16 Inter empty 0 18 18 18 0 247 194 20 1 0 sticky note`). The list scrolls inside the panel; a click on the rack's title
    row folds it. Thumbnails are read, never made: `<app data dir>/Abstractions/thumbs/<name>.png` (odd characters in the name,
    the `~`, become `_`: `ai.loop_.png`), looked up again each time the rack is shown. Table and helpers: `Source/Mcp/McpWidgetRack.h`
    (JUCE-free, `Tests/mcp/widget_rack_test.cpp`); cards and list: `Source/Sidebar/CopilotModuleRack.h`.

## Ctrl+V with a picture: refs/ and a [pic] box
19i. Ctrl+V on the canvas with a picture on the clipboard (no paste slot waiting; the clipboard read is the same as before) on a SAVED
    patch: the picture is written as `<patch folder>/refs/<patch>-<n>.png` (`refs/` is created; n is the lowest free number, an
    existing file is never touched; the stored picture is at most 960 px on its longest side, never enlarged) and ONE box
    `[pic -open refs/<file>]` is pasted at the mouse (in the middle of the view when the mouse is outside it). ELSE's [pic] opens
    its file when it is created and on every load; a [loadbang] -> [open( chain would stay empty after a paste, because loadbang
    fires when a patch loads, not when objects are pasted. The event `/pd/reference/image <path> <x> <y> <w> <h>` is sent as for a
    glass picture, now with the refs file's path (the server also writes a Thread `media` entry), and a line "Picture saved as
    refs/<file> ..." goes to the chat (sidebar and the pill's answer line). An unsaved patch, a refs/ that cannot be written, or a
    paste slot waiting (the pill's picture button): the older way, a picture on the glass, unchanged. Names and sizes:
    `Source/Mcp/McpRefsName.h` (JUCE-free, `Tests/mcp/refs_name_test.cpp`); the clipboard read and the paste:
    `Source/Mcp/McpPicturePaste.cpp` (moved out of `CanvasMcpTools.cpp`; see 19j for what changed after the 2026-11-17 review).

## Review 2026-11-17 of 19f..19i: follow-ups, no wire change
19j. (1) **A pasted picture keeps `refs/` when the patch is saved.** `PictureObject::openFile` cut the stored name of every [pic] down
    to its bare file name, so `[pic -open refs/a.png]` was saved as `pic 0 a.png ...` and no longer opened from the patch's folder
    at the next load. A name RELATIVE to the patch (`refs/a.png`, `./a.png`, `../x/a.png`) is now kept as given; an absolute path
    is still cut to its file name (upstream's behaviour). Upstream file `Source/Objects/PictureObject.h`: two hunks tagged
    `MCP-CARRIED(pic-keep-relative)` (SEAM.md), the rule in `mcp::picKeptFilename` (`McpRefsName.h`, `Tests/mcp/refs_name_test.cpp`).
    (2) **Ctrl+V starts no clipboard tool for text.** `Canvas::pasteSelection` asks for a picture first; when the JUCE clipboard
    holds any non-empty text, that is returned at once unless the text is the path of an image file (`/x.png`, `file:///x.png`):
    no xclip / wl-paste process, no wait of up to 3 x 3.5 s on the message thread (it stalled while PlugData itself owned the
    clipboard). Only a clipboard WITHOUT text (a screenshot, "copy image") runs the tools; a tool that outlives its 3.5 s is killed
    and reaped (`waitpid` through `ChildProcess::isRunning`). A clipboard that holds both an image and text now pastes the text.
    (3) **`refs/` only for a normal saved user patch.** The picture goes to the glass instead (the older way) when the canvas is
    locked or in presentation mode (`dragAndDropPaste` unlocks it, so it is not called), when the patch file is inside the app
    data dir (Documentation, Abstractions, Extra, Patches, Externals, ...), or when the patch's folder is not writable.
    (4) A canvas that never laid out a pill (a graph, one locked from the start) ignores chat lines: no state, no answer-line timer.
    The paste code is `Source/Mcp/McpPicturePaste.{h,cpp}`.
