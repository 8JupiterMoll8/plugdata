/*
 // Copyright (c) 2021-2025 Timothy Schoen
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

#pragma once

#include <unordered_map>
#include <nanovg.h>
#ifdef NANOVG_GL_IMPLEMENTATION
#    include <juce_opengl/juce_opengl.h>
using namespace juce::gl;
#    undef NANOVG_GL_IMPLEMENTATION
#    include <nanovg_gl_utils.h>
#    define NANOVG_GL_IMPLEMENTATION 1
#endif

#include "ObjectGrid.h"          // move to impl
#include "Utility/RateReducer.h" // move to impl
#include "Utility/ModifierKeyListener.h"
#include "Components/CheckedTooltip.h"
#include "Pd/MessageListener.h"
#include "Pd/Patch.h"
#include "Constants.h"
#include "Objects/ObjectParameters.h"
#include "NVGSurface.h"
#include "Utility/NVGUtils.h"
#include "Utility/GlobalMouseListener.h"

namespace pd {
class Patch;
}

class SuggestionComponent;
class GraphArea;
class GraphOnParent;
class Iolet;
class Object;
class Connection;
class PluginEditor;
class PluginProcessor;
class ConnectionPathUpdater;
class ConnectionBeingCreated;
class TabComponent;
class BorderResizer;
class CanvasSearchHighlight;
class ObjectsResizer;

struct ObjectDragState {
    bool wasDragDuplicated : 1 = false;
    bool didStartDragging : 1 = false;
    bool wasSelectedOnMouseDown : 1 = false;
    bool wasResized : 1 = false;
    bool wasDuplicated : 1 = false;
    Point<int> canvasDragStartPosition = { 0, 0 };
    Component::SafePointer<Object> componentBeingDragged;
    Component::SafePointer<Object> objectSnappingInbetween;
    Component::SafePointer<Connection> connectionToSnapInbetween;

    Point<int> duplicateOffset = { 0, 0 };
    Point<int> lastDuplicateOffset = { 0, 0 };
};

class Canvas final : public Component
    , public Value::Listener
    , public SettingsFileListener
    , public LassoSource<WeakReference<Component>>
    , public ModifierKeyListener
    , public pd::MessageListener
    , public AsyncUpdater
    , public NVGComponent
    , public ChangeListener {
public:
    Canvas(PluginEditor* parent, pd::Patch::Ptr patch, Component* parentGraph = nullptr);

    ~Canvas() override;

    PluginEditor* editor;
    PluginProcessor* pd;

    void mouseDown(MouseEvent const& e) override;
    void mouseMove(MouseEvent const& e) override;
    void mouseExit(MouseEvent const& e) override;

    // PRD overlay: handle a click that may land on an AI note (x = dismiss, body =
    // open the editor). Returns true if a note consumed it. Called by the canvas AND
    // by objects (so notes overlaying objects stay clickable).
    bool handleNoteClick(Point<int> canvasPt);
    bool isPointOverNote(Point<int> canvasPt) const;
    Object* findObjectByStableId(juce::String const& targetId) const;
    bool getAnnotationLiveBounds(juce::String const& targetId, juce::StringArray const& targetIds, float fallbackX, float fallbackY, float fallbackLx, float fallbackLy, bool fallbackHasLeader, float& outAx, float& outAy, float& outLx, float& outLy, bool& outHasLeader, juce::Rectangle<int>* outGroupBounds = nullptr) const;
    bool getAnnotationLiveBounds(juce::String const& targetId, float fallbackX, float fallbackY, float fallbackLx, float fallbackLy, bool fallbackHasLeader, float& outAx, float& outAy, float& outLx, float& outLy, bool& outHasLeader) const;
    juce::Rectangle<float> getAnnotationScreenBounds(size_t annIndex) const;
    void mouseDrag(MouseEvent const& e) override;
    void mouseUp(MouseEvent const& e) override;
    bool hitTest(int x, int y) override;

    void commandKeyChanged(bool isHeld) override;
    void shiftKeyChanged(bool isHeld) override;
    void middleMouseChanged(bool isHeld) override;
    void altKeyChanged(bool isHeld) override;

    void settingsChanged(String const& name, var const& value) override;

    void focusGained(FocusChangeType cause) override;
    void focusLost(FocusChangeType cause) override;

    void updateFramebuffers(NVGcontext* nvg) override;
    void performRender(NVGcontext* nvg, Rectangle<int> invalidRegion);

    void resized() override;

    void renderAllObjects(NVGcontext* nvg, Rectangle<int> area);
    void renderAllConnections(NVGcontext* nvg, Rectangle<int> area);

    int getOverlays() const;
    void updateOverlays();

    bool shouldShowObjectActivity() const;
    bool shouldShowIndex() const;
    bool shouldShowAIState() const;
    bool shouldShowAIRegions() const;
    bool shouldShowAIAnnotations() const;
    bool shouldShowAIGhosts() const;
    bool shouldShowAIHud() const;
    bool shouldShowAISketch() const;
    bool shouldShowAIReferences() const;
    void setSketchToolActive(bool active);
    bool isSketchToolActive() const;
    bool isSketchingActive() const;
    // Phase 1B: combined bounding box of all persistent ink strokes, in Canvas
    // component coordinates (empty rect when no strokes exist).
    juce::Rectangle<float> getSketchBoundsInCanvas() const;
    // All completed strokes serialized as [[{x,y,t},...],...] in PD patch coords.
    juce::String buildSketchJson() const;
    bool shouldShowConnectionDirection() const;
    bool shouldShowConnectionActivity() const;
    void setOverlayMask(int mask);

    bool hasOverlayTargets() const;
    bool isObjectTargeted(Object const* obj) const;
    bool isConnectionTargeted(Connection const* c) const;

    void save(std::function<void()> const& nestedCallback = [] { });
    void saveAs(std::function<void()> const& nestedCallback = [] { });

    void synchroniseAllCanvases();
    void synchroniseSplitCanvas();
    void synchronise();
    void performSynchronise();
    void handleAsyncUpdate() override;

    void updateDrawables();

    bool keyPressed(KeyPress const& key) override;
    void valueChanged(Value& v) override;

    void tabChanged();

    void hideAllActiveEditors();

    void copySelection();
    void removeSelection();
    void removeSelectedConnections();
    void dragAndDropPaste(String const& patchString, Point<int> mousePos, int patchWidth, int patchHeight, String const& name = String());
    void pasteSelection();
    void duplicateSelection();

    void encapsulateSelection(String const& subpatchName = "pd");
    // Selection-free variant: encapsulate an explicit object list (no GUI selection).
    void encapsulateSelection(String const& subpatchName, SmallArray<Object*> const& objectsToEncapsulate);
    void triggerizeSelection();
    void cycleSelection();
    void connectSelection();
    void tidySelection();

    void cancelConnectionCreation();

    void alignObjects(Align alignment);

    void undo();
    void redo();
    /** True if undo() would actually change something (an MCP transaction or a
     *  non-empty native undo stack). Lets the bridge report an honest no-op
     *  instead of always claiming success. */
    bool canUndo();

    void jumpToOrigin();
    void restoreViewportState();
    void saveViewportState();

    void zoomToFitAll();

    bool autoscroll(MouseEvent const& e);

    // Multi-dragger functions
    void deselectAll(bool broadcastChange = true);
    void setSelected(Component* component, bool shouldNowBeSelected, bool updateCommandStatus = true, bool broadcastChange = true);

    SelectedItemSet<WeakReference<Component>>& getLassoSelection() override;

    bool checkPanDragMode();
    bool setPanDragMode(bool shouldPan);

    bool isPointOutsidePluginArea(Point<int> point) const;

    void findLassoItemsInArea(Array<WeakReference<Component>>& itemsFound, Rectangle<int> const& area) override;

    void updateSidebarSelection();

    void orderConnections();

    void showSuggestions(Object* object, TextEditor* textEditor);
    void hideSuggestions();

    bool panningModifierDown() const;

    ObjectParameters& getInspectorParameters();

    void receiveMessage(t_symbol* symbol, SmallArray<pd::Atom> const& atoms) override;

    void activateCanvasSearchHighlight(Object* obj);
    void removeCanvasSearchHighlight();

    template<typename T>
    SmallArray<T*> getSelectionOfType()
    {
        SmallArray<T*> result;
        for (auto const& obj : selectedComponents) {
            if (auto* objOfType = dynamic_cast<T*>(obj.get())) {
                result.add(objOfType);
            }
        }
        return result;
    }

    std::unique_ptr<Viewport> viewport = nullptr;

    bool connectingWithDrag : 1 = false;
    bool connectionCancelled : 1 = false;
    SafePointer<Iolet> nearestIolet;

    std::unique_ptr<SuggestionComponent> suggestor;

    pd::Patch::Ptr refCountedPatch;
    pd::Patch& patch;

    Value locked = SynchronousValue();
    Value commandLocked;
    Value presentationMode;

    SmallArray<juce::WeakReference<NVGComponent>> drawables;

    // Needs to be allocated before object and connection so they can deselect themselves in the destructor
    SelectedItemSet<WeakReference<Component>> selectedComponents;
    PooledPtrArray<Object> objects;
    PooledPtrArray<Connection> connections;
    PooledPtrArray<ConnectionBeingCreated> connectionsBeingCreated;

    bool showOrigin : 1 = false;
    bool showBorder : 1 = false;
    bool showConnectionOrder : 1 = false;
    bool connectionsBehind : 1 = true;
    bool showObjectActivity : 1 = false;
    bool showIndex : 1 = false;
    bool showAiState : 1 = false;
    bool showAiRegions : 1 = true;
    bool showAiAnnotations : 1 = true;
    bool showAiGhosts : 1 = true;
    bool showAiHud : 1 = true;
    bool showAiSketch : 1 = true;
    bool showAiReferences : 1 = true;
    bool showConnectionDirection : 1 = false;
    bool showConnectionActivity : 1 = false;

    bool isZooming : 1 = false;
    bool isGraph : 1 = false;
    bool isDraggingLasso : 1 = false;
    bool needsSearchUpdate : 1 = false;
    bool altDown : 1 = false;
    bool shiftDown : 1 = false;

    Rectangle<int> currentRenderArea;

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
    void updateSelectionPill();
    void hideSelectionPill();
    void dismissSelectionPill();
    // Enter = live (spawn), Shift+Enter = queue for chat. TextEditor only fires
    // onReturnKey for a PLAIN Return (Shift+Return fails its key comparison), so
    // a KeyListener catches the Shift variant before the editor swallows it.
    void submitSelectionPill(bool queueForChat);
    struct SelectionPillKeyListener : public juce::KeyListener {
        explicit SelectionPillKeyListener(Canvas* c) : canvas(c) {}
        bool keyPressed(const juce::KeyPress& key, juce::Component* origin) override;
        Canvas* canvas;
    };
    std::unique_ptr<SelectionPillKeyListener> mcpSelectionPillKeys;
    // Ctrl+V catcher: fires before TextEditors can swallow the key, so the paste
    // slot works even while a tape-note editor has the caret.
    struct SlotPasteKeyListener : public juce::KeyListener {
        explicit SlotPasteKeyListener(Canvas* c) : canvas(c) {}
        bool keyPressed(const juce::KeyPress& key, juce::Component* origin) override;
        Canvas* canvas;
    };
    std::unique_ptr<SlotPasteKeyListener> mcpSlotPasteKeys;
    struct SelectionPillAnimator : public juce::Timer {
        explicit SelectionPillAnimator(Canvas* c) : canvas(c) {}
        void timerCallback() override;
        Canvas* canvas;
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
        explicit SketchFadeTimer(Canvas* c) : canvas(c) {}
        void timerCallback() override;
        Canvas* canvas;
    };
    std::unique_ptr<SketchFadeTimer> mcpSketchFadeTimer;
    void startSketchFadeTimer();
    void renderSketchOverlay(NVGcontext* nvg);

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
        int handle = 0;            // nvg texture via nvgCreateImage (stb_image path)
        NVGcontext* ctx = nullptr; // context it was made on — recreated if it changes
    };
    std::unordered_map<std::string, RefImageTexture> mcpRefImageCache;
    juce::String mcpDraggingRefImageId;
    juce::Point<int> mcpRefImageDragOffset;
    void renderReferenceImages(NVGcontext* nvg, Rectangle<int> invalidRegion);
    bool pasteImageFromClipboard();
    bool handleReferenceImageClick(MouseEvent const& e, Point<int> mousePos, bool isRightClick);

    // Phase 3 — Voice take chips on the glass: ▶ audition + mini-waveform + ✕.
    void renderVoiceTakes(NVGcontext* nvg, Rectangle<int> invalidRegion);
    bool handleVoiceTakeClick(MouseEvent const& e, Point<int> mousePos);
    void renderMidiTakes(NVGcontext* nvg, Rectangle<int> invalidRegion);
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

    Value isGraphChild = SynchronousValue(var(false));
    Value hideNameAndArgs = SynchronousValue(var(false));
    Value xRange = SynchronousValue();
    Value yRange = SynchronousValue();
    Value patchWidth = SynchronousValue();
    Value patchHeight = SynchronousValue();

    Value zoomScale;

    ObjectGrid objectGrid = ObjectGrid(this);

    int lastObjectGridSize = -1;

    NVGImage dotsLargeImage;

    Point<int> const canvasOrigin;

    std::unique_ptr<GraphArea> graphArea;

    SafePointer<Object> lastSelectedObject;         // For auto patching
    SafePointer<Connection> lastSelectedConnection; // For auto patching

    Point<int> pastedPosition;
    Point<int> pastedPadding;

    std::unique_ptr<ConnectionPathUpdater> pathUpdater;
    RateReducer objectRateReducer = RateReducer(90);

    ObjectDragState dragState;

    static constexpr int infiniteCanvasSize = 128000;

    Component objectLayer;
    Component connectionLayer;

    NVGImage resizeHandleImage;
    NVGImage presentationShadowImage;

    NVGcolor canvasBackgroundCol;
    Colour canvasBackgroundColJuce;
    NVGcolor canvasMarkingsCol;
    Colour canvasMarkingsColJuce;

    Colour canvasTextColJuce;
    NVGcolor presentationBackgroundCol;
    NVGcolor presentationWindowOutlineCol;

    NVGcolor lassoCol;
    NVGcolor lassoOutlineCol;

    // objectOutlineColourId
    NVGcolor objectOutlineCol;
    NVGcolor outlineCol;

    NVGcolor graphAreaCol;

    NVGcolor commentTextCol;

    // guiObjectInternalOutlineColour
    Colour guiObjectInternalOutlineColJuce;
    NVGcolor guiObjectInternalOutlineCol;
    NVGcolor guiObjectBackgroundCol;
    Colour guiObjectBackgroundColJuce;

    NVGcolor textObjectBackgroundCol;
    NVGcolor transparentObjectBackgroundCol;

    // objectSelectedOutlineColourId
    NVGcolor selectedOutlineCol;
    NVGcolor indexTextCol;
    NVGcolor ioletLockedCol;

    NVGcolor baseCol;
    NVGcolor dataCol;
    NVGcolor sigCol;
    NVGcolor gemCol;

    NVGcolor dataColBrighter;
    NVGcolor sigColBrighter;
    NVGcolor gemColBrigher;
    NVGcolor baseColBrigher;

private:
    void changeListenerCallback(ChangeBroadcaster* c) override;

    SelectedItemSet<WeakReference<Component>> previousSelectedComponents;

    void lookAndFeelChanged() override;

    void parentHierarchyChanged() override;

    GlobalMouseListener globalMouseListener;

    bool dimensionsAreBeingEdited = false;

    int lastMouseX, lastMouseY;
    LassoComponent<WeakReference<Component>> lasso;

    RateReducer canvasRateReducer = RateReducer(90);

    // Properties that can be shown in the inspector by right-clicking on canvas
    ObjectParameters parameters;

    std::unique_ptr<BorderResizer> canvasBorderResizer;

    std::unique_ptr<ObjectsResizer> objectsDistributeResizer;

    std::unique_ptr<CanvasSearchHighlight> canvasSearchHighlight;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Canvas)
};
