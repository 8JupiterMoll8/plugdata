/*
 // The mcp_* Pd system messages (Plan 3 P3.2 S5, registry P3.3 S7). These were cases of
 // PluginProcessor::receiveSysMessage, moved verbatim (with `session.` / `processor.`
 // prefixes). PluginProcessor calls McpHost::handleSysMessage first; every other
 // selector returns false and falls through to plugdata's own switch as before.
 // Each selector is one row of McpSysVerbs.inc. The id-addressed forms were removed
 // with the legacy /pd verbs that sent them (P3.3 S9); mcp_move_batch_id stays because
 // McpHost undo/redo replays a move transaction through it.
 */
#include "Mcp/McpSysCommon.h"

// Empties a canvas's native undo queue. Caller holds the Pd lock. Pd keeps an UNDO_INIT action at the
// head of every queue (canvas_undo_init at canvas creation, g_canvas.c) and canvas_undo_undo walks back
// to it; canvas_undo_free drops it too, so it is put back here. Without it the first GUI action after an
// MCP flush became the head, undoing it walked off the queue and left u_last NULL, and the next GUI edit
// crashed in canvas_undo_init (finding L6, P3.4 S9).
// Side effect (accepted, conservative): the re-init marks the canvas's clean state unknown, so undoing
// back to the head after an MCP flush leaves the patch flagged unsaved even right after a save.
static void resetNativeUndo(t_canvas* c)
{
    if (!c) return;
    canvas_undo_free(c);
    canvas_undo_init(c);
}

McpHost::SysTable const& McpHost::sysTable()
{
    static SysTable const table = [] {
        SysTable t;
        t.setSource("sys", mcp::VerbSource::Path); // the selector is the whole key
#define MCP_SYS(selector, handler) t.add("sys", selector, &McpHost::handler);
#include "Mcp/McpSysVerbs.inc"
        return t;
    }();
    return table;
}

bool McpHost::handleSysMessage(SmallString const& selector, SmallArray<pd::Atom> const& list)
{
    auto const* handler = sysTable().find("sys", selector.toString().toStdString());
    if (handler == nullptr)
        return false;

    (this->**handler)(selector, mcp::SysList(list));
    return true;
}

bool McpHost::callSys(SmallString const& selector, mcp::SysList const& list)
{
    auto const* handler = sysTable().find("sys", selector.toString().toStdString());
    if (handler == nullptr)
        return false;
    (this->**handler)(selector, list);
    return true;
}

void McpHost::sysSelectionGate(SmallString const& selector, mcp::SysList const& list)
{
    // P3.4 S3: reads the editors' selections, so it runs on the message thread (Pd-only selector).
    if (!juce::MessageManager::getInstance()->isThisTheMessageThread()) {
        postMessage([this, selector, list] { sysSelectionGate(selector, list); });
        return;
    }
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
                    return;
                }
            }
        }
    }
}

void McpHost::sysMove(SmallString const& selector, mcp::SysList const& list)
{
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

    }
}

void McpHost::sysClearSelection(SmallString const& selector, mcp::SysList const& list)
{
    // P3.4 S3: editors are message-thread objects (from the command worker this was 57 of a sweep's 61
    // JUCE assertions). No reply follows, so a hop keeps the protocol as it was.
    if (!juce::MessageManager::getInstance()->isThisTheMessageThread()) {
        postMessage([this, selector, list] { sysClearSelection(selector, list); });
        return;
    }
    for (auto* editor : processor.getEditors()) {
        if (auto* cnv = editor->getCurrentCanvas()) {
            cnv->deselectAll();
        }
    }
}

void McpHost::sysMoveBatch(SmallString const& selector, mcp::SysList const& list)
{
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


        mcp::SysList atoms; // P3.4 S7: plain values, no Pd symbols
        atoms.add((float)movedCount);
        sendMCPReply(String("/pd/move_batch/reply/" + correlation_id), atoms);
    }
}

void McpHost::sysClearIds(SmallString const& selector, mcp::SysList const& list)
{
    if (list.size() >= 2) {
        auto canvas_symbol = list[0].toString();
        auto correlation_id = list[1].toString();

        t_canvas* canvas = getCanvasBySymbol(canvas_symbol);
        auto canvasStr = (canvas ? MCPBridge::canonicalCanvasKey(canvas) : canvas_symbol).toStdString();
        // Drops the canvas's ids and their serials.
        if (identity.eraseCanvas(canvasStr))
            identity.bumpVersion();

        mcp::SysList atoms; // P3.4 S7: plain values, no Pd symbols
        atoms.add(1.0f);
        sendMCPReply(String("/pd/mcp_clear_ids/reply/" + correlation_id), atoms);
    }
}

