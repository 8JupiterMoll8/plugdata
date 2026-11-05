/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// P12a.1: Alphi on one Canvas, the flow-mode switch (owner decision 2026-11-03). A small robot face (white head, dark
// screen, two dot eyes; Teenage Engineering) in a corner of the visible view, at a fixed screen size, while the MCP
// bridge runs. Big Alphi 1a0: the corner is one no box, chip stack, pill or drawer lies under (default bottom right,
// McpAlphiDock.h, Tests/mcp/alphi_dock_test.cpp); the A8 live bug was Alphi sitting on the artist's boxes top left.
// Off: calm, dim, flat eyes. On: warm yellow glowing eyes, the antenna tip lit, "FLOW" on its chin.
// A click on it turns flow mode on (McpHost::alphiClicked, the only path to FlowMode::artistTurnOn), the next click
// off. No verb reaches it. Geometry, click rule and look: McpAlphi.h (Tests/mcp/alphi_test.cpp).

#include <juce_gui_basics/juce_gui_basics.h>
#include "Utility/Config.h"
#include "Utility/Fonts.h"
#include "Utility/NVGTextShim.h"

#include "Mcp/CanvasMcpLayer.h"
#include "Canvas.h"
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "CanvasViewport.h"
#include "Mcp/McpHost.h"
#include "Mcp/McpAlphi.h"
#include "Mcp/McpAlphiDock.h"
#include "Mcp/McpAlphiMotion.h"

namespace {

std::int64_t alphiNowMs() { return static_cast<std::int64_t>(juce::Time::currentTimeMillis()); }

mcp::ApprovalRect toApproval(juce::Rectangle<int> const& j) { return { j.getX(), j.getY(), j.getWidth(), j.getHeight() }; }

// A viewport overlay smaller than this on either side is a scrollbar (9 px at the view edge, clear of Alphi's 12 px
// margin); anything bigger (the minimap, bottom right) is a cover Alphi must not sit under.
constexpr int alphiOverlayMinSide = 40;

NVGcolor rgba(mcp::AlphiRgba c, float alphaScale = 1.0f)
{
    return nanovg::nvgRGBA(c.r, c.g, c.b, static_cast<unsigned char>(juce::jlimit(0.0f, 255.0f, c.a * alphaScale)));
}

} // namespace

bool CanvasMcpLayer::alphiShown() const
{
    // Every top-level view of a canvas while the bridge really runs (review L3: not merely while the object exists);
    // never inside an embedded graph-on-parent box.
    return canvas.pd && !canvas.isGraph && canvas.viewport && canvas.pd->getMcp()->bridgeRunning();
}

bool CanvasMcpLayer::alphiCoveredNow(juce::Rectangle<int> frame) const
{
    auto const r = [](juce::Rectangle<int> const& j) { return mcp::ApprovalRect { j.getX(), j.getY(), j.getWidth(), j.getHeight() }; };
    bool const pillUp = mcpSelectionPill && mcpSelectionPill->isVisible();
    return mcp::alphiCovered(r(frame), { r(mcpApprovalFrame), pillUp ? r(mcpSelectionPillFrame) : mcp::ApprovalRect {},
                                             pillUp ? r(mcpPillDrawerFrame) : mcp::ApprovalRect {}, pillUp ? r(mcpPaletteFrame) : mcp::ApprovalRect {} });
}

void CanvasMcpLayer::alphiVisibilityTick()
{
    bool const showing = canvas.isShowing();
    if (showing != mcpAlphiWasShowing) {
        mcpAlphiWasShowing = showing;
        mcpAlphiShownSinceMs = 0; // hidden: off the glass; shown again: the 400 ms start over at the next paint
        mcpAlphiDock.reset();     // 1a0: shown again: placed at once at the next paint
        if (showing) {
            canvas.repaint();
            if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
        }
        return;
    }
    if (!showing || !alphiShown())
        return;
    // 1a0 review H2: the move is DECIDED here, never inside a paint (a partial NVG render redraws only the damaged
    // area). A staged move asks for a full repaint; paintOverObjects applies it only when the render covers both the
    // old and the new frame (AlphiDock::apply), and asks at most maxApplyTries times (review L1).
    bool wantFull = false;
    if (mcpAlphiDock.hasStaged()) {
        wantFull = mcpAlphiDock.retryStaged();
    } else if (mcpAlphiDock.placed() && mcpAlphiDock.due(alphiNowMs())) {
        auto const [view, zoom] = alphiView();
        auto const boxes = alphiBoxes();
        auto const mouse = alphiMouse();
        bool const hold = mcp::alphiDockHold(mcp::alphiCornerRect(view, zoom, mcpAlphiDock.corner()), zoom, boxes,
            juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown(), mouse);
        wantFull = mcpAlphiDock.decide(view, zoom, boxes, alphiDockCovers(), hold, mouse, alphiNowMs());
    }
    if (wantFull) {
        canvas.repaint();
        if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
    }
}

