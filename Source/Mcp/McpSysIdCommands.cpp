/*
 // The id-addressed mcp_* Pd system messages (Plan 3 P3.3 S7): the *_id and *_batch_id
 // forms the legacy /pd verbs of MCPBridgeCmdLegacy.cpp send, and mcp_edit_id. Owner
 // decision O1 removes them with those verbs (P3.3 S9). Bodies moved verbatim out of
 // McpHost::dispatchSysMessage.
 */
#include "Mcp/McpSysCommon.h"

void McpHost::sysCreateBatchId(SmallString const& selector, SmallArray<pd::Atom> const& list)
{
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
}

void McpHost::sysCreateId(SmallString const& selector, SmallArray<pd::Atom> const& list)
{
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
}

void McpHost::sysConnectId(SmallString const& selector, SmallArray<pd::Atom> const& list)
{
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
}

void McpHost::sysDisconnectId(SmallString const& selector, SmallArray<pd::Atom> const& list)
{
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
}

void McpHost::sysDeleteId(SmallString const& selector, SmallArray<pd::Atom> const& list)
{
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
}

void McpHost::sysMoveId(SmallString const& selector, SmallArray<pd::Atom> const& list)
{
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
}

void McpHost::sysEditId(SmallString const& selector, SmallArray<pd::Atom> const& list)
{
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
}

void McpHost::sysMoveBatchId(SmallString const& selector, SmallArray<pd::Atom> const& list)
{
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
}

void McpHost::sysConnectBatchId(SmallString const& selector, SmallArray<pd::Atom> const& list)
{
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
}

void McpHost::sysDisconnectBatchId(SmallString const& selector, SmallArray<pd::Atom> const& list)
{
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
}
