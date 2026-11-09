/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// The fork's read-only Pd accessors (patches/submodules/pure-data/0004): what the bridge reads of Pd's private
// structs goes through these, never through a mirrored layout. Each checks the kind it is asked for and returns 0
// otherwise. Call them under the Pd lock.
#pragma once

#include <m_pd.h>
#include <g_canvas.h>

extern "C" {
// m_obj.c
t_inlet* mcp_inlet_next(t_inlet* x);
t_float* mcp_inlet_floatslot(t_inlet* x);   // floatinlet_new's slot, else 0
t_symbol** mcp_inlet_symslot(t_inlet* x);   // symbolinlet_new's slot, else 0
int mcp_inlet_ispointer(t_inlet* x);        // pointerinlet_new
int mcp_inlet_generic(t_inlet* x, t_symbol** from, t_symbol** to); // inlet_new: its selectors; 0 for other kinds
t_float* mcp_inlet_signalscalar(t_inlet* x); // a generic signal inlet's scalar, else 0
// m_obj.c, pure-data/0005 (truth slice 3): the last control message a wire or an inlet carried. At most
// kMcpLastAtoms atoms are copied into `atoms`; *argc is the full count (more = truncated); *seq is the instance's
// message counter at the store. 0 when nothing was recorded (or for a null pointer). A pointer atom is never followed.
int mcp_outconnect_last(t_outconnect* oc, t_symbol** sel, int* argc, t_atom* atoms, unsigned int* seq);
int mcp_inlet_last(t_inlet* x, t_symbol** sel, int* argc, t_atom* atoms, unsigned int* seq);
unsigned int mcp_last_seq(void); // the instance's counter: read before and after the bridge's own dispatch
// s_inter.c
void set_plugdata_mcp_wire_enabled(int enabled);
int plugdata_debugging_or_activity_enabled(void);
// d_ugen.c
int mcp_canvas_switchedon_chain(t_canvas* x); // every [switch~] up the owners is on
t_float canvas_getsr(t_canvas* x);
int canvas_getsignallength(t_canvas* x);
}

constexpr int kMcpLastAtoms = 4; // MCP_LAST_ATOMS in m_obj.c (pure-data/0005)
