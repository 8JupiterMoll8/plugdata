#!/usr/bin/env python3
"""Static thread map of the MCP bridge handlers (Plan 3 P3.4 S0).

For every row of Source/Pd/MCPBridgeVerbs.inc and Source/Mcp/McpSysVerbs.inc,
list what the handler does that matters for the execution model:

  lock     sys_lock() calls (Pd lock taken on the calling thread)
  pdq      enqueueFunctionAsync (upstream functionQueue: runs on whichever
           thread drains it next, without sys_lock)
  async    MessageManager::callAsync (message thread)
  wait     waitOrAbandon / Thread::sleep / cv.wait (blocks the calling thread)
  drain    sendMessagesFromQueue (runs queued jobs on the calling thread)
  editors  getEditors() (JUCE UI objects; message thread only)
  gui      McpGuiHost methods called (OFF = a message-thread-only method called off the message thread)
  sym      gensym / generateSymbol (permanent Pd symbols)
  capture  lambdas passed to enqueueFunctionAsync/callAsync that capture by
           reference ([&...]) or capture a raw Pd pointer local
  sys      mcp_* selectors reached through receiveSysMessage

"direct" = in the handler body; "via" = reached through functions it calls,
matched by NAME over every function defined in the MCP files (an
over-approximation: a name defined twice counts both).

Usage: scripts/mcp-thread-map.py [--json out.json] [--root .]
"""
import argparse
import json
import os
import re
import sys

PTR_TYPES = r"(?:t_canvas|t_glist|t_gobj|t_object|t_outconnect|t_signal|t_garray|t_text)"
FILES_GLOBS = [("Source/Pd", r"^MCPBridge.*\.(cpp|h|inc)$"), ("Source/Mcp", r".*\.(cpp|h)$")]

KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "catch", "static_cast",
            "reinterpret_cast", "const_cast", "dynamic_cast", "decltype", "alignof", "new",
            "delete", "throw", "else", "do", "case", "default"}


def strip_comments_and_strings(src):
    """Blank out comments and string/char literal CONTENTS (keep quotes and
    newlines) so brace matching and pattern search see code only. Returns
    (code, strings) where strings maps the literal's start offset to its text."""
    out = []
    i, n = 0, len(src)
    strings = {}
    while i < n:
        c = src[i]
        if src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", src[i:j]))
            i = j
        elif src.startswith('R"', i):
            m = re.match(r'R"([^(]*)\(', src[i:])
            if m:
                end = src.find(")" + m.group(1) + '"', i)
                end = n if end < 0 else end + len(m.group(1)) + 2
                out.append(re.sub(r"[^\n]", " ", src[i:end]))
                i = end
            else:
                out.append(c)
                i += 1
        elif c in "\"'":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            strings[i] = src[i + 1:j]
            out.append(c + " " * (j - i - 1) + (c if j < n else ""))
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out), strings


