/*
 // McpGuiHost implementation (Plan 3 P3.2 S6): the GUI blocks of the MCP bridge
 // handlers, moved verbatim out of Source/Pd/MCPBridge.cpp. Replies stay in the
 // bridge; the blocks here return what the reply needs. See Source/Mcp/McpGuiHost.h.
 */
#include <juce_gui_basics/juce_gui_basics.h>

#include "Mcp/McpGuiHost.h"
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Toolbar.h"
#include "Sidebar/Sidebar.h"
#include "Canvas.h"
#include "CanvasViewport.h"
#include "TabComponent.h"
#include "Object.h"
#include "Connection.h"
#include "Objects/GemCapture.h"
#include "Utility/Fonts.h"
#include "Dialogs/Dialogs.h"
#include "Pd/Patch.h"
#include "Pd/Interface.h"

#include <g_canvas.h>

namespace {

// Was mcpFindGuiCanvasFor (MCPBridge.cpp): the JUCE Canvas showing a Pd canvas.
Canvas* findGuiCanvasFor(PluginProcessor* proc, t_canvas* c)
{
    if (!proc || !c) return nullptr;
    for (auto* editor : proc->getEditors()) {
        if (!editor) continue;
        for (auto* cnvItem : editor->getCanvases()) {
            if (cnvItem && cnvItem->patch.getPointer().get() == c) return cnvItem;
        }
    }
    return nullptr;
}

// Was getOrCreateCanvasComponent (MCPBridge.cpp).
Canvas* getOrCreateCanvasComponent(PluginProcessor* proc, t_canvas* cnv)
{
    if (!proc || !cnv) return nullptr;
    Canvas* canvasComp = nullptr;
    for (auto* editor : proc->getEditors()) {
        if (!editor) continue;
        for (auto* c : editor->getCanvases()) {
            if (c && (c->patch.getUncheckedPointer() == cnv || c->patch.getRawPointer() == cnv || (c->patch.getPointer().get() == cnv))) {
                canvasComp = c;
                break;
            }
        }
        if (!canvasComp && editor->getCurrentCanvas()) {
            if (editor->getCurrentCanvas()->patch.getUncheckedPointer() == cnv || editor->getCurrentCanvas()->patch.getRawPointer() == cnv)
                canvasComp = editor->getCurrentCanvas();
        }
        if (!canvasComp) {
            canvasComp = editor->getTabComponent().openPatch(new pd::Patch(pd::WeakReference(cnv, proc), proc, false));
        }
        if (canvasComp) break;
    }
    return canvasComp;
}

// Was the lookup at the top of /pd/undo and /pd/redo (no tab is opened).
Canvas* findOpenCanvas(PluginProcessor* proc, t_canvas* cnv)
{
    Canvas* canvasComp = nullptr;
    for (auto* editor : proc->getEditors()) {
        if (!editor) continue;
        for (auto* c : editor->getCanvases()) {
            if (c && c->patch.getUncheckedPointer() == cnv) {
                canvasComp = c;
                break;
            }
        }
        if (canvasComp) break;
    }
    return canvasComp;
}

// The first editor (was repeated in ui_open_reference, ui_open_help, viewport, moodboard).
PluginEditor* firstEditor(PluginProcessor* proc)
{
    PluginEditor* editor = nullptr;
    for (auto* ed : proc->getEditors()) {
        if (ed) { editor = ed; break; }
    }
    if (!editor) editor = dynamic_cast<PluginEditor*>(proc->getActiveEditor());
    return editor;
}

Object* findObjectComponent(Canvas* canvasComp, t_gobj* g)
{
    for (auto* objComp : canvasComp->objects) {
        if (objComp && objComp->getPointer() == g)
            return objComp;
    }
    return nullptr;
}

// Was the tab match of /pd/focus_tab and /pd/close_tab.
bool tabMatches(Canvas* canvas, juce::File const& f, juce::String const& target)
{
    return canvas->patch.getCurrentFile() == f
        || canvas->patch.getCurrentFile().getFileName() == target
        || canvas->patch.getTitle() == target
        || canvas->patch.getTitle().equalsIgnoreCase(target);
}

class GuiHost final : public McpGuiHost {
public:
    explicit GuiHost(PluginProcessor& p)
        : proc(&p)
    {
    }

