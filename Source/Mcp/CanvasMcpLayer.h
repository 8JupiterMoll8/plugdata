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

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>
#include "Utility/Config.h"
#include "Utility/NVGUtils.h"
#include "Mcp/McpAiGeometry.h"   // live review 2026-11-19: where a tag and a region's name stand (JUCE-free)
#include "Mcp/McpApprovalState.h" // P12c / P12a.1: ApprovalRect for the chips and Alphi
#include "Mcp/McpAlphiDock.h"     // big Alphi 1a0: where Alphi docks
#include "Mcp/McpAlphiMotion.h"   // Alphi drop 1: motion repaints, the line clock
#include "Mcp/McpPillAnswer.h"     // the newest AI chat line, shown briefly under the pill
#include "Mcp/McpPillMotion.h"     // when the pill animator repaints, and the last repaint of its room
#include "Mcp/McpWidgetFocus.h"    // the widget chip -> selection pill shortcut, and what Enter means on it
struct NVGcontext;

class Canvas;
class Object;
class Connection;

class CanvasMcpLayer {
public:
    explicit CanvasMcpLayer(Canvas& canvas);
    ~CanvasMcpLayer();

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

    // The spotlight (design round B5): a spotlight is on AND this canvas (not a graph-on-parent box) holds one of its boxes. Only
    // then do the boxes outside the path get a veil, the wires change colour and the order discs show: another tab, or a GOP box
    // drawn inside this one, never fades because the AI looks at something else. Cached per targets version and box count.
    bool spotlightHere() const;
    // A wire's colour under a spotlight: ink on the path, the normal colour at 25 % off it. Unchanged without targets, on a canvas
    // the spotlight is not on, and for a selected wire. (Connection::render, MCP-HOOK(overlayTarget).)
    NVGcolor spotlightWire(Connection const* c, NVGcolor normal) const;
    // Any colour of a wire that is not on the lit path (a signal cord's dashes too) at 25 %: aiOffPathCordAlpha. Unchanged on the
    // path, for a selected wire, without targets and on a canvas the spotlight is not on. (Connection::render, MCP-HOOK(overlayTarget).)
    NVGcolor spotlightFade(Connection const* c, NVGcolor normal) const;
    // The blue square (6 px) on a targeted wire while it carries a message: `progress` is the wire's own activity animation (0..1);
    // nothing is drawn at 0 or 1 or without a spotlight. No animation of its own. (Connection::render, MCP-HOOK(overlayTarget).)
    void paintSpotlightActivity(NVGcontext* nvg, Connection const& c, float progress) const;
    // The order discs: a black disc with a white number in the middle of each wire between two targets, numbered from 1 in the
    // order the AI listed them. spotlightSteps is where they go (canvas component coordinates): paintSpotlightOrder draws them, and
    // a region's name steps aside for them. CACHED (review 2026-11-19): the discs are made again only when the spotlight's targets
    // or the canvas's wires changed (one pass over the wires per call, no path is measured); empty without a spotlight.
    struct SpotlightStep {
        juce::Point<float> centre;
        int number = 0;
    };
    std::vector<SpotlightStep> const& spotlightSteps() const;
    void paintSpotlightOrder(NVGcontext* nvg);
    // Does a wire (as its flattened segments, 4 px thick) or an order disc (`steps`) lie on a region's name spot (canvas component
    // coordinates)? Only wires whose bounds reach the spot are flattened (mcp::aiRegionTitlePlaceBy asks this lazily).
    bool regionTitleCovered(mcp::AiRectF spot, std::vector<SpotlightStep> const& steps) const;
    // A hash of the canvas's wires (their count and every path's elements): changes when a wire moves, appears or goes. One pass,
    // no flattening, no lock: the key of the cached region-name sides and of the cached discs.
    std::uint64_t wiresKey() const;
    // Repaint `area` (canvas component coordinates) once this render is over: a repaint asked from inside a render is lost (the
    // surface clears its damage when the frame is recorded).
    void repaintAfterRender(juce::Rectangle<int> area) const;