def find_functions(path, code, raw):
    """Yield (qualified_name, short_name, body_code, body_raw) for every
    function definition: an identifier( ... ) [const] [noexcept] { at any depth
    that is not a control statement or a lambda."""
    head = re.compile(r"([A-Za-z_][\w:]*(?:::~?[A-Za-z_]\w*)?)\s*\(([^;{}()]|\([^;{}()]*\))*\)\s*(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?(?:->\s*[\w:<>*&\s]+)?\{")
    for m in head.finditer(code):
        name = m.group(1)
        short = name.split("::")[-1]
        if short in KEYWORDS:
            continue
        # skip calls used as initialisers: previous non-space char must end a
        # declaration (type name, *, &, >) or a qualifier, not '=' ',' '(' 'return'
        k = m.start() - 1
        while k >= 0 and code[k] in " \t\n":
            k -= 1
        if k >= 0 and code[k] in "=,(!?:+-/%|^~[":
            if not (code[k] == ":" and code[k - 1] == ":"):
                continue
        prev_word = re.search(r"(\w+)\s*$", code[max(0, k - 40):k + 1])
        if prev_word and prev_word.group(1) in ("return", "new", "else"):
            continue
        open_brace = m.end() - 1
        depth, j = 0, open_brace
        while j < len(code):
            if code[j] == "{":
                depth += 1
            elif code[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        yield name, short, code[open_brace:j + 1], raw[open_brace:j + 1]


def strip_comments_and_strings_keep(src):
    """Remove // and /* */ comments only (the .inc tables need their strings)."""
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def lambda_captures(body):
    """Captures of lambdas passed to enqueueFunctionAsync / callAsync."""
    res = []
    for m in re.finditer(r"(enqueueFunctionAsync|callAsync)\s*\(\s*(?:[\w&*]+\s*,\s*)?\[([^\]]*)\]", body):
        res.append((m.group(1), m.group(2).strip()))
    return res


def lambda_spans(body, call):
    """(start, end) of the bodies of lambdas passed to `call`(...)."""
    spans = []
    for m in re.finditer(r"\b" + call + r"\s*\(", body):
        lb = body.find("[", m.end())
        if lb < 0 or body[m.end():lb].strip(" \t\n,&*\w") != "":
            continue
        ob = body.find("{", lb)
        if ob < 0:
            continue
        depth, j = 0, ob
        while j < len(body):
            if body[j] == "{":
                depth += 1
            elif body[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        spans.append((ob, j + 1))
    return spans


def mask(body, spans):
    out = list(body)
    for a, b in spans:
        for i in range(a, b):
            if out[i] != "\n":
                out[i] = " "
    return "".join(out)


def facts_of(body_code, body_raw, ptr_locals, off_methods):
    msg_spans = lambda_spans(body_code, "callAsync") + lambda_spans(body_code, "onMessageThread")
    on_msg = "".join(body_code[a:b] for a, b in msg_spans)
    full_code = body_code
    body_code = mask(body_code, msg_spans)   # message-thread lambda bodies do not count as off-thread
    f = {
        "lock": len(re.findall(r"\bsys_lock\s*\(", body_code)),
        "pdq": len(re.findall(r"\benqueueFunctionAsync\s*\(", body_code)),
        "async": len(re.findall(r"\bcallAsync\s*\(", body_code)),
        "wait": len(re.findall(r"\bwaitOrAbandon\s*\(|Thread::sleep\s*\(|\bcv\.wait\s*\(|sleep_for\s*\(", body_code)),
        "drain": len(re.findall(r"\bsendMessagesFromQueue\s*\(", body_code)),
        "editors": len(re.findall(r"\bgetEditors\s*\(", body_code)),
        "sym": len(re.findall(r"\bgensym\s*\(|\bgenerateSymbol\s*\(", body_code)),
        "gui": sorted(set(re.findall(r"gui\(\)\s*(?:->|\.)\s*(\w+)\s*\(", body_code))),
        "capture": [],
        "sys": sorted(set(re.findall(r'receiveSysMessage\s*\(\s*"(mcp_\w+)"', body_raw))),
    }
    f["msgEditors"] = len(re.findall(r"\bgetEditors\s*\(", on_msg)) + len(re.findall(r"gui\(\)\s*(?:->|\.)", on_msg))
    for kind, cap in lambda_captures(full_code):
        names = [c.strip().lstrip("&=").split("=")[0].strip() for c in cap.split(",") if c.strip()]
        byref = any(c.strip().startswith("&") for c in cap.split(","))
        ptrs = [nm for nm in names if nm in ptr_locals]
        if byref or ptrs:
            f["capture"].append({"to": kind, "captures": cap, "rawPtrs": ptrs, "byRef": byref})
    f["guiOff"] = sorted(set(f["gui"]) & off_methods)
    return f


def merge(a, b):
    out = dict(a)
    for k in ("lock", "pdq", "async", "wait", "drain", "editors", "sym", "msgEditors"):
        out[k] = a.get(k, 0) + b.get(k, 0)
    for k in ("gui", "guiOff", "sys"):
        out[k] = sorted(set(a[k]) | set(b[k]))
    out["capture"] = a["capture"] + b["capture"]
    return out


def empty():
    return {"lock": 0, "pdq": 0, "async": 0, "wait": 0, "drain": 0, "editors": 0, "sym": 0, "msgEditors": 0,
            "gui": [], "guiOff": [], "capture": [], "sys": []}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".")
    ap.add_argument("--json")
    args = ap.parse_args()
    root = args.root

    guihost = open(os.path.join(root, "Source/Mcp/McpGuiHost.h")).read()
    # Every McpGuiHost method is message-thread-only, except those whose comment starts "Any thread"
    # (P3.4 S3; before S3 the header marked the off-thread callers "OFF-THREAD" instead).
    off_methods = set()
    any_thread = False
    for line in guihost.splitlines():
        if "Any thread" in line:
            any_thread = True
        m = re.search(r"virtual\s+.*?\b(\w+)\s*\(", line)
        if m:
            if not any_thread and m.group(1) != "McpGuiHost":
                off_methods.add(m.group(1))
            any_thread = False

    funcs = {}  # short name -> list of (qualified, direct facts, callees, file)
    all_short = set()
    parsed = []
    for d, pat in FILES_GLOBS:
        for fn in sorted(os.listdir(os.path.join(root, d))):
            if not re.match(pat, fn):
                continue
            path = os.path.join(d, fn)
            raw = open(os.path.join(root, path), encoding="utf-8", errors="replace").read()
            code, _ = strip_comments_and_strings(raw)
            for q, s, bc, br in find_functions(path, code, raw):
                parsed.append((path, q, s, bc, br))
                all_short.add(s)

    for path, q, s, bc, br in parsed:
        ptr_locals = set(re.findall(PTR_TYPES + r"\s*\*\s*(\w+)", bc))
        # A function that starts by posting itself to the message thread when called elsewhere
        # ("if (!...isThisTheMessageThread()) { ...callAsync...; return; }") runs its body there.
        head = bc[:600]
        hops_first = "isThisTheMessageThread()" in head and "callAsync" in head and "return" in head
        f = facts_of(bc, br, ptr_locals, off_methods)
        if hops_first:
            f = empty()
        off_part = mask(bc, lambda_spans(bc, "callAsync") + lambda_spans(bc, "onMessageThread"))
        callees = set() if hops_first else set(re.findall(r"\b([A-Za-z_]\w*)\s*\(", off_part)) & all_short
        callees.discard(s)
        funcs.setdefault(s, []).append({"q": q, "file": path, "direct": f, "callees": callees})

    sys_table = {}
    sysinc = strip_comments_and_strings_keep(open(os.path.join(root, "Source/Mcp/McpSysVerbs.inc")).read())
    for m in re.finditer(r'MCP_SYS\("(\w+)",\s*(\w+)\)', sysinc):
        sys_table[m.group(1)] = m.group(2)

    memo = {}

    def closure(short, stack):
        if short in memo:
            return memo[short]
        if short in stack:
            return empty()
        stack = stack | {short}
        acc = empty()
        for ent in funcs.get(short, []):
            acc = merge(acc, ent["direct"])
            for c in ent["callees"]:
                acc = merge(acc, closure(c, stack))
            for sel in ent["direct"]["sys"]:
                h = sys_table.get(sel)
                if h:
                    acc = merge(acc, closure(h, stack))
        memo[short] = acc
        return acc

    rows = []
    verbs = strip_comments_and_strings_keep(open(os.path.join(root, "Source/Pd/MCPBridgeVerbs.inc")).read())
    for m in re.finditer(r"^\s*MCP_(VERB|ALIAS|DOMAIN)\(([^)]*)\)", verbs, re.M):
        kind = m.group(1)
        parts = [p.strip() for p in m.group(2).split(",")]
        if kind == "DOMAIN":
            domain, verb, handler = parts[0], "*", parts[1]
        else:
            domain, verb, handler = parts[0], parts[1].strip('"'), parts[2]
        rows.append(("osc", kind.lower(), f"/{domain}/{verb}", handler))
    for sel, h in sys_table.items():
        rows.append(("sys", "sys", sel, h))

    out = []
    for src, kind, key, handler in rows:
        ents = funcs.get(handler, [])
        direct = empty()
        for e in ents:
            direct = merge(direct, e["direct"])
        total = closure(handler, set())
        out.append({"source": src, "kind": kind, "key": key, "handler": handler,
                    "file": ents[0]["file"] if ents else None,
                    "direct": direct, "total": total})

    if args.json:
        with open(args.json, "w") as fh:
            json.dump({"offThreadMethods": sorted(off_methods), "rows": out}, fh, indent=1, default=list)

    # Human summary: one line per row (aliases skipped: same handler).
    def short(f):
        bits = []
        for k in ("lock", "pdq", "async", "wait", "drain", "editors", "sym"):
            if f[k]:
                bits.append(f"{k}{f[k]}")
        if f["guiOff"]:
            bits.append("OFF:" + "/".join(f["guiOff"]))
        if f["capture"]:
            rp = sum(1 for c in f["capture"] if c["rawPtrs"])
            br = sum(1 for c in f["capture"] if c["byRef"])
            bits.append(f"cap{len(f['capture'])}(ptr{rp},ref{br})")
        if f["sys"]:
            bits.append("sys:" + "/".join(s.replace("mcp_", "") for s in f["sys"]))
        return " ".join(bits) or "-"

    missing = [r for r in out if not r["file"]]
    for r in out:
        if r["kind"] == "alias":
            continue
        print(f"{r['key']:<34} {r['handler']:<26} direct[{short(r['direct'])}]  via[{short(r['total'])}]")
    if missing:
        print("UNRESOLVED handlers:", ", ".join(r["handler"] for r in missing), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
