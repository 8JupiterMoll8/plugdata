/*
 // The mcp_* Pd system messages (Plan 3 P3.2 S5). These were cases of
 // PluginProcessor::receiveSysMessage, moved verbatim (with `session.` / `processor.`
 // prefixes). PluginProcessor calls McpHost::handleSysMessage first; every other
 // selector returns false and falls through to plugdata's own switch as before.
 */
#include <clocale>
#include <memory>
#include <fstream>
#include <unordered_set>

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include "PluginProcessor.h"
#include "Pd/Library.h"
#include "Pd/MCPBridge.h"
#include "Pd/McpIemArgs.h"
#include "Mcp/PdOps.h" // MCP-HOOK(pdOps): garray_getname, removeObjects/renameObject for the mcp_* cases

#include "Utility/Config.h"
#include "Utility/Fonts.h"
#include "Utility/SettingsFile.h"
#include <g_all_guis.h>
#include "AllGuis.h"
extern "C" void knob_get_snd(void* x);
extern "C" void knob_get_rcv(void* x);
#include "Utility/PluginParameter.h"
#include "Utility/OSUtils.h"
#include "Utility/MidiDeviceManager.h"
#include "Utility/Autosave.h"
#include "Utility/Decompress.h"
#include "Standalone/InternalSynth.h"

#include "Utility/Presets.h"
#include "Canvas.h"
#include "PluginMode.h"
#include "PluginEditor.h"
#include "LookAndFeel.h"
#include "Object.h"
#include "Statusbar.h"
#include "Toolbar.h"

#include "Dialogs/Dialogs.h"
#include "Components/ConnectionMessageDisplay.h"
#include "Utility/Recorder.h"

#include "Sidebar/Sidebar.h"

#include "Object.h"

extern "C" {
#include <pd-cyclone/shared/common/file.h>
EXTERN char* pd_version;
}

#include "Mcp/McpHost.h"

static int getObjectIndex(t_canvas* canvas, t_gobj* obj) {
    int index = 0;
    for (t_gobj* y_obj = canvas->gl_list; y_obj; y_obj = y_obj->g_next) {
        if (y_obj == obj) {
            return index;
        }
        index++;
    }
    return -1;
}

bool McpHost::handleSysMessage(SmallString const& selector, SmallArray<pd::Atom> const& list)
{
    switch (hash(selector)) {
    case hash("mcp_selection_gate"):
    case hash("mcp_move"):
    case hash("mcp_clear_selection"):
    case hash("mcp_move_batch"):
    case hash("mcp_clear_ids"):
    case hash("mcp_clear_undo"):
    case hash("mcp_suspend_dsp"):
    case hash("mcp_resume_dsp"):
    case hash("mcp_create_batch_id"):
    case hash("mcp_create_id"):
    case hash("mcp_register_id"):
    case hash("mcp_connect_id"):
    case hash("mcp_disconnect_id"):
    case hash("mcp_delete_id"):
    case hash("mcp_rename_id"):
    case hash("mcp_move_id"):
    case hash("mcp_edit_id"):
    case hash("mcp_get_mappings"):
    case hash("mcp_census"):
    case hash("mcp_move_batch_id"):
    case hash("mcp_connect_batch_id"):
    case hash("mcp_disconnect_batch_id"):
        break;
    default:
        return false;
    }

    dispatchSysMessage(selector, list);
    return true;
}