std::pair<mcp::ApprovalRect, float> CanvasMcpLayer::alphiView() const
{
    auto const a = canvas.viewport->getViewArea();
    auto const view = mcp::approvalVisibleArea(static_cast<float>(a.getX()), static_cast<float>(a.getY()), static_cast<float>(a.getWidth()), static_cast<float>(a.getHeight()));
    float zoom = canvas.viewport->getViewScale();
    if (!(zoom > 0.01f) || !std::isfinite(zoom))
        zoom = 1.0f;
    return { view, zoom };
}

std::vector<mcp::ApprovalRect> CanvasMcpLayer::alphiDockCovers() const
{
    std::vector<mcp::ApprovalRect> covers;
    // The chip stack as last laid out (around Alphi; it only counts when it could not move off it), the selection pill,
    // its drawers and the palette.
    if (!mcpApprovalFrame.isEmpty())
        covers.push_back(toApproval(mcpApprovalFrame));
    if (mcpSelectionPill && mcpSelectionPill->isVisible()) {
        for (auto const& f : { mcpSelectionPillFrame, mcpPillDrawerFrame, mcpPaletteFrame })
            if (!f.isEmpty())
                covers.push_back(toApproval(f));
    }
    // PlugData's own overlays on the view (the minimap at the bottom right), in canvas coordinates.
    if (canvas.viewport) {
        for (auto* child : canvas.viewport->getChildren()) {
            if (child == &canvas || !child->isVisible())
                continue;
            auto const b = child->getBounds();
            if (b.getWidth() < alphiOverlayMinSide || b.getHeight() < alphiOverlayMinSide)
                continue;
            covers.push_back(toApproval(canvas.getLocalArea(canvas.viewport.get(), b)));
        }
    }
    return covers;
}

std::vector<mcp::ApprovalRect> CanvasMcpLayer::alphiBoxes() const
{
    // The boxes: the VISIBLE box of each object on this canvas, read on the message thread; no Pd lock, no census.
    // Object::getBounds() carries PlugData's invisible margin on every side (1a0 live check: with the 8 px pad that
    // blocked 14 px around each box); the pad in alphiOwnsPoint covers the inlet/outlet strips.
    std::vector<mcp::ApprovalRect> boxes;
    boxes.reserve(static_cast<std::size_t>(canvas.objects.size()));
    for (auto* o : canvas.objects)
        if (o && o->isVisible())
            boxes.push_back(toApproval(o->getBounds().reduced(Object::margin)));
    return boxes;
}

mcp::AlphiMouse CanvasMcpLayer::alphiMouse() const
{
    // Known only while the mouse is over this canvas (or a button is held on it); else no hover hold, no cover.
    bool const known = canvas.isMouseOver(true) || canvas.isMouseButtonDown(true);
    auto const m = canvas.getMouseXYRelative();
    return { known, m.x, m.y };
}

void CanvasMcpLayer::updateAlphiDock()
{
    mcpAlphiPaintBoxes.clear();
    if (!alphiShown()) {
        mcpAlphiDock.reset();
        return;
    }
    auto const [view, zoom] = alphiView();
    mcpAlphiPaintBoxes = alphiBoxes();
    auto const covers = alphiDockCovers();
    auto const now = alphiNowMs();
    mcpAlphiDock.look(view, zoom, mcpAlphiPaintBoxes, covers, now);
    // The first place is chosen here (there is no old frame to leave behind); every later move by the timer.
    if (!mcpAlphiDock.placed() && !mcpAlphiDock.hasStaged())
        mcpAlphiDock.decide(view, zoom, mcpAlphiPaintBoxes, covers, false, alphiMouse(), now);
    if (!mcpAlphiDock.hasStaged())
        return;
    // Review H2: the staged move lands only when this render redraws the old and the new frame.
    auto const oldFrame = mcp::alphiCornerRect(view, zoom, mcpAlphiDock.corner());
    auto const newFrame = mcp::alphiCornerRect(view, zoom, mcpAlphiDock.stagedCorner());
    if (mcpAlphiDock.apply(oldFrame, newFrame, toApproval(mcpAlphiRenderRegion)))
        mcpAlphiShownSinceMs = 0; // moved: a click aimed at what was in that corner is never flow (400 ms gate again)
}

