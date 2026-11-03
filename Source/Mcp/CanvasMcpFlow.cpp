/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// P12a A3: the FLOW badge of one Canvas. While flow mode is on (session.flowMode, McpFlowMode.h) a small badge sits at
// the top-left of the visible viewport at a fixed screen size; a click on it switches flow mode off (the safe
// direction). Turning it on happens only through the flow chip's ✓ (CanvasMcpApproval.cpp -> McpHost::answerApprovalChip).

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

namespace {
constexpr float kBadgeW = 132.0f; // design (screen) pixels
constexpr float kBadgeH = 22.0f;
constexpr float kBadgeMargin = 10.0f;
} // namespace

void CanvasMcpLayer::paintFlowBadge(NVGcontext* nvg)
{
    mcpFlowBadgeFrame = {};
    if (!canvas.pd || !canvas.pd->getMcp()->session.flowMode.isOn())
        return;
    juce::Rectangle<float> view = canvas.getLocalBounds().toFloat();
    if (canvas.viewport)
        view = canvas.viewport->getViewArea().toFloat();
    float zoom = canvas.viewport ? canvas.viewport->getViewScale() : getValue<float>(canvas.zoomScale);
    if (!(zoom > 0.01f))
        zoom = 1.0f;
    float const k = 1.0f / zoom; // canvas units per screen pixel
    float const x = view.getX() + kBadgeMargin * k;
    float const y = view.getY() + kBadgeMargin * k;
    mcpFlowBadgeFrame = juce::Rectangle<float>(x, y, kBadgeW * k, kBadgeH * k).getSmallestIntegerContainer();

    NVGScopedState scoped(nvg);
    nanovg::nvgTranslate(nvg, x, y);
    nanovg::nvgScale(nvg, k, k);
    nanovg::nvgDrawRoundedRect(nvg, 0, 0, kBadgeW, kBadgeH, nanovg::nvgRGBA(10, 60, 70, 235), nanovg::nvgRGBA(56, 210, 230, 240), kBadgeH * 0.5f);
    mcpnvg::nvgFontFace(nvg, "Inter");
    mcpnvg::nvgFontSize(nvg, 11.5f);
    mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(200, 250, 255, 250));
    mcpnvg::nvgText(nvg, kBadgeW * 0.5f, kBadgeH * 0.5f, "FLOW  \xc2\xb7  click to stop", nullptr);
}

bool CanvasMcpLayer::handleFlowBadgeClick(juce::Point<int> canvasPt)
{
    if (!canvas.pd || mcpFlowBadgeFrame.isEmpty() || !mcpFlowBadgeFrame.contains(canvasPt))
        return false;
    if (!canvas.pd->getMcp()->session.flowMode.isOn())
        return false;
    canvas.pd->getMcp()->flowModeOff("badge");
    mcpFlowBadgeFrame = {};
    canvas.repaint();
    if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
    return true;
}

// Review M2: the flow offer, apart from the approval stack so it is never mistaken for (or clicked instead of) a real
// question: top centre of the visible view, teal, its own button words. Same click rule as every chip: answerable only
// once fully on screen for 400 ms (markPaintedIfInside); Enter never answers it (ApprovalState::enterTarget).
void CanvasMcpLayer::paintFlowOfferChip(NVGcontext* nvg, mcp::ApprovalChip const& chip, mcp::ApprovalRect view)
{
    using M = mcp::ApprovalBarMetrics;
    float const zoom = canvas.viewport ? canvas.viewport->getViewScale() : getValue<float>(canvas.zoomScale);
    // Anchor: a point at the top centre of the view, so the bar sits just under the top edge (below the FLOW badge row).
    mcp::ApprovalRect const anchor { view.x + view.w / 2, view.y + static_cast<int>(28.0f / (zoom > 0.01f ? zoom : 1.0f)), 0, 0 };
    int const barW = M::w + 40;
    auto const layout = mcp::approvalStackLayout(anchor, view, zoom, barW, 1, false);
    if (layout.bars.empty()) return;
    auto const& bl = layout.bars.front();
    float const k = layout.scale;
    auto toJ = [](mcp::ApprovalRect const& r) { return juce::Rectangle<int>(r.x, r.y, r.w, r.h); };
    ApprovalHit hit;
    hit.nonce = juce::String(chip.nonce);
    hit.frame = toJ(bl.frame);
    hit.yes = toJ(bl.yes);
    hit.no = toJ(bl.no);
    mcpApprovalHits.push_back(hit);
    mcpFlowChipFrame = hit.frame;
    canvas.pd->getMcp()->session.approvals.markPaintedIfInside(chip.nonce, reinterpret_cast<std::uintptr_t>(this) ^ 0x1, bl.frame, view,
        static_cast<std::int64_t>(juce::Time::currentTimeMillis()));

    int const half = mcp::approvalButtonW(barW);
    float const btnH = static_cast<float>(M::btnH);
    NVGScopedState scoped(nvg);
    nanovg::nvgTranslate(nvg, static_cast<float>(bl.frame.x), static_cast<float>(bl.frame.y));
    nanovg::nvgScale(nvg, k, k);
    nanovg::nvgDrawRoundedRect(nvg, 0, 0, static_cast<float>(barW), static_cast<float>(M::h),
        nanovg::nvgRGBA(8, 40, 48, 250), nanovg::nvgRGBA(56, 210, 230, 240), 16.0f);
    mcpnvg::nvgFontFace(nvg, "Inter");
    mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    mcpnvg::nvgFontSize(nvg, 11.5f);
    nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(200, 250, 255, 250));
    mcpnvg::nvgText(nvg, static_cast<float>(M::textX), M::line1Y, chip.title.c_str(), nullptr);
    mcpnvg::nvgFontSize(nvg, 10.5f);
    nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(150, 220, 230, 220));
    mcpnvg::nvgText(nvg, static_cast<float>(M::textX), M::line2Y, chip.detail.c_str(), nullptr);
    mcpnvg::nvgFontSize(nvg, 13.0f);
    mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    float const yesX = static_cast<float>(M::btnX);
    float const noX = static_cast<float>(M::btnX + half + M::btnGap);
    float const btnY = static_cast<float>(M::btnY);
    nanovg::nvgDrawRoundedRect(nvg, yesX, btnY, static_cast<float>(half), btnH,
        nanovg::nvgRGBA(20, 120, 140, 235), nanovg::nvgRGBA(56, 210, 230, 255), 12.0f);
    nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(235, 255, 255, 255));
    mcpnvg::nvgText(nvg, yesX + half * 0.5f, btnY + btnH * 0.5f, "Flow on (click)", nullptr);
    nanovg::nvgDrawRoundedRect(nvg, noX, btnY, static_cast<float>(half), btnH,
        nanovg::nvgRGBA(40, 44, 52, 235), nanovg::nvgRGBA(90, 98, 112, 255), 12.0f);
    nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(210, 220, 235, 235));
    mcpnvg::nvgText(nvg, noX + half * 0.5f, btnY + btnH * 0.5f, "Not now", nullptr);
}
