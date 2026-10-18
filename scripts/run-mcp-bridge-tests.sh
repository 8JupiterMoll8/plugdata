#!/usr/bin/env bash
# Runs the bridge handler tests (Plan 3 P3.3 S10) on a scratch HOME.
#
#   cmake -S . -B build -DMCP_BRIDGE_TESTS=ON && cmake --build build --target mcp_bridge_tests
#   scripts/run-mcp-bridge-tests.sh [path/to/mcp_bridge_tests]
#
# The binary is $1, else $MCP_BRIDGE_TESTS_BIN, else build/mcp_bridge_tests next to this checkout.
# HOME is a scratch directory ($MCP_TESTS_HOME, default $TMPDIR/mcp_bridge_tests_home, kept between runs so only
# the first run unpacks plugdata's resources): plugdata migrates and deletes settings under the real HOME.
# No socket is opened by the tests; the processor's own bridge is stopped right after it starts, and if
# another plugdata holds UDP 9000 the processor only logs that it could not bind.
# The tests need a display for JUCE's look-and-feel (DISPLAY, XAUTHORITY as for the app).
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bin="${1:-${MCP_BRIDGE_TESTS_BIN:-$root/build/mcp_bridge_tests}}"
[ -x "$bin" ] || { echo "run-mcp-bridge-tests: no binary at $bin (build target mcp_bridge_tests with -DMCP_BRIDGE_TESTS=ON)" >&2; exit 2; }
home="${MCP_TESTS_HOME:-${TMPDIR:-/tmp}/mcp_bridge_tests_home}"
mkdir -p "$home"
auth="${XAUTHORITY:-/run/user/$(id -u)/gdm/Xauthority}"
[ -f "$auth" ] && [ "$auth" != "$home/.Xauthority" ] && cp "$auth" "$home/.Xauthority" 2>/dev/null || true
export MCP_TESTS_HOME="$home" XAUTHORITY="$home/.Xauthority" DISPLAY="${DISPLAY:-:1}"
exec timeout "${MCP_TESTS_TIMEOUT:-300}" "$bin"
