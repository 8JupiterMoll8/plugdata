/*
 // MCP telemetry hooks (Plan 3 P3.2 S5): the canvas selection and every console line,
 // forwarded to the [r #plugdata_mcp_ui] / [r #plugdata_mcp_console] receivers and to the
 // bridge. Moved verbatim from Sidebar::forceShowParameters and pd::Instance's console
 // handler. The "MCP_TELEMETRY: Pushed ..." line it used to print in the artist's console on
 // every selection is gone (P3.1 note).
 */
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "Mcp/McpHost.h"
#include "Pd/MCPBridge.h"
#include "Canvas.h"
#include "Object.h"

extern "C" {
#include <z_libpd.h>
}

// Sidebar::forceShowParameters, after the inspector is filled (message thread).
void McpHost::selectionChanged(SmallArray<Component*>& objects)
{
    // MCP: report the canvas selection to the bridge (get_selection / "this").
    auto* pd = &processor;
    if (pd && !objects.empty()) {
        auto const subpatch = (objects.size() > 0) ? (dynamic_cast<Object*>(objects[0]) ? dynamic_cast<Object*>(objects[0])->cnv->patch.getTitle() : "main") : "main";

        if (objects.size() == 1) {
            // Single Selection: selected_object [subpatch] [class] [index] [x] [y]
            if (auto* object = dynamic_cast<Object*>(objects[0])) {
                SmallArray<pd::Atom> atoms;
                atoms.add(pd::Atom(pd->generateSymbol(subpatch)));
                atoms.add(pd::Atom(pd->generateSymbol(object->getType(false))));
                atoms.add(pd::Atom(static_cast<float>(object->cnv->objects.index_of(object))));
                
                // Use Object Bounds for patch-relative coordinates
                auto bounds = object->getObjectBounds();
                atoms.add(pd::Atom(static_cast<float>(bounds.getX())));
                atoms.add(pd::Atom(static_cast<float>(bounds.getY())));
                
                sendSelectionTelemetry("selected_object", atoms);
            }
        } else {
            // Multi-Selection: selection_count [subpatch] [count]
            SmallArray<pd::Atom> countAtoms;
            countAtoms.add(pd::Atom(pd->generateSymbol(subpatch)));
            countAtoms.add(pd::Atom(static_cast<float>(objects.size())));
            sendSelectionTelemetry("selection_count", countAtoms);

            // selection_indices [subpatch] [idx1] [idx2] ...
            SmallArray<pd::Atom> indexAtoms;
            indexAtoms.add(pd::Atom(pd->generateSymbol(subpatch)));
            for (auto const objComp : objects) {
                if (auto const obj = dynamic_cast<Object*>(objComp)) {
                    indexAtoms.add(pd::Atom(static_cast<float>(obj->cnv->objects.index_of(obj))));
                }
            }
            sendSelectionTelemetry("selection_indices", indexAtoms);
        }
    }
}

// Was pd::Instance's ConsoleMessageHandler::processSelection + PluginProcessor::onSelectionChanged.
void McpHost::sendSelectionTelemetry(String const& selector, SmallArray<pd::Atom> const& list)
{
    libpd_set_instance(static_cast<t_pdinstance*>(processor.instance));
    t_symbol* s = gensym("#plugdata_mcp_ui");
    if (s && s->s_thing) {
        std::vector<t_atom> atoms(list.size());
        for (int i = 0; i < list.size(); ++i) {
            if (list[i].isFloat()) {
                SETFLOAT(&atoms[i], list[i].getFloat());
            } else if (list[i].isSymbol()) {
                SETSYMBOL(&atoms[i], list[i].getSymbol());
            }
        }
        // Send as a typed message (selector first) so [route] works natively
        pd_typedmess(s->s_thing, gensym(selector.toRawUTF8()), static_cast<int>(atoms.size()), atoms.data());
    }
    if (mcpBridge) {
        mcpBridge->sendSelectionTelemetry(selector, list);
    }
}

// Every console line, after plugdata's console has it (Pd thread; the gensym interning of
// each line is a P3.4 item). Was the tail of pd::Instance's console handler +
// PluginProcessor::onConsoleMessage.
void McpHost::consoleMessage(String const& message, bool const isError, void* const object)
{
    // Forward to PlugData receiver for MCP bridge
    libpd_set_instance(static_cast<t_pdinstance*>(processor.instance));
    auto* s = gensym("#plugdata_mcp_console");
    if (s && s->s_thing) {
        t_atom atoms[2];
        SETFLOAT(&atoms[0], isError ? 1.0f : 0.0f);
        SETSYMBOL(&atoms[1], gensym(message.toRawUTF8()));
        pd_typedmess(s->s_thing, gensym("list"), 2, atoms);
    }
    // V1 /pd/console_history: every line with a monotonic sequence number (a repeat of the last line bumps its
    // count and its seq), and the tempId of the object that raised it (pointer compared only, never dereferenced).
    {
        juce::String const tempId = object ? getStableId(static_cast<t_gobj*>(object)) : juce::String();
        std::lock_guard<std::mutex> const lock(session.consoleLock);
        uint64_t const seq = ++session.consoleSeq;
        auto& lines = session.consoleLines;
        if (!lines.empty() && lines.back().text == message && lines.back().error == isError && lines.back().tempId == tempId) {
            lines.back().seq = seq;
            ++lines.back().repeats;
        } else {
            lines.push_back({ seq, seq, message, tempId, isError, 1 });
            if (lines.size() > McpSession::kConsoleLines)
                lines.pop_front();
        }
    }
    if (mcpBridge) {
        // T2b: the object that printed the line (an error, or a warning such as "inlet: expected ..."), as its
        // tempId. The pointer is only compared with the registry's (never dereferenced: the object may be
        // mid-deletion). Pd printed this line with the Pd lock held, which is also what the identity registry needs;
        // PdLockScope is recursive. Plain post() lines carry no object, so the lookup is rare.
        juce::String objectId;
        if (object)
            objectId = getStableId(static_cast<t_gobj*>(object));
        mcpBridge->sendConsoleLog(message, isError, objectId);
    }
}
