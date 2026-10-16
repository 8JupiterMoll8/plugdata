/*
 // Copyright (c) 2021-2025 Timothy Schoen
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
 */
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_cryptography/juce_cryptography.h>
#include "Utility/Config.h"
#include "Utility/Fonts.h"
#include "Utility/NVGTextShim.h"

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
            border.setBorderThickness(BorderSize<int>(0, borderSize, 0, borderSize));
        } else {
            border.setBorderThickness(BorderSize<int>(borderSize, 0, borderSize, 0));
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
        if (cnv->panningModifierDown() || (cnv->viewport && cnv->viewport->isPerformingGesture()) || ModifierKeys::getCurrentModifiers().isAnyModifierKeyDown())
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
        auto const& colours = getThemeColours(*this);

        // draw background and outline
        auto const b = getBounds().reduced(tabMargin);
        auto iCol = nvgColour(colours.objectSelectedOutlineColour);

        iCol.a = 5; // Make the inner colour semi-transparent
        nanovg::nvgDrawRoundedRect(nvg, b.getX(), b.getY(), b.getWidth(), b.getHeight(), iCol, nvgColour(colours.objectSelectedOutlineColour), getPlugDataLook(*this).getObjectCornerRadius());

        // Draw handles at edge
        cnv->renderResizeHandles(nvg, nvgColour(colours.objectSelectedOutlineColour), b.toFloat().expanded(2.5f));
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

    // initialize to default zoom
    auto const defaultZoom = SettingsFile::getInstance()->getPropertyAsValue("default_zoom");
    zoomScale.setValue(getValue<float>(defaultZoom) / 100.0f);
    zoomScale.addListener(this);

    setSize(infiniteCanvasSize, infiniteCanvasSize);

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
        canvasViewport->onScroll = [this] {
            if (suggestor) {
                suggestor->updateBounds();
            }
            if (graphArea) {
                graphArea->updateBounds();
            }
        };
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

    // Add draggable border for setting graph position
    if (getValue<bool>(isGraphChild) && !isGraph) {
        graphArea = std::make_unique<GraphArea>(this);
        addAndMakeVisible(*graphArea);
        graphArea->setAlwaysOnTop(true);
    }

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

    lookAndFeelChanged();
}

