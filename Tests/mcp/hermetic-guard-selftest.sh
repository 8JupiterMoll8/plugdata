#!/usr/bin/env bash
# P11.6 review N4: the bridge-test binary must refuse (HERMETIC BREACH, exit 3) BEFORE it creates anything when its
# scratch HOME is unsafe. Every case lives in a temp dir: "the real home" is played by a temp dir handed to the guard
# as MCP_TESTS_GUARD_EXTRA_HOME (an ADDITIONAL home to protect; the guard always protects the getpwuid home too), so
# even a broken guard could only write into the temp dir, never the owner's home.
#   1. MCP_TESTS_HOME = the protected home itself.
#   2. a scratch HOME whose Documents is a symlink.
#   3. a scratch HOME that CONTAINS the protected home (its parent).
# Usage: Tests/mcp/hermetic-guard-selftest.sh [path/to/mcp_bridge_tests]
set -uo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
bin="${1:-$root/build/mcp_bridge_tests}"
[ -x "$bin" ] || { echo "hermetic-guard-selftest: no binary at $bin" >&2; exit 2; }
tmp="$(mktemp -d "${TMPDIR:-/tmp}/mcp_guard_selftest.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
fake_home="$tmp/parent/fakehome"
mkdir -p "$fake_home/Documents/plugdata" "$tmp/linked" "$tmp/elsewhere/plugdata"
ln -s "$tmp/elsewhere" "$tmp/linked/Documents"
fail=0
expect_refusal() { # <label> <home>
    local out code
    out="$(env -u XDG_DOCUMENTS_DIR -u XDG_DATA_HOME -u XDG_CONFIG_HOME -u XDG_CACHE_HOME \
        HOME="$2" MCP_TESTS_HOME="$2" MCP_TESTS_GUARD_EXTRA_HOME="$fake_home" DISPLAY="${DISPLAY:-:1}" timeout 30 "$bin" 2>&1)"
    code=$?
    if [ "$code" -eq 3 ] && grep -q "HERMETIC BREACH" <<<"$out"; then
        echo "  ok   $1 refused"
    else
        echo "  FAIL $1: exit $code"; echo "$out" | tail -3; fail=1
    fi
}
expect_refusal "the protected home itself" "$fake_home"
expect_refusal "a symlinked Documents" "$tmp/linked"
expect_refusal "a home containing the protected home" "$tmp/parent"
[ "$fail" -eq 0 ] && echo "hermetic-guard-selftest: OK" || echo "hermetic-guard-selftest: FAILED"
exit "$fail"
