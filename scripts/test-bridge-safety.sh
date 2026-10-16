#!/usr/bin/env bash
# Build and run the MCP bridge safety unit test (no JUCE, no Pd needed).
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror -o "$out/bridge_safety_test" "$root/Tests/mcp/bridge_safety_test.cpp"
"$out/bridge_safety_test"