void McpHost::sysClearUndo(SmallString const& selector, mcp::SysList const& list)
{
    // Called from /pd/clear_undo, resetCanvasUndo and encapsulate_gop via
    // receiveSysMessage, synchronously on the caller's thread (command worker,
    // message thread or a Pd job). canvas_undo_free calls canvas_suspend_dsp,
    // which needs the Pd lock: taken here (recursive when the caller holds it).
    // While an MCP undo/redo replays its batch, that batch's own clear must leave the MCP stacks alone:
    // the undo has just moved the transaction to the redo stack (and back), and wiping it here made every
    // redo after an undo of a batch a no-op (P3.4 S2, found once L12 was fixed).
    bool const replaying = session.isExecutingMcpUndoRedo.load();
    processor.setThis();
    sys_lock();
    auto wipeCanvasUndo = [](t_canvas* c) { resetNativeUndo(c); };

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
            if (!replaying)
                clearMcpTransactions(cnv);
        }
    }

    // Always also wipe undo on top-level root patch and all open canvas roots
    // so subpatch mutations never leave armed undo actions on the parent canvas.
    for (t_canvas* root = pd_this->pd_canvaslist; root; root = root->gl_next) {
        wipeCanvasUndo(root);
        if (!replaying)
            clearMcpTransactions(root);
    }
    sys_unlock();
}

void McpHost::sysRegisterId(SmallString const& selector, mcp::SysList const& list)
{
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
                identity.canvasMap(canonStr)[object_id.toStdString()] = targetObj;
                identity.assignSerial(targetObj);
                identity.bumpVersion();
                success = true;
            }
        }
        sys_unlock();

        mcp::SysList atoms; // P3.4 S7: plain values, no Pd symbols
        atoms.add(success ? 1.0f : 0.0f);
        sendMCPReply(String("/pd/mcp_register_id/reply/" + correlation_id), atoms);
    }
}

void McpHost::sysRenameId(SmallString const& selector, mcp::SysList const& list)
{
    if (list.size() >= 4) {
        auto canvas_symbol = list[0].toString();
        auto correlation_id = list[1].toString();
        auto old_id = list[2].toString();
        auto new_id = list[3].toString();

        sys_lock();
        bool success = false;
        t_canvas* canvas = getCanvasBySymbol(canvas_symbol);
        auto canvasStr = (canvas ? MCPBridge::canonicalCanvasKey(canvas) : canvas_symbol).toStdString();
        auto& map = identity.canvasMap(canvasStr);
        auto const renamed = mcp::identity::renameStableId(map, old_id.toStdString(), new_id.toStdString());
        if (renamed == mcp::identity::RenameResult::Applied)
            identity.bumpVersion();
        success = renamed != mcp::identity::RenameResult::Missing;
        sys_unlock();

        mcp::SysList atoms; // P3.4 S7: plain values, no Pd symbols
        atoms.add(success ? 1.0f : 0.0f);
        sendMCPReply(String("/pd/mcp_rename_id/reply/" + correlation_id), atoms);
    }
}

void McpHost::sysGetMappings(SmallString const& selector, mcp::SysList const& list)
{
    if (list.size() >= 2) {
        auto canvas_symbol = list[0].toString();
        auto correlation_id = list[1].toString();

        sys_lock();
        t_canvas* canvas = getCanvasBySymbol(canvas_symbol);
        mcp::SysList atoms; // P3.4 S7: plain values, no Pd symbols
        if (canvas) {
            auto canonStr = MCPBridge::canonicalCanvasKey(canvas).toStdString();
            auto& canvasMap = identity.canvasMap(canonStr);

            // For each ID in the map, find its current index in gl_list
            for (auto it = canvasMap.begin(); it != canvasMap.end();) {
                t_gobj* targetObj = it->second;
                int index = getObjectIndex(canvas, targetObj);
                if (index != -1) {
                    // Object still exists on the canvas, add to atoms
                    atoms.add(juce::String::fromUTF8(it->first.c_str()));
                    atoms.add((float)index);
                    ++it;
                } else {
                    // Object was deleted/freed, erase from stable map
                    it = canvasMap.erase(it);
                }
            }
        }
        sys_unlock();

        sendMCPReply(String("/pd/mcp_get_mappings/reply/" + correlation_id), atoms);
    }
}

void McpHost::sysCensus(SmallString const& selector, mcp::SysList const& list)
{
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
            if (auto const* canvasIds = identity.findCanvas(canonStr)) {
                for (auto const& [id, ptr] : *canvasIds) {
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

                    auto& canvasIdMap = identity.canvasMap(canonStr);
                    auto existing = canvasIdMap.find(minted.toStdString());
                    if (existing == canvasIdMap.end()) {
                        canvasIdMap[minted.toStdString()] = y_obj;
                        identity.ensureSerial(y_obj);
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
                identity.bumpVersion();
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

        mcp::SysList atoms; // P3.4 S7: plain values, no Pd symbols
        atoms.add(jsonString); // P3.4 S7: was a permanent Pd symbol per census
        sendMCPReply(String("/pd/census/reply/" + correlation_id), atoms);
    }
}

void McpHost::sysMoveBatchId(SmallString const& selector, mcp::SysList const& list)
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
                resetNativeUndo(canvas);
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

        mcp::SysList atoms; // P3.4 S7: plain values, no Pd symbols
        atoms.add((float)movedCount);
        sendMCPReply(String("/pd/mcp_move_batch_id/reply/" + correlation_id), atoms);
        enqueuePd([this] { synchroniseCanvases(); });
    }
}
