/*
 // Copyright (c) 2021-2025 Timothy Schoen
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
 */
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_cryptography/juce_cryptography.h>
#include "Utility/Config.h"
#include "Utility/Fonts.h"

#include "Sidebar/Sidebar.h"
#include "Statusbar.h"
#include "Canvas.h"
#include "Object.h"
#include "Sidebar/CopilotPanel.h"
#include "Connection.h"
#include "PluginProcessor.h"
#include "Pd/MCPBridge.h" // artist-note forwarding from the inline note editor
#include "PluginEditor.h"
#include "LookAndFeel.h"
#include "Components/SuggestionComponent.h"
#include "CanvasViewport.h"

#include "Objects/ObjectBase.h"

#include "Dialogs/Dialogs.h"
#include "Components/GraphArea.h"
#include "Components/CanvasBorderResizer.h"

#include "Components/CanvasSearchHighlight.h"

extern "C" {
void canvas_setgraph(t_glist* x, int flag, int nogoprect);
}

class ObjectsResizer final : public Component
    , NVGComponent
    , Value::Listener {
public:
    enum class ResizerMode { Horizontal,
        Vertical };

    ObjectsResizer(Canvas* parentCanvas, std::function<float(Rectangle<int> bounds)> onResize, std::function<void(Point<int> pos)> onMove, ResizerMode const mode = ResizerMode::Horizontal)
        : NVGComponent(this)
        , border(this, &constrainer)
        , cnv(parentCanvas)
        , mode(mode)
        , onResize(std::move(onResize))
        , onMove(std::move(onMove))
    {
        cnv->addAndMakeVisible(this);
        setAlwaysOnTop(true);

        cnv->zoomScale.addListener(this);

        auto selectedObjectBounds = Rectangle<int>();

        auto smallestObjectWidthOrHeight = std::numeric_limits<int>::max();
        auto largestObjectWidthOrHeight = 0;

        // work out the bounds for all objects, and also the bounds for the largest object (used for min size of constrainer)
        for (auto* obj : cnv->getSelectionOfType<Object>()) {
            obj->hideHandles(true);

            selectedObjectBounds = selectedObjectBounds.getUnion(obj->getBounds().reduced(Object::margin, Object::margin));

            // Find the smallest object to make the min constrainer size
            if (mode == ResizerMode::Horizontal) {
                auto const objWidth = obj->getObjectBounds().getWidth();
                if (objWidth < smallestObjectWidthOrHeight) {
                    smallestObjectWidthOrHeight = objWidth;
                }
                if (objWidth > largestObjectWidthOrHeight) {
                    largestObjectWidthOrHeight = objWidth;
                }
            } else {
                auto const objHeight = obj->getObjectBounds().getHeight();
                if (objHeight < smallestObjectWidthOrHeight) {
                    smallestObjectWidthOrHeight = objHeight;
                }
                if (objHeight > largestObjectWidthOrHeight) {
                    largestObjectWidthOrHeight = objHeight;
                }
            }
        }

        setBorderScale(cnv->zoomScale);

        if (mode == ResizerMode::Horizontal) {
            constrainer.setMinimumWidth(largestObjectWidthOrHeight + tabMargin * 2);
        } else {
            constrainer.setMinimumHeight(largestObjectWidthOrHeight + tabMargin * 2);
        }

        addAndMakeVisible(border);

        setBounds(selectedObjectBounds.expanded(tabMargin));
    }

    ~ObjectsResizer() override
    {
        for (auto* obj : cnv->getSelectionOfType<Object>()) {
            obj->hideHandles(false);
        }

        if (cnv) {
            cnv->zoomScale.removeListener(this);
        }
    }

    void setBorderScale(Value const& canvasScale)
    {
        auto const scale = getValue<float>(canvasScale);
        auto const borderSize = std::max(12.0f, 12 / scale);
        if (mode == ResizerMode::Horizontal) {
            border.setBorderThickness(juce::BorderSize<int>(0, borderSize, 0, borderSize));
        } else {
            border.setBorderThickness(juce::BorderSize<int>(borderSize, 0, borderSize, 0));
        }
    }

    void valueChanged(Value& v) override
    {
        if (v.refersToSameSourceAs(cnv->zoomScale)) {
            setBorderScale(cnv->zoomScale);
        }
    }

    bool hitTest(int x, int y) override
    {
        if (cnv->panningModifierDown() || ModifierKeys::getCurrentModifiers().isAnyModifierKeyDown())
            return false;

        return true;
    }

    void mouseDown(MouseEvent const& e) override
    {
        if (e.mods.isLeftButtonDown())
            originalPos = getPosition();

        // We can allow launching the right-click menu, however we would need to turn off alignment etc...
        // if (e.mods.isRightButtonDown()){
        //    cnv->objectLayer.getComponentAt(e.getEventRelativeTo(cnv).getPosition())->mouseDown(e);
        //}
    }

    void mouseDrag(MouseEvent const& e) override
    {
        if (e.originalComponent == &border)
            return;
        if (e.mods.isLeftButtonDown()) {
            auto const delta = e.getPosition() - e.getMouseDownPosition();
            auto const newPos = originalPos + delta;
            setTopLeftPosition(newPos);
        }
    }

    void moved() override
    {
        onMove(getPosition());
    }

    void resized() override
    {
        border.setBounds(getLocalBounds().expanded(tabMargin));
        spacer = onResize(getBounds().reduced(tabMargin));
    }

    void render(NVGcontext* nvg) override
    {
        // draw background and outline
        auto const b = getBounds().reduced(tabMargin);
        auto iCol = cnv->selectedOutlineCol;

        iCol.a = 5; // Make the inner colour semi-transparent
        nvgDrawRoundedRect(nvg, b.getX(), b.getY(), b.getWidth(), b.getHeight(), iCol, cnv->selectedOutlineCol, Corners::objectCornerRadius);

        // Draw handles at edge
        auto getCorners = [this] {
            auto const rect = getBounds().reduced(tabMargin);
            constexpr float offset = 2.0f;

            Array<Rectangle<float>> corners = { Rectangle<float>(9.0f, 9.0f).withCentre(rect.getTopLeft().toFloat()).translated(offset, offset), Rectangle<float>(9.0f, 9.0f).withCentre(rect.getBottomLeft().toFloat()).translated(offset, -offset),
                Rectangle<float>(9.0f, 9.0f).withCentre(rect.getBottomRight().toFloat()).translated(-offset, -offset), Rectangle<float>(9.0f, 9.0f).withCentre(rect.getTopRight().toFloat()).translated(-offset, offset) };

            return corners;
        };

        auto& resizeHandleImage = cnv->resizeHandleImage;
        int angle = 360;
        for (auto& corner : getCorners()) {
            NVGScopedState scopedState(nvg);
            // Rotate around centre
            nvgTranslate(nvg, corner.getCentreX(), corner.getCentreY());
            nvgRotate(nvg, degreesToRadians<float>(angle));
            nvgTranslate(nvg, -4.5f, -4.5f);

            nvgBeginPath(nvg);
            nvgRect(nvg, 0, 0, 9, 9);
            nvgFillPaint(nvg, nvgImageAlphaPattern(nvg, 0, 0, 9, 9, 0, resizeHandleImage.getImageId(), cnv->selectedOutlineCol));
            nvgFill(nvg);
            angle -= 90;
        }
// #define SPACER_TEXT
#ifdef SPACER_TEXT
        nvgBeginPath(nvg);
        auto textPos = getPosition().translated(0, -25);
        nvgFontSize(nvg, 20.0f);
        nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(nvg, nvgRGBA(240, 240, 240, 255));
        nvgText(nvg, textPos.x, textPos.y, String("Spacer size: " + String(spacer + 1.0f)).toRawUTF8(), nullptr);
#endif
    }

private:
    ResizableBorderComponent border;
    ComponentBoundsConstrainer constrainer;
    Canvas* cnv;
    int tabMargin = Object::margin;

    Point<int> originalPos;
    float spacer = 0;

    ResizerMode mode;

    std::function<float(Rectangle<int>)> onResize;
    std::function<void(Point<int>)> onMove;
};

Canvas::Canvas(PluginEditor* parent, pd::Patch::Ptr p, Component* parentGraph)
    : NVGComponent(this)
    , editor(parent)
    , pd(parent->pd)
    , refCountedPatch(p)
    , patch(*p)
    , canvasOrigin(Point<int>(infiniteCanvasSize / 2, infiniteCanvasSize / 2))
    , graphArea(nullptr)
    , pathUpdater(new ConnectionPathUpdater(this))
    , globalMouseListener(this)
{
    selectedComponents.addChangeListener(this);

    addAndMakeVisible(objectLayer);
    addAndMakeVisible(connectionLayer);

    objectLayer.setInterceptsMouseClicks(false, true);
    connectionLayer.setInterceptsMouseClicks(false, true);

    if (auto patchPtr = patch.getPointer()) {
        isGraphChild = glist_isgraph(patchPtr.get());
        hideNameAndArgs = static_cast<bool>(patchPtr->gl_hidetext);
        xRange = VarArray { var(patchPtr->gl_x1), var(patchPtr->gl_x2) };
        yRange = VarArray { var(patchPtr->gl_y2), var(patchPtr->gl_y1) };
    }

    pd->registerMessageListener(patch.getUncheckedPointer(), this);

    isGraphChild.addListener(this);
    hideNameAndArgs.addListener(this);
    xRange.addListener(this);
    yRange.addListener(this);

    auto const patchBounds = patch.getBounds();
    patchWidth = patchBounds.getWidth();
    patchHeight = patchBounds.getHeight();

    patchWidth.addListener(this);
    patchHeight.addListener(this);

    globalMouseListener.globalMouseMove = [this](MouseEvent const& e) {
        lastMouseX = e.x;
        lastMouseY = e.y;
    };
    globalMouseListener.globalMouseDrag = [this](MouseEvent const& e) {
        lastMouseX = e.x;
        lastMouseY = e.y;
    };

    suggestor = std::make_unique<SuggestionComponent>();
    canvasBorderResizer = std::make_unique<BorderResizer>(this);
    canvasBorderResizer->onDrag = [this] {
        patchWidth = std::max(0, canvasBorderResizer->getBounds().getCentreX() - canvasOrigin.x);
        patchHeight = std::max(0, canvasBorderResizer->getBounds().getCentreY() - canvasOrigin.y);
    };

    canvasBorderResizer->setCentrePosition(canvasOrigin.x + patchBounds.getWidth(), canvasOrigin.y + patchBounds.getHeight());
    addAndMakeVisible(canvasBorderResizer.get());

    // Check if canvas belongs to a graph
    if (parentGraph) {
        setLookAndFeel(&editor->getLookAndFeel());
        parentGraph->addAndMakeVisible(this);
        setInterceptsMouseClicks(false, true);
        isGraph = true;
    } else {
        isGraph = false;
    }
    if (!isGraph) {
        auto* canvasViewport = new CanvasViewport(editor, this);

        canvasViewport->setViewedComponent(this, false);

        canvasViewport->onScroll = [this] {
            if (suggestor) {
                suggestor->updateBounds();
            }
            if (graphArea) {
                graphArea->updateBounds();
            }
        };

        canvasViewport->setScrollBarsShown(true, true, true, true);

        viewport.reset(canvasViewport); // Owned by the tabbar, but doesn't exist for graph!
        restoreViewportState();
    }

    commandLocked.referTo(pd->commandLocked);
    commandLocked.addListener(this);

    // pd->commandLocked doesn't get updated when a canvas isn't active
    // So we set it to false here when a canvas is remade
    // Otherwise the last canvas could have set it true, and it would still be
    // in that state without command actually being locked
    if (!isGraph)
        commandLocked.setValue(false);

    // init border for testing
    settingsChanged("border", SettingsFile::getInstance()->getPropertyAsValue("border"));

    // Add draggable border for setting graph position
    if (getValue<bool>(isGraphChild) && !isGraph) {
        graphArea = std::make_unique<GraphArea>(this);
        addAndMakeVisible(*graphArea);
        graphArea->setAlwaysOnTop(true);
    }

    if (!isGraph) {
        editor->nvgSurface.addBufferedObject(this);
    }

    setSize(infiniteCanvasSize, infiniteCanvasSize);

    // initialize to default zoom
    auto const defaultZoom = SettingsFile::getInstance()->getPropertyAsValue("default_zoom");
    zoomScale.setValue(getValue<float>(defaultZoom) / 100.0f);
    zoomScale.addListener(this);

    // Add lasso component
    addAndMakeVisible(&lasso);
    lasso.setAlwaysOnTop(true);

    setWantsKeyboardFocus(true);

    if (!isGraph) {
        presentationMode.addListener(this);
    } else {
        presentationMode = false;
    }
    performSynchronise();

    // Start in unlocked mode if the patch is empty
    if (objects.empty()) {
        locked = false;
        if (auto patchPtr = patch.getPointer())
            patchPtr->gl_edit = false;
    } else {
        if (auto patchPtr = patch.getPointer())
            locked = !patchPtr->gl_edit;
    }

    locked.addListener(this);

    editor->addModifierKeyListener(this);

    updateOverlays();
    orderConnections();

    parameters.addParamBool("Is graph", cGeneral, &isGraphChild, { "No", "Yes" }, 0);
    parameters.addParamBool("Hide name and arguments", cGeneral, &hideNameAndArgs, { "No", "Yes" }, 0);
    parameters.addParamRange("X range", cGeneral, &xRange, { 0.0f, 1.0f });
    parameters.addParamRange("Y range", cGeneral, &yRange, { 1.0f, 0.0f });

    auto onInteractionFn = [this](bool const state) {
        dimensionsAreBeingEdited = state;
        repaint();
    };

    parameters.addParamInt("Width", cDimensions, &patchWidth, 527, true, 0, 1 << 30, onInteractionFn);
    parameters.addParamInt("Height", cDimensions, &patchHeight, 327, true, 0, 1 << 30, onInteractionFn);

    if (!isGraph) {
        patch.setVisible(true);
    }

    lookAndFeelChanged();
}

Canvas::~Canvas()
{
    for (auto* object : objects) {
        object->hideEditor();
    }

    if (!isGraph) {
        editor->nvgSurface.removeBufferedObject(this);
    }

    saveViewportState();
    zoomScale.removeListener(this);
    editor->removeModifierKeyListener(this);
    pd->unregisterMessageListener(this);

    // Drop this canvas's MCP undo/redo transactions. Without this the stacks
    // leak (<=50 full forward+inverse messages each) and, worse, a later canvas
    // could reuse the freed t_canvas* address and inherit a stale stack.
    if (!isGraph)
        pd->clearMcpTransactions(patch.getUncheckedPointer());

    patch.setVisible(false);
    selectedComponents.removeChangeListener(this);
}

void Canvas::changeListenerCallback(ChangeBroadcaster* c)
{
    if (c == &selectedComponents) {
        auto isSelectedDifferent = [](SelectedItemSet<WeakReference<Component>> const& set1, SelectedItemSet<WeakReference<Component>> const& set2) -> bool {
            if (set1.getNumSelected() != set2.getNumSelected())
                return true;
            for (int i = 0; i < set1.getNumSelected(); i++) {
                if (!set2.isSelected(set1.getSelectedItem(i)))
                    return true;
            }
            return false; // identical
        };

        if (isSelectedDifferent(selectedComponents, previousSelectedComponents)) {
            previousSelectedComponents = selectedComponents;
            editor->updateSelection(this);
            updateSelectionPill();
        }
    }
}

void Canvas::lookAndFeelChanged()
{
    // Canvas colours
    auto const& lnf = editor->getLookAndFeel();
    canvasBackgroundColJuce = lnf.findColour(PlugDataColour::canvasBackgroundColourId);
    canvasBackgroundCol = convertColour(canvasBackgroundColJuce);
    canvasMarkingsColJuce = lnf.findColour(PlugDataColour::canvasDotsColourId).interpolatedWith(canvasBackgroundColJuce, 0.2f);
    canvasMarkingsCol = convertColour(canvasMarkingsColJuce);
    canvasTextColJuce = lnf.findColour(PlugDataColour::canvasTextColourId);

    // Object colours
    objectOutlineCol = convertColour(lnf.findColour(PlugDataColour::objectOutlineColourId));
    outlineCol = convertColour(lnf.findColour(PlugDataColour::outlineColourId));
    textObjectBackgroundCol = convertColour(lnf.findColour(PlugDataColour::textObjectBackgroundColourId));
    ioletLockedCol = convertColour(canvasBackgroundColJuce.contrasting(0.5f));

    commentTextCol = convertColour(lnf.findColour(PlugDataColour::commentTextColourId));

    guiObjectInternalOutlineColJuce = lnf.findColour(PlugDataColour::guiObjectInternalOutlineColour);
    guiObjectInternalOutlineCol = convertColour(guiObjectInternalOutlineColJuce);
    guiObjectBackgroundColJuce = lnf.findColour(PlugDataColour::guiObjectBackgroundColourId);
    guiObjectBackgroundCol = convertColour(guiObjectBackgroundColJuce);

    auto const selectedColJuce = lnf.findColour(PlugDataColour::objectSelectedOutlineColourId);
    selectedOutlineCol = convertColour(selectedColJuce);
    transparentObjectBackgroundCol = convertColour(canvasBackgroundColJuce.contrasting(0.35f).withAlpha(0.1f));
    indexTextCol = convertColour(selectedColJuce.contrasting());

    graphAreaCol = convertColour(lnf.findColour(PlugDataColour::graphAreaColourId));

    // Lasso colours
    lassoCol = convertColour(selectedColJuce.withAlpha(0.075f));
    lassoOutlineCol = convertColour(canvasBackgroundColJuce.interpolatedWith(selectedColJuce, 0.65f));

    // Presentation mode colors
    auto const presentationBackgroundColJuce = lnf.findColour(PlugDataColour::presentationBackgroundColourId);
    presentationBackgroundCol = convertColour(presentationBackgroundColJuce);
    presentationWindowOutlineCol = convertColour(presentationBackgroundColJuce.contrasting(0.3f));

    // Connection / Iolet colours
    auto const dataColJuce = lnf.findColour(PlugDataColour::dataColourId);
    dataCol = convertColour(dataColJuce);
    auto const sigColJuce = lnf.findColour(PlugDataColour::signalColourId);
    sigCol = convertColour(sigColJuce);
    auto const gemColJuce = lnf.findColour(PlugDataColour::gemColourId);
    gemCol = convertColour(gemColJuce);
    auto const baseColJuce = lnf.findColour(PlugDataColour::connectionColourId);
    baseCol = convertColour(baseColJuce);

    dataColBrighter = convertColour(dataColJuce.brighter());
    sigColBrighter = convertColour(sigColJuce.brighter());
    gemColBrigher = convertColour(gemColJuce.brighter());
    baseColBrigher = convertColour(baseColJuce.brighter());

    dotsLargeImage.setDirty(); // Make sure bg colour actually gets updated
}

void Canvas::parentHierarchyChanged()
{
    // If the canvas has been added back into the editor, update the look and feel
    // We need to do this because canvases are removed from the parent hierarchy when not visible
    // TODO: consider setting a flag when look and feel actually changes, and read that here
    if (getParentComponent()) {
        sendLookAndFeelChange();
    }
}

void Canvas::updateFramebuffers(NVGcontext* nvg)
{
    auto const pixelScale = editor->getRenderScale();
    auto zoom = getValue<float>(zoomScale);

    constexpr int resizerLogicalSize = 9;
    float const viewScale = pixelScale * zoom;
    int const resizerBufferSize = resizerLogicalSize * viewScale;

    if (resizeHandleImage.needsUpdate(resizerBufferSize, resizerBufferSize)) {
        resizeHandleImage = NVGImage(nvg, resizerBufferSize, resizerBufferSize, [viewScale](Graphics& g) {
            g.addTransform(AffineTransform::scale(viewScale, viewScale));
            auto const b = Rectangle<int>(0, 0, 9, 9);
            // use the path with a hole in it to exclude the inner rounded rect from painting
            Path outerArea;
            outerArea.addRectangle(b);
            outerArea.setUsingNonZeroWinding(false);

            Path innerArea;
            auto const innerRect = b.translated(Object::margin / 2, Object::margin / 2);
            innerArea.addRoundedRectangle(innerRect, Corners::objectCornerRadius);
            outerArea.addPath(innerArea);
            g.reduceClipRegion(outerArea);

            g.setColour(Colours::white); // For alpha image colour isn't important
            g.fillRoundedRectangle(0.0f, 0.0f, 9.0f, 9.0f, Corners::resizeHanleCornerRadius); }, NVGImage::AlphaImage);
        editor->nvgSurface.invalidateAll();
    }

    auto gridLogicalSize = objectGrid.gridSize ? objectGrid.gridSize : 25;
    auto gridSizeCommon = 300;
    auto const gridBufferSize = gridSizeCommon * pixelScale * zoom;

    if (dotsLargeImage.needsUpdate(gridBufferSize, gridBufferSize) || lastObjectGridSize != gridLogicalSize) {
        lastObjectGridSize = gridLogicalSize;

        dotsLargeImage = NVGImage(nvg, gridBufferSize, gridBufferSize, [this, zoom, viewScale, gridLogicalSize, gridSizeCommon](Graphics& g) {
            g.addTransform(AffineTransform::scale(viewScale, viewScale));
            float const ellipseRadius = zoom < 1.0f ? jmap(zoom, 0.25f, 1.0f, 3.0f, 1.0f) : 1.0f;

            int decim = 0;
            switch (gridLogicalSize) {
            case 5:
            case 10:
                if (zoom < 1.0f)
                    decim = 4;
                if (zoom < 0.5f)
                    decim = 6;
                break;
            case 15:
                if (zoom < 1.0f)
                    decim = 4;
                if (zoom < 0.5f)
                    decim = 8;
                break;
            case 20:
            case 25:
                if (zoom < 1.0f)
                    decim = 3;
                if (zoom < 0.5f)
                    decim = 6;
                break;
            case 30:
                if (zoom < 1.0f)
                    decim = 12;
                if (zoom < 0.5f)
                    decim = 12;
                break;
            default: break;
            }

            auto const majorDotColour = canvasMarkingsColJuce.withAlpha(std::min(zoom * 0.8f, 1.0f));

            g.setColour(majorDotColour);
            // Draw ellipses on the grid
            for (int x = 0; x <= gridSizeCommon; x += gridLogicalSize)
            {
                for (int y = 0; y <= gridSizeCommon; y += gridLogicalSize)
                {
                    if (decim != 0) {
                        if (x % decim && y % decim)
                            continue;
                        g.setColour(majorDotColour);
                        if (x % decim == 0 && y % decim == 0)
                            g.setColour(canvasMarkingsColJuce);
                    }
                    // Add half smallest dot offset so the dot isn't at the edge of the texture
                    // We remove this when we position the texture on the canvas
                    float const centerX = static_cast<float>(x) + 2.5f;
                    float const centerY = static_cast<float>(y) + 2.5f;
                    g.fillEllipse(centerX - ellipseRadius, centerY - ellipseRadius, ellipseRadius * 2.0f, ellipseRadius * 2.0f);
                }
            } }, NVGImage::RepeatImage, canvasBackgroundColJuce);
        editor->nvgSurface.invalidateAll();
    }
}

namespace {
struct AnnotationPivotChip {
    juce::String label;
    juce::String prompt;
};

static std::vector<AnnotationPivotChip> getAnnotationPivotChips(const juce::String& text, const juce::String& /*kind*/)
{
    juce::String lower = text.toLowerCase();
    std::vector<AnnotationPivotChip> chips;

    if (lower.contains("cutoff") || lower.contains("filter") || lower.contains("vcf") || lower.contains("lop~") || lower.contains("synth") || lower.contains("core")) {
        chips.push_back({ "Dunkler", "Cutoff tiefer und sanfter filtern" });
        chips.push_back({ "Bissiger", "Mehr Obertöne, Drive und Filter-Bite" });
    } else if (lower.contains("decay") || lower.contains("envelope") || lower.contains("modulator") || lower.contains("vline~") || lower.contains("vca")) {
        chips.push_back({ "Kürzer", "Kürzere und knackigere Hüllkurve" });
        chips.push_back({ "Länger", "Längere Decay-Zeit für schwebenden Tail" });
    } else if (lower.contains("control") || lower.contains("macro") || lower.contains("bay") || lower.contains("freq")) {
        chips.push_back({ "Tief & Fett", "Tiefere Grundfrequenz und fetter Bass" });
        chips.push_back({ "Höher", "Höhere Lage für Melodie und Lead" });
    } else if (lower.contains("drive") || lower.contains("tanh~") || lower.contains("crunch") || lower.contains("grit")) {
        chips.push_back({ "Cleaner", "Weniger Sättigung und Verzerrung" });
        chips.push_back({ "Mehr Grit", "Mehr Drive und analoge Crunch-Sättigung" });
    } else {
        chips.push_back({ "Variieren", "Variiere die Klangfarbe und Parameter dieses Moduls" });
        chips.push_back({ "Extremer", "Extremere Modulation und Charakter" });
    }

    chips.push_back({ "Reply", "__reply__" });
    chips.push_back({ "Undo", "__undo__" });

    return chips;
}

static float getChipWidth(const juce::String& label)
{
    return std::max(46.0f, label.length() * 6.6f + 16.0f);
}
} // namespace

