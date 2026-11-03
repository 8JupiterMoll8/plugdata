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