    std::unique_ptr<LiveBounds> liveObjectBounds(t_canvas* cnv) override
    {
        Canvas* guiCanvas = findGuiCanvasFor(proc, cnv);
        if (!guiCanvas) return nullptr;
        auto bounds = std::make_unique<LiveBounds>();
        for (auto* obj : guiCanvas->objects) {
            if (obj) bounds->try_emplace(obj->getPointer(), obj->getSelectableBounds());
        }
        return bounds;
    }

    std::optional<ViewState> viewState(t_canvas* cnv) override
    {
        Canvas* guiCanvas = findGuiCanvasFor(proc, cnv);
        if (!guiCanvas) return std::nullopt;
        ViewState state;
        state.zoom = getValue<float>(guiCanvas->zoomScale);
        if (guiCanvas->viewport) {
            state.hasViewport = true;
            state.viewArea = guiCanvas->viewport->getViewArea();
        }
        return state;
    }

    bool ensureView(t_canvas* cnv) override
    {
        return getOrCreateCanvasComponent(proc, cnv) != nullptr;
    }

    void repaintView(t_canvas* cnv) override
    {
        if (auto* cc = getOrCreateCanvasComponent(proc, cnv)) cc->repaint();
    }

    void repaintAll() override
    {
        for (auto* ed : proc->getEditors()) {
            if (auto* pe = dynamic_cast<PluginEditor*>(ed)) {
                for (auto* c : pe->getCanvases()) {
                    if (c) c->repaint();
                }
                pe->nvgSurface.invalidateAll();
            }
        }
    }

    void frameCreatedObjects(t_canvas* cnv, std::vector<t_gobj*> const& ptrs) override
    {
        auto* cc = findGuiCanvasFor(proc, cnv);
        if (!cc || !cc->viewport) return;
        float z = std::sqrt(std::abs(cc->getTransform().getDeterminant()));
        if (z <= 0.0f) z = 1.0f;
        auto const origin = cc->canvasOrigin;
        auto area = cc->viewport->getViewArea().toFloat();
        juce::Rectangle<float> viewPatch(
            area.getX() / z - (float) origin.x,
            area.getY() / z - (float) origin.y,
            area.getWidth() / z,
            area.getHeight() / z);

        bool offscreen = false;
        bool any = false;
        juce::Rectangle<float> bbox;
        sys_lock();
        for (auto* g : ptrs) {
            if (!g) continue;
            int x = 0, y = 0, w = 0, h = 0;
            pd::Interface::getObjectBounds(cnv, g, &x, &y, &w, &h);
            juce::Rectangle<float> r((float) x, (float) y, (float) w, (float) h);
            bbox = any ? bbox.getUnion(r) : r;
            any = true;
            if (!viewPatch.contains(r.getCentre())) {
                offscreen = true;
                break;
            }
        }
        sys_unlock();
        if (!offscreen || !any) return;

        // Keep zoom, center the new objects.
        auto centre = bbox.getCentre();
        float targetX = centre.x - viewPatch.getWidth() * 0.5f;
        float targetY = centre.y - viewPatch.getHeight() * 0.5f;
        cc->viewport->setViewPosition(juce::Point<int>(
            (int) std::round(((float) origin.x + targetX) * z),
            (int) std::round(((float) origin.y + targetY) * z)).toFloat());
    }

    std::optional<int> overlayMask(t_canvas* cnv) override
    {
        auto* cc = getOrCreateCanvasComponent(proc, cnv);
        if (!cc) return std::nullopt;
        return cc->getOverlays();
    }

    void setOverlayMask(t_canvas* cnv, int mask) override
    {
        if (auto* cc = getOrCreateCanvasComponent(proc, cnv))
            cc->setOverlayMask(mask);
    }

    bool zoomToFit(t_canvas* cnv) override
    {
        Canvas* canvasComp = getOrCreateCanvasComponent(proc, cnv);
        if (!canvasComp) return false;
        canvasComp->zoomToFitAll();
        return true;
    }

