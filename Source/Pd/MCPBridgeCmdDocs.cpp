/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Bridge commands: object docs, library info, viewport (Plan 3 P3.3): handlers moved unchanged out of MCPBridge::handlePdDomain.

#include "MCPBridgeCmdCommon.h"

/** Build the full C++ ValueTree doc JSON for one object. Single source of
 *  truth shared by /pd/doc_get and the paginated /pd/doc_dump. */
static juce::String buildObjectDocJson(pd::Library* library, const juce::String& objectNameIn, const juce::String& creationArgs)
{
    auto const& objectName = objectNameIn;
    if (library == nullptr)
        return "{}";

    // Upstream's docs database is a typed table now; an unknown name returns an
    // empty table (no title).
    auto const* doc = &library->getObjectInfo(objectName);
    if (doc->title.isEmpty()) {
        doc = objectName.contains("/")
            ? &library->getObjectInfo(objectName.fromLastOccurrenceOf("/", false, false))
            : &library->getObjectInfo("ELSE/" + objectName);
    }
    if (doc->title.isEmpty())
        return "{}";

    auto* root = new juce::DynamicObject();
    root->setProperty("name", doc->title.isNotEmpty() ? doc->title : objectName);
    root->setProperty("description", doc->description);

    // Categories & origin
    juce::Array<juce::var> cats;
    juce::String origin = doc->origin;
    for (auto const& c : doc->categories) {
        cats.add(c);
        if (origin.isEmpty() && pd::Library::objectOrigins.contains(c))
            origin = c;
    }
    root->setProperty("categories", cats);
    root->setProperty("origin", origin);

    // Inlets & outlets, with dynamic arity when creation args were passed
    auto const numIn = static_cast<int>(doc->inlets.size());
    auto const numOut = static_cast<int>(doc->outlets.size());
    juce::Array<juce::var> inletsArr;
    juce::Array<juce::var> outletsArr;

    auto addIolet = [&objectName](juce::Array<juce::var>& arr, int index, juce::String const& tooltip, bool variable) {
        auto* io = new juce::DynamicObject();
        io->setProperty("index", index);
        io->setProperty("description", tooltip);
        io->setProperty("signalRate", tooltip.containsIgnoreCase("signal") || objectName.endsWith("~"));
        io->setProperty("variable", variable);
        arr.add(juce::var(io));
    };

    StackArray<juce::StringArray, 2> parsed;
    if (creationArgs.isNotEmpty() || numIn > 0 || numOut > 0) {
        auto const fullObjText = objectName + (creationArgs.isNotEmpty() ? " " + creationArgs : "");
        parsed = pd::Library::parseIoletTooltips(doc->inlets, doc->outlets, fullObjText, numIn, numOut);
    }

    if (parsed[0].size() > 0 || parsed[1].size() > 0) {
        for (int i = 0; i < parsed[0].size(); ++i)
            addIolet(inletsArr, i, parsed[0][i], false);
        for (int i = 0; i < parsed[1].size(); ++i)
            addIolet(outletsArr, i, parsed[1][i], false);
    } else {
        for (int i = 0; i < numIn; ++i)
            addIolet(inletsArr, i, doc->inlets[i].tooltip, doc->inlets[i].repeating);
        for (int i = 0; i < numOut; ++i)
            addIolet(outletsArr, i, doc->outlets[i].tooltip, doc->outlets[i].repeating);
    }

    root->setProperty("numInlets", inletsArr.size());
    root->setProperty("numOutlets", outletsArr.size());
    root->setProperty("inlets", inletsArr);
    root->setProperty("outlets", outletsArr);

    // Arguments
    juce::Array<juce::var> argsArr;
    int argIdx = 0;
    for (auto const& arg : doc->arguments) {
        auto* argObj = new juce::DynamicObject();
        argObj->setProperty("index", argIdx++);
        argObj->setProperty("type", arg.type);
        argObj->setProperty("description", arg.description);
        argsArr.add(juce::var(argObj));
    }
    root->setProperty("arguments", argsArr);

    // Methods
    juce::Array<juce::var> methodsArr;
    for (auto const& method : doc->methods) {
        auto* mObj = new juce::DynamicObject();
        mObj->setProperty("signature", method.type);
        mObj->setProperty("description", method.description);
        methodsArr.add(juce::var(mObj));
    }
    root->setProperty("methods", methodsArr);

    // Flags
    juce::Array<juce::var> flagsArr;
    for (auto const& flag : doc->flags) {
        auto* fObj = new juce::DynamicObject();
        auto fName = flag.type.trim();
        if (!fName.startsWith("-"))
            fName = "-" + fName;
        fObj->setProperty("name", fName);
        fObj->setProperty("description", flag.description);
        flagsArr.add(juce::var(fObj));
    }
    root->setProperty("flags", flagsArr);

    return juce::JSON::toString(juce::var(root));
}

