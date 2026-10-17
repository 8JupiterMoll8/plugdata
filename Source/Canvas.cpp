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
    mcpLayer = std::make_unique<CanvasMcpLayer>(*this); // MCP-HOOK(overlayPaint)
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
        pd->getMcp()->clearMcpTransactions(patch.getUncheckedPointer());

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

    mcpLayer->paintUnderObjects(nvg, invalidRegion); // MCP-HOOK(overlayPaint)

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

    mcpLayer->paintOverObjects(nvg); // MCP-HOOK(overlayPaint)

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

    mcpLayer->paintViewportFrame(nvg, zoom); // MCP-HOOK(overlayPaint)

    nanovg::nvgRestore(nvg);

    // Draw scrollbars
    if (viewport) {
        viewport->render(nvg, viewport->getLocalArea(this, invalidRegion));
    }

    mcpLayer->paintScreenOverlay(nvg); // MCP-HOOK(overlayPaint)
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
            if (showConnectionOrder && mcpLayer->isTargeted(connection)) { // MCP-HOOK(overlayTarget)
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
            auto tid = pd->getMcp()->getStableId(ptr);
            if (tid.isEmpty() && cnv) {
                tid = pd->getMcp()->getOrAdoptStableId(cnv, ptr);
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
        juce::ScopedLock sl(pd->getMcp()->session.mcpOverlayLock);
        std::erase_if(pd->getMcp()->session.mcpAnnotations, [](McpSession::McpAnnotation const& a) {
            return a.text.isEmpty() && a.kind == "artist";
        });

        McpSession::McpAnnotation ann;
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
        pd->getMcp()->session.mcpAnnotations.push_back(ann);
    }

    openNoteEditorForIndex(static_cast<int>(pd->getMcp()->session.getMcpAnnotations().size()) - 1);
}

