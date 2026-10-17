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

#include <optional>
#include <unordered_map>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>
#include "Utility/Config.h"
#include "Utility/NVGUtils.h"
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

    // ── Moved from Canvas.h (S8): AI note input, tools, studio mode, selection pill,
    //    reference images, takes, approvals. Members keep their names.
    // Input and lifecycle hooks (Source/Mcp/CanvasMcpInput.cpp), each called at the position
    // its block had in Canvas.cpp. A `true` return means the event is consumed.
    bool mouseDownEarly(MouseEvent const& e);   // before the left/right split
    bool mouseDownLeft(MouseEvent const& e);    // left click, before the empty-glass branch
    bool mouseDownOnGlass(MouseEvent const& e); // left click on the empty glass
    bool mouseDownRight(MouseEvent const& e);   // right click, before the context menu
    bool mouseDrag(MouseEvent const& e);
    // Object::mouseDown/mouseUp/mouseDrag, first thing: notes and the pen win over objects.
    bool objectMouseDown(MouseEvent const& e);
    bool objectMouseUp(MouseEvent const& e, bool wasLockedOnMouseDown);
    bool objectMouseDrag(MouseEvent const& e);
    // Dialogs::showCanvasRightClickMenu: the "Ask AI" entry (id askAIMenuId) and its result.
    static constexpr int askAIMenuId = 0x4d41; // far from the dialog's own ids (1..~230)
    void addContextMenuItems(PopupMenu& popupMenu, bool hasSelection);
    bool handleContextMenuResult(int result);
    bool mouseUp(MouseEvent const& e);
    void mouseMove(MouseEvent const& e); // Source/Mcp/CanvasMcpNotes.cpp
    void mouseExit(MouseEvent const& e);
    // Before the current-canvas check: a value means "return it from keyPressed".
    std::optional<bool> keyPressedEarly(KeyPress const& key);
    bool keyPressedForCanvas(KeyPress const& key);
    void objectCreatedByDoubleClick();
    void canvasDestroyed();
    void canvasSynchronised();
    // Canvas::undo/redo: an MCP transaction first, then the native stack. False: nothing done.
    bool undo();
    bool redo();
    bool canUndo(); // an MCP transaction or the native stack (also the editor's undo button state)
    bool canRedo();

    // PRD overlay: handle a click that may land on an AI note (x = dismiss, body =
    // open the editor). Returns true if a note consumed it. Called by the canvas AND
    // by objects (so notes overlaying objects stay clickable).
    bool handleNoteClick(Point<int> canvasPt);
    bool isPointOverNote(Point<int> canvasPt) const;
    Object* findObjectByStableId(juce::String const& targetId) const;
    bool getAnnotationLiveBounds(juce::String const& targetId, juce::StringArray const& targetIds, float fallbackX, float fallbackY, float fallbackLx, float fallbackLy, bool fallbackHasLeader, float& outAx, float& outAy, float& outLx, float& outLy, bool& outHasLeader, juce::Rectangle<int>* outGroupBounds = nullptr) const;
    bool getAnnotationLiveBounds(juce::String const& targetId, float fallbackX, float fallbackY, float fallbackLx, float fallbackLy, bool fallbackHasLeader, float& outAx, float& outAy, float& outLx, float& outLy, bool& outHasLeader) const;
    juce::Rectangle<float> getAnnotationScreenBounds(size_t annIndex) const;
    void setSketchToolActive(bool active);
    bool isSketchToolActive() const;
    bool isSketchingActive() const;
    // Phase 1B: combined bounding box of all persistent ink strokes, in Canvas
    // component coordinates (empty rect when no strokes exist).
    juce::Rectangle<float> getSketchBoundsInCanvas() const;
    // All completed strokes serialized as [[{x,y,t},...],...] in PD patch coords.
    juce::String buildSketchJson() const;

    // PRD overlay: inline editor to reply to an AI note (double-click a note tag).
    std::unique_ptr<juce::TextEditor> mcpNoteEditor;
    NVGImage mcpNoteRenderer; // draws the editor into the GPU frame (live typing)
    int mcpNoteEditIndex = -1;
    std::uint32_t mcpNoteOpenedAt = 0; // grace period so it can't self-close on open
    int mcpNoteHover = -1;             // hovered note index (-1 = none)
    bool mcpNoteHoverClose = false;    // hovering the dismiss "x"
    int mcpNoteHoverChip = -1;         // hovered pivot chip index (-1 = none)
    bool mcpNoteHoverSidebar = false;  // hovering the sidebar link icon "↗"
    bool mcpNoteHoverReply = false;    // hovering the inline reply button "💬"

    // PRD Copilot: floating selection prompt pill (the "sculptor's chisel").
    // A glass prompt field that blooms under the selection; Enter sends it via
    // bridge->sendPrompt() and the MCP server resolves the selection. It never
    // grabs keyboard focus until the artist clicks it, so canvas hotkeys
    // (o/m/c/b) keep working while the pill is visible.
    std::unique_ptr<juce::TextEditor> mcpSelectionPill;
    NVGImage mcpSelectionPillRenderer; // draws the prompt field into the GPU frame
    // After the artist sends/dismisses, briefly suppress re-showing so an
    // immediate sidebar refresh can't re-pop the pill. Time-based, NOT keyed
    // to the selection — re-selecting the same object must show it again.
    std::uint32_t mcpSelectionPillHideUntil = 0;
    bool mcpPillRequested = false;
    void updateSelectionPill();
    void hideSelectionPill();
    void dismissSelectionPill();
    // Enter = live (spawn), Shift+Enter = queue for chat. TextEditor only fires
    // onReturnKey for a PLAIN Return (Shift+Return fails its key comparison), so
    // a KeyListener catches the Shift variant before the editor swallows it.
    void submitSelectionPill(bool queueForChat);
    struct SelectionPillKeyListener : public juce::KeyListener {
        explicit SelectionPillKeyListener(CanvasMcpLayer* c) : canvas(c) {}
        bool keyPressed(const juce::KeyPress& key, juce::Component* origin) override;
        CanvasMcpLayer* canvas;
    };
    std::unique_ptr<SelectionPillKeyListener> mcpSelectionPillKeys;
    // Ctrl+V catcher: fires before TextEditors can swallow the key, so the paste
    // slot works even while a tape-note editor has the caret.
    struct SlotPasteKeyListener : public juce::KeyListener {
        explicit SlotPasteKeyListener(CanvasMcpLayer* c) : canvas(c) {}
        bool keyPressed(const juce::KeyPress& key, juce::Component* origin) override;
        CanvasMcpLayer* canvas;
    };
    std::unique_ptr<SlotPasteKeyListener> mcpSlotPasteKeys;
    struct SelectionPillAnimator : public juce::Timer {
        explicit SelectionPillAnimator(CanvasMcpLayer* c) : canvas(c) {}
        void timerCallback() override;
        CanvasMcpLayer* canvas;
    };
    std::unique_ptr<SelectionPillAnimator> mcpPillAnimator;

    struct SketchPoint {
        float x = 0.0f;
        float y = 0.0f;
        double t = 0.0;
    };
    std::vector<SketchPoint> mcpCurrentStroke;
    std::vector<std::vector<SketchPoint>> mcpCompletedStrokes;
    bool isSketching = false;
    bool mcpSketchToolActive = false;
    float mcpSketchAlpha = 1.0f;

    struct SketchFadeTimer : public juce::Timer {
        explicit SketchFadeTimer(CanvasMcpLayer* c) : canvas(c) {}
        void timerCallback() override;
        CanvasMcpLayer* canvas;
    };
    std::unique_ptr<SketchFadeTimer> mcpSketchFadeTimer;
    void startSketchFadeTimer();

    // Phase 1B — Moodboard Chisel: when no PD object is selected but ink exists,
    // the pill anchors 14px below the sketch bounding box as a tactile action
    // bar: [⚡ Build] | prompt | [↩ Undo] [🗑 Clear].
    bool mcpPillSketchMode = false;
    juce::Rectangle<float> mcpLastSketchBounds; // last ink bbox — keeps the chisel parked after Clear
    bool mcpEraseGesture = false;              // current drag is an eraser pass (toolstrip eraser)
    // Mood lane: clicking empty glass opens the pill as a "New Mood" palette
    // (tools + prompt) before any ink exists.
    bool mcpMoodPillActive = false;
    juce::Point<int> mcpMoodPillPos;
    // Paste slot: tapping 🖼 opens an empty dashed frame; Ctrl+V fills it.
    // Nothing is auto-read from the clipboard — paste is an explicit act.
    bool mcpImageSlotActive = false;
    juce::Point<int> mcpImageSlotPos;
    // Transient hint chip (e.g. "Clipboard has no image") under the mood strip.
    juce::String mcpMoodHint;
    std::uint32_t mcpMoodHintUntil = 0;
    // Shift+Enter: park the typed intent + artifacts without waking the AI.
    void parkSelectionPill();
    void sendSketchBuild(const juce::String& prompt); // dispatch /pd/sketch/build
    void undoLastStroke();
    void clearAllInk();

    // Studio brainstorm mode: the sidebar Studio drawer owns commit/undo/clear,
    // the pen is up, and the selection pill is suppressed — nothing to select
    // while collecting ideas. Edit mode (pill) returns when Studio mode is off.
    bool mcpStudioMode = false;
    void setStudioMode(bool active);
    bool isStudioMode() const { return mcpStudioMode; }
    // Commit the visible moodboard from the canvas itself (toolstrip / Ctrl+Enter):
    // pulls the drawer's typed draft, dispatches, clears the draft.
    void commitStudioFromCanvas();
    // Dedicated eraser tool (statusbar dock): left-drag deletes strokes,
    // exactly like Ctrl-drag. Mutually exclusive with the pen.
    bool mcpEraserToolActive = false;
    void setEraserToolActive(bool active);
    bool isEraserToolActive() const { return mcpEraserToolActive; }
    // Type tool (T): click the glass to pin a crisp console-tape text note on the
    // Moodboard layer — never a [comment] DSP box. Reuses the inline note editor.
    bool mcpTextToolActive = false;
    void setTextToolActive(bool active);
    bool isTextToolActive() const { return mcpTextToolActive; }
    void createTextNoteAt(Point<int> canvasPos);
    void openNoteEditorForIndex(int index);
    // Stable tempIds of the current object selection — the surgical lane's anchor.
    juce::StringArray getSelectionStableIds();
    // Visible-Only Ingestion: hidden ink is neither rendered nor sent to the AI.
    bool mcpStudioInkVisible = true;
    void setStudioInkVisible(bool visible);
    bool isStudioInkVisible() const { return mcpStudioInkVisible; }

    // Phase 2 — Reference Images: pasted schematics/photos live on the canvas
    // glass behind cords/objects. Textures cache the decoded image in a
    // context-safe NVGImage (auto-invalidated when the NanoVG context restarts).
    struct RefImageTexture {
        NVGImage texture;          // uploaded via the async recorder (render thread owns the GPU)
        NVGcontext* ctx = nullptr; // context it was made on — recreated if it changes
    };
    std::unordered_map<std::string, RefImageTexture> mcpRefImageCache;
    juce::String mcpDraggingRefImageId;
    juce::Point<int> mcpRefImageDragOffset;
    bool pasteImageFromClipboard();
    bool handleReferenceImageClick(MouseEvent const& e, Point<int> mousePos, bool isRightClick);

    // Phase 3 — Voice take chips on the glass: ▶ audition + mini-waveform + ✕.
    bool handleVoiceTakeClick(MouseEvent const& e, Point<int> mousePos);
    bool handleMidiTakeClick(MouseEvent const& e, Point<int> mousePos);
    juce::String mcpDraggingMidiTakeId;
    juce::Point<int> mcpMidiTakeDragOffset;
    juce::String mcpDraggingTakeId;
    juce::Point<int> mcpTakeDragOffset;
    juce::Point<int> mcpTakeSpawnPos; // where the next recorded take lands (canvas coords)
    // On-demand tool drawer (pressing "/" in the pill). Tools, not orders:
    // a native popup of neutral capabilities. Never shown unless asked.
    void showPillToolsMenu();
    void showPillLensesMenu();
    void sendPillPrompt(const juce::String& prompt, bool queueForChat = false);
    juce::String mcpActiveLens; // "" = free / no lens
    juce::Rectangle<int> mcpSelectionPillFrame; // full visual panel (LED + field + send)
    int mcpPillDrawerMode = 0; // 0 = closed, 1 = lenses, 2 = tools, 3 = smart palette
    juce::Rectangle<int> mcpPillDrawerFrame;
    juce::LookAndFeel_V4 mcpPillLookAndFeel;    // dark menu skin matching the pill
    void configurePillLookAndFeel();

    // Antigravity-style Smart Vertical Command Palette
    struct PillPaletteItem {
        juce::String id;
        juce::String title;
        juce::String category;
        juce::String desc;
        juce::String icon;
        juce::String prompt;
    };
    std::vector<PillPaletteItem> mcpPaletteItems;
    std::vector<int> mcpFilteredPaletteIndices;
    int mcpPaletteSelectedIndex = 0;
    juce::Rectangle<int> mcpPaletteFrame;
    void initPaletteItems();
    void filterPaletteItems(const juce::String& query);
    void showPillPalette();
    void triggerPaletteItem(int index);

    // Tier-2 artist approval: a destructive op from the agent surfaces as two
    // in-canvas chips [Approve] [Cancel] (Enter/Esc). Sends "approve"/"deny" —
    // the server holds the token inert until this human yes. Drawn in CANVAS
    // coords (like the pill) so click hit-testing is transform-free.
    bool isApprovalRequested() const;
    void sendApproval(bool approve);
    juce::Rectangle<int> mcpApprovalFrame; // full bar
    juce::Rectangle<int> mcpApprovalYes;   // Approve chip
    juce::Rectangle<int> mcpApprovalNo;    // Cancel chip

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