// Callback from canvasViewport to perform actual rendering
void Canvas::performRender(NVGcontext* nvg, Rectangle<int> invalidRegion)
{
    constexpr auto halfSize = infiniteCanvasSize / 2;
    auto const zoom = getValue<float>(zoomScale);
    bool const isLocked = getValue<bool>(locked);
    nvgSave(nvg);

    if (viewport) {
        nvgTranslate(nvg, -viewport->getViewPositionX(), -viewport->getViewPositionY());
        nvgScale(nvg, zoom, zoom);
        invalidRegion = invalidRegion.translated(viewport->getViewPositionX(), viewport->getViewPositionY());
        invalidRegion /= zoom;

        if (isLocked) {
            nvgFillColor(nvg, canvasBackgroundCol);
            nvgFillRect(nvg, invalidRegion.getX(), invalidRegion.getY(), invalidRegion.getWidth(), invalidRegion.getHeight());
        } else {
            nvgBeginPath(nvg);
            nvgRect(nvg, 0, 0, infiniteCanvasSize, infiniteCanvasSize);

            // Use least common multiple of grid sizes: 5,10,15,20,25,30 for texture size for now
            // We repeat the texture on GPU, this is so the texture does not become too small for GPU processing
            // There will be a best fit depending on CPU/GPU calcuations.
            // But currently 300 works well on GPU.
            {
                constexpr auto gridSizeCommon = 300;
                NVGScopedState scopedState(nvg);
                // offset image texture by 2.5f so no dots are on the edge of the texture
                nvgTranslate(nvg, canvasOrigin.x - 2.5f, canvasOrigin.x - 2.5f);

                nvgFillPaint(nvg, nvgImagePattern(nvg, 0, 0, gridSizeCommon, gridSizeCommon, 0, dotsLargeImage.getImageId(), 1));
                nvgFill(nvg);
            }
        }
    }

    currentRenderArea = invalidRegion;

    // PRD overlay: titled regions drawn BEHIND the objects (structure at a glance).
    if (pd && shouldShowAIRegions() && !pd->getMcpRegions().empty()) {
        for (auto const& r : pd->getMcpRegions()) {
            float rx = 0.0f, ry = 0.0f, rw = 0.0f, rh = 0.0f;
            if (r.targetIds.size() > 0) {
                bool first = true;
                int minX = 0, minY = 0, maxX = 0, maxY = 0;
                for (auto const& tid : r.targetIds) {
                    if (auto* obj = findObjectByStableId(tid)) {
                        auto const b = obj->getObjectBounds();
                        if (first) {
                            minX = b.getX();
                            minY = b.getY();
                            maxX = b.getRight();
                            maxY = b.getBottom();
                            first = false;
                        } else {
                            minX = std::min(minX, b.getX());
                            minY = std::min(minY, b.getY());
                            maxX = std::max(maxX, b.getRight());
                            maxY = std::max(maxY, b.getBottom());
                        }
                    }
                }
                if (first) continue; // All member objects deleted -> auto-prune
                constexpr int padX = 18;
                constexpr int padYTop = 48;
                constexpr int padYBottom = 18;
                rx = canvasOrigin.x + static_cast<float>(minX - padX);
                ry = canvasOrigin.y + static_cast<float>(minY - padYTop);
                rw = static_cast<float>(maxX - minX + padX * 2);
                rh = static_cast<float>(maxY - minY + padYTop + padYBottom);
            } else {
                rx = canvasOrigin.x + r.x;
                ry = canvasOrigin.y + r.y;
                rw = r.w;
                rh = r.h;
            }

            NVGScopedState scopedRegion(nvg);
            juce::String const k = r.kind.isNotEmpty() ? r.kind : juce::String("group");
            int ar = 0x8a, ag = 0x93, ab = 0xa6;
            if (k == "source") { ar = 0x3d; ag = 0xdd; ab = 0x8a; }      // Emerald Green
            else if (k == "fx") { ar = 0x4a; ag = 0x9e; ab = 0xff; }     // Electric Cyan
            else if (k == "loop") { ar = 0xff; ag = 0xbe; ab = 0x50; }   // Warm Amber
            else if (k == "output") { ar = 0xff; ag = 0x5a; ab = 0x5a; } // Coral Red

            // 1. Module Chassis Backplate
            nvgBeginPath(nvg);
            nvgRoundedRect(nvg, rx, ry, rw, rh, 8.0f);
            nvgFillColor(nvg, nvgRGBA(ar, ag, ab, 22));
            nvgFill(nvg);
            nvgStrokeColor(nvg, nvgRGBA(ar, ag, ab, 140));
            nvgStrokeWidth(nvg, 1.2f);
            nvgLineStyle(nvg, NVG_LINE_DASHED);
            nvgDashLength(nvg, 6.0f);
            nvgStroke(nvg);
            nvgLineStyle(nvg, NVG_LINE_SOLID);

            // 2. Hardware Module Faceplate Title Badge
            if (r.title.isNotEmpty()) {
                nvgFontSize(nvg, 11.0f);
                nvgFontFace(nvg, "Inter");
                float bounds[4];
                nvgTextBounds(nvg, rx + 10.0f, ry + 6.0f, r.title.toRawUTF8(), nullptr, bounds);
                float const tw = bounds[2] - bounds[0];

                // Dark chassis badge
                nvgBeginPath(nvg);
                nvgRoundedRect(nvg, rx + 8.0f, ry + 5.0f, tw + 22.0f, 18.0f, 4.0f);
                nvgFillColor(nvg, nvgRGBA(16, 18, 22, 230));
                nvgFill(nvg);
                nvgStrokeColor(nvg, nvgRGBA(ar, ag, ab, 100));
                nvgStrokeWidth(nvg, 1.0f);
                nvgStroke(nvg);

                // Status Jewel LED
                nvgBeginPath(nvg);
                nvgCircle(nvg, rx + 16.0f, ry + 14.0f, 2.5f);
                nvgFillColor(nvg, nvgRGBA(ar, ag, ab, 255));
                nvgFill(nvg);

                // Title Text
                nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                nvgFillColor(nvg, nvgRGBA(ar, ag, ab, 245));
                nvgText(nvg, rx + 23.0f, ry + 14.0f, r.title.toRawUTF8(), nullptr);
            }
        }
    }

    // PRD Phase 2: pasted reference images render as glass backdrops behind cords/objects.
    renderReferenceImages(nvg, invalidRegion);
    renderVoiceTakes(nvg, invalidRegion);

    auto drawBorder = [this, nvg, zoom](bool const bg, bool const fg) {
        if (viewport && (showOrigin || showBorder) && !::getValue<bool>(presentationMode)) {
            NVGScopedState scopedState(nvg);
            nvgBeginPath(nvg);

            auto const borderWidth = getValue<float>(patchWidth);
            auto const borderHeight = getValue<float>(patchHeight);
            constexpr auto pos = Point<int>(halfSize, halfSize);

            auto scaledStrokeSize = zoom < 1.0f ? jmap(zoom, 1.0f, 0.25f, 1.5f, 4.0f) : 1.5f;
            if (zoom < 0.3f && editor->getRenderScale() <= 1.0f)
                scaledStrokeSize = jmap(zoom, 0.3f, 0.25f, 4.0f, 8.0f);

            if (bg) {
                nvgBeginPath(nvg);
                nvgMoveTo(nvg, pos.x, pos.y);
                nvgLineTo(nvg, pos.x, pos.y + (showOrigin ? halfSize : borderHeight));
                nvgMoveTo(nvg, pos.x, pos.y);
                nvgLineTo(nvg, pos.x + (showOrigin ? halfSize : borderWidth), pos.y);

                if (showBorder) {
                    nvgMoveTo(nvg, pos.x + borderWidth, pos.y);
                    nvgLineTo(nvg, pos.x + borderWidth, pos.y + borderHeight);
                    nvgLineTo(nvg, pos.x, pos.y + borderHeight);
                }
                nvgLineStyle(nvg, NVG_LINE_SOLID);
                nvgStrokeColor(nvg, canvasBackgroundCol);
                nvgStrokeWidth(nvg, 8.0f);
                nvgStroke(nvg);

                nvgFillColor(nvg, canvasBackgroundCol);
                nvgFillRect(nvg, pos.x - 1.0f, pos.y - 1.0f, 2, 2);
            }

            nvgStrokeColor(nvg, canvasMarkingsCol);
            nvgStrokeWidth(nvg, scaledStrokeSize);
            nvgDashLength(nvg, 8.0f);
            nvgLineStyle(nvg, NVG_LINE_DASHED);

            if (fg) {
                nvgBeginPath(nvg);
                nvgMoveTo(nvg, pos.x, pos.y);
                nvgLineTo(nvg, pos.x, pos.y + (showOrigin ? halfSize : borderHeight));
                nvgStroke(nvg);
                nvgBeginPath(nvg);
                nvgMoveTo(nvg, pos.x, pos.y);
                nvgLineTo(nvg, pos.x + (showOrigin ? halfSize : borderWidth), pos.y);
                nvgStroke(nvg);
            }
            if (showBorder && fg) {
                nvgStrokeWidth(nvg, scaledStrokeSize);
                nvgLineStyle(nvg, NVG_LINE_DASHED);
                nvgBeginPath(nvg);
                nvgMoveTo(nvg, pos.x + borderWidth, pos.y + borderHeight);
                nvgLineTo(nvg, pos.x + borderWidth, pos.y);
                nvgStroke(nvg);
                nvgBeginPath(nvg);
                nvgMoveTo(nvg, pos.x + borderWidth, pos.y + borderHeight);
                nvgLineTo(nvg, pos.x, pos.y + borderHeight);
                nvgStroke(nvg);

                canvasBorderResizer->render(nvg);
            }
        }
    };

    if (!dimensionsAreBeingEdited)
        drawBorder(true, true);
    else
        drawBorder(true, false);

    // Render objects like [drawcurve], [fillcurve] etc. at the back
    for (auto drawable : drawables) {
        if (drawable) {
            auto const* component = dynamic_cast<Component*>(drawable.get());
            if (invalidRegion.intersects(component->getBounds())) {
                drawable->render(nvg);
            }
        }
    }

    if (::getValue<bool>(presentationMode) || isGraph) {
        renderAllObjects(nvg, invalidRegion);
        // render presentation mode as clipped 'virtual' plugin view
        if (::getValue<bool>(presentationMode)) {
            auto const borderWidth = getValue<float>(patchWidth);
            auto const borderHeight = getValue<float>(patchHeight);
            constexpr auto pos = Point<int>(halfSize, halfSize);
            auto const scale = getValue<float>(zoomScale);
            auto const windowCorner = Corners::windowCornerRadius / scale;

            NVGScopedState scopedState(nvg);

            // background colour to crop outside of border area
            nvgBeginPath(nvg);
            nvgRect(nvg, 0, 0, infiniteCanvasSize, infiniteCanvasSize);
            nvgPathWinding(nvg, NVG_HOLE);
            nvgRoundedRect(nvg, pos.getX(), pos.getY(), borderWidth, borderHeight, windowCorner);
            nvgFillColor(nvg, presentationBackgroundCol);
            nvgFill(nvg);

            // background drop shadow to simulate a virtual plugin
            nvgBeginPath(nvg);
            nvgRect(nvg, 0, 0, infiniteCanvasSize, infiniteCanvasSize);
            nvgPathWinding(nvg, NVG_HOLE);
            nvgRoundedRect(nvg, pos.getX(), pos.getY(), borderWidth, borderHeight, windowCorner);

            int const shadowSize = 24 / scale;
            auto borderArea = Rectangle<int>(0, 0, borderWidth, borderHeight).expanded(shadowSize);
            if (presentationShadowImage.needsUpdate(borderArea.getWidth(), borderArea.getHeight())) {
                presentationShadowImage = NVGImage(nvg, borderArea.getWidth(), borderArea.getHeight(), [borderArea, shadowSize, windowCorner](Graphics& g) {
                    auto shadowPath = Path();
                    shadowPath.addRoundedRectangle(borderArea.reduced(shadowSize).withPosition(shadowSize, shadowSize), windowCorner);
                    StackShadow::renderDropShadow(0, g, shadowPath, Colours::white.withAlpha(0.3f), shadowSize, Point<int>(0, 2)); }, NVGImage::AlphaImage);
            }
            auto const shadowImage = nvgImageAlphaPattern(nvg, pos.getX() - shadowSize, pos.getY() - shadowSize, borderArea.getWidth(), borderArea.getHeight(), 0, presentationShadowImage.getImageId(), convertColour(Colours::black));

            nvgStrokeColor(nvg, presentationWindowOutlineCol);
            nvgStrokeWidth(nvg, 0.5f / scale);
            nvgFillPaint(nvg, shadowImage);
            nvgFill(nvg);
            nvgStroke(nvg);
        }
    }
    // render connections infront or behind objects depending on lock mode or overlay setting
    else {
        if (connectionsBehind) {
            renderAllConnections(nvg, invalidRegion);
            renderAllObjects(nvg, invalidRegion);
        } else {
            renderAllObjects(nvg, invalidRegion);
            renderAllConnections(nvg, invalidRegion);
        }
    }

    for (auto* connection : connectionsBeingCreated) {
        NVGScopedState scopedState(nvg);
        connection->render(nvg);
    }

    // PRD overlay: Console Tape (Eurorack Scribble Strip) annotations.
    // Pinned directly above the module strip, bounded to column width (0% horizontal spill).
    if (pd && shouldShowAIAnnotations() && !pd->getMcpAnnotations().empty()) {
        int noteIdx = 0;
        for (auto const& a : pd->getMcpAnnotations()) {
            if (a.text.isEmpty()) continue;
            float rawAx = 0.0f, rawAy = 0.0f, rawLx = 0.0f, rawLy = 0.0f;
            bool hasLead = false;
            juce::Rectangle<int> grpBounds;
            if (!getAnnotationLiveBounds(a.targetId, a.targetIds, a.x, a.y, a.leaderX, a.leaderY, a.hasLeader, rawAx, rawAy, rawLx, rawLy, hasLead, &grpBounds))
                continue;

            auto const tapeBounds = getAnnotationScreenBounds(static_cast<size_t>(noteIdx));
            if (tapeBounds.isEmpty()) continue;

            NVGScopedState scopedAnn(nvg);
            float const ax = tapeBounds.getX();
            float const ay = tapeBounds.getY();
            float const tapeW = tapeBounds.getWidth();
            float const tapeH = tapeBounds.getHeight();

            // Colour by kind so the artist can tell notes apart at a glance.
            juce::String const k = a.kind.isNotEmpty() ? a.kind : juce::String("info");
            int ar = 0x4a, ag = 0x9e, ab = 0xff; // cyan
            if (k == "change") { ar = 0x3d; ag = 0xdd; ab = 0x8a; }      // green
            else if (k == "warn") { ar = 0xff; ag = 0x5a; ab = 0x5a; }   // red
            else if (k == "artist") { ar = 0xff; ag = 0xbe; ab = 0x50; } // amber
            NVGcolor const accent = nvgRGBA(ar, ag, ab, 255);
            bool const isHovered = (noteIdx == mcpNoteHover);
            bool const isCloseHover = (isHovered && mcpNoteHoverClose);

            // 1. Tactile Selection Region: illuminate with soft neon tint on hover
            if (isHovered && !grpBounds.isEmpty()) {
                constexpr int pad = 6;
                float const rx = canvasOrigin.x + static_cast<float>(grpBounds.getX() - pad);
                float const ry = canvasOrigin.y + static_cast<float>(grpBounds.getY() - pad);
                float const rw = static_cast<float>(grpBounds.getWidth() + pad * 2);
                float const rh = static_cast<float>(grpBounds.getHeight() + pad * 2);

                nvgBeginPath(nvg);
                nvgRoundedRect(nvg, rx, ry, rw, rh, 5.0f);
                nvgFillColor(nvg, nvgRGBA(ar, ag, ab, 22));
                nvgFill(nvg);
                nvgStrokeColor(nvg, nvgRGBA(ar, ag, ab, 170));
                nvgStrokeWidth(nvg, 1.2f);
                nvgLineStyle(nvg, NVG_LINE_DASHED);
                nvgDashLength(nvg, 5.0f);
                nvgStroke(nvg);
                nvgLineStyle(nvg, NVG_LINE_SOLID);
            }

            // 2. Hardware Console Tape Anchor: vertical tick connecting tape to module
            if (hasLead && !grpBounds.isEmpty()) {
                float const tickX = ax + 14.0f;
                float const tickY1 = ay + tapeH;
                float const tickY2 = canvasOrigin.y + static_cast<float>(grpBounds.getY());
                nvgBeginPath(nvg);
                nvgMoveTo(nvg, tickX, tickY1);
                nvgLineTo(nvg, tickX, tickY2);
                nvgStrokeColor(nvg, nvgRGBA(ar, ag, ab, isHovered ? 200 : 120));
                nvgStrokeWidth(nvg, 1.0f);
                nvgStroke(nvg);

                // Small anchor rivet dot at module top edge
                nvgBeginPath(nvg);
                nvgCircle(nvg, tickX, tickY2, 2.0f);
                nvgFillColor(nvg, nvgRGBA(ar, ag, ab, isHovered ? 240 : 160));
                nvgFill(nvg);
            }

            // 3. Drop Shadow for depth
            nvgBeginPath(nvg);
            nvgRoundedRect(nvg, ax, ay + 1.0f, tapeW, tapeH, 4.0f);
            nvgFillColor(nvg, nvgRGBA(10, 10, 14, 180));
            nvgFill(nvg);

            // 4. Console Tape Chassis (Matte dark Eurorack scribble strip)
            nvgBeginPath(nvg);
            nvgRoundedRect(nvg, ax, ay, tapeW, tapeH, 3.5f);
            nvgFillColor(nvg, nvgRGBA(20, 21, 26, isHovered ? 250 : 230));
            nvgFill(nvg);
            nvgStrokeColor(nvg, isHovered ? accent : nvgRGBA(ar, ag, ab, 140));
            nvgStrokeWidth(nvg, isHovered ? 1.4f : 1.0f);
            nvgStroke(nvg);

            // 5. Top bevel reflection (milled brushed aluminum edge)
            nvgBeginPath(nvg);
            nvgMoveTo(nvg, ax + 3.0f, ay + 1.0f);
            nvgLineTo(nvg, ax + tapeW - 3.0f, ay + 1.0f);
            nvgStrokeColor(nvg, nvgRGBA(255, 255, 255, isHovered ? 50 : 25));
            nvgStrokeWidth(nvg, 1.0f);
            nvgStroke(nvg);

            // 6. Left accent bar
            nvgBeginPath(nvg);
            nvgRoundedRect(nvg, ax + 2.0f, ay + 2.5f, 2.5f, tapeH - 5.0f, 1.2f);
            nvgFillColor(nvg, accent);
            nvgFill(nvg);

            // 7. Jewel LED Diode on tape
            float const ledX = ax + 14.0f;
            float const ledY = ay + tapeH * 0.5f;

            // Soft glow
            nvgBeginPath(nvg);
            nvgCircle(nvg, ledX, ledY, 5.0f);
            nvgFillColor(nvg, nvgRGBA(ar, ag, ab, isHovered ? 60 : 30));
            nvgFill(nvg);

            // Diode body
            nvgBeginPath(nvg);
            nvgCircle(nvg, ledX, ledY, 2.8f);
            nvgFillColor(nvg, accent);
            nvgFill(nvg);

            // Specular glint
            nvgBeginPath(nvg);
            nvgCircle(nvg, ledX - 0.8f, ledY - 0.8f, 0.8f);
            nvgFillColor(nvg, nvgRGBA(255, 255, 255, 220));
            nvgFill(nvg);

            // 8. Scribble text
            nvgFontSize(nvg, 11.5f);
            nvgFontFace(nvg, "Inter");
            nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

            // Scissor so text NEVER overflows past right action buttons
            float const textLeft = ax + 24.0f;
            float const rightZoneW = 32.0f; // space for sidebar icon + close x
            float const maxTextW = std::max(20.0f, tapeW - 24.0f - rightZoneW);
            nvgScissor(nvg, textLeft, ay, maxTextW, tapeH);
            nvgFillColor(nvg, isHovered ? nvgRGBA(255, 255, 255, 255) : nvgRGBA(225, 228, 238, 230));
            nvgText(nvg, textLeft, ay + tapeH * 0.5f, a.text.toRawUTF8(), nullptr);
            nvgResetScissor(nvg);

            // 9. Right Icons: Notebook link ↗ & Dismiss ✕
            // Sidebar Notebook link icon (subtle ↗ arrow)
            float const iconX = ax + tapeW - 23.0f;
            nvgBeginPath(nvg);
            nvgMoveTo(nvg, iconX - 2.5f, ay + tapeH * 0.5f + 2.5f);
            nvgLineTo(nvg, iconX + 2.5f, ay + tapeH * 0.5f - 2.5f);
            nvgMoveTo(nvg, iconX - 0.5f, ay + tapeH * 0.5f - 2.5f);
            nvgLineTo(nvg, iconX + 2.5f, ay + tapeH * 0.5f - 2.5f);
            nvgLineTo(nvg, iconX + 2.5f, ay + tapeH * 0.5f + 0.5f);
            nvgStrokeColor(nvg, (!isCloseHover && isHovered) ? accent : nvgRGBA(ar, ag, ab, 120));
            nvgStrokeWidth(nvg, 1.1f);
            nvgStroke(nvg);

            // Dismiss 'x'
            float const closeX = ax + tapeW - 9.0f;
            nvgFontSize(nvg, 10.5f);
            nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgFillColor(nvg, isCloseHover ? nvgRGBA(255, 90, 90, 255) : (isHovered ? nvgRGBA(255, 140, 140, 200) : nvgRGBA(ar, ag, ab, 130)));
            nvgText(nvg, closeX, ay + tapeH * 0.5f, "x", nullptr);

            // 10. Pivot Chips Drawer (blooms below hovered tape)
            if (isHovered && mcpNoteEditIndex != noteIdx) {
                auto const chips = getAnnotationPivotChips(a.text, a.kind);
                float totalChipsW = 0.0f;
                for (auto const& c : chips) totalChipsW += getChipWidth(c.label) + 4.0f;
                float const drawerW = std::max(tapeW, totalChipsW + 4.0f);
                float const drawerX = ax;
                float const drawerY = ay + tapeH + 2.0f;
                float const drawerH = 22.0f;

                // Subtle drop shadow
                nvgBeginPath(nvg);
                nvgRoundedRect(nvg, drawerX, drawerY + 1.0f, drawerW, drawerH, 4.0f);
                nvgFillColor(nvg, nvgRGBA(8, 9, 12, 160));
                nvgFill(nvg);

                // Smoked glass chassis with subtle accent stroke
                nvgBeginPath(nvg);
                nvgRoundedRect(nvg, drawerX, drawerY, drawerW, drawerH, 3.5f);
                nvgFillColor(nvg, nvgRGBA(18, 19, 24, 245));
                nvgFill(nvg);
                nvgStrokeColor(nvg, nvgRGBA(ar, ag, ab, 90));
                nvgStrokeWidth(nvg, 1.0f);
                nvgStroke(nvg);

                // Render each chip
                float curChipX = drawerX + 4.0f;
                float const chipY = drawerY + 2.5f;
                float const chipH = drawerH - 5.0f;

                for (size_t ci = 0; ci < chips.size(); ++ci) {
                    auto const& chip = chips[ci];
                    float const cw = getChipWidth(chip.label);
                    bool const isThisChipHovered = (mcpNoteHoverChip == static_cast<int>(ci));

                    NVGcolor bgCol, strkCol, txtCol;
                    if (chip.prompt == "__undo__") {
                        if (isThisChipHovered) {
                            bgCol = nvgRGBA(170, 45, 45, 240);
                            strkCol = nvgRGBA(255, 90, 90, 255);
                            txtCol = nvgRGBA(255, 255, 255, 255);
                        } else {
                            bgCol = nvgRGBA(34, 22, 24, 210);
                            strkCol = nvgRGBA(110, 40, 40, 160);
                            txtCol = nvgRGBA(230, 140, 140, 220);
                        }
                    } else if (chip.prompt == "__reply__") {
                        if (isThisChipHovered) {
                            bgCol = nvgRGBA(45, 115, 215, 240);
                            strkCol = nvgRGBA(90, 175, 255, 255);
                            txtCol = nvgRGBA(255, 255, 255, 255);
                        } else {
                            bgCol = nvgRGBA(24, 32, 46, 210);
                            strkCol = nvgRGBA(55, 95, 150, 160);
                            txtCol = nvgRGBA(155, 200, 255, 220);
                        }
                    } else {
                        // Sound pivot chip
                        if (isThisChipHovered) {
                            bgCol = nvgRGBA(ar, ag, ab, 220);
                            strkCol = accent;
                            txtCol = nvgRGBA(14, 16, 20, 255);
                        } else {
                            bgCol = nvgRGBA(28, 30, 38, 220);
                            strkCol = nvgRGBA(65, 75, 95, 160);
                            txtCol = nvgRGBA(215, 222, 235, 230);
                        }
                    }

                    nvgBeginPath(nvg);
                    nvgRoundedRect(nvg, curChipX, chipY, cw, chipH, 2.5f);
                    nvgFillColor(nvg, bgCol);
                    nvgFill(nvg);
                    nvgStrokeColor(nvg, strkCol);
                    nvgStrokeWidth(nvg, 1.0f);
                    nvgStroke(nvg);

                    nvgFontSize(nvg, 10.0f);
                    nvgFontFace(nvg, "Inter");
                    nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                    nvgFillColor(nvg, txtCol);
                    nvgText(nvg, curChipX + cw * 0.5f, chipY + chipH * 0.5f, chip.label.toRawUTF8(), nullptr);

                    curChipX += cw + 4.0f;
                }
            }

            ++noteIdx;
        }
    }

    // PRD overlay: draw the inline note editor INTO the GPU frame (same trick as
    // Object's text editor) so what the artist types is visible live.
    if (mcpNoteEditor && mcpNoteEditor->isVisible()) {
        NVGScopedState scopedNote(nvg);
        auto const nb = mcpNoteEditor->getBounds();
        nvgTranslate(nvg, static_cast<float>(nb.getX()), static_cast<float>(nb.getY()));
        nvgDrawRoundedRect(nvg, 0, 0, static_cast<float>(nb.getWidth()), static_cast<float>(nb.getHeight()),
                           nvgRGBA(22, 22, 28, 255), nvgRGBA(74, 158, 255, 255), 4.0f);
        mcpNoteRenderer.renderJUCEComponent(nvg, *mcpNoteEditor, zoom * editor->getRenderScale());
    }

    // PRD Copilot: the floating selection prompt pill — glass panel drawn in NVG,
    // live TextEditor composited on top (same trick as the note editor).
    if (mcpSelectionPill && mcpSelectionPill->isVisible()) {
        auto const panel = mcpSelectionPillFrame.isEmpty() ? mcpSelectionPill->getBounds() : mcpSelectionPillFrame;
        float const w = static_cast<float>(panel.getWidth());
        float const h = static_cast<float>(panel.getHeight());
        bool isVoiceRec = false;
        float voiceLvl = 0.0f;
        if (pd) {
            if (auto* br = pd->getMCPBridge()) {
                isVoiceRec = br->isVoiceCapturing();
                voiceLvl = br->getVoiceLiveLevel();
            }
        }

        {
            // TE-lite hardware look: matte dark panel, top bevel, ✦ Lenses, / Tools, ▶ Send.
            NVGScopedState scopedPanel(nvg);
            nvgTranslate(nvg, static_cast<float>(panel.getX()), static_cast<float>(panel.getY()));
            nvgDrawRoundedRect(nvg, 0, 0, w, h, nvgRGBA(24, 26, 30, 250), nvgRGBA(55, 60, 72, 220), 8.0f);

            // Audio-reactive border glow while recording voice / beatbox
            if (isVoiceRec) {
                float const lvl = voiceLvl;
                nvgBeginPath(nvg);
                nvgRoundedRect(nvg, -1.0f, -1.0f, w + 2.0f, h + 2.0f, 9.0f);
                nvgStrokeColor(nvg, nvgRGBA(255, 59, 48, static_cast<unsigned char>(130 + juce::jlimit(0.0f, 125.0f, lvl * 350.0f))));
                nvgStrokeWidth(nvg, 1.5f + lvl * 3.5f);
                nvgStroke(nvg);
            }

            // Subtle top highlight line
            nvgBeginPath(nvg);
            nvgMoveTo(nvg, 8.0f, 1.0f);
            nvgLineTo(nvg, w - 8.0f, 1.0f);
            nvgStrokeColor(nvg, nvgRGBA(255, 255, 255, 25));
            nvgStrokeWidth(nvg, 1.0f);
            nvgStroke(nvg);

            if (mcpPillSketchMode && mcpCompletedStrokes.empty()) {
                // Mood strip: icons first, surfaces second. [✏] [T] [🖼] [🎙 Melody] [🥁 Beat]
                float const btn = 30.0f;
                float const gap = 4.0f;
                float const pad = 6.0f;
                float const by = (h - btn) * 0.5f;
                bool const capturing = pd && pd->getMCPBridge() && pd->getMCPBridge()->isVoiceCapturing();
                int const captureMode = capturing ? pd->getMCPBridge()->getVoiceCaptureMode() : 0;
                bool const melodyRec = capturing && captureMode == 1;
                bool const beatRec = capturing && captureMode == 2;

                for (int i = 0; i < 5; ++i) {
                    float const bx = pad + i * (btn + gap);
                    bool const active = (i == 0 && mcpSketchToolActive) || (i == 1 && mcpTextToolActive) || (i == 3 && melodyRec) || (i == 4 && beatRec);

                    NVGcolor bg = nvgRGBA(38, 42, 50, 230);
                    NVGcolor border = nvgRGBA(160, 175, 200, 100);
                    if (active) {
                        bg = nvgRGBA(0, 190, 230, 245);
                        border = nvgRGBA(0, 245, 255, 255);
                    }
                    nvgDrawRoundedRect(nvg, bx, by, btn, btn, bg, border, 6.0f);

                    float const cx = bx + btn * 0.5f;
                    float const cy = by + btn * 0.5f;
                    NVGcolor const fg = active ? nvgRGBA(12, 14, 18, 255) : nvgRGBA(212, 224, 240, 235);

                    if (i == 0) { // pen
                        nvgBeginPath(nvg);
                        nvgMoveTo(nvg, cx - 5.0f, cy + 5.0f);
                        nvgLineTo(nvg, cx + 3.0f, cy - 5.0f);
                        nvgLineTo(nvg, cx + 5.5f, cy - 2.5f);
                        nvgLineTo(nvg, cx - 2.5f, cy + 5.5f);
                        nvgClosePath(nvg);
                        nvgStrokeColor(nvg, fg);
                        nvgStrokeWidth(nvg, 1.3f);
                        nvgStroke(nvg);
                        nvgBeginPath(nvg);
                        nvgMoveTo(nvg, cx - 5.0f, cy + 5.0f);
                        nvgLineTo(nvg, cx - 6.5f, cy + 6.5f);
                        nvgStroke(nvg);
                    } else if (i == 1) { // text
                        nvgFontFace(nvg, "Inter");
                        nvgFontSize(nvg, 13.0f);
                        nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                        nvgFillColor(nvg, fg);
                        nvgText(nvg, cx, cy, "T", nullptr);
                    } else if (i == 2) { // image
                        nvgBeginPath(nvg);
                        nvgRoundedRect(nvg, cx - 6.0f, cy - 5.0f, 12.0f, 10.0f, 1.8f);
                        nvgStrokeColor(nvg, fg);
                        nvgStrokeWidth(nvg, 1.2f);
                        nvgStroke(nvg);
                        nvgBeginPath(nvg);
                        nvgMoveTo(nvg, cx - 5.0f, cy + 3.0f);
                        nvgLineTo(nvg, cx - 1.0f, cy - 1.0f);
                        nvgLineTo(nvg, cx + 2.0f, cy + 1.5f);
                        nvgLineTo(nvg, cx + 5.0f, cy - 2.0f);
                        nvgStrokeColor(nvg, fg);
                        nvgStrokeWidth(nvg, 1.1f);
                        nvgStroke(nvg);
                    } else if (i == 3) { // melody (beamed notes)
                        nvgBeginPath(nvg);
                        nvgEllipse(nvg, cx - 3.5f, cy + 4.5f, 2.8f, 2.0f);
                        nvgFillColor(nvg, fg);
                        nvgFill(nvg);
                        nvgBeginPath(nvg);
                        nvgEllipse(nvg, cx + 4.0f, cy + 3.0f, 2.8f, 2.0f);
                        nvgFillColor(nvg, fg);
                        nvgFill(nvg);
                        nvgBeginPath(nvg);
                        nvgMoveTo(nvg, cx - 0.9f, cy + 4.4f);
                        nvgLineTo(nvg, cx - 0.9f, cy - 5.5f);
                        nvgLineTo(nvg, cx + 6.6f, cy - 6.8f);
                        nvgLineTo(nvg, cx + 6.6f, cy + 2.9f);
                        nvgStrokeColor(nvg, fg);
                        nvgStrokeWidth(nvg, 1.4f);
                        nvgStroke(nvg);

                        if (melodyRec) { // live level aura
                            float const lvl = pd->getMCPBridge()->getVoiceLiveLevel();
                            nvgBeginPath(nvg);
                            nvgRoundedRect(nvg, bx - 2.0f - lvl * 4.0f, by - 2.0f - lvl * 4.0f,
                                           btn + 4.0f + lvl * 8.0f, btn + 4.0f + lvl * 8.0f, 8.0f);
                            nvgStrokeColor(nvg, nvgRGBA(255, 60, 45, static_cast<unsigned char>(120 + juce::jlimit(0.0f, 130.0f, lvl * 350.0f))));
                            nvgStrokeWidth(nvg, 1.5f);
                            nvgStroke(nvg);
                        }
                    } else { // beat (drum glyph)
                        nvgBeginPath(nvg);
                        nvgRoundedRect(nvg, cx - 6.0f, cy - 0.5f, 12.0f, 6.5f, 2.0f);
                        nvgStrokeColor(nvg, fg);
                        nvgStrokeWidth(nvg, 1.2f);
                        nvgStroke(nvg);
                        nvgBeginPath(nvg);
                        nvgEllipse(nvg, cx, cy - 0.5f, 6.0f, 2.0f);
                        nvgStrokeColor(nvg, fg);
                        nvgStrokeWidth(nvg, 1.2f);
                        nvgStroke(nvg);
                        // crossed beaters
                        nvgBeginPath(nvg);
                        nvgMoveTo(nvg, cx - 4.5f, cy - 7.0f);
                        nvgLineTo(nvg, cx + 1.0f, cy - 1.5f);
                        nvgMoveTo(nvg, cx + 4.5f, cy - 7.0f);
                        nvgLineTo(nvg, cx - 1.0f, cy - 1.5f);
                        nvgStrokeColor(nvg, fg);
                        nvgStrokeWidth(nvg, 1.1f);
                        nvgStroke(nvg);

                        if (beatRec) { // live level aura
                            float const lvl = pd->getMCPBridge()->getVoiceLiveLevel();
                            nvgBeginPath(nvg);
                            nvgRoundedRect(nvg, bx - 2.0f - lvl * 4.0f, by - 2.0f - lvl * 4.0f,
                                           btn + 4.0f + lvl * 8.0f, btn + 4.0f + lvl * 8.0f, 8.0f);
                            nvgStrokeColor(nvg, nvgRGBA(255, 60, 45, static_cast<unsigned char>(120 + juce::jlimit(0.0f, 130.0f, lvl * 350.0f))));
                            nvgStrokeWidth(nvg, 1.5f);
                            nvgStroke(nvg);
                        }
                    }
                }

                // Record slot: declared intent + live level while capturing.
                if (melodyRec || beatRec) {
                    juce::String const recText = melodyRec
                        ? "Sing a melody - tap the note to stop"
                        : "Beatbox - tap the drum to stop";
                    float const boxW = 330.0f;
                    float const boxX = (w - boxW) * 0.5f;
                    float const boxY = -44.0f;
                    nvgDrawRoundedRect(nvg, boxX, boxY, boxW, 32.0f, nvgRGBA(26, 18, 20, 242), nvgRGBA(255, 70, 58, 220), 7.0f);

                    // pulsing red REC dot
                    float const pulse = 0.65f + 0.35f * std::sin(static_cast<float>(juce::Time::getMillisecondCounter()) * 0.008f);
                    float const lvl = juce::jlimit(0.0f, 1.0f, pd->getMCPBridge()->getVoiceLiveLevel() * 3.5f);
                    nvgBeginPath(nvg);
                    nvgCircle(nvg, boxX + 15.0f, boxY + 16.0f, 4.0f);
                    nvgFillColor(nvg, nvgRGBA(255, 60, 45, static_cast<unsigned char>(160 + 95 * pulse)));
                    nvgFill(nvg);

                    // live level meter
                    nvgDrawRoundedRect(nvg, boxX + 26.0f, boxY + 13.0f, 70.0f, 6.0f, nvgRGBA(50, 30, 32, 230), nvgRGBA(90, 50, 52, 120), 3.0f);
                    nvgDrawRoundedRect(nvg, boxX + 26.0f, boxY + 13.0f, 4.0f + lvl * 66.0f, 6.0f, nvgRGBA(255, 70, 55, 235), nvgRGBA(255, 120, 100, 120), 3.0f);

                    nvgFontFace(nvg, "Inter");
                    nvgFontSize(nvg, 10.5f);
                    nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                    nvgFillColor(nvg, nvgRGBA(255, 214, 208, 240));
                    nvgText(nvg, boxX + 104.0f, boxY + 16.0f, recText.toRawUTF8(), nullptr);
                }

                // Hint chip under the strip
                if (mcpMoodHint.isNotEmpty() && juce::Time::getMillisecondCounter() < mcpMoodHintUntil) {
                    nvgFontFace(nvg, "Inter");
                    nvgFontSize(nvg, 11.0f);
                    nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                    float tb[4];
                    nvgTextBounds(nvg, 0.0f, 0.0f, mcpMoodHint.toRawUTF8(), nullptr, tb);
                    float const hw = (tb[2] - tb[0]) + 16.0f;
                    nvgDrawRoundedRect(nvg, 0.0f, h + 6.0f, hw, 20.0f, nvgRGBA(16, 18, 22, 232), nvgRGBA(255, 190, 80, 150), 5.0f);
                    nvgFillColor(nvg, nvgRGBA(255, 214, 150, 240));
                    nvgText(nvg, 8.0f, h + 16.0f, mcpMoodHint.toRawUTF8(), nullptr);
                }
            } else if (mcpPillSketchMode) {
                // Moodboard Chisel: [⚡ Build] | prompt | [T] [🖼] [↩ Undo] [🗑 Clear] [✏️ pen]
                float const chipH = 22.0f;
                float const chipY = (h - chipH) * 0.5f;

                // ⚡ Build chip — the commit action (accent blue)
                {
                    float const bx = 6.0f;
                    float const bw = 80.0f;
                    nvgDrawRoundedRect(nvg, bx, chipY, bw, chipH, nvgRGBA(32, 108, 214, 245), nvgRGBA(96, 178, 255, 255), 5.0f);
                    // vector lightning bolt
                    nvgBeginPath(nvg);
                    nvgMoveTo(nvg, bx + 15.0f, chipY + 3.5f);
                    nvgLineTo(nvg, bx + 10.0f, chipY + 11.5f);
                    nvgLineTo(nvg, bx + 14.0f, chipY + 11.5f);
                    nvgLineTo(nvg, bx + 12.0f, chipY + 18.5f);
                    nvgLineTo(nvg, bx + 19.0f, chipY + 9.5f);
                    nvgLineTo(nvg, bx + 14.5f, chipY + 9.5f);
                    nvgLineTo(nvg, bx + 17.0f, chipY + 3.5f);
                    nvgClosePath(nvg);
                    nvgFillColor(nvg, nvgRGBA(255, 235, 130, 255));
                    nvgFill(nvg);
                    nvgFontFace(nvg, "Inter");
                    nvgFontSize(nvg, 11.5f);
                    nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                    nvgFillColor(nvg, nvgRGBA(255, 255, 255, 255));
                    nvgText(nvg, bx + 25.0f, chipY + chipH * 0.5f, "Build", nullptr);
                }

                // T attach-text chip (same tools as the surgical lane)
                {
                    float const tx = w - 234.0f;
                    float const tw2 = 24.0f;
                    nvgDrawRoundedRect(nvg, tx, chipY, tw2, chipH,
                                       mcpTextToolActive ? nvgRGBA(74, 158, 255, 240) : nvgRGBA(38, 42, 50, 230),
                                       mcpTextToolActive ? nvgRGBA(140, 195, 255, 255) : nvgRGBA(160, 175, 200, 110), 4.0f);
                    nvgFontFace(nvg, "Inter");
                    nvgFontSize(nvg, 12.0f);
                    nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                    nvgFillColor(nvg, mcpTextToolActive ? nvgRGBA(255, 255, 255, 255) : nvgRGBA(215, 225, 240, 235));
                    nvgText(nvg, tx + tw2 * 0.5f, chipY + chipH * 0.5f, "T", nullptr);
                }

                // 🖼 attach-image chip (pastes the clipboard onto the mood/sketch)
                {
                    float const ix = w - 206.0f;
                    float const iw = 24.0f;
                    float const icx = ix + iw * 0.5f;
                    float const icy = chipY + chipH * 0.5f;
                    nvgDrawRoundedRect(nvg, ix, chipY, iw, chipH, nvgRGBA(38, 42, 50, 230), nvgRGBA(160, 175, 200, 110), 4.0f);
                    nvgBeginPath(nvg);
                    nvgRoundedRect(nvg, icx - 5.0f, icy - 4.0f, 10.0f, 8.5f, 1.5f);
                    nvgStrokeColor(nvg, nvgRGBA(200, 215, 235, 220));
                    nvgStrokeWidth(nvg, 1.1f);
                    nvgStroke(nvg);
                    nvgBeginPath(nvg);
                    nvgMoveTo(nvg, icx - 4.0f, icy + 2.5f);
                    nvgLineTo(nvg, icx - 1.0f, icy - 0.5f);
                    nvgLineTo(nvg, icx + 1.5f, icy + 1.5f);
                    nvgLineTo(nvg, icx + 4.0f, icy + 0.5f);
                    nvgStrokeColor(nvg, nvgRGBA(160, 230, 200, 200));
                    nvgStrokeWidth(nvg, 1.0f);
                    nvgStroke(nvg);
                }

                // ↩ Undo chip
                {
                    float const ux = w - 178.0f;
                    float const uw = 70.0f;
                    float const ucy = chipY + chipH * 0.5f;
                    nvgDrawRoundedRect(nvg, ux, chipY, uw, chipH, nvgRGBA(38, 42, 50, 235), nvgRGBA(160, 175, 200, 110), 4.0f);
                    // back-arrow glyph
                    nvgBeginPath(nvg);
                    nvgMoveTo(nvg, ux + 18.0f, ucy);
                    nvgLineTo(nvg, ux + 10.0f, ucy);
                    nvgMoveTo(nvg, ux + 13.5f, ucy - 3.0f);
                    nvgLineTo(nvg, ux + 10.0f, ucy);
                    nvgLineTo(nvg, ux + 13.5f, ucy + 3.0f);
                    nvgStrokeColor(nvg, nvgRGBA(150, 200, 255, 235));
                    nvgStrokeWidth(nvg, 1.4f);
                    nvgStroke(nvg);
                    nvgFontFace(nvg, "Inter");
                    nvgFontSize(nvg, 10.5f);
                    nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                    nvgFillColor(nvg, nvgRGBA(215, 225, 240, 235));
                    nvgText(nvg, ux + 23.0f, ucy, "Undo", nullptr);
                }

                // 🗑 Clear chip
                {
                    float const cxX = w - 104.0f;
                    float const cxW = 70.0f;
                    float const ccy = chipY + chipH * 0.5f;
                    nvgDrawRoundedRect(nvg, cxX, chipY, cxW, chipH, nvgRGBA(58, 30, 34, 235), nvgRGBA(220, 110, 110, 140), 4.0f);
                    // trash glyph
                    float const tcx = cxX + 14.0f;
                    nvgBeginPath(nvg);
                    nvgRoundedRect(nvg, tcx - 3.5f, ccy - 3.0f, 7.0f, 8.5f, 1.5f);
                    nvgStrokeColor(nvg, nvgRGBA(255, 175, 175, 240));
                    nvgStrokeWidth(nvg, 1.3f);
                    nvgStroke(nvg);
                    nvgBeginPath(nvg);
                    nvgMoveTo(nvg, tcx - 5.0f, ccy - 4.5f);
                    nvgLineTo(nvg, tcx + 5.0f, ccy - 4.5f);
                    nvgStrokeColor(nvg, nvgRGBA(255, 175, 175, 240));
                    nvgStrokeWidth(nvg, 1.3f);
                    nvgStroke(nvg);
                    nvgFontFace(nvg, "Inter");
                    nvgFontSize(nvg, 10.5f);
                    nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                    nvgFillColor(nvg, nvgRGBA(255, 200, 200, 240));
                    nvgText(nvg, cxX + 24.0f, ccy, "Clear", nullptr);
                }

                // ✏️ Pen toggle — put the pen down / pick it back up without knowing Esc
                {
                    float const px = w - 30.0f;
                    float const pw = 24.0f;
                    float const pcy = chipY + chipH * 0.5f;
                    if (mcpSketchToolActive) {
                        nvgDrawRoundedRect(nvg, px, chipY, pw, chipH, nvgRGBA(0, 180, 220, 240), nvgRGBA(0, 245, 255, 255), 4.0f);
                    } else {
                        nvgDrawRoundedRect(nvg, px, chipY, pw, chipH, nvgRGBA(38, 42, 50, 230), nvgRGBA(160, 175, 200, 80), 4.0f);
                    }
                    float const pcx = px + pw * 0.5f;
                    nvgBeginPath(nvg);
                    nvgMoveTo(nvg, pcx - 4.5f, pcy + 4.5f);
                    nvgLineTo(nvg, pcx + 2.5f, pcy - 4.5f);
                    nvgLineTo(nvg, pcx + 5.0f, pcy - 2.0f);
                    nvgLineTo(nvg, pcx - 2.0f, pcy + 5.0f);
                    nvgClosePath(nvg);
                    nvgStrokeColor(nvg, mcpSketchToolActive ? nvgRGBA(255, 255, 255, 255) : nvgRGBA(200, 215, 235, 220));
                    nvgStrokeWidth(nvg, 1.2f);
                    nvgStroke(nvg);
                    nvgBeginPath(nvg);
                    nvgMoveTo(nvg, pcx - 4.5f, pcy + 4.5f);
                    nvgLineTo(nvg, pcx - 6.0f, pcy + 6.0f);
                    nvgStrokeColor(nvg, mcpSketchToolActive ? nvgRGBA(255, 255, 255, 255) : nvgRGBA(0, 230, 255, 220));
                    nvgStrokeWidth(nvg, 1.4f);
                    nvgStroke(nvg);
                }
            } else {
            // Left: + Add Context & Actions button (Antigravity vector plus!)
            float const lw = 24.0f;
            float const lh = 20.0f;
            float const lx = 6.0f;
            float const ly = (h - lh) * 0.5f;
            bool const isPlusActive = (mcpPillDrawerMode == 3);
            nvgDrawRoundedRect(nvg, lx, ly, lw, lh,
                               isPlusActive ? nvgRGBA(74, 158, 255, 80) : nvgRGBA(38, 42, 50, 230),
                               isPlusActive ? nvgRGBA(74, 158, 255, 240) : nvgRGBA(160, 175, 200, 100), 4.0f);
            float const cx = lx + lw * 0.5f;
            float const cy = ly + lh * 0.5f;
            nvgBeginPath(nvg);
            nvgMoveTo(nvg, cx - 4.5f, cy);
            nvgLineTo(nvg, cx + 4.5f, cy);
            nvgMoveTo(nvg, cx, cy - 4.5f);
            nvgLineTo(nvg, cx, cy + 4.5f);
            nvgStrokeColor(nvg, isPlusActive ? nvgRGBA(74, 180, 255, 255) : nvgRGBA(220, 230, 245, 230));
            nvgStrokeWidth(nvg, 1.6f);
            nvgStroke(nvg);

            // Surgical attachment chips: [✏ attach-sketch] [T attach-text] [🖼 attach-image]
            // Same tools as the Studio lane, but bound to the selected object.
            {
                float const chipW = 24.0f;
                float const chipH = 20.0f;
                float const chipY = (h - chipH) * 0.5f;
                struct AttachChip { float x; };
                float const sketchX = w - 144.0f;
                float const textX = w - 116.0f;
                float const imageX = w - 88.0f;

                bool const sketchArmed = mcpSketchToolActive;
                bool const textArmed = mcpTextToolActive;

                // ✏ attach-sketch
                nvgDrawRoundedRect(nvg, sketchX, chipY, chipW, chipH,
                                   sketchArmed ? nvgRGBA(0, 180, 220, 240) : nvgRGBA(38, 42, 50, 230),
                                   sketchArmed ? nvgRGBA(0, 245, 255, 255) : nvgRGBA(160, 175, 200, 80), 4.0f);
                {
                    float const pcx = sketchX + chipW * 0.5f;
                    float const pcy = chipY + chipH * 0.5f;
                    nvgBeginPath(nvg);
                    nvgMoveTo(nvg, pcx - 4.0f, pcy + 4.0f);
                    nvgLineTo(nvg, pcx + 2.5f, pcy - 4.5f);
                    nvgLineTo(nvg, pcx + 4.5f, pcy - 2.0f);
                    nvgLineTo(nvg, pcx - 2.0f, pcy + 5.0f);
                    nvgClosePath(nvg);
                    nvgStrokeColor(nvg, sketchArmed ? nvgRGBA(255, 255, 255, 255) : nvgRGBA(200, 215, 235, 220));
                    nvgStrokeWidth(nvg, 1.2f);
                    nvgStroke(nvg);
                }

                // T attach-text
                nvgDrawRoundedRect(nvg, textX, chipY, chipW, chipH,
                                   textArmed ? nvgRGBA(74, 158, 255, 240) : nvgRGBA(38, 42, 50, 230),
                                   textArmed ? nvgRGBA(140, 195, 255, 255) : nvgRGBA(160, 175, 200, 80), 4.0f);
                nvgFontFace(nvg, "Inter");
                nvgFontSize(nvg, 12.0f);
                nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                nvgFillColor(nvg, textArmed ? nvgRGBA(255, 255, 255, 255) : nvgRGBA(200, 215, 235, 220));
                nvgText(nvg, textX + chipW * 0.5f, chipY + chipH * 0.5f, "T", nullptr);

                // 🖼 attach-image (picture frame glyph)
                nvgDrawRoundedRect(nvg, imageX, chipY, chipW, chipH, nvgRGBA(38, 42, 50, 230), nvgRGBA(160, 175, 200, 80), 4.0f);
                {
                    float const icx = imageX + chipW * 0.5f;
                    float const icy = chipY + chipH * 0.5f;
                    nvgBeginPath(nvg);
                    nvgRoundedRect(nvg, icx - 5.0f, icy - 4.0f, 10.0f, 8.5f, 1.5f);
                    nvgStrokeColor(nvg, nvgRGBA(200, 215, 235, 220));
                    nvgStrokeWidth(nvg, 1.1f);
                    nvgStroke(nvg);
                    nvgBeginPath(nvg);
                    nvgMoveTo(nvg, icx - 4.0f, icy + 2.5f);
                    nvgLineTo(nvg, icx - 1.0f, icy - 0.5f);
                    nvgLineTo(nvg, icx + 1.5f, icy + 1.5f);
                    nvgLineTo(nvg, icx + 4.0f, icy + 0.5f);
                    nvgStrokeColor(nvg, nvgRGBA(160, 230, 200, 200));
                    nvgStrokeWidth(nvg, 1.0f);
                    nvgStroke(nvg);
                }
            }

            // Right: ✏️ Pen / Sketch button
            float const pw = 24.0f;
            float const ph = 20.0f;
            float const px = w - 58.0f;
            float const py = (h - ph) * 0.5f;

            if (mcpSketchToolActive) {
                nvgDrawRoundedRect(nvg, px, py, pw, ph, nvgRGBA(0, 180, 220, 240), nvgRGBA(0, 245, 255, 255), 4.0f);
            } else {
                nvgDrawRoundedRect(nvg, px, py, pw, ph, nvgRGBA(38, 42, 50, 230), nvgRGBA(160, 175, 200, 80), 4.0f);
            }

            float const pcx = px + pw * 0.5f;
            float const pcy = py + ph * 0.5f;
            nvgBeginPath(nvg);
            nvgMoveTo(nvg, pcx - 4.5f, pcy + 4.5f);
            nvgLineTo(nvg, pcx + 2.5f, pcy - 4.5f);
            nvgLineTo(nvg, pcx + 5.0f, pcy - 2.0f);
            nvgLineTo(nvg, pcx - 2.0f, pcy + 5.0f);
            nvgClosePath(nvg);
            nvgStrokeColor(nvg, mcpSketchToolActive ? nvgRGBA(255, 255, 255, 255) : nvgRGBA(200, 215, 235, 220));
            nvgStrokeWidth(nvg, 1.2f);
            nvgStroke(nvg);
            nvgBeginPath(nvg);
            nvgMoveTo(nvg, pcx - 4.5f, pcy + 4.5f);
            nvgLineTo(nvg, pcx - 6.0f, pcy + 6.0f);
            nvgStrokeColor(nvg, mcpSketchToolActive ? nvgRGBA(255, 255, 255, 255) : nvgRGBA(0, 230, 255, 220));
            nvgStrokeWidth(nvg, 1.4f);
            nvgStroke(nvg);

            // Right: 🎙️ Mic / Voice button
            float const mw = 24.0f;
            float const mh = 20.0f;
            float const mx = w - 30.0f;
            float const my = (h - mh) * 0.5f;

            if (isVoiceRec) {
                float const pulse = 0.8f + 0.2f * std::sin(static_cast<float>(juce::Time::getMillisecondCounter()) * 0.008f);
                float const levelExp = juce::jlimit(0.0f, 1.0f, voiceLvl * 3.5f);
                // Glowing outer aura reacting to live mic input level
                nvgDrawRoundedRect(nvg, mx - 2.0f - levelExp * 3.0f, my - 2.0f - levelExp * 3.0f,
                                   mw + 4.0f + levelExp * 6.0f, mh + 4.0f + levelExp * 6.0f,
                                   nvgRGBA(255, 45, 35, static_cast<int>(50 * pulse + levelExp * 100)),
                                   nvgRGBA(255, 60, 45, static_cast<int>(180 * pulse)), 5.0f);
                nvgDrawRoundedRect(nvg, mx, my, mw, mh, nvgRGBA(255, 50, 35, 240), nvgRGBA(255, 140, 120, 255), 4.0f);
            } else {
                nvgDrawRoundedRect(nvg, mx, my, mw, mh, nvgRGBA(38, 42, 50, 230), nvgRGBA(160, 175, 200, 80), 4.0f);
            }

            // Crisp vector microphone icon
            float const mcx = mx + mw * 0.5f;
            float const mcy = my + mh * 0.5f - 1.0f;
            nvgBeginPath(nvg);
            nvgRoundedRect(nvg, mcx - 2.5f, mcy - 5.0f, 5.0f, 8.0f, 2.5f);
            nvgFillColor(nvg, isVoiceRec ? nvgRGBA(255, 255, 255, 255) : nvgRGBA(200, 215, 235, 220));
            nvgFill(nvg);
            nvgBeginPath(nvg);
            nvgArc(nvg, mcx, mcy, 4.5f, 0.0f, juce::MathConstants<float>::pi, NVG_HOLE);
            nvgStrokeColor(nvg, isVoiceRec ? nvgRGBA(255, 255, 255, 255) : nvgRGBA(200, 215, 235, 220));
            nvgStrokeWidth(nvg, 1.2f);
            nvgStroke(nvg);
            nvgBeginPath(nvg);
            nvgMoveTo(nvg, mcx, mcy + 4.5f);
            nvgLineTo(nvg, mcx, mcy + 7.5f);
            nvgMoveTo(nvg, mcx - 3.0f, mcy + 7.5f);
            nvgLineTo(nvg, mcx + 3.0f, mcy + 7.5f);
            nvgStrokeColor(nvg, isVoiceRec ? nvgRGBA(255, 255, 255, 255) : nvgRGBA(200, 215, 235, 220));
            nvgStrokeWidth(nvg, 1.2f);
            nvgStroke(nvg);
            } // else: object-pill controls (+ / pen / mic)
        }

        // Ambient blinking caret when unfocused & empty to clearly signal interactive input affordance
        if (mcpSelectionPill && !mcpSelectionPill->hasKeyboardFocus(true) && mcpSelectionPill->getText().isEmpty()
            && !(mcpPillSketchMode && mcpCompletedStrokes.empty())) {
            bool const caretBlink = ((juce::Time::getMillisecondCounter() / 530) % 2) == 0;
            if (caretBlink) {
                NVGScopedState scopedCaret(nvg);
                float const caretX = panel.getX() + (mcpPillSketchMode ? 96.0f : 38.0f);
                float const caretY = panel.getY() + panel.getHeight() * 0.5f;
                nvgBeginPath(nvg);
                nvgMoveTo(nvg, caretX, caretY - 7.0f);
                nvgLineTo(nvg, caretX, caretY + 7.0f);
                nvgStrokeColor(nvg, nvgRGBA(74, 158, 255, 230));
                nvgStrokeWidth(nvg, 1.5f);
                nvgStroke(nvg);
            }
        }

        // Crisp on-canvas placeholder when empty (subtle dim when focused)
        if (mcpSelectionPill && mcpSelectionPill->getText().isEmpty()
            && !(mcpPillSketchMode && mcpCompletedStrokes.empty())) {
            NVGScopedState scopedHint(nvg);
            nvgFontFace(nvg, "Inter");
            nvgFontSize(nvg, 12.0f);
            nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            juce::String hint;
            if (mcpPillSketchMode) {
                hint = mcpCompletedStrokes.empty()
                    ? "New Mood — paint, paste, type... then Build"
                    : "Describe the build, or hit Build...";
                nvgFillColor(nvg, nvgRGBA(140, 155, 175, mcpSelectionPill->hasKeyboardFocus(true) ? 90 : 170));
            } else if (isVoiceRec) {
                hint = "Listening... (speak prompt or beatbox groove)";
                nvgFillColor(nvg, nvgRGBA(255, 110, 95, 240));
            } else if (mcpActiveLens.isNotEmpty()) {
                hint = "[" + mcpActiveLens.toUpperCase() + "] Ask AI on selection...";
                nvgFillColor(nvg, nvgRGBA(140, 155, 175, mcpSelectionPill->hasKeyboardFocus(true) ? 90 : 170));
            } else {
                hint = "Ask AI on selection...  (/ skills & actions)";
                nvgFillColor(nvg, nvgRGBA(140, 155, 175, mcpSelectionPill->hasKeyboardFocus(true) ? 90 : 170));
            }
            nvgText(nvg, panel.getX() + (mcpPillSketchMode ? 96.0f : 38.0f), panel.getY() + panel.getHeight() * 0.5f, hint.toRawUTF8(), nullptr);
        }

        {
            NVGScopedState scopedField(nvg);
            auto const fb = mcpSelectionPill->getBounds();
            nvgTranslate(nvg, static_cast<float>(fb.getX()), static_cast<float>(fb.getY()));
            mcpSelectionPillRenderer.renderJUCEComponent(nvg, *mcpSelectionPill, zoom * editor->getRenderScale());
        }

        // Native Horizontal Card Drawer or Smart Command Palette
        if (mcpPillDrawerMode != 0) {
            if (mcpPillDrawerMode == 3) {
                const_cast<Canvas*>(this)->initPaletteItems();
                float const pw = std::max(static_cast<float>(panel.getWidth()), 390.0f);
                float const rowH = 30.0f;
                int const totalCount = static_cast<int>(mcpFilteredPaletteIndices.size());
                int const visibleCount = std::min(8, totalCount);
                float const ph = 28.0f + visibleCount * rowH + 6.0f;
                float const px = static_cast<float>(panel.getX());
                float py = static_cast<float>(panel.getY() + panel.getHeight()) + 5.0f;
                if (py + ph > static_cast<float>(getHeight()) - 20.0f) {
                    py = static_cast<float>(panel.getY()) - ph - 5.0f;
                }
                const_cast<Canvas*>(this)->mcpPaletteFrame = juce::Rectangle<int>(roundToInt(px), roundToInt(py), roundToInt(pw), roundToInt(ph));

                NVGScopedState scopedPalette(nvg);
                nvgTranslate(nvg, px, py);
                // Frosted dark card container
                nvgDrawRoundedRect(nvg, 0, 0, pw, ph, nvgRGBA(16, 18, 22, 252), nvgRGBA(55, 62, 75, 240), 7.0f);

                // Subtle top highlight
                nvgBeginPath(nvg);
                nvgMoveTo(nvg, 6.0f, 1.0f);
                nvgLineTo(nvg, pw - 6.0f, 1.0f);
                nvgStrokeColor(nvg, nvgRGBA(255, 255, 255, 25));
                nvgStrokeWidth(nvg, 1.0f);
                nvgStroke(nvg);

                // Header title
                nvgFontFace(nvg, "Inter");
                nvgFontSize(nvg, 10.0f);
                nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                nvgFillColor(nvg, nvgRGBA(140, 155, 180, 220));
                nvgText(nvg, 12.0f, 14.0f, "ADD CONTEXT & ACTIONS (+)", nullptr);

                for (int i = 0; i < visibleCount; ++i) {
                    int const itemIdx = mcpFilteredPaletteIndices[static_cast<size_t>(i)];
                    auto const& item = mcpPaletteItems[static_cast<size_t>(itemIdx)];
                    float const ry = 26.0f + i * rowH;
                    bool const isSelected = (i == mcpPaletteSelectedIndex);

                    if (isSelected) {
                        nvgDrawRoundedRect(nvg, 4.0f, ry, pw - 8.0f, rowH - 2.0f,
                                           nvgRGBA(74, 158, 255, 55), nvgRGBA(74, 158, 255, 210), 4.0f);
                    }

                    // Icon / Prefix
                    nvgFontSize(nvg, 11.5f);
                    nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                    nvgFillColor(nvg, isSelected ? nvgRGBA(74, 180, 255, 255) : nvgRGBA(150, 165, 190, 220));
                    nvgText(nvg, 18.0f, ry + (rowH - 2.0f) * 0.5f, item.icon.toRawUTF8(), nullptr);

                    // Category badge chip (Action, Skill, Lens)
                    float const chipW = 42.0f;
                    float const chipH = 14.0f;
                    float const chipX = 30.0f;
                    float const chipY = ry + (rowH - 2.0f - chipH) * 0.5f;
                    NVGcolor chipBg = nvgRGBA(40, 48, 62, 200);
                    NVGcolor chipFg = nvgRGBA(160, 180, 210, 220);
                    if (item.category == "Action") {
                        chipBg = nvgRGBA(30, 90, 60, 150);
                        chipFg = nvgRGBA(100, 240, 160, 240);
                    } else if (item.category == "Skill") {
                        chipBg = nvgRGBA(90, 40, 110, 150);
                        chipFg = nvgRGBA(220, 140, 255, 240);
                    } else if (item.category == "Lens") {
                        chipBg = nvgRGBA(30, 70, 110, 150);
                        chipFg = nvgRGBA(100, 190, 255, 240);
                    }
                    nvgDrawRoundedRect(nvg, chipX, chipY, chipW, chipH, chipBg, chipFg, 3.0f);
                    nvgFontSize(nvg, 8.5f);
                    nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                    nvgFillColor(nvg, chipFg);
                    nvgText(nvg, chipX + chipW * 0.5f, chipY + chipH * 0.5f, item.category.toRawUTF8(), nullptr);

                    // Title
                    nvgFontSize(nvg, 11.0f);
                    nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                    nvgFillColor(nvg, isSelected ? nvgRGBA(255, 255, 255, 255) : nvgRGBA(215, 225, 240, 230));
                    nvgText(nvg, chipX + chipW + 8.0f, ry + (rowH - 2.0f) * 0.5f, item.title.toRawUTF8(), nullptr);

                    // Description (dimmed)
                    nvgFontSize(nvg, 10.0f);
                    nvgFillColor(nvg, isSelected ? nvgRGBA(180, 205, 240, 240) : nvgRGBA(120, 135, 155, 200));
                    nvgText(nvg, chipX + chipW + 130.0f, ry + (rowH - 2.0f) * 0.5f, item.desc.toRawUTF8(), nullptr);
                }
            } else {
                float const dw = w;
                float const dh = 30.0f;
                float const dx = static_cast<float>(panel.getX());
                float const dy = static_cast<float>(panel.getY() + panel.getHeight()) + 5.0f;
                mcpPillDrawerFrame = juce::Rectangle<int>(roundToInt(dx), roundToInt(dy), roundToInt(dw), roundToInt(dh));

                NVGScopedState scopedDrawer(nvg);
                nvgTranslate(nvg, dx, dy);
                // Frosted dark card background with soft border
                nvgDrawRoundedRect(nvg, 0, 0, dw, dh, nvgRGBA(18, 20, 24, 250), nvgRGBA(55, 62, 75, 230), 6.0f);

                // Subtle top highlight line
                nvgBeginPath(nvg);
                nvgMoveTo(nvg, 6.0f, 1.0f);
                nvgLineTo(nvg, dw - 6.0f, 1.0f);
                nvgStrokeColor(nvg, nvgRGBA(255, 255, 255, 20));
                nvgStrokeWidth(nvg, 1.0f);
                nvgStroke(nvg);

                nvgFontFace(nvg, "Inter");
                nvgFontSize(nvg, 11.5f);
                nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

                if (mcpPillDrawerMode == 1) {
                    // Lenses: Doctor | Jam | Genesis | Modular | Free
                    const char* lenses[] = { "Doctor", "Jam", "Genesis", "Modular", "Free" };
                    const char* lensKeys[] = { "doctor", "jam", "genesis", "modular", "free" };
                    float const itemW = (dw - 8.0f) / 5.0f;
                    for (int i = 0; i < 5; ++i) {
                        float const ix = 4.0f + i * itemW;
                        float const iy = 3.0f;
                        float const iw = itemW - 3.0f;
                        float const ih = dh - 6.0f;
                        bool const isActive = (mcpActiveLens == lensKeys[i]) || (mcpActiveLens.isEmpty() && i == 4);
                        if (isActive) {
                            nvgDrawRoundedRect(nvg, ix, iy, iw, ih, nvgRGBA(255, 102, 0, 235), nvgRGBA(255, 150, 60, 255), 4.0f);
                            nvgFillColor(nvg, nvgRGBA(18, 18, 20, 255));
                        } else {
                            nvgDrawRoundedRect(nvg, ix, iy, iw, ih, nvgRGBA(30, 34, 42, 220), nvgRGBA(50, 58, 70, 180), 4.0f);
                            nvgFillColor(nvg, nvgRGBA(210, 220, 235, 230));
                        }
                        nvgText(nvg, ix + iw * 0.5f, iy + ih * 0.5f, lenses[i], nullptr);
                    }
                } else if (mcpPillDrawerMode == 2) {
                    // Tools: Explain | Drive | Filter | Reverb | Pack GOP | Tidy
                    const char* tools[] = { "Explain", "Drive", "Filter", "Reverb", "Pack GOP", "Tidy" };
                    float const itemW = (dw - 8.0f) / 6.0f;
                    for (int i = 0; i < 6; ++i) {
                        float const ix = 4.0f + i * itemW;
                        float const iy = 3.0f;
                        float const iw = itemW - 3.0f;
                        float const ih = dh - 6.0f;
                        nvgDrawRoundedRect(nvg, ix, iy, iw, ih, nvgRGBA(30, 34, 42, 220), nvgRGBA(50, 58, 70, 180), 4.0f);
                        nvgFillColor(nvg, nvgRGBA(210, 220, 235, 230));
                        nvgText(nvg, ix + iw * 0.5f, iy + ih * 0.5f, tools[i], nullptr);
                    }
                }
            }
        } else {
            mcpPillDrawerFrame = {};
            mcpPaletteFrame = {};
        }
    }

    // Tier-2 approval chips — a destructive op waits for the artist's yes.
    // Drawn in CANVAS coords (same space as the pill) so clicks hit-test free.
    if (isApprovalRequested()) {
        juce::Rectangle<int> anchor;
        if (mcpSelectionPill && mcpSelectionPill->isVisible() && !mcpSelectionPillFrame.isEmpty()) {
            anchor = mcpSelectionPillFrame;
        } else {
            auto const sel = getSelectionOfType<Object>();
            juce::Rectangle<int> b;
            bool first = true;
            for (auto* o : sel) {
                if (!o) continue;
                auto const ob = o->getBounds();
                b = first ? ob : b.getUnion(ob);
                first = false;
            }
            if (!first) anchor = b;
            if (anchor.isEmpty())
                anchor = juce::Rectangle<int>(canvasOrigin.x - 210, canvasOrigin.y + 40, 420, 1);
        }
        constexpr int barW = 420;
        constexpr int barH = 46;
        int const bx = anchor.getCentreX() - barW / 2;
        int const by = anchor.getBottom() + 14;
        mcpApprovalFrame = juce::Rectangle<int>(bx, by, barW, barH);
        int const half = (barW - 24) / 2;
        mcpApprovalYes = juce::Rectangle<int>(bx + 8, by + 8, half, barH - 16);
        mcpApprovalNo = juce::Rectangle<int>(bx + 8 + half + 8, by + 8, half, barH - 16);

        NVGScopedState scopedAppr(nvg);
        nvgTranslate(nvg, static_cast<float>(bx), static_cast<float>(by));
        nvgDrawRoundedRect(nvg, 0, 0, static_cast<float>(barW), static_cast<float>(barH),
                           nvgRGBA(28, 16, 16, 250), nvgRGBA(255, 90, 70, 225), 10.0f);
        nvgFontFace(nvg, "Inter");
        nvgFontSize(nvg, 15.0f);
        nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        // Approve (green)
        nvgDrawRoundedRect(nvg, 8, 8, static_cast<float>(half), static_cast<float>(barH - 16),
                           nvgRGBA(40, 170, 90, 235), nvgRGBA(90, 225, 140, 255), 6.0f);
        nvgFillColor(nvg, nvgRGBA(255, 255, 255, 255));
        nvgText(nvg, 8 + half * 0.5f, barH * 0.5f, "Approve", nullptr);
        // Cancel (grey)
        nvgDrawRoundedRect(nvg, 8 + half + 8, 8, static_cast<float>(half), static_cast<float>(barH - 16),
                           nvgRGBA(40, 44, 52, 235), nvgRGBA(90, 98, 112, 255), 6.0f);
        nvgFillColor(nvg, nvgRGBA(210, 220, 235, 235));
        nvgText(nvg, 8 + half + 8 + half * 0.5f, barH * 0.5f, "Cancel", nullptr);
    } else {
        mcpApprovalFrame = {};
        mcpApprovalYes = {};
        mcpApprovalNo = {};
    }
    // Multimodal Pen / Sketch Ink Overlay (Rendered directly on GPU in canvas coords)
    renderSketchOverlay(nvg);

    // Paste slot ghost: an empty dashed frame waiting for Ctrl+V. Explicit,
    // local, never auto-read from the clipboard.
    if (mcpImageSlotActive) {
        float const scx = static_cast<float>(mcpImageSlotPos.x);
        float const scy = static_cast<float>(mcpImageSlotPos.y);
        float const sx = scx - 160.0f;
        float const sy = scy - 110.0f;

        NVGScopedState scopedSlot(nvg);
        nvgBeginPath(nvg);
        nvgRoundedRect(nvg, sx, sy, 320.0f, 220.0f, 8.0f);
        nvgFillColor(nvg, nvgRGBA(20, 24, 30, 150));
        nvgFill(nvg);
        nvgStrokeColor(nvg, nvgRGBA(124, 77, 255, 200));
        nvgStrokeWidth(nvg, 1.5f);
        nvgLineStyle(nvg, NVG_LINE_DASHED);
        nvgDashLength(nvg, 7.0f);
        nvgStroke(nvg);
        nvgLineStyle(nvg, NVG_LINE_SOLID);

        nvgFontFace(nvg, "Inter");
        nvgFontSize(nvg, 13.0f);
        nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(nvg, nvgRGBA(198, 208, 226, 225));
        nvgText(nvg, scx, scy - 8.0f, "Ctrl+V to paste image", nullptr);
        nvgFontSize(nvg, 11.0f);
        nvgFillColor(nvg, nvgRGBA(150, 160, 180, 170));
        nvgText(nvg, scx, scy + 12.0f, "(Esc cancels)", nullptr);
    }

    // PRD overlay: ghost/preview of PROPOSED (uncommitted) changes. Drawn above
    // the patch, never part of it — the artist sees the change before it's real.
    if (pd && shouldShowAIGhosts() && !pd->getMcpGhosts().empty()) {
        for (auto const& g : pd->getMcpGhosts()) {
            NVGScopedState scopedGhost(nvg);
            if (g.kind == 0) {
                float const gx = canvasOrigin.x + g.x;
                float const gy = canvasOrigin.y + g.y;
                float const gw = g.w > 0 ? g.w : 120.0f;
                float const gh = g.h > 0 ? g.h : 22.0f;
                nvgBeginPath(nvg);
                nvgRoundedRect(nvg, gx, gy, gw, gh, Corners::objectCornerRadius);
                nvgFillColor(nvg, nvgRGBA(74, 158, 255, 55));
                nvgFill(nvg);
                nvgStrokeColor(nvg, nvgRGBA(74, 158, 255, 220));
                nvgStrokeWidth(nvg, 1.5f);
                nvgLineStyle(nvg, NVG_LINE_DASHED);
                nvgDashLength(nvg, 6.0f);
                nvgStroke(nvg);
                nvgLineStyle(nvg, NVG_LINE_SOLID);
                if (g.label.isNotEmpty()) {
                    nvgFontSize(nvg, 12.0f);
                    nvgFontFace(nvg, "Inter");
                    nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                    nvgFillColor(nvg, nvgRGBA(74, 158, 255, 255));
                    nvgText(nvg, gx + 6.0f, gy + gh * 0.5f, g.label.toRawUTF8(), nullptr);
                }
            } else {
                float const x1 = canvasOrigin.x + g.x1, y1 = canvasOrigin.y + g.y1;
                float const x2 = canvasOrigin.x + g.x2, y2 = canvasOrigin.y + g.y2;
                nvgBeginPath(nvg);
                nvgMoveTo(nvg, x1, y1);
                float const dx = std::abs(x2 - x1) * 0.5f + 20.0f;
                nvgBezierTo(nvg, x1, y1 + dx, x2, y2 - dx, x2, y2);
                nvgStrokeColor(nvg, nvgRGBA(74, 158, 255, 220));
                nvgStrokeWidth(nvg, 2.0f);
                nvgLineStyle(nvg, NVG_LINE_DASHED);
                nvgDashLength(nvg, 6.0f);
                nvgStroke(nvg);
                nvgLineStyle(nvg, NVG_LINE_SOLID);
            }
        }
    }

    if (graphArea) {
        NVGScopedState scopedState(nvg);
        nvgTranslate(nvg, graphArea->getX(), graphArea->getY());
        graphArea->render(nvg);
    }

    objectGrid.render(nvg);

    if (viewport && lasso.isVisible() && !lasso.getBounds().isEmpty()) {
        auto lassoBounds = lasso.getBounds();
        lassoBounds = lassoBounds.withSize(jmax(lasso.getWidth(), 2), jmax(lasso.getHeight(), 2));
        nvgDrawRoundedRect(nvg, lassoBounds.getX(), lassoBounds.getY(), lassoBounds.getWidth(), lassoBounds.getHeight(), lassoCol, lassoOutlineCol, 0.0f);
    }

    suggestor->renderAutocompletion(nvg);

    // Draw the search panel's selected object over all other objects
    // Because objects can be underneath others
    if (canvasSearchHighlight)
        canvasSearchHighlight->render(nvg);

    if (dimensionsAreBeingEdited) {
        bool borderWasShown = showBorder;
        showBorder = true;
        drawBorder(false, true);
        showBorder = borderWasShown;
    }

    if (objectsDistributeResizer)
        objectsDistributeResizer->render(nvg);

    // Studio ink mode: a subtle neon frame around the visible viewport so the
    // armed pen is unmistakable — even after you look away. (Crosshair alone is
    // easy to miss.) Screen-constant width via /zoom.
    if ((mcpSketchToolActive || mcpEraserToolActive) && viewport && !::getValue<bool>(presentationMode) && zoom > 0.0f) {
        float const vx = static_cast<float>(viewport->getViewPositionX()) / zoom;
        float const vy = static_cast<float>(viewport->getViewPositionY()) / zoom;
        float const vw = static_cast<float>(viewport->getWidth()) / zoom;
        float const vh = static_cast<float>(viewport->getHeight()) / zoom;
        float const inset = 2.0f / zoom;

        NVGScopedState scopedArmed(nvg);
        nvgBeginPath(nvg);
        nvgRect(nvg, vx + inset, vy + inset, vw - inset * 2.0f, vh - inset * 2.0f);
        nvgStrokeColor(nvg, nvgRGBA(0, 230, 255, 22));
        nvgStrokeWidth(nvg, 9.0f / zoom);
        nvgStroke(nvg);

        nvgBeginPath(nvg);
        nvgRect(nvg, vx + inset, vy + inset, vw - inset * 2.0f, vh - inset * 2.0f);
        nvgStrokeColor(nvg, nvgRGBA(0, 230, 255, 115));
        nvgStrokeWidth(nvg, 1.5f / zoom);
        nvgStroke(nvg);
    }

    nvgRestore(nvg);

    // Draw scrollbars
    if (viewport) {
        reinterpret_cast<CanvasViewport*>(viewport.get())->render(nvg, viewport->getLocalArea(this, invalidRegion));
    }

    // Cinematic Viewport HUD & Subtitle Overlay (Video & Tutorial Mode)
    // Rendered in screen coordinates (unscaled/unpanned) — zero cord collisions.
    if (pd) {
        auto hud = pd->getMcpHud();
        if (hud.active && shouldShowAIHud()) {
            NVGScopedState scopedHud(nvg);
            float const vw = static_cast<float>(viewport ? viewport->getWidth() : getWidth());
            float const vh = static_cast<float>(viewport ? viewport->getHeight() : getHeight());

            bool const hasPrompt = hud.prompt.isNotEmpty();
            bool const hasReceipt = hud.receipt.isNotEmpty();
            bool const hasTool = hud.tool.isNotEmpty();
            bool const hasChapter = hud.chapter.isNotEmpty();

            // Card title (Chapter, action name, or tool)
            juce::String cardTitle;
            if (hasChapter) cardTitle = hud.chapter;
            else if (hasTool) cardTitle = hud.tool;

            // Direct action statement
            juce::String cardDesc;
            if (hasPrompt) cardDesc = hud.prompt;
            else if (hasReceipt) cardDesc = hud.receipt;

            // Sanitize description text: strip newlines and unprintable characters that cause tofu boxes
            juce::String cleanDesc = cardDesc.replaceCharacter('\r', ' ');
            while (cleanDesc.contains("\n\n"))
                cleanDesc = cleanDesc.replace("\n\n", " \xE2\x80\xA2 ");
            cleanDesc = cleanDesc.replaceCharacter('\n', ' ').trim();

            // Sizing: floating centered card with title-safe margins
            constexpr float maxCardW = 760.0f;
            float const cardW = jmin(vw - 48.0f, maxCardW);
            float const cardX = (vw - cardW) * 0.5f;
            float const innerPadX = 18.0f;
            float const breakWidth = cardW - (innerPadX * 2.0f);

            // Compute text height via nvgTextBoxBounds for multi-line wrapped text
            float descH = 0.0f;
            if (cleanDesc.isNotEmpty()) {
                nvgFontSize(nvg, 14.5f);
                nvgFontFace(nvg, "Inter");
                float bounds[4];
                nvgTextBoxBounds(nvg, 0, 0, breakWidth, cleanDesc.toRawUTF8(), nullptr, bounds);
                descH = bounds[3] - bounds[1];
                descH = jlimit(18.0f, 85.0f, descH);
            }

            float cardH = 50.0f;
            if (cardTitle.isNotEmpty() && cleanDesc.isNotEmpty()) {
                cardH = 13.0f + 22.0f + 9.0f + descH + 14.0f;
            } else if (cleanDesc.isNotEmpty()) {
                cardH = 14.0f + descH + 14.0f;
            }
            float const cardY = vh - cardH - 34.0f;

            // Determine status accent color based on chapter
            juce::String const upperChapter = hud.chapter.toUpperCase();
            NVGcolor accentCol = nvgRGBA(56, 189, 248, 255);    // Electric Cyan default (THINKING / WORKING / BUILDING)
            NVGcolor badgeBg = nvgRGBA(56, 189, 248, 30);
            NVGcolor badgeBorder = nvgRGBA(56, 189, 248, 110);

            if (upperChapter.contains("DONE") || upperChapter.contains("APPROVED") || upperChapter.contains("VOICE")) {
                accentCol = nvgRGBA(16, 185, 129, 255);          // Emerald Green
                badgeBg = nvgRGBA(16, 185, 129, 32);
                badgeBorder = nvgRGBA(16, 185, 129, 120);
            } else if (upperChapter.contains("APPROVE")) {
                accentCol = nvgRGBA(245, 158, 11, 255);          // Amber Gold
                badgeBg = nvgRGBA(245, 158, 11, 40);
                badgeBorder = nvgRGBA(245, 158, 11, 140);
            } else if (upperChapter.contains("ERROR") || upperChapter.contains("DENIED") || upperChapter.contains("FAIL")) {
                accentCol = nvgRGBA(244, 63, 94, 255);           // Coral Red
                badgeBg = nvgRGBA(244, 63, 94, 40);
                badgeBorder = nvgRGBA(244, 63, 94, 140);
            }

            // 1. Soft Ambient Box-Gradient Shadow
            NVGpaint shadowPaint = nvgBoxGradient(nvg, cardX, cardY + 5.0f, cardW, cardH, 14.0f, 24.0f, nvgRGBA(0, 0, 0, 160), nvgRGBA(0, 0, 0, 0));
            nvgBeginPath(nvg);
            nvgRect(nvg, cardX - 20.0f, cardY - 12.0f, cardW + 40.0f, cardH + 44.0f);
            nvgFillPaint(nvg, shadowPaint);
            nvgFill(nvg);

            // 2. Obsidian Glass Body with subtle linear gradient
            NVGpaint bodyPaint = nvgLinearGradient(nvg, cardX, cardY, cardX, cardY + cardH,
                                                   nvgRGBA(18, 22, 32, 246),
                                                   nvgRGBA(10, 12, 17, 252));
            nvgBeginPath(nvg);
            nvgRoundedRect(nvg, cardX, cardY, cardW, cardH, 13.0f);
            nvgFillPaint(nvg, bodyPaint);
            nvgFill(nvg);

            // 3. Subtle Frosted Border
            nvgBeginPath(nvg);
            nvgRoundedRect(nvg, cardX, cardY, cardW, cardH, 13.0f);
            nvgStrokeColor(nvg, nvgRGBA(255, 255, 255, 30));
            nvgStrokeWidth(nvg, 1.0f);
            nvgStroke(nvg);

            // 4. Specular Top Highlight
            nvgBeginPath(nvg);
            nvgRoundedRect(nvg, cardX + 1.5f, cardY + 1.0f, cardW - 3.0f, 1.5f, 1.0f);
            nvgFillColor(nvg, nvgRGBA(255, 255, 255, 45));
            nvgFill(nvg);

            // 5. Header Row (LED Capsule Badge + Tool Tag + Meta)
            float const headerY = cardY + 13.0f;
            float nextHeaderX = cardX + innerPadX;

            if (cardTitle.isNotEmpty()) {
                nvgFontSize(nvg, 11.5f);
                nvgFontFace(nvg, "Inter-Bold");
                float titleBounds[4];
                nvgTextBounds(nvg, 0, 0, cardTitle.toRawUTF8(), nullptr, titleBounds);
                float const pillTextW = titleBounds[2] - titleBounds[0];
                float const pillH = 22.0f;
                float const pillW = pillTextW + 28.0f;

                // Pill Background
                nvgBeginPath(nvg);
                nvgRoundedRect(nvg, nextHeaderX, headerY, pillW, pillH, 11.0f);
                nvgFillColor(nvg, badgeBg);
                nvgFill(nvg);
                nvgStrokeColor(nvg, badgeBorder);
                nvgStrokeWidth(nvg, 1.0f);
                nvgStroke(nvg);

                // Glowing LED Dot
                float const ledX = nextHeaderX + 10.0f;
                float const ledY = headerY + pillH * 0.5f;
                NVGpaint ledGlow = nvgRadialGradient(nvg, ledX, ledY, 1.5f, 6.5f, accentCol, nvgRGBA(0, 0, 0, 0));
                nvgBeginPath(nvg);
                nvgCircle(nvg, ledX, ledY, 6.5f);
                nvgFillPaint(nvg, ledGlow);
                nvgFill(nvg);

                nvgBeginPath(nvg);
                nvgCircle(nvg, ledX, ledY, 3.0f);
                nvgFillColor(nvg, accentCol);
                nvgFill(nvg);

                // Title Text inside Pill
                nvgFontSize(nvg, 11.0f);
                nvgFontFace(nvg, "Inter-Bold");
                nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                nvgFillColor(nvg, nvgRGBA(248, 250, 252, 255));
                nvgText(nvg, nextHeaderX + 18.0f, headerY + pillH * 0.5f + 0.5f, cardTitle.toRawUTF8(), nullptr);

                nextHeaderX += pillW + 10.0f;
            }

            // Secondary Tool Tag
            if (hasTool && hud.tool != cardTitle) {
                nvgFontSize(nvg, 12.0f);
                nvgFontFace(nvg, "Inter");
                nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                nvgFillColor(nvg, nvgRGBA(148, 163, 184, 210));
                juce::String const toolStr = "// " + hud.tool;
                nvgText(nvg, nextHeaderX, headerY + 11.0f, toolStr.toRawUTF8(), nullptr);
            }

            // Right-aligned Telemetry / Latency
            juce::String const metaRight = hud.latency.isNotEmpty() ? hud.latency : hud.telemetry;
            if (metaRight.isNotEmpty()) {
                nvgFontSize(nvg, 11.5f);
                nvgFontFace(nvg, "Inter-Tabular");
                nvgTextAlign(nvg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
                nvgFillColor(nvg, nvgRGBA(100, 116, 139, 230));
                nvgText(nvg, cardX + cardW - innerPadX, headerY + 11.0f, metaRight.toRawUTF8(), nullptr);
            }

            // 6. Direct Description with Word Wrapping (nvgTextBox)
            if (cleanDesc.isNotEmpty()) {
                float const descY = cardTitle.isNotEmpty() ? (headerY + 22.0f + 8.0f) : (cardY + 13.0f);
                nvgFontSize(nvg, 14.5f);
                nvgFontFace(nvg, "Inter");
                nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
                nvgFillColor(nvg, nvgRGBA(226, 232, 240, 245));
                nvgTextBox(nvg, cardX + innerPadX, descY, breakWidth, cleanDesc.toRawUTF8(), nullptr);
            }
        }
    }
}

void Canvas::renderAllObjects(NVGcontext* nvg, Rectangle<int> const area)
{
    for (auto* obj : objects) {
        {
            auto b = obj->getBounds();
            if (b.intersects(area) && obj->isVisible()) {
                NVGScopedState scopedState(nvg);
                nvgTranslate(nvg, b.getX(), b.getY());
                obj->render(nvg);
            }
        }

        // Draw label in canvas coordinates
        obj->renderLabel(nvg);
    }
}
void Canvas::renderAllConnections(NVGcontext* nvg, Rectangle<int> const area)
{
    if (!connectionLayer.isVisible())
        return;

    // TODO: Can we clean this up? We will want to have selected connections in-front,
    //  and take precedence over non-selected for resize handles

    SmallArray<Connection*> connectionsToDraw;
    SmallArray<Connection*> connectionsToDrawSelected;
    Connection* hovered = nullptr;

    for (auto* connection : connections) {
        NVGScopedState scopedState(nvg);
        if (connection->intersectsRectangle(area) && connection->isVisible()) {
            if (connection->isMouseHovering())
                hovered = connection;
            else if (!connection->isSelected())
                connection->render(nvg);
            else
                connectionsToDrawSelected.add(connection);
            if (showConnectionOrder && isConnectionTargeted(connection)) {
                connectionsToDraw.add(connection);
            }
        }
    }
    // Draw all selected connections in front
    if (connectionsToDrawSelected.not_empty()) {
        for (auto* connection : connectionsToDrawSelected) {
            NVGScopedState scopedState(nvg);
            connection->render(nvg);
        }
    }

    if (hovered) {
        NVGScopedState scopedState(nvg);
        hovered->render(nvg);
    }

    if (connectionsToDraw.not_empty()) {
        for (auto const* connection : connectionsToDraw) {
            NVGScopedState scopedState(nvg);
            connection->renderConnectionOrder(nvg);
        }
    }
}

void Canvas::settingsChanged(String const& name, var const& value)
{
    switch (hash(name)) {
    case hash("grid_size"):
        repaint();
        break;
    case hash("border"):
        showBorder = static_cast<int>(value);
        repaint();
        break;
    case hash("edit"):
    case hash("lock"):
    case hash("run"):
    case hash("alt"):
    case hash("alt_mode"): {
        updateOverlays();
        break;
    }
    default:
        break;
    }
}

bool Canvas::shouldShowObjectActivity() const
{
    return showObjectActivity && !presentationMode.getValue() && !isGraph;
}

bool Canvas::shouldShowIndex() const
{
    return showIndex && !presentationMode.getValue();
}

bool Canvas::shouldShowAIState() const
{
    return showAiState && !presentationMode.getValue();
}

bool Canvas::shouldShowAIRegions() const
{
    if (presentationMode.getValue()) return false;
    if (pd && !pd->getMcpRegions().empty()) return true;
    return showAiRegions;
}

bool Canvas::shouldShowAIAnnotations() const
{
    if (presentationMode.getValue()) return false;
    if (pd && !pd->getMcpAnnotations().empty()) return true;
    return showAiAnnotations;
}

bool Canvas::shouldShowAIGhosts() const
{
    return showAiGhosts && !presentationMode.getValue();
}

bool Canvas::shouldShowAIHud() const
{
    return showAiHud && !presentationMode.getValue();
}

bool Canvas::shouldShowAISketch() const
{
    if (presentationMode.getValue()) return false;
    return showAiSketch;
}

bool Canvas::shouldShowAIReferences() const
{
    if (presentationMode.getValue() || isGraph) return false;
    return showAiReferences;
}

void Canvas::setSketchToolActive(bool active)
{
    mcpSketchToolActive = active;
    if (active) {
        mcpEraserToolActive = false; // one tool at a time
        mcpTextToolActive = false;
    }
    setMouseCursor((active || mcpEraserToolActive || mcpTextToolActive) ? MouseCursor::CrosshairCursor : MouseCursor::NormalCursor);
    // The chisel's presence follows the pen: arming shows it, dropping it hides
    // it (when the board is empty). Keeps the mode state always visible.
    updateSelectionPill();
    repaint();
    if (editor) editor->nvgSurface.renderAll();
}

void Canvas::setEraserToolActive(bool active)
{
    mcpEraserToolActive = active;
    if (active) {
        mcpSketchToolActive = false; // one tool at a time
        mcpTextToolActive = false;
    }
    setMouseCursor((active || mcpSketchToolActive) ? MouseCursor::CrosshairCursor : MouseCursor::NormalCursor);
    updateSelectionPill();
    repaint();
    if (editor) editor->nvgSurface.renderAll();
}

void Canvas::setTextToolActive(bool active)
{
    mcpTextToolActive = active;
    if (active) {
        mcpSketchToolActive = false; // one tool at a time
        mcpEraserToolActive = false;
        setMouseCursor(MouseCursor::IBeamCursor);
    } else if (!mcpSketchToolActive && !mcpEraserToolActive) {
        setMouseCursor(MouseCursor::NormalCursor);
    }
    updateSelectionPill();
    repaint();
    if (editor) editor->nvgSurface.renderAll();
}

bool Canvas::isSketchToolActive() const
{
    return mcpSketchToolActive;
}

bool Canvas::isSketchingActive() const
{
    return isSketching;
}

void Canvas::setStudioMode(bool active)
{
    mcpStudioMode = active;
    // Brainstorm mode = pen up; edit mode = pen down, pill back in charge.
    setSketchToolActive(active);
    updateSelectionPill();
    repaint();
    if (editor) editor->nvgSurface.renderAll();
}

void Canvas::setStudioInkVisible(bool visible)
{
    mcpStudioInkVisible = visible;
    repaint();
    if (editor) editor->nvgSurface.renderAll();
}

void Canvas::commitStudioFromCanvas()
{
    juce::String draft;
    if (editor && editor->sidebar) {
        if (auto* cp = editor->sidebar->getCopilotPanel()) draft = cp->getStudioDraft();
    }
    sendSketchBuild(draft);
    if (editor && editor->sidebar) {
        if (auto* cp = editor->sidebar->getCopilotPanel()) cp->clearStudioDraft();
    }
}

juce::StringArray Canvas::getSelectionStableIds()
{
    juce::StringArray ids;
    if (!pd) return ids;
    auto patchPtr = patch.getPointer();
    t_canvas* cnv = patchPtr ? patchPtr.get() : nullptr;
    for (auto* obj : getSelectionOfType<Object>()) {
        if (auto* ptr = obj->getPointer()) {
            auto tid = pd->getStableId(ptr);
            if (tid.isEmpty() && cnv) {
                tid = pd->getOrAdoptStableId(cnv, ptr);
            }
            if (tid.isNotEmpty()) ids.add(tid);
        }
    }
    return ids;
}

void Canvas::createTextNoteAt(Point<int> canvasPos)
{
    if (!pd) return;

    // Drop any abandoned empty artist note first so note indices never drift.
    {
        juce::ScopedLock sl(pd->mcpOverlayLock);
        std::erase_if(pd->mcpAnnotations, [](PluginProcessor::McpAnnotation const& a) {
            return a.text.isEmpty() && a.kind == "artist";
        });

        PluginProcessor::McpAnnotation ann;
        ann.x = static_cast<float>(canvasPos.x - canvasOrigin.x);
        ann.y = static_cast<float>(canvasPos.y - canvasOrigin.y);
        ann.kind = "artist";
        ann.t = juce::Time::getMillisecondCounterHiRes() / 1000.0;
        // Surgical lane: a selection turns the note into an attachment bound to it
        // (renders anchored beside the object with a leader tick).
        auto const selIds = getSelectionStableIds();
        if (selIds.size() > 0) {
            ann.targetId = selIds[0];
            ann.targetIds = selIds;
        }
        pd->mcpAnnotations.push_back(ann);
    }

    openNoteEditorForIndex(static_cast<int>(pd->getMcpAnnotations().size()) - 1);
}

void Canvas::openNoteEditorForIndex(int index)
{
    if (!pd) return;
    auto const anns = pd->getMcpAnnotations();
    if (index < 0 || index >= static_cast<int>(anns.size())) return;
    auto const ann = anns[static_cast<size_t>(index)];

    if (!mcpNoteEditor) {
        mcpNoteEditor = std::make_unique<juce::TextEditor>();
        mcpNoteEditor->setMultiLine(false);
        mcpNoteEditor->setReturnKeyStartsNewLine(false);
        mcpNoteEditor->setWantsKeyboardFocus(true);

        // Commit: pin the typed note. Empty text = discard it entirely.
        auto commit = [this] {
            if (mcpNoteEditIndex < 0) return;
            auto const txt = mcpNoteEditor ? mcpNoteEditor->getText().trim() : juce::String();
            if (pd) {
                juce::ScopedLock sl(pd->mcpOverlayLock);
                if (mcpNoteEditIndex < static_cast<int>(pd->mcpAnnotations.size())) {
                    if (txt.isEmpty()) {
                        pd->mcpAnnotations.erase(pd->mcpAnnotations.begin() + mcpNoteEditIndex);
                    } else {
                        auto& a = pd->mcpAnnotations[static_cast<size_t>(mcpNoteEditIndex)];
                        a.text = txt;
                        a.kind = "artist";
                    }
                }
            }
            mcpNoteEditIndex = -1;
            if (mcpNoteEditor) mcpNoteEditor->setVisible(false);
            repaint();
            if (editor) editor->nvgSurface.renderAll();
        };

        mcpNoteEditor->onReturnKey = [commit] { commit(); };
        mcpNoteEditor->onFocusLost = [this, commit] {
            if (juce::Time::getMillisecondCounter() - mcpNoteOpenedAt < 500) return;
            commit();
        };
        // Live feedback: write the field's text onto the note as you type.
        mcpNoteEditor->onTextChange = [this] {
            if (mcpNoteEditor && pd && mcpNoteEditIndex >= 0) {
                juce::ScopedLock sl(pd->mcpOverlayLock);
                if (mcpNoteEditIndex < static_cast<int>(pd->mcpAnnotations.size()))
                    pd->mcpAnnotations[static_cast<size_t>(mcpNoteEditIndex)].text = mcpNoteEditor->getText();
            }
            repaint();
            if (editor) editor->nvgSurface.renderAll();
        };
        mcpNoteEditor->onEscapeKey = [commit] { commit(); }; // Esc = finish editing

        mcpNoteEditor->setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff16161c));
        mcpNoteEditor->setColour(juce::TextEditor::textColourId, juce::Colours::white);
        mcpNoteEditor->setColour(juce::TextEditor::outlineColourId, juce::Colour(0xffffbe50));
        mcpNoteEditor->setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour(0xffffbe50));
        mcpNoteEditor->setColour(juce::TextEditor::highlightColourId, juce::Colour(0xffffbe50));
        mcpNoteEditor->setColour(juce::CaretComponent::caretColourId, juce::Colours::white);
        addAndMakeVisible(*mcpNoteEditor);
        if (!mcpSlotPasteKeys) mcpSlotPasteKeys = std::make_unique<SlotPasteKeyListener>(this);
        mcpNoteEditor->addKeyListener(mcpSlotPasteKeys.get());
    }

    mcpNoteEditIndex = index;
    mcpNoteOpenedAt = juce::Time::getMillisecondCounter();
    mcpNoteEditor->setBounds(juce::roundToInt(canvasOrigin.x + ann.x - 6.0f),
                             juce::roundToInt(canvasOrigin.y + ann.y - 26.0f),
                             220, 20);
    mcpNoteEditor->setText(ann.text, false);
    mcpNoteEditor->setVisible(true);
    mcpNoteEditor->toFront(true);

    auto* ed = mcpNoteEditor.get();
    juce::MessageManager::callAsync([ed] {
        if (ed) {
            ed->grabKeyboardFocus();
            ed->selectAll();
        }
    });

    repaint();
    if (editor) editor->nvgSurface.renderAll();
}