void McpHost::dispatchSysMessage(SmallString const& selector, SmallArray<pd::Atom> const& list)
{
    switch (hash(selector)) {
    case hash("mcp_selection_gate"): {
        String msgStr(selector.toString());
        if (msgStr.trim().toLowerCase() == "mcp_selection_gate") {
            processor.logMessage("MCP_DEBUG: Global Selection Query Received (Indestructible)");
            for (auto* editor : processor.getEditors()) {
                // Priority 1: Current Active Canvas (User Focus)
                auto* currentCanvas = editor->getCurrentCanvas();
                if (currentCanvas && currentCanvas->patch.getRawPointer() != nullptr) {
                    auto selectedObjects = currentCanvas->getSelectionOfType<Object>();
                    if (!selectedObjects.empty()) {
                        processor.logMessage("MCP_DEBUG: Found selection in ACTIVE: " + currentCanvas->patch.getTitle() + " (count: " + String(selectedObjects.size()) + ")");
                        SmallArray<pd::Atom> atoms;
                        auto titleAtoms = pd::Atom::atomsFromString(currentCanvas->patch.getTitle());
                        if (!titleAtoms.empty())
                            atoms.add(titleAtoms[0]);

                        for (auto* obj : selectedObjects) {
                            for (int i = 0; i < currentCanvas->objects.size(); ++i) {
                                if (currentCanvas->objects[i] == obj) {
                                    atoms.add(pd::Atom((float)i));
                                    break;
                                }
                            }
                        }
                        processor.sendMessage("mcp_telemetry_reply", "/mcp/telemetry/selected", atoms);
                        processor.logMessage("MCP_DEBUG: Dispatched to Pd: mcp_telemetry_reply");
                        processor.sendMessagesFromQueue();
                        return; // Focus found, we are done!
                    }
                }

                // Priority 2: X-Ray Scan (Background Patches)
                auto canvases = editor->getCanvases();
                for (int c = canvases.size() - 1; c >= 0; --c) {
                    auto* canvas = canvases[c];
                    if (canvas == currentCanvas) continue; // Already checked

                    auto selectedObjects = canvas->getSelectionOfType<Object>();
                    if (!selectedObjects.empty()) {
                        processor.logMessage("MCP_DEBUG: Found selection in X-RAY: " + canvas->patch.getTitle() + " (count: " + String(selectedObjects.size()) + ")");
                        SmallArray<pd::Atom> atoms;
                        auto titleAtoms = pd::Atom::atomsFromString(canvas->patch.getTitle());
                        if (!titleAtoms.empty())
                            atoms.add(titleAtoms[0]);

                        for (auto* obj : selectedObjects) {
                            for (int i = 0; i < canvas->objects.size(); ++i) {
                                if (canvas->objects[i] == obj) {
                                    atoms.add(pd::Atom((float)i));
                                    break;
                                }
                            }
                        }
                        processor.sendMessage("mcp_telemetry_reply", "/mcp/telemetry/selected", atoms);
                        processor.logMessage("MCP_DEBUG: Dispatched to Pd: mcp_telemetry_reply");
                        processor.sendMessagesFromQueue();
                        return;
                    }
                }
            }
        }
        break;
    }
    case hash("mcp_move"): {
        if (list.size() >= 4) {
            auto canvas_symbol = list[0].toString();
            int object_index = (int)list[1].getFloat();
            int x = (int)list[2].getFloat();
            int y = (int)list[3].getFloat();

            sys_lock();
            t_canvas* canvas = getCanvasBySymbol(canvas_symbol);
            if (canvas) {
                t_gobj* targetObj = nullptr;
                int currentIndex = 0;
                for (t_gobj* y_obj = canvas->gl_list; y_obj; y_obj = y_obj->g_next) {
                    if (currentIndex == object_index) {
                        targetObj = y_obj;
                        break;
                    }
                    currentIndex++;
                }

                if (targetObj) {
                    pd::Interface::moveObject(canvas, targetObj, x, y);
                    canvas_dirty(canvas, 1);
                }
            }
            sys_unlock();

            processor.sendMessagesFromQueue();
        }
        break;
    }
    case hash("mcp_clear_selection"): {
        for (auto* editor : processor.getEditors()) {
            if (auto* cnv = editor->getCurrentCanvas()) {
                cnv->deselectAll();
            }
        }
        break;
    }
    case hash("mcp_move_batch"): {
        if (list.size() >= 2) {
            auto canvas_symbol = list[0].toString();
            auto correlation_id = list[1].toString();

            int movedCount = 0;
            sys_lock();
            t_canvas* canvas = getCanvasBySymbol(canvas_symbol);
            if (canvas) {
                std::vector<t_gobj*> objects;
                for (t_gobj* y_obj = canvas->gl_list; y_obj; y_obj = y_obj->g_next) {
                    objects.push_back(y_obj);
                }

                for (int i = 2; i + 2 < list.size(); i += 3) {
                    int object_index = (int)list[i].getFloat();
                    int x = (int)list[i+1].getFloat();
                    int y = (int)list[i+2].getFloat();

                    if (object_index >= 0 && object_index < objects.size()) {
                        t_gobj* targetObj = objects[object_index];
                        pd::Interface::moveObject(canvas, targetObj, x, y);
                        movedCount++;
                    }
                }
                if (movedCount > 0) {
                    canvas_dirty(canvas, 1);
                }
            }
            sys_unlock();


            SmallArray<pd::Atom> atoms;
            atoms.add(pd::Atom((float)movedCount));
            sendMCPReply(String("/pd/move_batch/reply/" + correlation_id), atoms);
            processor.sendMessagesFromQueue();
        }
        break;
    }
    case hash("mcp_clear_ids"): {
        if (list.size() >= 2) {
            auto canvas_symbol = list[0].toString();
            auto correlation_id = list[1].toString();

            t_canvas* canvas = getCanvasBySymbol(canvas_symbol);
            auto canvasStr = (canvas ? MCPBridge::canonicalCanvasKey(canvas) : canvas_symbol).toStdString();
            // Purge serials for this canvas entries before erasing the map
            auto it = session.mcpStableObjectMap.find(canvasStr);
            if (it != session.mcpStableObjectMap.end()) {
                for (auto& kv : it->second) {
                    session.mcpStableSerialMap.erase(kv.second);
                }
                session.mcpStableObjectMap.erase(it);
                session.mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
            }

            SmallArray<pd::Atom> atoms;
            atoms.add(pd::Atom(1.0f));
            sendMCPReply(String("/pd/mcp_clear_ids/reply/" + correlation_id), atoms);
            processor.sendMessagesFromQueue();
        }
        break;
    }
    case hash("mcp_clear_undo"): {
        // Called from /pd/clear_undo handler via receiveSysMessage.
        // Runs on the Pd scheduler thread — the only thread where
        // canvas_undo_free (which calls canvas_suspend_dsp) is safe.
        auto wipeCanvasUndo = [](t_canvas* c) {
            if (!c) return;
            canvas_undo_free(c);
            if (auto* udo = canvas_undo_get(c)) {
                udo->u_queue = nullptr;
                udo->u_last = nullptr;
                udo->u_cleanstate = nullptr;
            }
        };

        // #28b: the MCP transaction stacks are a SEPARATE undo layer from the
        // native canvas undo. Wiping only the native queue (canvas_undo_free)
        // left stale MCP transactions armed — the next undo then replayed a
        // pre-restructure inverse against a canvas it no longer matched (silent
        // no-op) yet still reported success ("undo succeeded" while the
        // encapsulate stood). Clear BOTH layers together, so after any op that
        // flushes the native stack (encapsulate / GOP / refactor / clear / load)
        // undo is honest — empty → reply 0.0 — instead of a false success.
        if (list.size() >= 1) {
            auto canvas_symbol = list[0].toString();
            t_canvas* cnv = getCanvasBySymbol(canvas_symbol);
            if (!cnv && (canvas_symbol == "pd-main")) cnv = pd_this->pd_canvaslist;
            if (cnv) {
                wipeCanvasUndo(cnv);
                clearMcpTransactions(cnv);
            }
        }

        // Always also wipe undo on top-level root patch and all open canvas roots
        // so subpatch mutations never leave armed undo actions on the parent canvas.
        for (t_canvas* root = pd_this->pd_canvaslist; root; root = root->gl_next) {
            wipeCanvasUndo(root);
            clearMcpTransactions(root);
        }
        break;
    }
    case hash("mcp_suspend_dsp"): {
        sys_lock();
        session.mcpSuspendedDspState = canvas_suspend_dsp();
        sys_unlock();
        break;
    }
    case hash("mcp_resume_dsp"): {
        sys_lock();
        canvas_resume_dsp(session.mcpSuspendedDspState);
        sys_unlock();
        break;
    }
    case hash("mcp_create_batch_id"): {
        if (list.size() >= 3) {
            auto canvas_symbol = list[0].toString();
            auto correlation_id = list[1].toString();
            int count = (int)list[2].getFloat();
            int cursor = 3;
            int created = 0;

            std::vector<std::string> createdIds;
            std::vector<t_gobj*> createdPtrs;

            sys_lock();
            t_canvas* canvas = getCanvasBySymbol(canvas_symbol);
            if (canvas) {
                auto canonStr = MCPBridge::canonicalCanvasKey(canvas).toStdString();
                int const dspstate = canvas_suspend_dsp();
                pd::Patch patchWrapper(pd::WeakReference(canvas, &processor), &processor, false);
                for (int o = 0; o < count; o++) {
                    if (cursor + 5 >= (int)list.size()) break;
                    auto object_id = list[cursor].toString(); cursor++;
                    int x = (int)list[cursor].getFloat(); cursor++;
                    int y = (int)list[cursor].getFloat(); cursor++;
                    auto kind = list[cursor].toString(); cursor++;
                    auto objtype = list[cursor].toString(); cursor++;
                    int nargs = (int)list[cursor].getFloat(); cursor++;

                    StringArray tokens;
                    tokens.add(objtype);
                    for (int a = 0; a < nargs && cursor < (int)list.size(); a++) {
                        tokens.add(list[cursor].toString());
                        cursor++;
                    }

                    // IEM GUI short-form guard (see Pd/McpIemArgs.h).
                    tokens = mcp::expandIemGuiShortForm(tokens);

                    String objectText;
                    if (kind == "msg") {
                        if (!tokens.isEmpty() && tokens[0] == "msg") tokens.remove(0);
                        objectText = "msg " + tokens.joinIntoString(" ");
                    } else if (kind == "text" || kind == "comment") {
                        if (!tokens.isEmpty() && (tokens[0] == "text" || tokens[0] == "comment")) tokens.remove(0);
                        objectText = "comment " + tokens.joinIntoString(" ");
                    } else if (kind == "floatatom" || kind == "floatbox") {
                        if (!tokens.isEmpty() && (tokens[0] == "floatatom" || tokens[0] == "floatbox")) tokens.remove(0);
                        objectText = "floatbox " + tokens.joinIntoString(" ");
                    } else if (kind == "symbolatom" || kind == "symbolbox") {
                        if (!tokens.isEmpty() && (tokens[0] == "symbolatom" || tokens[0] == "symbolbox")) tokens.remove(0);
                        objectText = "symbolbox " + tokens.joinIntoString(" ");
                    } else {
                        objectText = tokens.joinIntoString(" ");
                    }

                    t_gobj* targetObj = patchWrapper.createObject(x, y, objectText);
                    if (targetObj) {
                        session.mcpStableObjectMap[canonStr][object_id.toStdString()] = targetObj;
                        session.mcpStableSerialMap[targetObj] = session.mcpSerialCounter++;
                        session.mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
                        createdIds.push_back(object_id.toStdString());
                        createdPtrs.push_back(targetObj);
                        created++;
                        if (pd_class(&targetObj->g_pd) == canvas_class) {
                            t_canvas* subcanvas = reinterpret_cast<t_canvas*>(targetObj);
                            if (!canvas_isabstraction(subcanvas)) {
                                pd_typedmess(reinterpret_cast<t_pd*>(subcanvas), gensym("clear"), 0, nullptr);
                                pd_typedmess(reinterpret_cast<t_pd*>(subcanvas), gensym("loadbang"), 0, nullptr);
                            }
                        }
                    }
                }
                canvas_resume_dsp(dspstate);
                canvas_dirty(canvas, 1);
            }

            SmallArray<pd::Atom> atoms;
            atoms.add(pd::Atom((float)created));

            // Append inline identity mappings: [tempId, index, tempId, index, ...]
            // sys_lock is still held here, so getObjectIndex is thread-safe.
            if (canvas) {
                for (size_t o = 0; o < createdIds.size(); o++) {
                    int index = getObjectIndex(canvas, createdPtrs[o]);
                    if (index >= 0) {
                        atoms.add(pd::Atom(processor.generateSymbol(String(createdIds[o]))));
                        atoms.add(pd::Atom((float)index));
                    }
                }
            }
            sys_unlock();

            sendMCPReply(String("/pd/create_batch_id/reply/" + correlation_id), atoms);
            processor.sendMessagesFromQueue();
            processor.enqueueFunctionAsync([this] { synchroniseCanvases(); });
        }
        break;
    }
    case hash("mcp_create_id"): {
        if (list.size() >= 6) {
            auto canvas_symbol = list[0].toString();
            auto correlation_id = list[1].toString();
            auto object_id = list[2].toString();
            int x = (int)list[3].getFloat();
            int y = (int)list[4].getFloat();
            auto kind = list[5].toString();

            StringArray tokens;
            for (int i = 6; i < list.size(); i++) {
                tokens.add(list[i].toString());
            }

            // IEM GUI short-form guard (see Pd/McpIemArgs.h).
            tokens = mcp::expandIemGuiShortForm(tokens);

            String objectText;
            if (kind == "msg") {
                if (!tokens.isEmpty() && tokens[0] == "msg") tokens.remove(0);
                objectText = "msg " + tokens.joinIntoString(" ");
            } else if (kind == "text" || kind == "comment") {
                if (!tokens.isEmpty() && (tokens[0] == "text" || tokens[0] == "comment")) tokens.remove(0);
                objectText = "comment " + tokens.joinIntoString(" ");
            } else if (kind == "floatatom" || kind == "floatbox") {
                if (!tokens.isEmpty() && (tokens[0] == "floatatom" || tokens[0] == "floatbox")) tokens.remove(0);
                objectText = "floatbox " + tokens.joinIntoString(" ");
            } else if (kind == "symbolatom" || kind == "symbolbox") {
                if (!tokens.isEmpty() && (tokens[0] == "symbolatom" || tokens[0] == "symbolbox")) tokens.remove(0);
                objectText = "symbolbox " + tokens.joinIntoString(" ");
            } else {
                objectText = tokens.joinIntoString(" ");
            }

            sys_lock();
            t_canvas* canvas = getCanvasBySymbol(canvas_symbol);
            bool success = false;
            if (canvas) {
                auto canonStr = MCPBridge::canonicalCanvasKey(canvas).toStdString();
                pd::Patch patchWrapper(pd::WeakReference(canvas, &processor), &processor, false);
                t_gobj* targetObj = patchWrapper.createObject(x, y, objectText);
                if (targetObj) {
                    session.mcpStableObjectMap[canonStr][object_id.toStdString()] = targetObj;
                    session.mcpStableSerialMap[targetObj] = session.mcpSerialCounter++;
                    session.mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
                    success = true;
                    canvas_dirty(canvas, 1);
                    if (pd_class(&targetObj->g_pd) == canvas_class) {
                        t_canvas* subcanvas = reinterpret_cast<t_canvas*>(targetObj);
                        if (!canvas_isabstraction(subcanvas)) {
                            pd_typedmess(reinterpret_cast<t_pd*>(subcanvas), gensym("clear"), 0, nullptr);
                            pd_typedmess(reinterpret_cast<t_pd*>(subcanvas), gensym("loadbang"), 0, nullptr);
                        }
                    }
                }
            }
            sys_unlock();

            SmallArray<pd::Atom> atoms;
            atoms.add(pd::Atom(success ? 1.0f : 0.0f));
            sendMCPReply(String("/pd/mcp_create_id/reply/" + correlation_id), atoms);
            processor.sendMessagesFromQueue();
            processor.enqueueFunctionAsync([this] { synchroniseCanvases(); });
        }
        break;
    }
    case hash("mcp_register_id"): {
        if (list.size() >= 4) {
            auto canvas_symbol = list[0].toString();
            auto correlation_id = list[1].toString();
            int object_index = (int)list[2].getFloat();
            auto object_id = list[3].toString();

            sys_lock();
            t_canvas* canvas = getCanvasBySymbol(canvas_symbol);
            bool success = false;
            if (canvas) {
                auto canonStr = MCPBridge::canonicalCanvasKey(canvas).toStdString();
                t_gobj* targetObj = nullptr;
                if (object_index == -1) {
                    targetObj = pd::Interface::getNewest(canvas);
                } else {
                    int currentIndex = 0;
                    for (t_gobj* y_obj = canvas->gl_list; y_obj; y_obj = y_obj->g_next) {
                        if (currentIndex == object_index) {
                            targetObj = y_obj;
                            break;
                        }
                        currentIndex++;
                    }
                }

                if (targetObj) {
                    session.mcpStableObjectMap[canonStr][object_id.toStdString()] = targetObj;
                    session.mcpStableSerialMap[targetObj] = session.mcpSerialCounter++;
                    session.mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
                    success = true;
                }
            }
            sys_unlock();

            SmallArray<pd::Atom> atoms;
            atoms.add(pd::Atom(success ? 1.0f : 0.0f));
            sendMCPReply(String("/pd/mcp_register_id/reply/" + correlation_id), atoms);
            processor.sendMessagesFromQueue();
        }
        break;
    }
    case hash("mcp_connect_id"): {
        if (list.size() >= 6) {
            auto canvas_symbol = list[0].toString();
            auto correlation_id = list[1].toString();
            auto src_id = list[2].toString();
            int src_out = (int)list[3].getFloat();
            auto dest_id = list[4].toString();
            int dest_in = (int)list[5].getFloat();

            sys_lock();
            t_canvas* canvas = getCanvasBySymbol(canvas_symbol);
            bool success = false;
            if (canvas) {
                t_gobj* srcObj = resolveStableId(canvas_symbol, src_id);
                t_gobj* destObj = resolveStableId(canvas_symbol, dest_id);
                t_object* srcText = pd::Interface::checkObject(srcObj);
                t_object* destText = pd::Interface::checkObject(destObj);

                if (srcText && destText && pd::Interface::canConnect(canvas, srcText, src_out, destText, dest_in)) {
                    int srcIdx = glist_getindex(canvas, srcObj);
                    int destIdx = glist_getindex(canvas, destObj);
                    if (srcIdx >= 0 && destIdx >= 0) {
                        t_atom cArgs[4];
                        SETFLOAT(&cArgs[0], static_cast<float>(srcIdx));
                        SETFLOAT(&cArgs[1], static_cast<float>(src_out));
                        SETFLOAT(&cArgs[2], static_cast<float>(destIdx));
                        SETFLOAT(&cArgs[3], static_cast<float>(dest_in));
                        pd_typedmess(reinterpret_cast<t_pd*>(canvas), gensym("connect"), 4, cArgs);
                        canvas_dirty(canvas, 1);
                        success = true;
                    }
                }
            }
            sys_unlock();

            SmallArray<pd::Atom> atoms;
            atoms.add(pd::Atom(success ? 1.0f : 0.0f));
            sendMCPReply(String("/pd/mcp_connect_id/reply/" + correlation_id), atoms);
            processor.sendMessagesFromQueue();
            processor.enqueueFunctionAsync([this] { synchroniseCanvases(); });
        }
        break;
    }
    case hash("mcp_disconnect_id"): {
        if (list.size() >= 6) {
            auto canvas_symbol = list[0].toString();
            auto correlation_id = list[1].toString();
            auto src_id = list[2].toString();
            int src_out = (int)list[3].getFloat();
            auto dest_id = list[4].toString();
            int dest_in = (int)list[5].getFloat();

            sys_lock();
            t_canvas* canvas = getCanvasBySymbol(canvas_symbol);
            bool success = false;
            if (canvas) {
                t_gobj* srcObj = resolveStableId(canvas_symbol, src_id);
                t_gobj* destObj = resolveStableId(canvas_symbol, dest_id);
                t_object* srcText = pd::Interface::checkObject(srcObj);
                t_object* destText = pd::Interface::checkObject(destObj);

                if (srcText && destText && pd::Interface::hasConnection(canvas, srcText, src_out, destText, dest_in)) {
                    pd::Interface::removeConnection(canvas, srcText, src_out, destText, dest_in, nullptr);
                    canvas_dirty(canvas, 1);
                    success = true;
                }
            }
            sys_unlock();

            SmallArray<pd::Atom> atoms;
            atoms.add(pd::Atom(success ? 1.0f : 0.0f));
            sendMCPReply(String("/pd/mcp_disconnect_id/reply/" + correlation_id), atoms);
            processor.sendMessagesFromQueue();
            processor.enqueueFunctionAsync([this] { synchroniseCanvases(); });
        }
        break;
    }
    case hash("mcp_delete_id"): {
        if (list.size() >= 3) {
            auto canvas_symbol = list[0].toString();
            auto correlation_id = list[1].toString();
            auto object_id = list[2].toString();

            sys_lock();
            t_canvas* canvas = getCanvasBySymbol(canvas_symbol);
            bool success = false;
            if (canvas) {
                auto canonStr = MCPBridge::canonicalCanvasKey(canvas).toStdString();
                t_gobj* targetObj = resolveStableId(canvas_symbol, object_id);
                if (targetObj) {
                    SmallArray<t_gobj*> objectsToDelete;
                    objectsToDelete.add(targetObj);
                    mcp::PdOps::removeObjects(canvas, objectsToDelete);
                    session.mcpStableObjectMap[canonStr].erase(object_id.toStdString());
                    session.mcpStableSerialMap.erase(targetObj);
                    session.mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
                    success = true;
                    canvas_dirty(canvas, 1);
                }
            }
            sys_unlock();

            // Force full GUI repaint (matches mcp_move_id / mcp_edit_id pattern)
            {
                SmallArray<pd::Atom> syncAtoms;
                syncAtoms.add(pd::Atom(-1.0f));
                syncAtoms.add(pd::Atom(-1.0f));
                syncAtoms.add(pd::Atom(-1.0f));
                syncAtoms.add(pd::Atom(-1.0f));
                processor.sendMessage(canvas_symbol.toRawUTF8(), "disconnect", syncAtoms);
            }

            SmallArray<pd::Atom> atoms;
            atoms.add(pd::Atom(success ? 1.0f : 0.0f));
            sendMCPReply(String("/pd/mcp_delete_id/reply/" + correlation_id), atoms);
            processor.sendMessagesFromQueue();
            processor.enqueueFunctionAsync([this] { synchroniseCanvases(); });
        }
        break;
    }
    case hash("mcp_rename_id"): {
        if (list.size() >= 4) {
            auto canvas_symbol = list[0].toString();
            auto correlation_id = list[1].toString();
            auto old_id = list[2].toString();
            auto new_id = list[3].toString();

            sys_lock();
            bool success = false;
            t_canvas* canvas = getCanvasBySymbol(canvas_symbol);
            auto canvasStr = (canvas ? MCPBridge::canonicalCanvasKey(canvas) : canvas_symbol).toStdString();
            auto& map = session.mcpStableObjectMap[canvasStr];
            auto const renamed = mcp::identity::renameStableId(map, old_id.toStdString(), new_id.toStdString());
            if (renamed == mcp::identity::RenameResult::Applied)
                session.mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
            success = renamed != mcp::identity::RenameResult::Missing;
            sys_unlock();

            SmallArray<pd::Atom> atoms;
            atoms.add(pd::Atom(success ? 1.0f : 0.0f));
            sendMCPReply(String("/pd/mcp_rename_id/reply/" + correlation_id), atoms);
            processor.sendMessagesFromQueue();
        }
        break;
    }
    case hash("mcp_move_id"): {
        if (list.size() >= 5) {
            auto canvas_symbol = list[0].toString();
            auto correlation_id = list[1].toString();
            auto object_id = list[2].toString();
            int x = (int)list[3].getFloat();
            int y = (int)list[4].getFloat();

            sys_lock();
            t_canvas* canvas = getCanvasBySymbol(canvas_symbol);
            bool success = false;
            if (canvas) {
                t_gobj* targetObj = resolveStableId(canvas_symbol, object_id);
                if (targetObj) {
                    pd::Interface::moveObject(canvas, targetObj, x, y);
                    canvas_dirty(canvas, 1);
                    success = true;
                }
            }
            sys_unlock();


            SmallArray<pd::Atom> atoms;
            atoms.add(pd::Atom(success ? 1.0f : 0.0f));
            sendMCPReply(String("/pd/mcp_move_id/reply/" + correlation_id), atoms);
            processor.sendMessagesFromQueue();
        }
        break;
    }
    case hash("mcp_edit_id"): {
        if (list.size() >= 4) {
            auto canvas_symbol = list[0].toString();
            auto correlation_id = list[1].toString();
            auto object_id = list[2].toString();
            
            String new_text = list[3].toString();
            if (list.size() > 4) {
                StringArray tokens;
                for (int i = 3; i < list.size(); i++) {
                    tokens.add(list[i].toString());
                }
                new_text = tokens.joinIntoString(" ");
            }

            sys_lock();
            t_canvas* canvas = getCanvasBySymbol(canvas_symbol);
            bool success = false;
            if (canvas) {
                auto canonStr = MCPBridge::canonicalCanvasKey(canvas).toStdString();
                t_gobj* targetObj = resolveStableId(canvas_symbol, object_id);
                if (targetObj) {
                    mcp::PdOps::renameObject(canvas, targetObj, new_text.toRawUTF8(), new_text.getNumBytesAsUTF8());
                    
                    // Check if targetObj is still present on the canvas
                    bool stillExists = false;
                    for (t_gobj* y_obj = canvas->gl_list; y_obj; y_obj = y_obj->g_next) {
                        if (y_obj == targetObj) {
                            stillExists = true;
                            break;
                        }
                    }
                    
                    t_gobj* newObj = nullptr;
                    if (stillExists) {
                        newObj = targetObj;
                    } else {
                        newObj = pd::Interface::getNewest(canvas);
                    }
                    
                    if (newObj) {
                        session.mcpStableObjectMap[canonStr][object_id.toStdString()] = newObj;
                        if (newObj != targetObj) {
                            session.mcpStableSerialMap.erase(targetObj);
                        }
                        session.mcpStableSerialMap[newObj] = session.mcpSerialCounter++;
                        session.mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
                        success = true;
                    }
                }
            }
            sys_unlock();


            SmallArray<pd::Atom> atoms;
            atoms.add(pd::Atom(success ? 1.0f : 0.0f));
            sendMCPReply(String("/pd/mcp_edit_id/reply/" + correlation_id), atoms);
            processor.sendMessagesFromQueue();
        }
        break;
    }
    case hash("mcp_get_mappings"): {
        if (list.size() >= 2) {
            auto canvas_symbol = list[0].toString();
            auto correlation_id = list[1].toString();

            sys_lock();
            t_canvas* canvas = getCanvasBySymbol(canvas_symbol);
            SmallArray<pd::Atom> atoms;
            if (canvas) {
                auto canonStr = MCPBridge::canonicalCanvasKey(canvas).toStdString();
                auto& canvasMap = session.mcpStableObjectMap[canonStr];
                
                // For each ID in the map, find its current index in gl_list
                for (auto it = canvasMap.begin(); it != canvasMap.end();) {
                    t_gobj* targetObj = it->second;
                    int index = getObjectIndex(canvas, targetObj);
                    if (index != -1) {
                        // Object still exists on the canvas, add to atoms
                        atoms.add(pd::Atom(gensym(it->first.c_str())));
                        atoms.add(pd::Atom((float)index));
                        ++it;
                    } else {
                        // Object was deleted/freed, erase from stable map
                        it = canvasMap.erase(it);
                    }
                }
            }
            sys_unlock();

            sendMCPReply(String("/pd/mcp_get_mappings/reply/" + correlation_id), atoms);
            processor.sendMessagesFromQueue();
        }
        break;
    }
    case hash("mcp_census"): {
        if (list.size() >= 2) {
            auto canvas_symbol = list[0].toString();
            auto correlation_id = list[1].toString();

            String jsonString;
            sys_lock();
            // STRICT: an unknown canvas name must NOT silently fall back to the
            // root canvas (that returned the root's objects under a bogus
            // subpatch label with re-minted tempIds, e.g.
            // osc_sig_pd_<typo>_0). 'main'/'pd-main'/'' aliases still resolve.
            t_canvas* canvas = getCanvasBySymbolStrict(canvas_symbol);

            if (canvas) {
                // One canonical key for this canvas — the SAME key every
                // identity site (bridge sidecar/load/create) stores under.
                // The caller's raw symbol ("main", focus-dependent) is only an
                // addressing alias, never the map key.
                juce::String canonKey = MCPBridge::canonicalCanvasKey(canvas);
                auto canonStr = canonKey.toStdString();

                auto* rootObj = new DynamicObject();
                rootObj->setProperty("canvas", canvas_symbol);

                // Build reverse map of gobj* -> tempId
                std::unordered_map<t_gobj*, String> ptrToId;
                if (session.mcpStableObjectMap.count(canonStr)) {
                    for (auto const& [id, ptr] : session.mcpStableObjectMap[canonStr]) {
                        if (ptr) ptrToId[ptr] = String(id);
                    }
                }

                Array<var> objArray;
                int objectIndex = 0;
                bool adoptedAny = false; // Phase 2 §2.1: any auto-adoption this census
                for (t_gobj* y_obj = canvas->gl_list; y_obj; y_obj = y_obj->g_next, ++objectIndex) {
                    auto* o = new DynamicObject();
                    o->setProperty("index", objectIndex);

                    String tempId = ptrToId.count(y_obj) ? ptrToId[y_obj] : String();
                    o->setProperty("id", tempId);

                    t_class* cl = pd_class(&y_obj->g_pd);
                    const char* clName = class_getname(cl);
                    String className = clName ? String::fromUTF8(clName) : String("unknown");

                    t_object* ob = pd_checkobject(&y_obj->g_pd);
                    int ninlets = ob ? obj_ninlets(ob) : 0;
                    int noutlets = ob ? obj_noutlets(ob) : 0;
                    o->setProperty("inlets", ninlets);
                    o->setProperty("outlets", noutlets);

                    int x = 0, y = 0, w = 0, h = 0;
                    pd::Interface::getObjectBounds(canvas, y_obj, &x, &y, &w, &h);
                    o->setProperty("x", x);
                    o->setProperty("y", y);
                    o->setProperty("width", w);
                    o->setProperty("height", h);

                    String kind = "obj";
                    String type = className;
                    // PRD Phase 2 §2.1: real class name for file-loaded
                    // abstractions (MERDA .m~, .pd, GOP); empty for inline
                    // [pd foo] and regular objects. Hoisted so the auto-adopt
                    // block below can mint the tempId from it.
                    juce::String abstractName;
                    Array<var> argsList;

                    if (cl == canvas_class) {
                        kind = "canvas";
                        t_canvas* sub = reinterpret_cast<t_canvas*>(y_obj);
                        type = "pd";
                        if (sub->gl_name) {
                            argsList.add(String::fromUTF8(sub->gl_name->s_name));
                        }
                        // PRD §2.2 (Phase 1): expose the real class name for
                        // abstractions loaded from disk. `type` stays "pd" for
                        // structural backward-compat (§2.6.2 Option A);
                        // `node_class` carries the human-facing identity.
                        // Inline [pd foo] gets none.
                        pd::getAbstractionFileName(y_obj, abstractName);
                    } else if (cl == garray_class) {
                        kind = "table";
                        type = "table";
                        t_garray* ga = reinterpret_cast<t_garray*>(y_obj);
                        t_symbol* arrayName = nullptr;
                        if (garray_getname(ga, &arrayName) && arrayName) {
                            argsList.add(String::fromUTF8(arrayName->s_name));
                        }
                        int arraySize = 0;
                        t_word* vec = nullptr;
                        if (garray_getfloatwords(ga, &arraySize, &vec)) {
                            argsList.add(arraySize);
                        }
                    } else if (pd::Interface::isTextObject(y_obj)) {
                        t_text* textObj = reinterpret_cast<t_text*>(y_obj);
                        if (textObj->te_type == T_MESSAGE) {
                            kind = "msg";
                            type = "msg";
                        } else if (textObj->te_type == T_ATOM) {
                            kind = (className.containsIgnoreCase("symbol") ? "symbolatom" : "floatatom");
                            type = kind;
                        } else if (textObj->te_type == T_TEXT) {
                            kind = "text";
                            type = "text";
                        }

                        char* textBuf = nullptr;
                        int textSize = 0;
                        binbuf_gettext(textObj->te_binbuf, &textBuf, &textSize);
                        if (textBuf && textSize > 0) {
                            String fullText = String::fromUTF8(textBuf, textSize).trim();
                            StringArray tokens;
                            tokens.addTokens(fullText, " \t\r\n", "\"");
                            if (tokens.size() > 0) {
                                if (kind == "obj") {
                                    type = tokens[0];
                                    for (int tIdx = 1; tIdx < tokens.size(); ++tIdx) {
                                        argsList.add(tokens[tIdx]);
                                    }
                                } else {
                                    for (auto const& tok : tokens) {
                                        argsList.add(tok);
                                    }
                                }
                            }
                            freebytes(textBuf, textSize);
                        }
                    } else {
                        // IEM GUI or custom widget
                        kind = "gui";
                        type = className;
                    }

                    o->setProperty("kind", kind);
                    o->setProperty("type", type);
                    if (abstractName.isNotEmpty()) {
                        o->setProperty("node_class", abstractName);
                    }
                    o->setProperty("args", argsList);

                    // ── GUI send/receive for lint (orphan knob detection) ──
                    // Vanilla iemgui + else/knob store snd/rcv outside binbuf; expose them so lint can see them.
                    if (kind == "gui") {
                        String sndSym, rcvSym;
                        if (type == "knob") {
                            auto* knb = reinterpret_cast<t_fake_knob*>(y_obj);
                            // t_fake_knob layout matches pd-else t_knob for this region
                            knob_get_rcv(knb);
                            knob_get_snd(knb);
                            auto valid = [](t_symbol* s) -> bool {
                                return s && s->s_name && s->s_name[0]
                                    && String::fromUTF8(s->s_name) != "empty";
                            };
                            if (knb->x_rcv_raw && valid(knb->x_rcv_raw)) rcvSym = String::fromUTF8(knb->x_rcv_raw->s_name).replace("\\ ", " ");
                            else if (knb->x_snd_raw && valid(knb->x_snd_raw)) sndSym = String::fromUTF8(knb->x_snd_raw->s_name).replace("\\ ", " ");
                            // knob reports rcv as primary (receive), snd as fallback
                            if (rcvSym.isNotEmpty()) o->setProperty("rcv", rcvSym);
                            if (sndSym.isNotEmpty()) o->setProperty("snd", sndSym);
                        } else {
                            // Only genuine iemgui objects share the t_iemgui
                            // layout. nbx/gatom (and other custom widgets) do
                            // NOT — reading x_fsf/x_rcv/x_snd from them reads
                            // out-of-layout garbage (a dangling t_symbol*),
                            // which crashed the census nondeterministically.
                            // Guard by class name before the reinterpret_cast.
                            static const std::unordered_set<std::string> iemGuiClasses = {
                                "hsl", "vsl", "tgl", "bng", "vu",
                                "hradio", "vradio", "cnv", "iemgui"
                            };
                            if (iemGuiClasses.count(type.toStdString()) == 0) {
                                o->setProperty("kind", kind);
                                o->setProperty("type", type);
                                if (abstractName.isNotEmpty()) o->setProperty("node_class", abstractName);
                                o->setProperty("args", argsList);
                                objArray.add(var(o));
                                continue;
                            }
                            auto* iem = reinterpret_cast<t_iemgui*>(y_obj);
                            // Guard with x_fsf — raw pointers are garbage when not set (vu bug: "nosndno")
                            bool hasRcv = iem->x_fsf.x_rcv_able;
                            bool hasSnd = iem->x_fsf.x_snd_able;
                            auto valid = [](t_symbol* s) -> bool {
                                return s && s != gensym("") && s->s_name && s->s_name[0]
                                    && String::fromUTF8(s->s_name) != "empty";
                            };
                            if (hasRcv && valid(iem->x_rcv)) rcvSym = String::fromUTF8(iem->x_rcv->s_name);
                            if (hasSnd && valid(iem->x_snd)) sndSym = String::fromUTF8(iem->x_snd->s_name);
                            if (rcvSym.isNotEmpty()) o->setProperty("rcv", rcvSym);
                            if (sndSym.isNotEmpty()) o->setProperty("snd", sndSym);
                        }
                    }

                    // ── PRD Phase 2 §2.1: NATIVE AUTO-ADOPTION ─────────────
                    // C++ mints a readable tempId for every object not yet in
                    // the stable map (i.e. hand-added by the artist in the
                    // GUI). One census and the object is named, resolvable by
                    // probe/meter, and version-bumped — zero TS involvement,
                    // zero adoption window. Naming mirrors the TS policy
                    // exactly: sanitize(node_class || type)_<canvas>_<index>.
                    // Never overwrites an existing (possibly artist-renamed)
                    // registration.
                    if (tempId.isEmpty()) {
                        juce::String classSrc = abstractName.isNotEmpty() ? abstractName : type;
                        // Minted tempIds use the CANONICAL canvas key as their
                        // base — never the caller's raw alias — so a miss after
                        // a focus switch reads sig_ident_test_pd_3, never
                        // sig_main_3. Subcanvases drop the "pd-" prefix to keep
                        // names clean (pd-mysub → mysub).
                        juce::String canvasKeySymbol = canonKey;
                        if (canvasKeySymbol.startsWith("pd-"))
                            canvasKeySymbol = canvasKeySymbol.substring(3);
                        juce::String canvasKey;
                        for (auto ch : canvasKeySymbol)
                            canvasKey << (juce::CharacterFunctions::isLetterOrDigit(ch) || ch == '_'
                                               ? juce::String::charToString(ch)
                                               : juce::String("_"));
                        juce::String minted = pd::sanitizeClassNameForTempId(classSrc)
                                                  + "_" + canvasKey + "_" + String(objectIndex);

                        auto& canvasIdMap = session.mcpStableObjectMap[canonStr];
                        auto existing = canvasIdMap.find(minted.toStdString());
                        if (existing == canvasIdMap.end()) {
                            canvasIdMap[minted.toStdString()] = y_obj;
                            if (session.mcpStableSerialMap.find(y_obj) == session.mcpStableSerialMap.end()) {
                                session.mcpStableSerialMap[y_obj] = session.mcpSerialCounter++;
                            }
                            tempId = minted;
                            adoptedAny = true;
                        } else if (existing->second == y_obj) {
                            // Already registered under exactly this minted name
                            tempId = minted;
                        }
                        // else: minted name collides with a different object —
                        // leave unregistered (artist rename territory).
                        o->setProperty("id", tempId);
                    }

                    objArray.add(var(o));
                }
                // One version bump per census that adopted anything — the
                // identity map changed, so identity_snapshot consumers resync.
                if (adoptedAny) {
                    session.mcpIdentityVersion.fetch_add(1, std::memory_order_relaxed);
                }
                rootObj->setProperty("objectCount", objectIndex);
                rootObj->setProperty("objects", objArray);

                Array<var> connArray;
                int connCount = 0;
                t_linetraverser t;
                t_outconnect *oc = nullptr;
                linetraverser_start(&t, canvas);
                while ((oc = linetraverser_next_nosize(&t))) {
                    int srcIndex = getObjectIndex(canvas, &t.tr_ob->ob_g);
                    int sinkIndex = getObjectIndex(canvas, &t.tr_ob2->ob_g);
                    int srcOut = t.tr_outno;
                    int destIn = t.tr_inno;

                    auto* c = new DynamicObject();
                    c->setProperty("srcIndex", srcIndex);
                    c->setProperty("srcOut", srcOut);
                    c->setProperty("destIndex", sinkIndex);
                    c->setProperty("destIn", destIn);

                    t_gobj* srcGobj = &t.tr_ob->ob_g;
                    t_gobj* sinkGobj = &t.tr_ob2->ob_g;
                    c->setProperty("srcId", ptrToId.count(srcGobj) ? ptrToId[srcGobj] : String());
                    c->setProperty("destId", ptrToId.count(sinkGobj) ? ptrToId[sinkGobj] : String());

                    connArray.add(var(c));
                    connCount++;
                }
                rootObj->setProperty("connectionCount", connCount);
                rootObj->setProperty("connections", connArray);

                jsonString = JSON::toString(var(rootObj), true);
            } else {
                auto* errObj = new DynamicObject();
                errObj->setProperty("error", "Canvas not found");
                errObj->setProperty("canvas", canvas_symbol);
                jsonString = JSON::toString(var(errObj), true);
            }
            sys_unlock();

            SmallArray<pd::Atom> atoms;
            atoms.add(pd::Atom(gensym(jsonString.toRawUTF8())));
            sendMCPReply(String("/pd/census/reply/" + correlation_id), atoms);
            processor.sendMessagesFromQueue();
        }
        break;
    }
    case hash("mcp_move_batch_id"): {
        if (list.size() >= 2) {
            auto canvas_symbol = list[0].toString();
            auto correlation_id = list[1].toString();

            int movedCount = 0;
            struct MoveUndo { juce::String id; int oldX, oldY, newX, newY; };
            std::vector<MoveUndo> moveUndo;
            sys_lock();
            t_canvas* canvas = getCanvasBySymbol(canvas_symbol);
            if (canvas) {
                for (int i = 2; i + 2 < list.size(); i += 3) {
                    auto object_id = list[i].toString();
                    int x = (int)list[i+1].getFloat();
                    int y = (int)list[i+2].getFloat();

                    t_gobj* targetObj = resolveStableId(canvas_symbol, object_id);
                    if (targetObj) {
                        int ox = 0, oy = 0, ow = 0, oh = 0;
                        pd::Interface::getObjectBounds(canvas, targetObj, &ox, &oy, &ow, &oh);
                        pd::Interface::moveObject(canvas, targetObj, x, y);
                        movedCount++;
                        if (ox != x || oy != y)
                            moveUndo.push_back({ object_id, ox, oy, x, y });
                    }
                }
                if (movedCount > 0) {
                    canvas_dirty(canvas, 1);
                    canvas_undo_free(canvas);
                    if (auto* udo = canvas_undo_get(canvas)) {
                        udo->u_queue = nullptr;
                        udo->u_last = nullptr;
                        udo->u_cleanstate = nullptr;
                    }
                }
            }
            sys_unlock();

            // Register an undoable move (message-delta, replayed via move_batch_id).
            // Skipped while an undo/redo is replaying so we never push recursively.
            if (canvas && !moveUndo.empty() && !session.isExecutingMcpUndoRedo) {
                juce::OSCMessage fwd { juce::OSCAddressPattern("/pd/move_batch_id") };
                juce::OSCMessage inv { juce::OSCAddressPattern("/pd/move_batch_id") };
                fwd.addArgument(canvas_symbol);
                inv.addArgument(canvas_symbol);
                fwd.addArgument(correlation_id);
                inv.addArgument(correlation_id);
                for (auto const& m : moveUndo) {
                    fwd.addArgument(m.id); fwd.addArgument(static_cast<float>(m.newX)); fwd.addArgument(static_cast<float>(m.newY));
                    inv.addArgument(m.id); inv.addArgument(static_cast<float>(m.oldX)); inv.addArgument(static_cast<float>(m.oldY));
                }
                pushMcpTransaction(canvas, canvas_symbol, fwd, inv, "move_batch_id");
            }

            SmallArray<pd::Atom> atoms;
            atoms.add(pd::Atom((float)movedCount));
            sendMCPReply(String("/pd/mcp_move_batch_id/reply/" + correlation_id), atoms);
            processor.sendMessagesFromQueue();
            processor.enqueueFunctionAsync([this] { synchroniseCanvases(); });
        }
        break;
    }
    case hash("mcp_connect_batch_id"): {
        if (list.size() >= 2) {
            auto canvas_symbol = list[0].toString();
            auto correlation_id = list[1].toString();

            int connectedCount = 0;
            sys_lock();
            t_canvas* canvas = getCanvasBySymbol(canvas_symbol);
            if (canvas) {
                for (int i = 2; i + 3 < list.size(); i += 4) {
                    auto src_id = list[i].toString();
                    int src_out = (int)list[i+1].getFloat();
                    auto dest_id = list[i+2].toString();
                    int dest_in = (int)list[i+3].getFloat();

                    t_gobj* srcObj = resolveStableId(canvas_symbol, src_id);
                    t_gobj* destObj = resolveStableId(canvas_symbol, dest_id);
                    t_object* srcText = pd::Interface::checkObject(srcObj);
                    t_object* destText = pd::Interface::checkObject(destObj);

                    if (srcText && destText && pd::Interface::canConnect(canvas, srcText, src_out, destText, dest_in)) {
                        int srcIdx = glist_getindex(canvas, srcObj);
                        int destIdx = glist_getindex(canvas, destObj);
                        if (srcIdx >= 0 && destIdx >= 0) {
                            t_atom cArgs[4];
                            SETFLOAT(&cArgs[0], static_cast<float>(srcIdx));
                            SETFLOAT(&cArgs[1], static_cast<float>(src_out));
                            SETFLOAT(&cArgs[2], static_cast<float>(destIdx));
                            SETFLOAT(&cArgs[3], static_cast<float>(dest_in));
                            pd_typedmess(reinterpret_cast<t_pd*>(canvas), gensym("connect"), 4, cArgs);
                            connectedCount++;
                        }
                    }
                }
                if (connectedCount > 0) {
                    canvas_dirty(canvas, 1);
                }
            }
            sys_unlock();

            SmallArray<pd::Atom> atoms;
            atoms.add(pd::Atom((float)connectedCount));
            sendMCPReply(String("/pd/mcp_connect_batch_id/reply/" + correlation_id), atoms);
            processor.sendMessagesFromQueue();
            processor.enqueueFunctionAsync([this] { synchroniseCanvases(); });
        }
        break;
    }
    case hash("mcp_disconnect_batch_id"): {
        if (list.size() >= 2) {
            auto canvas_symbol = list[0].toString();
            auto correlation_id = list[1].toString();

            int disconnectedCount = 0;
            sys_lock();
            t_canvas* canvas = getCanvasBySymbol(canvas_symbol);
            if (canvas) {
                for (int i = 2; i + 3 < list.size(); i += 4) {
                    auto src_id = list[i].toString();
                    int src_out = (int)list[i+1].getFloat();
                    auto dest_id = list[i+2].toString();
                    int dest_in = (int)list[i+3].getFloat();

                    t_gobj* srcObj = resolveStableId(canvas_symbol, src_id);
                    t_gobj* destObj = resolveStableId(canvas_symbol, dest_id);
                    t_object* srcText = pd::Interface::checkObject(srcObj);
                    t_object* destText = pd::Interface::checkObject(destObj);

                    if (srcText && destText && pd::Interface::hasConnection(canvas, srcText, src_out, destText, dest_in)) {
                        pd::Interface::removeConnection(canvas, srcText, src_out, destText, dest_in, nullptr);
                        disconnectedCount++;
                    }
                }
                if (disconnectedCount > 0) {
                    canvas_dirty(canvas, 1);
                }
            }
            sys_unlock();

            SmallArray<pd::Atom> atoms;
            atoms.add(pd::Atom((float)disconnectedCount));
            sendMCPReply(String("/pd/mcp_disconnect_batch_id/reply/" + correlation_id), atoms);
            processor.sendMessagesFromQueue();
            processor.enqueueFunctionAsync([this] { synchroniseCanvases(); });
        }
        break;
    }
    default:
        break;
    }
}
