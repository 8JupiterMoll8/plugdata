#!/usr/bin/env bash
# Build and run the MCP bridge unit tests (no JUCE, no Pd needed).
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
for t in bridge_safety_test handoff_test json_escape_test identity_rename_test identity_service_test registry_test worker_test v2_codec_test v2_router_test v2_store_test trace_taps_test ac_level_test wire_activity_test output_ceiling_test ceiling_console_test tab_token_test approval_state_test flow_provenance_test visible_tab_test alphi_test alphi_dock_test alphi_say_test alphi_gate_test; do
  "${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror -pthread -o "$out/$t" "$root/Tests/mcp/$t.cpp"
  "$out/$t"
done
# P9 output ceiling: once more at the Release flags (-O3 -DNDEBUG), so the non-finite check is proven optimised too.
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror -pthread -O3 -DNDEBUG -o "$out/output_ceiling_test_release" "$root/Tests/mcp/output_ceiling_test.cpp"
"$out/output_ceiling_test_release"
# The v2 listener (Plan 4 P4.1 S2) is a .cpp of its own; its test opens real loopback sockets.
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror -pthread -o "$out/v2_server_test" \
  "$root/Tests/mcp/v2_server_test.cpp" "$root/Source/Pd/MCPBridgeV2Server.cpp"
"$out/v2_server_test"
# THREADING rule 13 (P4.1 S3): a handler hops through postMessage / runJob / McpHost, which carry the request's work
# token; a bare callAsync would not (its replies would lose their request, which would end before them).
# Allowed (none answers a request): MCPBridge::postMessage itself, the save hook's sidecar write, the repaint helper in
# MCPBridgeCmdCommon.h, and the voice/MIDI take timers (artist events and take stops, started from the GUI).
bare="$(grep -n "MessageManager::callAsync\|Timer::callAfterDelay" "$root"/Source/Pd/MCPBridge*.cpp "$root"/Source/Pd/MCPBridge*.h \
  | grep -v ":\s*//" \
  | grep -v "MCPBridge.cpp:.*weak = juce::WeakReference<MCPBridge>" \
  | grep -v "MCPBridge.cpp:.*\[p, sidecar, path\]" \
  | grep -v "MCPBridgeCmdCommon.h:.*\[proc, cnv\]" \
  | grep -v "MCPBridge.cpp:.*callAfterDelay(1600, \[this\]" \
  | grep -v "MCPBridge.cpp:.*callAfterDelay(.*\[this, takeId\]" || true)"
if [ -n "$bare" ]; then
  echo "$bare" >&2
  echo "test-bridge-safety: bridge code posts with a bare callAsync/callAfterDelay (use postMessage: THREADING rule 13)" >&2
  exit 1
