/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// The armed control-message window and its native Pd receiver class, moved unchanged out of
// MCPBridge.cpp (Plan 3 P3.3) so the /meter/arm_control and /meter/read_control handlers and
// MCPBridge itself (constructor, destructor, mcpCtrlCapture) share one definition.
// Included by MCPBridgeCmdCommon.h after the Pd headers.

#pragma once

// ═══════════════════════════════════════════════════════════════════════════
// Phase 4 — Armed CONTROL-message window (PRD_MEASUREMENT_WINDOWS §5)
// ═══════════════════════════════════════════════════════════════════════════
//
// The audio peak-hold (/meter/arm + /meter/read) already solves the "window
// opened too late" race for AUDIO. Control messages have the same disease:
// tap_control_flow / probe_control_msg start listening when called, so a
// message already sent is missed. The fix is the same SHAPE (arm → send →
// read) but a different primitive: a real Pd receiver, bound to the target
// name, that accumulates every atom dispatched to it while armed.
//
// Why this is safe without a lock on the capture path: every Pd message
// dispatch in PlugData runs while the global `sys_lock` is held — libpd's
// own `libpd_bang/float/list/...` take it, `libpd_process_raw` (the audio +
// scheduler tick) takes it, and the GUI takes it. arm/read take it too, so
// writers and the reader are mutually exclusive. The capture itself only
// touches atomics + a preallocated buffer; it never allocates and never
// blocks, so it is safe even when a message originates on the audio thread.

// Fixed capture capacity. A measurement window is short and control traffic is
// sparse; overflow increments `dropped` honestly rather than reallocating.
static constexpr int MCP_CTRL_MAX_MSGS = 128;
static constexpr int MCP_CTRL_MAX_ATOMS = 32;
static constexpr int MCP_CTRL_MAX_SYMLEN = 64;

struct CtrlAtom {
    bool isSymbol = false;
    double num = 0.0;
    char sym[MCP_CTRL_MAX_SYMLEN] = {};
};

struct CtrlMessage {
    bool bang = false;
    int argc = 0;
    char selector[MCP_CTRL_MAX_SYMLEN] = {};
    CtrlAtom atoms[MCP_CTRL_MAX_ATOMS];
};

// The armed window: a preallocated message log plus the live Pd binding.
// Definition MUST stay at global scope (the header forward-declares it and
// MCPBridge holds a unique_ptr<McpControlWindow>).
struct McpControlWindow {
    std::atomic<bool> armed { false };
    std::atomic<int> count { 0 };
    std::atomic<int> dropped { 0 };

    juce::String name;              // receiver name this window is bound to
    t_symbol* boundName = nullptr;  // realised Pd symbol
    void* receiver = nullptr;       // our CLASS_PD receiver object

    CtrlMessage msgs[MCP_CTRL_MAX_MSGS];

