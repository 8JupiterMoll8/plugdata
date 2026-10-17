/*
 // The MCP layer of one Canvas (Plan 3 P3.2 S7): the AI overlay painting that used to sit
 // inline in Canvas::performRender (regions, reference images, voice/MIDI takes, notes with
 // pivot chips, note editor and selection pill frames, approval chips, sketch ink, image slot,
 // ghosts, studio ink frame, HUD), the per-object AI state mark, overlay targeting and the
 // AI overlay toggles. Each Canvas owns one (Canvas::getMcpLayer()); the paint calls sit
 // exactly where their blocks were, so the paint order is unchanged. Moved verbatim; implicit
 // Canvas members became `canvas.` accesses.
 //
 // All methods run on the message thread (paint and settings callbacks).
 */
#pragma once

#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>
struct NVGcontext;

class Canvas;
class Object;
class Connection;

class CanvasMcpLayer {
public:
    explicit CanvasMcpLayer(Canvas& canvas);

    // Paint hooks, in paint order (Canvas::performRender).
    // After the grid, before the border: regions, reference images, voice and MIDI takes.
    void paintUnderObjects(NVGcontext* nvg, juce::Rectangle<int> invalidRegion);
    // After objects and connections: notes, note editor, selection pill, approval chips,
    // sketch ink, image slot, ghosts.
    void paintOverObjects(NVGcontext* nvg);
    // Last item in canvas coordinates: the studio ink frame around the viewport.
    void paintViewportFrame(NVGcontext* nvg, float zoom);
    // After the scrollbars, in screen coordinates: the HUD.
    void paintScreenOverlay(NVGcontext* nvg);

    // Object::performRender: draws the AI state mark of `object` in `b`. Returns true when
    // the AI state overlay is on, which ends the object's overlay chain (the index is not drawn).
    bool decorateObject(NVGcontext* nvg, Object& object, juce::Rectangle<int> b);

    // Spotlight targeting: true for everything when no targets are set.
    bool hasOverlayTargets() const;
    bool isTargeted(Object const* obj) const;
    bool isTargeted(Connection const* c) const;

    // Writes the overlay bit mask into the settings (lock or edit set) and re-reads them.
    void setOverlayMask(int mask);
    // Canvas::updateOverlays: takes the AI bits of the overlay state.
    void updateOverlays(int overlayState);

    bool shouldShowAIState() const;
    bool shouldShowAIRegions() const;
    bool shouldShowAIAnnotations() const;
    bool shouldShowAIGhosts() const;
    bool shouldShowAIHud() const;
    bool shouldShowAISketch() const;
    bool shouldShowAIReferences() const;

    // Pivot chips of an AI note (shared with the note click/hover code in Canvas.cpp).
    struct AnnotationPivotChip {
        juce::String label;
        juce::String prompt;
    };
    static std::vector<AnnotationPivotChip> getAnnotationPivotChips(juce::String const& text, juce::String const& kind);
    static float getChipWidth(juce::String const& label);

private:
    void renderSketchOverlay(NVGcontext* nvg);
    void renderReferenceImages(NVGcontext* nvg, juce::Rectangle<int> invalidRegion);
    void renderVoiceTakes(NVGcontext* nvg, juce::Rectangle<int> invalidRegion);
    void renderMidiTakes(NVGcontext* nvg, juce::Rectangle<int> invalidRegion);

    Canvas& canvas;

    bool showAiState : 1 = false;
    bool showAiRegions : 1 = true;
    bool showAiAnnotations : 1 = true;
    bool showAiGhosts : 1 = true;
    bool showAiHud : 1 = true;
    bool showAiSketch : 1 = true;
    bool showAiReferences : 1 = true;
};
