// The OSC verb table of the MCP bridge (Plan 3 P3.3). One row per verb; this
// file is the single list. It is included, with the macros below defined, by
//   MCPBridge.h           declares each handler as a private member function
//   MCPBridgeRegistry.cpp fills the command table
//   Tests/mcp/registry_test.cpp  checks the rows without JUCE
// and parsed as text by the TS wire-inventory extractor
// (scripts/protocol/extract-wire-inventory.ts), so keep one row per line and
// write the verb as a string literal.
//
//   MCP_SOURCE(domain, Path|PathOrArg0|Arg0)  where the verb of a message comes from
//   MCP_VERB(domain, "verb", handler)         one handler per verb
//   MCP_ALIAS(domain, "verb", handler)        another name for an existing handler
//   MCP_DOMAIN(domain, handler)               a verb-agnostic domain (param, chat)
//
// A handler is `void handler(const juce::String& action, const juce::OSCMessage& msg)`
// and keeps the body it had inside handle<Domain>Domain; `action` is the verb
// the message was dispatched under. An includer defines only the macros it
// needs; the rest expand to nothing. Rows move in here one domain at a time
// (P3.3 S2-S7); until a verb has a row, handle<Domain>Domain still serves it.

#ifndef MCP_SOURCE
#define MCP_SOURCE(domain, source)
#endif
#ifndef MCP_VERB
#define MCP_VERB(domain, verb, handler)
#endif
#ifndef MCP_ALIAS
#define MCP_ALIAS(domain, verb, handler)
#endif
#ifndef MCP_DOMAIN
#define MCP_DOMAIN(domain, handler)
#endif

// ── Domains: how oscMessageReceived finds the verb ──
MCP_SOURCE(pd, Path)
MCP_SOURCE(param, Path)
MCP_SOURCE(trigger, Path)
MCP_SOURCE(telemetry, Arg0)
MCP_SOURCE(array, PathOrArg0)
MCP_SOURCE(bridge, PathOrArg0)
MCP_SOURCE(morph, PathOrArg0)
MCP_SOURCE(meter, PathOrArg0)
MCP_SOURCE(transport, PathOrArg0)
MCP_SOURCE(seq, PathOrArg0)
MCP_SOURCE(chat, PathOrArg0)

// ── Verbs: empty until S2 ──

#undef MCP_SOURCE
#undef MCP_VERB
#undef MCP_ALIAS
#undef MCP_DOMAIN