void Canvas::SketchFadeTimer::timerCallback()
{
    if (!canvas) return;
    canvas->mcpSketchAlpha -= 0.025f;
    if (canvas->mcpSketchAlpha <= 0.0f) {
        canvas->mcpSketchAlpha = 0.0f;
        canvas->mcpCompletedStrokes.clear();
        stopTimer();
    }
    canvas->repaint();
    if (canvas->editor) canvas->editor->nvgSurface.renderAll();
}

void Canvas::startSketchFadeTimer()
{
    mcpSketchAlpha = 1.0f;
    if (!mcpSketchFadeTimer) {
        mcpSketchFadeTimer = std::make_unique<SketchFadeTimer>(this);
    }
    mcpSketchFadeTimer->startTimerHz(40);
}

void Canvas::renderSketchOverlay(NVGcontext* nvg)
{
    if (!shouldShowAISketch()) return;

    auto drawStroke = [&](const std::vector<SketchPoint>& stroke, float alpha) {
        if (stroke.size() < 2) return;

        // 1. Outer Glow (Electric Cyan, soft wide stroke)
        nvgBeginPath(nvg);
        nvgMoveTo(nvg, stroke[0].x, stroke[0].y);
        for (size_t i = 1; i < stroke.size(); ++i) {
            nvgLineTo(nvg, stroke[i].x, stroke[i].y);
        }
        nvgLineCap(nvg, NVG_ROUND);
        nvgLineJoin(nvg, NVG_ROUND);
        nvgStrokeWidth(nvg, 7.0f);
        nvgStrokeColor(nvg, nvgRGBA(0, 230, 255, static_cast<unsigned char>(alpha * 120.0f)));
        nvgStroke(nvg);

        // 2. Cyan Body
        nvgBeginPath(nvg);
        nvgMoveTo(nvg, stroke[0].x, stroke[0].y);
        for (size_t i = 1; i < stroke.size(); ++i) {
            nvgLineTo(nvg, stroke[i].x, stroke[i].y);
        }
        nvgLineCap(nvg, NVG_ROUND);
        nvgLineJoin(nvg, NVG_ROUND);
        nvgStrokeWidth(nvg, 3.5f);
        nvgStrokeColor(nvg, nvgRGBA(0, 245, 255, static_cast<unsigned char>(alpha * 220.0f)));
        nvgStroke(nvg);

        // 3. Hot White Center Core
        nvgBeginPath(nvg);
        nvgMoveTo(nvg, stroke[0].x, stroke[0].y);
        for (size_t i = 1; i < stroke.size(); ++i) {
            nvgLineTo(nvg, stroke[i].x, stroke[i].y);
        }
        nvgLineCap(nvg, NVG_ROUND);
        nvgLineJoin(nvg, NVG_ROUND);
        nvgStrokeWidth(nvg, 1.5f);
        nvgStrokeColor(nvg, nvgRGBA(255, 255, 255, static_cast<unsigned char>(alpha * 240.0f)));
        nvgStroke(nvg);
    };

    if (mcpSketchAlpha > 0.001f && mcpStudioInkVisible) {
        for (auto const& s : mcpCompletedStrokes) {
            drawStroke(s, mcpSketchAlpha);
        }
    }

    if (isSketching && !mcpCurrentStroke.empty()) {
        drawStroke(mcpCurrentStroke, 1.0f);
    }
}

