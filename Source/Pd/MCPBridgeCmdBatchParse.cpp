/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// batch_atomic stage 1 (P3.4 S9): parse and validate the request on the command worker.

#include "MCPBridgeCmdBatch.h"

// Escape semicolons for Pd paste buffer format
static juce::String escapePdText(const juce::String& text)
{
    return text.replace(";", " \\;");
}

// Escape commas AND semicolons inside OBJECT box creation args so
// binbuf_text doesn't split them into separate messages. binbuf_text
// treats `,` as a message separator, so an unescaped `[expr pow($f1, 2)]`
// was created as `expr pow($f1` + a stray `2)` message. Backslash escapes
// are the Pd paste-buffer convention: `\,` → literal comma in the atom.
static juce::String escapePdObjArgs(const juce::String& text)
{
    juce::String out = text;
    out = out.replace("\\", "\\\\");
    out = out.replace(",", "\\,");
    out = out.replace(";", "\\;");
    return out;
}

// Convert MCP kind + tokens to Pd paste-format line
static juce::String formatAsPdLine(const juce::String& kind,
                                    const juce::StringArray& tokens,
                                    int x, int y)
{
    juce::StringArray t = tokens;

    if (kind == "msg" || kind == "message" || (!t.isEmpty() && t[0] == "msg")) {
        if (!t.isEmpty() && (t[0] == "msg" || t[0] == "message")) t.remove(0);
        while (!t.isEmpty() && t[0].isEmpty()) t.remove(0);
        return "#X msg " + juce::String(x) + " " + juce::String(y)
               + " " + escapePdText(t.joinIntoString(" ").trim()) + ";";
    }
    if (kind == "text" || kind == "comment" || (!t.isEmpty() && (t[0] == "text" || t[0] == "comment"))) {
        if (!t.isEmpty() && (t[0] == "text" || t[0] == "comment")) t.remove(0);
        while (!t.isEmpty() && t[0].isEmpty()) t.remove(0);
        return "#X text " + juce::String(x) + " " + juce::String(y)
               + " " + escapePdText(t.joinIntoString(" ").trim()) + ";";
    }
    if (kind == "floatatom" || kind == "floatbox") {
        if (!t.isEmpty() && (t[0] == "floatatom" || t[0] == "floatbox")) t.remove(0);
        // The TS sends type:"" for kind floatatom/symbolatom, so t may START with
        // an empty token. Strip it (like the msg/text branches) or the paste
        // binbuf gets a stray empty atom that shifts the IEM arg count and the
        // object fails to create ("couldn't create — red box rolled back").
        while (!t.isEmpty() && t[0].isEmpty()) t.remove(0);
        // PlugData's FloatAtomObject has a GUI-component lifecycle bug that
        // crashes the renderer when a [floatatom] is created/destroyed via the
        // MCP mutation engine (reproduced by the GUI stress gauntlet). [numbox]
        // is the equivalent numeric display/input with a stable component and
        // identical gatom argument layout, so substitute it. NOTE: the Pd class
        // name is "nbx" (g_numbox.c class_new(gensym("nbx"))) — "numbox" is only
        // an MCP-side alias and does NOT resolve in the paste/object-maker, which
        // is why '#X obj … numbox …' failed to create.
        t.insert(0, "nbx");
        return "#X obj " + juce::String(x) + " " + juce::String(y)
               + " " + escapePdObjArgs(t.joinIntoString(" ").trim()) + ";";
    }
    if (kind == "symbolatom" || kind == "symbolbox") {
        if (!t.isEmpty() && (t[0] == "symbolatom" || t[0] == "symbolbox")) t.remove(0);
        while (!t.isEmpty() && t[0].isEmpty()) t.remove(0);
        return "#X symbolatom " + juce::String(x) + " " + juce::String(y)
               + " " + t.joinIntoString(" ").trim() + ";";
    }

    // IEM GUI short-form guard: `hsl 128 15 0 1000 0` and friends must be
    // expanded to Pd's full 17-arg form or the range/init is silently dropped.
    t = mcp::expandIemGuiShortForm(t);

    if (!t.isEmpty() && t[0] == "+") t.set(0, "\\+");
    return "#X obj " + juce::String(x) + " " + juce::String(y)
           + " " + escapePdObjArgs(t.joinIntoString(" ").trim()) + ";";
}

