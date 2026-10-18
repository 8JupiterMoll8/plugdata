/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Bridge commands: stable object ids (snapshot, version, register, rename, mappings) (Plan 3 P3.3): handlers moved unchanged out of MCPBridge::handlePdDomain.

#include "MCPBridgeCmdCommon.h"

void MCPBridge::pdIdentitySnapshot(const juce::String& action, const juce::OSCMessage& msg)
{
    // /pd/identity_snapshot <canvasName> <correlationId>
    // Returns a JSON string with the complete identity state:
    // { "version": N, "canvas": "pd-main", "entries": [
    //   { "id": "fm_carrier", "index": 3, "type": "osc~", "text": "osc~ 440" },
    //   ...
    // ]}
    // Single O(n) walk of gl_list builds a reverse pointer→index map, then
    // iterates the stable map. No per-entry O(n) scan. Total: O(n+m).
    if (msg.size() >= 2 && processor) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        auto correlationId = getArgString(msg[1]);

        sys_lock();
        t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);

        if (!cnv) {
            sys_unlock();
            sendReply("/pd/identity_snapshot/error/" + correlationId, "Canvas not found: " + canvasName);
            return;
        }

        // Step 1: Build pointer→index map with ONE walk of gl_list (O(n))
        std::unordered_map<t_gobj*, int> ptrToIndex;
        int idx = 0;
        for (t_gobj* y = cnv->gl_list; y; y = y->g_next) {
            ptrToIndex[y] = idx++;
        }
        int totalObjects = idx;

        // Step 2: Walk the identity map and validate each entry (O(m))
        auto canonKey = canonicalCanvasKey(cnv);
        auto canvasStr = canonKey.toStdString();
        auto& canvasMap = processor->getMcp()->identity.canvasMap(cnv);
        uint64_t version = processor->getMcp()->identity.version();

        auto* rootObj = new juce::DynamicObject();
        rootObj->setProperty("version", static_cast<juce::int64>(version));
        rootObj->setProperty("canvas", canvasName);
        // P3.5 S6: the canvas's wire key and its own version ("version" stays the global counter).
        rootObj->setProperty("canvasKey", juce::String(processor->getMcp()->identity.keyOf(cnv)));
        rootObj->setProperty("totalObjects", totalObjects);

        juce::Array<juce::var> entries;

        std::vector<std::string> toEvict;
        for (auto& [tempId, ptr] : canvasMap) {
            auto ptrIt = ptrToIndex.find(ptr);
            if (ptrIt == ptrToIndex.end()) {
                // Object no longer on canvas — mark for eviction
                toEvict.push_back(tempId);
                continue;
            }

            // Serial verification (UAF defense)
            if (!processor->getMcp()->identity.valid(cnv, tempId)) { // P3.5 S5: the bound object itself still lives
                toEvict.push_back(tempId);
                continue;
            }

            auto* entry = new juce::DynamicObject();
            entry->setProperty("id", juce::String(tempId));
            entry->setProperty("index", ptrIt->second);

            // Read class name
            juce::String className = juce::String::fromUTF8(class_getname(pd_class(&ptr->g_pd)));
            entry->setProperty("class", className);

            // Read object text (type + args)
            t_object* obj = pd::Interface::checkObject(ptr);
            if (obj) {
                char* text = nullptr;
                int len = 0;
                mcpGetObjectText(obj, &text, &len);
                if (text && len > 0) {
                    entry->setProperty("text", juce::String::fromUTF8(text, len));
                    freebytes(text, len);
                }
                entry->setProperty("inlets", obj_ninlets(obj));
                entry->setProperty("outlets", obj_noutlets(obj));
            }

            entries.add(juce::var(entry));
        }

        // Evict stale entries
        for (auto& staleId : toEvict) {
            auto staleIt = canvasMap.find(staleId);
            if (staleIt != canvasMap.end())
                canvasMap.erase(staleIt); // P3.5 S5: keep the serial: the address may belong to a new, bound object
        }
        if (!toEvict.empty()) {
            processor->getMcp()->identity.bumpVersion(cnv);
            version = processor->getMcp()->identity.version();
            rootObj->setProperty("version", static_cast<juce::int64>(version));
            rootObj->setProperty("evicted", static_cast<int>(toEvict.size()));
        }

        rootObj->setProperty("canvasVersion", static_cast<juce::int64>(processor->getMcp()->identity.canvasVersion(cnv)));
        rootObj->setProperty("entryCount", entries.size());
        rootObj->setProperty("entries", entries);

        juce::String jsonString = juce::JSON::toString(juce::var(rootObj), true);
        sys_unlock();

        juce::OSCMessage reply { juce::OSCAddressPattern("/pd/identity_snapshot/reply/" + correlationId) };
        reply.addArgument(jsonString);
        sender.send(reply);
    }
    return;
}

void MCPBridge::pdIdentityVersion(const juce::String& action, const juce::OSCMessage& msg)
{
    // Node.js uses this to decide if a full snapshot fetch is needed (cheap: one walk of the open canvases).
    // P3.5 S6: /pd/identity_version <corrId> [canvas]. It first looks at every open canvas (registerOpenCanvases
    // takes each structure stamp), so a GUI edit since the last look bumps the version now. With a canvas: that
    // canvas's version; without: the global counter (bumped by every canvas change).
    if (msg.size() >= 1 && processor) {
        auto correlationId = getArgString(msg[0]);
        uint64_t version = 0;
        {
            McpHost::PdLockScope lock(*processor);
            processor->getMcp()->registerOpenCanvases();
            if (msg.size() >= 2) {
                auto* cnv = processor->getMcp()->getCanvasBySymbol(normalizeCanvas(getArgString(msg[1])));
                if (!cnv) {
                    sendReply("/pd/identity_version/error/" + correlationId, juce::String("Canvas not found: " + getArgString(msg[1])));
                    return;
                }
                version = processor->getMcp()->identity.canvasVersion(cnv);
            } else {
                version = processor->getMcp()->identity.version();
            }
        }

        juce::OSCMessage reply { juce::OSCAddressPattern("/pd/identity_version/reply/" + correlationId) };
        reply.addArgument(static_cast<int32>(static_cast<int>(version & 0x7FFFFFFF)));
        sender.send(reply);
    }
    return;
}

