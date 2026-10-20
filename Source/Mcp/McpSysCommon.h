/*
 // Shared by the mcp_* Pd system message handlers (Plan 3 P3.3 S7): the include block
 // that used to head McpSysCommands.cpp, and the one helper more than one handler uses.
 */
#pragma once

#include <clocale>
#include <memory>
#include <fstream>
#include <unordered_set>

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include "PluginProcessor.h"
#include "Pd/Library.h"
#include "Pd/MCPBridge.h"
#include "Pd/McpIemArgs.h"
#include "Mcp/PdOps.h" // MCP-HOOK(pdOps): garray_getname, removeObjects/renameObject for the mcp_* cases

#include "Utility/Config.h"
#include "Utility/Fonts.h"
#include "Utility/SettingsFile.h"
#include <g_all_guis.h>
#include "AllGuis.h"
extern "C" void knob_get_snd(void* x);
extern "C" void knob_get_rcv(void* x);
#include "Utility/PluginParameter.h"
#include "Utility/OSUtils.h"
#include "Utility/MidiDeviceManager.h"
#include "Utility/Autosave.h"
#include "Utility/Decompress.h"
#include "Standalone/InternalSynth.h"

#include "Utility/Presets.h"
#include "Canvas.h"
#include "PluginMode.h"
#include "PluginEditor.h"
#include "LookAndFeel.h"
#include "Object.h"
#include "Statusbar.h"
#include "Toolbar.h"

#include "Dialogs/Dialogs.h"
#include "Components/ConnectionMessageDisplay.h"
#include "Utility/Recorder.h"

#include "Sidebar/Sidebar.h"

#include "Object.h"

extern "C" {
#include <pd-cyclone/shared/common/file.h>
EXTERN char* pd_version;
}

#include "Mcp/McpHost.h"

// Index of obj in canvas->gl_list, or -1.
inline int getObjectIndex(t_canvas* canvas, t_gobj* obj) {
    int index = 0;
    for (t_gobj* y_obj = canvas->gl_list; y_obj; y_obj = y_obj->g_next) {
        if (y_obj == obj) {
            return index;
        }
        index++;
    }
    return -1;
}
