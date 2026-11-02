#!/usr/bin/env bash
# Build and run the MCP bridge unit tests (no JUCE, no Pd needed).
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
for t in bridge_safety_test handoff_test json_escape_test identity_rename_test identity_service_test registry_test worker_test v2_codec_test v2_router_test v2_store_test trace_taps_test output_ceiling_test ceiling_console_test approval_state_test; do
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
bash "$root/Tests/mcp/check_seam_test.sh"