void MCPBridge::pdClearIds(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 2 && processor) {
        mcp::SysList atoms; // P3.4 S7: plain values, no Pd symbols
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        auto correlationId = getArgString(msg[1]);
        atoms.add(juce::String(canvasName));
        atoms.add(juce::String(correlationId));
        processor->getMcp()->callSys("mcp_clear_ids", atoms);
    }
    return;
}

void MCPBridge::pdRegisterId(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 4 && processor) {
        mcp::SysList atoms; // P3.4 S7: plain values, no Pd symbols
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        auto correlationId = getArgString(msg[1]);
        float index = getArgFloat(msg[2]);
        auto objectId = getArgString(msg[3]);

        atoms.add(juce::String(canvasName));
        atoms.add(juce::String(correlationId));
        atoms.add(index);
        atoms.add(juce::String(objectId));

        processor->getMcp()->callSys("mcp_register_id", atoms);
    }
    return;
}

void MCPBridge::pdRenameId(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 4 && processor) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        auto correlationId = getArgString(msg[1]);
        auto oldId = getArgString(msg[2]).toStdString();
        auto newId = getArgString(msg[3]).toStdString();

        bool success = false;
        bool taken = false;
        {
            McpHost::PdLockScope lock(*processor); // P3.5 S2: was unlocked on the command worker
            t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
            if (cnv) { // P3.5 S3: an unknown canvas renames nothing (it used to rename in a map named after the request)
                auto& map = processor->getMcp()->identity.canvasMap(cnv);
                auto const renamed = mcp::identity::renameStableId(map, oldId, newId);
                if (renamed == mcp::identity::RenameResult::Applied)
                    processor->getMcp()->identity.bumpVersion(cnv);
                taken = renamed == mcp::identity::RenameResult::Taken;
                success = renamed == mcp::identity::RenameResult::Applied || renamed == mcp::identity::RenameResult::NoOp;
            }
        }
        if (taken) { // P3.5 S4 (C5): renaming onto an id that names another object is refused
            sendReply("/pd/rename_id/error/" + correlationId, juce::String("id taken: " + juce::String(newId)));
            return;
        }

        sendReply("/pd/rename_id/reply/" + correlationId, success ? 1.0f : 0.0f);
    }
    return;
}

    // /pd/rename_id_batch <canvas> <count> <old0> <new0> <old1> <new1> ... <corrId>
    // Batch rename with a reply — renames directly in the identity map
    // (inline, synchronous — no receiveSysMessage async queue).
void MCPBridge::pdRenameIdBatch(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 2 && processor) {
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        int count = static_cast<int>(getArgFloat(msg[1]));
        int cursor = 2;
        int renamed = 0;
        bool changed = false;
        std::vector<std::string> takenIds;

        // Inline rename — directly mutates the identity map, no queue. P3.5 S2: under the Pd lock.
        {
            McpHost::PdLockScope lock(*processor);
            t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
            for (int i = 0; i < count && (cursor + 1) < msg.size(); i++) {
                auto oldId = getArgString(msg[cursor++]).toStdString();
                auto newId = getArgString(msg[cursor++]).toStdString();
                if (!cnv) continue; // P3.5 S3: an unknown canvas renames nothing
                auto const result = mcp::identity::renameStableId(processor->getMcp()->identity.canvasMap(cnv), oldId, newId);
                if (result == mcp::identity::RenameResult::Applied || result == mcp::identity::RenameResult::NoOp) renamed++;
                if (result == mcp::identity::RenameResult::Applied) changed = true;
                if (result == mcp::identity::RenameResult::Taken) takenIds.push_back(newId); // P3.5 S4 (C5)
            }
            if (changed)
                processor->getMcp()->identity.bumpVersion(cnv);
        }

        auto correlationId = (cursor < msg.size()) ? getArgString(msg[cursor]) : "0";
        if (takenIds.empty()) {
            sendReply("/pd/rename_id_batch/reply/" + correlationId, static_cast<float>(renamed));
        } else {
            // P3.5 S4 (C5): the count, then each refused target (it names another object).
            juce::OSCMessage reply { juce::OSCAddressPattern("/pd/rename_id_batch/reply/" + correlationId) };
            reply.addFloat32(static_cast<float>(renamed));
            for (auto const& id : takenIds)
                reply.addString(juce::String(id));
            sender.send(reply);
        }
    }
    return;
}

void MCPBridge::pdGetMappings(const juce::String& action, const juce::OSCMessage& msg)
{
    if (msg.size() >= 2 && processor) {
        mcp::SysList atoms; // P3.4 S7: plain values, no Pd symbols
        auto canvasName = normalizeCanvas(getArgString(msg[0]));
        auto correlationId = getArgString(msg[1]);
        atoms.add(juce::String(canvasName));
        atoms.add(juce::String(correlationId));
        processor->getMcp()->callSys("mcp_get_mappings", atoms);
    }
    return;
}