Canvas::~Canvas()
{
    for (auto* object : objects) {
        object->hideEditor();
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

    if (getValue<bool>(isGraphChild) && isGraph)
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
    dotsLargeImage.setDirty();
    resizeHandleImage.setDirty();
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

void Canvas::updateCanvasDots(NVGcontext* nvg)
{
    auto const pixelScale = editor->getRenderScale();
    auto zoom = getValue<float>(zoomScale);

    auto gridLogicalSize = objectGrid.gridSize ? objectGrid.gridSize : 25;
    auto constexpr gridSizeCommon = 300;
    auto const gridBufferSize = gridSizeCommon * pixelScale * zoom;

    if (dotsLargeImage.needsUpdate(gridBufferSize, gridBufferSize) || lastObjectGridSize != gridLogicalSize) {
        lastObjectGridSize = gridLogicalSize;

        dotsLargeImage.renderToFramebuffer(nvg, gridBufferSize, gridBufferSize, [this, gridBufferSize, gridLogicalSize, zoom, pixelScale](NVGcontext* nvg){
            auto const& colours = getThemeColours(*this);


            nanovg::viewport(nvg, 0, 0, gridBufferSize, gridBufferSize);
            nanovg::clear(nvg);
            nanovg::nvgBeginFrame(nvg, gridSizeCommon * zoom, gridSizeCommon * zoom, pixelScale);
            nanovg::nvgScale(nvg, zoom, zoom);

            nanovg::nvgFillColor(nvg, nvgColour(colours.canvasBackgroundColour));
            nanovg::nvgFillRect(nvg, 0, 0, gridSizeCommon, gridSizeCommon);

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

            auto markingColour = colours.canvasDotsColour.interpolatedWith(colours.canvasBackgroundColour, 0.2f);
            auto const majorDotColour = markingColour.withAlpha(std::min(zoom * 0.8f, 1.0f));

            nanovg::nvgFillColor(nvg, nvgColour(majorDotColour));

            // Draw ellipses on the grid
            for (int x = 0; x <= gridSizeCommon; x += gridLogicalSize)
            {
                for (int y = 0; y <= gridSizeCommon; y += gridLogicalSize)
                {
                    NVGcolor c = nvgColour(majorDotColour);
                    if (decim != 0) {
                        if (x % decim && y % decim)
                            continue;
                        if (x % decim == 0 && y % decim == 0)
                            c = nvgColour(markingColour);
                    }
                    // Add half smallest dot offset so the dot isn't at the edge of the texture
                    // We remove this when we position the texture on the canvas
                    float const centerX = static_cast<float>(x) + 2.5f;
                    float const centerY = static_cast<float>(y) + 2.5f;
                    float const ellipseSize = ellipseRadius * 2.0f;
                    nanovg::nvgDrawRoundedRect(nvg, centerX - ellipseRadius, centerY - ellipseRadius, ellipseSize, ellipseSize, c, c, ellipseRadius);
                }
            }

            nanovg::nvgGlobalScissor(nvg, 0, 0, gridBufferSize, gridBufferSize);
            nanovg::nvgEndFrameWithoutPublishing(nvg);
        }, NVG_IMAGE_REPEATX | NVG_IMAGE_REPEATY);
    }
}

void Canvas::renderResizeHandles(NVGcontext* nvg, NVGcolor const colour, Rectangle<float> const bounds)
{
    auto constexpr resizeHandleSize = 9.0f;
    auto constexpr resizeHandleMargin = 1.0f;
    auto constexpr resizeHandleImageSize = resizeHandleSize + resizeHandleMargin * 2.0f;

    auto const scale = getValue<float>(zoomScale) * editor->getRenderScale();
    auto const pixelSize = jlimit(8, 256, nextPowerOfTwo(static_cast<int>(std::ceil(resizeHandleImageSize * scale))));

    if (resizeHandleImage.needsUpdate(pixelSize, pixelSize)) {
        constexpr float kappa = 0.5522847498307936f;
        auto const innerOffset = static_cast<float>(Object::margin) / 2.0f;
        auto const outerRadius = jlimit(0.0f, resizeHandleSize * 0.5f, Corners::resizeHanleCornerRadius);
        auto const innerRadius = jlimit(0.0f, resizeHandleSize - innerOffset, getPlugDataLook(*this).getObjectCornerRadius());
        auto const outerControl = outerRadius * (1.0f - kappa);
        auto const innerControl = innerRadius * (1.0f - kappa);

        Path handleShape;
        handleShape.startNewSubPath(outerRadius, 0.0f);
        handleShape.lineTo(resizeHandleSize - outerRadius, 0.0f);
        handleShape.cubicTo(resizeHandleSize - outerControl, 0.0f, resizeHandleSize, outerControl, resizeHandleSize, outerRadius);
        handleShape.lineTo(resizeHandleSize, innerOffset);
        handleShape.lineTo(innerOffset + innerRadius, innerOffset);
        if (innerRadius > 0.0f) {
            handleShape.cubicTo(innerOffset + innerControl, innerOffset, innerOffset, innerOffset + innerControl, innerOffset, innerOffset + innerRadius);
        } else {
            handleShape.lineTo(innerOffset, innerOffset);
        }
        handleShape.lineTo(innerOffset, resizeHandleSize);
        handleShape.lineTo(outerRadius, resizeHandleSize);
        handleShape.cubicTo(outerControl, resizeHandleSize, 0.0f, resizeHandleSize - outerControl, 0.0f, resizeHandleSize - outerRadius);
        handleShape.lineTo(0.0f, outerRadius);
        handleShape.cubicTo(0.0f, outerControl, outerControl, 0.0f, outerRadius, 0.0f);
        handleShape.closeSubPath();

        resizeHandleImage = NVGImage(nvg, pixelSize, pixelSize, [handleShape, pixelSize](Graphics& g) {
            g.setColour(Colours::white);
            g.fillPath(handleShape, AffineTransform::translation(resizeHandleMargin, resizeHandleMargin).scaled(pixelSize / resizeHandleImageSize));
        }, NVGImage::AlphaImage);
    }

    if (!resizeHandleImage.isValid())
        return;
    
    auto const imageId = resizeHandleImage.getImageId();
    NVGScopedState scopedState(nvg);
    for (int i = 0; i < 4; i++) {
        bool const isRight = i == 1 || i == 2;
        bool const isBottom = i >= 2;
        auto const x = isRight ? bounds.getRight() + resizeHandleMargin : bounds.getX() - resizeHandleMargin;
        auto const y = isBottom ? bounds.getBottom() + resizeHandleMargin : bounds.getY() - resizeHandleMargin;

        nanovg::nvgFillPaint(nvg, nanovg::nvgImageAlphaPattern(nvg, x, y, resizeHandleImageSize, resizeHandleImageSize, i * MathConstants<float>::halfPi, imageId, colour));
        nanovg::nvgFillRect(nvg, isRight ? x - resizeHandleImageSize : x, isBottom ? y - resizeHandleImageSize : y, resizeHandleImageSize, resizeHandleImageSize);
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
    auto const& colours = getThemeColours(*this);

    constexpr auto halfSize = infiniteCanvasSize / 2;
    auto const zoom = getValue<float>(zoomScale);
    bool const isLocked = getValue<bool>(locked);
    nanovg::nvgSave(nvg);

    if (viewport) {
        nanovg::nvgTranslate(nvg, -viewport->getViewPositionX(), -viewport->getViewPositionY());
        nanovg::nvgScale(nvg, zoom, zoom);
        invalidRegion = invalidRegion.translated(viewport->getViewPositionX(), viewport->getViewPositionY());
        invalidRegion /= zoom;

        if (isLocked) {
            nanovg::nvgFillColor(nvg, nvgColour(colours.canvasBackgroundColour));
            nanovg::nvgFillRect(nvg, invalidRegion.getX(), invalidRegion.getY(), invalidRegion.getWidth(), invalidRegion.getHeight());
        } else {
            // Use least common multiple of grid sizes: 5,10,15,20,25,30 for texture size for now
            // We repeat the texture on GPU, this is so the texture does not become too small for GPU processing
            // There will be a best fit depending on CPU/GPU calcuations.
            // But currently 300 works well on GPU.
            {
                constexpr auto gridSizeCommon = 300;
                updateCanvasDots(nvg);

                NVGScopedState scopedState(nvg);
                // offset image texture by 2.5f so no dots are on the edge of the texture
                nanovg::nvgTranslate(nvg, canvasOrigin.x - 2.5f, canvasOrigin.x - 2.5f);

                nanovg::nvgFillPaint(nvg, nanovg::nvgImagePattern(nvg, 0, 0, gridSizeCommon, gridSizeCommon, 0, dotsLargeImage.getImage(), 1));
                auto startPos = infiniteCanvasSize / 2;
                nanovg::nvgFillRect(nvg, -startPos, -startPos, infiniteCanvasSize, infiniteCanvasSize);
            }
        }
    }

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
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, rx, ry, rw, rh, 8.0f);
            nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(ar, ag, ab, 22));
            nanovg::nvgFill(nvg);
            nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(ar, ag, ab, 140));
            nanovg::nvgStrokeWidth(nvg, 1.2f);
            nanovg::nvgLineStyle(nvg, NVG_LINE_DASHED);
            nanovg::nvgDashLength(nvg, 6.0f);
            nanovg::nvgStroke(nvg);
            nanovg::nvgLineStyle(nvg, NVG_LINE_SOLID);

            // 2. Hardware Module Faceplate Title Badge
            if (r.title.isNotEmpty()) {
                mcpnvg::nvgFontSize(nvg, 11.0f);
                mcpnvg::nvgFontFace(nvg, "Inter");
                float bounds[4];
                mcpnvg::nvgTextBounds(nvg, rx + 10.0f, ry + 6.0f, r.title.toRawUTF8(), nullptr, bounds);
                float const tw = bounds[2] - bounds[0];

                // Dark chassis badge
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgRoundedRect(nvg, rx + 8.0f, ry + 5.0f, tw + 22.0f, 18.0f, 4.0f);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(16, 18, 22, 230));
                nanovg::nvgFill(nvg);
                nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(ar, ag, ab, 100));
                nanovg::nvgStrokeWidth(nvg, 1.0f);
                nanovg::nvgStroke(nvg);

                // Status Jewel LED
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgCircle(nvg, rx + 16.0f, ry + 14.0f, 2.5f);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(ar, ag, ab, 255));
                nanovg::nvgFill(nvg);

                // Title Text
                mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(ar, ag, ab, 245));
                mcpnvg::nvgText(nvg, rx + 23.0f, ry + 14.0f, r.title.toRawUTF8(), nullptr);
            }
        }
    }

    // PRD Phase 2: pasted reference images render as glass backdrops behind cords/objects.
    renderReferenceImages(nvg, invalidRegion);
    renderVoiceTakes(nvg, invalidRegion);
    renderMidiTakes(nvg, invalidRegion);

    auto drawBorder = [this, nvg](bool const bg, bool const fg) {
        auto const& colours = getThemeColours(*this);

        if (viewport && (showOrigin || showBorder) && !::getValue<bool>(presentationMode)) {
            NVGScopedState scopedState(nvg);
            nanovg::nvgBeginPath(nvg);

            auto const borderWidth = getValue<float>(patchWidth);
            auto const borderHeight = getValue<float>(patchHeight);
            constexpr auto pos = Point<int>(halfSize, halfSize);

            if (bg) {
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgMoveTo(nvg, pos.x, pos.y);
                nanovg::nvgLineTo(nvg, pos.x, pos.y + (showOrigin ? halfSize : borderHeight));
                nanovg::nvgMoveTo(nvg, pos.x, pos.y);
                nanovg::nvgLineTo(nvg, pos.x + (showOrigin ? halfSize : borderWidth), pos.y);

                if (showBorder) {
                    nanovg::nvgMoveTo(nvg, pos.x + borderWidth, pos.y);
                    nanovg::nvgLineTo(nvg, pos.x + borderWidth, pos.y + borderHeight);
                    nanovg::nvgLineTo(nvg, pos.x, pos.y + borderHeight);
                }
                nanovg::nvgLineStyle(nvg, NVG_LINE_SOLID);
                nanovg::nvgStrokeColor(nvg, nvgColour(colours.canvasBackgroundColour));
                nanovg::nvgStrokeWidth(nvg, 8.0f);
                nanovg::nvgStroke(nvg);

                nanovg::nvgFillColor(nvg, nvgColour(colours.canvasBackgroundColour));
                nanovg::nvgFillRect(nvg, pos.x - 1.0f, pos.y - 1.0f, 2, 2);
            }

            nanovg::nvgStrokeColor(nvg, nvgColour(colours.canvasDotsColour.interpolatedWith(colours.canvasBackgroundColour, 0.2f)));
            nanovg::nvgStrokeWidth(nvg, 1.5f);
            nanovg::nvgDashLength(nvg, 8.0f);
            nanovg::nvgLineStyle(nvg, NVG_LINE_DASHED);

            if (fg) {
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgMoveTo(nvg, pos.x, pos.y);
                nanovg::nvgLineTo(nvg, pos.x, pos.y + (showOrigin ? halfSize : borderHeight));
                nanovg::nvgStroke(nvg);
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgMoveTo(nvg, pos.x, pos.y);
                nanovg::nvgLineTo(nvg, pos.x + (showOrigin ? halfSize : borderWidth), pos.y);
                nanovg::nvgStroke(nvg);
            }
            if (showBorder && fg) {
                nanovg::nvgStrokeWidth(nvg, 1.5f);
                nanovg::nvgLineStyle(nvg, NVG_LINE_DASHED);
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgMoveTo(nvg, pos.x + borderWidth, pos.y + borderHeight);
                nanovg::nvgLineTo(nvg, pos.x + borderWidth, pos.y);
                nanovg::nvgStroke(nvg);
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgMoveTo(nvg, pos.x + borderWidth, pos.y + borderHeight);
                nanovg::nvgLineTo(nvg, pos.x, pos.y + borderHeight);
                nanovg::nvgStroke(nvg);

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
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRect(nvg, 0, 0, infiniteCanvasSize, infiniteCanvasSize);
            nanovg::nvgPathWinding(nvg, NVG_HOLE);
            nanovg::nvgRoundedRect(nvg, pos.getX(), pos.getY(), borderWidth, borderHeight, windowCorner);
            nanovg::nvgFillColor(nvg, nvgColour(colours.presentationBackgroundColour));
            nanovg::nvgFill(nvg);

            // background drop shadow to simulate a virtual plugin
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRect(nvg, 0, 0, infiniteCanvasSize, infiniteCanvasSize);
            nanovg::nvgPathWinding(nvg, NVG_HOLE);
            nanovg::nvgRoundedRect(nvg, pos.getX(), pos.getY(), borderWidth, borderHeight, windowCorner);

            int const shadowSize = 24 / scale;
            auto borderArea = Rectangle<int>(0, 0, borderWidth, borderHeight).expanded(shadowSize);
            if (presentationShadowImage.needsUpdate(borderArea.getWidth(), borderArea.getHeight())) {
                presentationShadowImage = NVGImage(nvg, borderArea.getWidth(), borderArea.getHeight(), [borderArea, shadowSize, windowCorner](Graphics& g) { StackShadow::drawShadowForRect(g, borderArea.reduced(shadowSize).withPosition(shadowSize, shadowSize), shadowSize, windowCorner, 0.3f, 2); }, NVGImage::AlphaImage);
            }
            auto const shadowImage = nanovg::nvgImageAlphaPattern(nvg, pos.getX() - shadowSize, pos.getY() - shadowSize, borderArea.getWidth(), borderArea.getHeight(), 0, presentationShadowImage.getImageId(), nvgColour(Colours::black));

            nanovg::nvgStrokeColor(nvg, nvgColour(colours.presentationBackgroundColour.contrasting(0.3f)));
            nanovg::nvgStrokeWidth(nvg, 0.5f / scale);
            nanovg::nvgFillPaint(nvg, shadowImage);
            nanovg::nvgFill(nvg);
            nanovg::nvgStroke(nvg);
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
            NVGcolor const accent = nanovg::nvgRGBA(ar, ag, ab, 255);
            bool const isHovered = (noteIdx == mcpNoteHover);
            bool const isCloseHover = (isHovered && mcpNoteHoverClose);

            // 1. Tactile Selection Region: illuminate with soft neon tint on hover
            if (isHovered && !grpBounds.isEmpty()) {
                constexpr int pad = 6;
                float const rx = canvasOrigin.x + static_cast<float>(grpBounds.getX() - pad);
                float const ry = canvasOrigin.y + static_cast<float>(grpBounds.getY() - pad);
                float const rw = static_cast<float>(grpBounds.getWidth() + pad * 2);
                float const rh = static_cast<float>(grpBounds.getHeight() + pad * 2);

                nanovg::nvgBeginPath(nvg);
                nanovg::nvgRoundedRect(nvg, rx, ry, rw, rh, 5.0f);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(ar, ag, ab, 22));
                nanovg::nvgFill(nvg);
                nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(ar, ag, ab, 170));
                nanovg::nvgStrokeWidth(nvg, 1.2f);
                nanovg::nvgLineStyle(nvg, NVG_LINE_DASHED);
                nanovg::nvgDashLength(nvg, 5.0f);
                nanovg::nvgStroke(nvg);
                nanovg::nvgLineStyle(nvg, NVG_LINE_SOLID);
            }

            // 2. Hardware Console Tape Anchor: vertical tick connecting tape to module
            if (hasLead && !grpBounds.isEmpty()) {
                float const tickX = ax + 14.0f;
                float const tickY1 = ay + tapeH;
                float const tickY2 = canvasOrigin.y + static_cast<float>(grpBounds.getY());
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgMoveTo(nvg, tickX, tickY1);
                nanovg::nvgLineTo(nvg, tickX, tickY2);
                nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(ar, ag, ab, isHovered ? 200 : 120));
                nanovg::nvgStrokeWidth(nvg, 1.0f);
                nanovg::nvgStroke(nvg);

                // Small anchor rivet dot at module top edge
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgCircle(nvg, tickX, tickY2, 2.0f);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(ar, ag, ab, isHovered ? 240 : 160));
                nanovg::nvgFill(nvg);
            }

            // 3. Drop Shadow for depth
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, ax, ay + 1.0f, tapeW, tapeH, 4.0f);
            nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(10, 10, 14, 180));
            nanovg::nvgFill(nvg);

            // 4. Console Tape Chassis (Matte dark Eurorack scribble strip)
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, ax, ay, tapeW, tapeH, 3.5f);
            nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(20, 21, 26, isHovered ? 250 : 230));
            nanovg::nvgFill(nvg);
            nanovg::nvgStrokeColor(nvg, isHovered ? accent : nanovg::nvgRGBA(ar, ag, ab, 140));
            nanovg::nvgStrokeWidth(nvg, isHovered ? 1.4f : 1.0f);
            nanovg::nvgStroke(nvg);

            // 5. Top bevel reflection (milled brushed aluminum edge)
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgMoveTo(nvg, ax + 3.0f, ay + 1.0f);
            nanovg::nvgLineTo(nvg, ax + tapeW - 3.0f, ay + 1.0f);
            nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(255, 255, 255, isHovered ? 50 : 25));
            nanovg::nvgStrokeWidth(nvg, 1.0f);
            nanovg::nvgStroke(nvg);

            // 6. Left accent bar
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, ax + 2.0f, ay + 2.5f, 2.5f, tapeH - 5.0f, 1.2f);
            nanovg::nvgFillColor(nvg, accent);
            nanovg::nvgFill(nvg);

            // 7. Jewel LED Diode on tape
            float const ledX = ax + 14.0f;
            float const ledY = ay + tapeH * 0.5f;

            // Soft glow
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgCircle(nvg, ledX, ledY, 5.0f);
            nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(ar, ag, ab, isHovered ? 60 : 30));
            nanovg::nvgFill(nvg);

            // Diode body
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgCircle(nvg, ledX, ledY, 2.8f);
            nanovg::nvgFillColor(nvg, accent);
            nanovg::nvgFill(nvg);

            // Specular glint
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgCircle(nvg, ledX - 0.8f, ledY - 0.8f, 0.8f);
            nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 255, 255, 220));
            nanovg::nvgFill(nvg);

            // 8. Scribble text
            mcpnvg::nvgFontSize(nvg, 11.5f);
            mcpnvg::nvgFontFace(nvg, "Inter");
            mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

            // Scissor so text NEVER overflows past right action buttons
            float const textLeft = ax + 24.0f;
            float const rightZoneW = 32.0f; // space for sidebar icon + close x
            float const maxTextW = std::max(20.0f, tapeW - 24.0f - rightZoneW);
            nanovg::nvgScissor(nvg, textLeft, ay, maxTextW, tapeH);
            nanovg::nvgFillColor(nvg, isHovered ? nanovg::nvgRGBA(255, 255, 255, 255) : nanovg::nvgRGBA(225, 228, 238, 230));
            mcpnvg::nvgText(nvg, textLeft, ay + tapeH * 0.5f, a.text.toRawUTF8(), nullptr);
            nanovg::nvgResetScissor(nvg);

            // 9. Right Icons: Notebook link ↗ & Dismiss ✕
            // Sidebar Notebook link icon (subtle ↗ arrow)
            float const iconX = ax + tapeW - 23.0f;
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgMoveTo(nvg, iconX - 2.5f, ay + tapeH * 0.5f + 2.5f);
            nanovg::nvgLineTo(nvg, iconX + 2.5f, ay + tapeH * 0.5f - 2.5f);
            nanovg::nvgMoveTo(nvg, iconX - 0.5f, ay + tapeH * 0.5f - 2.5f);
            nanovg::nvgLineTo(nvg, iconX + 2.5f, ay + tapeH * 0.5f - 2.5f);
            nanovg::nvgLineTo(nvg, iconX + 2.5f, ay + tapeH * 0.5f + 0.5f);
            nanovg::nvgStrokeColor(nvg, (!isCloseHover && isHovered) ? accent : nanovg::nvgRGBA(ar, ag, ab, 120));
            nanovg::nvgStrokeWidth(nvg, 1.1f);
            nanovg::nvgStroke(nvg);

            // Dismiss 'x'
            float const closeX = ax + tapeW - 9.0f;
            mcpnvg::nvgFontSize(nvg, 10.5f);
            mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nanovg::nvgFillColor(nvg, isCloseHover ? nanovg::nvgRGBA(255, 90, 90, 255) : (isHovered ? nanovg::nvgRGBA(255, 140, 140, 200) : nanovg::nvgRGBA(ar, ag, ab, 130)));
            mcpnvg::nvgText(nvg, closeX, ay + tapeH * 0.5f, "x", nullptr);

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
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgRoundedRect(nvg, drawerX, drawerY + 1.0f, drawerW, drawerH, 4.0f);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(8, 9, 12, 160));
                nanovg::nvgFill(nvg);

                // Smoked glass chassis with subtle accent stroke
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgRoundedRect(nvg, drawerX, drawerY, drawerW, drawerH, 3.5f);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(18, 19, 24, 245));
                nanovg::nvgFill(nvg);
                nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(ar, ag, ab, 90));
                nanovg::nvgStrokeWidth(nvg, 1.0f);
                nanovg::nvgStroke(nvg);

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
                            bgCol = nanovg::nvgRGBA(170, 45, 45, 240);
                            strkCol = nanovg::nvgRGBA(255, 90, 90, 255);
                            txtCol = nanovg::nvgRGBA(255, 255, 255, 255);
                        } else {
                            bgCol = nanovg::nvgRGBA(34, 22, 24, 210);
                            strkCol = nanovg::nvgRGBA(110, 40, 40, 160);
                            txtCol = nanovg::nvgRGBA(230, 140, 140, 220);
                        }
                    } else if (chip.prompt == "__reply__") {
                        if (isThisChipHovered) {
                            bgCol = nanovg::nvgRGBA(45, 115, 215, 240);
                            strkCol = nanovg::nvgRGBA(90, 175, 255, 255);
                            txtCol = nanovg::nvgRGBA(255, 255, 255, 255);
                        } else {
                            bgCol = nanovg::nvgRGBA(24, 32, 46, 210);
                            strkCol = nanovg::nvgRGBA(55, 95, 150, 160);
                            txtCol = nanovg::nvgRGBA(155, 200, 255, 220);
                        }
                    } else {
                        // Sound pivot chip
                        if (isThisChipHovered) {
                            bgCol = nanovg::nvgRGBA(ar, ag, ab, 220);
                            strkCol = accent;
                            txtCol = nanovg::nvgRGBA(14, 16, 20, 255);
                        } else {
                            bgCol = nanovg::nvgRGBA(28, 30, 38, 220);
                            strkCol = nanovg::nvgRGBA(65, 75, 95, 160);
                            txtCol = nanovg::nvgRGBA(215, 222, 235, 230);
                        }
                    }

                    nanovg::nvgBeginPath(nvg);
                    nanovg::nvgRoundedRect(nvg, curChipX, chipY, cw, chipH, 2.5f);
                    nanovg::nvgFillColor(nvg, bgCol);
                    nanovg::nvgFill(nvg);
                    nanovg::nvgStrokeColor(nvg, strkCol);
                    nanovg::nvgStrokeWidth(nvg, 1.0f);
                    nanovg::nvgStroke(nvg);

                    mcpnvg::nvgFontSize(nvg, 10.0f);
                    mcpnvg::nvgFontFace(nvg, "Inter");
                    mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                    nanovg::nvgFillColor(nvg, txtCol);
                    mcpnvg::nvgText(nvg, curChipX + cw * 0.5f, chipY + chipH * 0.5f, chip.label.toRawUTF8(), nullptr);

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
        nanovg::nvgTranslate(nvg, static_cast<float>(nb.getX()), static_cast<float>(nb.getY()));
        nanovg::nvgDrawRoundedRect(nvg, 0, 0, static_cast<float>(nb.getWidth()), static_cast<float>(nb.getHeight()),
                           nanovg::nvgRGBA(22, 22, 28, 255), nanovg::nvgRGBA(74, 158, 255, 255), 4.0f);
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
            nanovg::nvgTranslate(nvg, static_cast<float>(panel.getX()), static_cast<float>(panel.getY()));
            nanovg::nvgDrawRoundedRect(nvg, 0, 0, w, h, nanovg::nvgRGBA(24, 26, 30, 250), nanovg::nvgRGBA(55, 60, 72, 220), 8.0f);

            // Audio-reactive border glow while recording voice / beatbox
            if (isVoiceRec) {
                float const lvl = voiceLvl;
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgRoundedRect(nvg, -1.0f, -1.0f, w + 2.0f, h + 2.0f, 9.0f);
                nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(255, 59, 48, static_cast<unsigned char>(130 + juce::jlimit(0.0f, 125.0f, lvl * 350.0f))));
                nanovg::nvgStrokeWidth(nvg, 1.5f + lvl * 3.5f);
                nanovg::nvgStroke(nvg);
            }

            // Subtle top highlight line
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgMoveTo(nvg, 8.0f, 1.0f);
            nanovg::nvgLineTo(nvg, w - 8.0f, 1.0f);
            nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(255, 255, 255, 25));
            nanovg::nvgStrokeWidth(nvg, 1.0f);
            nanovg::nvgStroke(nvg);

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
                bool const midiRec = pd && pd->getMCPBridge() && pd->getMCPBridge()->isMidiCapturing();

                for (int i = 0; i < 6; ++i) {
                    float const bx = pad + i * (btn + gap);
                    bool const active = (i == 0 && mcpSketchToolActive) || (i == 1 && mcpTextToolActive) || (i == 3 && melodyRec) || (i == 4 && beatRec) || (i == 5 && midiRec);

                    NVGcolor bg = nanovg::nvgRGBA(38, 42, 50, 230);
                    NVGcolor border = nanovg::nvgRGBA(160, 175, 200, 100);
                    if (active) {
                        if (i == 5) {
                            bg = nanovg::nvgRGBA(140, 60, 230, 245);
                            border = nanovg::nvgRGBA(180, 100, 255, 255);
                        } else {
                            bg = nanovg::nvgRGBA(0, 190, 230, 245);
                            border = nanovg::nvgRGBA(0, 245, 255, 255);
                        }
                    }
                    nanovg::nvgDrawRoundedRect(nvg, bx, by, btn, btn, bg, border, 6.0f);

                    float const cx = bx + btn * 0.5f;
                    float const cy = by + btn * 0.5f;
                    NVGcolor const fg = active ? nanovg::nvgRGBA(12, 14, 18, 255) : nanovg::nvgRGBA(212, 224, 240, 235);

                    if (i == 0) { // pen
                        nanovg::nvgBeginPath(nvg);
                        nanovg::nvgMoveTo(nvg, cx - 5.0f, cy + 5.0f);
                        nanovg::nvgLineTo(nvg, cx + 3.0f, cy - 5.0f);
                        nanovg::nvgLineTo(nvg, cx + 5.5f, cy - 2.5f);
                        nanovg::nvgLineTo(nvg, cx - 2.5f, cy + 5.5f);
                        nanovg::nvgClosePath(nvg);
                        nanovg::nvgStrokeColor(nvg, fg);
                        nanovg::nvgStrokeWidth(nvg, 1.3f);
                        nanovg::nvgStroke(nvg);
                        nanovg::nvgBeginPath(nvg);
                        nanovg::nvgMoveTo(nvg, cx - 5.0f, cy + 5.0f);
                        nanovg::nvgLineTo(nvg, cx - 6.5f, cy + 6.5f);
                        nanovg::nvgStroke(nvg);
                    } else if (i == 1) { // text
                        mcpnvg::nvgFontFace(nvg, "Inter");
                        mcpnvg::nvgFontSize(nvg, 13.0f);
                        mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                        nanovg::nvgFillColor(nvg, fg);
                        mcpnvg::nvgText(nvg, cx, cy, "T", nullptr);
                    } else if (i == 2) { // image
                        nanovg::nvgBeginPath(nvg);
                        nanovg::nvgRoundedRect(nvg, cx - 6.0f, cy - 5.0f, 12.0f, 10.0f, 1.8f);
                        nanovg::nvgStrokeColor(nvg, fg);
                        nanovg::nvgStrokeWidth(nvg, 1.2f);
                        nanovg::nvgStroke(nvg);
                        nanovg::nvgBeginPath(nvg);
                        nanovg::nvgMoveTo(nvg, cx - 5.0f, cy + 3.0f);
                        nanovg::nvgLineTo(nvg, cx - 1.0f, cy - 1.0f);
                        nanovg::nvgLineTo(nvg, cx + 2.0f, cy + 1.5f);
                        nanovg::nvgLineTo(nvg, cx + 5.0f, cy - 2.0f);
                        nanovg::nvgStrokeColor(nvg, fg);
                        nanovg::nvgStrokeWidth(nvg, 1.1f);
                        nanovg::nvgStroke(nvg);
                    } else if (i == 3) { // melody (beamed notes)
                        nanovg::nvgBeginPath(nvg);
                        nanovg::nvgEllipse(nvg, cx - 3.5f, cy + 4.5f, 2.8f, 2.0f);
                        nanovg::nvgFillColor(nvg, fg);
                        nanovg::nvgFill(nvg);
                        nanovg::nvgBeginPath(nvg);
                        nanovg::nvgEllipse(nvg, cx + 4.0f, cy + 3.0f, 2.8f, 2.0f);
                        nanovg::nvgFillColor(nvg, fg);
                        nanovg::nvgFill(nvg);
                        nanovg::nvgBeginPath(nvg);
                        nanovg::nvgMoveTo(nvg, cx - 0.9f, cy + 4.4f);
                        nanovg::nvgLineTo(nvg, cx - 0.9f, cy - 5.5f);
                        nanovg::nvgLineTo(nvg, cx + 6.6f, cy - 6.8f);
                        nanovg::nvgLineTo(nvg, cx + 6.6f, cy + 2.9f);
                        nanovg::nvgStrokeColor(nvg, fg);
                        nanovg::nvgStrokeWidth(nvg, 1.4f);
                        nanovg::nvgStroke(nvg);

                        if (melodyRec) { // live level aura
                            float const lvl = pd->getMCPBridge()->getVoiceLiveLevel();
                            nanovg::nvgBeginPath(nvg);
                            nanovg::nvgRoundedRect(nvg, bx - 2.0f - lvl * 4.0f, by - 2.0f - lvl * 4.0f,
                                           btn + 4.0f + lvl * 8.0f, btn + 4.0f + lvl * 8.0f, 8.0f);
                            nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(255, 60, 45, static_cast<unsigned char>(120 + juce::jlimit(0.0f, 130.0f, lvl * 350.0f))));
                            nanovg::nvgStrokeWidth(nvg, 1.5f);
                            nanovg::nvgStroke(nvg);
                        }
                    } else if (i == 4) { // beat (drum glyph)
                        nanovg::nvgBeginPath(nvg);
                        nanovg::nvgRoundedRect(nvg, cx - 6.0f, cy - 0.5f, 12.0f, 6.5f, 2.0f);
                        nanovg::nvgStrokeColor(nvg, fg);
                        nanovg::nvgStrokeWidth(nvg, 1.2f);
                        nanovg::nvgStroke(nvg);
                        nanovg::nvgBeginPath(nvg);
                        nanovg::nvgEllipse(nvg, cx, cy - 0.5f, 6.0f, 2.0f);
                        nanovg::nvgStrokeColor(nvg, fg);
                        nanovg::nvgStrokeWidth(nvg, 1.2f);
                        nanovg::nvgStroke(nvg);
                        // crossed beaters
                        nanovg::nvgBeginPath(nvg);
                        nanovg::nvgMoveTo(nvg, cx - 4.5f, cy - 7.0f);
                        nanovg::nvgLineTo(nvg, cx + 1.0f, cy - 1.5f);
                        nanovg::nvgMoveTo(nvg, cx + 4.5f, cy - 7.0f);
                        nanovg::nvgLineTo(nvg, cx - 1.0f, cy - 1.5f);
                        nanovg::nvgStrokeColor(nvg, fg);
                        nanovg::nvgStrokeWidth(nvg, 1.1f);
                        nanovg::nvgStroke(nvg);

                        if (beatRec) { // live level aura
                            float const lvl = pd->getMCPBridge()->getVoiceLiveLevel();
                            nanovg::nvgBeginPath(nvg);
                            nanovg::nvgRoundedRect(nvg, bx - 2.0f - lvl * 4.0f, by - 2.0f - lvl * 4.0f,
                                           btn + 4.0f + lvl * 8.0f, btn + 4.0f + lvl * 8.0f, 8.0f);
                            nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(255, 60, 45, static_cast<unsigned char>(120 + juce::jlimit(0.0f, 130.0f, lvl * 350.0f))));
                            nanovg::nvgStrokeWidth(nvg, 1.5f);
                            nanovg::nvgStroke(nvg);
                        }
                    } else { // i == 5: MIDI (piano keyboard glyph)
                        nanovg::nvgBeginPath(nvg);
                        nanovg::nvgRoundedRect(nvg, cx - 7.0f, cy - 6.0f, 14.0f, 12.0f, 1.5f);
                        nanovg::nvgStrokeColor(nvg, fg);
                        nanovg::nvgStrokeWidth(nvg, 1.2f);
                        nanovg::nvgStroke(nvg);
                        nanovg::nvgBeginPath(nvg);
                        nanovg::nvgMoveTo(nvg, cx - 2.5f, cy - 6.0f);
                        nanovg::nvgLineTo(nvg, cx - 2.5f, cy + 6.0f);
                        nanovg::nvgMoveTo(nvg, cx + 2.0f, cy - 6.0f);
                        nanovg::nvgLineTo(nvg, cx + 2.0f, cy + 6.0f);
                        nanovg::nvgStrokeColor(nvg, fg);
                        nanovg::nvgStrokeWidth(nvg, 1.0f);
                        nanovg::nvgStroke(nvg);
                        nanovg::nvgBeginPath(nvg);
                        nanovg::nvgRect(nvg, cx - 4.5f, cy - 6.0f, 1.8f, 6.5f);
                        nanovg::nvgRect(nvg, cx + 0.2f, cy - 6.0f, 1.8f, 6.5f);
                        nanovg::nvgFillColor(nvg, fg);
                        nanovg::nvgFill(nvg);

                        if (midiRec) {
                            float const pulse = 0.65f + 0.35f * std::sin(static_cast<float>(juce::Time::getMillisecondCounter()) * 0.008f);
                            nanovg::nvgBeginPath(nvg);
                            nanovg::nvgRoundedRect(nvg, bx - 2.0f, by - 2.0f, btn + 4.0f, btn + 4.0f, 8.0f);
                            nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(180, 70, 255, static_cast<unsigned char>(140 + 100 * pulse)));
                            nanovg::nvgStrokeWidth(nvg, 1.6f);
                            nanovg::nvgStroke(nvg);
                        }
                    }
                }

                // Record slot: declared intent + live level while capturing.
                if (melodyRec || beatRec || midiRec) {
                    int const nNotes = (pd && pd->getMCPBridge()) ? pd->getMCPBridge()->getMidiCaptureNoteCount() : 0;
                    juce::String const recText = midiRec
                        ? ("Play MIDI keyboard - tap keys to stop (" + juce::String(nNotes) + " notes)")
                        : (melodyRec ? "Sing a melody - tap the note to stop" : "Beatbox - tap the drum to stop");
                    float const boxW = midiRec ? 340.0f : 330.0f;
                    float const boxX = (w - boxW) * 0.5f;
                    float const boxY = -44.0f;
                    NVGcolor const recBorder = midiRec ? nanovg::nvgRGBA(180, 80, 255, 220) : nanovg::nvgRGBA(255, 70, 58, 220);
                    nanovg::nvgDrawRoundedRect(nvg, boxX, boxY, boxW, 32.0f, nanovg::nvgRGBA(26, 18, 20, 242), recBorder, 7.0f);

                    float const pulse = 0.65f + 0.35f * std::sin(static_cast<float>(juce::Time::getMillisecondCounter()) * 0.008f);
                    float const lvl = juce::jlimit(0.0f, 1.0f, pd->getMCPBridge()->getVoiceLiveLevel() * 3.5f);
                    nanovg::nvgBeginPath(nvg);
                    nanovg::nvgCircle(nvg, boxX + 15.0f, boxY + 16.0f, 4.0f);
                    NVGcolor const dotColor = midiRec
                        ? nanovg::nvgRGBA(190, 80, 255, static_cast<unsigned char>(160 + 95 * pulse))
                        : nanovg::nvgRGBA(255, 60, 45, static_cast<unsigned char>(160 + 95 * pulse));
                    nanovg::nvgFillColor(nvg, dotColor);
                    nanovg::nvgFill(nvg);

                    if (!midiRec) {
                        // live level meter
                        nanovg::nvgDrawRoundedRect(nvg, boxX + 26.0f, boxY + 13.0f, 70.0f, 6.0f, nanovg::nvgRGBA(50, 30, 32, 230), nanovg::nvgRGBA(90, 50, 52, 120), 3.0f);
                        nanovg::nvgDrawRoundedRect(nvg, boxX + 26.0f, boxY + 13.0f, 4.0f + lvl * 66.0f, 6.0f, nanovg::nvgRGBA(255, 70, 55, 235), nanovg::nvgRGBA(255, 120, 100, 120), 3.0f);
                    }

                    mcpnvg::nvgFontFace(nvg, "Inter");
                    mcpnvg::nvgFontSize(nvg, 10.5f);
                    mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                    nanovg::nvgFillColor(nvg, midiRec ? nanovg::nvgRGBA(235, 220, 255, 240) : nanovg::nvgRGBA(255, 214, 208, 240));
                    float const tx = midiRec ? (boxX + 28.0f) : (boxX + 104.0f);
                    mcpnvg::nvgText(nvg, tx, boxY + 16.0f, recText.toRawUTF8(), nullptr);
                }

                // Hint chip under the strip
                if (mcpMoodHint.isNotEmpty() && juce::Time::getMillisecondCounter() < mcpMoodHintUntil) {
                    mcpnvg::nvgFontFace(nvg, "Inter");
                    mcpnvg::nvgFontSize(nvg, 11.0f);
                    mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                    float tb[4];
                    mcpnvg::nvgTextBounds(nvg, 0.0f, 0.0f, mcpMoodHint.toRawUTF8(), nullptr, tb);
                    float const hw = (tb[2] - tb[0]) + 16.0f;
                    nanovg::nvgDrawRoundedRect(nvg, 0.0f, h + 6.0f, hw, 20.0f, nanovg::nvgRGBA(16, 18, 22, 232), nanovg::nvgRGBA(255, 190, 80, 150), 5.0f);
                    nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 214, 150, 240));
                    mcpnvg::nvgText(nvg, 8.0f, h + 16.0f, mcpMoodHint.toRawUTF8(), nullptr);
                }
            } else if (mcpPillSketchMode) {
                // Moodboard Chisel: [⚡ Build] | prompt | [T] [🖼] [↩ Undo] [🗑 Clear] [✏️ pen]
                float const chipH = 22.0f;
                float const chipY = (h - chipH) * 0.5f;

                // ⚡ Build chip — the commit action (accent blue)
                {
                    float const bx = 6.0f;
                    float const bw = 80.0f;
                    nanovg::nvgDrawRoundedRect(nvg, bx, chipY, bw, chipH, nanovg::nvgRGBA(32, 108, 214, 245), nanovg::nvgRGBA(96, 178, 255, 255), 5.0f);
                    // vector lightning bolt
                    nanovg::nvgBeginPath(nvg);
                    nanovg::nvgMoveTo(nvg, bx + 15.0f, chipY + 3.5f);
                    nanovg::nvgLineTo(nvg, bx + 10.0f, chipY + 11.5f);
                    nanovg::nvgLineTo(nvg, bx + 14.0f, chipY + 11.5f);
                    nanovg::nvgLineTo(nvg, bx + 12.0f, chipY + 18.5f);
                    nanovg::nvgLineTo(nvg, bx + 19.0f, chipY + 9.5f);
                    nanovg::nvgLineTo(nvg, bx + 14.5f, chipY + 9.5f);
                    nanovg::nvgLineTo(nvg, bx + 17.0f, chipY + 3.5f);
                    nanovg::nvgClosePath(nvg);
                    nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 235, 130, 255));
                    nanovg::nvgFill(nvg);
                    mcpnvg::nvgFontFace(nvg, "Inter");
                    mcpnvg::nvgFontSize(nvg, 11.5f);
                    mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                    nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 255, 255, 255));
                    mcpnvg::nvgText(nvg, bx + 25.0f, chipY + chipH * 0.5f, "Build", nullptr);
                }

                // T attach-text chip (same tools as the surgical lane)
                {
                    float const tx = w - 234.0f;
                    float const tw2 = 24.0f;
                    nanovg::nvgDrawRoundedRect(nvg, tx, chipY, tw2, chipH,
                                       mcpTextToolActive ? nanovg::nvgRGBA(74, 158, 255, 240) : nanovg::nvgRGBA(38, 42, 50, 230),
                                       mcpTextToolActive ? nanovg::nvgRGBA(140, 195, 255, 255) : nanovg::nvgRGBA(160, 175, 200, 110), 4.0f);
                    mcpnvg::nvgFontFace(nvg, "Inter");
                    mcpnvg::nvgFontSize(nvg, 12.0f);
                    mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                    nanovg::nvgFillColor(nvg, mcpTextToolActive ? nanovg::nvgRGBA(255, 255, 255, 255) : nanovg::nvgRGBA(215, 225, 240, 235));
                    mcpnvg::nvgText(nvg, tx + tw2 * 0.5f, chipY + chipH * 0.5f, "T", nullptr);
                }

                // 🖼 attach-image chip (pastes the clipboard onto the mood/sketch)
                {
                    float const ix = w - 206.0f;
                    float const iw = 24.0f;
                    float const icx = ix + iw * 0.5f;
                    float const icy = chipY + chipH * 0.5f;
                    nanovg::nvgDrawRoundedRect(nvg, ix, chipY, iw, chipH, nanovg::nvgRGBA(38, 42, 50, 230), nanovg::nvgRGBA(160, 175, 200, 110), 4.0f);
                    nanovg::nvgBeginPath(nvg);
                    nanovg::nvgRoundedRect(nvg, icx - 5.0f, icy - 4.0f, 10.0f, 8.5f, 1.5f);
                    nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(200, 215, 235, 220));
                    nanovg::nvgStrokeWidth(nvg, 1.1f);
                    nanovg::nvgStroke(nvg);
                    nanovg::nvgBeginPath(nvg);
                    nanovg::nvgMoveTo(nvg, icx - 4.0f, icy + 2.5f);
                    nanovg::nvgLineTo(nvg, icx - 1.0f, icy - 0.5f);
                    nanovg::nvgLineTo(nvg, icx + 1.5f, icy + 1.5f);
                    nanovg::nvgLineTo(nvg, icx + 4.0f, icy + 0.5f);
                    nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(160, 230, 200, 200));
                    nanovg::nvgStrokeWidth(nvg, 1.0f);
                    nanovg::nvgStroke(nvg);
                }

                // ↩ Undo chip
                {
                    float const ux = w - 178.0f;
                    float const uw = 70.0f;
                    float const ucy = chipY + chipH * 0.5f;
                    nanovg::nvgDrawRoundedRect(nvg, ux, chipY, uw, chipH, nanovg::nvgRGBA(38, 42, 50, 235), nanovg::nvgRGBA(160, 175, 200, 110), 4.0f);
                    // back-arrow glyph
                    nanovg::nvgBeginPath(nvg);
                    nanovg::nvgMoveTo(nvg, ux + 18.0f, ucy);
                    nanovg::nvgLineTo(nvg, ux + 10.0f, ucy);
                    nanovg::nvgMoveTo(nvg, ux + 13.5f, ucy - 3.0f);
                    nanovg::nvgLineTo(nvg, ux + 10.0f, ucy);
                    nanovg::nvgLineTo(nvg, ux + 13.5f, ucy + 3.0f);
                    nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(150, 200, 255, 235));
                    nanovg::nvgStrokeWidth(nvg, 1.4f);
                    nanovg::nvgStroke(nvg);
                    mcpnvg::nvgFontFace(nvg, "Inter");
                    mcpnvg::nvgFontSize(nvg, 10.5f);
                    mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                    nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(215, 225, 240, 235));
                    mcpnvg::nvgText(nvg, ux + 23.0f, ucy, "Undo", nullptr);
                }

                // 🗑 Clear chip
                {
                    float const cxX = w - 104.0f;
                    float const cxW = 70.0f;
                    float const ccy = chipY + chipH * 0.5f;
                    nanovg::nvgDrawRoundedRect(nvg, cxX, chipY, cxW, chipH, nanovg::nvgRGBA(58, 30, 34, 235), nanovg::nvgRGBA(220, 110, 110, 140), 4.0f);
                    // trash glyph
                    float const tcx = cxX + 14.0f;
                    nanovg::nvgBeginPath(nvg);
                    nanovg::nvgRoundedRect(nvg, tcx - 3.5f, ccy - 3.0f, 7.0f, 8.5f, 1.5f);
                    nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(255, 175, 175, 240));
                    nanovg::nvgStrokeWidth(nvg, 1.3f);
                    nanovg::nvgStroke(nvg);
                    nanovg::nvgBeginPath(nvg);
                    nanovg::nvgMoveTo(nvg, tcx - 5.0f, ccy - 4.5f);
                    nanovg::nvgLineTo(nvg, tcx + 5.0f, ccy - 4.5f);
                    nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(255, 175, 175, 240));
                    nanovg::nvgStrokeWidth(nvg, 1.3f);
                    nanovg::nvgStroke(nvg);
                    mcpnvg::nvgFontFace(nvg, "Inter");
                    mcpnvg::nvgFontSize(nvg, 10.5f);
                    mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                    nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 200, 200, 240));
                    mcpnvg::nvgText(nvg, cxX + 24.0f, ccy, "Clear", nullptr);
                }

                // ✏️ Pen toggle — put the pen down / pick it back up without knowing Esc
                {
                    float const px = w - 30.0f;
                    float const pw = 24.0f;
                    float const pcy = chipY + chipH * 0.5f;
                    if (mcpSketchToolActive) {
                        nanovg::nvgDrawRoundedRect(nvg, px, chipY, pw, chipH, nanovg::nvgRGBA(0, 180, 220, 240), nanovg::nvgRGBA(0, 245, 255, 255), 4.0f);
                    } else {
                        nanovg::nvgDrawRoundedRect(nvg, px, chipY, pw, chipH, nanovg::nvgRGBA(38, 42, 50, 230), nanovg::nvgRGBA(160, 175, 200, 80), 4.0f);
                    }
                    float const pcx = px + pw * 0.5f;
                    nanovg::nvgBeginPath(nvg);
                    nanovg::nvgMoveTo(nvg, pcx - 4.5f, pcy + 4.5f);
                    nanovg::nvgLineTo(nvg, pcx + 2.5f, pcy - 4.5f);
                    nanovg::nvgLineTo(nvg, pcx + 5.0f, pcy - 2.0f);
                    nanovg::nvgLineTo(nvg, pcx - 2.0f, pcy + 5.0f);
                    nanovg::nvgClosePath(nvg);
                    nanovg::nvgStrokeColor(nvg, mcpSketchToolActive ? nanovg::nvgRGBA(255, 255, 255, 255) : nanovg::nvgRGBA(200, 215, 235, 220));
                    nanovg::nvgStrokeWidth(nvg, 1.2f);
                    nanovg::nvgStroke(nvg);
                    nanovg::nvgBeginPath(nvg);
                    nanovg::nvgMoveTo(nvg, pcx - 4.5f, pcy + 4.5f);
                    nanovg::nvgLineTo(nvg, pcx - 6.0f, pcy + 6.0f);
                    nanovg::nvgStrokeColor(nvg, mcpSketchToolActive ? nanovg::nvgRGBA(255, 255, 255, 255) : nanovg::nvgRGBA(0, 230, 255, 220));
                    nanovg::nvgStrokeWidth(nvg, 1.4f);
                    nanovg::nvgStroke(nvg);
                }
            } else {
            // Left: subtle generative AI sparkle icon ✨
            {
                float const cx = 15.0f;
                float const cy = h * 0.5f;
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgCircle(nvg, cx, cy, 6.0f);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(74, 158, 255, 30));
                nanovg::nvgFill(nvg);
                // 4-point sparkle star
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgMoveTo(nvg, cx, cy - 4.5f);
                nanovg::nvgQuadTo(nvg, cx, cy, cx + 4.5f, cy);
                nanovg::nvgQuadTo(nvg, cx, cy, cx, cy + 4.5f);
                nanovg::nvgQuadTo(nvg, cx, cy, cx - 4.5f, cy);
                nanovg::nvgQuadTo(nvg, cx, cy, cx, cy - 4.5f);
                nanovg::nvgClosePath(nvg);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(140, 195, 255, 220));
                nanovg::nvgFill(nvg);
            }

            // Right ~26px: sleek submit button ↵
            {
                float const subW = 20.0f;
                float const subH = 18.0f;
                float const subX = w - 25.0f;
                float const subY = (h - subH) * 0.5f;
                nanovg::nvgDrawRoundedRect(nvg, subX, subY, subW, subH, nanovg::nvgRGBA(74, 158, 255, 220), nanovg::nvgRGBA(140, 195, 255, 255), 4.0f);
                mcpnvg::nvgFontFace(nvg, "Inter");
                mcpnvg::nvgFontSize(nvg, 11.5f);
                mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 255, 255, 255));
                mcpnvg::nvgText(nvg, subX + subW * 0.5f, subY + subH * 0.5f, "↵", nullptr);
            }
            } // else: object-pill controls
        }

        // Ambient blinking caret when unfocused & empty to clearly signal interactive input affordance
        if (mcpSelectionPill && !mcpSelectionPill->hasKeyboardFocus(true) && mcpSelectionPill->getText().isEmpty()
            && !(mcpPillSketchMode && mcpCompletedStrokes.empty())) {
            bool const caretBlink = ((juce::Time::getMillisecondCounter() / 530) % 2) == 0;
            if (caretBlink) {
                NVGScopedState scopedCaret(nvg);
                float const caretX = panel.getX() + (mcpPillSketchMode ? 96.0f : 38.0f);
                float const caretY = panel.getY() + panel.getHeight() * 0.5f;
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgMoveTo(nvg, caretX, caretY - 7.0f);
                nanovg::nvgLineTo(nvg, caretX, caretY + 7.0f);
                nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(74, 158, 255, 230));
                nanovg::nvgStrokeWidth(nvg, 1.5f);
                nanovg::nvgStroke(nvg);
            }
        }

        // Crisp on-canvas placeholder when empty (subtle dim when focused)
        if (mcpSelectionPill && mcpSelectionPill->getText().isEmpty()
            && !(mcpPillSketchMode && mcpCompletedStrokes.empty())) {
            NVGScopedState scopedHint(nvg);
            mcpnvg::nvgFontFace(nvg, "Inter");
            mcpnvg::nvgFontSize(nvg, 12.0f);
            mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            juce::String hint;
            if (mcpPillSketchMode) {
                hint = mcpCompletedStrokes.empty()
                    ? "New Mood — paint, paste, type... then Build"
                    : "Describe the build, or hit Build...";
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(140, 155, 175, mcpSelectionPill->hasKeyboardFocus(true) ? 90 : 170));
            } else if (isVoiceRec) {
                hint = "Listening... (speak prompt or beatbox groove)";
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 110, 95, 240));
            } else if (mcpActiveLens.isNotEmpty()) {
                hint = "[" + mcpActiveLens.toUpperCase() + "] Ask AI on selection...";
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(140, 155, 175, mcpSelectionPill->hasKeyboardFocus(true) ? 90 : 170));
            } else {
                hint = "Ask AI on selection... (Enter to mutate, Esc to cancel)";
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(140, 155, 175, mcpSelectionPill->hasKeyboardFocus(true) ? 90 : 170));
            }
            mcpnvg::nvgText(nvg, panel.getX() + (mcpPillSketchMode ? 96.0f : 38.0f), panel.getY() + panel.getHeight() * 0.5f, hint.toRawUTF8(), nullptr);
        }

        {
            NVGScopedState scopedField(nvg);
            auto const fb = mcpSelectionPill->getBounds();
            nanovg::nvgTranslate(nvg, static_cast<float>(fb.getX()), static_cast<float>(fb.getY()));
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
                nanovg::nvgTranslate(nvg, px, py);
                // Frosted dark card container
                nanovg::nvgDrawRoundedRect(nvg, 0, 0, pw, ph, nanovg::nvgRGBA(16, 18, 22, 252), nanovg::nvgRGBA(55, 62, 75, 240), 7.0f);

                // Subtle top highlight
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgMoveTo(nvg, 6.0f, 1.0f);
                nanovg::nvgLineTo(nvg, pw - 6.0f, 1.0f);
                nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(255, 255, 255, 25));
                nanovg::nvgStrokeWidth(nvg, 1.0f);
                nanovg::nvgStroke(nvg);

                // Header title
                mcpnvg::nvgFontFace(nvg, "Inter");
                mcpnvg::nvgFontSize(nvg, 10.0f);
                mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(140, 155, 180, 220));
                mcpnvg::nvgText(nvg, 12.0f, 14.0f, "ADD CONTEXT & ACTIONS (+)", nullptr);

                for (int i = 0; i < visibleCount; ++i) {
                    int const itemIdx = mcpFilteredPaletteIndices[static_cast<size_t>(i)];
                    auto const& item = mcpPaletteItems[static_cast<size_t>(itemIdx)];
                    float const ry = 26.0f + i * rowH;
                    bool const isSelected = (i == mcpPaletteSelectedIndex);

                    if (isSelected) {
                        nanovg::nvgDrawRoundedRect(nvg, 4.0f, ry, pw - 8.0f, rowH - 2.0f,
                                           nanovg::nvgRGBA(74, 158, 255, 55), nanovg::nvgRGBA(74, 158, 255, 210), 4.0f);
                    }

                    // Icon / Prefix
                    mcpnvg::nvgFontSize(nvg, 11.5f);
                    mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                    nanovg::nvgFillColor(nvg, isSelected ? nanovg::nvgRGBA(74, 180, 255, 255) : nanovg::nvgRGBA(150, 165, 190, 220));
                    mcpnvg::nvgText(nvg, 18.0f, ry + (rowH - 2.0f) * 0.5f, item.icon.toRawUTF8(), nullptr);

                    // Category badge chip (Action, Skill, Lens)
                    float const chipW = 42.0f;
                    float const chipH = 14.0f;
                    float const chipX = 30.0f;
                    float const chipY = ry + (rowH - 2.0f - chipH) * 0.5f;
                    NVGcolor chipBg = nanovg::nvgRGBA(40, 48, 62, 200);
                    NVGcolor chipFg = nanovg::nvgRGBA(160, 180, 210, 220);
                    if (item.category == "Action") {
                        chipBg = nanovg::nvgRGBA(30, 90, 60, 150);
                        chipFg = nanovg::nvgRGBA(100, 240, 160, 240);
                    } else if (item.category == "Skill") {
                        chipBg = nanovg::nvgRGBA(90, 40, 110, 150);
                        chipFg = nanovg::nvgRGBA(220, 140, 255, 240);
                    } else if (item.category == "Lens") {
                        chipBg = nanovg::nvgRGBA(30, 70, 110, 150);
                        chipFg = nanovg::nvgRGBA(100, 190, 255, 240);
                    }
                    nanovg::nvgDrawRoundedRect(nvg, chipX, chipY, chipW, chipH, chipBg, chipFg, 3.0f);
                    mcpnvg::nvgFontSize(nvg, 8.5f);
                    mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                    nanovg::nvgFillColor(nvg, chipFg);
                    mcpnvg::nvgText(nvg, chipX + chipW * 0.5f, chipY + chipH * 0.5f, item.category.toRawUTF8(), nullptr);

                    // Title
                    mcpnvg::nvgFontSize(nvg, 11.0f);
                    mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                    nanovg::nvgFillColor(nvg, isSelected ? nanovg::nvgRGBA(255, 255, 255, 255) : nanovg::nvgRGBA(215, 225, 240, 230));
                    mcpnvg::nvgText(nvg, chipX + chipW + 8.0f, ry + (rowH - 2.0f) * 0.5f, item.title.toRawUTF8(), nullptr);

                    // Description (dimmed)
                    mcpnvg::nvgFontSize(nvg, 10.0f);
                    nanovg::nvgFillColor(nvg, isSelected ? nanovg::nvgRGBA(180, 205, 240, 240) : nanovg::nvgRGBA(120, 135, 155, 200));
                    mcpnvg::nvgText(nvg, chipX + chipW + 130.0f, ry + (rowH - 2.0f) * 0.5f, item.desc.toRawUTF8(), nullptr);
                }
            } else {
                float const dw = w;
                float const dh = 30.0f;
                float const dx = static_cast<float>(panel.getX());
                float const dy = static_cast<float>(panel.getY() + panel.getHeight()) + 5.0f;
                mcpPillDrawerFrame = juce::Rectangle<int>(roundToInt(dx), roundToInt(dy), roundToInt(dw), roundToInt(dh));

                NVGScopedState scopedDrawer(nvg);
                nanovg::nvgTranslate(nvg, dx, dy);
                // Frosted dark card background with soft border
                nanovg::nvgDrawRoundedRect(nvg, 0, 0, dw, dh, nanovg::nvgRGBA(18, 20, 24, 250), nanovg::nvgRGBA(55, 62, 75, 230), 6.0f);

                // Subtle top highlight line
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgMoveTo(nvg, 6.0f, 1.0f);
                nanovg::nvgLineTo(nvg, dw - 6.0f, 1.0f);
                nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(255, 255, 255, 20));
                nanovg::nvgStrokeWidth(nvg, 1.0f);
                nanovg::nvgStroke(nvg);

                mcpnvg::nvgFontFace(nvg, "Inter");
                mcpnvg::nvgFontSize(nvg, 11.5f);
                mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

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
                            nanovg::nvgDrawRoundedRect(nvg, ix, iy, iw, ih, nanovg::nvgRGBA(255, 102, 0, 235), nanovg::nvgRGBA(255, 150, 60, 255), 4.0f);
                            nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(18, 18, 20, 255));
                        } else {
                            nanovg::nvgDrawRoundedRect(nvg, ix, iy, iw, ih, nanovg::nvgRGBA(30, 34, 42, 220), nanovg::nvgRGBA(50, 58, 70, 180), 4.0f);
                            nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(210, 220, 235, 230));
                        }
                        mcpnvg::nvgText(nvg, ix + iw * 0.5f, iy + ih * 0.5f, lenses[i], nullptr);
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
                        nanovg::nvgDrawRoundedRect(nvg, ix, iy, iw, ih, nanovg::nvgRGBA(30, 34, 42, 220), nanovg::nvgRGBA(50, 58, 70, 180), 4.0f);
                        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(210, 220, 235, 230));
                        mcpnvg::nvgText(nvg, ix + iw * 0.5f, iy + ih * 0.5f, tools[i], nullptr);
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
        nanovg::nvgTranslate(nvg, static_cast<float>(bx), static_cast<float>(by));
        nanovg::nvgDrawRoundedRect(nvg, 0, 0, static_cast<float>(barW), static_cast<float>(barH),
                           nanovg::nvgRGBA(28, 16, 16, 250), nanovg::nvgRGBA(255, 90, 70, 225), 10.0f);
        mcpnvg::nvgFontFace(nvg, "Inter");
        mcpnvg::nvgFontSize(nvg, 15.0f);
        mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        // Approve (green)
        nanovg::nvgDrawRoundedRect(nvg, 8, 8, static_cast<float>(half), static_cast<float>(barH - 16),
                           nanovg::nvgRGBA(40, 170, 90, 235), nanovg::nvgRGBA(90, 225, 140, 255), 6.0f);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 255, 255, 255));
        mcpnvg::nvgText(nvg, 8 + half * 0.5f, barH * 0.5f, "Approve", nullptr);
        // Cancel (grey)
        nanovg::nvgDrawRoundedRect(nvg, 8 + half + 8, 8, static_cast<float>(half), static_cast<float>(barH - 16),
                           nanovg::nvgRGBA(40, 44, 52, 235), nanovg::nvgRGBA(90, 98, 112, 255), 6.0f);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(210, 220, 235, 235));
        mcpnvg::nvgText(nvg, 8 + half + 8 + half * 0.5f, barH * 0.5f, "Cancel", nullptr);
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
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgRoundedRect(nvg, sx, sy, 320.0f, 220.0f, 8.0f);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(20, 24, 30, 150));
        nanovg::nvgFill(nvg);
        nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(124, 77, 255, 200));
        nanovg::nvgStrokeWidth(nvg, 1.5f);
        nanovg::nvgLineStyle(nvg, NVG_LINE_DASHED);
        nanovg::nvgDashLength(nvg, 7.0f);
        nanovg::nvgStroke(nvg);
        nanovg::nvgLineStyle(nvg, NVG_LINE_SOLID);

        mcpnvg::nvgFontFace(nvg, "Inter");
        mcpnvg::nvgFontSize(nvg, 13.0f);
        mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(198, 208, 226, 225));
        mcpnvg::nvgText(nvg, scx, scy - 8.0f, "Ctrl+V to paste image", nullptr);
        mcpnvg::nvgFontSize(nvg, 11.0f);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(150, 160, 180, 170));
        mcpnvg::nvgText(nvg, scx, scy + 12.0f, "(Esc cancels)", nullptr);
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
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgRoundedRect(nvg, gx, gy, gw, gh, Corners::objectCornerRadius);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(74, 158, 255, 55));
                nanovg::nvgFill(nvg);
                nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(74, 158, 255, 220));
                nanovg::nvgStrokeWidth(nvg, 1.5f);
                nanovg::nvgLineStyle(nvg, NVG_LINE_DASHED);
                nanovg::nvgDashLength(nvg, 6.0f);
                nanovg::nvgStroke(nvg);
                nanovg::nvgLineStyle(nvg, NVG_LINE_SOLID);
                if (g.label.isNotEmpty()) {
                    mcpnvg::nvgFontSize(nvg, 12.0f);
                    mcpnvg::nvgFontFace(nvg, "Inter");
                    mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                    nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(74, 158, 255, 255));
                    mcpnvg::nvgText(nvg, gx + 6.0f, gy + gh * 0.5f, g.label.toRawUTF8(), nullptr);
                }
            } else {
                float const x1 = canvasOrigin.x + g.x1, y1 = canvasOrigin.y + g.y1;
                float const x2 = canvasOrigin.x + g.x2, y2 = canvasOrigin.y + g.y2;
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgMoveTo(nvg, x1, y1);
                float const dx = std::abs(x2 - x1) * 0.5f + 20.0f;
                nanovg::nvgBezierTo(nvg, x1, y1 + dx, x2, y2 - dx, x2, y2);
                nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(74, 158, 255, 220));
                nanovg::nvgStrokeWidth(nvg, 2.0f);
                nanovg::nvgLineStyle(nvg, NVG_LINE_DASHED);
                nanovg::nvgDashLength(nvg, 6.0f);
                nanovg::nvgStroke(nvg);
                nanovg::nvgLineStyle(nvg, NVG_LINE_SOLID);
            }
        }
    }

    if (graphArea) {
        NVGScopedState scopedState(nvg);
        nanovg::nvgTranslate(nvg, graphArea->getX(), graphArea->getY());
        graphArea->render(nvg);
    }

    objectGrid.render(nvg);

    if (viewport && lasso.isVisible() && !lasso.getBounds().isEmpty()) {
        auto lassoBounds = lasso.getBounds();
        lassoBounds = lassoBounds.withSize(jmax(lasso.getWidth(), 2), jmax(lasso.getHeight(), 2));
        nanovg::nvgDrawRoundedRect(nvg, lassoBounds.getX(), lassoBounds.getY(), lassoBounds.getWidth(), lassoBounds.getHeight(), nvgColour(colours.objectSelectedOutlineColour.withAlpha(0.075f)), nvgColour(colours.canvasBackgroundColour.interpolatedWith(colours.objectSelectedOutlineColour, 0.65f)), 0.0f);
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
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgRect(nvg, vx + inset, vy + inset, vw - inset * 2.0f, vh - inset * 2.0f);
        nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(0, 230, 255, 22));
        nanovg::nvgStrokeWidth(nvg, 9.0f / zoom);
        nanovg::nvgStroke(nvg);

        nanovg::nvgBeginPath(nvg);
        nanovg::nvgRect(nvg, vx + inset, vy + inset, vw - inset * 2.0f, vh - inset * 2.0f);
        nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(0, 230, 255, 115));
        nanovg::nvgStrokeWidth(nvg, 1.5f / zoom);
        nanovg::nvgStroke(nvg);
    }

    nanovg::nvgRestore(nvg);

    // Draw scrollbars
    if (viewport) {
        viewport->render(nvg, viewport->getLocalArea(this, invalidRegion));
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
                mcpnvg::nvgFontSize(nvg, 14.5f);
                mcpnvg::nvgFontFace(nvg, "Inter");
                float bounds[4];
                mcpnvg::nvgTextBoxBounds(nvg, 0, 0, breakWidth, cleanDesc.toRawUTF8(), nullptr, bounds);
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
            NVGcolor accentCol = nanovg::nvgRGBA(56, 189, 248, 255);    // Electric Cyan default (THINKING / WORKING / BUILDING)
            NVGcolor badgeBg = nanovg::nvgRGBA(56, 189, 248, 30);
            NVGcolor badgeBorder = nanovg::nvgRGBA(56, 189, 248, 110);

            if (upperChapter.contains("DONE") || upperChapter.contains("APPROVED") || upperChapter.contains("VOICE")) {
                accentCol = nanovg::nvgRGBA(16, 185, 129, 255);          // Emerald Green
                badgeBg = nanovg::nvgRGBA(16, 185, 129, 32);
                badgeBorder = nanovg::nvgRGBA(16, 185, 129, 120);
            } else if (upperChapter.contains("APPROVE")) {
                accentCol = nanovg::nvgRGBA(245, 158, 11, 255);          // Amber Gold
                badgeBg = nanovg::nvgRGBA(245, 158, 11, 40);
                badgeBorder = nanovg::nvgRGBA(245, 158, 11, 140);
            } else if (upperChapter.contains("ERROR") || upperChapter.contains("DENIED") || upperChapter.contains("FAIL")) {
                accentCol = nanovg::nvgRGBA(244, 63, 94, 255);           // Coral Red
                badgeBg = nanovg::nvgRGBA(244, 63, 94, 40);
                badgeBorder = nanovg::nvgRGBA(244, 63, 94, 140);
            }

            // 1. Soft Ambient Box-Gradient Shadow
            NVGpaint shadowPaint = nanovg::nvgBoxGradient(nvg, cardX, cardY + 5.0f, cardW, cardH, 14.0f, 24.0f, nanovg::nvgRGBA(0, 0, 0, 160), nanovg::nvgRGBA(0, 0, 0, 0));
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRect(nvg, cardX - 20.0f, cardY - 12.0f, cardW + 40.0f, cardH + 44.0f);
            nanovg::nvgFillPaint(nvg, shadowPaint);
            nanovg::nvgFill(nvg);

            // 2. Obsidian Glass Body with subtle linear gradient
            NVGpaint bodyPaint = nanovg::nvgLinearGradient(nvg, cardX, cardY, cardX, cardY + cardH,
                                                   nanovg::nvgRGBA(18, 22, 32, 246),
                                                   nanovg::nvgRGBA(10, 12, 17, 252));
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, cardX, cardY, cardW, cardH, 13.0f);
            nanovg::nvgFillPaint(nvg, bodyPaint);
            nanovg::nvgFill(nvg);

            // 3. Subtle Frosted Border
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, cardX, cardY, cardW, cardH, 13.0f);
            nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(255, 255, 255, 30));
            nanovg::nvgStrokeWidth(nvg, 1.0f);
            nanovg::nvgStroke(nvg);

            // 4. Specular Top Highlight
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, cardX + 1.5f, cardY + 1.0f, cardW - 3.0f, 1.5f, 1.0f);
            nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 255, 255, 45));
            nanovg::nvgFill(nvg);

            // 5. Header Row (LED Capsule Badge + Tool Tag + Meta)
            float const headerY = cardY + 13.0f;
            float nextHeaderX = cardX + innerPadX;

            if (cardTitle.isNotEmpty()) {
                mcpnvg::nvgFontSize(nvg, 11.5f);
                mcpnvg::nvgFontFace(nvg, "Inter-Bold");
                float titleBounds[4];
                mcpnvg::nvgTextBounds(nvg, 0, 0, cardTitle.toRawUTF8(), nullptr, titleBounds);
                float const pillTextW = titleBounds[2] - titleBounds[0];
                float const pillH = 22.0f;
                float const pillW = pillTextW + 28.0f;

                // Pill Background
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgRoundedRect(nvg, nextHeaderX, headerY, pillW, pillH, 11.0f);
                nanovg::nvgFillColor(nvg, badgeBg);
                nanovg::nvgFill(nvg);
                nanovg::nvgStrokeColor(nvg, badgeBorder);
                nanovg::nvgStrokeWidth(nvg, 1.0f);
                nanovg::nvgStroke(nvg);

                // Glowing LED Dot
                float const ledX = nextHeaderX + 10.0f;
                float const ledY = headerY + pillH * 0.5f;
                NVGpaint ledGlow = nanovg::nvgRadialGradient(nvg, ledX, ledY, 1.5f, 6.5f, accentCol, nanovg::nvgRGBA(0, 0, 0, 0));
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgCircle(nvg, ledX, ledY, 6.5f);
                nanovg::nvgFillPaint(nvg, ledGlow);
                nanovg::nvgFill(nvg);

                nanovg::nvgBeginPath(nvg);
                nanovg::nvgCircle(nvg, ledX, ledY, 3.0f);
                nanovg::nvgFillColor(nvg, accentCol);
                nanovg::nvgFill(nvg);

                // Title Text inside Pill
                mcpnvg::nvgFontSize(nvg, 11.0f);
                mcpnvg::nvgFontFace(nvg, "Inter-Bold");
                mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(248, 250, 252, 255));
                mcpnvg::nvgText(nvg, nextHeaderX + 18.0f, headerY + pillH * 0.5f + 0.5f, cardTitle.toRawUTF8(), nullptr);

                nextHeaderX += pillW + 10.0f;
            }

            // Secondary Tool Tag
            if (hasTool && hud.tool != cardTitle) {
                mcpnvg::nvgFontSize(nvg, 12.0f);
                mcpnvg::nvgFontFace(nvg, "Inter");
                mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(148, 163, 184, 210));
                juce::String const toolStr = "// " + hud.tool;
                mcpnvg::nvgText(nvg, nextHeaderX, headerY + 11.0f, toolStr.toRawUTF8(), nullptr);
            }

            // Right-aligned Telemetry / Latency
            juce::String const metaRight = hud.latency.isNotEmpty() ? hud.latency : hud.telemetry;
            if (metaRight.isNotEmpty()) {
                mcpnvg::nvgFontSize(nvg, 11.5f);
                mcpnvg::nvgFontFace(nvg, "Inter-Tabular");
                mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(100, 116, 139, 230));
                mcpnvg::nvgText(nvg, cardX + cardW - innerPadX, headerY + 11.0f, metaRight.toRawUTF8(), nullptr);
            }

            // 6. Direct Description with Word Wrapping (nvgTextBox)
            if (cleanDesc.isNotEmpty()) {
                float const descY = cardTitle.isNotEmpty() ? (headerY + 22.0f + 8.0f) : (cardY + 13.0f);
                mcpnvg::nvgFontSize(nvg, 14.5f);
                mcpnvg::nvgFontFace(nvg, "Inter");
                mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(226, 232, 240, 245));
                mcpnvg::nvgTextBox(nvg, cardX + innerPadX, descY, breakWidth, cleanDesc.toRawUTF8(), nullptr);
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
                nanovg::nvgTranslate(nvg, b.getX(), b.getY());
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
    case hash("overlays"): {
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
    return false; // Canvas pen is disabled — canvas stays clean Pure Data
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
    if (editor) editor->nvgSurface.invalidateAll();
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
    if (editor) editor->nvgSurface.invalidateAll();
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
    if (editor) editor->nvgSurface.invalidateAll();
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
    mcpStudioMode = false;
    setSketchToolActive(false);
    updateSelectionPill();
    repaint();
    if (editor) editor->nvgSurface.invalidateAll();
}

