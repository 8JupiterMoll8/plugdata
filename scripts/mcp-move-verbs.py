#!/usr/bin/env python3
"""Moves verb branches out of a handle<Domain>Domain function of Source/Pd/MCPBridge.cpp
into member functions of a new MCPBridgeCmd<Name>.cpp (Plan 3 P3.3, pure moves).

  scripts/mcp-move-verbs.py deps   --func handlePdDomain --verbs a,b,c
  scripts/mcp-move-verbs.py move   --func handlePdDomain --domain pd --out Source/Pd/MCPBridgeCmdSystem.cpp \
        --title "System" --verbs ping,clear_console --alias doc=doc_get

`deps` lists the file-scope names of MCPBridge.cpp that the chosen branches use, and who else uses them.
`move` cuts each branch (its leading comment lines included) from the function, writes it as
`void MCPBridge::<domain><Verb>(const juce::String& action, const juce::OSCMessage& msg)` with the body
dedented by 4, appends the rows to Source/Pd/MCPBridgeVerbs.inc and declares nothing else: the header
reads the .inc. Run `check` afterwards to compare the line multisets.
"""
import argparse, collections, json, re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "Source/Pd/MCPBridge.cpp"
INC = ROOT / "Source/Pd/MCPBridgeVerbs.inc"