void MCPBridge::pdDocGet(const juce::String& action, const juce::OSCMessage& msg)
{
    // /pd/doc_get <objectName> <corrId> [creationArgs]
    // Reply: JSON on /pd/doc_get/reply/<corrId>
    if (msg.size() >= 1 && processor) {
        auto objectName = getArgString(msg[0]);
        auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : juce::String("0");
        auto creationArgs = msg.size() > 2 ? getArgString(msg[2]) : juce::String();
        if (!processor->objectLibrary) {
            sendReply("/pd/doc_get/reply/" + correlationId, "{}");
            return;
        }
        processor->objectLibrary->ensureDatabaseInitialised();
        sendReply("/pd/doc_get/reply/" + correlationId,
                  buildObjectDocJson(processor->objectLibrary.get(), objectName, creationArgs));
    }
    return;
}

void MCPBridge::pdDocSearch(const juce::String& action, const juce::OSCMessage& msg)
{
    // /pd/doc_search <query> <corrId> [limit]
    // Live name oracle: the same fuzzy engine behind the Documentation
    // Browser. Reply JSON { results: ["name", ...] }.
    if (msg.size() >= 1 && processor) {
        auto query = getArgString(msg[0]);
        auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : juce::String("0");
        int limit = msg.size() > 2 ? static_cast<int>(getArgFloat(msg[2])) : 10;
        if (limit < 1) limit = 10;
        if (limit > 50) limit = 50;
        juce::Array<juce::var> results;
        if (processor->objectLibrary) {
            processor->objectLibrary->ensureDatabaseInitialised();
            auto names = processor->objectLibrary->searchObjectDocumentation(query);
            for (int i = 0; i < names.size() && i < limit; ++i) results.add(names[i]);
        }
        auto* root = new juce::DynamicObject();
        root->setProperty("results", results);
        sendReply("/pd/doc_search/reply/" + correlationId, juce::JSON::toString(juce::var(root)));
    }
    return;
}

void MCPBridge::pdDocDump(const juce::String& action, const juce::OSCMessage& msg)
{
    // /pd/doc_dump <corrId> <offset> <limit>
    // Paginated full-library doc dump. Reply JSON { total, offset, items }.
    if (msg.size() >= 1 && processor) {
        auto correlationId = getArgString(msg[0]);
        int offset = msg.size() > 1 ? static_cast<int>(getArgFloat(msg[1])) : 0;
        int limit = msg.size() > 2 ? static_cast<int>(getArgFloat(msg[2])) : 20;
        if (offset < 0) offset = 0;
        if (limit < 1) limit = 1;
        if (limit > 25) limit = 25;
        juce::Array<juce::var> items;
        int total = 0;
        if (processor->objectLibrary) {
            processor->objectLibrary->ensureDatabaseInitialised();
            auto names = processor->objectLibrary->getAllObjects();
            total = names.size();
            for (int i = offset; i < names.size() && i < offset + limit; ++i) {
                auto json = buildObjectDocJson(processor->objectLibrary.get(), names[i], "");
                if (json == "{}") continue;
                items.add(juce::JSON::parse(json));
            }
        }
        auto* root = new juce::DynamicObject();
        root->setProperty("total", total);
        root->setProperty("offset", offset);
        root->setProperty("items", items);
        sendReply("/pd/doc_dump/reply/" + correlationId, juce::JSON::toString(juce::var(root)));
    }
    return;
}