    // Writes the overlay bit mask into the settings (lock or edit set) and re-reads them.
    void setOverlayMask(int mask);
    // Canvas::updateOverlays: takes the AI bits of the overlay state.
    void updateOverlays(int overlayState);

    // The key of this canvas's ghosts, tags and regions in the session (McpSession::OverlayKey): the canvas's patch pointer, the
    // same one the bridge resolves a canvas name to. Never dereferenced; safe on the render thread.
    void const* overlayKey() const;

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
    // Canvas::undo/redo: one step of the canvas's one history (McpHost::undoStep, P4.4). False: nothing done.
    bool undo();
    bool redo();
    bool canUndo(); // an MCP transaction or the native stack (also the editor's undo button state)
    bool canRedo();
    // Slice 3 F5: Canvas::removeSelection is about to delete these boxes (the artist's Cut/Delete): their ids go to the
    // canvas's undo journal (McpHost::journalEditorRemove), so Ctrl+Z gives them back.
    void aboutToRemove(SmallArray<struct _gobj*> const& objects);

    // PRD overlay: handle a click that may land on an AI note (x = dismiss, body =
    // open the editor). Returns true if a note consumed it. Called by the canvas AND
    // by objects (so notes overlaying objects stay clickable).
    bool handleNoteClick(Point<int> canvasPt);
    bool isPointOverNote(Point<int> canvasPt) const;
    Object* findObjectByStableId(juce::String const& targetId) const;
    // The stable id of an object of this canvas ("" when none). Both read one cache (refreshStableIdCache): the
    // overlay paint looks ids up per annotation / region / chip target every frame, and each getStableId took the
    // Pd lock and scanned every identity record (a freeze while the AI's command worker held the lock).
    juce::String stableIdOfObject(Object const* obj) const;
    void refreshStableIdCache() const;
    void repaintHudArea(); // the band the HUD card is drawn in (paintScreenOverlay), not the whole view
    static constexpr int hudBandHeight = 200; // the HUD card with its shadow is at most ~190 px high
    juce::Rectangle<int> mcpPaintRegion; // the current render's region, canvas coordinates (paintUnderObjects)
    std::uint64_t stableIdObjectsKey() const;
    mutable std::unordered_map<std::string, Object*> mcpIdToObject;
    mutable std::unordered_map<Object const*, juce::String> mcpObjectToId;
    mutable std::uint64_t mcpIdCacheVersion = 0;
    mutable std::uint64_t mcpIdCacheObjects = 0;
    mutable bool mcpIdCacheValid = false;
    // spotlightHere's cache (render thread) and the targets version updateOverlays last saw (message thread).
    mutable std::uint64_t mcpSpotVersion = ~std::uint64_t(0);
    mutable std::size_t mcpSpotObjects = ~std::size_t(0);
    mutable bool mcpSpotHere = false;
    std::uint64_t mcpSeenTargetsVersion = 0;
    // spotlightSteps' cache: the discs and the key (targets version + the wires) they were made for (render thread).
    mutable std::vector<SpotlightStep> mcpSteps;
    mutable std::uint64_t mcpStepsKey = 0;
    mutable bool mcpStepsValid = false;
    // Which side each region's name stands on, remembered between renders (render thread); see mcp::AiTitleSideCache.
    mcp::AiTitleSideCache mcpTitleSides;
    bool getAnnotationLiveBounds(juce::String const& targetId, juce::StringArray const& targetIds, float fallbackX, float fallbackY, float fallbackLx, float fallbackLy, bool fallbackHasLeader, float& outAx, float& outAy, float& outLx, float& outLy, bool& outHasLeader, juce::Rectangle<int>* outGroupBounds = nullptr) const;
    bool getAnnotationLiveBounds(juce::String const& targetId, float fallbackX, float fallbackY, float fallbackLx, float fallbackLy, bool fallbackHasLeader, float& outAx, float& outAy, float& outLx, float& outLy, bool& outHasLeader) const;
    // The frame of one AI tag (design round B2): the ONE place a tag is drawn AND hit-tested (hover, the close cross, the chips drawer
    // and the click all read it), so a tag is never drawn where it cannot be clicked. Canvas component coordinates (the canvas origin
    // is added). `tag` = glyph slot + estimated text + the close slot; empty when the annotation's boxes are gone (auto-prune).
    // Right of the target boxes, vertically centred, 14 px from their right edge; when another box lies there, where the bridge put
    // it (above the target, or on the side it found free).
    struct AnnotationFrame {
        juce::Rectangle<float> tag;
        juce::Rectangle<float> group; // the target boxes' union (empty: a tag with no target)
        juce::Point<float> leaderFrom; // on the boxes
        juce::Point<float> leaderTo;   // on the tag
        bool hasLeader = false;
    };
    static constexpr float annotationCloseSlotPx = 16.0f; // the close cross sits in the last 16 px of the frame (shown on hover)
    static constexpr float annotationMaxWidthPx = 380.0f; // a longer text is cut by the frame, never drawn past it
    // Every box of the canvas as a tag's obstacle (patch coordinates), `owners[k]` being the box of `obstacles[k]`. Built ONCE per
    // paint (or per pointer event) and shared by all the tags of it: the old code built this list again for each tag, on every
    // paint and every hover (review 2026-11-19). A tag skips the boxes it speaks about by index (mcp::aiTagPlace).
    struct AnnotationScene {
        std::vector<Object const*> owners;
        std::vector<mcp::AiRectF> obstacles;
    };
    AnnotationScene buildAnnotationScene() const;
    AnnotationFrame getAnnotationFrame(size_t annIndex, AnnotationScene const& scene) const;
    juce::Rectangle<float> getAnnotationScreenBounds(size_t annIndex, AnnotationScene const& scene) const;
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
    // An ai.* widget's chip ("AI...") is a shortcut for "select it, the pill opens" (McpWidgetFocus.h). `object` is the widget's box, or the box
    // that holds it (a wrapper abstraction, a graph-on-parent subpatch). The chip is pressed in run mode and the pill lives in edit mode: the canvas
    // goes to edit mode for it and goes back when the artist sends, presses Enter on nothing or Escape (restoreRunMode). False when the pill cannot
    // be shown here (a graph, a box being typed in, a canvas not on screen): then nothing changed.
    bool focusPillOn(Object* object, mcp::WidgetPillTarget const& target);
    // Enter on no text: if the one selected box is an ai.* widget, tells it to ask (the pill's "complete it"). Fills `target` for the
    // decision (McpWidgetFocus.h pillAction); false = not a single widget.
    mcp::WidgetPillTarget selectedWidgetTarget(SmallArray<Object*> const& selected);
    // The canvas goes back to the mode the chip found it in (run mode / presentation), once, and only when it is still in the edit mode the chip
    // put it in; any other way the pill goes away forgets it (the artist took over).
    void restoreRunMode();
    struct WidgetFocus {
        bool active = false;
        bool wasLocked = false;
        bool wasPresentation = false;
        struct _gobj* box = nullptr; // compared only
        mcp::WidgetPillTarget target;
    } mcpWidgetFocus;
    // Enter = live (spawn), Shift+Enter = queue for chat. TextEditor only fires
    // onReturnKey for a PLAIN Return (Shift+Return fails its key comparison), so
    // a KeyListener catches the Shift variant before the editor swallows it.
    void submitSelectionPill(bool queueForChat);
    // The QUICK / DEEP chip on the object pill, between the prompt field and the send arrow. DEEP = a typed prompt goes down
    // the deep lane (wire "queue"); QUICK = the in-app lane. The lane lives in the bridge (isPillDeepLane): the server pushes
    // its sticky lane (/chat/lane) and a click sets it here at once and tells the server (the lane word as a prompt).
    static constexpr int pillLaneChipW = 46; // the room the chip takes in the pill's width
    static juce::Rectangle<int> pillLaneChipRect(juce::Rectangle<int> panel)
    {
        return { panel.getRight() - 32 - pillLaneChipW + 3, panel.getY() + (panel.getHeight() - 16) / 2, pillLaneChipW - 6, 16 };
    }
    void togglePillLane();
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
        mcp::PillAnimPlan last; // the plan of the tick before: its room gets one last repaint when it ends (McpPillMotion.h)
    };
    std::unique_ptr<SelectionPillAnimator> mcpPillAnimator;

    // The pill's answer line: the newest AI chat line (`/chat/msg ai`) as one short line under the pill's place, holding
    // ~7 s and fading ~1 s, cleared by the artist's next prompt (McpPillAnswer.h). The sidebar keeps every line: this
    // layer is one more reader of the bridge's chat lines (MCPBridge::addChatListener), not a replacement of its slot.
    mcp::PillAnswer mcpPillAnswer;
    struct PillAnswerTimer : public juce::Timer {
        explicit PillAnswerTimer(CanvasMcpLayer* c) : canvas(c) {}
        void timerCallback() override;
        CanvasMcpLayer* canvas;
    };
    std::unique_ptr<PillAnswerTimer> mcpPillAnswerTimer;
    void onChatLine(juce::String const& role, juce::String const& text);
    void paintPillAnswer(NVGcontext* nvg);
    void repaintPillAnswer(); // the strip under the pill's last place, not the whole canvas

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

    // P12c artist approval: one chip per pending record of the server (session.approvals, set only by
    // /pd/approval_show; HUD text never shows chips). A click answers exactly the record of its chip:
    // /pd/approval/answer <nonce> yes|no. Enter only for exactly one chip on screen >= 400 ms; Esc cancels all.
    // Drawn in canvas coords at a fixed SCREEN size (design px / zoom, mcp::approvalStackLayout), the whole stack
    // clamped inside the visible viewport (CanvasMcpApproval.cpp).
    bool isApprovalRequested() const;
    void answerApproval(juce::String const& nonce, bool yes);
    void paintApprovalChips(NVGcontext* nvg);
    bool handleApprovalClick(juce::Point<int> canvasPt);
    std::optional<bool> handleApprovalKey(juce::KeyPress const& key);
    struct ApprovalHit {
        juce::String nonce;
        juce::Rectangle<int> frame, yes, no;
    };
    std::vector<ApprovalHit> mcpApprovalHits;
    juce::Rectangle<int> mcpApprovalFrame; // the whole stack (hit priority over the pill)
    // Repaints the canvas when the next chip expires (C++ TTL), so an expired chip leaves the glass on time.
    void repaintApprovals();
    struct ApprovalExpiryTimer : public juce::Timer {
        explicit ApprovalExpiryTimer(CanvasMcpLayer* c) : canvas(c) {}
        void timerCallback() override;
        CanvasMcpLayer* canvas;
    };
    std::unique_ptr<ApprovalExpiryTimer> mcpApprovalExpiryTimer;

    // P12a.1: Alphi, the robot face in a corner of the visible view (fixed screen size) while the MCP bridge runs. It is
    // THE flow-mode switch: a click turns flow mode on, the next one off (CanvasMcpFlow.cpp, McpAlphi.h). It counts a
    // click only once it has been on screen 400 ms; the approval chips are laid out around it (alphiKeepOut); it
    // steps aside (not drawn, not clickable) while the selection pill covers its corner.
    // Big Alphi 1a0: it docks in a corner no box, chip stack, pill or drawer lies under (default bottom right;
    // McpAlphiDock.h). updateAlphiDock runs each paint before the chips and applies a move the 100 ms timer decided,
    // only in a render that redraws both frames (review H2); a move restarts the 400 ms click gate.
    void updateAlphiDock();
    void paintAlphi(NVGcontext* nvg);
    bool handleAlphiClick(juce::Point<int> canvasPt);
    // Alphi's rect in canvas coordinates when it is drawn on this view, for the chip layout; nullopt when it is not.
    std::optional<mcp::ApprovalRect> alphiKeepOut(mcp::ApprovalRect view, float zoom) const;
    bool alphiShown() const;
    // Review M2 / L2: a chip stack, the pill or its drawers cover Alphi's rect (then it is not drawn or clickable).
    bool alphiCoveredNow(juce::Rectangle<int> frame) const;
    // Review L1: the tab went hidden or came back: Alphi's 400 ms on-screen time starts again.
    void alphiVisibilityTick();
    // 1a0: the view (canvas units) and zoom Alphi docks on; what lies on the glass that Alphi must not sit under;
    // whether the mouse is on/next to Alphi or holds a button (then it never moves).
    std::pair<mcp::ApprovalRect, float> alphiView() const;
    std::vector<mcp::ApprovalRect> alphiDockCovers() const;
    std::vector<mcp::ApprovalRect> alphiBoxes() const;
    mcp::AlphiMouse alphiMouse() const;
    mcp::AlphiDock mcpAlphiDock;
    std::vector<mcp::ApprovalRect> mcpAlphiPaintBoxes; // the boxes of this paint (updateAlphiDock -> paintAlphi)
    // Review H2: the canvas region this render redraws (paintUnderObjects), used once by updateAlphiDock/paintAlphi.
    juce::Rectangle<int> mcpAlphiRenderRegion;
    // Review M2: Alphi's frame on screen at the last full paint of it.
    juce::Point<int> mcpAlphiScreenPos;
    bool mcpAlphiHasScreenPos = false;
    mcp::AlphiRepaintState mcpAlphiRepaint; // review N2: one repaint of Alphi's frame per look change / unstarted gate
    juce::Rectangle<int> mcpAlphiFrame;
    std::int64_t mcpAlphiShownSinceMs = 0;
    bool mcpAlphiWasShowing = false;
    struct AlphiVisibilityTimer : public juce::Timer {
        explicit AlphiVisibilityTimer(CanvasMcpLayer* c) : layer(c) {}
        void timerCallback() override { layer->alphiVisibilityTick(); }
        CanvasMcpLayer* layer;
    };
    std::unique_ptr<AlphiVisibilityTimer> mcpAlphiVisibilityTimer;

    // Alphi drop 1 (docs/plans/alphi-drop1-alive-steps.md in the MCP repo). Display only: none of this reaches the
    // flow switch, the click gate or the dock (scripts/test-bridge-safety.sh).
    // D11 (CanvasMcpFlow.cpp): no free corner -> the edge tab; a click on it brings him out, the next click back.
    mcp::AlphiPeek mcpAlphiPeek;
    juce::Rectangle<int> mcpAlphiTabFrame; // the tab as drawn in the last paint (empty: not tabbed)
    bool mcpAlphiTabUnder = false;         // no free edge spot: the tab is drawn under the boxes (paintUnderObjects)
    juce::Rectangle<int> mcpAlphiTabFrameUnder; // where paintUnderObjects draws that tab (from the last paint)
    mcp::AlphiCorner mcpAlphiFaceCorner = mcp::AlphiCorner::bottomRight; // where the drawn face sits (for the strip)
    // Review N3: the dock pass is skipped in a paint that is Alphi's own (the motion timer asked, the region stays
    // near him); CanvasMcpFlow.cpp.
    bool alphiDockPassNeeded();
    void alphiRepaintView(); // repaint his view (the canvas viewport), not the whole editor
    // CanvasMcpAlphiSay.cpp: the pulse (D2/D3), the gaze (D5), sparks / dots / spiral, the strip (D6) and the D4
    // motion timer. It repaints only Alphi's rect and strip, and never touches the gate or the dock.
    void alphiMotionSample(std::int64_t nowMs); // the ceiling's pulse and the say board's line clock, now
    mcp::AlphiEyes alphiEyesNow(mcp::AlphiLook const& look, bool coveredHisRect); // draws the held level; records it only when covered
    mcp::AlphiGaze alphiGazeNow(mcp::ApprovalRect face) const;
    void paintAlphiFx(NVGcontext* nvg, std::int64_t nowMs);
    void paintAlphiDizzyEye(NVGcontext* nvg, float x, float y, mcp::AlphiRgba colour, std::int64_t nowMs);
    void paintAlphiStrip(NVGcontext* nvg);
    mcp::ApprovalRect paintAlphiStripNow(NVGcontext* nvg); // draws the strip when shown; its rect, else empty
    void paintAlphiTab(NVGcontext* nvg);
    void alphiMotionWake();
    void alphiMotionTick();
    void alphiMotionRepaint(bool sayChanged);
    void alphiRememberZone(); // review HIGH: the layout key near Alphi after each paint (CanvasMcpFlow.cpp)
    void alphiSetTabUnder(juce::Rectangle<int> next); // review L4: repaints the old spot of a tab under the boxes
    void alphiMotionStop();
    mcp::AlphiLineClock mcpAlphiLineClock;
    float mcpAlphiPulseLevel = 0.0f;  // the followed level (alphiPulseFollow)
    float mcpAlphiPulseTarget = 0.0f; // what the ceiling asks for now (alphiPulse)
    float mcpAlphiPulseDrawn = 0.0f;  // the level last drawn: the held one (alphiPulseHeld, the eye size on a 0.25 px grid)
    std::uint64_t mcpAlphiPulseBlocks = 0;
    std::int64_t mcpAlphiPulseFreshMs = 0;  // when the ceiling's block counter last moved (GUI clock)
    std::int64_t mcpAlphiPulseSampleMs = 0; // the last follow step
    mcp::AlphiGaze mcpAlphiGazeDrawn;
    std::string mcpAlphiSayKeyDrawn;
    bool mcpAlphiDizzyDrawn = false;
    float mcpAlphiLineAlphaDrawn = 0.0f;
    juce::Rectangle<int> mcpAlphiStripFrame; // the strip as drawn in the last paint (empty: not shown); swallows its click
    mcp::AlphiMotionAsk mcpAlphiMotionAsk;   // the motion timer asked for the next paint (N3); seen by every pass of that frame
    juce::Rectangle<int> mcpAlphiStripRegion; // review M: this paint's render region, for the strip's frame
    mcp::ApprovalRect mcpAlphiZoneRect;       // review HIGH: the zone and layout key of the last paint
    std::uint64_t mcpAlphiZoneKey = 0;
    bool mcpAlphiZoneKnown = false;
    struct AlphiMotionTimer : public juce::Timer {
        explicit AlphiMotionTimer(CanvasMcpLayer* c) : layer(c) {}
        void timerCallback() override { layer->alphiMotionTick(); }
        CanvasMcpLayer* layer;
    };
    std::unique_ptr<AlphiMotionTimer> mcpAlphiMotionTimer;
    // Alphi's pointer (alphiMouse): from the mouse events of the canvas and every component on it, never queried in
    // a paint (getMouseXYRelative / isMouseOver are a synchronous XQueryPointer each on Linux).
    struct AlphiMouseTracker final : public juce::MouseListener {
        explicit AlphiMouseTracker(CanvasMcpLayer& l);
        ~AlphiMouseTracker() override;
        void mouseEnter(juce::MouseEvent const& e) override { seen(e, true); }
        void mouseMove(juce::MouseEvent const& e) override { seen(e, true); }
        void mouseDown(juce::MouseEvent const& e) override { seen(e, true); }
        void mouseDrag(juce::MouseEvent const& e) override { seen(e, true); }
        void mouseUp(juce::MouseEvent const& e) override { seen(e, true); }
        void mouseExit(juce::MouseEvent const& e) override { seen(e, false); }
        void seen(juce::MouseEvent const& e, bool inside);
        CanvasMcpLayer& layer;
        bool known = false;
        juce::Point<int> position; // canvas coordinates
    };
    std::unique_ptr<AlphiMouseTracker> mcpAlphiMouseTracker;
    // While busy (sparks / dots only) the motion timer repaints at most every busyRepaintMs (~12 Hz), the dots only
    // when the lit one changes (CanvasMcpAlphiStrip.cpp alphiMotionTick).
    static constexpr std::int64_t busyRepaintMs = 83;
    std::int64_t mcpAlphiBusyRepaintMs = 0;
    int mcpAlphiBusyKeyDrawn = -2;

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