    bool alignObjects(t_canvas* cnv, std::vector<t_gobj*> const* targets, juce::String const& alignStr) override
    {
        Canvas* canvasComp = getOrCreateCanvasComponent(proc, cnv);
        if (!canvasComp) return false;

        if (targets) {
            canvasComp->patch.deselectAll();
            for (auto* g : *targets) {
                if (auto* objComp = findObjectComponent(canvasComp, g))
                    canvasComp->setSelected(objComp, true);
            }
        }

        Align alignMode = Align::Left;
        if (alignStr == "right") alignMode = Align::Right;
        else if (alignStr == "hcentre" || alignStr == "hcenter") alignMode = Align::HCentre;
        else if (alignStr == "hdistribute") alignMode = Align::HDistribute;
        else if (alignStr == "top") alignMode = Align::Top;
        else if (alignStr == "bottom") alignMode = Align::Bottom;
        else if (alignStr == "vcentre" || alignStr == "vcenter") alignMode = Align::VCentre;
        else if (alignStr == "vdistribute") alignMode = Align::VDistribute;

        canvasComp->alignObjects(alignMode);
        return true;
    }

    std::optional<Selection> select(t_canvas* cnv, std::vector<t_gobj*> const& objects) override
    {
        Canvas* canvasComp = getOrCreateCanvasComponent(proc, cnv);
        if (!canvasComp) return std::nullopt;

        canvasComp->patch.deselectAll();
        Selection selection;
        selection.title = canvasComp->patch.getTitle();
        for (auto* g : objects) {
            if (auto* objComp = findObjectComponent(canvasComp, g)) {
                canvasComp->setSelected(objComp, true);
                selection.objects.push_back({ juce::String(objComp->getType(false).data()),
                    objComp->cnv->objects.index_of(objComp),
                    objComp->getObjectBounds() });
            }
        }
        return selection;
    }

    int countViewObjects(t_canvas* cnv, std::vector<t_gobj*> const& objects) override
    {
        SmallArray<Object*> found;
        if (!collectObjects(cnv, objects, found)) return -1;
        return static_cast<int>(found.size());
    }

    void encapsulate(t_canvas* cnv, juce::String const& name, std::vector<t_gobj*> const& objects) override
    {
        SmallArray<Object*> toEncapsulate;
        if (!collectObjects(cnv, objects, toEncapsulate)) return;
        if (auto* canvasComp = getOrCreateCanvasComponent(proc, cnv))
            canvasComp->encapsulateSelection(name, toEncapsulate);
    }

    std::vector<ViewWire> viewWiresOf(t_canvas* cnv, t_gobj* box) override
    {
        std::vector<ViewWire> wires;
        Canvas* canvasComp = getOrCreateCanvasComponent(proc, cnv);
        if (!canvasComp) return wires;
        Object* subComp = findObjectComponent(canvasComp, box);
        if (!subComp) return wires;
        for (auto* c : canvasComp->connections) {
            if (!c) continue;
            if (c->inobj.get() != subComp && c->outobj.get() != subComp) continue;
            auto* srcO = c->outobj.get();
            auto* dstO = c->inobj.get();
            wires.push_back({ srcO ? srcO->getPointer() : nullptr, c->outIdx,
                dstO ? dstO->getPointer() : nullptr, c->inIdx });
        }
        return wires;
    }

    UndoResult undo(t_canvas* cnv) override
    {
        Canvas* canvasComp = findOpenCanvas(proc, cnv);
        if (!canvasComp) return UndoResult::NoView;
        if (!canvasComp->canUndo()) return UndoResult::Empty;
        canvasComp->undo();
        return UndoResult::Done;
    }

    bool redo(t_canvas* cnv) override
    {
        Canvas* canvasComp = findOpenCanvas(proc, cnv);
        if (!canvasComp) return false;
        canvasComp->redo();
        return true;
    }

    juce::String screenshot(t_canvas* cnv, float scale, LabelSource const* labelSource, juce::String const& correlationId) override;

    juce::Image captureGemWindow() override
    {
#if ENABLE_GEM
        return captureGemWindowImage();
#else
        return {};
#endif
    }