juce::Rectangle<float> Canvas::getSketchBoundsInCanvas() const
{
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    bool any = false;
    for (auto const& stroke : mcpCompletedStrokes) {
        for (auto const& p : stroke) {
            minX = std::min(minX, p.x);
            minY = std::min(minY, p.y);
            maxX = std::max(maxX, p.x);
            maxY = std::max(maxY, p.y);
            any = true;
        }
    }
    if (!any) return {};
    return { minX, minY, maxX - minX, maxY - minY };
}

juce::String Canvas::buildSketchJson() const
{
    juce::Array<juce::var> strokesVar;
    for (auto const& stroke : mcpCompletedStrokes) {
        juce::Array<juce::var> ptsVar;
        for (auto const& p : stroke) {
            auto* obj = new juce::DynamicObject();
            obj->setProperty("x", p.x - static_cast<float>(canvasOrigin.x));
            obj->setProperty("y", p.y - static_cast<float>(canvasOrigin.y));
            obj->setProperty("t", p.t);
            ptsVar.add(juce::var(obj));
        }
        strokesVar.add(juce::var(ptsVar));
    }
    return juce::JSON::toString(juce::var(strokesVar), true);
}

void Canvas::sendSketchBuild(const juce::String& prompt)
{
    if (!pd) return;
    // Visible-Only Ingestion: hidden ink is excluded from the brief (sent as []).
    if (mcpCompletedStrokes.empty() && pd->getMcpReferenceImages().empty()) return;

    // Committing = done drawing: disarm the pen so the artist can immediately
    // drag/tweak the freshly built patch without leaving ink everywhere.
    // (In Studio mode the drawer owns the pen — keep it up for the next idea.)
    if (!mcpStudioMode) setSketchToolActive(false);

    // Bounding box in PD patch coords — construct_patch_v4 speaks this space.
    juce::String bboxJson = "{}";
    if (mcpStudioInkVisible && !mcpCompletedStrokes.empty()) {
        auto const sb = getSketchBoundsInCanvas();
        auto* bboxObj = new juce::DynamicObject();
        bboxObj->setProperty("x", sb.getX() - static_cast<float>(canvasOrigin.x));
        bboxObj->setProperty("y", sb.getY() - static_cast<float>(canvasOrigin.y));
        bboxObj->setProperty("w", sb.getWidth());
        bboxObj->setProperty("h", sb.getHeight());
        bboxJson = juce::JSON::toString(juce::var(bboxObj), true);
    }

    // Park the artist's words in the sidebar log (canvas stays calm).
    if (editor && editor->sidebar) {
        if (auto* cp = editor->sidebar->getCopilotPanel()) {
            cp->receiveMessage("user", prompt.isNotEmpty() ? prompt : juce::String("Build from moodboard"));
        }
    }

    auto const subpatch = patch.getPointer() ? juce::String(reinterpret_cast<uintptr_t>(patch.getPointer().get())) : juce::String("0");
    if (auto* br = pd->getMCPBridge()) {
        br->sendSketchBuild(subpatch, mcpStudioInkVisible ? buildSketchJson() : juce::String("[]"), bboxJson, prompt);
    }

    // ── Commit: the ink did its job and clears; the mood STAYS. ───────────────
    // Artist-first default (learned in the field): pictures, notes and takes are
    // moodboard furniture — they survive the build so you can A/B the new patch
    // against the reference it came from. Remove them explicitly with 🗑/🔒.
    mcpCompletedStrokes.clear();
    mcpCurrentStroke.clear();
    mcpLastSketchBounds = {};

    updateSelectionPill();
    repaint();
    if (editor) editor->nvgSurface.renderAll();
}

void Canvas::undoLastStroke()
{
    if (mcpCompletedStrokes.empty()) return;
    mcpCompletedStrokes.pop_back();
    updateSelectionPill();
    repaint();
    if (editor) editor->nvgSurface.renderAll();
}

void Canvas::clearAllInk()
{
    mcpCompletedStrokes.clear();
    mcpCurrentStroke.clear();
    mcpSketchAlpha = 1.0f;
    if (mcpSketchFadeTimer) mcpSketchFadeTimer->stopTimer();
    updateSelectionPill();
    repaint();
    if (editor) editor->nvgSurface.renderAll();
}

//===----------------------------------------------------------------------===//
// Phase 2 — Reference Images (pasted schematics on the canvas glass)
//===----------------------------------------------------------------------===//

namespace {
// JUCE 7.0's SystemClipboard only speaks text. Image paste therefore tries, in order:
//  1. Linux/X11 + Wayland clipboard tools (xclip / wl-paste), bounded by `timeout`;
//  2. a text-path heuristic — many apps copy "file:///path/img.png" as clipboard text.
juce::Image loadImageFromClipboard()
{
    auto const clipboardText = juce::SystemClipboard::getTextFromClipboard().trim();
    if (clipboardText.isNotEmpty() && clipboardText.length() < 4096) {
        juce::File file;
        if (clipboardText.startsWithIgnoreCase("file://"))
            file = juce::URL(clipboardText).getLocalFile();
        else if (juce::File::isAbsolutePath(clipboardText))
            file = juce::File(clipboardText);

        if (file.existsAsFile()) {
            if (auto image = juce::ImageFileFormat::loadFrom(file); image.isValid()) {
                return image;
            }
        }
    }

#if JUCE_LINUX
    auto const tmp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("plugdata_paste_" + juce::String(juce::Random::getSystemRandom().nextInt64()) + ".img");
    auto const quotedPath = "'" + tmp.getFullPathName().replace("'", "'\\''") + "'";
    juce::StringArray attempts;
    attempts.add("timeout 3 xclip -selection clipboard -t image/png -o > " + quotedPath);
    attempts.add("timeout 3 wl-paste --type image/png > " + quotedPath);
    attempts.add("timeout 3 wl-paste > " + quotedPath);
    for (auto const& command : attempts) {
        tmp.deleteFile();
        juce::ChildProcess proc;
        if (!proc.start(juce::StringArray { "/bin/sh", "-c", command })) {
            continue;
        }
        proc.waitForProcessToFinish(3500);
        if (proc.isRunning()) proc.kill();
        if (tmp.existsAsFile() && tmp.getSize() > 0) {
            if (auto image = juce::ImageFileFormat::loadFrom(tmp); image.isValid()) {
                tmp.deleteFile();
                return image;
            }
        }
    }
    tmp.deleteFile();
#endif

    return {};
}
} // namespace

void Canvas::renderReferenceImages(NVGcontext* nvg, Rectangle<int> invalidRegion)
{
    if (!pd || !shouldShowAIReferences()) return; // 👁 layer toggle + performance mode
    auto const images = pd->getMcpReferenceImages();
    if (images.empty()) return;

    for (auto const& img : images) {
        if (!img.visible) continue; // Visible-Only Ingestion Law

        // Native nvg texture from the cached PNG (stb_image path). The JUCE-ARGB
        // upload path proved unreliable for file-loaded colour images, so this
        // bypasses NVGImage entirely.
        auto& entry = mcpRefImageCache.try_emplace(img.id.toStdString()).first->second;
        if (entry.handle == 0 || entry.ctx != nvg) {
            entry.handle = nvgCreateImage(nvg, img.filePath.toRawUTF8(), 0);
            entry.ctx = nvg;
        }
        if (entry.handle == 0) continue;

        Rectangle<float> const b(static_cast<float>(canvasOrigin.x) + img.x,
                                 static_cast<float>(canvasOrigin.y) + img.y,
                                 img.width, img.height);
        if (!invalidRegion.intersects(b.getSmallestIntegerContainer())) continue;

        NVGScopedState scopedImage(nvg);

        // Light paper matte behind the image — dark screenshots on a dark canvas
        // would otherwise blend into the background and look "missing".
        nvgBeginPath(nvg);
        nvgRoundedRect(nvg, b.getX() - 6.0f, b.getY() - 6.0f, b.getWidth() + 12.0f, b.getHeight() + 12.0f, 9.0f);
        nvgFillColor(nvg, nvgRGBA(228, 230, 236, 240));
        nvgFill(nvg);

        // The image itself — straight through nvgImagePattern.
        {
            nvgBeginPath(nvg);
            nvgRoundedRect(nvg, b.getX(), b.getY(), b.getWidth(), b.getHeight(), 4.0f);
            nvgFillPaint(nvg, nvgImagePattern(nvg, b.getX(), b.getY(), b.getWidth(), b.getHeight(), 0.0f, entry.handle, img.alpha));
            nvgFill(nvg);
        }

        // Hardware frame: cool slate normally, warm amber when locked
        nvgBeginPath(nvg);
        nvgRoundedRect(nvg, b.getX() - 1.0f, b.getY() - 1.0f, b.getWidth() + 2.0f, b.getHeight() + 2.0f, 5.0f);
        nvgStrokeColor(nvg, img.isLocked ? nvgRGBA(255, 190, 80, 210) : nvgRGBA(150, 165, 190, 140));
        nvgStrokeWidth(nvg, img.isLocked ? 1.6f : 1.2f);
        nvgStroke(nvg);

        // Title tag above the image (+ lock glyph)
        juce::String const tag = img.title.isNotEmpty() ? img.title : juce::File(img.filePath).getFileName();
        nvgFontFace(nvg, "Inter");
        nvgFontSize(nvg, 10.0f);
        nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        float textBounds[4];
        nvgTextBounds(nvg, 0.0f, 0.0f, tag.toRawUTF8(), nullptr, textBounds);
        float const tagW = (textBounds[2] - textBounds[0]) + (img.isLocked ? 34.0f : 18.0f);
        nvgBeginPath(nvg);
        nvgRoundedRect(nvg, b.getX(), b.getY() - 20.0f, tagW, 16.0f, 3.5f);
        nvgFillColor(nvg, nvgRGBA(16, 18, 22, 225));
        nvgFill(nvg);
        nvgStrokeColor(nvg, nvgRGBA(120, 135, 160, 120));
        nvgStrokeWidth(nvg, 1.0f);
        nvgStroke(nvg);
        nvgFillColor(nvg, nvgRGBA(205, 216, 232, 235));
        nvgText(nvg, b.getX() + 8.0f, b.getY() - 12.0f, tag.toRawUTF8(), nullptr);

        if (img.isLocked) {
            float const lx = b.getX() + tagW - 12.0f;
            float const ly = b.getY() - 11.5f;
            nvgBeginPath(nvg);
            nvgRoundedRect(nvg, lx - 3.5f, ly - 0.5f, 7.0f, 5.5f, 1.0f);
            nvgFillColor(nvg, nvgRGBA(255, 190, 80, 235));
            nvgFill(nvg);
            nvgBeginPath(nvg);
            nvgArc(nvg, lx, ly - 2.0f, 2.6f, juce::MathConstants<float>::pi, 0.0f, NVG_HOLE);
            nvgStrokeColor(nvg, nvgRGBA(255, 190, 80, 235));
            nvgStrokeWidth(nvg, 1.2f);
            nvgStroke(nvg);
        }
    }
}

bool Canvas::pasteImageFromClipboard()
{
    auto const clipboardImage = loadImageFromClipboard();
    if (!clipboardImage.isValid()) {
        // Slot is open but the clipboard has no image yet — coach, keep the slot.
        if (mcpImageSlotActive) {
            mcpMoodHint = "Clipboard has no image — copy one, then Ctrl+V here";
            mcpMoodHintUntil = juce::Time::getMillisecondCounter() + 2600;
            repaint();
            if (editor) editor->nvgSurface.renderAll();
        }
        return false;
    }
    if (!pd) return true; // consume the paste; there is nowhere to put it

    // Encode once: hash names the cache file, bytes are reused for the disk write.
    juce::MemoryOutputStream pngData;
    juce::PNGImageFormat pngFormat;
    if (!pngFormat.writeImageToStream(clipboardImage, pngData)) return true;
    auto const hash = juce::MD5(pngData.getData(), pngData.getDataSize()).toHexString();

    auto const refsDir = ProjectInfo::appDataDir.getChildFile("references");
    refsDir.createDirectory();
    auto const pngFile = refsDir.getChildFile(hash + ".png");
    if (!pngFile.existsAsFile()) {
        if (auto stream = std::unique_ptr<juce::FileOutputStream>(pngFile.createOutputStream())) {
            stream->write(pngData.getData(), pngData.getDataSize());
            stream->flush();
        }
    }

    // Fit the image to the paste slot when one is open, else to a sane default.
    float const maxW = mcpImageSlotActive ? 320.0f : 520.0f;
    float const maxH = mcpImageSlotActive ? 220.0f : 420.0f;
    float const fit = juce::jmin(1.0f, juce::jmin(maxW / static_cast<float>(clipboardImage.getWidth()), maxH / static_cast<float>(clipboardImage.getHeight())));
    float const w = static_cast<float>(clipboardImage.getWidth()) * fit;
    float const h = static_cast<float>(clipboardImage.getHeight()) * fit;

    auto const mousePosition = getMouseXYRelative() - canvasOrigin;

    PluginProcessor::McpReferenceImage ref;
    ref.id = hash;
    ref.filePath = pngFile.getFullPathName();
    ref.title = "Pasted_" + hash.substring(0, 6) + ".png";
    // Surgical lane: pasting over a selection attaches the image to it.
    auto const selIds = getSelectionStableIds();
    if (selIds.size() > 0) ref.targetId = selIds[0];
    if (mcpImageSlotActive) {
        // Drop into the waiting slot, centered.
        ref.x = static_cast<float>(mcpImageSlotPos.x - canvasOrigin.x) - w * 0.5f;
        ref.y = static_cast<float>(mcpImageSlotPos.y - canvasOrigin.y) - h * 0.5f;
        mcpImageSlotActive = false; // slot consumed
    } else {
        ref.x = static_cast<float>(mousePosition.x) - w * 0.5f;
        ref.y = static_cast<float>(mousePosition.y) - h * 0.5f;
    }
    ref.width = w;
    ref.height = h;
    pd->addMcpReferenceImage(ref);

    // Bridge dispatch: /pd/reference/image <path> <x> <y> <w> <h> (PD patch coords)
    if (auto* br = pd->getMCPBridge()) {
        br->sendReferenceImage(ref.filePath, ref.x, ref.y, ref.width, ref.height);
    }

    // Pasting a reference = composing mode: put the pen down so the fresh image
    // is immediately draggable / lockable (re-arm with the ✏️ chip when done).
    // In Studio brainstorm mode the drawer owns the pen — keep drawing.
    if (!mcpStudioMode) setSketchToolActive(false);

    repaint();
    if (editor) editor->nvgSurface.renderAll();
    return true;
}

