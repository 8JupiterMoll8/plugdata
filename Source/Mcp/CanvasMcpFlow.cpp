/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// P12a.1: Alphi on one Canvas, the flow-mode switch (owner decision 2026-11-03). A small robot face (white head, dark
// screen, two dot eyes; Teenage Engineering) at the top left of the visible view, at a fixed screen size, while the MCP
// bridge runs. Off: calm, dim, flat eyes. On: warm yellow glowing eyes, the antenna tip lit, "FLOW" on its chin.
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

namespace {

std::int64_t alphiNowMs() { return static_cast<std::int64_t>(juce::Time::currentTimeMillis()); }

NVGcolor rgba(mcp::AlphiRgba c, float alphaScale = 1.0f)
{
    return nanovg::nvgRGBA(c.r, c.g, c.b, static_cast<unsigned char>(juce::jlimit(0.0f, 255.0f, c.a * alphaScale)));
}

} // namespace

bool CanvasMcpLayer::alphiShown() const
{
    // Every top-level view of a canvas while the bridge runs; never inside an embedded graph-on-parent box.
    return canvas.pd && !canvas.isGraph && canvas.viewport && canvas.pd->getMcp()->getMCPBridge() != nullptr;
}

std::optional<mcp::ApprovalRect> CanvasMcpLayer::alphiKeepOut(mcp::ApprovalRect view, float zoom) const
{
    if (!alphiShown())
        return std::nullopt;
    return mcp::alphiRect(view, zoom);
}

void CanvasMcpLayer::paintAlphi(NVGcontext* nvg)
{
    mcpAlphiFrame = {};
    if (!alphiShown()) {
        mcpAlphiShownSinceMs = 0;
        return;
    }
    auto const a = canvas.viewport->getViewArea();
    auto const view = mcp::approvalVisibleArea(static_cast<float>(a.getX()), static_cast<float>(a.getY()), static_cast<float>(a.getWidth()), static_cast<float>(a.getHeight()));
    float zoom = canvas.viewport->getViewScale();
    if (!(zoom > 0.01f))
        zoom = 1.0f;
    auto const r = mcp::alphiRect(view, zoom);
    juce::Rectangle<int> const frame(r.x, r.y, r.w, r.h);

    // The selection pill (and its drawers) win their corner: Alphi steps aside while they cover it.
    bool const pillUp = mcpSelectionPill && mcpSelectionPill->isVisible();
    if (pillUp && (frame.intersects(mcpSelectionPillFrame) || frame.intersects(mcpPillDrawerFrame) || frame.intersects(mcpPaletteFrame))) {
        mcpAlphiShownSinceMs = 0;
        return;
    }
    mcpAlphiFrame = frame;
    if (mcpAlphiShownSinceMs == 0)
        mcpAlphiShownSinceMs = alphiNowMs();

    using A = mcp::AlphiMetrics;
    bool const on = canvas.pd->getMcp()->session.flowMode.isOn();
    auto const look = mcp::alphiLook(on);
    float const k = static_cast<float>(r.w) / static_cast<float>(A::w); // canvas units per design pixel

    NVGScopedState scoped(nvg);
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
    // Just appeared (a new tab, the bridge just started): not clickable yet, but the click is still Alphi's.
    auto const now = alphiNowMs();
    if (!mcp::alphiClickAllowed(mcpAlphiShownSinceMs, now))
        return true;
    canvas.pd->getMcp()->alphiClicked(now);
    canvas.repaint();
    if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
    return true;
}
