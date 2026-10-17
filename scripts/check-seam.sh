#!/usr/bin/env bash
# Seam check: how much of plugdata's own code does the MCP fork touch?
#
#   scripts/check-seam.sh --report   per upstream-owned file: hunks, +/- lines, MCP-HOOK hunks (always exit 0)
#   scripts/check-seam.sh --gate     the report, then exit 1 when the seam rules are broken
#
# Upstream-owned = a file that exists at the base in docs/mcp-bridge/UPSTREAM_BASE and
# that the fork modified under Source/. Fork-only files (Source/Pd/MCPBridge*, ...) are
# not counted. Hunks come from `git diff --diff-algorithm=histogram -w -U0`: the default
# algorithm misaligns moved code (280 hunks instead of 215 at the P3.2 start).
#
# Rules (docs/mcp-bridge/SEAM.md):
#   a. every hunk in an upstream file carries `MCP-HOOK(<name>)` on an added line, or
#      its file is listed under "Carried patches" in SEAM.md;
#   b. Source/Pd/MCPBridge* includes no plugdata GUI header.
set -uo pipefail

mode="${1:---report}"
if [[ "$mode" != "--report" && "$mode" != "--gate" ]]; then
  echo "usage: check-seam.sh [--report|--gate]" >&2
  exit 3
fi

cd "$(git rev-parse --show-toplevel)" || exit 3
base_file=docs/mcp-bridge/UPSTREAM_BASE
seam_file=docs/mcp-bridge/SEAM.md
if [[ ! -f "$base_file" ]]; then
  echo "check-seam: $base_file is missing" >&2
  exit 3
fi
base="$(tr -d '[:space:]' < "$base_file")"

# GUI headers the bridge must not include (plan item 2.3).
gui_headers='PluginEditor|Toolbar|Sidebar|Canvas|CanvasViewport|TabComponent|Object|Connection|ObjectBase|AllGuis|GemCapture|Fonts|Dialogs'

# file <TAB> hunks <TAB> plus <TAB> minus <TAB> tagged-hunks
stats="$(git diff --diff-algorithm=histogram -w -U0 --diff-filter=M "$base" HEAD -- Source | awk '
  function flush() {
    if (in_hunk && hunk_tagged) tagged_hunks++
    if (file != "") printf "%s\t%d\t%d\t%d\t%d\n", file, hunks, plus, minus, tagged_hunks
  }
  /^diff --git / { flush(); file = $4; sub(/^b\//, "", file); hunks = plus = minus = tagged_hunks = 0; in_hunk = 0; hunk_tagged = 0; next }
  /^@@/ {
    if (in_hunk && hunk_tagged) tagged_hunks++
    hunks++; in_hunk = 1; hunk_tagged = 0; next
  }
  in_hunk && /^\+/ { plus++; if (index($0, "MCP-HOOK(") > 0) hunk_tagged = 1; next }
  in_hunk && /^-/  { minus++; next }
  END { flush() }
')"

echo "Seam report: fork HEAD vs base $(git rev-parse --short "$base") (histogram, -w, -U0, upstream-owned files)"
printf '%-46s %6s %8s %7s %6s\n' FILE HUNKS "+LINES" "-LINES" HOOKS
sort -t$'\t' -k2,2nr <<<"$stats" | awk -F'\t' 'NF == 5 { printf "%-46s %6d %8d %7d %6d\n", $1, $2, $3, $4, $5 }'
awk -F'\t' 'NF == 5 { f++; h += $2; p += $3; m += $4; t += $5 } END { printf "%-46s %6d %8d %7d %6d   (%d files)\n", "TOTAL", h, p, m, t, f }' <<<"$stats"

# Files listed in the "Carried patches" table of SEAM.md: | `path` | reason |
carried=""
if [[ -f "$seam_file" ]]; then
  carried="$(awk '/^## /{ in_sec = ($0 ~ /^## Carried patches/) } in_sec && /^\|[[:space:]]*`/ { s = $0; sub(/^\|[[:space:]]*`/, "", s); sub(/`.*/, "", s); print s }' "$seam_file")"
fi

problems=0
while IFS=$'\t' read -r file hunks _plus _minus tagged; do
  [[ -z "$file" ]] && continue
  untagged=$((hunks - tagged))
  if [[ $untagged -gt 0 ]] && ! grep -qxF "$file" <<<"$carried"; then
    echo "gate: FAIL $file has $untagged untagged hunk(s) and is not in the SEAM.md carried list"
    problems=$((problems + 1))
  fi
done <<<"$stats"

for f in Source/Pd/MCPBridge*; do
  [[ -f "$f" ]] || continue
  while IFS= read -r header; do
    echo "gate: FAIL $f includes GUI header $header"
    problems=$((problems + 1))
  done < <(grep -oE "^#include[[:space:]]+[\"<]([^\">]*/)?($gui_headers)\\.h[\">]" "$f" | grep -oE "($gui_headers)\\.h")
done

if [[ $problems -eq 0 ]]; then
  echo "gate: OK"
else
  echo "gate: $problems problem(s)"
fi
[[ "$mode" == "--report" || $problems -eq 0 ]]