// Phase 3 — Voice take chips: ▶ audition, mini-waveform, duration, ✕ remove.
void Canvas::renderVoiceTakes(NVGcontext* nvg, Rectangle<int> invalidRegion)
{
    if (!pd || presentationMode.getValue()) return;
    auto takes = pd->getMcpVoiceTakes();
    if (takes.empty()) return;

    auto* br = pd->getMCPBridge();
    juce::String const playingId = br ? br->getPlayingVoiceTakeId() : juce::String();

    constexpr float chipW = 240.0f;
    constexpr float chipH = 38.0f;
    int slot = 0;

    for (auto& take : takes) {
        if (!take.visible) continue;

        if (!take.positioned) {
            // First sight: park near where the record strip was (or canvas centre).
            float const px = mcpTakeSpawnPos.x != 0 ? static_cast<float>(mcpTakeSpawnPos.x)
                                                    : static_cast<float>(canvasOrigin.x + 40);
            float const py = mcpTakeSpawnPos.y != 0 ? static_cast<float>(mcpTakeSpawnPos.y)
                                                    : static_cast<float>(canvasOrigin.y + 40);
            pd->setMcpVoiceTakePos(take.id, px - static_cast<float>(canvasOrigin.x),
                                   py - static_cast<float>(canvasOrigin.y) + 46.0f * static_cast<float>(slot));
            take.x = px - static_cast<float>(canvasOrigin.x);
            take.y = py - static_cast<float>(canvasOrigin.y) + 46.0f * static_cast<float>(slot);
            take.positioned = true;
        }
        ++slot;

        Rectangle<float> const b(static_cast<float>(canvasOrigin.x) + take.x,
                                 static_cast<float>(canvasOrigin.y) + take.y,
                                 chipW, chipH);
        if (!invalidRegion.intersects(b.getSmallestIntegerContainer())) continue;

        bool const playing = playingId == take.id;
        bool const melody = take.mode.startsWithIgnoreCase("melody");
        NVGcolor const accent = melody ? nvgRGBA(255, 190, 60, playing ? 255 : 210)
                                       : nvgRGBA(80, 220, 140, playing ? 255 : 210);

        NVGScopedState scopedChip(nvg);

        // Cassette body: deep slate + per-type accent border (amber = melody, green = beat)
        nvgDrawRoundedRect(nvg, b.getX(), b.getY(), b.getWidth(), b.getHeight(),
                           nvgRGBA(18, 22, 28, 242), accent, 6.0f);

        // Type glyph: beamed notes (melody) or drum (beat) — the strip's language
        {
            float const gx = b.getX() + 18.0f;
            float const gy = b.getCentreY();
            nvgStrokeColor(nvg, accent);
            nvgFillColor(nvg, accent);
            if (melody) {
                nvgBeginPath(nvg);
                nvgEllipse(nvg, gx - 3.0f, gy + 3.5f, 2.4f, 1.7f);
                nvgFill(nvg);
                nvgBeginPath(nvg);
                nvgEllipse(nvg, gx + 3.2f, gy + 2.2f, 2.4f, 1.7f);
                nvgFill(nvg);
                nvgBeginPath(nvg);
                nvgMoveTo(nvg, gx - 0.7f, gy + 3.4f);
                nvgLineTo(nvg, gx - 0.7f, gy - 4.5f);
                nvgLineTo(nvg, gx + 5.6f, gy - 5.7f);
                nvgLineTo(nvg, gx + 5.6f, gy + 2.1f);
                nvgStrokeWidth(nvg, 1.3f);
                nvgStroke(nvg);
            } else {
                nvgBeginPath(nvg);
                nvgRoundedRect(nvg, gx - 5.0f, gy - 0.5f, 10.0f, 5.5f, 1.7f);
                nvgStrokeWidth(nvg, 1.1f);
                nvgStroke(nvg);
                nvgBeginPath(nvg);
                nvgEllipse(nvg, gx, gy - 0.5f, 5.0f, 1.7f);
                nvgStrokeWidth(nvg, 1.1f);
                nvgStroke(nvg);
                nvgBeginPath(nvg);
                nvgMoveTo(nvg, gx - 3.8f, gy - 6.0f);
                nvgLineTo(nvg, gx + 0.8f, gy - 1.2f);
                nvgMoveTo(nvg, gx + 3.8f, gy - 6.0f);
                nvgLineTo(nvg, gx - 0.8f, gy - 1.2f);
                nvgStrokeWidth(nvg, 1.0f);
                nvgStroke(nvg);
            }
        }

        // ▶ / ⏸ (playing) — pause bars while the take is auditioning
        float const pcx = b.getX() + 46.0f;
        float const pcy = b.getCentreY();
        if (playing) {
            nvgBeginPath(nvg);
            nvgRoundedRect(nvg, pcx - 4.5f, pcy - 5.0f, 3.4f, 10.0f, 1.2f);
            nvgFillColor(nvg, accent);
            nvgFill(nvg);
            nvgBeginPath(nvg);
            nvgRoundedRect(nvg, pcx + 1.1f, pcy - 5.0f, 3.4f, 10.0f, 1.2f);
            nvgFillColor(nvg, accent);
            nvgFill(nvg);
        } else {
            nvgBeginPath(nvg);
            nvgMoveTo(nvg, pcx - 4.5f, pcy - 6.5f);
            nvgLineTo(nvg, pcx + 6.5f, pcy);
            nvgLineTo(nvg, pcx - 4.5f, pcy + 6.5f);
            nvgClosePath(nvg);
            nvgFillColor(nvg, accent);
            nvgFill(nvg);
        }

        // Mini-waveform (64 peak buckets)
        float const wfX = b.getX() + 62.0f;
        float const wfW = b.getWidth() - 62.0f - 72.0f;
        float const wfCy = pcy;
        constexpr float wfMaxH = 22.0f;
        if (take.peaks.size() >= 2) {
            float const barW = wfW / static_cast<float>(take.peaks.size());
            nvgBeginPath(nvg);
            for (size_t k = 0; k < take.peaks.size(); ++k) {
                float const h = juce::jmax(1.5f, juce::jlimit(0.0f, 1.0f, take.peaks[k]) * wfMaxH);
                nvgRect(nvg, wfX + static_cast<float>(k) * barW, wfCy - h * 0.5f, juce::jmax(0.8f, barW - 0.4f), h);
            }
            nvgFillColor(nvg, accent);
            nvgFill(nvg);
        } else {
            nvgBeginPath(nvg);
            nvgMoveTo(nvg, wfX, wfCy);
            nvgLineTo(nvg, wfX + wfW, wfCy);
            nvgStrokeColor(nvg, accent);
            nvgStrokeWidth(nvg, 1.0f);
            nvgStroke(nvg);
        }

        // Duration m:ss
        int const totalSec = juce::roundToInt(take.durationSec);
        juce::String const dur = juce::String(totalSec / 60) + ":" + juce::String(totalSec % 60).paddedLeft('0', 2);
        nvgFontFace(nvg, "Inter");
        nvgFontSize(nvg, 10.0f);
        nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(nvg, nvgRGBA(230, 220, 200, 220));
        nvgText(nvg, b.getX() + b.getWidth() - 66.0f, pcy, dur.toRawUTF8(), nullptr);

        // ✕ remove
        float const xx = b.getRight() - 14.0f;
        nvgBeginPath(nvg);
        nvgMoveTo(nvg, xx - 4.0f, pcy - 4.0f);
        nvgLineTo(nvg, xx + 4.0f, pcy + 4.0f);
        nvgMoveTo(nvg, xx + 4.0f, pcy - 4.0f);
        nvgLineTo(nvg, xx - 4.0f, pcy + 4.0f);
        nvgStrokeColor(nvg, nvgRGBA(220, 150, 150, 230));
        nvgStrokeWidth(nvg, 1.4f);
        nvgStroke(nvg);
    }
}

bool Canvas::handleVoiceTakeClick(MouseEvent const& e, Point<int> mousePos)
{
    if (!pd) return false;
    auto const takes = pd->getMcpVoiceTakes();
    for (auto it = takes.rbegin(); it != takes.rend(); ++it) {
        auto const& take = *it;
        if (!take.visible) continue;
        Rectangle<int> const b(static_cast<int>(canvasOrigin.x + take.x), static_cast<int>(canvasOrigin.y + take.y), 240, 38);
        if (!b.contains(mousePos)) continue;

        int const relX = mousePos.x - b.getX();
        auto* br = pd->getMCPBridge();

        if (relX >= 30 && relX <= 60) {
            // ▶ / ■ audition toggle
            if (br) br->playVoiceTake(take.id, take.wavPath);
            repaint();
            if (editor) editor->nvgSurface.renderAll();
            return true;
        }
        if (relX >= b.getWidth() - 30) {
            // ✕ remove (stop audition first if it's this one)
            if (br && br->getPlayingVoiceTakeId() == take.id) br->stopVoiceTake();
            pd->removeMcpVoiceTake(take.id);
            repaint();
            if (editor) editor->nvgSurface.renderAll();
            return true;
        }

        // Body: drag the chip
        mcpDraggingTakeId = take.id;
        mcpTakeDragOffset = mousePos - b.getPosition();
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);
        return true;
    }
    return false;
}

bool Canvas::handleReferenceImageClick(MouseEvent const& e, Point<int> mousePos, bool isRightClick)
{
    if (!pd) return false;
    auto const images = pd->getMcpReferenceImages();
    for (auto it = images.rbegin(); it != images.rend(); ++it) {
        auto const& img = *it;
        // Include the title header in the hit zone so the tag doubles as a grab handle
        Rectangle<float> const b(static_cast<float>(canvasOrigin.x) + img.x - 4.0f,
                                 static_cast<float>(canvasOrigin.y) + img.y - 22.0f,
                                 img.width + 8.0f, img.height + 26.0f);
        if (!b.contains(mousePos.toFloat())) continue;

        if (!isRightClick) {
            if (img.isLocked) return true; // locked: swallow the click, don't lasso through it
            mcpDraggingRefImageId = img.id;
            mcpRefImageDragOffset = mousePos - Point<int>(juce::roundToInt(static_cast<float>(canvasOrigin.x) + img.x),
                                                          juce::roundToInt(static_cast<float>(canvasOrigin.y) + img.y));
            setMouseCursor(juce::MouseCursor::DraggingHandCursor);
            return true;
        }

        auto const imageId = img.id;
        auto const title = img.title.isNotEmpty() ? img.title : juce::File(img.filePath).getFileName();
        auto const locked = img.isLocked;

        juce::PopupMenu menu;
        menu.addSectionHeader("Reference Image");
        menu.addItem(1, "Analyze Circuit with AI");
        menu.addItem(2, locked ? "Unlock Position" : "Lock Position");
        menu.addSeparator();
        menu.addItem(3, "Delete Reference Image");
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(Rectangle<int>(e.getScreenPosition(), e.getScreenPosition().translated(1, 1))),
            [safeThis = juce::Component::SafePointer<Canvas>(this), imageId, title](int result) {
                auto* canvas = safeThis.getComponent();
                if (!canvas || !canvas->pd) return;

                if (result == 1) {
                    auto const prompt = "[REFERENCE IMAGE] Analyze the pasted schematic \"" + title + "\" on the canvas and build a live patch that matches its circuit or panel.";
                    if (canvas->editor && canvas->editor->sidebar) {
                        if (auto* cp = canvas->editor->sidebar->getCopilotPanel()) cp->receiveMessage("user", prompt);
                    }
                    canvas->sendPillPrompt(prompt, false);
                } else if (result == 2) {
                    for (auto const& im : canvas->pd->getMcpReferenceImages()) {
                        if (im.id == imageId) {
                            canvas->pd->setMcpReferenceLock(imageId, !im.isLocked);
                            break;
                        }
                    }
                    canvas->repaint();
                    if (canvas->editor) canvas->editor->nvgSurface.renderAll();
                } else if (result == 3) {
                    canvas->pd->removeMcpReferenceImage(imageId);
                    canvas->mcpRefImageCache.erase(imageId.toStdString());
                    canvas->repaint();
                    if (canvas->editor) canvas->editor->nvgSurface.renderAll();
                }
            });
        return true;
    }
    return false;
}

// The server signals a pending destructive op via the HUD chapter ("APPROVE?").
bool Canvas::isApprovalRequested() const
{
    if (!pd) return false;
    auto const hud = pd->getMcpHud();
    return hud.active && hud.chapter.containsIgnoreCase("APPROVE");
}

// Artist clicked [Approve] / [Cancel] (or pressed Enter / Esc). The pill prompt
// path carries it to the server, which holds the token inert until this yes.
void Canvas::sendApproval(bool approve)
{
    mcpApprovalFrame = {};
    mcpApprovalYes = {};
    mcpApprovalNo = {};
    sendPillPrompt(approve ? "approve" : "deny");
    repaint();
    if (editor) editor->nvgSurface.renderAll();
}

bool Canvas::shouldShowConnectionDirection() const
{
    return showConnectionDirection;
}

bool Canvas::shouldShowConnectionActivity() const
{
    return showConnectionActivity;
}

void Canvas::setOverlayMask(int mask)
{
    auto overlaysTree = SettingsFile::getInstance()->getValueTree().getChildWithName("Overlays");
    if (overlaysTree.isValid()) {
        auto const key = (locked.getValue() || commandLocked.getValue()) ? "lock" : "edit";
        overlaysTree.setProperty(key, mask, nullptr);
    }
    updateOverlays();
}

bool Canvas::hasOverlayTargets() const
{
    return pd && pd->hasMcpOverlayTargets();
}

bool Canvas::isObjectTargeted(Object const* obj) const
{
    if (!pd || !pd->hasMcpOverlayTargets()) return true;
    if (!obj) return false;
    return pd->isMcpTargeted(obj->getPointer());
}

bool Canvas::isConnectionTargeted(Connection const* c) const
{
    if (!pd || !pd->hasMcpOverlayTargets()) return true;
    if (!c) return false;
    t_gobj* const outPtr = c->outobj ? c->outobj->getPointer() : nullptr;
    t_gobj* const inPtr = c->inobj ? c->inobj->getPointer() : nullptr;
    return pd->isMcpConnectionTargeted(outPtr, inPtr);
}

int Canvas::getOverlays() const
{
    int overlayState = 0;

    auto const overlaysTree = SettingsFile::getInstance()->getValueTree().getChildWithName("Overlays");

    auto const altModeEnabled = overlaysTree.getProperty("alt_mode") && !isGraph;

    if (!locked.getValue()) {
        overlayState = overlaysTree.getProperty("edit");
    }
    if (locked.getValue() || commandLocked.getValue()) {
        overlayState = overlaysTree.getProperty("lock");
    }
    if (altModeEnabled) {
        overlayState = overlaysTree.getProperty("alt");
    }

    return overlayState;
}

void Canvas::updateOverlays()
{
    int const overlayState = getOverlays();

    showBorder = overlayState & Border;
    showOrigin = overlayState & Origin;
    showConnectionOrder = overlayState & Order;
    connectionsBehind = overlayState & Behind;
    showObjectActivity = overlayState & ActivationState;
    showIndex = overlayState & Index;
    showAiState = overlayState & AIState;
    showAiRegions = overlayState & AIRegions;
    showAiAnnotations = overlayState & AIAnnotations;
    showAiGhosts = overlayState & AIGhosts;
    showAiHud = overlayState & AIHud;
    showAiSketch = overlayState & AISketch;
    showAiReferences = overlayState & AIReferences;
    showConnectionDirection = overlayState & Direction;
    showConnectionActivity = overlayState & ConnectionActivity;

    set_plugdata_activity_enabled(showObjectActivity);
    orderConnections();

    repaint();
}

void Canvas::jumpToOrigin()
{
    if (viewport)
        viewport->setViewPosition((canvasOrigin + Point<int>(1, 1)).transformedBy(getTransform()));
}

void Canvas::restoreViewportState()
{
    if (viewport) {
        viewport->setViewPosition((patch.lastViewportPosition + canvasOrigin).transformedBy(getTransform()));
        zoomScale.setValue(patch.lastViewportScale);
        setTransform(AffineTransform().scaled(patch.lastViewportScale));
    }
}

void Canvas::saveViewportState()
{
    if (viewport) {
        patch.lastViewportPosition = viewport->getViewPosition().transformedBy(getTransform().inverted()) - canvasOrigin;
        patch.lastViewportScale = getValue<float>(zoomScale);
    }
}

void Canvas::zoomToFitAll()
{
    if (objects.empty() || !viewport)
        return;

    auto scale = getValue<float>(zoomScale);

    auto regionOfInterest = Rectangle<int>(canvasOrigin.x, canvasOrigin.y, 20, 20);

    if (!presentationMode.getValue()) {
        for (auto const* object : objects) {
            regionOfInterest = regionOfInterest.getUnion(object->getBounds().reduced(Object::margin));
        }
    }

    // Add a bit of margin to make it nice
    regionOfInterest = regionOfInterest.expanded(16);

    auto const viewArea = viewport->getViewArea() / scale;

    auto const roiHeight = static_cast<float>(regionOfInterest.getHeight());
    auto const roiWidth = static_cast<float>(regionOfInterest.getWidth());

    auto const scaleWidth = viewArea.getWidth() / roiWidth;
    auto const scaleHeight = viewArea.getHeight() / roiHeight;
    scale = jmin(scaleWidth, scaleHeight);
    scale = std::clamp(scale, 0.05f, 3.0f);

    auto transform = getTransform();
    transform = transform.scaled(scale);
    setTransform(transform);

    scale = std::sqrt(std::abs(transform.getDeterminant()));
    zoomScale.setValue(scale);

    auto const viewportCentre = viewport->getViewArea().withZeroOrigin().getCentre();
    auto const newViewPos = regionOfInterest.transformedBy(getTransform()).getCentre() - viewportCentre;
    viewport->setViewPosition(newViewPos);
}

void Canvas::tabChanged()
{
    patch.setCurrent();

    synchronise();
    updateDrawables();

    for (auto const* obj : objects) {
        if (!obj->gui)
            continue;

        obj->gui->tabChanged();
    }

    editor->statusbar->updateZoomLevel();
    editor->repaint(); // Make sure everything it up to date
}

void Canvas::save(std::function<void()> const& nestedCallback)
{
    auto const* canvasToSave = this;
    if (patch.isSubpatch()) {
        for (auto const& parentCanvas : editor->getCanvases()) {
            if (patch.getRoot() == parentCanvas->patch.getRawPointer()) {
                canvasToSave = parentCanvas;
            }
        }
    }

    if (canvasToSave->patch.getCurrentFile().existsAsFile()) {
        canvasToSave->patch.savePatch();
        SettingsFile::getInstance()->addToRecentlyOpened(canvasToSave->patch.getCurrentFile());
        pd->titleChanged();
        nestedCallback();
    } else {
        saveAs(nestedCallback);
    }
}

void Canvas::saveAs(std::function<void()> const& nestedCallback)
{
    Dialogs::showSaveDialog([this, nestedCallback](URL const& resultURL) mutable {
        auto result = resultURL.getLocalFile();
        if (result.getFullPathName().isNotEmpty()) {
            if (result.exists())
                result.deleteFile();

            if (!result.hasFileExtension("pd"))
                result = result.getFullPathName() + ".pd";

            patch.savePatch(resultURL);
            SettingsFile::getInstance()->addToRecentlyOpened(result);
            pd->titleChanged();
        }

        nestedCallback();
    },
        "*.pd", "Patch", this);
}

void Canvas::handleAsyncUpdate()
{
    performSynchronise();
}

void Canvas::synchronise()
{
    triggerAsyncUpdate();
}

void Canvas::synchroniseAllCanvases()
{
    for (auto* editorWindow : pd->getEditors()) {
        for (auto* canvas : editorWindow->getTabComponent().getVisibleCanvases()) {
            canvas->synchronise();
        }
    }
}

void Canvas::synchroniseSplitCanvas()
{
    for (auto* e : pd->getEditors()) {
        for (auto* canvas : e->getTabComponent().getVisibleCanvases()) {
            canvas->synchronise();
        }
    }
}

// Synchronise state with pure-data
// Used for loading and for complicated actions like undo/redo
void Canvas::performSynchronise()
{
    static bool alreadyFlushed = false;
    bool const needsFlush = !alreadyFlushed;
    ScopedValueSetter<bool> flushGuard(alreadyFlushed, true);
    // By flushing twice, we can make sure that any message sent before this point will be dequeued
    if (needsFlush && !isGraph) {
        if (!pd->getIsProcessingAudio())
            pd->doubleFlushMessageQueue();
        else
            pd->flushMessageQueue();
    }

    // Remove deleted connections
    for (int n = connections.size() - 1; n >= 0; n--) {
        if (!connections[n]->getPointer()) {
            connections.remove_at(n);
        }
    }

    // Remove deleted objects
    for (int n = objects.size() - 1; n >= 0; n--) {
        // If the object is showing it's initial editor, meaning no object was assigned yet, allow it to exist without pointing to an object
        if (auto* object = objects[n]; !object->getPointer() && !object->isInitialEditorShown()) {
            setSelected(object, false, false);
            objects.remove_at(n);
        }
    }

    // Check for connections that need to be remade because of invalid iolets
    for (int n = connections.size() - 1; n >= 0; n--) {
        if (!connections[n]->inlet || !connections[n]->outlet) {
            connections.remove_at(n);
        }
    }

    auto pdObjects = patch.getObjects();
    objects.reserve(pdObjects.size());

    for (auto object : pdObjects) {
        auto const* it = std::ranges::find_if(objects, [&object](Object const* b) { return b->getPointer() && b->getPointer() == object.getRawUnchecked<void>(); });
        if (!object.isValid())
            continue;

        if (it == objects.end()) {
            auto* newObject = objects.add(object, this);
            newObject->toFront(false);

            if (newObject->gui && newObject->gui->getLabel())
                newObject->gui->getLabel()->toFront(false);
        } else {
            auto* object = *it;

            // Check if number of inlets/outlets is correct
            object->updateIolets();
            object->updateBounds();

            object->toFront(false);
            if (object->gui && object->gui->getLabel())
                object->gui->getLabel()->toFront(false);
            if (object->gui)
                object->gui->update();
        }
    }

    // Make sure objects have the same order
    std::ranges::sort(objects,
        [&pdObjects](Object const* first, Object const* second) mutable {
            return pdObjects.index_of(first->getPointer()) < pdObjects.index_of(second->getPointer());
        });

    auto pdConnections = isGraph ? pd::Connections() : patch.getConnections();
    connections.reserve(pdConnections.size());

    for (auto& connection : pdConnections) {
        auto& [ptr, inno, inobj, outno, outobj] = connection;

        Iolet *inlet = nullptr, *outlet = nullptr;

        // Find the objects that this connection is connected to
        for (auto* obj : objects) {
            if (outobj && &outobj->te_g == obj->getPointer()) {

                // Check if we have enough outlets, should never return false
                if (isPositiveAndBelow(obj->numInputs + outno, obj->iolets.size())) {
                    outlet = obj->iolets[obj->numInputs + outno];
                } else {
                    break;
                }
            }
            if (inobj && &inobj->te_g == obj->getPointer()) {

                // Check if we have enough inlets, should never return false
                if (isPositiveAndBelow(inno, obj->iolets.size())) {
                    inlet = obj->iolets[inno];
                } else {
                    break;
                }
            }
        }

        // This shouldn't be necessary, but just to be sure...
        if (!inlet || !outlet) {
            jassertfalse;
            continue;
        }

        auto const* it = std::ranges::find_if(connections,
            [c_ptr = ptr](auto* c) {
                return c_ptr == c->getPointer();
            });

        if (it == connections.end()) {
            connections.add(this, inlet, outlet, ptr);
        } else {
            // This is necessary to make resorting a subpatchers iolets work
            // And it can't hurt to check if the connection is valid anyway
            if (auto& c = **it; c.inlet != inlet || c.outlet != outlet) {
                int const idx = connections.index_of(*it);
                connections.remove_one(*it);
                connections.insert(idx, this, inlet, outlet, ptr);
            } else {
                c.popPathState();
            }
        }
    }

    for (auto* c : connections) {
        c->updatePath();
    }

    if (!isGraph) {
        setTransform(AffineTransform().scaled(getValue<float>(zoomScale)));
    }

    if (graphArea)
        graphArea->updateBounds();

    editor->updateCommandStatus();
    repaint();

    needsSearchUpdate = true;

    pd->updateObjectImplementations();
    cancelPendingUpdate(); // if an update got retriggered, cancel it
}

void Canvas::updateDrawables()
{
    for (auto const* object : objects) {
        if (object->gui) {
            object->gui->updateDrawables();
        }
    }
}

void Canvas::shiftKeyChanged(bool const isHeld)
{
    shiftDown = isHeld;

    if (!isGraph) {
        SettingsFile::getInstance()->getValueTree().getChildWithName("Overlays").setProperty("alt_mode", altDown && shiftDown, nullptr);
    }

    if (!isHeld)
        return;

    if (connectionsBeingCreated.size() == 1) {
        Iolet* connectingOutlet = connectionsBeingCreated[0]->getIolet();
        Iolet* targetInlet = nullptr;
        for (auto const& object : objects) {
            for (auto const& iolet : object->iolets) {
                if (iolet->isTargeted && iolet != connectingOutlet) {
                    targetInlet = iolet;
                    break;
                }
            }
        }

        if (targetInlet) {
            bool const inverted = connectingOutlet->isInlet;
            if (inverted)
                std::swap(connectingOutlet, targetInlet);

            if (auto x = patch.getPointer()) {
                auto* outObj = connectingOutlet->object->getPointer();
                auto* inObj = targetInlet->object->getPointer();
                auto const outletIndex = connectingOutlet->ioletIdx;
                auto const inletIndex = targetInlet->ioletIdx;

                SmallArray<t_gobj*> selectedObjects;
                for (auto const* object : getSelectionOfType<Object>()) {
                    if (auto* ptr = object->getPointer()) {
                        selectedObjects.add(ptr);
                    }
                }

                // If we autopatch from inlet to outlet with multiple selection, pure-data can't handle it
                if (inverted && selectedObjects.size() > 1)
                    return;

                t_outconnect const* connection = nullptr;
                if (auto selectedConnections = getSelectionOfType<Connection>(); selectedConnections.size() == 1) {
                    connection = selectedConnections[0]->getPointer();
                }

                pd::Interface::shiftAutopatch(x.get(), inObj, inletIndex, outObj, outletIndex, selectedObjects, connection);
            }
        }

        synchronise();
    }
}

void Canvas::commandKeyChanged(bool const isHeld)
{
    commandLocked = isHeld;
}

void Canvas::middleMouseChanged(bool isHeld)
{
    checkPanDragMode();
}

void Canvas::altKeyChanged(bool const isHeld)
{
    altDown = isHeld;

    if (!isGraph) {
        SettingsFile::getInstance()->getValueTree().getChildWithName("Overlays").setProperty("alt_mode", altDown && shiftDown, nullptr);
    }
}

Object* Canvas::findObjectByStableId(juce::String const& targetId) const
{
    if (targetId.isEmpty() || !pd) return nullptr;
    for (auto* obj : objects) {
        if (!obj) continue;
        if (auto* ptr = obj->getPointer()) {
            if (pd->getStableId(ptr) == targetId)
                return obj;
        }
    }
    return nullptr;
}

bool Canvas::getAnnotationLiveBounds(juce::String const& targetId, juce::StringArray const& targetIds, float fallbackX, float fallbackY, float fallbackLx, float fallbackLy, bool fallbackHasLeader, float& outAx, float& outAy, float& outLx, float& outLy, bool& outHasLeader, juce::Rectangle<int>* outGroupBounds) const
{
    auto const ids = targetIds.size() > 0 ? targetIds : (targetId.isNotEmpty() ? juce::StringArray(targetId) : juce::StringArray());
    if (ids.size() > 0) {
        bool first = true;
        int minX = 0, minY = 0, maxX = 0, maxY = 0;
        for (auto const& tid : ids) {
            if (auto* obj = findObjectByStableId(tid)) {
                auto const b = obj->getObjectBounds();
                if (first) {
                    minX = b.getX(); minY = b.getY();
                    maxX = b.getRight(); maxY = b.getBottom();
                    first = false;
                } else {
                    minX = std::min(minX, b.getX());
                    minY = std::min(minY, b.getY());
                    maxX = std::max(maxX, b.getRight());
                    maxY = std::max(maxY, b.getBottom());
                }
            }
        }
        if (!first) {
            if (outGroupBounds) {
                *outGroupBounds = juce::Rectangle<int>(minX, minY, maxX - minX, maxY - minY);
            }

            // Console Tape: pinned directly above the top edge of the module strip
            outLx = static_cast<float>(minX + 14);
            outLy = static_cast<float>(minY);
            outAx = static_cast<float>(minX);
            outAy = static_cast<float>(minY - 24);
            outHasLeader = true;

            // Stack multiple notes on the same group vertically upwards
            if (pd) {
                auto const allAnns = pd->getMcpAnnotations();
                int noteOrder = 0;
                for (auto const& prevA : allAnns) {
                    bool isSelf = false;
                    if (prevA.targetIds.size() > 0 && ids.size() > 0) {
                        if (prevA.targetIds == ids) isSelf = true;
                    } else if (prevA.targetId == targetId) {
                        isSelf = true;
                    }
                    if (isSelf) break;
                    // Check if targeting the same objects
                    bool sameTarget = false;
                    auto const prevIds = prevA.targetIds.size() > 0 ? prevA.targetIds : (prevA.targetId.isNotEmpty() ? juce::StringArray(prevA.targetId) : juce::StringArray());
                    for (auto const& ptid : prevIds) {
                        for (auto const& tid : ids) {
                            if (ptid == tid) { sameTarget = true; break; }
                        }
                        if (sameTarget) break;
                    }
                    if (sameTarget) ++noteOrder;
                }
                outAy -= noteOrder * 24.0f;
            }

            return true;
        }
        return false; // All target objects deleted -> auto-prune
    }

    if (outGroupBounds) *outGroupBounds = juce::Rectangle<int>();
    outAx = fallbackX;
    outAy = fallbackY;
    outLx = fallbackLx;
    outLy = fallbackLy;
    outHasLeader = fallbackHasLeader;
    return true;
}

bool Canvas::getAnnotationLiveBounds(juce::String const& targetId, float fallbackX, float fallbackY, float fallbackLx, float fallbackLy, bool fallbackHasLeader, float& outAx, float& outAy, float& outLx, float& outLy, bool& outHasLeader) const
{
    return getAnnotationLiveBounds(targetId, juce::StringArray(), fallbackX, fallbackY, fallbackLx, fallbackLy, fallbackHasLeader, outAx, outAy, outLx, outLy, outHasLeader, nullptr);
}

juce::Rectangle<float> Canvas::getAnnotationScreenBounds(size_t annIndex) const
{
    if (!pd) return {};
    auto const anns = pd->getMcpAnnotations();
    if (annIndex >= anns.size()) return {};
    auto const& a = anns[annIndex];
    float rawAx = 0.0f, rawAy = 0.0f, rawLx = 0.0f, rawLy = 0.0f;
    bool hasLead = false;
    juce::Rectangle<int> grpBounds;
    if (!getAnnotationLiveBounds(a.targetId, a.targetIds, a.x, a.y, a.leaderX, a.leaderY, a.hasLeader, rawAx, rawAy, rawLx, rawLy, hasLead, &grpBounds))
        return {};

    float const ax = canvasOrigin.x + rawAx;
    float const ay = canvasOrigin.y + rawAy;
    constexpr float tapeH = 20.0f;
    float const textW = a.text.length() * 6.8f + 40.0f;
    float const groupW = grpBounds.isEmpty() ? 140.0f : static_cast<float>(grpBounds.getWidth());
    float tapeW = 0.0f;
    if (!grpBounds.isEmpty() && groupW >= 90.0f) {
        tapeW = std::min(groupW, std::max(80.0f, textW));
    } else {
        tapeW = std::max(groupW, std::min(160.0f, textW));
    }
    return juce::Rectangle<float>(ax, ay, tapeW, tapeH);
}

void Canvas::mouseMove(MouseEvent const& e)
{
    // Hover feedback: brighten the note under the cursor & detect chip hovers.
    int hoverIdx = -1;
    bool hoverClose = false;
    int hoverChip = -1;

    if (pd && !pd->getMcpAnnotations().empty()) {
        auto const pt = e.getPosition().toFloat();
        auto const anns = pd->getMcpAnnotations();
        for (int i = static_cast<int>(anns.size()) - 1; i >= 0; --i) {
            auto const tb = getAnnotationScreenBounds(static_cast<size_t>(i));
            if (tb.isEmpty()) continue;

            auto const chips = getAnnotationPivotChips(anns[static_cast<size_t>(i)].text, anns[static_cast<size_t>(i)].kind);
            float totalChipsW = 0.0f;
            for (auto const& c : chips) totalChipsW += getChipWidth(c.label) + 4.0f;
            float const drawerW = std::max(tb.getWidth(), totalChipsW + 4.0f);
            constexpr float drawerH = 22.0f;
            juce::Rectangle<float> const combinedBounds(tb.getX(), tb.getY(), drawerW, tb.getHeight() + drawerH + 3.0f);

            if (combinedBounds.contains(pt)) {
                hoverIdx = i;
                hoverClose = (pt.y <= tb.getBottom() && pt.x >= tb.getRight() - 16.0f);

                if (pt.y > tb.getBottom() + 1.0f) {
                    float curChipX = tb.getX() + 4.0f;
                    float const chipY = tb.getY() + tb.getHeight() + 2.5f;
                    float const chipH = drawerH - 5.0f;
                    for (size_t ci = 0; ci < chips.size(); ++ci) {
                        float const cw = getChipWidth(chips[ci].label);
                        juce::Rectangle<float> chipBox(curChipX, chipY, cw, chipH);
                        if (chipBox.contains(pt)) {
                            hoverChip = static_cast<int>(ci);
                            break;
                        }
                        curChipX += cw + 4.0f;
                    }
                }
                break;
            }
        }
    }
    if (hoverIdx != mcpNoteHover || hoverClose != mcpNoteHoverClose || hoverChip != mcpNoteHoverChip) {
        mcpNoteHover = hoverIdx;
        mcpNoteHoverClose = hoverClose;
        mcpNoteHoverChip = hoverChip;
        repaint();
        if (editor) editor->nvgSurface.renderAll();
    }
}

void Canvas::mouseExit(MouseEvent const& e)
{
    if (mcpNoteHover != -1 || mcpNoteHoverClose || mcpNoteHoverChip != -1) {
        mcpNoteHover = -1;
        mcpNoteHoverClose = false;
        mcpNoteHoverChip = -1;
        repaint();
        if (editor) editor->nvgSurface.renderAll();
    }
}

bool Canvas::isPointOverNote(Point<int> canvasPt) const
{
    if (mcpSelectionPill && mcpSelectionPill->isVisible()) {
        auto const panel = mcpSelectionPillFrame.isEmpty() ? mcpSelectionPill->getBounds() : mcpSelectionPillFrame;
        if (panel.contains(canvasPt)) return true;
        if (mcpPillDrawerMode != 0 && mcpPillDrawerFrame.contains(canvasPt)) return true;
        if (mcpPillDrawerMode == 3 && mcpPaletteFrame.contains(canvasPt)) return true;
    }
    if (!pd || pd->getMcpAnnotations().empty()) return false;
    auto const pt = canvasPt.toFloat();
    auto const anns = pd->getMcpAnnotations();
    for (size_t i = 0; i < anns.size(); ++i) {
        auto const tb = getAnnotationScreenBounds(i);
        if (tb.isEmpty()) continue;
        if (static_cast<int>(i) == mcpNoteHover) {
            auto const chips = getAnnotationPivotChips(anns[i].text, anns[i].kind);
            float totalChipsW = 0.0f;
            for (auto const& c : chips) totalChipsW += getChipWidth(c.label) + 4.0f;
            float const drawerW = std::max(tb.getWidth(), totalChipsW + 4.0f);
            juce::Rectangle<float> combined(tb.getX(), tb.getY(), drawerW, tb.getHeight() + 25.0f);
            if (combined.contains(pt)) return true;
        } else if (tb.contains(pt)) {
            return true;
        }
    }
    if (mcpNoteEditor && mcpNoteEditor->isVisible() && mcpNoteEditor->getBounds().contains(canvasPt))
        return true;
    return false;
}

