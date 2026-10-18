/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Execution-model audit (Plan 3 P3.4 S0). Off unless PLUGDATA_MCP_AUDIT=1 is
// set when plugdata starts; then the Pd lock is wrapped (setup_lock) so every
// enter/exit is timed per thread role, and each bridge command prints one line
// to stderr:
//
//   MCP_AUDIT begin <address>
//   MCP_AUDIT end <address> us=<run> net=<holds>/<maxHold>/<sumHold> audioWaitMax=<us>
//             audioHoldMax=<us> msg=<holds>/<maxHold> other=<holds>/<maxHold>
//
// "audioWaitMax" is the longest time the audio thread waited for the lock
// while the command ran: the dropout risk. JUCE assertions printed between
// begin and end belong to the command (async ones may land after it).
// With the switch off nothing is installed and every call returns at once.
#pragma once

namespace mcp::audit {

bool enabled();

// Wrap the instance's Pd lock. Call once, with the Pd instance current, before
// audio starts. `criticalSection` is the juce::CriticalSection setup_lock uses.
void installLock(void const* criticalSection);

// Called by the bridge around each dispatched command (the command's thread).
void commandBegin(char const* address);
void commandEnd();

// Called at the top of every audio block: tags the calling thread as audio.
void markAudioThread();

} // namespace mcp::audit
