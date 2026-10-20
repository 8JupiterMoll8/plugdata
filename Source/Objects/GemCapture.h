/*
 // PRD 1.4: lightweight declaration for native GEM render capture.
 // Defined in ImplementationBase.cpp (which includes the heavy Gem.h).
 // Kept out of MCPBridge.cpp so the bridge does not pull in Gem/OpenGL headers.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#if ENABLE_GEM
// Capture the live GEM render window as an image. MUST run on the JUCE message
// thread. Returns an invalid image when no GEM window exists.
juce::Image captureGemWindowImage();
#endif