bool Canvas::handleNoteClick(Point<int> canvasPt)
{
    // Tier-2 approval chips take priority — they float above the pill.
    if (!mcpApprovalFrame.isEmpty() && mcpApprovalFrame.contains(canvasPt)) {
        if (mcpApprovalYes.contains(canvasPt)) sendApproval(true);
        else if (mcpApprovalNo.contains(canvasPt)) sendApproval(false);
        return true;
    }

    if (mcpSelectionPill && mcpSelectionPill->isVisible()) {
        // Smart Command Palette clicks
        if (mcpPillDrawerMode == 3 && mcpPaletteFrame.contains(canvasPt)) {
            int const relY = canvasPt.y - (mcpPaletteFrame.getY() + 24);
            int const rowH = 30;
            if (relY >= 0) {
                int const clickedRow = relY / rowH;
                triggerPaletteItem(clickedRow);
                return true;
            }
        }

        // 1. Drawer clicks (Lenses / Tools)
        if (mcpPillDrawerMode != 0 && mcpPillDrawerFrame.contains(canvasPt)) {
            int const relX = canvasPt.x - mcpPillDrawerFrame.getX();
            int const dw = mcpPillDrawerFrame.getWidth();
            if (mcpPillDrawerMode == 1) {
                // Lenses: Free | Signal | Logic | Order
                float const itemW = (static_cast<float>(dw) - 8.0f) / 4.0f;
                int idx = static_cast<int>((relX - 4) / itemW);
                if (idx >= 0 && idx < 4) {
                    const char* lenses[] = { "", "signal", "logic", "order" };
                    juce::String lens(lenses[idx]);
                    mcpActiveLens = lens;
                    if (pd) {
                        if (auto* br = pd->getMCPBridge()) br->sendLens(lens.isEmpty() ? "free" : lens);
                    }
                    mcpPillDrawerMode = 0;
                    mcpPillDrawerFrame = {};
                    repaint();
                    if (editor) editor->nvgSurface.renderAll();
                    return true;
                }
            } else if (mcpPillDrawerMode == 2) {
                // Tools: Explain | Drive | Filter | Reverb | Pack GOP | Tidy
                const char* toolPrompts[] = {
                    "explain this",
                    "add tanh~ saturation",
                    "add lowpass filter",
                    "add reverb space",
                    "pack this into gop module",
                    "tidy layout"
                };
                float const itemW = (static_cast<float>(dw) - 8.0f) / 6.0f;
                int idx = static_cast<int>((relX - 4) / itemW);
                if (idx >= 0 && idx < 6) {
                    mcpPillDrawerMode = 0;
                    mcpPillDrawerFrame = {};
                    sendPillPrompt(toolPrompts[idx]);
                    dismissSelectionPill();
                    return true;
                }
            }
            return true;
        }

        // 2. Pill clicks
        auto const panel = mcpSelectionPillFrame.isEmpty() ? mcpSelectionPill->getBounds() : mcpSelectionPillFrame;
        if (panel.contains(canvasPt)) {
            int const relX = canvasPt.x - panel.getX();

            // Mood strip: [✏] [T] [🖼] [🎙] [🔊] — icons first, surfaces second.
            if (mcpPillSketchMode && mcpCompletedStrokes.empty()) {
                if (relX >= 6 && relX < 176) {
                    int const idx = (relX - 6) / 34;
                    switch (idx) {
                    case 0:
                        setSketchToolActive(!isSketchToolActive());
                        repaint();
                        if (editor) editor->nvgSurface.renderAll();
                        return true;
                    case 1:
                        createTextNoteAt(mcpMoodPillActive ? mcpMoodPillPos : juce::Point<int>(panel.getCentreX(), panel.getBottom()));
                        return true;
                    case 2:
                        // Open (or cancel) a paste slot — Ctrl+V fills it. Nothing
                        // is read from the clipboard automatically.
                        mcpImageSlotActive = !mcpImageSlotActive;
                        mcpImageSlotPos = mcpMoodPillActive
                            ? mcpMoodPillPos
                            : juce::Point<int>(panel.getCentreX(), panel.getBottom() + 130);
                        // Pull keyboard focus to the canvas so Ctrl+V can never be
                        // swallowed by a lingering tape-note editor.
                        grabKeyboardFocus();
                        repaint();
                        if (editor) editor->nvgSurface.renderAll();
                        return true;
                    case 3:
                        // Melody record: declared intent — sing (no guessing).
                        if (pd) {
                            if (auto* br = pd->getMCPBridge()) {
                                if (br->isVoiceCapturing()) {
                                    br->stopVoiceCaptureAndAnalyze();
                                } else {
                                    setSketchToolActive(false);
                                    setEraserToolActive(false);
                                    setTextToolActive(false);
                                    mcpTakeSpawnPos = mcpMoodPillActive ? mcpMoodPillPos : juce::Point<int>(panel.getCentreX(), panel.getBottom() + 170);
                                    br->startVoiceCapture(60, 1);
                                }
                                repaint();
                                if (editor) editor->nvgSurface.renderAll();
                            }
                        }
                        return true;
                    default:
                        // Beat record: declared intent — beatbox (no guessing).
                        if (pd) {
                            if (auto* br = pd->getMCPBridge()) {
                                if (br->isVoiceCapturing()) {
                                    br->stopVoiceCaptureAndAnalyze();
                                } else {
                                    setSketchToolActive(false);
                                    setEraserToolActive(false);
                                    setTextToolActive(false);
                                    mcpTakeSpawnPos = mcpMoodPillActive ? mcpMoodPillPos : juce::Point<int>(panel.getCentreX(), panel.getBottom() + 170);
                                    br->startVoiceCapture(60, 2);
                                }
                                repaint();
                                if (editor) editor->nvgSurface.renderAll();
                            }
                        }
                        return true;
                    }
                }
                return true; // the strip frame swallows clicks
            }

            // Sketch / Moodboard Chisel: [⚡ Build] | prompt | [T] [🖼] [↩ Undo] [🗑 Clear] [✏️ pen]
            if (mcpPillSketchMode) {
                if (relX <= 88) {
                    sendSketchBuild(mcpSelectionPill ? mcpSelectionPill->getText().trim() : juce::String());
                    return true;
                }
                if (relX >= panel.getWidth() - 238 && relX < panel.getWidth() - 210) {
                    setTextToolActive(!isTextToolActive());
                    repaint();
                    if (editor) editor->nvgSurface.renderAll();
                    return true;
                }
                if (relX >= panel.getWidth() - 210 && relX < panel.getWidth() - 182) {
                    pasteImageFromClipboard();
                    return true;
                }
                if (relX >= panel.getWidth() - 182 && relX < panel.getWidth() - 106) {
                    undoLastStroke();
                    return true;
                }
                if (relX >= panel.getWidth() - 106 && relX < panel.getWidth() - 32) {
                    clearAllInk();
                    return true;
                }
                if (relX >= panel.getWidth() - 32) {
                    // Pen toggle: put it down / pick it back up mid-sketch.
                    setSketchToolActive(!isSketchToolActive());
                    return true;
                }
                mcpSelectionPill->grabKeyboardFocus();
                return true;
            }

            // Left ~34px: + button (toggles Antigravity Actions/Skills popover!)
            if (relX <= 34) {
                showPillPalette();
                return true;
            }

            // Surgical attachment chips — same tools as Studio, bound to the selection:
            // [✏ attach-sketch] [T attach-text] [🖼 attach-image]
            if (relX >= panel.getWidth() - 148 && relX < panel.getWidth() - 118) {
                setSketchToolActive(!isSketchToolActive());
                repaint();
                if (editor) editor->nvgSurface.renderAll();
                return true;
            }
            if (relX >= panel.getWidth() - 118 && relX < panel.getWidth() - 90) {
                setTextToolActive(!isTextToolActive());
                repaint();
                if (editor) editor->nvgSurface.renderAll();
                return true;
            }
            if (relX >= panel.getWidth() - 90 && relX < panel.getWidth() - 62) {
                pasteImageFromClipboard();
                return true;
            }

            // Right ~60px..~34px: ✏️ Pen / Sketch tool button
            if (relX >= panel.getWidth() - 60 && relX < panel.getWidth() - 32) {
                setSketchToolActive(!isSketchToolActive());
                repaint();
                if (editor) editor->nvgSurface.renderAll();
                return true;
            }

            // Right ~36px: 🎙️ Mic / Voice button
            if (relX >= panel.getWidth() - 36) {
                if (pd) {
                    if (auto* br = pd->getMCPBridge()) {
                        if (br->isVoiceCapturing())
                            br->stopVoiceCaptureAndAnalyze();
                        else
                            br->startVoiceCapture(60);
                        repaint();
                        if (editor) editor->nvgSurface.renderAll();
                    }
                }
                return true;
            }
            // Middle: Text field focus!
            mcpSelectionPill->grabKeyboardFocus();
            return true;
        }
    }

    if (!pd || pd->getMcpAnnotations().empty()) return false;
    auto const pt = canvasPt.toFloat();
    auto anns = pd->getMcpAnnotations();
    for (int i = static_cast<int>(anns.size()) - 1; i >= 0; --i) {
        auto const& a = anns[static_cast<size_t>(i)];
        auto const tb = getAnnotationScreenBounds(static_cast<size_t>(i));
        if (tb.isEmpty()) continue;

        auto const chips = getAnnotationPivotChips(a.text, a.kind);
        float totalChipsW = 0.0f;
        for (auto const& c : chips) totalChipsW += getChipWidth(c.label) + 4.0f;
        float const drawerW = std::max(tb.getWidth(), totalChipsW + 4.0f);
        constexpr float drawerH = 22.0f;
        juce::Rectangle<float> const combinedBounds(tb.getX(), tb.getY(), drawerW, tb.getHeight() + drawerH + 3.0f);

        if (!combinedBounds.contains(pt)) continue;

        // 1. Rightmost ~16px of the tape header is the dismiss "x"
        juce::Rectangle<float> closeZone(tb.getRight() - 16.0f, tb.getY(), 16.0f, tb.getHeight());
        if (closeZone.contains(pt)) {
            if (mcpNoteEditor) mcpNoteEditor->setVisible(false);
            mcpNoteEditIndex = -1;
            juce::ScopedLock sl(pd->mcpOverlayLock);
            if (i < static_cast<int>(pd->mcpAnnotations.size()))
                pd->mcpAnnotations.erase(pd->mcpAnnotations.begin() + i);
            repaint();
            if (editor) editor->nvgSurface.renderAll();
            return true;
        }

        // 2. Click inside the Pivot Chips drawer (pt.y > tb.getBottom())
        if (pt.y > tb.getBottom() + 1.0f) {
            float curChipX = tb.getX() + 4.0f;
            float const chipY = tb.getY() + tb.getHeight() + 2.5f;
            float const chipH = drawerH - 5.0f;
            for (size_t ci = 0; ci < chips.size(); ++ci) {
                float const cw = getChipWidth(chips[ci].label);
                juce::Rectangle<float> chipBox(curChipX, chipY, cw, chipH);
                if (chipBox.contains(pt)) {
                    auto const& chip = chips[ci];
                    if (chip.prompt == "__undo__") {
                        undo();
                        mcpNoteHover = -1;
                        mcpNoteHoverChip = -1;
                        repaint();
                        if (editor) editor->nvgSurface.renderAll();
                        return true;
                    }
                    if (chip.prompt == "__reply__") {
                        // Open direct inline reply editor right below the tape!
                        // Select target objects first so context is wired
                        if (a.targetIds.size() > 0 || a.targetId.isNotEmpty()) {
                            deselectAll();
                            auto const ids = a.targetIds.size() > 0 ? a.targetIds : juce::StringArray(a.targetId);
                            for (auto const& tid : ids) {
                                if (auto* obj = findObjectByStableId(tid)) setSelected(obj, true);
                            }
                        }
                        if (!mcpNoteEditor) {
                            mcpNoteEditor = std::make_unique<juce::TextEditor>();
                            mcpNoteEditor->setMultiLine(false);
                            mcpNoteEditor->setReturnKeyStartsNewLine(false);
                            mcpNoteEditor->setWantsKeyboardFocus(true);
                            mcpNoteEditor->setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff16161c));
                            mcpNoteEditor->setColour(juce::TextEditor::textColourId, juce::Colours::white);
                            mcpNoteEditor->setColour(juce::TextEditor::outlineColourId, juce::Colour(0xff4a9eff));
                            mcpNoteEditor->setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour(0xff4a9eff));
                            mcpNoteEditor->setColour(juce::TextEditor::highlightColourId, juce::Colour(0xff4a9eff));
                            mcpNoteEditor->setColour(juce::CaretComponent::caretColourId, juce::Colours::white);
                            addAndMakeVisible(*mcpNoteEditor);
                        }
                        mcpNoteEditIndex = i;
                        mcpNoteOpenedAt = juce::Time::getMillisecondCounter();
                        mcpNoteEditor->onReturnKey = [this, targetId = a.targetId, targetIds = a.targetIds] {
                            if (mcpNoteEditor && pd) {
                                juce::String const replyTxt = mcpNoteEditor->getText().trim();
                                if (replyTxt.isNotEmpty()) {
                                    if (targetIds.size() > 0 || targetId.isNotEmpty()) {
                                        deselectAll();
                                        auto const ids = targetIds.size() > 0 ? targetIds : juce::StringArray(targetId);
                                        for (auto const& tid : ids) {
                                            if (auto* obj = findObjectByStableId(tid)) setSelected(obj, true);
                                        }
                                    }
                                    sendPillPrompt(replyTxt, false);
                                    if (editor && editor->sidebar) {
                                        if (auto* cp = editor->sidebar->getCopilotPanel())
                                            cp->receiveMessage("user", replyTxt);
                                    }
                                    if (auto* br = pd->getMCPBridge())
                                        br->sendArtistNote(targetId, replyTxt);
                                }
                            }
                            mcpNoteEditIndex = -1;
                            if (mcpNoteEditor) mcpNoteEditor->setVisible(false);
                            repaint();
                            if (editor) editor->nvgSurface.renderAll();
                        };
                        mcpNoteEditor->onEscapeKey = [this] {
                            mcpNoteEditIndex = -1;
                            if (mcpNoteEditor) mcpNoteEditor->setVisible(false);
                            repaint();
                            if (editor) editor->nvgSurface.renderAll();
                        };
                        mcpNoteEditor->onFocusLost = [this] {
                            if (juce::Time::getMillisecondCounter() - mcpNoteOpenedAt < 500) return;
                            mcpNoteEditIndex = -1;
                            if (mcpNoteEditor) mcpNoteEditor->setVisible(false);
                            repaint();
                            if (editor) editor->nvgSurface.renderAll();
                        };
                        mcpNoteEditor->setBounds(juce::roundToInt(tb.getX()),
                                                 juce::roundToInt(tb.getY() + tb.getHeight() + 2.0f),
                                                 juce::roundToInt(std::max(tb.getWidth(), 240.0f)), 22);
                        mcpNoteEditor->setText(juce::String(), false);
                        mcpNoteEditor->setTextToShowWhenEmpty("Reply to Copilot on this module... (Enter sends)", juce::Colour(0xff7088a0));
                        mcpNoteEditor->setVisible(true);
                        mcpNoteEditor->toFront(true);
                        {
                            auto* ed = mcpNoteEditor.get();
                            juce::MessageManager::callAsync([ed] { if (ed) ed->grabKeyboardFocus(); });
                        }
                        repaint();
                        if (editor) editor->nvgSurface.renderAll();
                        return true;
                    }

                    // Sound pivot chip clicked:
                    // 1. Select the module objects
                    if (a.targetIds.size() > 0 || a.targetId.isNotEmpty()) {
                        deselectAll();
                        auto const ids = a.targetIds.size() > 0 ? a.targetIds : juce::StringArray(a.targetId);
                        for (auto const& tid : ids) {
                            if (auto* obj = findObjectByStableId(tid)) setSelected(obj, true);
                        }
                    }
                    // 2. Dispatch prompt
                    sendPillPrompt(chip.prompt, false);
                    // 3. Log to Sidebar chat
                    if (editor && editor->sidebar) {
                        if (auto* cp = editor->sidebar->getCopilotPanel())
                            cp->receiveMessage("user", chip.prompt);
                    }
                    mcpNoteHover = -1;
                    mcpNoteHoverChip = -1;
                    repaint();
                    if (editor) editor->nvgSurface.renderAll();
                    return true;
                }
                curChipX += cw + 4.0f;
            }
            return true;
        }

        // 3. Body click on tape header:
        // Select module objects & open note in Sidebar Master Notebook
        if (a.targetIds.size() > 0 || a.targetId.isNotEmpty()) {
            deselectAll();
            auto const ids = a.targetIds.size() > 0 ? a.targetIds : juce::StringArray(a.targetId);
            for (auto const& tid : ids) {
                if (auto* obj = findObjectByStableId(tid)) setSelected(obj, true);
            }
        }
        if (editor && editor->sidebar) {
            editor->sidebar->showSidebar(true);
            editor->sidebar->showPanel(Sidebar::SidePanel::CopilotPan);
            if (auto* cp = editor->sidebar->getCopilotPanel()) {
                cp->showAnnotationNote(a.text, a.kind, a.targetIds);
            }
        }
        repaint();
        if (editor) editor->nvgSurface.renderAll();
        return true;
    }
    return false;
}

void Canvas::mouseDown(MouseEvent const& e)
{
    if (isGraph)
        return;

    PopupMenu::dismissAllActiveMenus();

    if (checkPanDragMode())
        return;

    if (objectsDistributeResizer) {
        objectsDistributeResizer.reset();
    }

    auto* source = e.originalComponent;

    // Paste slot: click inside the frame = paste now; click outside = cancel it.
    if (mcpImageSlotActive && source == this) {
        auto const pos = e.getEventRelativeTo(this).getPosition();
        juce::Rectangle<int> const slot(mcpImageSlotPos.x - 160, mcpImageSlotPos.y - 110, 320, 220);
        if (slot.contains(pos)) {
            pasteImageFromClipboard();
        } else {
            mcpImageSlotActive = false;
            repaint();
            if (editor) editor->nvgSurface.renderAll();
        }
        return;
    }

    // The pill/chisel and its chips float ABOVE everything: their clicks must beat
    // the pen, the reference images and the lasso. Without this, an armed pen
    // swallows [↩ Undo] / [🗑 Clear] clicks as new strokes.
    if (source == this && !e.mods.isRightButtonDown() && handleNoteClick(e.getPosition())) return;

    // Voice take chips float above the glass: ▶ audition / ✕ / drag.
    if (source == this && !e.mods.isRightButtonDown() && handleVoiceTakeClick(e, e.getEventRelativeTo(this).getPosition())) return;

    // PRD Phase 2: reference images on the canvas glass — drag to move, right-click
    // for the menu. In edit mode an armed pen draws over them; in Studio mode they
    // stay grabbable (Alt forces drawing, even there).
    bool const forceDraw = e.mods.isAltDown();
    bool const penBlocksImages = !mcpStudioMode && (forceDraw || mcpSketchToolActive || mcpEraserToolActive);
    if (source == this && !penBlocksImages) {
        if (handleReferenceImageClick(e, e.getEventRelativeTo(this).getPosition(), e.mods.isRightButtonDown())) return;
    }

    // Left-click
    if (!e.mods.isRightButtonDown()) {

        // Type tool (T): click the glass to drop a console-tape note at the cursor.
        if (mcpTextToolActive && source == this) {
            createTextNoteAt(e.getEventRelativeTo(this).getPosition());
            return;
        }

        // Multimodal Pen / Sketch overlay: Alt-drag, active pen, or eraser tool
        if (shouldShowAISketch() && (e.mods.isAltDown() || mcpSketchToolActive || mcpEraserToolActive)) {
            // Two lanes: with no selection, the ink is moodboard material (the
            // chisel takes over the glass). With a selection, the ink is a
            // surgical annotation — the selection IS kept and the pill stays.
            isSketching = true;
            mcpEraseGesture = mcpEraserToolActive && !e.mods.isAltDown();
            mcpSketchAlpha = 1.0f;
            if (mcpSketchFadeTimer) mcpSketchFadeTimer->stopTimer();
            mcpCurrentStroke.clear();
            if (!mcpEraseGesture) {
                auto const pt = e.getPosition();
                mcpCurrentStroke.push_back({static_cast<float>(pt.x), static_cast<float>(pt.y), juce::Time::getMillisecondCounterHiRes()});
            }
            repaint();
            if (editor) editor->nvgSurface.renderAll();
            return;
        }

        if (source == this) {
            // Mood lane: a click on empty glass opens the pill as a "New Mood"
            // palette (same tools + prompt) when there is no selection and no ink.
            if (getSelectionOfType<Object>().empty() && mcpCompletedStrokes.empty()) {
                mcpMoodPillActive = true;
                mcpMoodPillPos = e.getPosition();
                updateSelectionPill();
            }

            // Pill / note clicks were already handled above (before the pen branch).
            if (false && pd && !pd->getMcpAnnotations().empty()) {
                auto const pt = e.getPosition().toFloat();
                auto anns = pd->getMcpAnnotations();
                for (int i = static_cast<int>(anns.size()) - 1; i >= 0; --i) {
                    auto const& a = anns[static_cast<size_t>(i)];
                    float const tx = canvasOrigin.x + a.x - 6.0f;
                    float const ty = canvasOrigin.y + a.y - 9.0f;
                    float const tw = a.text.length() * 7.0f + 30.0f; // estimated hit width
                    if (Rectangle<float>(tx, ty, tw, 18.0f).contains(pt)) {
                        // Click a note to reply; notes auto-fade, so there is no dismiss
                        // gesture (no "x" to fight with).
                        {
                            // PRD overlay: click a note -> reply inline (becomes your message to the AI)
                            if (!mcpNoteEditor) {
                                mcpNoteEditor = std::make_unique<juce::TextEditor>();
                                mcpNoteEditor->setMultiLine(false);
                                mcpNoteEditor->setReturnKeyStartsNewLine(false);
                                mcpNoteEditor->setWantsKeyboardFocus(true);
                                // Commit: write the text back onto the note and forward it to the AI.
                                auto commit = [this](bool send) {
                                    if (mcpNoteEditIndex < 0) return;
                                    juce::String txt = mcpNoteEditor ? mcpNoteEditor->getText().trim() : juce::String();
                                    juce::String targetId;
                                    if (pd) {
                                        juce::ScopedLock sl(pd->mcpOverlayLock);
                                        if (mcpNoteEditIndex < static_cast<int>(pd->mcpAnnotations.size())) {
                                            auto& ann = pd->mcpAnnotations[static_cast<size_t>(mcpNoteEditIndex)];
                                            ann.text = txt;
                                            ann.kind = "artist"; // it's the artist's note now
                                            targetId = ann.targetId;
                                        }
                                    }
                                    mcpNoteEditIndex = -1;
                                    if (mcpNoteEditor) mcpNoteEditor->setVisible(false);
                                    if (send && txt.isNotEmpty() && pd) {
                                        if (auto* br = pd->getMCPBridge()) br->sendArtistNote(targetId, txt);
                                    }
                                    repaint();
                                };
                                mcpNoteEditor->onReturnKey = [commit] { commit(true); };
                                // Never lose typing: also commit when focus leaves the field —
                                // but ignore the focus-loss that fires right after opening
                                // (the field doesn't have focus yet, so it would self-close).
                                mcpNoteEditor->onFocusLost = [this, commit] {
                                    if (juce::Time::getMillisecondCounter() - mcpNoteOpenedAt < 500) return;
                                    commit(true);
                                };
                                // Live feedback: write the field's text onto the NOTE as you type
                                // (the note is GPU-drawn, so it updates reliably every keystroke).
                                mcpNoteEditor->onTextChange = [this] {
                                    if (mcpNoteEditor && pd && mcpNoteEditIndex >= 0) {
                                        juce::ScopedLock sl(pd->mcpOverlayLock);
                                        if (mcpNoteEditIndex < static_cast<int>(pd->mcpAnnotations.size()))
                                            pd->mcpAnnotations[static_cast<size_t>(mcpNoteEditIndex)].text = mcpNoteEditor->getText();
                                    }
                                    repaint();
                                    if (editor) editor->nvgSurface.renderAll();
                                };
                                mcpNoteEditor->onEscapeKey = [this] {
                                    mcpNoteEditIndex = -1;
                                    if (mcpNoteEditor) mcpNoteEditor->setVisible(false);
                                };
                                mcpNoteEditor->setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff16161c));
                                mcpNoteEditor->setColour(juce::TextEditor::textColourId, juce::Colours::white);
                                mcpNoteEditor->setColour(juce::TextEditor::outlineColourId, juce::Colour(0xff4a9eff));
                                mcpNoteEditor->setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour(0xff4a9eff));
                                mcpNoteEditor->setColour(juce::TextEditor::highlightColourId, juce::Colour(0xff4a9eff));
                                mcpNoteEditor->setColour(juce::CaretComponent::caretColourId, juce::Colours::white);
                                addAndMakeVisible(*mcpNoteEditor);
                            }
                            mcpNoteEditIndex = i;
                            mcpNoteOpenedAt = juce::Time::getMillisecondCounter(); // arm the grace period
                            // Place the editor ABOVE the note so the note (and its "x") stay clickable.
                            mcpNoteEditor->setBounds(juce::roundToInt(canvasOrigin.x + a.x - 6.0f),
                                                     juce::roundToInt(canvasOrigin.y + a.y - 34.0f),
                                                     juce::jmax(140, juce::roundToInt(a.text.length() * 7.0f) + 40), 20);
                            mcpNoteEditor->setText(a.text, false);
                            mcpNoteEditor->setVisible(true);
                            mcpNoteEditor->toFront(true);
                            // Grab focus AFTER the click finishes — grabbing during mouseDown
                            // gets overridden by the canvas, so typed keys would go nowhere.
                            {
                                auto* ed = mcpNoteEditor.get();
                                juce::MessageManager::callAsync([ed] {
                                    if (ed) { ed->grabKeyboardFocus(); ed->selectAll(); }
                                });
                            }
                            repaint();
                            return;
                        }
                    }
                }
            }
            dragState.duplicateOffset = { 0, 0 };
            dragState.lastDuplicateOffset = { 0, 0 };
            dragState.wasDuplicated = false;
            cancelConnectionCreation();

            if (SettingsFile::getInstance()->getProperty<bool>("cmd_click_switches_mode")) {
                if (e.mods.isCommandDown()) {
                    // Lock if cmd + click on canvas
                    deselectAll();

                    presentationMode.setValue(false);

                    // when command + click on canvas, swap between locked / edit mode
                    locked.setValue(!locked.getValue());
                    locked.getValueSource().sendChangeMessage(true);

                    updateOverlays();
                }
            }

            if (!e.mods.isShiftDown()) {
                deselectAll();
            }

            if (!(e.source.isTouch() && e.source.getIndex() != 0) && !getValue<bool>(locked)) {
                lasso.beginLasso(e.getEventRelativeTo(this), this);
                isDraggingLasso = true;
            }
        }

        // Update selected object in sidebar when we click a object
        if (source && source->findParentComponentOfClass<Object>()) {
            updateSidebarSelection();
        }

        editor->updateCommandStatus();
    }
    // Right click
    else {
        if (shouldShowAISketch() && (mcpSketchToolActive || mcpEraserToolActive)) {
            // Right-drag eraser — selection stays (surgical lane).
            isSketching = true;
            mcpEraseGesture = true; // right-drag always erases
            mcpSketchAlpha = 1.0f;
            if (mcpSketchFadeTimer) mcpSketchFadeTimer->stopTimer();
            mcpCurrentStroke.clear();
            repaint();
            if (editor) editor->nvgSurface.renderAll();
            return;
        }
        Dialogs::showCanvasRightClickMenu(this, source, e.getScreenPosition());
    }
}

bool Canvas::hitTest(int const x, int const y)
{
    // allow panning to happen anywhere, even when in presentation mode
    if (panningModifierDown())
        return true;

    // disregard mouse drag if outside of patch
    if (::getValue<bool>(presentationMode)) {
        if (isPointOutsidePluginArea(Point<int>(x, y)))
            return false;
    }
    return true;
}

void Canvas::mouseDrag(MouseEvent const& e)
{
    // Voice take chip drag.
    if (mcpDraggingTakeId.isNotEmpty()) {
        auto const pt = e.getEventRelativeTo(this).getPosition();
        auto const pdPos = pt - mcpTakeDragOffset - canvasOrigin;
        if (pd) pd->setMcpVoiceTakePos(mcpDraggingTakeId, static_cast<float>(pdPos.x), static_cast<float>(pdPos.y));
        repaint();
        if (editor) editor->nvgSurface.renderAll();
        return;
    }

    // PRD Phase 2: dragging a reference image across the canvas glass.
    if (mcpDraggingRefImageId.isNotEmpty()) {
        auto const pt = e.getEventRelativeTo(this).getPosition();
        auto const pdPos = pt - mcpRefImageDragOffset - canvasOrigin;
        if (pd) pd->moveMcpReferenceImage(mcpDraggingRefImageId, static_cast<float>(pdPos.x), static_cast<float>(pdPos.y));
        repaint();
        if (editor) editor->nvgSurface.renderAll();
        return;
    }

    if (isSketching) {
        auto const pt = e.getPosition();

        // ERASER MODE: dedicated eraser tool, Ctrl-drag, or Right-drag hit-tests strokes
        if (mcpEraseGesture || e.mods.isCtrlDown() || e.mods.isRightButtonDown()) {
            float eraseRadius = 25.0f;
            bool erasedAny = false;
            for (auto it = mcpCompletedStrokes.begin(); it != mcpCompletedStrokes.end(); ) {
                bool hit = false;
                for (auto const& p : *it) {
                    float dx = p.x - pt.x;
                    float dy = p.y - pt.y;
                    if (dx*dx + dy*dy < eraseRadius*eraseRadius) {
                        hit = true;
                        break;
                    }
                }
                if (hit) {
                    it = mcpCompletedStrokes.erase(it);
                    erasedAny = true;
                } else {
                    ++it;
                }
            }
            if (erasedAny) {
                updateSelectionPill();
                repaint();
                if (editor) editor->nvgSurface.renderAll();
            }
            return;
        }

        mcpCurrentStroke.push_back({static_cast<float>(pt.x), static_cast<float>(pt.y), juce::Time::getMillisecondCounterHiRes()});
        repaint();
        if (editor) editor->nvgSurface.renderAll();
        return;
    }

    if (canvasRateReducer.tooFast() || panningModifierDown())
        return;

    if (connectingWithDrag) {
        for (auto* obj : objects) {
            for (auto const& iolet : obj->iolets) {
                iolet->mouseDrag(e.getEventRelativeTo(iolet));
            }
        }
    }

    // Ignore on graphs or when locked
    if ((isGraph || locked == var(true) || commandLocked == var(true)) && !ObjectBase::isBeingEdited()) {
        bool hasToggled = false;

        // Behaviour for dragging over toggles, bang and radiogroup to toggle them
        for (auto const* object : objects) {
            if (!object->getBounds().contains(e.getEventRelativeTo(this).getPosition()) || !object->gui)
                continue;

            if (auto* obj = object->gui.get()) {
                obj->toggleObject(e.getEventRelativeTo(obj).getPosition());
                hasToggled = true;
                break;
            }
        }

        if (!hasToggled) {
            for (auto const* object : objects) {
                if (auto* obj = object->gui.get()) {
                    obj->untoggleObject();
                }
            }
        }

        return;
    }

    auto const viewportEvent = e.getEventRelativeTo(viewport.get());
    if (viewport && !ObjectBase::isBeingEdited() && autoscroll(viewportEvent)) {
        beginDragAutoRepeat(25);
    }

    // Drag lasso
    if (!(e.source.isTouch() && e.source.getIndex() != 0)) {
        lasso.dragLasso(e);
        lasso.setBounds(lasso.getBounds().withWidth(jmax(2, lasso.getWidth())).withHeight(jmax(2, lasso.getHeight())));
    }
}

bool Canvas::autoscroll(MouseEvent const& e)
{
    if (!viewport)
        return false;

    auto x = viewport->getViewPositionX();
    auto y = viewport->getViewPositionY();
    auto const oldY = y;
    auto const oldX = x;

    auto const pos = e.getPosition();

    if (pos.x > viewport->getWidth()) {
        x += std::clamp((pos.x - viewport->getWidth()) / 6, 1, 14);
    } else if (pos.x < 0) {
        x -= std::clamp(-pos.x / 6, 1, 14);
    }
    if (pos.y > viewport->getHeight()) {
        y += std::clamp((pos.y - viewport->getHeight()) / 6, 1, 14);
    } else if (pos.y < 0) {
        y -= std::clamp(-pos.y / 6, 1, 14);
    }

    if (x != oldX || y != oldY) {
        viewport->setViewPosition(x, y);
        return true;
    }

    return false;
}

void Canvas::mouseUp(MouseEvent const& e)
{
    // Finish a voice take chip drag.
    if (mcpDraggingTakeId.isNotEmpty()) {
        mcpDraggingTakeId.clear();
        setMouseCursor(juce::MouseCursor::NormalCursor);
        repaint();
        if (editor) editor->nvgSurface.renderAll();
        return;
    }

    // PRD Phase 2: finish a reference-image drag.
    if (mcpDraggingRefImageId.isNotEmpty()) {
        mcpDraggingRefImageId.clear();
        setMouseCursor(juce::MouseCursor::NormalCursor);
        repaint();
        if (editor) editor->nvgSurface.renderAll();
        return;
    }

    if (isSketching) {
        isSketching = false;
        bool const wasEraseGesture = mcpEraseGesture;
        mcpEraseGesture = false;
        if (mcpCurrentStroke.size() >= 2) {
            mcpCompletedStrokes.push_back(mcpCurrentStroke);
            juce::Array<juce::var> ptsVar;
            for (auto const& p : mcpCurrentStroke) {
                auto* obj = new juce::DynamicObject();
                obj->setProperty("x", p.x - static_cast<float>(canvasOrigin.x));
                obj->setProperty("y", p.y - static_cast<float>(canvasOrigin.y));
                obj->setProperty("t", p.t);
                ptsVar.add(juce::var(obj));
            }
            auto jsonStr = juce::JSON::toString(juce::var(ptsVar), true);
            bool const isFastPath = e.mods.isShiftDown();
            if (pd) {
                if (auto* br = pd->getMCPBridge()) {
                    auto const subpatch = patch.getPointer() ? juce::String(reinterpret_cast<uintptr_t>(patch.getPointer().get())) : juce::String("0");
                    br->sendSketchStroke(subpatch, jsonStr, isFastPath);
                }
            }
            mcpCurrentStroke.clear();
            // startSketchFadeTimer(); // PERSISTENT INK
        } else {
            mcpCurrentStroke.clear();
            // A TAP (not a drag) opens the mood palette — even with the pen armed.
            // Studio entry auto-arms the pen, so this is how you call up the pill.
            if (!wasEraseGesture && mcpCompletedStrokes.empty() && getSelectionOfType<Object>().empty()) {
                mcpMoodPillActive = true;
                mcpMoodPillPos = e.getPosition();
            }
        }
        // Ink persists -> anchor the Moodboard Chisel under the fresh bbox.
        updateSelectionPill();
        repaint();
        if (editor) editor->nvgSurface.renderAll();
        return;
    }

    setPanDragMode(false);
    setMouseCursor(MouseCursor::NormalCursor);

    connectionCancelled = false;

    // Double-click canvas to create new object (ignore clicks on AI notes)
    if (!isPointOverNote(e.getPosition()) && e.mods.isLeftButtonDown() && e.getNumberOfClicks() == 2 && e.originalComponent == this && !isGraph && !getValue<bool>(locked)) {
        auto* newObject = objects.add(this, "", e.getPosition());
        deselectAll();
        setSelected(newObject, true); // Select newly created object
    }

    // Make sure the drag-over toggle action is ended
    if (!isDraggingLasso) {
        for (auto const* object : objects) {
            if (auto* obj = object->gui.get()) {
                obj->untoggleObject();
            }
        }
    }

    updateSidebarSelection();

    editor->updateCommandStatus();

    lasso.endLasso();
    isDraggingLasso = false;
    for (auto* object : objects)
        object->originalBounds = Rectangle<int>(0, 0, 0, 0);

    if (connectingWithDrag) {
        for (auto* obj : objects) {
            for (auto const& iolet : obj->iolets) {
                auto relativeEvent = e.getEventRelativeTo(this);
                if (iolet->getCanvasBounds().expanded(20).contains(relativeEvent.getPosition())) {
                    iolet->mouseUp(relativeEvent);
                }
            }
        }
    }
}

void Canvas::updateSidebarSelection()
{
    // Post to message queue so that sidebar parameters are updated AFTER objects resize is run
    // Otherwise position XY is not populated

    MessageManager::callAsync([_this = SafePointer(this), this] {
        if (!_this)
            return;

        auto lassoSelection = getSelectionOfType<Object>();
        SmallArray<ObjectParameters, 6> allParameters;
        auto toShow = SmallArray<Component*>();

        bool showOnSelect = false;

        for (auto* object : lassoSelection) {
            if (!object->gui)
                continue;
            auto parameters = object->gui ? object->gui->getParameters() : ObjectParameters();
            showOnSelect = object->gui && object->gui->showParametersWhenSelected();
            allParameters.add(parameters);
            toShow.add(object);
        }

        editor->sidebar->showParameters(toShow, allParameters, showOnSelect);
        updateSelectionPill();
    });
}