    std::optional<ViewportInfo> focusedViewport() override
    {
        PluginEditor* editor = firstEditor(proc);
        if (!editor) return std::nullopt;
        auto* canvas = editor->getCurrentCanvas();
        if (!canvas) return std::nullopt;
        auto* vp = canvas->viewport.get();
        float zoom = getValue<float>(canvas->zoomScale);
        float viewX = vp ? static_cast<float>(vp->getViewPositionX()) : 0.0f;
        float viewY = vp ? static_cast<float>(vp->getViewPositionY()) : 0.0f;
        float panX = (viewX / zoom) - static_cast<float>(canvas->canvasOrigin.x);
        float panY = (viewY / zoom) - static_cast<float>(canvas->canvasOrigin.y);
        float vpW = vp ? static_cast<float>(vp->getViewArea().getWidth()) : static_cast<float>(canvas->getWidth());
        float vpH = vp ? static_cast<float>(vp->getViewArea().getHeight()) : static_cast<float>(canvas->getHeight());
        float scaleFactor = editor->getRenderScale();
        return ViewportInfo { zoom, panX, panY, vpW, vpH, scaleFactor };
    }

    std::optional<SketchInfo> focusedSketch() override
    {
        PluginEditor* editor = firstEditor(proc);
        if (!editor) return std::nullopt;
        auto* canvas = editor->getCurrentCanvas();
        if (!canvas) return std::nullopt;
        SketchInfo info;
        info.strokesJson = canvas->buildSketchJson();
        auto const sb = canvas->getSketchBoundsInCanvas();
        if (!sb.isEmpty())
            info.bounds = sb.translated(-static_cast<float>(canvas->canvasOrigin.x), -static_cast<float>(canvas->canvasOrigin.y));
        info.selection = canvas->getSelectionStableIds();
        return info;
    }

    std::vector<TabInfo> listTabs() override
    {
        std::vector<TabInfo> tabs;
        for (auto* editor : proc->getEditors()) {
            if (!editor) continue;
            auto* focused = editor->getCurrentCanvas();
            for (auto* canvas : editor->getCanvases()) {
                if (!canvas) continue;
                tabs.push_back({ canvas->patch.getPointer().get(), canvas->patch.getTitle(),
                    canvas->patch.getCurrentFile(), canvas == focused });
            }
        }
        return tabs;
    }

    bool openPatch(juce::File const& file) override
    {
        for (auto* editor : proc->getEditors()) {
            if (!editor) continue;
            editor->getTabComponent().openPatch(juce::URL(file));
            return true;
        }
        return false;
    }

    void closeDuplicateTabs(juce::File const& f) override
    {
        bool keptFirst = false;
        for (auto* editor : proc->getEditors()) {
            if (!editor) continue;
            for (auto* canvas : editor->getCanvases()) {
                if (!canvas || canvas->patch.getCurrentFile() != f) continue;
                if (!keptFirst) { keptFirst = true; continue; }
                editor->getTabComponent().closeTab(canvas);
                proc->logMessage("MCP open_patch: closed duplicate tab for " + f.getFullPathName());
            }
        }
    }

    t_canvas* findOpenPatch(juce::File const& f) override
    {
        for (auto* editor : proc->getEditors()) {
            if (!editor) continue;
            for (auto* canvas : editor->getCanvases()) {
                if (!canvas || canvas->patch.getCurrentFile() != f) continue;
                return canvas->patch.getPointer().get();
            }
            break;
        }
        return nullptr;
    }

    std::optional<juce::String> focusTab(juce::String const& target) override
    {
        juce::File f(target);
        for (auto* editor : proc->getEditors()) {
            if (!editor) continue;
            for (auto* canvas : editor->getCanvases()) {
                if (!canvas) continue;
                if (tabMatches(canvas, f, target)) {
                    editor->getTabComponent().showTab(canvas, canvas->patch.splitViewIndex);
                    editor->getTabComponent().setActiveSplit(canvas);
                    return canvas->patch.getTitle();
                }
            }
        }
        return std::nullopt;
    }