void MCPBridge::pdLibraryInfo(const juce::String& action, const juce::OSCMessage& msg)
{
    // /pd/library_info <corrId>
    // Live inventory count for pd-docs staleness checks.
    if (msg.size() >= 1 && processor) {
        auto correlationId = getArgString(msg[0]);
        int count = 0;
        if (processor->objectLibrary) {
            processor->objectLibrary->ensureDatabaseInitialised();
            count = processor->objectLibrary->getAllObjects().size();
        }
        auto* root = new juce::DynamicObject();
        root->setProperty("objects", count);
        sendReply("/pd/library_info/reply/" + correlationId, juce::JSON::toString(juce::var(root)));
    }
    return;
}

void MCPBridge::pdUiOpenReference(const juce::String& action, const juce::OSCMessage& msg)
{
    // /pd/ui_open_reference <objectName> <corrId>
    if (msg.size() >= 1 && processor) {
        auto objectName = getArgString(msg[0]);
        auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : juce::String("0");

        juce::MessageManager::callAsync([proc = processor, objectName, correlationId, this] {
            if (proc->getMcp()->gui().showObjectReference(objectName)) {
                sendReply("/pd/ui_open_reference/reply/" + correlationId, 1.0f);
            } else {
                sendReply("/pd/ui_open_reference/reply/" + correlationId, 0.0f);
            }
        });
    }
    return;
}

void MCPBridge::pdUiOpenHelp(const juce::String& action, const juce::OSCMessage& msg)
{
    // /pd/ui_open_help <objectName> <corrId>
    if (msg.size() >= 1 && processor) {
        auto objectName = getArgString(msg[0]);
        auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : juce::String("0");

        juce::MessageManager::callAsync([proc = processor, objectName, correlationId, this] {
            auto helpFile = pd::Library::findHelpfile(objectName);
            if (helpFile.existsAsFile()) {
                if (proc->getMcp()->gui().openHelpPatch(helpFile)) {
                    sendReply("/pd/ui_open_help/reply/" + correlationId, 1.0f);
                    return;
                }
            }
            sendReply("/pd/ui_open_help/reply/" + correlationId, 0.0f);
        });
    }
    return;
}

void MCPBridge::pdViewport(const juce::String& action, const juce::OSCMessage& msg)
{
    // /pd/viewport <canvas> <corrId>
    auto canvasName = msg.size() > 0 ? getArgString(msg[0]) : juce::String("main");
    auto correlationId = msg.size() > 1 ? getArgString(msg[1]) : juce::String("0");

    juce::MessageManager::callAsync([proc = processor, canvasName, correlationId, this] {
        if (auto info = proc->getMcp()->gui().focusedViewport()) {
            juce::OSCMessage reply(juce::OSCAddressPattern("/pd/viewport/reply/" + correlationId));
            reply.addArgument(info->zoom);
            reply.addArgument(info->panX);
            reply.addArgument(info->panY);
            reply.addArgument(info->width);
            reply.addArgument(info->height);
            reply.addArgument(info->renderScale);
            sender.send(reply);
            return;
        }
        juce::OSCMessage reply(juce::OSCAddressPattern("/pd/viewport/reply/" + correlationId));
        reply.addArgument(1.0f);
        reply.addArgument(0.0f);
        reply.addArgument(0.0f);
        reply.addArgument(1920.0f);
        reply.addArgument(1080.0f);
        reply.addArgument(1.0f);
        sender.send(reply);
    });
    return;
}