void Canvas::setStudioInkVisible(bool visible)
{
    mcpStudioInkVisible = visible;
    repaint();
    if (editor) editor->nvgSurface.invalidateAll();
}

void Canvas::commitStudioFromCanvas()
{
    juce::String draft;
    if (editor) {
        if (auto* cp = editor->getCopilotPanel()) draft = cp->getStudioDraft();
    }
    sendSketchBuild(draft);
    if (editor) {
        if (auto* cp = editor->getCopilotPanel()) cp->clearStudioDraft();
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
            if (editor) editor->nvgSurface.invalidateAll();
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
            if (editor) editor->nvgSurface.invalidateAll();
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
    if (editor) editor->nvgSurface.invalidateAll();
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
    if (canvas->editor) canvas->editor->nvgSurface.invalidateAll();
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
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgMoveTo(nvg, stroke[0].x, stroke[0].y);
        for (size_t i = 1; i < stroke.size(); ++i) {
            nanovg::nvgLineTo(nvg, stroke[i].x, stroke[i].y);
        }
        nanovg::nvgLineCap(nvg, NVG_ROUND);
        nanovg::nvgLineJoin(nvg, NVG_ROUND);
        nanovg::nvgStrokeWidth(nvg, 7.0f);
        nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(0, 230, 255, static_cast<unsigned char>(alpha * 120.0f)));
        nanovg::nvgStroke(nvg);

        // 2. Cyan Body
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgMoveTo(nvg, stroke[0].x, stroke[0].y);
        for (size_t i = 1; i < stroke.size(); ++i) {
            nanovg::nvgLineTo(nvg, stroke[i].x, stroke[i].y);
        }
        nanovg::nvgLineCap(nvg, NVG_ROUND);
        nanovg::nvgLineJoin(nvg, NVG_ROUND);
        nanovg::nvgStrokeWidth(nvg, 3.5f);
        nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(0, 245, 255, static_cast<unsigned char>(alpha * 220.0f)));
        nanovg::nvgStroke(nvg);

        // 3. Hot White Center Core
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgMoveTo(nvg, stroke[0].x, stroke[0].y);
        for (size_t i = 1; i < stroke.size(); ++i) {
            nanovg::nvgLineTo(nvg, stroke[i].x, stroke[i].y);
        }
        nanovg::nvgLineCap(nvg, NVG_ROUND);
        nanovg::nvgLineJoin(nvg, NVG_ROUND);
        nanovg::nvgStrokeWidth(nvg, 1.5f);
        nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(255, 255, 255, static_cast<unsigned char>(alpha * 240.0f)));
        nanovg::nvgStroke(nvg);
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
    if (editor) {
        if (auto* cp = editor->getCopilotPanel()) {
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
    if (editor) editor->nvgSurface.invalidateAll();
}

void Canvas::undoLastStroke()
{
    if (mcpCompletedStrokes.empty()) return;
    mcpCompletedStrokes.pop_back();
    updateSelectionPill();
    repaint();
    if (editor) editor->nvgSurface.invalidateAll();
}

void Canvas::clearAllInk()
{
    mcpCompletedStrokes.clear();
    mcpCurrentStroke.clear();
    mcpSketchAlpha = 1.0f;
    if (mcpSketchFadeTimer) mcpSketchFadeTimer->stopTimer();
    updateSelectionPill();
    repaint();
    if (editor) editor->nvgSurface.invalidateAll();
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

        // Decode with JUCE and upload as ARGB, like PictureObject. The old
        // ::nvgCreateImage(filename) path is not allowed any more: with the async
        // recorder only the render thread may touch the real GPU context.
        auto& entry = mcpRefImageCache.try_emplace(img.id.toStdString()).first->second;
        if (!entry.texture.isValid() || entry.ctx != nvg) {
            auto const decoded = juce::ImageFileFormat::loadFrom(juce::File(img.filePath));
            if (decoded.isValid())
                entry.texture.loadJUCEImage(nvg, decoded.convertedToFormat(juce::Image::ARGB));
            entry.ctx = nvg;
        }
        if (!entry.texture.isValid()) continue;
        int const imageHandle = entry.texture.getImageId();

        Rectangle<float> const b(static_cast<float>(canvasOrigin.x) + img.x,
                                 static_cast<float>(canvasOrigin.y) + img.y,
                                 img.width, img.height);
        if (!invalidRegion.intersects(b.getSmallestIntegerContainer())) continue;

        NVGScopedState scopedImage(nvg);

        // Light paper matte behind the image — dark screenshots on a dark canvas
        // would otherwise blend into the background and look "missing".
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgRoundedRect(nvg, b.getX() - 6.0f, b.getY() - 6.0f, b.getWidth() + 12.0f, b.getHeight() + 12.0f, 9.0f);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(228, 230, 236, 240));
        nanovg::nvgFill(nvg);

        // The image itself — straight through nvgImagePattern.
        {
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, b.getX(), b.getY(), b.getWidth(), b.getHeight(), 4.0f);
            nanovg::nvgFillPaint(nvg, nanovg::nvgImagePattern(nvg, b.getX(), b.getY(), b.getWidth(), b.getHeight(), 0.0f, imageHandle, img.alpha));
            nanovg::nvgFill(nvg);
        }

        // Hardware frame: cool slate normally, warm amber when locked
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgRoundedRect(nvg, b.getX() - 1.0f, b.getY() - 1.0f, b.getWidth() + 2.0f, b.getHeight() + 2.0f, 5.0f);
        nanovg::nvgStrokeColor(nvg, img.isLocked ? nanovg::nvgRGBA(255, 190, 80, 210) : nanovg::nvgRGBA(150, 165, 190, 140));
        nanovg::nvgStrokeWidth(nvg, img.isLocked ? 1.6f : 1.2f);
        nanovg::nvgStroke(nvg);

        // Title tag above the image (+ lock glyph)
        juce::String const tag = img.title.isNotEmpty() ? img.title : juce::File(img.filePath).getFileName();
        mcpnvg::nvgFontFace(nvg, "Inter");
        mcpnvg::nvgFontSize(nvg, 10.0f);
        mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        float textBounds[4];
        mcpnvg::nvgTextBounds(nvg, 0.0f, 0.0f, tag.toRawUTF8(), nullptr, textBounds);
        float const tagW = (textBounds[2] - textBounds[0]) + (img.isLocked ? 34.0f : 18.0f);
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgRoundedRect(nvg, b.getX(), b.getY() - 20.0f, tagW, 16.0f, 3.5f);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(16, 18, 22, 225));
        nanovg::nvgFill(nvg);
        nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(120, 135, 160, 120));
        nanovg::nvgStrokeWidth(nvg, 1.0f);
        nanovg::nvgStroke(nvg);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(205, 216, 232, 235));
        mcpnvg::nvgText(nvg, b.getX() + 8.0f, b.getY() - 12.0f, tag.toRawUTF8(), nullptr);

        if (img.isLocked) {
            float const lx = b.getX() + tagW - 12.0f;
            float const ly = b.getY() - 11.5f;
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, lx - 3.5f, ly - 0.5f, 7.0f, 5.5f, 1.0f);
            nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 190, 80, 235));
            nanovg::nvgFill(nvg);
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgArc(nvg, lx, ly - 2.0f, 2.6f, juce::MathConstants<float>::pi, 0.0f, NVG_HOLE);
            nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(255, 190, 80, 235));
            nanovg::nvgStrokeWidth(nvg, 1.2f);
            nanovg::nvgStroke(nvg);
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
            if (editor) editor->nvgSurface.invalidateAll();
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
    if (editor) editor->nvgSurface.invalidateAll();
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
        NVGcolor const accent = melody ? nanovg::nvgRGBA(255, 190, 60, playing ? 255 : 210)
                                       : nanovg::nvgRGBA(80, 220, 140, playing ? 255 : 210);

        NVGScopedState scopedChip(nvg);

        // Cassette body: deep slate + per-type accent border (amber = melody, green = beat)
        nanovg::nvgDrawRoundedRect(nvg, b.getX(), b.getY(), b.getWidth(), b.getHeight(),
                           nanovg::nvgRGBA(18, 22, 28, 242), accent, 6.0f);

        // Type glyph: beamed notes (melody) or drum (beat) — the strip's language
        {
            float const gx = b.getX() + 18.0f;
            float const gy = b.getCentreY();
            nanovg::nvgStrokeColor(nvg, accent);
            nanovg::nvgFillColor(nvg, accent);
            if (melody) {
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgEllipse(nvg, gx - 3.0f, gy + 3.5f, 2.4f, 1.7f);
                nanovg::nvgFill(nvg);
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgEllipse(nvg, gx + 3.2f, gy + 2.2f, 2.4f, 1.7f);
                nanovg::nvgFill(nvg);
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgMoveTo(nvg, gx - 0.7f, gy + 3.4f);
                nanovg::nvgLineTo(nvg, gx - 0.7f, gy - 4.5f);
                nanovg::nvgLineTo(nvg, gx + 5.6f, gy - 5.7f);
                nanovg::nvgLineTo(nvg, gx + 5.6f, gy + 2.1f);
                nanovg::nvgStrokeWidth(nvg, 1.3f);
                nanovg::nvgStroke(nvg);
            } else {
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgRoundedRect(nvg, gx - 5.0f, gy - 0.5f, 10.0f, 5.5f, 1.7f);
                nanovg::nvgStrokeWidth(nvg, 1.1f);
                nanovg::nvgStroke(nvg);
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgEllipse(nvg, gx, gy - 0.5f, 5.0f, 1.7f);
                nanovg::nvgStrokeWidth(nvg, 1.1f);
                nanovg::nvgStroke(nvg);
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgMoveTo(nvg, gx - 3.8f, gy - 6.0f);
                nanovg::nvgLineTo(nvg, gx + 0.8f, gy - 1.2f);
                nanovg::nvgMoveTo(nvg, gx + 3.8f, gy - 6.0f);
                nanovg::nvgLineTo(nvg, gx - 0.8f, gy - 1.2f);
                nanovg::nvgStrokeWidth(nvg, 1.0f);
                nanovg::nvgStroke(nvg);
            }
        }

        // ▶ / ⏸ (playing) — pause bars while the take is auditioning
        float const pcx = b.getX() + 46.0f;
        float const pcy = b.getCentreY();
        if (playing) {
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, pcx - 4.5f, pcy - 5.0f, 3.4f, 10.0f, 1.2f);
            nanovg::nvgFillColor(nvg, accent);
            nanovg::nvgFill(nvg);
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, pcx + 1.1f, pcy - 5.0f, 3.4f, 10.0f, 1.2f);
            nanovg::nvgFillColor(nvg, accent);
            nanovg::nvgFill(nvg);
        } else {
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgMoveTo(nvg, pcx - 4.5f, pcy - 6.5f);
            nanovg::nvgLineTo(nvg, pcx + 6.5f, pcy);
            nanovg::nvgLineTo(nvg, pcx - 4.5f, pcy + 6.5f);
            nanovg::nvgClosePath(nvg);
            nanovg::nvgFillColor(nvg, accent);
            nanovg::nvgFill(nvg);
        }

        // Mini-waveform (64 peak buckets)
        float const wfX = b.getX() + 62.0f;
        float const wfW = b.getWidth() - 62.0f - 72.0f;
        float const wfCy = pcy;
        constexpr float wfMaxH = 22.0f;
        if (take.peaks.size() >= 2) {
            float const barW = wfW / static_cast<float>(take.peaks.size());
            nanovg::nvgBeginPath(nvg);
            for (size_t k = 0; k < take.peaks.size(); ++k) {
                float const h = juce::jmax(1.5f, juce::jlimit(0.0f, 1.0f, take.peaks[k]) * wfMaxH);
                nanovg::nvgRect(nvg, wfX + static_cast<float>(k) * barW, wfCy - h * 0.5f, juce::jmax(0.8f, barW - 0.4f), h);
            }
            nanovg::nvgFillColor(nvg, accent);
            nanovg::nvgFill(nvg);
        } else {
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgMoveTo(nvg, wfX, wfCy);
            nanovg::nvgLineTo(nvg, wfX + wfW, wfCy);
            nanovg::nvgStrokeColor(nvg, accent);
            nanovg::nvgStrokeWidth(nvg, 1.0f);
            nanovg::nvgStroke(nvg);
        }

        // Duration m:ss
        int const totalSec = juce::roundToInt(take.durationSec);
        juce::String const dur = juce::String(totalSec / 60) + ":" + juce::String(totalSec % 60).paddedLeft('0', 2);
        mcpnvg::nvgFontFace(nvg, "Inter");
        mcpnvg::nvgFontSize(nvg, 10.0f);
        mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(230, 220, 200, 220));
        mcpnvg::nvgText(nvg, b.getX() + b.getWidth() - 66.0f, pcy, dur.toRawUTF8(), nullptr);

        // ✕ remove
        float const xx = b.getRight() - 14.0f;
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgMoveTo(nvg, xx - 4.0f, pcy - 4.0f);
        nanovg::nvgLineTo(nvg, xx + 4.0f, pcy + 4.0f);
        nanovg::nvgMoveTo(nvg, xx + 4.0f, pcy - 4.0f);
        nanovg::nvgLineTo(nvg, xx - 4.0f, pcy + 4.0f);
        nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(220, 150, 150, 230));
        nanovg::nvgStrokeWidth(nvg, 1.4f);
        nanovg::nvgStroke(nvg);
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
            if (editor) editor->nvgSurface.invalidateAll();
            return true;
        }
        if (relX >= b.getWidth() - 30) {
            // ✕ remove (stop audition first if it's this one)
            if (br && br->getPlayingVoiceTakeId() == take.id) br->stopVoiceTake();
            pd->removeMcpVoiceTake(take.id);
            repaint();
            if (editor) editor->nvgSurface.invalidateAll();
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

void Canvas::renderMidiTakes(NVGcontext* nvg, Rectangle<int> invalidRegion)
{
    if (!pd || presentationMode.getValue()) return;
    auto takes = pd->getMcpMidiTakes();
    if (takes.empty()) return;

    auto* br = pd->getMCPBridge();
    juce::String const playingId = br ? br->getPlayingMidiTakeId() : juce::String();

    constexpr float chipW = 240.0f;
    constexpr float chipH = 38.0f;
    int slot = 0;

    for (auto& take : takes) {
        if (!take.visible) continue;

        if (!take.positioned) {
            float const px = mcpTakeSpawnPos.x != 0 ? static_cast<float>(mcpTakeSpawnPos.x)
                                                    : static_cast<float>(canvasOrigin.x + 40);
            float const py = mcpTakeSpawnPos.y != 0 ? static_cast<float>(mcpTakeSpawnPos.y)
                                                    : static_cast<float>(canvasOrigin.y + 40);
            pd->setMcpMidiTakePos(take.id, px - static_cast<float>(canvasOrigin.x),
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

        bool const playing = (playingId == take.id);
        NVGcolor const accent = nanovg::nvgRGBA(180, 90, 255, playing ? 255 : 220); // synth royal purple (bright glow while playing)

        NVGScopedState scopedChip(nvg);

        // Cassette body: deep slate + royal purple border
        nanovg::nvgDrawRoundedRect(nvg, b.getX(), b.getY(), b.getWidth(), b.getHeight(),
                           nanovg::nvgRGBA(20, 18, 28, 245), accent, 6.0f);

        // 🎹 Piano keyboard glyph (at x = 16)
        {
            float const gx = b.getX() + 16.0f;
            float const gy = b.getCentreY();
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, gx - 7.0f, gy - 6.0f, 14.0f, 12.0f, 1.5f);
            nanovg::nvgStrokeColor(nvg, accent);
            nanovg::nvgStrokeWidth(nvg, 1.2f);
            nanovg::nvgStroke(nvg);
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgMoveTo(nvg, gx - 2.5f, gy - 6.0f);
            nanovg::nvgLineTo(nvg, gx - 2.5f, gy + 6.0f);
            nanovg::nvgMoveTo(nvg, gx + 2.0f, gy - 6.0f);
            nanovg::nvgLineTo(nvg, gx + 2.0f, gy + 6.0f);
            nanovg::nvgStrokeColor(nvg, accent);
            nanovg::nvgStrokeWidth(nvg, 1.0f);
            nanovg::nvgStroke(nvg);
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRect(nvg, gx - 4.5f, gy - 6.0f, 1.8f, 6.5f);
            nanovg::nvgRect(nvg, gx + 0.2f, gy - 6.0f, 1.8f, 6.5f);
            nanovg::nvgFillColor(nvg, accent);
            nanovg::nvgFill(nvg);
        }

        // ▶ / ⏸ Audition toggle button (at x = 38)
        float const pcx = b.getX() + 38.0f;
        float const pcy = b.getCentreY();
        if (playing) {
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, pcx - 4.5f, pcy - 5.0f, 3.4f, 10.0f, 1.2f);
            nanovg::nvgFillColor(nvg, accent);
            nanovg::nvgFill(nvg);
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, pcx + 1.1f, pcy - 5.0f, 3.4f, 10.0f, 1.2f);
            nanovg::nvgFillColor(nvg, accent);
            nanovg::nvgFill(nvg);
        } else {
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgMoveTo(nvg, pcx - 4.5f, pcy - 6.0f);
            nanovg::nvgLineTo(nvg, pcx + 6.0f, pcy);
            nanovg::nvgLineTo(nvg, pcx - 4.5f, pcy + 6.0f);
            nanovg::nvgClosePath(nvg);
            nanovg::nvgFillColor(nvg, accent);
            nanovg::nvgFill(nvg);
        }

        // Title and notes (at x = 54)
        float const tx = b.getX() + 54.0f;
        float const ty = b.getY() + 14.0f;
        mcpnvg::nvgFontFace(nvg, "Inter-Medium");
        mcpnvg::nvgFontSize(nvg, 11.5f);
        mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(235, 225, 250, 240));
        juce::String const title = take.id + " · " + juce::String(take.noteCount) + " notes";
        mcpnvg::nvgText(nvg, tx, ty, title.toRawUTF8(), nullptr);

        // Subtitle: voice binding + duration (at x = 54)
        float const subY = b.getY() + 27.0f;
        mcpnvg::nvgFontFace(nvg, "Inter");
        mcpnvg::nvgFontSize(nvg, 9.5f);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(180, 165, 205, 200));
        juce::String const sub = "voice: " + take.voice + " · " + juce::String(take.durationSec, 1) + "s";
        mcpnvg::nvgText(nvg, tx, subY, sub.toRawUTF8(), nullptr);

        // ✕ Remove button on the far right
        float const xx = b.getRight() - 14.0f;
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgMoveTo(nvg, xx - 4.0f, pcy - 4.0f);
        nanovg::nvgLineTo(nvg, xx + 4.0f, pcy + 4.0f);
        nanovg::nvgMoveTo(nvg, xx + 4.0f, pcy - 4.0f);
        nanovg::nvgLineTo(nvg, xx - 4.0f, pcy + 4.0f);
        nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(200, 160, 220, 210));
        nanovg::nvgStrokeWidth(nvg, 1.3f);
        nanovg::nvgStroke(nvg);
    }
}

