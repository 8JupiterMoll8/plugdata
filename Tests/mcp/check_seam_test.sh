#!/usr/bin/env bash
# Tests scripts/check-seam.sh on a throwaway git repo (no JUCE, no Pd needed).
set -uo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
script="$root/scripts/check-seam.sh"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

passed=0
failed=0
expect() { # <expected exit> <name> <command...>
  local want="$1" name="$2"
  shift 2
  out="$("$@" 2>&1)"
  got=$?
  if [[ $got -eq $want ]]; then
    echo "  ok   $name"
    passed=$((passed + 1))
  else
    echo "  FAIL $name: exit $got, wanted $want"
    echo "$out" | sed 's/^/       /'
    failed=$((failed + 1))
  fi
}
expect_in_output() { # <regex> <name>
  if grep -Eq "$1" <<<"$out"; then
    echo "  ok   $2"
    passed=$((passed + 1))
  else
    echo "  FAIL $2: no match for /$1/ in:"
    echo "$out" | sed 's/^/       /'
    failed=$((failed + 1))
  fi
}

cd "$work"
git init -q
git config user.email test@example.com
git config user.name test
mkdir -p Source/Pd docs/mcp-bridge
printf 'a\nb\nc\nd\ne\n' > Source/Canvas.cpp
git add -A
git commit -q -m base
git rev-parse HEAD > docs/mcp-bridge/UPSTREAM_BASE
seam() { printf '# Seam\n\n## Carried patches\n\n| File | Why |\n|---|---|\n%s\n\n## Known upstream behaviour\n\nnone\n' "$1" > docs/mcp-bridge/SEAM.md; }
commit() { git add -A && git commit -q -m "$1"; }

echo "check-seam.sh"
seam ""
commit "metadata"
expect 0 "no change: gate passes" "$script" --gate

printf 'a\nB\nc\nd\ne\nnew line\n' > Source/Canvas.cpp
commit "untagged edit"
expect 1 "an untagged hunk in an upstream file fails the gate" "$script" --gate
expect_in_output 'Source/Canvas.cpp' "the gate names the file"
expect 0 "--report never fails" "$script" --report
expect_in_output 'Source/Canvas.cpp[[:space:]]+2[[:space:]]+' "the report counts the two hunks of the file"

seam '| `Source/Canvas.cpp` | test reason |'
commit "carried"
expect 0 "a carried file passes the gate" "$script" --gate

seam ""
printf 'a\nB\nc\nd\ne\nhook(); // MCP-HOOK(paint)\n' > Source/Canvas.cpp
commit "tagged"
expect 1 "a tag on only one of two hunks still fails" "$script" --gate

printf 'a\nb\nc\nd\ne\nhook(); // MCP-HOOK(paint)\n' > Source/Canvas.cpp
commit "single tagged hunk"
expect 0 "a hunk with an MCP-HOOK tag passes" "$script" --gate
expect 0 "--report on a tagged file" "$script" --report
expect_in_output 'Source/Canvas.cpp[[:space:]]+1[[:space:]]+1[[:space:]]+0[[:space:]]+1$' "the report counts the tagged hunk"

printf 'x\ny\n' > Source/Other.cpp
git add -A && git commit -q -m "other file" && git rev-parse HEAD~1 > /dev/null
printf '#include "Canvas.h"\n// bridge\n' > Source/Pd/MCPBridge.cpp
commit "gui include"
expect 1 "the bridge including a GUI header fails the gate" "$script" --gate
expect_in_output 'Canvas\.h' "the gate names the header"

printf '#include "Utility/Config.h"\n#include <juce_core/juce_core.h>\n// bridge\n' > Source/Pd/MCPBridge.cpp
commit "non-gui includes"
expect 0 "non-GUI includes pass" "$script" --gate

printf '#include "Sidebar/Sidebar.h"\n' > Source/Pd/MCPBridge.h
commit "gui include in the header"
expect 1 "a GUI header in MCPBridge.h also fails (directory prefix is ignored)" "$script" --gate

echo "$passed passed, $failed failed"
[[ $failed -eq 0 ]]
