/*
 // Pd-side field reads of GUI externals (Plan 3 P3.2 S6). These read the structs of the
 // generated AllGuis.h (t_fake_knob, ...), which the bridge may no longer include;
 // moved verbatim from Source/Pd/MCPBridge.cpp. No JUCE component is touched here.
 */
#pragma once

#include <juce_core/juce_core.h>

extern "C" {
#include <m_pd.h>
}

// Live `props` for a GUI object (shared by obj_get / obj_get_batch).
juce::DynamicObject* mcpBuildGuiProps(const juce::String& className, t_object* obj);

// A named, float-settable GUI parameter.
struct McpGuiParam { juce::String name; double min = 0, max = 1, value = 0; };

bool mcpGetGuiParam(const juce::String& className, t_object* obj, McpGuiParam& out);

// Screenshot label name of an else/knob: its receive name, else its send name
// (empty when neither is set). Was inline in /pd/screenshot_canvas.
juce::String mcpKnobLabelName(t_gobj* y);