std::optional<mcp::ApprovalRect> CanvasMcpLayer::alphiKeepOut(mcp::ApprovalRect view, float zoom) const
{
    if (!alphiShown())
        return std::nullopt;
    return mcp::alphiCornerRect(view, zoom, mcpAlphiDock.corner());
}

void CanvasMcpLayer::paintAlphi(NVGcontext* nvg)
{
    mcpAlphiFrame = {};
    auto const region = toApproval(mcpAlphiRenderRegion);
    mcpAlphiRenderRegion = {}; // used once: a paint that skipped paintUnderObjects never reuses a stale full region
    if (!mcpAlphiVisibilityTimer && alphiShown()) {
        mcpAlphiVisibilityTimer = std::make_unique<AlphiVisibilityTimer>(this);
        mcpAlphiWasShowing = canvas.isShowing();
        mcpAlphiVisibilityTimer->startTimer(100);
    }
    if (!alphiShown() || !mcpAlphiDock.placed()) {
        mcpAlphiShownSinceMs = 0;
        return;
    }
    auto const [view, zoom] = alphiView();
    auto const r = mcp::alphiCornerRect(view, zoom, mcpAlphiDock.corner());
    juce::Rectangle<int> const frame(r.x, r.y, r.w, r.h);

    // The 400 ms click gate (mcp::alphiGateAfterPaint): a chip stack that could not move off it (a tiny or split view),
    // the selection pill and its drawers cover it (P12a.1 review M2: not drawn); its frame moved on screen (review M2:
    // resize, split view, sidebar); and it starts only in a render that redrew Alphi's whole frame (review H2). A box
    // under it does not touch the gate: its click points are the box's (mcp::alphiOwnsPoint, per point).
    mcp::AlphiGateInput gate;
    gate.covered = alphiCoveredNow(frame);
    bool const underBox = mcp::alphiFrameBlocked(r, mcpAlphiPaintBoxes);
    gate.frameInRegion = mcp::alphiRectContains(region, r);
    auto const screen = canvas.localPointToGlobal(frame.getPosition());
    gate.screenMoved = mcpAlphiHasScreenPos && mcp::alphiScreenMoved(mcpAlphiScreenPos.x, mcpAlphiScreenPos.y, screen.x, screen.y);
    if (gate.frameInRegion) {
        mcpAlphiScreenPos = screen;
        mcpAlphiHasScreenPos = true;
    }
    mcpAlphiShownSinceMs = mcp::alphiGateAfterPaint(mcpAlphiShownSinceMs, alphiNowMs(), gate);
    // Review N2: a box arrived under Alphi or left, or his gate is still 0, and this render missed his frame: repaint
    // just his frame once (mcp::alphiAfterPaint never asks twice for one state).
    if (mcp::alphiAfterPaint(mcpAlphiRepaint, underBox, mcpAlphiShownSinceMs, gate.frameInRegion, gate.covered)) {
        auto const m = mcp::alphiMotionArea(r, zoom); // F0: grown by 3 design px, so the rounding never cuts his frame
        juce::Rectangle<int> const area(m.x, m.y, m.w, m.h);
        canvas.repaint(area);
        if (canvas.editor) canvas.editor->nvgSurface.invalidateArea(canvas.editor->nvgSurface.getLocalArea(&canvas, area));
    }
    if (gate.covered)
        return;
    mcpAlphiFrame = frame;

    using A = mcp::AlphiMetrics;
    bool const on = canvas.pd->getMcp()->session.flowMode.isOn();
    auto const look = mcp::alphiLook(on);
    float const k = static_cast<float>(r.w) / static_cast<float>(A::w); // canvas units per design pixel

    NVGScopedState scoped(nvg);
    if (underBox)
        nanovg::nvgGlobalAlpha(nvg, 0.6f); // a box under it: lightly faded, the box shows through (its points are its own)
    nanovg::nvgTranslate(nvg, static_cast<float>(r.x), static_cast<float>(r.y));
    nanovg::nvgScale(nvg, k, k); // from here on: design pixels, 48 x 56

    float const cx = A::w * 0.5f;
    // Antenna: a stem and a tip that lights up warm when flow is on.
    nanovg::nvgBeginPath(nvg);
    nanovg::nvgMoveTo(nvg, cx, 11.0f);
    nanovg::nvgLineTo(nvg, cx, 5.5f);
    nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(205, 205, 200, 255));
    nanovg::nvgStrokeWidth(nvg, 2.0f);
    nanovg::nvgStroke(nvg);
    if (look.antennaLit) {
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgCircle(nvg, cx, 4.5f, 6.0f);
        nanovg::nvgFillColor(nvg, rgba(look.antenna, 0.28f));
        nanovg::nvgFill(nvg);
    }
    nanovg::nvgBeginPath(nvg);
    nanovg::nvgCircle(nvg, cx, 4.5f, 3.2f);
    nanovg::nvgFillColor(nvg, rgba(look.antenna));
    nanovg::nvgFill(nvg);

    // Head: a chunky rounded white block with a soft shadow.
    nanovg::nvgBeginPath(nvg);
    nanovg::nvgRoundedRect(nvg, 2.0f, 12.5f, 44.0f, 43.0f, 12.0f);
    nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(0, 0, 0, 60));
    nanovg::nvgFill(nvg);
    nanovg::nvgDrawRoundedRect(nvg, 2.0f, 10.0f, 44.0f, 44.0f, nanovg::nvgRGBA(242, 241, 236, 255), nanovg::nvgRGBA(196, 196, 190, 255), 12.0f);

    // Screen: dark glass.
    nanovg::nvgDrawRoundedRect(nvg, 8.0f, 17.0f, 32.0f, 21.0f, nanovg::nvgRGBA(24, 26, 31, 255), nanovg::nvgRGBA(10, 11, 14, 255), 6.0f);

    // Eyes.
    float const eyeY = 27.5f;
    for (float ex : { cx - 7.0f, cx + 7.0f }) {
        if (look.glow) {
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgCircle(nvg, ex, eyeY, look.eyeRadius * 2.1f);
            nanovg::nvgFillColor(nvg, rgba(look.eye, 0.22f));
            nanovg::nvgFill(nvg);
        }
        nanovg::nvgBeginPath(nvg);
        if (look.sleepy)
            nanovg::nvgRoundedRect(nvg, ex - look.eyeRadius * 1.3f, eyeY - 1.1f, look.eyeRadius * 2.6f, 2.2f, 1.1f);
        else
            nanovg::nvgCircle(nvg, ex, eyeY, look.eyeRadius);
        nanovg::nvgFillColor(nvg, rgba(look.eye));
        nanovg::nvgFill(nvg);
    }

    // Chin: "FLOW" while on.
    if (look.label) {
        mcpnvg::nvgFontFace(nvg, "Inter");
        mcpnvg::nvgFontSize(nvg, 9.0f);
        mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(214, 140, 0, 255));
        mcpnvg::nvgText(nvg, cx, 46.5f, look.labelText, nullptr);
    }
}