bool Canvas::keyPressed(KeyPress const& key)
{
    // Paste-slot fast path: Ctrl/Cmd+V pastes the clipboard image directly,
    // independent of the global Paste command and its focus rules.
    if (mcpImageSlotActive) {
        auto const kc = key.getKeyCode();
        bool const ctrlV = (key.getModifiers().isCtrlDown() || key.getModifiers().isCommandDown())
            && (kc == 'v' || kc == 'V' || kc == 22 /* Ctrl+V control code */ || key.getTextCharacter() == 'v');
        if (ctrlV) {
            pasteImageFromClipboard();
            return true;
        }
    }

    if (mcpSelectionPill && mcpSelectionPill->hasKeyboardFocus(true))
        return false;

    // Tier-2 approval: Enter approves the pending destructive op, Esc cancels.
    if (isApprovalRequested()) {
        if (key == juce::KeyPress::returnKey) {
            sendApproval(true);
            return true;
        }
        if (key == juce::KeyPress::escapeKey) {
            sendApproval(false);
            return true;
        }
    }

    if (editor->getCurrentCanvas() != this || isGraph)
        return false;

    // Paste slot: Esc cancels the waiting frame.
    if (mcpImageSlotActive && key.getKeyCode() == juce::KeyPress::escapeKey) {
        mcpImageSlotActive = false;
        repaint();
        if (editor) editor->nvgSurface.renderAll();
        return true;
    }

    // Moodboard tools escape hatch: Esc always disarms the tool first, so the
    // artist is never trapped drawing/typing when the chisel replaced the pill.
    if ((mcpSketchToolActive || mcpEraserToolActive || mcpTextToolActive) && key.getKeyCode() == juce::KeyPress::escapeKey) {
        setSketchToolActive(false);
        setEraserToolActive(false);
        setTextToolActive(false);
        return true;
    }

    // Moodboard hotkeys (Lane 1): T arms the type tool, Tab flips Pen <-> Eraser,
    // Z / Ctrl+Z pops the last ink stroke. Esc exits (handled above).
    bool const moodboardArmed = mcpSketchToolActive || mcpEraserToolActive || mcpTextToolActive || mcpStudioMode || mcpMoodPillActive;
    if (moodboardArmed) {
        if (key.getKeyCode() == 't' || key.getKeyCode() == 'T') {
            setTextToolActive(!mcpTextToolActive);
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::tabKey) {
            if (mcpSketchToolActive) setEraserToolActive(true);
            else setSketchToolActive(true);
            return true;
        }
        if (key.getKeyCode() == 'z' || key.getKeyCode() == 'Z') {
            if (!mcpCompletedStrokes.empty()) {
                undoLastStroke();
                return true;
            }
        }
    }

    // Studio brainstorm: Enter commits from the keyboard — no mouse travel to the
    // sidebar. Shift+Enter PARKS the intent in the bay without waking the AI.
    if (mcpStudioMode && key.getKeyCode() == juce::KeyPress::returnKey
        && (key.getModifiers().isCtrlDown() || key.getModifiers().isShiftDown() || key.getModifiers().isCommandDown())) {
        bool const cmdCombo = key.getModifiers().isCtrlDown() || key.getModifiers().isCommandDown();
        if (key.getModifiers().isShiftDown() && !cmdCombo) {
            parkSelectionPill(); // Shift = park (no AI)
        } else {
            commitStudioFromCanvas(); // Ctrl/Cmd+Enter = send
        }
        return true;
    }

    // Fast-focus / hotkey handling when the selection pill is visible but unfocused:
    // - '/' opens the smart command palette or tools menu
    // - Tab or Enter focuses the prompt text field immediately
    // - Esc dismisses the pill
    // All other keys (o, m, c, b, Delete, arrows) pass through to the canvas patch!
    if (mcpSelectionPill && mcpSelectionPill->isVisible()) {
        if (key.getTextCharacter() == '/' || key.getKeyCode() == '/') {
            showPillToolsMenu();
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::tabKey || key.getKeyCode() == juce::KeyPress::returnKey) {
            // Mood strip has no prompt field — focus keys do nothing there.
            if (mcpPillSketchMode && mcpCompletedStrokes.empty()) return true;
            mcpSelectionPill->grabKeyboardFocus();
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::escapeKey) {
            dismissSelectionPill();
            return true;
        }
    }

    int const keycode = key.getKeyCode();

    auto moveSelection = [this](int const x, int const y) {
        auto objects = getSelectionOfType<Object>();
        if (objects.empty())
            return false;

        SmallArray<t_gobj*> pdObjects;
        for (auto const* object : objects) {
            if (auto* ptr = object->getPointer()) {
                pdObjects.add(ptr);
            }
        }

        patch.moveObjects(pdObjects, x, y);

        // Update object bounds and store the total bounds of the selection
        auto totalBounds = Rectangle<int>();
        for (auto* object : objects) {
            object->updateBounds();
            totalBounds = totalBounds.getUnion(object->getBounds());
        }

        // TODO: consider calculating the totalBounds with object->getBounds().reduced(Object::margin)
        // then adding viewport padding in screen pixels so it's consistent regardless of scale
        auto const scale = ::getValue<float>(zoomScale);
        constexpr auto viewportPadding = 10;

        auto viewX = viewport->getViewPositionX() / scale;
        auto viewY = viewport->getViewPositionY() / scale;
        auto const viewWidth = (viewport->getWidth() - viewportPadding) / scale;
        auto const viewHeight = (viewport->getHeight() - viewportPadding) / scale;
        if (x < 0 && totalBounds.getX() < viewX) {
            viewX = totalBounds.getX();
        } else if (totalBounds.getRight() > viewX + viewWidth) {
            viewX = totalBounds.getRight() - viewWidth;
        }
        if (y < 0 && totalBounds.getY() < viewY) {
            viewY = totalBounds.getY();
        } else if (totalBounds.getBottom() > viewY + viewHeight) {
            viewY = totalBounds.getBottom() - viewHeight;
        }
        viewport->setViewPosition(viewX * scale, viewY * scale);
        return true;
    };

    // Cancel connections being created by ESC key
    if (keycode == KeyPress::escapeKey && !connectionsBeingCreated.empty()) {
        cancelConnectionCreation();
        return true;
    }

    // Move objects with arrow keys
    int moveDistance = objectGrid.gridSize;
    if (key.getModifiers().isShiftDown()) {
        moveDistance = 1;
    } else if (key.getModifiers().isCommandDown()) {
        moveDistance *= 4;
    }

    if (keycode == KeyPress::leftKey) {
        moveSelection(-moveDistance, 0);
        return false;
    }
    if (keycode == KeyPress::rightKey) {
        moveSelection(moveDistance, 0);
        return false;
    }
    if (keycode == KeyPress::upKey) {
        moveSelection(0, -moveDistance);
        return false;
    }
    if (keycode == KeyPress::downKey) {
        moveSelection(0, moveDistance);
        return false;
    }
    if (keycode == KeyPress::tabKey) {
        cycleSelection();
        return false;
    }

    return false;
}

void Canvas::deselectAll(bool const broadcastChange)
{
    if (!broadcastChange)
        selectedComponents.removeChangeListener(this);

    selectedComponents.deselectAll();
    editor->sidebar->hideParameters();
    // Fall back to the sketch pill when ink exists; otherwise this hides it.
    updateSelectionPill();

    if (!broadcastChange) {
        // Add back the listener, but make sure it's added back 'after' the last event on the message queue
        MessageManager::callAsync([this] { selectedComponents.addChangeListener(this); });
    }
}

void Canvas::hideAllActiveEditors()
{
    hideSelectionPill();
    for (auto* object : objects) {
        object->hideEditor();
    }
}

