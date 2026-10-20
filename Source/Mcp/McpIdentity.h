/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/
#pragma once

// The identity service as plugdata uses it (Plan 3 P3.5): the JUCE-free core over Pd object pointers. One
// instance lives in McpHost (`identity`); nothing else holds tempId state. McpHost installs the hooks: the
// canvas base key (identityBaseKey) and a pd::WeakReference liveness check per canvas record.
#include "McpIdentityCore.h"

struct _gobj;
struct _glist;

namespace mcp {
using IdentityService = identity::IdentityCore<struct _gobj*, struct _glist*>;
}