    // Append one dispatched message. Called under sys_lock (serialized), so the
    // plain buffer writes below are never concurrent with arm/read.
    void record(t_symbol* selector, int argc, t_atom* argv)
    {
        if (!armed.load(std::memory_order_acquire)) return;
        if (argc < 0) argc = 0;

        int const idx = count.load(std::memory_order_relaxed);
        if (idx >= MCP_CTRL_MAX_MSGS) {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        count.store(idx + 1, std::memory_order_release);

        CtrlMessage& m = msgs[idx];
        m.bang = false;
        m.argc = 0;

        const char* sel = (selector && selector->s_name) ? selector->s_name : "";
        copyTrunc(m.selector, MCP_CTRL_MAX_SYMLEN, sel);
        if (juce::String(sel) == "bang") m.bang = true;

        if (argc > MCP_CTRL_MAX_ATOMS) argc = MCP_CTRL_MAX_ATOMS;
        for (int i = 0; i < argc; ++i) {
            t_atom const& a = argv[i];
            CtrlAtom& out = m.atoms[i];
            if (a.a_type == A_FLOAT) {
                out.isSymbol = false;
                out.num = a.a_w.w_float;
            } else if (a.a_type == A_SYMBOL) {
                out.isSymbol = true;
                out.num = 0.0;
                copyTrunc(out.sym, MCP_CTRL_MAX_SYMLEN,
                          a.a_w.w_symbol ? a.a_w.w_symbol->s_name : "");
            } else if (a.a_type == A_POINTER) {
                out.isSymbol = true;
                out.num = 0.0;
                copyTrunc(out.sym, MCP_CTRL_MAX_SYMLEN, "pointer");
            } else {
                out.isSymbol = true;
                out.num = 0.0;
                copyTrunc(out.sym, MCP_CTRL_MAX_SYMLEN, "?");
            }
        }
        m.argc = argc;
    }

    juce::String toJson(int n) const
    {
        if (n < 0) n = 0;
        if (n > MCP_CTRL_MAX_MSGS) n = MCP_CTRL_MAX_MSGS;
        juce::String out = "{\"count\":" + juce::String(n)
            + ",\"dropped\":" + juce::String(dropped.load(std::memory_order_relaxed))
            + ",\"receiver\":\"" + jsonEscape(name.toRawUTF8()) + "\""
            + ",\"messages\":[";
        for (int i = 0; i < n; ++i) {
            if (i) out << ",";
            CtrlMessage const& m = msgs[i];
            out << "{\"selector\":\"" << jsonEscape(m.selector) << "\"";
            if (m.bang) out << ",\"bang\":true";
            out << ",\"atoms\":[";
            for (int a = 0; a < m.argc; ++a) {
                if (a) out << ",";
                CtrlAtom const& at = m.atoms[a];
                if (at.isSymbol) out << "\"" << jsonEscape(at.sym) << "\"";
                else out << juce::String(at.num, 6);
            }
            out << "]}";
        }
        out << "]}";
        return out;
    }

    // Free the Pd binding. Caller holds sys_lock.
    void unbindReceiver()
    {
        armed.store(false, std::memory_order_release);
        if (receiver) {
            pd_free(reinterpret_cast<t_pd*>(receiver));
            receiver = nullptr;
        }
        boundName = nullptr;
    }
};

// ── The native receiver class ──────────────────────────────────────────────
// CLASS_PD (non-graphical, not on any canvas). Bound straight to the target
// symbol via pd_bind, so it coexists with any real [r name] in the patch —
// messages fan out to both. Zero canvas objects, zero DSP recompile.
struct McpCtrlReceiver {
    t_object obj;            // ob_pd at offset 0 → bindable/dispatchable
    t_symbol* sym = nullptr;
    MCPBridge* bridge = nullptr;
};

inline t_class* mcpCtrlReceiverClass = nullptr;

inline void mcpCtrlReceiverFree(McpCtrlReceiver* x)
{
    if (x->sym) { pd_unbind(&x->obj.ob_pd, x->sym); x->sym = nullptr; }
}

inline void mcpCtrlReceiverBang(McpCtrlReceiver* x)
{
    if (x->bridge) x->bridge->mcpCtrlCapture(gensym("bang"), 0, nullptr);
}

inline void mcpCtrlReceiverFloat(McpCtrlReceiver* x, t_float f)
{
    if (!x->bridge) return;
    t_atom a; SETFLOAT(&a, f);
    x->bridge->mcpCtrlCapture(gensym("float"), 1, &a);
}

inline void mcpCtrlReceiverSymbol(McpCtrlReceiver* x, t_symbol* s)
{
    if (!x->bridge) return;
    t_atom a; SETSYMBOL(&a, s);
    x->bridge->mcpCtrlCapture(gensym("symbol"), 1, &a);
}

inline void mcpCtrlReceiverList(McpCtrlReceiver* x, t_symbol* s, int argc, t_atom* argv)
{
    if (x->bridge) x->bridge->mcpCtrlCapture(s, argc, argv);
}

inline void mcpCtrlReceiverAnything(McpCtrlReceiver* x, t_symbol* s, int argc, t_atom* argv)
{
    if (x->bridge) x->bridge->mcpCtrlCapture(s, argc, argv);
}

// Caller holds sys_lock. Idempotent.
inline void ensureMcpCtrlReceiverClass()
{
    if (mcpCtrlReceiverClass) return;
    mcpCtrlReceiverClass = class_new(gensym("mcp_ctrl_receive"), nullptr,
        reinterpret_cast<t_method>(mcpCtrlReceiverFree),
        sizeof(McpCtrlReceiver), CLASS_PD, static_cast<t_atomtype>(0));
    class_addbang(mcpCtrlReceiverClass, mcpCtrlReceiverBang);
    class_addfloat(mcpCtrlReceiverClass, reinterpret_cast<t_method>(mcpCtrlReceiverFloat));
    class_addsymbol(mcpCtrlReceiverClass, reinterpret_cast<t_method>(mcpCtrlReceiverSymbol));
    class_addlist(mcpCtrlReceiverClass, reinterpret_cast<t_method>(mcpCtrlReceiverList));
    class_addanything(mcpCtrlReceiverClass, reinterpret_cast<t_method>(mcpCtrlReceiverAnything));
}