void Canvas::openNoteEditorForIndex(int index)
{
    if (!pd) return;
    auto const anns = pd->getMcp()->session.getMcpAnnotations();
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
                juce::ScopedLock sl(pd->getMcp()->session.mcpOverlayLock);
                if (mcpNoteEditIndex < static_cast<int>(pd->getMcp()->session.mcpAnnotations.size())) {
                    if (txt.isEmpty()) {
                        pd->getMcp()->session.mcpAnnotations.erase(pd->getMcp()->session.mcpAnnotations.begin() + mcpNoteEditIndex);
                    } else {
                        auto& a = pd->getMcp()->session.mcpAnnotations[static_cast<size_t>(mcpNoteEditIndex)];
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
                juce::ScopedLock sl(pd->getMcp()->session.mcpOverlayLock);
                if (mcpNoteEditIndex < static_cast<int>(pd->getMcp()->session.mcpAnnotations.size()))
                    pd->getMcp()->session.mcpAnnotations[static_cast<size_t>(mcpNoteEditIndex)].text = mcpNoteEditor->getText();
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
    if (mcpCompletedStrokes.empty() && pd->getMcp()->session.getMcpReferenceImages().empty()) return;

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
    if (auto* br = pd->getMcp()->getMCPBridge()) {
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

    McpSession::McpReferenceImage ref;
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
    pd->getMcp()->session.addMcpReferenceImage(ref);

    // Bridge dispatch: /pd/reference/image <path> <x> <y> <w> <h> (PD patch coords)
    if (auto* br = pd->getMcp()->getMCPBridge()) {
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

bool Canvas::handleVoiceTakeClick(MouseEvent const& e, Point<int> mousePos)
{
    if (!pd) return false;
    auto const takes = pd->getMcp()->session.getMcpVoiceTakes();
    for (auto it = takes.rbegin(); it != takes.rend(); ++it) {
        auto const& take = *it;
        if (!take.visible) continue;
        Rectangle<int> const b(static_cast<int>(canvasOrigin.x + take.x), static_cast<int>(canvasOrigin.y + take.y), 240, 38);
        if (!b.contains(mousePos)) continue;

        int const relX = mousePos.x - b.getX();
        auto* br = pd->getMcp()->getMCPBridge();

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
            pd->getMcp()->session.removeMcpVoiceTake(take.id);
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

bool Canvas::handleMidiTakeClick(MouseEvent const& e, Point<int> mousePos)
{
    if (!pd) return false;
    auto const takes = pd->getMcp()->session.getMcpMidiTakes();
    for (auto it = takes.rbegin(); it != takes.rend(); ++it) {
        auto const& take = *it;
        if (!take.visible) continue;
        Rectangle<int> const b(static_cast<int>(canvasOrigin.x + take.x), static_cast<int>(canvasOrigin.y + take.y), 240, 38);
        if (!b.contains(mousePos)) continue;

        int const relX = mousePos.x - b.getX();
        auto* br = pd->getMcp()->getMCPBridge();

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
            pd->getMcp()->session.removeMcpMidiTake(take.id);
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
    auto const images = pd->getMcp()->session.getMcpReferenceImages();
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
                    for (auto const& im : canvas->pd->getMcp()->session.getMcpReferenceImages()) {
                        if (im.id == imageId) {
                            canvas->pd->getMcp()->session.setMcpReferenceLock(imageId, !im.isLocked);
                            break;
                        }
                    }
                    canvas->repaint();
                    if (canvas->editor) canvas->editor->nvgSurface.invalidateAll();
                } else if (result == 3) {
                    canvas->pd->getMcp()->session.removeMcpReferenceImage(imageId);
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
    auto const hud = pd->getMcp()->session.getMcpHud();
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
    mcpLayer->updateOverlays(overlayState); // MCP-HOOK(overlayPaint)
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
            if (pd->getMcp()->getStableId(ptr) == targetId)
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
                auto const allAnns = pd->getMcp()->session.getMcpAnnotations();
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
    auto const anns = pd->getMcp()->session.getMcpAnnotations();
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

    if (pd && !pd->getMcp()->session.getMcpAnnotations().empty()) {
        auto const pt = e.getPosition().toFloat();
        auto const anns = pd->getMcp()->session.getMcpAnnotations();
        for (int i = static_cast<int>(anns.size()) - 1; i >= 0; --i) {
            auto const tb = getAnnotationScreenBounds(static_cast<size_t>(i));
            if (tb.isEmpty()) continue;

            auto const chips = CanvasMcpLayer::getAnnotationPivotChips(anns[static_cast<size_t>(i)].text, anns[static_cast<size_t>(i)].kind);
            float totalChipsW = 0.0f;
            for (auto const& c : chips) totalChipsW += CanvasMcpLayer::getChipWidth(c.label) + 4.0f;
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
                        float const cw = CanvasMcpLayer::getChipWidth(chips[ci].label);
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
    if (!pd || pd->getMcp()->session.getMcpAnnotations().empty()) return false;
    auto const pt = canvasPt.toFloat();
    auto const anns = pd->getMcp()->session.getMcpAnnotations();
    for (size_t i = 0; i < anns.size(); ++i) {
        auto const tb = getAnnotationScreenBounds(i);
        if (tb.isEmpty()) continue;
        if (static_cast<int>(i) == mcpNoteHover) {
            auto const chips = CanvasMcpLayer::getAnnotationPivotChips(anns[i].text, anns[i].kind);
            float totalChipsW = 0.0f;
            for (auto const& c : chips) totalChipsW += CanvasMcpLayer::getChipWidth(c.label) + 4.0f;
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
                        if (auto* br = pd->getMcp()->getMCPBridge()) br->sendLens(lens.isEmpty() ? "free" : lens);
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
                            if (auto* br = pd->getMcp()->getMCPBridge()) {
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
                            if (auto* br = pd->getMcp()->getMCPBridge()) {
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
                            if (auto* br = pd->getMcp()->getMCPBridge()) {
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

    if (!pd || pd->getMcp()->session.getMcpAnnotations().empty()) return false;
    auto const pt = canvasPt.toFloat();
    auto anns = pd->getMcp()->session.getMcpAnnotations();
    for (int i = static_cast<int>(anns.size()) - 1; i >= 0; --i) {
        auto const& a = anns[static_cast<size_t>(i)];
        auto const tb = getAnnotationScreenBounds(static_cast<size_t>(i));
        if (tb.isEmpty()) continue;

        auto const chips = CanvasMcpLayer::getAnnotationPivotChips(a.text, a.kind);
        float totalChipsW = 0.0f;
        for (auto const& c : chips) totalChipsW += CanvasMcpLayer::getChipWidth(c.label) + 4.0f;
        float const drawerW = std::max(tb.getWidth(), totalChipsW + 4.0f);
        constexpr float drawerH = 22.0f;
        juce::Rectangle<float> const combinedBounds(tb.getX(), tb.getY(), drawerW, tb.getHeight() + drawerH + 3.0f);

        if (!combinedBounds.contains(pt)) continue;

        // 1. Rightmost ~16px of the tape header is the dismiss "x"
        juce::Rectangle<float> closeZone(tb.getRight() - 16.0f, tb.getY(), 16.0f, tb.getHeight());
        if (closeZone.contains(pt)) {
            if (mcpNoteEditor) mcpNoteEditor->setVisible(false);
            mcpNoteEditIndex = -1;
            juce::ScopedLock sl(pd->getMcp()->session.mcpOverlayLock);
            if (i < static_cast<int>(pd->getMcp()->session.mcpAnnotations.size()))
                pd->getMcp()->session.mcpAnnotations.erase(pd->getMcp()->session.mcpAnnotations.begin() + i);
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
                float const cw = CanvasMcpLayer::getChipWidth(chips[ci].label);
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
                                    if (auto* br = pd->getMcp()->getMCPBridge())
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
        if (mcpLayer->shouldShowAISketch() && (e.mods.isAltDown() || mcpSketchToolActive || mcpEraserToolActive)) {
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
            if (false && pd && !pd->getMcp()->session.getMcpAnnotations().empty()) {
                auto const pt = e.getPosition().toFloat();
                auto anns = pd->getMcp()->session.getMcpAnnotations();
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
                                        juce::ScopedLock sl(pd->getMcp()->session.mcpOverlayLock);
                                        if (mcpNoteEditIndex < static_cast<int>(pd->getMcp()->session.mcpAnnotations.size())) {
                                            auto& ann = pd->getMcp()->session.mcpAnnotations[static_cast<size_t>(mcpNoteEditIndex)];
                                            ann.text = txt;
                                            ann.kind = "artist"; // it's the artist's note now
                                            targetId = ann.targetId;
                                        }
                                    }
                                    mcpNoteEditIndex = -1;
                                    if (mcpNoteEditor) mcpNoteEditor->setVisible(false);
                                    if (send && txt.isNotEmpty() && pd) {
                                        if (auto* br = pd->getMcp()->getMCPBridge()) br->sendArtistNote(targetId, txt);
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
                                        juce::ScopedLock sl(pd->getMcp()->session.mcpOverlayLock);
                                        if (mcpNoteEditIndex < static_cast<int>(pd->getMcp()->session.mcpAnnotations.size()))
                                            pd->getMcp()->session.mcpAnnotations[static_cast<size_t>(mcpNoteEditIndex)].text = mcpNoteEditor->getText();
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
        if (mcpLayer->shouldShowAISketch() && (mcpSketchToolActive || mcpEraserToolActive)) {
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
        if (pd) pd->getMcp()->session.setMcpVoiceTakePos(mcpDraggingTakeId, static_cast<float>(pdPos.x), static_cast<float>(pdPos.y));
        repaint();
        if (editor) editor->nvgSurface.invalidateAll();
        return;
    }

    // MIDI take chip drag.
    if (mcpDraggingMidiTakeId.isNotEmpty()) {
        auto const pt = e.getEventRelativeTo(this).getPosition();
        auto const pdPos = pt - mcpMidiTakeDragOffset - canvasOrigin;
        if (pd) pd->getMcp()->session.setMcpMidiTakePos(mcpDraggingMidiTakeId, static_cast<float>(pdPos.x), static_cast<float>(pdPos.y));
        repaint();
        if (editor) editor->nvgSurface.invalidateAll();
        return;
    }

    // PRD Phase 2: dragging a reference image across the canvas glass.
    if (mcpDraggingRefImageId.isNotEmpty()) {
        auto const pt = e.getEventRelativeTo(this).getPosition();
        auto const pdPos = pt - mcpRefImageDragOffset - canvasOrigin;
        if (pd) pd->getMcp()->session.moveMcpReferenceImage(mcpDraggingRefImageId, static_cast<float>(pdPos.x), static_cast<float>(pdPos.y));
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
                if (auto* br = pd->getMcp()->getMCPBridge()) {
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
        McpSession::McpAnnotation ann;
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

        juce::ScopedLock sl(pd->getMcp()->session.mcpOverlayLock);
        pd->getMcp()->session.mcpAnnotations.push_back(ann);
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
                auto tid = pd->getMcp()->getStableId(ptr);
                if (tid.isEmpty() && cnv) {
                    tid = pd->getMcp()->getOrAdoptStableId(cnv, ptr);
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
            auto tid = pd->getMcp()->getStableId(ptr);
            if (tid.isEmpty() && cnv) {
                tid = pd->getMcp()->getOrAdoptStableId(cnv, ptr);
            }
            if (tid.isNotEmpty()) targetIds.add(tid);
        }
    }
    if (auto* br = pd->getMcp()->getMCPBridge()) br->sendSelectionPrompt(prompt, targetIds, queueForChat);
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
    if (pd && pd->getMcp()->hasMcpTransaction(patch.getUncheckedPointer())) {
        pd->getMcp()->undoMcpTransaction(patch.getUncheckedPointer());
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
    if (pd && pd->getMcp()->hasMcpTransaction(patch.getUncheckedPointer())) return true;
    return patch.canUndo();
}

void Canvas::redo()
{
    // 1. If an AI transaction was undone, redo that first to recreate objects before redoing manual edits on them
    if (pd && pd->getMcp()->hasMcpRedoTransaction(patch.getUncheckedPointer())) {
        pd->getMcp()->redoMcpTransaction(patch.getUncheckedPointer());
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