    CloseResult closeTab(juce::String const& target, bool force) override
    {
        juce::File f(target);
        for (auto* editor : proc->getEditors()) {
            if (!editor) continue;
            for (auto* canvas : editor->getCanvases()) {
                if (!canvas) continue;
                if (tabMatches(canvas, f, target)) {
                    if (canvas->patch.isDirty() && !force)
                        return { CloseResult::Dirty, canvas->patch.getTitle() };
                    juce::String title = canvas->patch.getTitle();
                    editor->getTabComponent().closeTab(canvas);
                    return { CloseResult::Closed, title };
                }
            }
        }
        return {};
    }

    void setPatchFile(t_canvas* cnv, juce::File const& f) override
    {
        for (auto* editor : proc->getEditors()) {
            if (!editor) continue;
            for (auto* canvas : editor->getCanvases()) {
                if (canvas && canvas->patch.getPointer().get() == cnv) {
                    canvas->patch.setCurrentFile(URL(f));
                    canvas->patch.setTitle(f.getFileName());
                    return;
                }
            }
        }
    }

    bool showObjectReference(juce::String const& objectName) override
    {
        PluginEditor* editor = firstEditor(proc);
        if (!editor) return false;
        Dialogs::showObjectReferenceDialog(&editor->openedDialog, editor, objectName);
        return true;
    }

    bool openHelpPatch(juce::File const& helpFile) override
    {
        PluginEditor* editor = firstEditor(proc);
        if (!editor) return false;
        editor->getTabComponent().openHelpPatch(juce::URL(helpFile));
        return true;
    }

    void clearConsole() override
    {
        for (auto* editor : proc->getEditors()) {
            if (auto* consoleSidebar = editor ? editor->getSidebarForPanel(Sidebar::ConsolePanel) : nullptr)
                consoleSidebar->clearConsole();
        }
    }

    juce::Font defaultFont() override
    {
        return Fonts::getDefaultFont();
    }

    float cpuUsage() override
    {
        return proc->toolbarSource ? proc->toolbarSource->getCPUUsage() : 0.0f;
    }

private:
    // Resolves gobjs to distinct components of the view; false on the first miss
    // (was the fail-closed loop of /pd/encapsulate and /pd/encapsulate_to_file).
    bool collectObjects(t_canvas* cnv, std::vector<t_gobj*> const& objects, SmallArray<Object*>& out)
    {
        Canvas* canvasComp = getOrCreateCanvasComponent(proc, cnv);
        if (!canvasComp) return false;
        for (auto* g : objects) {
            auto* objComp = g ? findObjectComponent(canvasComp, g) : nullptr;
            if (!objComp) return false;
            if (!out.contains(objComp))
                out.add(objComp);
        }
        return true;
    }

