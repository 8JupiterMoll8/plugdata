#!/usr/bin/env bash
# Build and run the MCP bridge unit tests (no JUCE, no Pd needed).
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
for t in bridge_safety_test handoff_test json_escape_test identity_rename_test identity_service_test registry_test worker_test v2_codec_test v2_router_test v2_store_test trace_taps_test output_ceiling_test ceiling_console_test tab_token_test approval_state_test flow_provenance_test visible_tab_test alphi_test; do
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
  | grep -v "Source/Mcp/CanvasMcpFlow.cpp:[0-9]*:    canvas.pd->getMcp()->alphiClicked(now);$" || true)"
if [ -n "$alphiCallers" ]; then
  echo "$alphiCallers" >&2
  echo "test-bridge-safety: alphiClicked is named outside the canvas click (CanvasMcpLayer::handleAlphiClick)" >&2
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
bash "$root/Tests/mcp/check_seam_test.sh"