bool Canvas::handleMidiTakeClick(MouseEvent const& e, Point<int> mousePos)
{
    if (!pd) return false;
    auto const takes = pd->getMcpMidiTakes();
    for (auto it = takes.rbegin(); it != takes.rend(); ++it) {
        auto const& take = *it;
        if (!take.visible) continue;
        Rectangle<int> const b(static_cast<int>(canvasOrigin.x + take.x), static_cast<int>(canvasOrigin.y + take.y), 240, 38);
        if (!b.contains(mousePos)) continue;

        int const relX = mousePos.x - b.getX();
        auto* br = pd->getMCPBridge();

        if (relX >= 26 && relX <= 50) {
            // ▶ / ⏸ audition toggle
            if (br) br->playMidiTake(take.id, take.json);
            repaint();
            if (editor) editor->nvgSurface.invalidateAll();
            return true;
        }

        if (relX >= b.getWidth() - 30) {
            // ✕ remove (stop playback first if it's currently playing)
            if (br && br->getPlayingMidiTakeId() == take.id) br->stopMidiTake();
            pd->removeMcpMidiTake(take.id);
            repaint();
            if (editor) editor->nvgSurface.invalidateAll();
            return true;
        }

        // Body: drag the chip
        mcpDraggingMidiTakeId = take.id;
        mcpMidiTakeDragOffset = mousePos - b.getPosition();
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
                    if (canvas->editor) {
                        if (auto* cp = canvas->editor->getCopilotPanel()) cp->receiveMessage("user", prompt);
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
                    if (canvas->editor) canvas->editor->nvgSurface.invalidateAll();
                } else if (result == 3) {
                    canvas->pd->removeMcpReferenceImage(imageId);
                    canvas->mcpRefImageCache.erase(imageId.toStdString());
                    canvas->repaint();
                    if (canvas->editor) canvas->editor->nvgSurface.invalidateAll();
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
    if (editor) editor->nvgSurface.invalidateAll();
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

    auto const overlaysTree = SettingsFile::getInstance()->getProperty<DynamicObject>("overlays");

    auto const altModeEnabled = overlaysTree->getProperty("alt_mode") && !isGraph;

    if (!locked.getValue()) {
        overlayState = overlaysTree->getProperty("edit");
    }
    if (locked.getValue() || commandLocked.getValue()) {
        overlayState = overlaysTree->getProperty("lock");
    }
    if (altModeEnabled) {
        overlayState = overlaysTree->getProperty("alt");
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
        viewport->setViewPositionAnimated((canvasOrigin.toFloat() + Point<float>(1, 1)) * getValue<float>(zoomScale));
}

void Canvas::restoreViewportState()
{
    if (viewport) {
        zoomScale.setValue(patch.lastViewportScale);
        viewport->setViewPosition((patch.lastViewportPosition + canvasOrigin.toFloat()) * patch.lastViewportScale);
    }
}

void Canvas::saveViewportState()
{
    if (viewport) {
        patch.lastViewportScale = getValue<float>(zoomScale);
        patch.lastViewportPosition = ((viewport->getViewPosition() / patch.lastViewportScale) - canvasOrigin.toFloat());
    }
}

void Canvas::zoomToFitAll()
{
    if (objects.empty() || !viewport)
        return;

    auto regionOfInterest = Rectangle<float>(canvasOrigin.x, canvasOrigin.y, 20, 20);
    if (!presentationMode.getValue()) {
        for (auto const* object : objects) {
            regionOfInterest = regionOfInterest.getUnion(object->getBounds().toFloat());
        }
    }
    regionOfInterest = regionOfInterest.expanded(24); // Nice margin

    auto const viewportW = static_cast<float>(viewport->getWidth());
    auto const viewportH = static_cast<float>(viewport->getHeight());

    auto const scaleX = viewportW / regionOfInterest.getWidth();
    auto const scaleY = viewportH / regionOfInterest.getHeight();

    auto targetScale = std::clamp(jmin(scaleX, scaleY), 0.25f, 3.0f);

    // zoomScale = targetScale;

    auto const roiCentreScaled = regionOfInterest.getCentre() * targetScale;
    auto const screenCentre = Point<float>(viewportW * 0.5f, viewportH * 0.5f);

    auto const targetViewPos = roiCentreScaled - screenCentre;
    viewport->setViewPositionAnimated(targetViewPos, targetScale);
}

void Canvas::tabChanged()
{
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

pd::Patch::Ptr Canvas::getPatchToSave() const
{
    // Subpatches get saved as part of the patch that contains them, which may be open in another window
    if (patch.isSubpatch()) {
        auto const* root = patch.getRoot();
        for (auto const& openedPatch : pd->patches) {
            if (openedPatch->getRawPointer() == root)
                return openedPatch;
        }
    }

    return refCountedPatch;
}

void Canvas::save(std::function<void()> const& nestedCallback)
{
    auto const patchToSave = getPatchToSave();
    if (patchToSave->getCurrentFile().existsAsFile()) {
        patchToSave->savePatch();
        SettingsFile::getInstance()->addToRecentlyOpened(patchToSave->getCurrentURL());
        pd->titleChanged();
        nestedCallback();
    } else {
        saveAs(nestedCallback);
    }
}

void Canvas::saveAs(std::function<void()> const& nestedCallback)
{
    // The dialog stays attached to this canvas, even when saving a parent patch:
    // on iOS, the file picker is shown inside it, and the parent patch's tab might not be visible
    auto const patchToSave = getPatchToSave();
    Dialogs::showSaveDialog([this, patchToSave, nestedCallback](URL const& resultURL) mutable {
        auto result = resultURL.getLocalFile();
        if (result.getFullPathName().isNotEmpty()) {
            if (result.exists())
                result.deleteFile();

            patchToSave->savePatch(resultURL);
            SettingsFile::getInstance()->addToRecentlyOpened(patchToSave->getCurrentURL());
            pd->titleChanged();
        }

        nestedCallback();
    },
        "*.pd", "Patch", this, false, patchToSave->getPatchFile().getFileNameWithoutExtension());
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
                object->gui->updateProperties();
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
        viewport->updateCanvasTransform();
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
        SettingsFile::getInstance()->getProperty<DynamicObject>("overlays")->setProperty("alt_mode", altDown && shiftDown);
        SettingsFile::getInstance()->triggerSettingsChange("overlays");
    }

    if (!isHeld)
        return;

    if (connectionsBeingCreated.size() == 1) {
        Iolet* connectingOutlet = connectionsBeingCreated[0]->getIolet();
        Iolet* targetInlet = nullptr;
        for (auto const& object : objects) {
            for (auto const& iolet : object->iolets) {
                if (iolet->isTargeted() && iolet != connectingOutlet) {
                    targetInlet = iolet;
                    break;
                }
            }
        }

        if (targetInlet) {
            bool const inverted = connectingOutlet->isInlet();
            if (inverted)
                std::swap(connectingOutlet, targetInlet);

            if (auto x = patch.getPointer()) {
                auto* outObj = connectingOutlet->getObject()->getPointer();
                auto* inObj = targetInlet->getObject()->getPointer();
                auto const outletIndex = connectingOutlet->getIndex();
                auto const inletIndex = targetInlet->getIndex();

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
        SettingsFile::getInstance()->getProperty<DynamicObject>("overlays")->setProperty("alt_mode", altDown && shiftDown);
        SettingsFile::getInstance()->triggerSettingsChange("overlays");
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
        if (editor) editor->nvgSurface.invalidateAll();
    }
}

void Canvas::mouseExit(MouseEvent const& e)
{
    if (mcpNoteHover != -1 || mcpNoteHoverClose || mcpNoteHoverChip != -1) {
        mcpNoteHover = -1;
        mcpNoteHoverClose = false;
        mcpNoteHoverChip = -1;
        repaint();
        if (editor) editor->nvgSurface.invalidateAll();
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
                    if (editor) editor->nvgSurface.invalidateAll();
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

            // Mood strip: [✏] [T] [🖼] [🎙 Melody] [🥁 Beat] [🎹 MIDI] — icons first, surfaces second.
            if (mcpPillSketchMode && mcpCompletedStrokes.empty()) {
                if (relX >= 6 && relX < 210) {
                    int const idx = (relX - 6) / 34;
                    switch (idx) {
                    case 0:
                        setSketchToolActive(!isSketchToolActive());
                        repaint();
                        if (editor) editor->nvgSurface.invalidateAll();
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
                        if (editor) editor->nvgSurface.invalidateAll();
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
                                if (editor) editor->nvgSurface.invalidateAll();
                            }
                        }
                        return true;
                    case 4:
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
                                if (editor) editor->nvgSurface.invalidateAll();
                            }
                        }
                        return true;
                    case 5:
                        // MIDI record: declared intent — MIDI keyboard (exact notes).
                        if (pd) {
                            if (auto* br = pd->getMCPBridge()) {
                                if (br->isMidiCapturing()) {
                                    br->stopMidiCaptureAndStage();
                                } else {
                                    setSketchToolActive(false);
                                    setEraserToolActive(false);
                                    setTextToolActive(false);
                                    mcpTakeSpawnPos = mcpMoodPillActive ? mcpMoodPillPos : juce::Point<int>(panel.getCentreX(), panel.getBottom() + 170);
                                    br->startMidiCapture(120, "canvas");
                                }
                                repaint();
                                if (editor) editor->nvgSurface.invalidateAll();
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
                    if (editor) editor->nvgSurface.invalidateAll();
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



            // Right ~32px: ↵ Submit button
            if (relX >= panel.getWidth() - 32) {
                submitSelectionPill(false);
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
            if (editor) editor->nvgSurface.invalidateAll();
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
                        if (editor) editor->nvgSurface.invalidateAll();
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
                                    if (editor) {
                                        if (auto* cp = editor->getCopilotPanel())
                                            cp->receiveMessage("user", replyTxt);
                                    }
                                    if (auto* br = pd->getMCPBridge())
                                        br->sendArtistNote(targetId, replyTxt);
                                }
                            }
                            mcpNoteEditIndex = -1;
                            if (mcpNoteEditor) mcpNoteEditor->setVisible(false);
                            repaint();
                            if (editor) editor->nvgSurface.invalidateAll();
                        };
                        mcpNoteEditor->onEscapeKey = [this] {
                            mcpNoteEditIndex = -1;
                            if (mcpNoteEditor) mcpNoteEditor->setVisible(false);
                            repaint();
                            if (editor) editor->nvgSurface.invalidateAll();
                        };
                        mcpNoteEditor->onFocusLost = [this] {
                            if (juce::Time::getMillisecondCounter() - mcpNoteOpenedAt < 500) return;
                            mcpNoteEditIndex = -1;
                            if (mcpNoteEditor) mcpNoteEditor->setVisible(false);
                            repaint();
                            if (editor) editor->nvgSurface.invalidateAll();
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
                        if (editor) editor->nvgSurface.invalidateAll();
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
                    if (editor) {
                        if (auto* cp = editor->getCopilotPanel())
                            cp->receiveMessage("user", chip.prompt);
                    }
                    mcpNoteHover = -1;
                    mcpNoteHoverChip = -1;
                    repaint();
                    if (editor) editor->nvgSurface.invalidateAll();
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
        if (auto* copilotSidebar = editor ? editor->getSidebarForPanel(Sidebar::CopilotChatPanel) : nullptr) {
            copilotSidebar->showSidebar(true);
            copilotSidebar->showPanel(Sidebar::CopilotChatPanel);
            if (auto* cp = editor->getCopilotPanel()) {
                cp->showAnnotationNote(a.text, a.kind, a.targetIds);
            }
        }
        repaint();
        if (editor) editor->nvgSurface.invalidateAll();
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
            if (editor) editor->nvgSurface.invalidateAll();
        }
        return;
    }

    // The pill/chisel and its chips float ABOVE everything: their clicks must beat
    // the pen, the reference images and the lasso. Without this, an armed pen
    // swallows [↩ Undo] / [🗑 Clear] clicks as new strokes.
    if (source == this && !e.mods.isRightButtonDown() && handleNoteClick(e.getPosition())) return;

    // Voice take chips float above the glass: ▶ audition / ✕ / drag.
    if (source == this && !e.mods.isRightButtonDown() && handleVoiceTakeClick(e, e.getEventRelativeTo(this).getPosition())) return;
    if (source == this && !e.mods.isRightButtonDown() && handleMidiTakeClick(e, e.getEventRelativeTo(this).getPosition())) return;

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
            if (editor) editor->nvgSurface.invalidateAll();
            return;
        }

        if (source == this) {
            // Empty glass click: let native PlugData handle deselect and lasso cleanly.
            // The Mood Strip only appears when Studio Mode is explicitly active.
            mcpMoodPillActive = false;
            mcpPillRequested = false;
            hideSelectionPill();

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
                                    if (editor) editor->nvgSurface.invalidateAll();
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
            if (editor) editor->nvgSurface.invalidateAll();
            return;
        }
        Dialogs::showCanvasRightClickMenu(this, source, e.getScreenPosition());
    }
}

bool Canvas::hitTest(int const x, int const y)
{
    // allow panning to happen anywhere, even when in presentation mode
    if (panningModifierDown() || (viewport && viewport->isPerformingGesture()))
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
        if (editor) editor->nvgSurface.invalidateAll();
        return;
    }

    // MIDI take chip drag.
    if (mcpDraggingMidiTakeId.isNotEmpty()) {
        auto const pt = e.getEventRelativeTo(this).getPosition();
        auto const pdPos = pt - mcpMidiTakeDragOffset - canvasOrigin;
        if (pd) pd->setMcpMidiTakePos(mcpDraggingMidiTakeId, static_cast<float>(pdPos.x), static_cast<float>(pdPos.y));
        repaint();
        if (editor) editor->nvgSurface.invalidateAll();
        return;
    }

    // PRD Phase 2: dragging a reference image across the canvas glass.
    if (mcpDraggingRefImageId.isNotEmpty()) {
        auto const pt = e.getEventRelativeTo(this).getPosition();
        auto const pdPos = pt - mcpRefImageDragOffset - canvasOrigin;
        if (pd) pd->moveMcpReferenceImage(mcpDraggingRefImageId, static_cast<float>(pdPos.x), static_cast<float>(pdPos.y));
        repaint();
        if (editor) editor->nvgSurface.invalidateAll();
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
                if (editor) editor->nvgSurface.invalidateAll();
            }
            return;
        }

        mcpCurrentStroke.push_back({static_cast<float>(pt.x), static_cast<float>(pt.y), juce::Time::getMillisecondCounterHiRes()});
        repaint();
        if (editor) editor->nvgSurface.invalidateAll();
        return;
    }

    if (canvasRateReducer.tooFast())
        return;

    if (panningModifierDown() || (viewport && viewport->isPerformingGesture())) {
        if (isDraggingLasso) {
            lasso.endLasso();
            isDraggingLasso = false;
        }
        return;
    }

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
        viewport->setViewPosition(Point<float>(x, y));
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
        if (editor) editor->nvgSurface.invalidateAll();
        return;
    }

    // Finish a MIDI take chip drag.
    if (mcpDraggingMidiTakeId.isNotEmpty()) {
        mcpDraggingMidiTakeId.clear();
        setMouseCursor(juce::MouseCursor::NormalCursor);
        repaint();
        if (editor) editor->nvgSurface.invalidateAll();
        return;
    }

    // PRD Phase 2: finish a reference-image drag.
    if (mcpDraggingRefImageId.isNotEmpty()) {
        mcpDraggingRefImageId.clear();
        setMouseCursor(juce::MouseCursor::NormalCursor);
        repaint();
        if (editor) editor->nvgSurface.invalidateAll();
        return;
    }

    if (isSketching) {
        isSketching = false;
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
            // Empty canvas tap: zero popups, keep canvas 100% clean
            mcpMoodPillActive = false;
        }
        // Ink persists -> anchor the Moodboard Chisel under the fresh bbox.
        updateSelectionPill();
        repaint();
        if (editor) editor->nvgSurface.invalidateAll();
        return;
    }

    setPanDragMode(false);
    setMouseCursor(MouseCursor::NormalCursor);

    connectionCancelled = false;

    // Double-click canvas to create new object (ignore clicks on AI notes)
    if (!isPointOverNote(e.getPosition()) && e.mods.isLeftButtonDown() && e.getNumberOfClicks() == 2 && e.originalComponent == this && !isGraph && !getValue<bool>(locked)) {
        // IMMUNITY: Kill any open mood strip and suppress AI pills during object creation
        mcpMoodPillActive = false;
        mcpPillRequested = false;
        hideSelectionPill();
        mcpSelectionPillHideUntil = juce::Time::getMillisecondCounter() + 1500;

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

        if (auto* s = editor->getSidebarForPanel(Sidebar::InspectorPanel))
            s->showParameters(toShow, allParameters, showOnSelect);

        if (auto* s = editor->getSidebarForPanel(Sidebar::ObjectReferencePanel))
            s->updateReference(lassoSelection.size() == 1 ? lassoSelection[0]->getType().toString() : String());

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
        if (editor) editor->nvgSurface.invalidateAll();
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
    // Only hijack hotkeys if the pen/eraser is ACTIVELY in hand or Studio mode is explicitly ON.
    // In regular patching, 't' must always create a native Toggle box!
    bool const moodboardArmed = mcpSketchToolActive || mcpEraserToolActive || mcpTextToolActive || mcpStudioMode;
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

    // Summon the AI pill prompt on selected object(s) when Spacebar (Leertaste) or "/" is pressed:
    if (key.getKeyCode() == juce::KeyPress::spaceKey || key.getTextCharacter() == '/' || key.getKeyCode() == '/') {
        if (!getSelectionOfType<Object>().empty() && !ObjectBase::isBeingEdited()) {
            mcpPillRequested = true;
            mcpPillDrawerMode = 0;
            mcpPaletteFrame = {};
            updateSelectionPill();
            if (mcpSelectionPill) {
                mcpSelectionPill->setText(juce::String(), false);
                mcpSelectionPill->grabKeyboardFocus();
                // If triggered by slash, open the command/skills palette directly
                if (key.getTextCharacter() == '/' || key.getKeyCode() == '/') {
                    showPillToolsMenu();
                }
            }
            return true;
        }
    }

    // Fast-focus / hotkey handling when the selection pill is visible:
    if (mcpSelectionPill && mcpSelectionPill->isVisible()) {
        if (key.getKeyCode() == juce::KeyPress::tabKey || key.getKeyCode() == juce::KeyPress::returnKey) {
            // Mood strip has no prompt field — focus keys do nothing there.
            if (mcpPillSketchMode && mcpCompletedStrokes.empty()) return true;
            mcpSelectionPill->grabKeyboardFocus();
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::escapeKey) {
            mcpPillRequested = false;
            dismissSelectionPill();
            return true;
        }
    }
    pd->keyHandler.sendKeyPress(key); // Send keypress for listers to #key, #keyup and #keyname

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
        viewport->setViewPosition(Point<float>(viewX * scale, viewY * scale));
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
    if (auto* s = editor->getSidebarForPanel(Sidebar::InspectorPanel))
        s->hideParameters();
    // Fall back to the sketch pill when ink exists; otherwise this hides it.
    updateSelectionPill();

    if (!broadcastChange) {
        MessageManager::callAsync([_this = SafePointer(this)] {
            if (_this)
                _this->selectedComponents.addChangeListener(_this);
        });
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
    // IMMUNITY: Never show the selection pill while ANY object or comment text is being edited!
    // Native Pure Data object naming + autocomplete suggestor must have 100% exclusive priority.
    if (ObjectBase::isBeingEdited()) {
        mcpPillSketchMode = false;
        mcpPillRequested = false;
        hideSelectionPill();
        return;
    }

    if (isGraph || getValue<bool>(locked) || presentationMode.getValue()) {
        mcpPillSketchMode = false;
        mcpPillRequested = false;
        hideSelectionPill();
        return;
    }

    auto selectedObjects = getSelectionOfType<Object>();
    // The chisel owns the canvas while ink exists, the pen/eraser is up, or a
    // fresh mood palette was opened; otherwise hide.
    if (selectedObjects.empty() && mcpCompletedStrokes.empty() && !mcpSketchToolActive && !mcpEraserToolActive && !mcpMoodPillActive) {
        mcpPillSketchMode = false;
        mcpPillRequested = false;
        hideSelectionPill();
        return;
    }

    // Object mode: only show the pill if the artist explicitly requested it (e.g. hit "/"),
    // or if Studio mode is active, or if ink was sketched over the selection.
    // This stops the 480px pill from obstructing cords during everyday box dragging!
    if (!selectedObjects.empty() && !mcpPillRequested && !mcpStudioMode && mcpCompletedStrokes.empty()) {
        hideSelectionPill();
        return;
    }

    // Brief grace window after a send/dismiss so an immediate sidebar/selection
    // refresh can't re-pop the pill. Time-based (not keyed to the selection), so
    // re-selecting the SAME object later always brings it back.
    if (juce::Time::getMillisecondCounter() < mcpSelectionPillHideUntil) return;

    // Keep canvas 100% clean — never pop up floating chisel bar on empty canvas!
    if (selectedObjects.empty()) {
        mcpPillSketchMode = false;
        hideSelectionPill();
        return;
    }
    bool const sketchMode = false;
    mcpPillSketchMode = false;

    int pillH = 32;
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
        if (mcpCompletedStrokes.empty()) {
            mcpPillSketchMode = false;
            hideSelectionPill();
            return;
        }
        sketchStrip = false;
        pillW = 520;
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

        pillW = 380;
        pillH = 28;
        // Position purely in CANVAS content coords (same space as obj->getBounds()).
        pillX = bounds.getCentreX() - pillW / 2;
        pillY = bounds.getBottom() + 10;
        editorX = 30;
        editorW = pillW - 30 - 32; // [⚡] ...prompt field... [↵]
    }

    mcpSelectionPillFrame = juce::Rectangle<int>(pillX, pillY, pillW, pillH);
    if (!sketchMode) {
        mcpPillDrawerMode = 0;
        mcpPillDrawerFrame = {};
        mcpPaletteFrame = {};
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
                if (editor) editor->nvgSurface.invalidateAll();
                return;
            }
            if (mcpSelectionPill) mcpSelectionPill->setText(juce::String(), false);
            dismissSelectionPill();
        };
        mcpSelectionPill->onTextChange = [this] {
            repaint();
            if (editor) editor->nvgSurface.invalidateAll();
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
    if (editor) editor->nvgSurface.invalidateAll();
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
        if (editor) editor->nvgSurface.invalidateAll();
    }
}