    PluginProcessor* proc;
};

juce::String GuiHost::screenshot(t_canvas* cnv, float scale, LabelSource const* labelSource, juce::String const& correlationId)
{
    // Prefer named canvas; fall back to focused canvas in any open editor
    Canvas* canvasComp = cnv ? getOrCreateCanvasComponent(proc, cnv) : nullptr;
    if (!canvasComp && proc) {
        for (auto* editor : proc->getEditors()) {
            if (editor && editor->getCurrentCanvas()) {
                canvasComp = editor->getCurrentCanvas();
                break;
            }
        }
    }

    if (!canvasComp) {
        return juce::String("error:no_canvas");
    }

    // ── NVG surface capture ──
    // PlugData's Canvas is an NVGComponent: its JUCE paint() path is EMPTY
    // and all rendering goes to the GPU via PluginEditor::nvgSurface.
    // createComponentSnapshot therefore returns a blank image — verified
    // (3KB all-black PNGs). Correct path: force-render the target region
    // into the surface FBO, then pull pixels with renderFrameToImage.
    PluginEditor* editor = nullptr;
    for (auto* ed : proc->getEditors()) {
        if (ed && ed->getCurrentCanvas() == canvasComp) { editor = ed; break; }
    }
    if (!editor) {
        return juce::String("error:no_editor");
    }

    // ── Native window capture (createSnapshotOfNativeWindow) ──
    // PlugData's Canvas is NVGComponent: JUCE paint() is empty, GPU-side
    // rendering. createSnapshotOfNativeWindow uses XGetImage on the X11
    // display connection JUCE already holds — no shell, no env issues.
    // It returns a juce::Image of the WHOLE editor window at native pixels.
    auto* peer = editor->getPeer();
    if (!peer) {
        return juce::String("error:no_peer");
    }

    // Canvas viewport + editor bounds in SCREEN coordinates.
    juce::Rectangle<int> viewportScreen;
    if (canvasComp->viewport != nullptr)
        viewportScreen = canvasComp->viewport->getScreenBounds();
    else
        viewportScreen = canvasComp->getScreenBounds();
    auto editorScreen = editor->getScreenBounds();

    // Grab the whole editor window via XGetImage (JUCE public API).
    // getNativeHandle() returns ::Window (XID) on Linux — same value
    // xwd -id uses, but via the existing X11 connection (no shell needed).
    auto img = juce::createSnapshotOfNativeWindow(peer->getNativeHandle());
    if (!img.isValid()) {
        return juce::String("error:snapshot_failed");
    }
    DBG("[screenshot_canvas] snapshot " + juce::String(img.getWidth()) + "x" + juce::String(img.getHeight())
        + " editorScreen=" + editorScreen.toString()
        + " viewportScreen=" + viewportScreen.toString());

    // createSnapshotOfNativeWindow returns the image at the display's
    // physical pixel scale (already divided by desktop scale inside JUCE).
    // editorScreen + viewportScreen are in logical pixels; no extra scale needed.
    // Crop to the viewport sub-rect within the window.
    juce::Rectangle<int> crop;
    crop.setX(viewportScreen.getX() - editorScreen.getX());
    crop.setY(viewportScreen.getY() - editorScreen.getY());
    crop.setWidth(viewportScreen.getWidth());
    crop.setHeight(viewportScreen.getHeight());
    crop = crop.getIntersection(img.getBounds());
    if (crop.isEmpty()) {
        return juce::String("error:empty_crop w=") + juce::String(img.getWidth())
            + " h=" + juce::String(img.getHeight())
            + " crop=" + crop.toString();
    }

    // Guard: at least 1% pixel variance (catches blank/hidden windows)
    {
        juce::Image::BitmapData bd(img, juce::Image::BitmapData::readOnly);
        long varying = 0, total = 0;
        juce::uint8 firstR = 0, firstG = 0, firstB = 0;
        bool haveFirst = false;
        for (int row = crop.getY(); row < crop.getBottom(); row += 6) {
            for (int col = crop.getX(); col < crop.getRight(); col += 6) {
                auto px = img.getPixelAt(col, row);
                if (!haveFirst) { firstR = px.getRed(); firstG = px.getGreen(); firstB = px.getBlue(); haveFirst = true; }
                if (px.getRed() != firstR || px.getGreen() != firstG || px.getBlue() != firstB) varying++;
                total++;
            }
        }
        if (total > 0 && varying * 100 / total < 1) {
            return juce::String("error:blank_window (uniform canvas — hidden or off-screen?)");
        }
    }

    img = img.getClippedImage(crop);

    // Apply requested scale
    if (std::abs(scale - 1.0f) > 0.01f && img.getWidth() > 0 && img.getHeight() > 0) {
        int targetW = juce::roundToInt(img.getWidth() * scale);
        int targetH = juce::roundToInt(img.getHeight() * scale);
        if (targetW > 0 && targetH > 0) {
            juce::Image resized(juce::Image::ARGB, targetW, targetH, true);
            juce::Graphics rg(resized);
            rg.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
            rg.drawImage(img, juce::Rectangle<float>(0, 0, (float)targetW, (float)targetH),
                juce::RectanglePlacement::stretchToFit);
            img = resized;
        }
    }

    // ── Label collection (the bridge walks the Pd canvas of the captured view) ──
    std::vector<ScreenshotLabel> labels;
    if (labelSource) {
        labels = (*labelSource)(canvasComp->patch.getRawPointer());

        // Diagnostic: label count + first label coords -> stderr (DBG)
        DBG("[screenshot_labels] collected=" + juce::String((int)labels.size())
            + " img=" + juce::String(img.getWidth()) + "x" + juce::String(img.getHeight()));
        if (!labels.empty()) {
            float zoom0 = getValue<float>(canvasComp->zoomScale);
            if (zoom0 <= 0.001f) zoom0 = 1.0f;
            int vx0 = canvasComp->viewport ? canvasComp->viewport->getViewArea().getX() : 0;
            int vy0 = canvasComp->viewport ? canvasComp->viewport->getViewArea().getY() : 0;
            DBG("[screenshot_labels] first=" + labels[0].text
                + " pd(" + juce::String(labels[0].x) + "," + juce::String(labels[0].y) + ")"
                + " zoom=" + juce::String(zoom0, 2)
                + " view=(" + juce::String(vx0) + "," + juce::String(vy0) + ")"
                + " imgX=" + juce::String((labels[0].x * zoom0 - vx0) * scale, 1)
                + " imgY=" + juce::String((labels[0].y * zoom0 - vy0) * scale, 1));
        }
    }

    // ── Paint labels onto the captured bitmap (never the canvas) ──
    // Coordinate model (Object::updateBounds + Canvas transform):
    //   object in canvas-component px = canvasOrigin + pdPos
    //   viewport shows canvas at viewPos; zoom via component transform
    //   object in viewport px = zoom × (canvasOrigin + pdPos − viewPos)
    //   image px = (viewport-relative) × scale, since img IS the viewport
    if (!labels.empty()) {
        float zoom = 1.0f;
        int viewX = 0, viewY = 0;
        if (canvasComp->viewport) {
            zoom = getValue<float>(canvasComp->zoomScale);
            if (zoom <= 0.001f) zoom = 1.0f;
            viewX = canvasComp->viewport->getViewPositionX();
            viewY = canvasComp->viewport->getViewPositionY();
        }
        auto const canvasOrigin = canvasComp->canvasOrigin;

        juce::Graphics g(img);
        juce::Font labelFont = Fonts::getDefaultFont()
            .withHeight(juce::jlimit(9.0f, 16.0f, 12.0f * scale));
        g.setFont(labelFont);

        for (auto const& lbl : labels) {
            // object top-left in viewport px
            float vpX = zoom * static_cast<float>(canvasOrigin.x + lbl.x - viewX);
            float vpY = zoom * static_cast<float>(canvasOrigin.y + lbl.y - viewY);
            // → image coords (image = viewport × scale)
            float imgX = vpX * scale;
            float imgY = vpY * scale;
            float objHImg = static_cast<float>(lbl.h) * zoom * scale;

            float tw = juce::GlyphArrangement().getStringWidth(labelFont, lbl.text);
            float th = labelFont.getHeight();
            float pad = 2.0f * scale;
            float chipH = th + 2.0f * pad;
            float chipW = tw + 3.0f * pad;

            // Place chip above the object; flip below when clipped at top
            float chipY = imgY - chipH - 1.0f;
            if (chipY < 0.0f) chipY = imgY + objHImg + 1.0f;

            // Skip labels fully outside the capture
            if (imgX + chipW < 0.0f || imgX > static_cast<float>(img.getWidth())
                || chipY + chipH < 0.0f || chipY > static_cast<float>(img.getHeight()))
                continue;

            g.setColour(juce::Colours::black.withAlpha(0.72f));
            g.fillRoundedRectangle(juce::Rectangle<float>(imgX, chipY, chipW, chipH), 3.0f * scale);
            g.setColour(juce::Colours::white.withAlpha(0.92f));
            g.drawText(lbl.text, juce::Rectangle<float>(imgX + pad, chipY + pad, tw, th),
                juce::Justification::centredLeft);
        }
    }

    // Write PNG to a temp file — TS reads it synchronously (same machine)
    auto tmpFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("plugdata_canvas_" + correlationId + ".png");

    juce::FileOutputStream fos(tmpFile);
    if (!fos.openedOk()) {
        return juce::String("error:file_open");
    }

    juce::PNGImageFormat png;
    if (!png.writeImageToStream(img, fos)) {
        return juce::String("error:png_encode");
    }
    fos.flush();

    return tmpFile.getFullPathName();
}

} // namespace

std::unique_ptr<McpGuiHost> createMcpGuiHost(PluginProcessor& processor)
{
    return std::make_unique<GuiHost>(processor);
}