void Canvas::updateSelectionPill()
{
    if (isGraph || getValue<bool>(locked) || presentationMode.getValue()) {
        mcpPillSketchMode = false;
        hideSelectionPill();
        return;
    }

    // NOTE: Studio mode deliberately does NOT suppress the pill anymore — the
    // drawer is the cart/commit surface, while the pill is the workbench:
    // empty glass → mood palette, selection → surgical palette, ink → chisel.

    auto selectedObjects = getSelectionOfType<Object>();
    // The chisel owns the canvas while ink exists, the pen/eraser is up, or a
    // fresh mood palette was opened; otherwise hide.
    if (selectedObjects.empty() && mcpCompletedStrokes.empty() && !mcpSketchToolActive && !mcpEraserToolActive && !mcpMoodPillActive) {
        mcpPillSketchMode = false;
        hideSelectionPill();
        return;
    }

    // Brief grace window after a send/dismiss so an immediate sidebar/selection
    // refresh can't re-pop the pill. Time-based (not keyed to the selection), so
    // re-selecting the SAME object later always brings it back.
    if (juce::Time::getMillisecondCounter() < mcpSelectionPillHideUntil) return;

    // Phase 1B: no PD object selected but ink exists -> the pill becomes the
    // Moodboard Chisel, anchored 14px beneath the combined stroke bounding box.
    bool const sketchMode = selectedObjects.empty();
    mcpPillSketchMode = sketchMode;

    constexpr int pillH = 34;
    int pillW = 0;
    int pillX = 0;
    int pillY = 0;
    int editorX = 0;
    int editorW = 0;

    bool sketchStrip = false;
    if (sketchMode) {
        // Prefer the live ink bbox; fall back to the last one so the chisel stays
        // parked after [🗑 Clear]; fall back again to the fresh mood-palette click.
        auto const liveBounds = getSketchBoundsInCanvas();
        if (!liveBounds.isEmpty()) mcpLastSketchBounds = liveBounds;
        juce::Rectangle<float> sb = !liveBounds.isEmpty() ? liveBounds : mcpLastSketchBounds;
        if (sb.isEmpty() && mcpMoodPillActive) {
            sb = juce::Rectangle<float>(static_cast<float>(mcpMoodPillPos.x), static_cast<float>(mcpMoodPillPos.y), 1.0f, 1.0f);
        }
        if (sb.isEmpty()) {
            mcpPillSketchMode = false;
            hideSelectionPill();
            return;
        }
        // No ink yet -> compact icon strip (tool palette first, surfaces second).
        // Ink exists -> the full chisel (Build / prompt / Undo / Clear).
        sketchStrip = mcpCompletedStrokes.empty();
        pillW = sketchStrip ? 182 : 520;
        pillX = juce::roundToInt(sb.getCentreX()) - pillW / 2;
        pillY = juce::roundToInt(sb.getBottom()) + 14;
        editorX = 92; // [⚡ Build] chip
        editorW = pillW - 92 - 238; // ...prompt... [T] [🖼] [↩ Undo] [🗑 Clear] [✏️ pen]
        mcpPillDrawerMode = 0;
        mcpPillDrawerFrame = {};
        mcpPaletteFrame = {};
    } else {
        // Object::getBounds() is already in Canvas component coordinates, so the pill
        // scrolls/zooms with the patch for free (canvasOrigin is constant).
        Rectangle<int> bounds;
        bool first = true;
        for (auto const* obj : selectedObjects) {
            if (!obj) continue;
            auto const ob = obj->getBounds();
            bounds = first ? ob : bounds.getUnion(ob);
            first = false;
        }
        if (first) {
            hideSelectionPill();
            return;
        }

        pillW = 480;
        // Position purely in CANVAS content coords (same space as obj->getBounds()).
        // Do NOT clamp against viewport->getViewArea(): that returns zoomed/screen
        // coords, so at any zoom != 100% it threw the pill off-screen.
        pillX = bounds.getCentreX() - pillW / 2;
        pillY = bounds.getBottom() + 12;
        editorX = 36;
        editorW = pillW - 36 - 148; // ...prompt field... [attach ✏ T 🖼] [pen] [mic]
    }

    mcpSelectionPillFrame = juce::Rectangle<int>(pillX, pillY, pillW, pillH);
    if (!sketchMode) {
        if (mcpPillDrawerMode != 0) {
            mcpPillDrawerFrame = juce::Rectangle<int>(pillX, pillY + pillH + 5, pillW, 30);
        } else {
            mcpPillDrawerFrame = {};
        }
    }

    if (!mcpSelectionPill) {
        mcpSelectionPill = std::make_unique<juce::TextEditor>();
        mcpSelectionPill->getProperties().set("NoBackground", true);
        mcpSelectionPill->getProperties().set("NoOutline", true);
        mcpSelectionPill->setMultiLine(false);
        mcpSelectionPill->setReturnKeyStartsNewLine(false);
        mcpSelectionPill->setWantsKeyboardFocus(true);
        mcpSelectionPill->setColour(juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
        mcpSelectionPill->setColour(juce::TextEditor::textColourId, juce::Colours::white);
        mcpSelectionPill->setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        mcpSelectionPill->setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
        mcpSelectionPill->setColour(juce::TextEditor::highlightColourId, juce::Colour(0xff4a9eff));
        mcpSelectionPill->setColour(juce::CaretComponent::caretColourId, juce::Colours::white);
        mcpSelectionPill->setFont(juce::Font("Inter", 13.0f, juce::Font::plain));
        mcpSelectionPill->applyFontToAllText(juce::Font("Inter", 13.0f, juce::Font::plain));
        mcpSelectionPill->setBorder(juce::BorderSize<int>(0, 0, 0, 0));
        mcpSelectionPill->setIndents(2, 0);
        mcpSelectionPill->setJustification(juce::Justification::centredLeft);

        mcpSelectionPill->onReturnKey = [this] { submitSelectionPill(false); };
        mcpSelectionPillKeys = std::make_unique<SelectionPillKeyListener>(this);
        mcpSelectionPill->addKeyListener(mcpSelectionPillKeys.get());
        if (!mcpSlotPasteKeys) mcpSlotPasteKeys = std::make_unique<SlotPasteKeyListener>(this);
        mcpSelectionPill->addKeyListener(mcpSlotPasteKeys.get());
        mcpSelectionPill->onEscapeKey = [this] {
            if (mcpPillDrawerMode != 0) {
                mcpPillDrawerMode = 0;
                mcpPillDrawerFrame = {};
                mcpPaletteFrame = {};
                repaint();
                if (editor) editor->nvgSurface.renderAll();
                return;
            }
            if (mcpSelectionPill) mcpSelectionPill->setText(juce::String(), false);
            dismissSelectionPill();
        };
        mcpSelectionPill->onTextChange = [this] {
            if (mcpSelectionPill) {
                auto const t = mcpSelectionPill->getText().trimStart();
                if (t.startsWithChar('/')) {
                    if (mcpPillDrawerMode != 3) {
                        initPaletteItems();
                        mcpPillDrawerMode = 3;
                    }
                    filterPaletteItems(t.substring(1));
                } else if (mcpPillDrawerMode == 3) {
                    mcpPillDrawerMode = 0;
                    mcpPaletteFrame = {};
                }
            }
            repaint();
            if (editor) editor->nvgSurface.renderAll();
        };
        mcpSelectionPill->onFocusLost = [this] {
            // Keep a typed prompt, but if it's empty and drawer is closed get out of the way.
            if (mcpSelectionPill && mcpSelectionPill->getText().trim().isEmpty() && mcpPillDrawerMode == 0)
                dismissSelectionPill();
        };
        addAndMakeVisible(*mcpSelectionPill);
    }

    // Object mode: room on the left for + and on the right for 🎙️ Mic.
    // Sketch mode: room for the [⚡ Build] chip and the [↩ Undo] [🗑 Clear] pair.
    // edH = 30 gives generous headroom and descender room so text is never clipped at the bottom.
    int const edH = 30;
    int const edY = pillY + (pillH - edH) / 2;
    if (sketchStrip) {
        // Mood strip: no prompt field. Park the editor off-cells (still "visible"
        // so hiding it can't trigger a focus-loss dismiss).
        mcpSelectionPill->setBounds(pillX - 60, pillY, 1, 1);
    } else {
        mcpSelectionPill->setBounds(pillX + editorX, edY, editorW, edH);
    }
    mcpSelectionPill->setVisible(true);
    // toFront(false), NOT true: bringing it forward must never steal keyboard
    // focus, or typing 'o'/'m'/'c'/'b' on the canvas would land in the prompt
    // instead of creating/editing objects.
    mcpSelectionPill->toFront(false);
    if (!mcpPillAnimator) {
        mcpPillAnimator = std::make_unique<SelectionPillAnimator>(this);
    }
    mcpPillAnimator->startTimerHz(6);
    repaint();
    if (editor) editor->nvgSurface.renderAll();
}

void Canvas::hideSelectionPill()
{
    if (mcpPillAnimator) {
        mcpPillAnimator->stopTimer();
    }
    mcpPillDrawerMode = 0;
    mcpPillDrawerFrame = {};
    mcpPaletteFrame = {};
    if (mcpSelectionPill && mcpSelectionPill->isVisible()) {
        mcpSelectionPill->setVisible(false);
        repaint();
        if (editor) editor->nvgSurface.renderAll();
    }
}

void Canvas::dismissSelectionPill()
{
    mcpPillDrawerMode = 0;
    mcpPillDrawerFrame = {};
    mcpPaletteFrame = {};
    mcpMoodPillActive = false;
    // Artist dismissed (send / Esc / empty focus-out): suppress re-show for a
    // short window so the immediate refresh can't re-pop it. Time-based, so
    // re-selecting the same object later always brings it back.
    mcpSelectionPillHideUntil = juce::Time::getMillisecondCounter() + 700;
    hideSelectionPill();
}

// Shift+Enter: park, don't wake the AI. The typed intent becomes a tape note
// (bound to the selection when there is one); ink and images stay staged.
void Canvas::parkSelectionPill()
{
    if (!pd) {
        dismissSelectionPill();
        return;
    }

    auto const prompt = mcpSelectionPill ? mcpSelectionPill->getText().trim() : juce::String();
    if (prompt.isNotEmpty()) {
        PluginProcessor::McpAnnotation ann;
        ann.kind = "artist";
        ann.text = prompt;
        ann.t = juce::Time::getMillisecondCounterHiRes() / 1000.0;

        auto const selIds = getSelectionStableIds();
        if (selIds.size() > 0) {
            ann.targetId = selIds[0];
            ann.targetIds = selIds;
        } else {
            auto sb = getSketchBoundsInCanvas();
            if (sb.isEmpty() && mcpMoodPillActive) {
                ann.x = static_cast<float>(mcpMoodPillPos.x - canvasOrigin.x);
                ann.y = static_cast<float>(mcpMoodPillPos.y - canvasOrigin.y);
            } else {
                ann.x = sb.getCentreX() - static_cast<float>(canvasOrigin.x);
                ann.y = sb.getCentreY() - static_cast<float>(canvasOrigin.y);
            }
        }

        juce::ScopedLock sl(pd->mcpOverlayLock);
        pd->mcpAnnotations.push_back(ann);
    }

    if (mcpSelectionPill) mcpSelectionPill->setText(juce::String(), false);
    dismissSelectionPill();
    repaint();
    if (editor) editor->nvgSurface.renderAll();
}

void Canvas::submitSelectionPill(bool queueForChat)
{
    mcpPillDrawerMode = 0;
    mcpPillDrawerFrame = {};
    mcpPaletteFrame = {};
    if (!mcpSelectionPill) return;

    auto const prompt = mcpSelectionPill->getText().trim();

    // Sketch mode: Enter / Shift+Enter = Commit & Build the staged ink.
    if (mcpPillSketchMode) {
        sendSketchBuild(prompt);
        mcpSelectionPill->setText(juce::String(), false);
        // Keep the chisel parked so [↩ Undo] / [🗑 Clear] stay one tap away.
        updateSelectionPill();
        repaint();
        if (editor) editor->nvgSurface.renderAll();
        return;
    }

    if (prompt.isNotEmpty() && pd) {
        // Find selection stable tempIds & compute group bounding box
        juce::StringArray targetIds;
        bool first = true;
        int minX = 0, minY = 0, maxX = 0, maxY = 0;
        auto patchPtr = patch.getPointer();
        t_canvas* cnv = patchPtr ? patchPtr.get() : nullptr;
        for (auto* obj : getSelectionOfType<Object>()) {
            if (auto* ptr = obj->getPointer()) {
                auto tid = pd->getStableId(ptr);
                if (tid.isEmpty() && cnv) {
                    tid = pd->getOrAdoptStableId(cnv, ptr);
                }
                if (tid.isNotEmpty()) targetIds.add(tid);
            }
            auto const b = obj->getObjectBounds();
            if (first) {
                minX = b.getX(); minY = b.getY();
                maxX = b.getRight(); maxY = b.getBottom();
                first = false;
            } else {
                minX = std::min(minX, b.getX());
                minY = std::min(minY, b.getY());
                maxX = std::max(maxX, b.getRight());
                maxY = std::max(maxY, b.getBottom());
            }
        }

        // Park the prompt in the Sidebar Copilot chat log (no clutter on canvas)
        if (editor && editor->sidebar) {
            if (auto* cp = editor->sidebar->getCopilotPanel()) {
                cp->receiveMessage("user", prompt);
            }
        }

        // C++ truth: sendPillPrompt resolves the selection's stable tempIds from the
        // bridge-owned map — the server never guesses from its identity mirror.
        sendPillPrompt(prompt, queueForChat);
        mcpSelectionPill->setText(juce::String(), false);
    }

    dismissSelectionPill();
    repaint();
    if (editor) editor->nvgSurface.renderAll();
}

bool Canvas::SlotPasteKeyListener::keyPressed(const juce::KeyPress& key, juce::Component*)
{
    if (!canvas || !canvas->mcpImageSlotActive) return false;
    auto const kc = key.getKeyCode();
    bool const ctrlV = (key.getModifiers().isCtrlDown() || key.getModifiers().isCommandDown())
        && (kc == 'v' || kc == 'V' || kc == 22 /* Ctrl+V control code */);
    if (ctrlV) {
        canvas->pasteImageFromClipboard();
        return true;
    }
    return false;
}

bool Canvas::SelectionPillKeyListener::keyPressed(const juce::KeyPress& key, juce::Component*)
{    if (canvas && canvas->mcpPillDrawerMode == 3) {
        auto const count = static_cast<int>(canvas->mcpFilteredPaletteIndices.size());
        if (key.getKeyCode() == juce::KeyPress::upKey && count > 0) {
            canvas->mcpPaletteSelectedIndex = (canvas->mcpPaletteSelectedIndex - 1 + count) % count;
            canvas->repaint();
            if (canvas->editor) canvas->editor->nvgSurface.renderAll();
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::downKey && count > 0) {
            canvas->mcpPaletteSelectedIndex = (canvas->mcpPaletteSelectedIndex + 1) % count;
            canvas->repaint();
            if (canvas->editor) canvas->editor->nvgSurface.renderAll();
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::returnKey && !key.getModifiers().isShiftDown() && count > 0) {
            canvas->triggerPaletteItem(canvas->mcpPaletteSelectedIndex);
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::escapeKey) {
            canvas->mcpPillDrawerMode = 0;
            canvas->mcpPaletteFrame = {};
            canvas->repaint();
            if (canvas->editor) canvas->editor->nvgSurface.renderAll();
            return true;
        }
    }
    // Shift+Enter = Park: keep the artifacts + typed intent in the bay without
    // waking the AI. (Plain Enter is the send — TextEditor fires onReturnKey.)
    if (key.getKeyCode() == juce::KeyPress::returnKey && key.getModifiers().isShiftDown()) {
        if (canvas) canvas->parkSelectionPill();
        return true;
    }
    // "/" opens the on-demand smart command palette if input is empty
    if (key.getTextCharacter() == '/' || key.getKeyCode() == '/') {
        if (canvas && canvas->mcpSelectionPill && canvas->mcpSelectionPill->getText().trim().isEmpty()) {
            canvas->showPillToolsMenu();
            return true;
        }
    }
    // "*" (the ✦ key) opens the Lenses drawer if input is empty
    if (key.getTextCharacter() == '*') {
        if (canvas && canvas->mcpSelectionPill && canvas->mcpSelectionPill->getText().trim().isEmpty()) {
            canvas->showPillLensesMenu();
            return true;
        }
    }
    return false;
}

void Canvas::SelectionPillAnimator::timerCallback()
{
    if (canvas && canvas->mcpSelectionPill && canvas->mcpSelectionPill->isVisible()) {
        canvas->repaint();
        if (canvas->editor) canvas->editor->nvgSurface.renderAll();
    } else {
        stopTimer();
    }
}

void Canvas::showPillLensesMenu()
{
    if (!mcpSelectionPill || !mcpSelectionPill->isVisible()) return;
    mcpPillDrawerMode = (mcpPillDrawerMode == 1 ? 0 : 1);
    if (mcpPillDrawerMode != 0) {
        auto const p = mcpSelectionPillFrame.isEmpty() ? mcpSelectionPill->getBounds() : mcpSelectionPillFrame;
        mcpPillDrawerFrame = juce::Rectangle<int>(p.getX(), p.getBottom() + 5, p.getWidth(), 30);
    } else {
        mcpPillDrawerFrame = {};
    }
    repaint();
    if (editor) editor->nvgSurface.renderAll();
}

void Canvas::configurePillLookAndFeel()
{
}

void Canvas::showPillToolsMenu()
{
    showPillPalette();
}

void Canvas::initPaletteItems()
{
    if (!mcpPaletteItems.empty()) return;

    // ⚡ Actions (FastPath - 0 Tokens, 5 ms)
    mcpPaletteItems.push_back({ "tidy", "Tidy Layout", "Action", "Begradigt Kabel & Spalten (0 Tokens)", "⚡", "[@action:tidy]" });
    mcpPaletteItems.push_back({ "pack-gop", "Pack into GOP", "Action", "Erstellt Eurorack-Modul mit Faceplate", "📦", "[@action:pack-gop]" });
    mcpPaletteItems.push_back({ "array10", "10x Osc Array", "Action", "Boilerplate-Bank mit Gain-Schutz", "🔢", "[@action:array10]" });
    mcpPaletteItems.push_back({ "add-knobs", "Add Controls", "Action", "Regler fuer Signal-Inlets erzeugen", "🎛️", "[@action:add-knobs]" });
    mcpPaletteItems.push_back({ "undo", "Undo Step", "Action", "Letzte Aenderung sofort rueckgaengig", "↩️", "[@action:undo]" });

    // 🧬 Skills (AI Sounddesign)
    mcpPaletteItems.push_back({ "kick-drum", "Punchy 909 Kick", "Skill", "Pitch-Sweep & [tanh~] Saettigung", "🥁", "[@skill:kick-drum]" });
    mcpPaletteItems.push_back({ "acid-bass", "Acid 303 Bass", "Skill", "Resonanz-Filter mit Slide & Envelope", "🎛️", "[@skill:acid-bass]" });
    mcpPaletteItems.push_back({ "ambient-verb", "Ambient Space Verb", "Skill", "Schwebende Hallfahne mit Resonator", "🌌", "[@skill:ambient-verb]" });
    mcpPaletteItems.push_back({ "tape-drive", "Lo-Fi Tape Saturator", "Skill", "Analoger Bandschmutz mit Wow/Flutter", "📻", "[@skill:tape-drive]" });
    mcpPaletteItems.push_back({ "genesis", "Chaos Genesis", "Skill", "Experimentelle ELSE-Kreuzmodulation", "🧬", "[@skill:genesis]" });

    // ✦ Lenses (Personas)
    mcpPaletteItems.push_back({ "doctor", "Doctor Lens", "Lens", "Röntgenblick auf Stille, Fehler & Klicks", "🩺", "[@lens:doctor]" });
    mcpPaletteItems.push_back({ "jam", "Jam Lens", "Lens", "Schneller, kreativer Co-Produzent", "⚡", "[@lens:jam]" });
    mcpPaletteItems.push_back({ "modular", "Modular Lens", "Lens", "Eurorack CV & Voltage Routing", "🔌", "[@lens:modular]" });

    filterPaletteItems({});
}

void Canvas::filterPaletteItems(const juce::String& query)
{
    mcpFilteredPaletteIndices.clear();
    auto q = query.trim().toLowerCase();
    if (q.startsWithChar('/')) q = q.substring(1).trim();

    for (size_t i = 0; i < mcpPaletteItems.size(); ++i) {
        if (q.isEmpty() ||
            mcpPaletteItems[i].title.toLowerCase().contains(q) ||
            mcpPaletteItems[i].desc.toLowerCase().contains(q) ||
            mcpPaletteItems[i].category.toLowerCase().contains(q)) {
            mcpFilteredPaletteIndices.push_back(static_cast<int>(i));
        }
    }
    mcpPaletteSelectedIndex = 0;
}

void Canvas::showPillPalette()
{
    if (!mcpSelectionPill || !mcpSelectionPill->isVisible()) return;
    initPaletteItems();
    mcpPillDrawerMode = (mcpPillDrawerMode == 3 ? 0 : 3);
    if (mcpPillDrawerMode == 3) {
        filterPaletteItems({});
    } else {
        mcpPaletteFrame = {};
    }
    repaint();
    if (editor) editor->nvgSurface.renderAll();
}

void Canvas::triggerPaletteItem(int index)
{
    if (index >= 0 && index < static_cast<int>(mcpFilteredPaletteIndices.size())) {
        auto const& item = mcpPaletteItems[static_cast<size_t>(mcpFilteredPaletteIndices[static_cast<size_t>(index)])];
        mcpPillDrawerMode = 0;
        mcpPaletteFrame = {};

        juce::String currentText = mcpSelectionPill ? mcpSelectionPill->getText().trim() : juce::String();

        // 1-Click FastPath convenience:
        // If the text editor was completely empty and user selected a pure action (like Tidy or Undo):
        if (currentText.isEmpty() && (item.category == "Action" && item.id != "array10")) {
            sendPillPrompt(item.prompt, false);
            dismissSelectionPill();
            return;
        }

        // Composable Tag Insertion:
        // Append tag so user can either hit Enter immediately or keep typing!
        juce::String newText = currentText.isEmpty() ? (item.prompt + " ") : (currentText + " " + item.prompt + " ");
        if (mcpSelectionPill) {
            mcpSelectionPill->setText(newText, true);
            mcpSelectionPill->setCaretPosition(newText.length());
            mcpSelectionPill->grabKeyboardFocus();
        }
        repaint();
        if (editor) editor->nvgSurface.renderAll();
    }
}

void Canvas::sendPillPrompt(const juce::String& prompt, bool queueForChat)
{
    if (prompt.isEmpty() || !pd) return;
    juce::StringArray targetIds;
    auto patchPtr = patch.getPointer();
    t_canvas* cnv = patchPtr ? patchPtr.get() : nullptr;
    for (auto* obj : getSelectionOfType<Object>()) {
        if (auto* ptr = obj->getPointer()) {
            auto tid = pd->getStableId(ptr);
            if (tid.isEmpty() && cnv) {
                tid = pd->getOrAdoptStableId(cnv, ptr);
            }
            if (tid.isNotEmpty()) targetIds.add(tid);
        }
    }
    if (auto* br = pd->getMCPBridge()) br->sendSelectionPrompt(prompt, targetIds, queueForChat);
}

void Canvas::copySelection()
{
    // Tell pd to select all objects that are currently selected
    SmallArray<t_gobj*> objects;
    for (auto const* object : getSelectionOfType<Object>()) {
        if (auto* ptr = object->getPointer()) {
            objects.add(ptr);
        }
    }

    // Tell pd to copy
    patch.copy(objects);
    patch.deselectAll();
}

void Canvas::focusGained(FocusChangeType cause)
{
    pd->openedEditors.move(pd->openedEditors.indexOf(editor), 0);

    pd->enqueueFunctionAsync([pd = this->pd, patchPtr = patch.getUncheckedPointer(), hasFocus = static_cast<float>(hasKeyboardFocus(true))] {
        auto* activeGui = pd->generateSymbol("#active_gui")->s_thing;
        auto* hammarGui = pd->generateSymbol("#hammergui")->s_thing;
        if (activeGui || hammarGui) {
            // canvas.active listener
            char buf[MAXPDSTRING];
            snprintf(buf, MAXPDSTRING - 1, ".x%lx.c", reinterpret_cast<unsigned long>(patchPtr));
            pd->lockAudioThread();
            pd->sendTypedMessage(activeGui, "_focus", { pd->generateSymbol(buf), hasFocus });
            pd->sendTypedMessage(hammarGui, "_focus", { pd->generateSymbol(buf), hasFocus });
            pd->unlockAudioThread();
        }
    });
}

void Canvas::focusLost(FocusChangeType cause)
{
    pd->enqueueFunctionAsync([pd = this->pd, patchPtr = patch.getUncheckedPointer(), hasFocus = static_cast<float>(hasKeyboardFocus(true))] {
        auto* activeGui = pd->generateSymbol("#active_gui")->s_thing;
        auto* hammarGui = pd->generateSymbol("#hammergui")->s_thing;
        if (activeGui || hammarGui) {
            // canvas.active listener
            char buf[MAXPDSTRING];
            snprintf(buf, MAXPDSTRING - 1, ".x%lx.c", reinterpret_cast<unsigned long>(patchPtr));
            pd->lockAudioThread();
            pd->sendTypedMessage(activeGui, "_focus", { pd->generateSymbol(buf), hasFocus });
            pd->sendTypedMessage(hammarGui, "_focus", { pd->generateSymbol(buf), hasFocus });
            pd->unlockAudioThread();
        }
    });
}

void Canvas::dragAndDropPaste(String const& patchString, Point<int> const mousePos, int const patchWidth, int const patchHeight, String const& name)
{
    locked = false;
    presentationMode = false;

    // force the valueChanged to run, and wait for them to return
    locked.getValueSource().sendChangeMessage(true);
    presentationMode.getValueSource().sendChangeMessage(true);

    MessageManager::callAsync([_this = SafePointer(this)] {
        if (_this)
            _this->grabKeyboardFocus();
    });

    auto undoText = String("Add object");
    if (name.isNotEmpty())
        undoText = String("Add " + name.toLowerCase());

    patch.startUndoSequence(undoText);

    auto const patchSize = Point<int>(patchWidth, patchHeight);
    String const translatedObjects = pd::Patch::translatePatchAsString(patchString, mousePos - patchSize / 2.0f);

    if (auto patchPtr = patch.getPointer()) {
        pd::Interface::paste(patchPtr.get(), translatedObjects.toRawUTF8());
    }

    deselectAll();

    // Load state from pd
    performSynchronise();

    patch.setCurrent();

    SmallArray<t_gobj*> pastedObjects;

    if (auto patchPtr = patch.getPointer()) {
        for (auto* object : objects) {
            auto* objectPtr = object->getPointer();
            if (objectPtr && glist_isselected(patchPtr.get(), objectPtr)) {
                setSelected(object, true);
                pastedObjects.emplace_back(objectPtr);
            }
        }
    }

    patch.deselectAll();
    pastedObjects.clear();
    patch.endUndoSequence(undoText);

    updateSidebarSelection();
}

void Canvas::pasteSelection()
{
    // PRD Phase 2: an image on the system clipboard becomes a canvas reference
    // image; otherwise fall through to the classic PD object paste.
    if (pasteImageFromClipboard()) return;

    patch.startUndoSequence("Paste object/s");

    // Paste at mousePos, adds padding if pasted the same place
    auto const mousePosition = getMouseXYRelative() - canvasOrigin;
    if (mousePosition == pastedPosition) {
        pastedPadding.addXY(10, 10);
    } else {
        pastedPadding.setXY(-10, -10);
    }
    pastedPosition = mousePosition;

    // Tell pd to paste with offset applied to the clipboard string
    patch.paste(Point<int>(pastedPosition.x + pastedPadding.x, pastedPosition.y + pastedPadding.y));

    deselectAll();

    // Load state from pd
    performSynchronise();

    patch.setCurrent();

    SmallArray<t_gobj*> pastedObjects;

    if (auto patchPtr = patch.getPointer()) {
        for (auto* object : objects) {
            auto* objectPtr = object->getPointer();
            if (objectPtr && glist_isselected(patchPtr.get(), objectPtr)) {
                setSelected(object, true);
                pastedObjects.emplace_back(objectPtr);
            }
        }
    }

    patch.deselectAll();
    pastedObjects.clear();
    patch.endUndoSequence("Paste object/s");

    updateSidebarSelection();
}

void Canvas::duplicateSelection()
{
    auto selection = getSelectionOfType<Object>();

    patch.startUndoSequence("Duplicate object/s");

    SmallArray<t_gobj*> objectsToDuplicate;
    for (auto const* object : selection) {
        if (auto* ptr = object->getPointer()) {
            objectsToDuplicate.add(ptr);
        }
    }

    // If absolute grid is enabled, snap duplication to grid
    if (dragState.duplicateOffset.isOrigin() && SettingsFile::getInstance()->getProperty<bool>("grid_enabled") && SettingsFile::getInstance()->getProperty<int>("grid_type") & 1) {
        dragState.duplicateOffset = { objectGrid.gridSize - 10, objectGrid.gridSize - 10 };
    }

    // If we previously duplicated and dragged before, and then drag again, the new offset should be relative
    // to the offset we already applied with the previous drag
    if (dragState.lastDuplicateOffset != dragState.duplicateOffset) {
        dragState.duplicateOffset += dragState.lastDuplicateOffset;
    }

    dragState.lastDuplicateOffset = dragState.duplicateOffset;

    t_outconnect* connection = nullptr;
    auto selectedConnections = getSelectionOfType<Connection>();
    SafePointer<Connection> connectionSelectedOriginally = nullptr;
    if (selectedConnections.size() == 1) {
        connectionSelectedOriginally = selectedConnections[0];
        connection = selectedConnections[0]->getPointer();
    }

    // Tell pd to duplicate
    patch.duplicate(objectsToDuplicate, connection);

    deselectAll();

    // Load state from pd immediately
    performSynchronise();

    auto* patchPtr = patch.getRawPointer();
    if (!patchPtr)
        return;

    // Store the duplicated objects for later selection
    SmallArray<Object*> duplicated;
    for (auto* object : objects) {
        auto* objectPtr = object->getPointer();
        if (objectPtr && glist_isselected(patchPtr, objectPtr)) {
            duplicated.add(object);
        }
    }

    // Move duplicated objects if they overlap exisisting objects
    SmallArray<t_gobj*> moveObjects;
    for (auto const* dup : duplicated) {
        moveObjects.add(dup->getPointer());
    }

    patch.moveObjects(moveObjects, dragState.duplicateOffset.x, dragState.duplicateOffset.y);

    for (auto* object : objects) {
        object->updateBounds();
    }

    // Select the newly duplicated objects, and calculate new viewport position
    Rectangle<int> selectionBounds;
    for (auto* obj : duplicated) {
        setSelected(obj, true);
        selectionBounds = selectionBounds.getUnion(obj->getBounds());
    }

    selectionBounds = selectionBounds.transformedBy(getTransform());

    // Adjust the viewport position to ensure the duplicated objects are visible
    auto const viewportPos = viewport->getViewPosition();
    auto const viewWidth = viewport->getWidth();
    auto const viewHeight = viewport->getHeight();
    if (!selectionBounds.isEmpty()) {
        int deltaX = 0, deltaY = 0;

        if (selectionBounds.getRight() > viewportPos.getX() + viewWidth) {
            deltaX = selectionBounds.getRight() - (viewportPos.getX() + viewWidth);
        } else if (selectionBounds.getX() < viewportPos.getX()) {
            deltaX = selectionBounds.getX() - viewportPos.getX();
        }

        if (selectionBounds.getBottom() > viewportPos.getY() + viewHeight) {
            deltaY = selectionBounds.getBottom() - (viewportPos.getY() + viewHeight);
        } else if (selectionBounds.getY() < viewportPos.getY()) {
            deltaY = selectionBounds.getY() - viewportPos.getY();
        }

        // Set the new viewport position
        viewport->setViewPosition(viewportPos + Point<int>(deltaX, deltaY));
    }

    dragState.wasDuplicated = true;

    patch.endUndoSequence("Duplicate object/s");
    patch.deselectAll();

    if (connectionSelectedOriginally) {
        setSelected(connectionSelectedOriginally.getComponent(), true);
    }
}

void Canvas::removeSelection()
{
    patch.startUndoSequence("Remove object/s");
    // Make sure object isn't selected and stop updating gui
    editor->sidebar->hideParameters();

    // Find selected objects and make them selected in pd
    SmallArray<t_gobj*> objects;
    for (auto const* object : getSelectionOfType<Object>()) {
        if (auto* ptr = object->getPointer()) {
            objects.add(ptr);
        }
    }

    auto wasDeleted = [&objects](t_gobj* ptr) {
        return objects.contains(ptr);
    };

    // remove selection
    patch.removeObjects(objects);

    // Remove connection afterwards and make sure they aren't already deleted
    for (auto const* con : connections) {
        if (con->isSelected()) {
            auto* outPtr = con->outobj->getPointer();
            auto* inPtr = con->inobj->getPointer();
            auto* checkedOutPtr = pd::Interface::checkObject(outPtr);
            auto* checkedInPtr = pd::Interface::checkObject(inPtr);
            if (checkedOutPtr && checkedInPtr && !(wasDeleted(outPtr) || wasDeleted(inPtr))) {
                patch.removeConnection(checkedOutPtr, con->outIdx, checkedInPtr, con->inIdx, con->getPathState());
            }
        }
    }

    patch.finishRemove(); // Makes sure that the extra removed connections will be grouped in the same undo action

    deselectAll();

    // Load state from pd
    synchronise();
    handleUpdateNowIfNeeded();

    patch.endUndoSequence("Remove object/s");

    patch.deselectAll();

    synchroniseSplitCanvas();
}

void Canvas::removeSelectedConnections()
{
    patch.startUndoSequence("Remove connection/s");

    for (auto const* con : connections) {
        if (con->isSelected()) {
            auto* checkedOutPtr = pd::Interface::checkObject(con->outobj->getPointer());
            auto* checkedInPtr = pd::Interface::checkObject(con->inobj->getPointer());
            if (!checkedInPtr || !checkedOutPtr)
                continue;

            patch.removeConnection(checkedOutPtr, con->outIdx, checkedInPtr, con->inIdx, con->getPathState());
        }
    }

    patch.endUndoSequence("Remove connection/s");

    // Load state from pd
    synchronise();
    handleUpdateNowIfNeeded();

    synchroniseSplitCanvas();
}

void Canvas::cycleSelection()
{
    if (connectionsBeingCreated.size() == 1) {
        connectionsBeingCreated[0]->toNextIolet();
        return;
    }
    // Get the selected objects
    if (auto selectedObjects = getSelectionOfType<Object>(); selectedObjects.size() == 1) {
        // Find the index of the currently selected object
        auto const currentIdx = objects.index_of(selectedObjects[0]);
        setSelected(selectedObjects[0], false);

        // Calculate the next index (wrap around if at the end)
        auto const nextIdx = (currentIdx + 1) % objects.size();
        setSelected(objects[nextIdx], true);

        return;
    }

    // Get the selected connections if no objects are selected
    if (auto selectedConnections = getSelectionOfType<Connection>(); selectedConnections.size() == 1) {
        // Find the index of the currently selected connection
        auto const currentIdx = connections.index_of(selectedConnections[0]);
        setSelected(selectedConnections[0], false);

        // Calculate the next index (wrap around if at the end)
        auto const nextIdx = (currentIdx + 1) % connections.size();
        setSelected(connections[nextIdx], true);
    }
}

void Canvas::tidySelection()
{
    SmallArray<t_gobj*> selectedObjects;
    for (auto const* object : getSelectionOfType<Object>()) {
        if (auto* ptr = object->getPointer()) {
            selectedObjects.add(ptr);
        }
    }

    if (auto patchPtr = patch.getPointer()) {
        pd::Interface::tidy(patchPtr.get(), selectedObjects);
    }

    synchronise();
}

void Canvas::triggerizeSelection()
{
    SmallArray<t_gobj*> selectedObjects;
    for (auto const* object : getSelectionOfType<Object>()) {
        if (auto* ptr = object->getPointer()) {
            selectedObjects.add(ptr);
        }
    }

    t_outconnect const* connection = nullptr;
    auto selectedConnections = getSelectionOfType<Connection>();
    if (selectedConnections.size() == 1) {
        connection = selectedConnections[0]->getPointer();
    }

    t_gobj const* triggerizedObject = nullptr;
    if (auto patchPtr = patch.getPointer()) {
        triggerizedObject = pd::Interface::triggerize(patchPtr.get(), selectedObjects, connection);
    }

    performSynchronise();

    if (triggerizedObject) {
        for (auto* object : objects) {
            if (object->getPointer() == triggerizedObject) {
                setSelected(object, true);
                object->showEditor();
                hideSuggestions();
            }
        }
    }
}

void Canvas::encapsulateSelection(String const& subpatchName)
{
    encapsulateSelection(subpatchName, getSelectionOfType<Object>());
}

void Canvas::encapsulateSelection(String const& subpatchName, SmallArray<Object*> const& objectsToEncapsulate)
{
    auto selectedObjects = objectsToEncapsulate;

    // Sort by index in pd patch
    selectedObjects.sort([this](auto const* a, auto const* b) -> bool {
        return objects.index_of(a) < objects.index_of(b);
    });

    // If two connections have the same target inlet/outlet, we only need 1 [inlet/outlet] object
    auto usedIolets = SmallArray<Iolet*>();
    auto targetIolets = UnorderedMap<Iolet*, SmallArray<Iolet*>>();

    auto newInternalConnections = String();
    auto newExternalConnections = UnorderedMap<int, SmallArray<Iolet*>>();

    // First, find all the incoming and outgoing connections
    for (auto* connection : connections) {
        if (selectedObjects.contains(connection->inobj.get()) && !selectedObjects.contains(connection->outobj.get())) {
            auto* inlet = connection->inlet.get();
            targetIolets[inlet].add(connection->outlet.get());
            usedIolets.add_unique(inlet);
        }
    }
    for (auto* connection : connections) {
        if (selectedObjects.contains(connection->outobj.get()) && !selectedObjects.contains(connection->inobj.get())) {
            auto* outlet = connection->outlet.get();
            targetIolets[outlet].add(connection->inlet.get());
            usedIolets.add_unique(outlet);
        }
    }

    auto newEdgeObjects = String();

    usedIolets.sort([](auto* a, auto* b) -> bool {
        // Inlets before outlets
        if (a->isInlet != b->isInlet)
            return a->isInlet;

        auto apos = a->getCanvasBounds().getPosition();
        auto bpos = b->getCanvasBounds().getPosition();

        if (apos.x == bpos.x) {
            return apos.y < bpos.y;
        }

        return apos.x < bpos.x;
    });

    int i = 0;
    int numIn = 0;
    for (auto* iolet : usedIolets) {
        bool isSignal = iolet->isSignal;
        if (iolet->isInlet) {
            // If all incoming wires from outside are control-rate, create [inlet], not [inlet~]
            bool hasSignalInput = false;
            for (auto* outEdge : targetIolets[iolet]) {
                if (outEdge && outEdge->isSignal) {
                    hasSignalInput = true;
                    break;
                }
            }
            isSignal = hasSignalInput;
        }

        auto type = String(iolet->isInlet ? "inlet" : "outlet") + String(isSignal ? "~" : "");
        int xPos = 50 + (iolet->isInlet ? i : (i - numIn)) * 100;
        int yPos = iolet->isInlet ? 20 : 400;
        newEdgeObjects += "#X obj " + String(xPos) + " " + String(yPos) + " " + type + ";\n";

        int objIdx = selectedObjects.index_of(iolet->object);
        int ioletObjectIdx = selectedObjects.size() + i;
        if (iolet->isInlet) {
            newInternalConnections += "#X connect " + String(ioletObjectIdx) + " 0 " + String(objIdx) + " " + String(iolet->ioletIdx) + ";\n";
            numIn++;
        } else {
            newInternalConnections += "#X connect " + String(objIdx) + " " + String(iolet->ioletIdx) + " " + String(ioletObjectIdx) + " 0;\n";
        }

        for (auto* target : targetIolets[iolet]) {
            newExternalConnections[i].add(target);
        }

        i++;
    }

    patch.deselectAll();

    auto bounds = Rectangle<int>();
    SmallArray<t_gobj*> objects;
    for (auto* object : selectedObjects) {
        if (auto* ptr = object->getPointer()) {
            bounds = bounds.getUnion(object->getBounds());
            objects.add(ptr);
        }
    }
    auto centre = bounds.getCentre() - canvasOrigin;

    auto nameToken = (subpatchName.isEmpty() || subpatchName == "pd") ? "pd" : ("pd " + subpatchName);
    auto copypasta = String("#N canvas 733 172 450 300 0 1;\n") + "$$_COPY_HERE_$$" + newEdgeObjects + newInternalConnections + "#X restore " + String(centre.x) + " " + String(centre.y) + " " + nameToken + ";\n";

    // Apply the changed on Pd's thread
    if (auto patchPtr = patch.getPointer()) {
        int size;
        char const* text = pd::Interface::copy(patchPtr.get(), &size, objects);
        auto copied = String::fromUTF8(text, size);

        // Wrap it in an undo sequence, to allow undoing everything in 1 step
        patch.startUndoSequence("Encapsulate");

        pd::Interface::removeObjects(patchPtr.get(), objects);

        auto replacement = copypasta.replace("$$_COPY_HERE_$$", copied);

        pd::Interface::paste(patchPtr.get(), replacement.toRawUTF8());
        auto lastObject = patch.getObjects().back();
        if (!lastObject.isValid())
            return;

        auto* newObject = pd::Interface::checkObject(lastObject.getRaw<t_pd>());
        if (!newObject) {
            patch.endUndoSequence("Encapsulate");
            pd->unlockAudioThread();
            return;
        }

        for (auto& [idx, iolets] : newExternalConnections) {
            for (auto* iolet : iolets) {
                if (auto* externalObject = reinterpret_cast<t_object*>(iolet->object->getPointer())) {
                    if (iolet->isInlet) {
                        pd::Interface::createConnection(patchPtr.get(), newObject, idx - numIn, externalObject, iolet->ioletIdx);
                    } else {
                        pd::Interface::createConnection(patchPtr.get(), externalObject, iolet->ioletIdx, newObject, idx);
                    }
                }
            }
        }

        patch.endUndoSequence("Encapsulate");
    }

    synchronise();
    handleUpdateNowIfNeeded();

    patch.deselectAll();
}

void Canvas::connectSelection()
{
    SmallArray<t_gobj*> selectedObjects;
    for (auto const* object : getSelectionOfType<Object>()) {
        if (auto* ptr = object->getPointer()) {
            selectedObjects.add(ptr);
        }
    }

    t_outconnect const* connection = nullptr;
    if (auto selectedConnections = getSelectionOfType<Connection>(); selectedConnections.size() == 1) {
        connection = selectedConnections[0]->getPointer();
    }

    if (auto patchPtr = patch.getPointer()) {
        pd::Interface::connectSelection(patchPtr.get(), selectedObjects, connection);
    }

    synchronise();
}

void Canvas::cancelConnectionCreation()
{
    connectionsBeingCreated.clear();
    if (connectingWithDrag) {
        connectingWithDrag = false;
        connectionCancelled = true;
        if (nearestIolet) {
            nearestIolet->isTargeted = false;
            nearestIolet->repaint();
            nearestIolet = nullptr;
        }
    }
}

void Canvas::alignObjects(Align const alignment)
{
    auto selectedObjects = getSelectionOfType<Object>();

    if (selectedObjects.size() < 2)
        return;

    auto sortByXPos = [](SmallArray<Object*>& objects) {
        objects.sort([](auto const& a, auto const& b) {
            auto aX = a->getBounds().getX();
            auto bX = b->getBounds().getX();
            if (aX == bX) {
                return a->getBounds().getY() < b->getBounds().getY();
            }
            return aX < bX;
        });
    };

    auto sortByYPos = [](SmallArray<Object*>& objects) {
        objects.sort([](auto const& a, auto const& b) {
            auto aY = a->getBounds().getY();
            auto bY = b->getBounds().getY();
            if (aY == bY)
                return a->getBounds().getX() < b->getBounds().getX();

            return aY < bY;
        });
    };

    auto getBoundingBox = [](SmallArray<Object*>& objects) -> Rectangle<int> {
        auto totalBounds = Rectangle<int>();
        for (auto const* object : objects) {
            if (object->getPointer()) {
                totalBounds = totalBounds.getUnion(object->getBounds());
            }
        }
        return totalBounds;
    };

    patch.startUndoSequence("Align objects");

    // mark canvas as dirty, and set undo for all positions
    if (auto patchPtr = patch.getPointer()) {
        canvas_dirty(patchPtr.get(), 1);
        for (auto const object : objects) {
            if (auto* ptr = object->getPointer())
                pd::Interface::undoApply(patchPtr.get(), ptr);
        }
    }

    // get the bounding box of all selected objects
    auto const selectedBounds = getBoundingBox(selectedObjects);

    auto onMove = [this, selectedObjects](Point<int> const position) {
        // Calculate the bounding box of all selected objects
        Rectangle<int> totalSize;

        for (auto const obj : selectedObjects) {
            totalSize = totalSize.getUnion(obj->getBounds());
        }

        // Determine the offset for each object from the top-left of the total bounding box
        Array<Point<int>> offsets;
        for (auto const obj : selectedObjects) {
            offsets.add(obj->getPosition() - totalSize.getPosition());
        }

        // Move each object to the new position, maintaining its relative offset
        for (int i = 0; i < selectedObjects.size(); i++) {
            auto const* obj = selectedObjects[i];
            patch.moveObjectTo(obj->getPointer(), position.x + offsets[i].x, position.y + offsets[i].y);
        }

        synchronise();
    };

    switch (alignment) {
    case Align::Left: {
        auto const leftPos = selectedBounds.getTopLeft().getX();
        for (auto const* object : selectedObjects) {
            patch.moveObjectTo(object->getPointer(), leftPos, object->getBounds().getY());
        }
        break;
    }
    case Align::Right: {
        auto const rightPos = selectedBounds.getRight();
        for (auto const* object : selectedObjects) {
            auto objectBounds = object->getBounds();
            patch.moveObjectTo(object->getPointer(), rightPos - objectBounds.getWidth(), objectBounds.getY());
        }
        break;
    }
    case Align::VCentre: {
        auto const centrePos = selectedBounds.getCentreX();
        for (auto const* object : selectedObjects) {
            auto objectBounds = object->getBounds();
            patch.moveObjectTo(object->getPointer(), centrePos - objectBounds.withZeroOrigin().getCentreX(), objectBounds.getY());
        }
        break;
    }
    case Align::Top: {
        auto const topPos = selectedBounds.getTopLeft().y;
        for (auto const* object : selectedObjects) {
            patch.moveObjectTo(object->getPointer(), object->getX(), topPos);
        }
        break;
    }
    case Align::Bottom: {
        auto const bottomPos = selectedBounds.getBottom();
        for (auto const* object : selectedObjects) {
            auto objectBounds = object->getBounds();
            patch.moveObjectTo(object->getPointer(), objectBounds.getX(), bottomPos - objectBounds.getHeight());
        }
        break;
    }
    case Align::HCentre: {
        auto const centerPos = selectedBounds.getCentreY();
        for (auto const* object : selectedObjects) {
            auto objectBounds = object->getBounds();
            patch.moveObjectTo(object->getPointer(), objectBounds.getX(), centerPos - objectBounds.withZeroOrigin().getCentreY());
        }
        break;
    }
    case Align::HDistribute: {
        sortByXPos(selectedObjects);

        auto onResize = [this, selectedObjects](Rectangle<int> const newBounds) {
            int totalObjectsWidth = 0;
            for (auto const* obj : selectedObjects) {
                totalObjectsWidth += obj->getBounds().getWidth() - Object::doubleMargin;
            }

            int const totalSpacing = newBounds.getWidth() - totalObjectsWidth;
            float const spacer = selectedObjects.size() > 1 ? static_cast<float>(totalSpacing) / (selectedObjects.size() - 1) : 0;

            float offsetX = newBounds.getX() - Object::margin;
            for (int i = 0; i < selectedObjects.size(); i++) {
                auto const* obj = selectedObjects[i];

                // Set the object position
                if (i == selectedObjects.size() - 1) {
                    patch.moveObjectTo(obj->getPointer(), newBounds.getRight() - obj->getWidth() + Object::margin, obj->getBounds().getY());
                } else {
                    patch.moveObjectTo(obj->getPointer(), std::max(newBounds.toFloat().getX() - Object::margin, offsetX), obj->getBounds().getY());
                    offsetX += obj->getBounds().getWidth() + spacer - Object::doubleMargin;
                }
            }
            synchronise();
            return spacer;
        };
        objectsDistributeResizer.reset(std::make_unique<ObjectsResizer>(this, onResize, onMove).release());
        break;
    }
    case Align::VDistribute: {
        sortByYPos(selectedObjects);

        auto onResize = [this, selectedObjects](Rectangle<int> const newBounds) {
            int totalObjectsHeight = 0;
            for (auto const* obj : selectedObjects) {
                totalObjectsHeight += obj->getBounds().getHeight() - Object::doubleMargin;
            }

            int const totalSpacing = newBounds.getHeight() - totalObjectsHeight;
            float const spacer = selectedObjects.size() > 1 ? static_cast<float>(totalSpacing) / (selectedObjects.size() - 1) : 0;

            float offsetY = newBounds.getY() - Object::margin;
            for (int i = 0; i < selectedObjects.size(); i++) {
                auto const* obj = selectedObjects[i];

                // Set the object position
                if (i == selectedObjects.size() - 1) {
                    patch.moveObjectTo(obj->getPointer(), obj->getBounds().getX(), newBounds.getBottom() - obj->getHeight() + Object::margin);
                } else {
                    patch.moveObjectTo(obj->getPointer(), obj->getBounds().getX(), std::max(newBounds.toFloat().getY() - Object::margin, offsetY));
                    offsetY += obj->getBounds().getHeight() + spacer - Object::doubleMargin;
                }
            }
            synchronise();
            return spacer;
        };
        objectsDistributeResizer.reset(std::make_unique<ObjectsResizer>(this, onResize, onMove, ObjectsResizer::ResizerMode::Vertical).release());
        break;
    }
    default:
        break;
    }

    performSynchronise();

    for (auto* connection : connections) {
        connection->forceUpdate();
    }

    patch.endUndoSequence("Align objects");
}

void Canvas::undo()
{

    // If there is an object with an active editor, we interpret undo as wanting to undo the creation of that object editor
    // This is because the initial object editor is not communicated with Pd, so we can't rely on patch undo to do that
    // If we don't do this, it will undo the old last action before creating this editor, which would be confusing
    for (auto const object : objects) {
        if (object->isInitialEditorShown()) {
            object->hideEditor();
        }
    }

    // 1. If an AI transaction exists, undo that first (safe transactional rollback with semantic IDs)
    if (pd && pd->hasMcpTransaction(patch.getUncheckedPointer())) {
        pd->undoMcpTransaction(patch.getUncheckedPointer());
        synchronise();
        handleUpdateNowIfNeeded();
        patch.deselectAll();
        synchroniseSplitCanvas();
        updateSidebarSelection();
        return;
    }

    // 2. Otherwise, if user has manual GUI edits on Pure Data's native stack, undo that
    if (patch.canUndo()) {
        patch.undo();
        synchronise();
        handleUpdateNowIfNeeded();
        patch.deselectAll();
        synchroniseSplitCanvas();
        updateSidebarSelection();
        return;
    }

    // Nothing to undo
}

bool Canvas::canUndo()
{
    if (pd && pd->hasMcpTransaction(patch.getUncheckedPointer())) return true;
    return patch.canUndo();
}

void Canvas::redo()
{
    // 1. If an AI transaction was undone, redo that first to recreate objects before redoing manual edits on them
    if (pd && pd->hasMcpRedoTransaction(patch.getUncheckedPointer())) {
        pd->redoMcpTransaction(patch.getUncheckedPointer());
        synchronise();
        handleUpdateNowIfNeeded();
        patch.deselectAll();
        synchroniseSplitCanvas();
        updateSidebarSelection();
        return;
    }

    // 2. Otherwise, if user has manual GUI edits to redo, tell pd to redo
    if (patch.canRedo()) {
        patch.redo();
        synchronise();
        handleUpdateNowIfNeeded();
        patch.deselectAll();
        synchroniseSplitCanvas();
        updateSidebarSelection();
        return;
    }
}

void Canvas::valueChanged(Value& v)
{
    // Update zoom
    if (v.refersToSameSourceAs(zoomScale)) {
        editor->statusbar->updateZoomLevel();
        patch.lastViewportScale = getValue<float>(zoomScale);
        hideSuggestions();
    } else if (v.refersToSameSourceAs(patchWidth)) {
        // limit canvas width to smallest object (11px)
        patchWidth = jmax(11, getValue<int>(patchWidth));
        if (auto cnv = patch.getPointer()) {
            auto x1 = static_cast<float>(cnv->gl_screenx1);
            auto y1 = static_cast<float>(cnv->gl_screeny1);
            auto x2 = static_cast<float>(getValue<int>(patchWidth) + x1);
            auto y2 = static_cast<float>(cnv->gl_screeny2);

            char buf[MAXPDSTRING];
            snprintf(buf, MAXPDSTRING - 1, ".x%lx", reinterpret_cast<unsigned long>(cnv.get()));
            pd->sendMessage(buf, "setbounds", { x1, y1, x2, y2 });
        }
        repaint();
    } else if (v.refersToSameSourceAs(patchHeight)) {
        patchHeight = jmax(11, getValue<int>(patchHeight));
        if (auto cnv = patch.getPointer()) {
            auto x1 = static_cast<float>(cnv->gl_screenx1);
            auto y1 = static_cast<float>(cnv->gl_screeny1);
            auto x2 = static_cast<float>(cnv->gl_screenx2);
            auto y2 = static_cast<float>(getValue<int>(patchHeight) + y1);

            char buf[MAXPDSTRING];
            snprintf(buf, MAXPDSTRING - 1, ".x%lx", reinterpret_cast<unsigned long>(cnv.get()));
            pd->sendMessage(buf, "setbounds", { x1, y1, x2, y2 });
        }
        repaint();
    }
    // When lock changes
    else if (v.refersToSameSourceAs(locked)) {
        bool const editMode = !getValue<bool>(v);

        if (auto ptr = patch.getPointer()) {
            pd->sendDirectMessage(ptr.get(), "editmode", { static_cast<float>(editMode) });
        }

        cancelConnectionCreation();
        deselectAll();

        // Makes sure no objects keep keyboard focus after locking/unlocking
        if (isShowing() && isVisible())
            grabKeyboardFocus();

        editor->updateCommandStatus();
        updateOverlays();
        orderConnections();
    } else if (v.refersToSameSourceAs(commandLocked)) {
        updateOverlays();
        repaint();
    }
    // Should only get called when the canvas isn't a real graph
    else if (v.refersToSameSourceAs(presentationMode)) {
        connectionLayer.setVisible(!getValue<bool>(presentationMode));
        deselectAll();
    } else if (v.refersToSameSourceAs(hideNameAndArgs)) {
        if (!patch.getPointer())
            return;

        int hideText = getValue<bool>(hideNameAndArgs);
        if (auto glist = patch.getPointer()) {
            hideText = glist->gl_isgraph && hideText;
            canvas_setgraph(glist.get(), glist->gl_isgraph + 2 * hideText, 0);
        }

        hideNameAndArgs = hideText;
    } else if (v.refersToSameSourceAs(isGraphChild)) {
        if (!patch.getPointer())
            return;

        int const graphChild = getValue<bool>(isGraphChild);

        if (auto glist = patch.getPointer()) {
            canvas_setgraph(glist.get(), graphChild + 2 * (graphChild && glist->gl_hidetext), 0);
        }

        if (!graphChild) {
            hideNameAndArgs = false;
        }

        if (graphChild && !isGraph) {
            graphArea = std::make_unique<GraphArea>(this);
            addAndMakeVisible(*graphArea);
            graphArea->setAlwaysOnTop(true);
            graphArea->updateBounds();
        } else {
            graphArea.reset(nullptr);
        }

        updateOverlays();
        repaint();
    } else if (v.refersToSameSourceAs(xRange)) {
        if (auto glist = patch.getPointer()) {
            glist->gl_x1 = static_cast<float>(xRange.getValue().getArray()->getReference(0));
            glist->gl_x2 = static_cast<float>(xRange.getValue().getArray()->getReference(1));
        }
        updateDrawables();
    } else if (v.refersToSameSourceAs(yRange)) {
        if (auto glist = patch.getPointer()) {
            glist->gl_y2 = static_cast<float>(yRange.getValue().getArray()->getReference(0));
            glist->gl_y1 = static_cast<float>(yRange.getValue().getArray()->getReference(1));
        }
        updateDrawables();
    }
}

void Canvas::orderConnections()
{
    // move connection layer to back when canvas is locked & connections behind is active
    if (connectionsBehind) {
        connectionLayer.toBack();
    } else
        objectLayer.toBack();

    repaint();
}

void Canvas::showSuggestions(Object* object, TextEditor* textEditor)
{
    suggestor->createCalloutBox(object, textEditor);
}
void Canvas::hideSuggestions()
{
    suggestor->removeCalloutBox();
}

// Makes component selected
void Canvas::setSelected(Component* component, bool const shouldNowBeSelected, bool const updateCommandStatus, bool const broadcastChange)
{
    if (!broadcastChange) {
        selectedComponents.removeChangeListener(this);
    }

    if (!shouldNowBeSelected) {
        selectedComponents.deselect(component);
    } else {
        selectedComponents.addToSelection(component);
    }

    if (updateCommandStatus) {
        editor->updateCommandStatus();
    }

    if (!broadcastChange) {
        // Add back the listener, but make sure it's added back 'after' the last event on the message queue
        MessageManager::callAsync([this] { selectedComponents.addChangeListener(this); });
    }
}

SelectedItemSet<WeakReference<Component>>& Canvas::getLassoSelection()
{
    return selectedComponents;
}

bool Canvas::checkPanDragMode()
{
    auto const panDragEnabled = panningModifierDown();
    setPanDragMode(panDragEnabled);

    return panDragEnabled;
}

bool Canvas::setPanDragMode(bool const shouldPan)
{
    if (auto* v = dynamic_cast<CanvasViewport*>(viewport.get())) {
        v->enableMousePanning(shouldPan);
        return true;
    }
    return false;
}

bool Canvas::isPointOutsidePluginArea(Point<int> const point) const
{
    auto const borderWidth = getValue<float>(patchWidth);
    auto const borderHeight = getValue<float>(patchHeight);
    constexpr auto halfSize = infiniteCanvasSize / 2;
    constexpr auto pos = Point<int>(halfSize, halfSize);

    auto const pluginBounds = Rectangle<int>(pos.x, pos.y, borderWidth, borderHeight);

    return !pluginBounds.contains(point);
}

void Canvas::findLassoItemsInArea(Array<WeakReference<Component>>& itemsFound, Rectangle<int> const& area)
{
    auto const lassoBounds = area.withWidth(jmax(2, area.getWidth())).withHeight(jmax(2, area.getHeight()));

    if (!altDown) { // Alt enable connection only mode
        for (auto* object : objects) {
            if (lassoBounds.intersects(object->getSelectableBounds())) {
                itemsFound.add(object);
            } else if (!ModifierKeys::getCurrentModifiers().isAnyModifierKeyDown()) {
                setSelected(object, false, false);
            }
        }
    }

    auto const anyModifiersDown = ModifierKeys::getCurrentModifiers().isAnyModifierKeyDown();
    auto const canSelectConnections = itemsFound.isEmpty() || anyModifiersDown;

    for (auto const& connection : connections) {
        // If total bounds don't intersect, there can't be an intersection with the line
        // This is cheaper than checking the path intersection, so do this first
        if (!connection->getBounds().intersects(lassoBounds)) {
            setSelected(connection, false, false);
            continue;
        }

        // Check if path intersects with lasso
        if (canSelectConnections && connection->intersects(lassoBounds.toFloat())) {
            itemsFound.add(connection);
        } else if (!anyModifiersDown) {
            setSelected(connection, false, false);
        }
    }
}

ObjectParameters& Canvas::getInspectorParameters()
{
    return parameters;
}

bool Canvas::panningModifierDown() const
{
#if JUCE_IOS
    return OSUtils::ScrollTracker::isScrolling();
#endif
    auto const& commandManager = editor->commandManager;
    // check the command manager for the keycode that is assigned to pan drag key
    auto const panDragKeycode = commandManager.getKeyMappings()->getKeyPressesAssignedToCommand(CommandIDs::PanDragKey).getFirst().getKeyCode();

    // get the current modifier keys, removing the left mouse button modifier (as that is what is needed to activate a pan drag with key down)
    auto const currentMods = ModifierKeys(ModifierKeys::getCurrentModifiers().getRawFlags() & ~ModifierKeys::leftButtonModifier);

    bool isPanDragKeysActive = false;

    if (KeyPress::isKeyCurrentlyDown(panDragKeycode)) {
        // construct a fake keypress with the current pan drag keycode key, with current modifiers, to test if it matches the command id's code & mods
        auto const keyWithMod = KeyPress(panDragKeycode, currentMods, 0);
        isPanDragKeysActive = commandManager.getKeyMappings()->containsMapping(CommandIDs::PanDragKey, keyWithMod);
    }

    return isPanDragKeysActive || ModifierKeys::getCurrentModifiers().isMiddleButtonDown();
}

void Canvas::receiveMessage(t_symbol* symbol, SmallArray<pd::Atom> const& atoms)
{
    switch (hash(symbol->s_name)) {
    case hash("sync"):
    case hash("obj"):
    case hash("msg"):
    case hash("floatatom"):
    case hash("listbox"):
    case hash("symbolatom"):
    case hash("text"):
    case hash("graph"):
    case hash("scalar"):
    case hash("bng"):
    case hash("toggle"):
    case hash("vslider"):
    case hash("hslider"):
    case hash("hdial"):
    case hash("vdial"):
    case hash("hradio"):
    case hash("vradio"):
    case hash("vumeter"):
    case hash("mycnv"):
    case hash("numbox"):
    case hash("connect"):
    case hash("clear"):
    case hash("cut"):
    case hash("disconnect"): {
        // This will trigger an asyncupdater, so it's thread-safe to do this here
        synchronise();
        break;
    }
    case hash("editmode"): {
        if (::getValue<bool>(commandLocked))
            return;

        if (atoms.size() >= 1) {
            if (int const flag = atoms[0].getFloat(); flag % 2 == 0) {
                locked = true;
            } else {
                locked = false;
                presentationMode = false;
            }
        }
        break;
    }
    case hash("setbounds"): {
        if (atoms.size() >= 4) {
            auto const width = atoms[2].getFloat() - atoms[0].getFloat();
            auto const height = atoms[3].getFloat() - atoms[1].getFloat();
            setValueExcludingListener(patchWidth, static_cast<int>(width), this);
            setValueExcludingListener(patchHeight, static_cast<int>(height), this);
            repaint();
        }

        break;
    }
    case hash("coords"):
    case hash("donecanvasdialog"): {
        synchroniseSplitCanvas();
        break;
    }
    default:
        break;
    }
}

void Canvas::resized()
{
    connectionLayer.setBounds(getLocalBounds());
    objectLayer.setBounds(getLocalBounds());
}

void Canvas::activateCanvasSearchHighlight(Object* obj)
{
    canvasSearchHighlight.reset(std::make_unique<CanvasSearchHighlight>(this, obj).release());
}

void Canvas::removeCanvasSearchHighlight()
{
    if (canvasSearchHighlight)
        canvasSearchHighlight.reset(nullptr);
}