fi
# P11 (take files): no fork source writes to a hard-coded user path (the bridge wrote midi-take.json / voice-take.json
# into one developer's checkout; the TS persists both from the /pd/midi/event and /pd/voice/event replies).
# V1 read-only facts: the fact verbs never change the patch, the device or the audio.
factWrites="$(grep -nE "pd_typedmess|pd_float\(|pd_bang\(|glist_delete|obj_connect|obj_disconnect|canvas_undo|setAudioDeviceSetup|binbuf_eval|sys_vgui|mcpObjSetCore" "$root/Source/Pd/MCPBridgeCmdFacts.cpp" "$root/Source/Pd/MCPBridgeInletValues.h" "$root/Source/Pd/MCPBridgeBroken.h" | grep -v ":\s*//" || true)"
if [ -n "$factWrites" ]; then
  echo "$factWrites" >&2
  echo "test-bridge-safety: a V1 fact verb writes (the fact verbs are read only)" >&2
  exit 1
fi
# V1 wire window: the hook runs on Pd's thread inside every outlet call: no lock, no allocation, no JUCE.
wireRt="$(grep -nE "std::mutex|lock_guard|unique_lock|ScopedLock|new |malloc|std::vector|std::string|juce::|std::function" "$root/Source/Pd/MCPBridgeWireActivity.h" "$root/Source/Mcp/McpWireHook.h" | grep -v ":\s*//" | grep -v "^[^:]*:[0-9]*: \*\|^[^:]*:[0-9]*://" || true)"
if [ -n "$wireRt" ]; then
  echo "$wireRt" >&2
  echo "test-bridge-safety: the wire-activity hook path must not lock, allocate or use JUCE" >&2
  exit 1
fi
# Review 2026-11-06: Pd's private struct _inlet is read through the fork's class-checked accessors (pure-data/0004),
# never through a mirrored layout.
inletMirror="$(grep -rnE "InletMirror|struct _inlet *\{|i_floatslot|i_symslot|iu_floatsignalvalue" "$root"/Source/Pd "$root"/Source/Mcp | grep -v ":\s*//" || true)"
if [ -n "$inletMirror" ]; then
  echo "$inletMirror" >&2
  echo "test-bridge-safety: bridge code mirrors Pd's struct _inlet (use the mcp_inlet_* accessors)" >&2
  exit 1
fi
wireCalls="$(grep -rn "mcp::wireMessage(" "$root"/Source --include=*.cpp --include=*.h | grep -v ":\s*//" | grep -c . || true)"
if [ "$wireCalls" != "1" ]; then
  echo "test-bridge-safety: mcp::wireMessage must be called exactly once (MessageDispatcher::enqueueMessage), found $wireCalls" >&2
  exit 1
fi
userPaths="$(grep -rnE '"/(home|Users)/[^"]*' "$root"/Source/Pd "$root"/Source/Mcp | grep -v ":\s*//" || true)"
if [ -n "$userPaths" ]; then
  echo "$userPaths" >&2
  echo "test-bridge-safety: fork source names a hard-coded user path (take it from the config or leave the file to the TS)" >&2
  exit 1
fi
# P12a A3 / P12a.1: flow mode goes ON only through the artist's click on Alphi, the robot face on the canvas. The one
# call of artistTurnOn is in McpHost::alphiClicked (Source/Mcp/McpFlow.cpp), and the one call of alphiClicked is the
# canvas click (CanvasMcpLayer::handleAlphiClick, Source/Mcp/CanvasMcpFlow.cpp). Review L4: any mention counts (a
# call, a function pointer, a std::function), and only a line that IS a comment is skipped (a "://" in code is not).
codeOnly() { grep -vE '^[^:]+:[0-9]+:[[:space:]]*//' || true; }
turnOnCount="$(grep -c "artistTurnOn" "$root/Source/Mcp/McpFlow.cpp" || true)"
if [ "$turnOnCount" != "1" ]; then
  echo "test-bridge-safety: McpFlow.cpp must name artistTurnOn exactly once (the call in alphiClicked), found $turnOnCount" >&2
  exit 1
fi
turnOn="$(grep -rnw "artistTurnOn" "$root"/Source --include=*.cpp --include=*.h --include=*.inc | codeOnly \
  | grep -v "Source/Mcp/McpFlowMode.h:[0-9]*:    void artistTurnOn(std::int64_t nowMs)$" \
  | grep -v "Source/Mcp/McpFlow.cpp:[0-9]*:    session.flowMode.artistTurnOn(nowMs);$" || true)"
if [ -n "$turnOn" ]; then
  echo "$turnOn" >&2
  echo "test-bridge-safety: artistTurnOn is named outside McpHost::alphiClicked (flow mode must be the artist's click only)" >&2
  exit 1
fi
# The call must sit inside alphiClicked: the nearest function head above it is McpHost::alphiClicked.
turnOnHost="$(awk '/^[A-Za-z].*McpHost::[A-Za-z_]+\(/ { fn = $0 } /session\.flowMode\.artistTurnOn\(/ { print fn }' "$root/Source/Mcp/McpFlow.cpp")"
if [ "$turnOnHost" != "bool McpHost::alphiClicked(std::int64_t nowMs)" ]; then
  echo "test-bridge-safety: artistTurnOn is called in '$turnOnHost', not in McpHost::alphiClicked" >&2
  exit 1
fi
alphiCallers="$(grep -rnw "alphiClicked" "$root"/Source --include=*.cpp --include=*.h --include=*.inc | codeOnly \
  | grep -v "Source/Mcp/McpHost.h:[0-9]*:    bool alphiClicked(std::int64_t nowMs);$" \
  | grep -v "Source/Mcp/McpFlow.cpp:[0-9]*:bool McpHost::alphiClicked(std::int64_t nowMs)$" \
  | grep -v "Source/Mcp/CanvasMcpFlow.cpp:[0-9]*:        canvas.pd->getMcp()->alphiClicked(now);$" || true)"
if [ -n "$alphiCallers" ]; then
  echo "$alphiCallers" >&2
  echo "test-bridge-safety: alphiClicked is named outside the canvas click (CanvasMcpLayer::handleAlphiClick)" >&2
  exit 1
fi
# The brake (d1 live check 2026-11-05): flow ON + a click on Alphi goes to McpHost::alphiBrake, which only turns flow
# off (artistTurnOn stays alphiClicked's alone, above); it too is called only by the canvas click.
brakeCallers="$(grep -rnw "alphiBrake" "$root"/Source --include=*.cpp --include=*.h --include=*.inc | codeOnly \
  | grep -v "Source/Mcp/McpHost.h:[0-9]*:    bool alphiBrake(std::int64_t nowMs);$" \
  | grep -v "Source/Mcp/McpFlow.cpp:[0-9]*:bool McpHost::alphiBrake(std::int64_t nowMs)$" \
  | grep -v "Source/Mcp/CanvasMcpFlow.cpp:[0-9]*:        host->alphiBrake(now);$" || true)"
if [ -n "$brakeCallers" ]; then
  echo "$brakeCallers" >&2
  echo "test-bridge-safety: alphiBrake is named outside the canvas click (CanvasMcpLayer::handleAlphiClick)" >&2
  exit 1
fi
handleAlphi="$(grep -rnw "handleAlphiClick" "$root"/Source --include=*.cpp --include=*.h --include=*.inc | codeOnly \
  | grep -v "Source/Mcp/CanvasMcpLayer.h:[0-9]*:    bool handleAlphiClick(juce::Point<int> canvasPt);$" \
  | grep -v "Source/Mcp/CanvasMcpFlow.cpp:[0-9]*:bool CanvasMcpLayer::handleAlphiClick(juce::Point<int> canvasPt)$" \
  | grep -v "Source/Mcp/CanvasMcpNotes.cpp:[0-9]*:    if (handleAlphiClick(canvasPt))$" || true)"
if [ -n "$handleAlphi" ]; then
  echo "$handleAlphi" >&2
  echo "test-bridge-safety: handleAlphiClick is named outside the canvas mouse path (CanvasMcpLayer::handleNoteClick)" >&2
  exit 1
fi
# P12a.1 review M1: a click inside Alphi is never the canvas's (isPointOverNote, which the double-click box asks).
# Big Alphi 1a0 review H1: through mcp::alphiOwnsPoint, which gives the point back to the canvas while a box lies
# under Alphi (Tests/mcp/alphi_dock_test.cpp).
if ! awk '/^bool CanvasMcpLayer::isPointOverNote/ { f = 1 } f && /mcp::alphiOwnsPoint\(/ { print; exit } f && /^}/ { exit }' "$root/Source/Mcp/CanvasMcpNotes.cpp" | grep -q "mcpAlphiFrame.*alphiBoxes()"; then
  echo "test-bridge-safety: CanvasMcpLayer::isPointOverNote must hold Alphi's frame through mcp::alphiOwnsPoint(mcpAlphiFrame, alphiBoxes(), ...)" >&2
  exit 1
fi
# 1a0 review H1: the canvas click asks mcp::alphiOwnsPoint before it can reach alphiClicked.
flowSrc="$root/Source/Mcp/CanvasMcpFlow.cpp"
ownsLine="$(awk '/^bool CanvasMcpLayer::handleAlphiClick/ { f = 1 } f && /mcp::alphiOwnsPoint\(/ { print NR; exit } f && /^}/ { exit }' "$flowSrc")"
clickLine="$(awk '/^bool CanvasMcpLayer::handleAlphiClick/ { f = 1 } f && /alphiClicked\(/ { print NR; exit } f && /^}/ { exit }' "$flowSrc")"
if [ -z "$ownsLine" ] || [ -z "$clickLine" ] || [ "$ownsLine" -ge "$clickLine" ]; then
  echo "test-bridge-safety: handleAlphiClick must ask mcp::alphiOwnsPoint before alphiClicked (a box under Alphi is never flow)" >&2
  exit 1
fi
brakeLine="$(awk '/^bool CanvasMcpLayer::handleAlphiClick/ { f = 1 } f && /alphiBrake\(/ { print NR; exit } f && /^}/ { exit }' "$flowSrc")"
if [ -z "$brakeLine" ] || [ "$ownsLine" -ge "$brakeLine" ]; then
  echo "test-bridge-safety: handleAlphiClick must ask mcp::alphiOwnsPoint before alphiBrake (a box under Alphi is never flow)" >&2
  exit 1
fi
# 1a0 live check b': alphiBoxes gives the VISIBLE box (Object::getBounds() carries PlugData's invisible 6 px margin;
# with the 8 px pad that blocked 14 px around every box, and a folded Alphi on a dense patch was clickable only at
# its top strip).
if ! awk '/^std::vector<mcp::ApprovalRect> CanvasMcpLayer::alphiBoxes/ { f = 1 } f && /push_back/ { print; exit } f && /^}/ { exit }' "$flowSrc" | grep -q "getBounds().reduced(Object::margin)"; then
  echo "test-bridge-safety: CanvasMcpLayer::alphiBoxes must push o->getBounds().reduced(Object::margin) (the visible box)" >&2
  exit 1
fi
# 1a0 review H2 / M2: the 400 ms gate. A move (AlphiDock::apply) restarts it on the very next line; it is set only to 0
# or by mcp::alphiGateAfterPaint (which starts it only in a render that redrew Alphi's whole frame).
if ! awk '/mcpAlphiDock\.apply\(/ { getline; print; exit }' "$flowSrc" | grep -q "mcpAlphiShownSinceMs = 0;"; then
  echo "test-bridge-safety: a move of Alphi (mcpAlphiDock.apply) must restart its click gate (mcpAlphiShownSinceMs = 0) on the next line" >&2
  exit 1
fi
gateSets="$(grep -rn "mcpAlphiShownSinceMs = " "$root"/Source --include=*.cpp --include=*.h | codeOnly \
  | grep -v "mcpAlphiShownSinceMs = 0;" | grep -v "mcpAlphiShownSinceMs = mcp::alphiGateAfterPaint(" || true)"
if [ -n "$gateSets" ]; then
  echo "$gateSets" >&2
  echo "test-bridge-safety: Alphi's click gate is set outside mcp::alphiGateAfterPaint (only 0 or the paint rule)" >&2
  exit 1
fi
# 1a0 review H2: a move is decided by the timer (alphiVisibilityTick); a paint decides only the first place.
decides="$(grep -n "mcpAlphiDock\.decide(" "$flowSrc" | codeOnly | cut -d: -f1)"
for n in $decides; do
  fn="$(awk -v n="$n" 'NR <= n && /^[a-zA-Z].*CanvasMcpLayer::[a-zA-Z]+\(/ { h = $0 } NR == n { print h; exit }' "$flowSrc")"
  prev="$(sed -n "$((n - 1))p" "$flowSrc")"
  case "$fn" in
    *"CanvasMcpLayer::alphiVisibilityTick("*) ;;
    *"CanvasMcpLayer::updateAlphiDock("*) echo "$prev" | grep -q '!mcpAlphiDock.placed()' || { echo "test-bridge-safety: updateAlphiDock decides a move inside a paint (only the first place may be decided there)" >&2; exit 1; } ;;
    *) echo "test-bridge-safety: mcpAlphiDock.decide called in '$fn' (only the timer decides a move)" >&2; exit 1 ;;
  esac
done
# P12a.1: the flow verbs are get and off only (no verb turns it on; the offer verb is gone for good).
flowVerbs="$(grep -o 'MCP_VERB([a-z]*, "[a-z_]*flow[a-z_]*"' "$root/Source/Pd/MCPBridgeVerbs.inc" | sort | tr '\n' ' ')"
if [ "$flowVerbs" != 'MCP_VERB(pd, "flow" MCP_VERB(pd, "flow_mode_get" MCP_VERB(pd, "flow_mode_off" ' ]; then
  echo "test-bridge-safety: flow verbs are '$flowVerbs'; only /pd/flow (layout), flow_mode_get and flow_mode_off may exist" >&2
  exit 1
fi
if grep -rnE "offerFlowMode|flow_mode_offer|flowOffer" "$root"/Source --include=*.cpp --include=*.h --include=*.inc | codeOnly | grep -q .; then
  grep -rnE "offerFlowMode|flow_mode_offer|flowOffer" "$root"/Source --include=*.cpp --include=*.h --include=*.inc | codeOnly >&2
  echo "test-bridge-safety: the flow offer path is back (P12a.1 removed it: only the click on Alphi turns flow on)" >&2
  exit 1
fi
callers="$(grep -rnw "answerApprovalChip" "$root"/Source --include=*.cpp --include=*.h | codeOnly \
  | grep -v "Source/Mcp/McpHost.h:[0-9]*:    bool answerApprovalChip(std::string const& nonce, bool yes, std::int64_t nowMs);$" \
  | grep -v "Source/Mcp/McpFlow.cpp:[0-9]*:bool McpHost::answerApprovalChip(std::string const& nonce, bool yes, std::int64_t nowMs)$" \
  | grep -v "Source/Mcp/CanvasMcpApproval.cpp:[0-9]*:    if (!canvas.pd->getMcp()->answerApprovalChip(nonce.toStdString(), yes, nowMs())) return;$" || true)"
if [ -n "$callers" ]; then
  echo "$callers" >&2
  echo "test-bridge-safety: answerApprovalChip is named outside the canvas input (CanvasMcpApproval.cpp)" >&2
  exit 1
fi
# Alphi drop 1 (W): what Alphi says is display only. The say code (the board McpAlphiSay.h and CanvasMcpAlphiSay.cpp,
# which holds the /pd/alphi_say handler; from F3 also the strip, motion and say drawing: CanvasMcpAlphiStrip.cpp,
# McpAlphiStrip.h, McpAlphiMotion.h) never names the flow switch or the
# click on Alphi, comments included, and never takes the Pd lock or queues Pd work. Only the handler writes the board.
sayFiles=("$root/Source/Mcp/McpAlphiSay.h" "$root/Source/Mcp/CanvasMcpAlphiSay.cpp" "$root/Source/Mcp/CanvasMcpAlphiStrip.cpp" \
  "$root/Source/Mcp/McpAlphiStrip.h" "$root/Source/Mcp/McpAlphiMotion.h")
for f in "${sayFiles[@]}"; do
  if [ ! -f "$f" ]; then
    echo "test-bridge-safety: $f is missing (the say code moved: update this check)" >&2
    exit 1
  fi
done
if grep -nwE "alphiClicked|artistTurnOn|handleAlphiClick" "${sayFiles[@]}"; then
  echo "test-bridge-safety: the say/strip code names the flow switch or the click on Alphi (display only: it never may)" >&2
  exit 1
fi
if grep -nwE "sys_lock|lockAudioThread|pdWait|enqueuePd|enqueueFunctionAsync" "${sayFiles[@]}" | codeOnly | grep -q .; then
  grep -nwE "sys_lock|lockAudioThread|pdWait|enqueuePd|enqueueFunctionAsync" "${sayFiles[@]}" >&2
  echo "test-bridge-safety: the say code takes the Pd lock or queues Pd work (/pd/alphi_say never touches Pd)" >&2
  exit 1
fi
# Alphi drop 1 F0: the say, strip and motion code (pulse, sparks, the D4 timer) never touches the 400 ms click gate or
# the dock: a repaint never restarts the gate and never moves Alphi (Tests/mcp/alphi_gate_test.cpp).
if grep -nE "mcpAlphiShownSinceMs|mcpAlphiDock|alphiGateAfterPaint" "${sayFiles[@]}" | codeOnly | grep -q .; then
  grep -nE "mcpAlphiShownSinceMs|mcpAlphiDock|alphiGateAfterPaint" "${sayFiles[@]}" | codeOnly >&2
  echo "test-bridge-safety: the say/strip/motion code touches Alphi's click gate or the dock (a repaint never may)" >&2
  exit 1
fi
sayWriters="$(grep -rn "alphiSay\.apply(" "$root"/Source --include=*.cpp --include=*.h --include=*.inc | codeOnly \
  | grep -v "Source/Mcp/CanvasMcpAlphiSay.cpp:" || true)"
if [ -n "$sayWriters" ]; then
  echo "$sayWriters" >&2
  echo "test-bridge-safety: the say board is written outside the /pd/alphi_say handler (CanvasMcpAlphiSay.cpp)" >&2
  exit 1
fi
bash "$root/Tests/mcp/check_seam_test.sh"
