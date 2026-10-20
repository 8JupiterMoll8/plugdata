/*
 // Native GEM window capture for /pd/gem_capture (Plan 3 P3.2 S8: moved verbatim
 // out of Source/Objects/ImplementationBase.cpp; declared in Objects/GemCapture.h).
 */
#include <juce_gui_basics/juce_gui_basics.h>
#include <typeinfo>
#include "Utility/Config.h"
#include "Objects/GemCapture.h"

#if ENABLE_GEM
// PRD 1.4: native GEM render capture. Upstream moved GEM's JUCE window into
// the Gem library (gemjucewindow::Window, a private top-level Component) and
// dropped plugdata's per-instance window map, so find it among the desktop
// components by its dynamic type. Mirror screenshot_canvas: grab the native
// window. MUST be called on the JUCE message thread (same constraint as
// createSnapshotOfNativeWindow). Returns an invalid image when no GEM window
// is open.
juce::Image captureGemWindowImage()
{
    auto& desktop = juce::Desktop::getInstance();
    for (int i = 0; i < desktop.getNumComponents(); ++i) {
        auto* win = desktop.getComponent(i);
        if (!win || !win->isVisible())
            continue;
        if (!juce::String(typeid(*win).name()).contains("gemjucewindow"))
            continue;
        if (auto* peer = win->getPeer())
            return juce::createSnapshotOfNativeWindow(peer->getNativeHandle());
    }

    return {};
}
#endif