bool CanvasMcpLayer::handleAlphiClick(juce::Point<int> canvasPt)
{
    if (!alphiShown() || mcpAlphiFrame.isEmpty() || !mcpAlphiFrame.contains(canvasPt))
        return false;
    // Covered right now (review L2: the pill or a chip arrived since the last paint) or on a hidden tab: not Alphi's
    // click at all; what covers it takes it.
    if (!canvas.isShowing() || alphiCoveredNow(mcpAlphiFrame)) {
        mcpAlphiShownSinceMs = 0;
        mcpAlphiFrame = {};
        return false;
    }
    // 1a0 review H1 / round 3 (per point): a click inside a box under Alphi (8 px pad) is the box's or the canvas's,
    // never flow; a click on a free part of Alphi goes on to the normal gate.
    if (!mcp::alphiOwnsPoint(toApproval(mcpAlphiFrame), alphiBoxes(), canvasPt.x, canvasPt.y, alphiView().second))
        return false;
    // Just appeared (a new tab, the bridge just started, uncovered) or an approval chip left the glass less than
    // 400 ms ago (review M2): not clickable, but the click is still Alphi's.
    auto const now = alphiNowMs();
    if (!mcp::alphiClickAllowed(mcpAlphiShownSinceMs, now, canvas.pd->getMcp()->session.approvals.lastChipGoneMs()))
        return true;
    canvas.pd->getMcp()->alphiClicked(now);
    canvas.repaint();
    if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
    return true;
}
