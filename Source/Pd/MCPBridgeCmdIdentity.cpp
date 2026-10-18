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
        if (!cnv && canvasName == "pd-main") cnv = pd_this->pd_canvaslist;

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
        auto& canvasMap = processor->getMcp()->identity.canvasMap(canvasStr);
        uint64_t version = processor->getMcp()->identity.version();

        auto* rootObj = new juce::DynamicObject();
        rootObj->setProperty("version", static_cast<juce::int64>(version));
        rootObj->setProperty("canvas", canvasName);
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
            if (!processor->getMcp()->identity.hasSerial(ptr)) {
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
            if (staleIt != canvasMap.end()) {
                processor->getMcp()->identity.eraseSerial(staleIt->second);
                canvasMap.erase(staleIt);
            }
        }
        if (!toEvict.empty()) {
            processor->getMcp()->identity.bumpVersion();
            version = processor->getMcp()->identity.version();
            rootObj->setProperty("version", static_cast<juce::int64>(version));
            rootObj->setProperty("evicted", static_cast<int>(toEvict.size()));
        }

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
    // /pd/identity_version <correlationId>
    // Returns the current identity version counter.
    // Ultra-cheap: one atomic read, no locks, no canvas walk.
    // Node.js uses this to decide if a full snapshot fetch is needed.
    if (msg.size() >= 1 && processor) {
        auto correlationId = getArgString(msg[0]);
        uint64_t version = processor->getMcp()->identity.version();

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
        {
            McpHost::PdLockScope lock(*processor); // P3.5 S2: was unlocked on the command worker
            t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
            auto mapKey = (cnv ? canonicalCanvasKey(cnv) : canvasName).toStdString();
            auto& map = processor->getMcp()->identity.canvasMap(mapKey);
            auto const renamed = mcp::identity::renameStableId(map, oldId, newId);
            if (renamed == mcp::identity::RenameResult::Applied)
                processor->getMcp()->identity.bumpVersion();
            success = renamed != mcp::identity::RenameResult::Missing;
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

        // Inline rename — directly mutates the identity map, no queue. P3.5 S2: under the Pd lock.
        {
            McpHost::PdLockScope lock(*processor);
            t_canvas* cnv = processor->getMcp()->getCanvasBySymbol(canvasName);
            auto mapKey = (cnv ? canonicalCanvasKey(cnv) : canvasName).toStdString();
            auto& map = processor->getMcp()->identity.canvasMap(mapKey);
            for (int i = 0; i < count && (cursor + 1) < msg.size(); i++) {
                auto oldId = getArgString(msg[cursor++]).toStdString();
                auto newId = getArgString(msg[cursor++]).toStdString();
                auto const result = mcp::identity::renameStableId(map, oldId, newId);
                if (result != mcp::identity::RenameResult::Missing) renamed++;
                if (result == mcp::identity::RenameResult::Applied) changed = true;
            }
            if (changed)
                processor->getMcp()->identity.bumpVersion();
        }

        auto correlationId = (cursor < msg.size()) ? getArgString(msg[cursor]) : "0";
        sendReply("/pd/rename_id_batch/reply/" + correlationId, static_cast<float>(renamed));
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
