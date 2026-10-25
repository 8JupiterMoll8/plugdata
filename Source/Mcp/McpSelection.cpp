/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// P11.5 V26: the live editor selection for /pd/selection_get ("this", "it", $selected are read in the call, never
// remembered). The selection is GUI state: McpGuiHost::liveSelection reads it on the message thread; the ids are then
// resolved (or minted, as the census does) under the Pd lock, only for objects still on their canvas.
//
// Reply JSON: {canvasKey, ids:[tempId], classes:[class], count, elsewhere?:{canvasKey, count}}. Empty ids = nothing
// selected. Without a canvas: the focused view's canvas (a subpatch view answers the subpatch's key). With one: the
// selection inside that canvas; when the focused view has a selection on ANOTHER canvas, `elsewhere` names it, so the
// caller can say "selected in X, not in Y".

#include "Pd/MCPBridgeCmdCommon.h" // McpHost, McpGuiHost, MCPBridge::canonicalCanvasKey, mcpCanvasIsLive, Pd headers

namespace {

bool onCanvas(t_canvas* c, t_gobj* g)
{
    for (t_gobj* y = c ? c->gl_list : nullptr; y; y = y->g_next)
        if (y == g)
            return true;
    return false;
}

juce::String classOf(t_gobj* g)
{
    juce::String abstraction;
    if (pd::getAbstractionFileName(g, abstraction) && abstraction.isNotEmpty())
        return abstraction;
    t_class* cl = pd_class(&g->g_pd);
    if (cl == canvas_class)
        return "pd";
    if (auto const* t = pd_checkobject(&g->g_pd); t && t->te_binbuf && binbuf_getnatom(t->te_binbuf) > 0) {
        auto const* a = binbuf_getvec(t->te_binbuf);
        if (t->te_type == T_OBJECT && a[0].a_type == A_SYMBOL)
            return juce::String::fromUTF8(a[0].a_w.w_symbol->s_name);
    }
    return juce::String::fromUTF8(class_getname(cl));
}

} // namespace

std::optional<juce::String> McpHost::selectionGet(juce::String const& canvasName, juce::String& error)
{
    t_canvas* target = nullptr;
    if (canvasName.isNotEmpty()) {
        target = getCanvasBySymbol(canvasName);
        if (!target) {
            error = "canvas not found: " + canvasName;
            return std::nullopt;
        }
    }
    struct Both {
        McpGuiHost::LiveSelection focused, inTarget;
    };
    auto const live = onMessageThread([this, target] {
        Both b;
        b.focused = gui().liveSelection(nullptr);
        if (target)
            b.inTarget = gui().liveSelection(target);
        return b;
    }, 2000, "selection_get");
    if (!live) {
        error = "the editor did not answer";
        return std::nullopt;
    }

    PdLockScope lock(processor);
    auto const& sel = target ? live->inTarget : live->focused;
    t_canvas* canvas = target ? target : sel.canvas;
    juce::Array<juce::var> ids, classes;
    // Review M1: the canvas came from a GUI read on another thread; it is dereferenced (its key, its objects) only
    // while it is still an open canvas.
    bool const live = canvas && mcpCanvasIsLive(canvas);
    if (live) {
        for (auto* g : sel.objects) {
            if (!onCanvas(canvas, g))
                continue; // deleted since the GUI read
            auto const id = getOrAdoptStableId(canvas, g);
            if (id.isEmpty())
                continue;
            ids.add(id);
            classes.add(classOf(g));
        }
    }
    auto* root = new juce::DynamicObject();
    root->setProperty("canvasKey", live ? MCPBridge::canonicalCanvasKey(canvas) : juce::String());
    root->setProperty("ids", ids);
    root->setProperty("classes", classes);
    root->setProperty("count", ids.size());
    if (target && live->focused.canvas && live->focused.canvas != target && !live->focused.objects.empty()
        && mcpCanvasIsLive(live->focused.canvas)) {
        auto* other = new juce::DynamicObject();
        other->setProperty("canvasKey", MCPBridge::canonicalCanvasKey(live->focused.canvas));
        other->setProperty("count", static_cast<int>(live->focused.objects.size()));
        root->setProperty("elsewhere", juce::var(other));
    }
    return juce::JSON::toString(juce::var(root), true);
}