void mcp::BatchJob::parse(const juce::OSCMessage& msg)
{
    int cursor = 2;

    if (msg.size() >= 7) {
        // 5-phase extended atomic protocol
        deleteCount = static_cast<int>(getArgFloat(msg[cursor++]));
        disconnectCount = static_cast<int>(getArgFloat(msg[cursor++]));
        editCount = static_cast<int>(getArgFloat(msg[cursor++]));
        createCount = static_cast<int>(getArgFloat(msg[cursor++]));
        connectCount = static_cast<int>(getArgFloat(msg[cursor++]));
    } else {
        // Legacy 3-phase protocol fallback (creates, connects, edits)
        createCount = static_cast<int>(getArgFloat(msg[cursor++]));
        connectCount = static_cast<int>(getArgFloat(msg[cursor++]));
        editCount = static_cast<int>(getArgFloat(msg[cursor++]));
    }

    // =========================================================================
    // PHASE 0: PRE-PARSE outside audio thread — zero contention
    // =========================================================================
    for (int d = 0; d < deleteCount && cursor < msg.size(); d++)
        preDeletes.push_back({ getArgString(msg[cursor++]) });

    for (int dc = 0; dc < disconnectCount && cursor + 3 < msg.size(); dc++) {
        auto s = getArgString(msg[cursor++]); int so = static_cast<int>(getArgFloat(msg[cursor++]));
        auto d2 = getArgString(msg[cursor++]); int di = static_cast<int>(getArgFloat(msg[cursor++]));
        preDisconnects.push_back({ s, so, d2, di });
    }

    for (int e = 0; e < editCount && cursor < msg.size(); e++) {
        auto oid = getArgString(msg[cursor++]);
        auto nt = (cursor < msg.size()) ? getArgString(msg[cursor++]) : juce::String();
        int na = (cursor < msg.size()) ? static_cast<int>(getArgFloat(msg[cursor++])) : 0;
        juce::StringArray tk;
        if (nt != "msg" && nt.isNotEmpty()) tk.add(nt);
        for (int a = 0; a < na && cursor < msg.size(); a++) tk.add(getArgString(msg[cursor++]));
        preEdits.push_back({ oid, tk.joinIntoString(" ") });
    }

    for (int o = 0; o < createCount && cursor < msg.size(); o++) {
        auto oid = getArgString(msg[cursor++]);
        float px = (cursor < msg.size()) ? getArgFloat(msg[cursor++]) : 0;
        float py = (cursor < msg.size()) ? getArgFloat(msg[cursor++]) : 0;
        auto kind = (cursor < msg.size()) ? getArgString(msg[cursor++]) : juce::String();
        auto ot = (cursor < msg.size()) ? getArgString(msg[cursor++]) : juce::String();
        int na = (cursor < msg.size()) ? static_cast<int>(getArgFloat(msg[cursor++])) : 0;
        juce::StringArray tk;
        if (ot.isNotEmpty()) tk.add(ot);
        for (int a = 0; a < na && cursor < msg.size(); a++) tk.add(getArgString(msg[cursor++]));

        // tempId theft guard (authoritative — the bridge OWNS the id map).
        // A create whose tempId already resolves to a LIVE object would
        // silently overwrite mcpStableObjectMap (below), orphaning the
        // original as gui_* and misrouting every later op on that id.
        // Refuse it here; skip the paste + registration entirely.
        if (ot.isNotEmpty() && !oid.isEmpty()
            && processor->getMcp()->resolveStableId(canvasName, oid) != nullptr) {
            CreateFailure cf;
            cf.tempId = oid.toStdString();
            cf.type = ot.toStdString();
            // On a RETRY the model re-sends the whole batch, including
            // objects its previous call already created — "pick a new
            // id" then sends it renaming things it already owns. The
            // object it wants IS there; say that.
            cf.reason = "tempId already exists on the canvas - it was NOT re-created. To USE it, wire or set it directly (connect[]/set[]); to REPLACE it, rename or delete it first";
            createFailures.push_back(cf);
            continue;
        }

        // ── Control-kind preflight (PRD): detect the two SILENT GUI
        // failures straight from the creation args — inverted geometry
        // (an hsl built 15x120 reads as "no slider") and an invalid
        // range. The object still gets created; this only advises.
        // Arg positions match Source/analysis patch-analyzer
        // parseGuiProperties: tk[0]=class, tk[1]=width, tk[2]=height,
        // tk[3]=min, tk[4]=max (for hsl/vsl/nbx).
        {
            const juce::String cls = ot.toLowerCase();
            if (cls == "hsl" || cls == "vsl" || cls == "nbx") {
                auto num = [&tk](int i, double& out) -> bool {
                    if (i < 0 || i >= tk.size()) return false;
                    if (tk[i].isEmpty()) return false;
                    out = tk[i].getDoubleValue();
                    return true;
                };
                double w = 0, h = 0, lo = 0, hi = 0;
                if ((cls == "hsl" || cls == "vsl")
                    && num(1, w) && num(2, h) && w > 0 && h > 0) {
                    const bool inverted = (cls == "hsl") ? (h >= w) : (w >= h);
                    if (inverted) {
                        createAdvisories.push_back({ oid,
                            cls + " " + juce::String(w) + "x" + juce::String(h)
                            + (cls == "hsl"
                                ? " — width/height look inverted: hsl wants width>height (wide+short), e.g. [128,15]"
                                : " — width/height look inverted: vsl wants height>width (narrow+tall), e.g. [15,128]") });
                    }
                }
                if (num(3, lo) && num(4, hi) && lo > hi) {
                    createAdvisories.push_back({ oid,
                        cls + " min " + juce::String(lo) + " > max " + juce::String(hi)
                        + " — invalid range: swap min/max" });
                }
            }
            if (cls == "knob") {
                // ELSE knob args: knob <size> <min> <max> [exp] [load] …
                // With fewer than 3 args Pd reads the missing min/max as 0,
                // giving a 0→0 range = a knob that controls NOTHING (the
                // artist turns it and hears no change).
                if (tk.size() < 4) {
                    createAdvisories.push_back({ oid,
                        "knob '" + oid + "' has only a size — min/max default to 0 (range 0→0, controls NOTHING). Use knob <size> <min> <max> [exp] [load], e.g. knob 40 20 20000 1 220" });
                }
            }
        }

        // line~ in this fork takes NO creation arguments (line_tilde_new(void)),
        // so `[line~ 0.057]` is silently created at 0. Capture a numeric init
        // arg here and re-seed it onto the object right after paste, so
        // `[r x] -> [line~ 440] -> [osc~]` actually starts at 440 instead of 0.
        bool seedInit = false;
        float initValue = 0.0f;
        if ((ot == "line~" || ot == "vline~") && na >= 1 && tk.size() >= 2) {
            juce::String firstArg = tk[1];
            if (firstArg.isNotEmpty()) {
                auto charptr = firstArg.getCharPointer();
                auto ptr = charptr;
                juce::CharacterFunctions::readDoubleValue(ptr);
                if (ptr - charptr == firstArg.getNumBytesAsUTF8()) {
                    initValue = firstArg.getFloatValue();
                    seedInit = true;
                }
            }
        }

        pastaBuffer += formatAsPdLine(kind, tk, static_cast<int>(px), static_cast<int>(py)) + "\n";
        pendingCreates.push_back({ oid, ot, kind, static_cast<int>(px), static_cast<int>(py), initValue, seedInit });
    }

    // ALL connections go through obj_connect after paste (no #X connect in buffer)
    for (int c = 0; c < connectCount && cursor + 3 < msg.size(); c++) {
        auto s = getArgString(msg[cursor++]); int so = static_cast<int>(getArgFloat(msg[cursor++]));
        auto d2 = getArgString(msg[cursor++]); int di = static_cast<int>(getArgFloat(msg[cursor++]));
        allConns.push_back({ s, so, d2, di });
    }

    if (cursor < msg.size()) autoLayout = getArgFloat(msg[cursor++]) > 0.5f;
    if (cursor < msg.size()) layoutPad = static_cast<int>(getArgFloat(msg[cursor++]));
    if (cursor < msg.size()) layoutSnap = static_cast<int>(getArgFloat(msg[cursor++]));
    if (cursor < msg.size()) autoFixOcclusions = getArgFloat(msg[cursor++]) > 0.5f;

    if (processor->getMcp()->session.isExecutingMcpUndoRedo) {
        autoLayout = false;
        autoFixOcclusions = false;
    }
}