void Canvas::dismissSelectionPill()
{
    mcpPillDrawerMode = 0;
    mcpPillDrawerFrame = {};
    mcpPaletteFrame = {};
    mcpMoodPillActive = false;
    mcpPillRequested = false;
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
    if (editor) editor->nvgSurface.invalidateAll();
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
        if (editor) editor->nvgSurface.invalidateAll();
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
        if (editor) {
            if (auto* cp = editor->getCopilotPanel()) {
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
    if (editor) editor->nvgSurface.invalidateAll();
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
            if (canvas->editor) canvas->editor->nvgSurface.invalidateAll();
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::downKey && count > 0) {
            canvas->mcpPaletteSelectedIndex = (canvas->mcpPaletteSelectedIndex + 1) % count;
            canvas->repaint();
            if (canvas->editor) canvas->editor->nvgSurface.invalidateAll();
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
            if (canvas->editor) canvas->editor->nvgSurface.invalidateAll();
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
        if (canvas->editor) canvas->editor->nvgSurface.invalidateAll();
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
    if (editor) editor->nvgSurface.invalidateAll();
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
    if (editor) editor->nvgSurface.invalidateAll();
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
        if (editor) editor->nvgSurface.invalidateAll();
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

void Canvas::dragAndDropPaste(String const& patchString, Point<int> const mousePos, int const patchWidth, int const patchHeight)
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

    auto args = StringArray::fromTokens(patchString, false);
    auto undoText = args.size() > 4 ? String("Add " + args[4]) : String("Add object");
    patch.startUndoSequence(undoText);

    auto const patchSize = Point<int>(patchWidth, patchHeight);
    String const translatedObjects = pd::Patch::translatePatchAsString(patchString, mousePos - patchSize / 2.0f);

    if (auto patchPtr = patch.getPointer()) {
        pd::Interface::paste(patchPtr.get(), translatedObjects.toRawUTF8());
    }

    deselectAll();

    // Load state from pd
    performSynchronise();

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

    selectionBounds = selectionBounds * getValue<float>(zoomScale);

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
        viewport->setViewPosition(viewportPos + Point<float>(deltaX, deltaY));
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
    if (auto* s = editor->getSidebarForPanel(Sidebar::InspectorPanel))
        s->hideParameters();

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

// The MCP bridge encapsulates an explicit object list under a chosen name, so the
// selection and the "pd" name are parameters here (the menu command passes neither).
void Canvas::encapsulateSelection(String const& subpatchName, SmallArray<Object*> const& objectsToEncapsulate)
{
    auto selectedObjects = objectsToEncapsulate;

    if(auto* sidebar = editor->getSidebarForPanel(Sidebar::SidePanel::InspectorPanel))
        sidebar->clearInspector();

    // Sort by index in pd patch
    selectedObjects.sort([this](auto const* a, auto const* b) -> bool {
        return objects.index_of(a) < objects.index_of(b);
    });

    // Objects without a valid pointer can't be copied, so they can't be part of the subpatch
    auto encapsulatedObjects = SmallArray<Object*>();
    auto encapsulatedPointers = SmallArray<t_gobj*>();
    auto selectionBounds = Rectangle<int>();
    for (auto* object : selectedObjects) {
        if (auto* ptr = object->getPointer()) {
            encapsulatedObjects.add(object);
            encapsulatedPointers.add(ptr);
            selectionBounds = selectionBounds.getUnion(object->getObjectBounds());
        }
    }

    if (encapsulatedObjects.empty())
        return;

    struct IoletGroup {
        bool isInlet;
        bool isSignal;
        SmallArray<Iolet*> internalIolets; // iolets on the objects that move into the subpatch
        SmallArray<Iolet*> externalIolets; // iolets on the objects that stay behind
    };

    auto internalIolets = SmallArray<Iolet*>();
    auto externalIoletsFor = UnorderedMap<Iolet*, SmallArray<Iolet*>>();

    for (auto* connection : connections) {
        bool const inletInside = encapsulatedObjects.contains(connection->inobj.get());
        bool const outletInside = encapsulatedObjects.contains(connection->outobj.get());

        // Connections that are entirely inside or entirely outside the selection stay as they are
        if (inletInside == outletInside)
            continue;

        auto* internalIolet = inletInside ? connection->inlet.get() : connection->outlet.get();
        auto* externalIolet = inletInside ? connection->outlet.get() : connection->inlet.get();
        if (!internalIolet || !externalIolet)
            continue;

        internalIolets.add_unique(internalIolet);
        externalIoletsFor[internalIolet].add_unique(externalIolet);
    }

    // Inlets before outlets, then left to right. Pd numbers a subpatch's iolets by the horizontal
    // position of the [inlet]/[outlet] objects, so this is also the order we lay them out in below
    internalIolets.sort([](auto* a, auto* b) -> bool {
        if (a->isInlet() != b->isInlet())
            return a->isInlet();

        auto const apos = a->getCanvasBounds().getPosition();
        auto const bpos = b->getCanvasBounds().getPosition();

        if (apos.x == bpos.x) {
            return apos.y < bpos.y;
        }

        return apos.x < bpos.x;
    });

    auto ioletGroups = SmallArray<IoletGroup>();
    for (auto* iolet : internalIolets) {
        auto const& externalIolets = externalIoletsFor[iolet];

        // An inlet fed only by control-rate wires from outside becomes [inlet], not [inlet~]
        bool const isSignal = iolet->isInlet()
            ? std::any_of(externalIolets.begin(), externalIolets.end(), [](auto* source) { return source->isSignal(); })
            : iolet->isSignal();

        IoletGroup* group = nullptr;
        for (auto& candidate : ioletGroups) {
            // A signal and a data iolet can never share an object, even if they connect to the same things
            if (candidate.isInlet != iolet->isInlet() || candidate.isSignal != isSignal)
                continue;
            if (candidate.externalIolets.size() != externalIolets.size())
                continue;

            if (std::all_of(externalIolets.begin(), externalIolets.end(), [&candidate](auto* target) { return candidate.externalIolets.contains(target); })) {
                group = &candidate;
                break;
            }
        }

        if (!group) {
            ioletGroups.add(IoletGroup { iolet->isInlet(), isSignal, {}, externalIolets });
            group = &ioletGroups.back();
        }

        group->internalIolets.add(iolet);
    }

    constexpr int ioletObjectSpacing = 45;

    auto newIoletObjects = String();
    auto newInternalConnections = String();

    int numInlets = 0;
    int lastX[2] = {};
    bool hasPlaced[2] = {};

    for (int i = 0; i < static_cast<int>(ioletGroups.size()); i++) {
        auto const& group = ioletGroups[i];
        int const row = group.isInlet ? 0 : 1;

        // Put the object above (or below) the iolet it feeds, but never to the left of the previous one:
        // pd derives the iolet index from the horizontal position, and the connections below rely on it
        int x = group.internalIolets[0]->getCanvasBounds().getCentreX() - canvasOrigin.x;
        if (hasPlaced[row])
            x = jmax(x, lastX[row] + ioletObjectSpacing);
        lastX[row] = x;
        hasPlaced[row] = true;

        int const y = group.isInlet ? selectionBounds.getY() - ioletObjectSpacing : selectionBounds.getBottom() + ioletObjectSpacing;
        auto const type = String(group.isInlet ? "inlet" : "outlet") + String(group.isSignal ? "~" : "");
        newIoletObjects += "#X obj " + String(x) + " " + String(y) + " " + type + ";\n";

        int const ioletObjectIdx = static_cast<int>(encapsulatedObjects.size()) + i;
        for (auto* internalIolet : group.internalIolets) {
            int const objIdx = encapsulatedObjects.index_of(internalIolet->getObject());
            if (objIdx < 0)
                continue;

            if (group.isInlet) {
                newInternalConnections += "#X connect " + String(ioletObjectIdx) + " 0 " + String(objIdx) + " " + String(internalIolet->getIndex()) + ";\n";
            } else {
                newInternalConnections += "#X connect " + String(objIdx) + " " + String(internalIolet->getIndex()) + " " + String(ioletObjectIdx) + " 0;\n";
            }
        }

        numInlets += group.isInlet;
    }

    patch.deselectAll();

    auto const centre = selectionBounds.getCentre();
    auto const nameToken = (subpatchName.isEmpty() || subpatchName == "pd") ? String("pd") : ("pd " + subpatchName);
    auto copypasta = String("#N canvas 733 172 450 300 0 1;\n") + "$$_COPY_HERE_$$" + newIoletObjects + newInternalConnections + "#X restore " + String(centre.x) + " " + String(centre.y) + " " + nameToken + ";\n";

    // Apply the changed on Pd's thread
    if (auto patchPtr = patch.getPointer()) {
        int size;
        char const* text = pd::Interface::copy(patchPtr.get(), &size, encapsulatedPointers);
        auto copied = String::fromUTF8(text, size);

        // Wrap it in an undo sequence, to allow undoing everything in 1 step
        patch.startUndoSequence("Encapsulate");

        pd::Interface::removeObjects(patchPtr.get(), encapsulatedPointers);

        auto replacement = copypasta.replace("$$_COPY_HERE_$$", copied);

        pd::Interface::paste(patchPtr.get(), replacement.toRawUTF8());

        auto patchObjects = patch.getObjects();
        t_object* newObject = nullptr;
        if (!patchObjects.empty() && patchObjects.back().isValid())
            newObject = pd::Interface::checkObject(patchObjects.back().getRaw<t_pd>());

        if (newObject) {
            for (int i = 0; i < static_cast<int>(ioletGroups.size()); i++) {
                auto const& group = ioletGroups[i];

                // Inlets and outlets are numbered separately, and all the inlet groups come first
                int const subpatchIoletIdx = group.isInlet ? i : i - numInlets;

                for (auto* externalIolet : group.externalIolets) {
                    auto* externalObject = reinterpret_cast<t_object*>(externalIolet->getObject()->getPointer());
                    if (!externalObject)
                        continue;

                    if (group.isInlet) {
                        pd::Interface::createConnection(patchPtr.get(), externalObject, externalIolet->getIndex(), newObject, subpatchIoletIdx);
                    } else {
                        pd::Interface::createConnection(patchPtr.get(), newObject, subpatchIoletIdx, externalObject, externalIolet->getIndex());
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
            nearestIolet->setTargeted(false);
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
            if (ptr->gl_edit != editMode) {
            pd->sendDirectMessage(ptr.get(), "editmode", { static_cast<float>(editMode) });
        }
        }

        for(auto* object : objects)
            object->repaint();

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
        MessageManager::callAsync([_this = SafePointer(this)] {
            if (_this)
                _this->selectedComponents.addChangeListener(_this);
        });
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
    if (auto* v = viewport.get()) {
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
    if (isGraph)
        return false;

    if (viewport->isPerformingGesture())
        return true;

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
    case hash("array"): {
        if (isGraph) {
            // Async because this will destroy the current canvas, which the caller might be in the middle of using
            MessageManager::callAsync([_this = SafePointer<Canvas>(this)]() {
                if (!_this)
                    return;
                if (auto* object = _this->findParentComponentOfClass<Object>()) {
                    _this->setSelected(object, false);
                    if (auto* s = _this->editor->getSidebarForPanel(Sidebar::InspectorPanel))
                        s->hideParameters();
                    object->setType(object->gui->getText().toString(), object->gui->ptr);
                }
            });
        }
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
    case hash("goprect"): {
        if (graphArea)
            graphArea->updateBounds();
        break;
    }
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

void Canvas::activateCanvasSearchHighlight(Point<float> viewPos, Object* obj)
{
    canvasSearchHighlight = std::make_unique<CanvasSearchHighlight>(this, obj);
    if (viewport)
        canvasSearchHighlight->chainAnimation(viewport->getMoveAnimation(viewPos));
}

void Canvas::removeCanvasSearchHighlight()
{
    if (canvasSearchHighlight)
        canvasSearchHighlight.reset(nullptr);
}