def strip_code(text):
    """Same length as text, with comments, string and char literal contents blanked (newlines kept)."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i)); i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j])); i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(c + " " * (j - i - 2) + c if j - i >= 2 else c); i = j
        else:
            out.append(c); i += 1
    return "".join(out)


def find_function(lines, func):
    start = next(i for i, l in enumerate(lines) if l.startswith(f"void MCPBridge::{func}("))
    end = next(i for i in range(start + 1, len(lines)) if lines[i] == "}")
    return start, end


def branches(lines, start, end):
    """[(first_line_idx_incl_comments, if_idx, close_idx, [labels])] at depth 1 of the function."""
    code = strip_code("\n".join(lines)).split("\n")
    depth, found = 0, []
    for i in range(start, end + 1):
        m = re.match(r'^    (?:\} else )?if \((action == "[^"]+"(?: \|\| action == "[^"]+")*)\) \{\s*$', lines[i])
        if m and depth == 1:
            labels = re.findall(r'"([^"]+)"', m.group(1))
            d, j = 0, i
            while True:
                d += code[j].count("{") - code[j].count("}")
                if d == 0 and j > i: break
                if d == 0 and j == i and "{" in code[j]: break
                j += 1
            first = i
            while first - 1 > start and lines[first - 1].lstrip().startswith("//"):
                first -= 1
            found.append((first, i, j, labels))
        depth += code[i].count("{") - code[i].count("}")
    return found


def camel(domain, verb):
    return domain + "".join(p[:1].upper() + p[1:] for p in re.split(r"[_]+", verb))


def file_scope_names(lines, func_ranges):
    names = {}
    skip = set()
    for a, b in func_ranges:
        skip.update(range(a, b + 1))
    pat = re.compile(r"^(?:static\s+|inline\s+|constexpr\s+|extern\s+)*(?:const\s+)?[\w:<>,\*&\s]+?[\s\*&]+(\w+)\s*(?:\(|=|\{|;)")
    for i, l in enumerate(lines):
        if i in skip or not l or l[0] in " \t/#}{" : continue
        m = re.match(r"^(?:struct|class)\s+(\w+)", l)
        if m: names.setdefault(m.group(1), i + 1); continue
        if l.startswith(("static ", "inline ", "constexpr ")):
            m = re.match(r"^(?:static |inline |constexpr |const )+[\w:<>,\*&\s]*?(\w+)\s*(?:\(|=|\{|;)", l)
            if m: names.setdefault(m.group(1), i + 1)
    return names


def all_member_ranges(lines):
    out = []
    for i, l in enumerate(lines):
        if re.match(r"^[\w:<>,\*&\s]+ MCPBridge::\w+\(", l) or re.match(r"^MCPBridge::\w+\(", l):
            j = next(k for k in range(i + 1, len(lines)) if lines[k] == "}")
            out.append((i, j))
    return out


def cmd_deps(a):
    lines = SRC.read_text().split("\n")
    start, end = find_function(lines, a.func)
    brs = [b for b in branches(lines, start, end) if set(b[3]) & set(a.verbs.split(","))]
    members = all_member_ranges(lines)
    names = file_scope_names(lines, members)
    moved = set()
    for f, i, j, _ in brs: moved.update(range(f, j + 1))
    body_ids = collections.Counter(); rest_ids = collections.Counter()
    for k, l in enumerate(strip_code("\n".join(lines)).split("\n")):
        ids = re.findall(r"[A-Za-z_]\w*", l)
        (body_ids if k in moved else rest_ids).update(ids)
    print(f"{len(brs)} branches, {len(moved)} lines")
    for n, line in sorted(names.items(), key=lambda x: x[1]):
        if body_ids[n]:
            print(f"  {n:34s} defined @{line:5d}  used in moved: {body_ids[n]:3d}  elsewhere: {rest_ids[n] - 1 if rest_ids[n] else 0}")


def cmd_move(a):
    lines = SRC.read_text().split("\n")
    start, end = find_function(lines, a.func)
    want = a.verbs.split(",")
    aliases = dict(x.split("=") for x in a.alias.split(",")) if a.alias else {}
    brs = branches(lines, start, end)
    chosen = []
    for w in want:
        hit = [b for b in brs if w in b[3]]
        if len(hit) != 1: sys.exit(f"verb {w}: {len(hit)} branches")
        chosen.append(hit[0])
    for f, i, j, labels in chosen:
        if lines[j - 1].strip() != "return;": sys.exit(f"branch {labels} does not end in return;")
    out_funcs, rows, cut = [], [], set()
    for (f, i, j, labels), w in zip(chosen, want):
        h = camel(a.domain, labels[0])
        body = [l[4:] if l.startswith("    ") else l for l in lines[i + 1:j]]
        lead = [l for l in lines[f:i]]
        out_funcs.append("\n".join(lead + [f"void MCPBridge::{h}(const juce::String& action, const juce::OSCMessage& msg)", "{"] + body + ["}"]))
        rows.append(f'MCP_VERB({a.domain}, "{labels[0]}", {h})')
        for extra in labels[1:]:
            rows.append(f'MCP_ALIAS({a.domain}, "{extra}", {h})')
        cut.update(range(f, j + 1))
    for alias, target in aliases.items():
        rows.append(f'MCP_ALIAS({a.domain}, "{alias}", {camel(a.domain, target)})')
    # drop the cut ranges; turn a leading `} else if` into a plain `if` is not needed (every branch returns)
    kept = [l for k, l in enumerate(lines) if k not in cut]
    SRC.write_text("\n".join(kept))
    Path(ROOT / a.out).write_text(Path(ROOT / a.out).read_text() + "\n" + "\n\n".join(out_funcs) + "\n" if Path(ROOT / a.out).exists() else "\n\n".join(out_funcs) + "\n")
    inc = INC.read_text()
    marker = "#undef MCP_SOURCE"
    block = f"// ── {a.title} ──\n" + "\n".join(rows) + "\n\n"
    INC.write_text(inc.replace(marker, block + marker, 1))
    print(f"moved {len(chosen)} branches, {len(cut)} lines -> {a.out}")


def cmd_check(a):
    """Line multisets (whitespace-normalized) of MCPBridge.cpp at --ref against the working copies of
    MCPBridge.cpp plus --files. Prints old-minus-new and new-minus-old: for a pure move the first holds only
    the cut `if (action == ...) {` lines and the second only function headers, braces and includes."""
    import subprocess
    old = subprocess.check_output(["git", "show", f"{a.ref}:Source/Pd/MCPBridge.cpp"], cwd=ROOT).decode().split("\n")
    new = SRC.read_text().split("\n")
    for f in (a.files or "").split(","):
        if f: new += (ROOT / f).read_text().split("\n")
    norm = lambda l: " ".join(l.split())
    co = collections.Counter(norm(l) for l in old if l.strip())
    cn = collections.Counter(norm(l) for l in new if l.strip())
    print("--- old minus new")
    for l, c in sorted((co - cn).items()): print(f"{c:3d}  {l}")
    print("--- new minus old")
    for l, c in sorted((cn - co).items()): print(f"{c:3d}  {l}")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("cmd", choices=["deps", "move", "check"])
    p.add_argument("--func"); p.add_argument("--ref", default="HEAD"); p.add_argument("--files")
    p.add_argument("--verbs")
    p.add_argument("--domain"); p.add_argument("--out"); p.add_argument("--title"); p.add_argument("--alias")
    a = p.parse_args()
    if a.cmd != "check" and not (a.func and a.verbs): p.error("--func and --verbs are required")
    {"deps": cmd_deps, "move": cmd_move, "check": cmd_check}[a.cmd](a)


main()
