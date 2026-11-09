/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Instance i (0..clone_get_n-1) of a [clone], whatever its first voice (-s). Pd's clone_get_instance(x, n) takes a
// VOICE number: it subtracts the start voice and clamps, so walking 0..n-1 of [clone -s 1 abs 3] read instance 0
// twice and never the last one (delta review of proof3 H5). pure-data patch 0006 adds clone_get_startvoice.
// Caller holds the Pd lock.
#pragma once

#include <m_pd.h>

extern "C" {
extern t_class* clone_class;
extern int clone_get_n(t_gobj*);
extern t_glist* clone_get_instance(t_gobj*, int);
extern int clone_get_startvoice(t_gobj*);
}

inline t_glist* mcpCloneInstance(t_gobj* g, int i)
{
    return clone_get_instance(g, clone_get_startvoice(g) + i);
}
