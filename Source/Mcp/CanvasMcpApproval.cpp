/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// P12c: the canvas approval chips of one Canvas. The state is the session's (McpApprovalState.h, set only by the
// server through /pd/approval_show|clear); this file draws one bar per pending record (reason, ✓ Approve, ✗ Cancel),
// outlines the record's targets (the server-built ghost preview), and turns a click / Enter / Esc into
// /pd/approval/answer <nonce> yes|no for exactly that record. The whole stack is clamped inside the visible viewport;
// with no pill or selection to sit under it goes to the viewport centre, never to canvasOrigin (L5).

#include <juce_gui_basics/juce_gui_basics.h>
#include "Utility/Config.h"
#include "Utility/Fonts.h"
#include "Utility/NVGTextShim.h"

#include "Mcp/CanvasMcpLayer.h"
#include "Canvas.h"
#include "Object.h"
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "CanvasViewport.h"
#include "Mcp/McpHost.h"
#include "Pd/MCPBridge.h"

#include <set>

namespace {
constexpr int kBarW = 440;
constexpr int kBarH = 62;
constexpr int kBarGap = 6;
constexpr int kMaxBars = 4; // more pending records show as a count line under the stack

std::int64_t nowMs() { return static_cast<std::int64_t>(juce::Time::currentTimeMillis()); }

mcp::ApprovalRect toRect(juce::Rectangle<int> r) { return { r.getX(), r.getY(), r.getWidth(), r.getHeight() }; }
} // namespace

bool CanvasMcpLayer::isApprovalRequested() const
{
    return canvas.pd && canvas.pd->getMcp()->session.approvals.any(nowMs());
}

void CanvasMcpLayer::answerApproval(juce::String const& nonce, bool yes)
{
    if (!canvas.pd || nonce.isEmpty()) return;
    // Taken locally first: a double click or a held Enter cannot answer twice; the server clears it too.
    if (!canvas.pd->getMcp()->session.approvals.take(nonce.toStdString(), nowMs())) return;
    if (auto* br = canvas.pd->getMcp()->getMCPBridge()) br->sendApprovalAnswer(nonce, yes);
    mcpApprovalHits.clear();
    mcpApprovalFrame = {};
    canvas.repaint();
    if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
}

bool CanvasMcpLayer::handleApprovalClick(juce::Point<int> canvasPt)
{
    if (!canvas.pd || mcpApprovalFrame.isEmpty() || !mcpApprovalFrame.contains(canvasPt)) return false;
    auto& state = canvas.pd->getMcp()->session.approvals;
    for (auto const& hit : mcpApprovalHits) {
        bool const yes = hit.yes.contains(canvasPt);
        if (!yes && !hit.no.contains(canvasPt)) continue;
        // A chip that just appeared (or is not fully on screen) is not clickable yet: same 400 ms as Enter.
        if (state.clickAllowed(hit.nonce.toStdString(), nowMs())) answerApproval(hit.nonce, yes);
        break;
    }
    return true; // a click on the stack never falls through to the canvas
}

std::optional<bool> CanvasMcpLayer::handleApprovalKey(juce::KeyPress const& key)
{
    if (!canvas.pd || !isApprovalRequested()) return std::nullopt;
    auto& state = canvas.pd->getMcp()->session.approvals;
    if (key == juce::KeyPress::returnKey) {
        // Exactly one pending record, its chip on screen long enough (M5); else Enter does nothing here.
        if (auto nonce = state.enterTarget(nowMs())) answerApproval(juce::String(*nonce), true);
        return true;
    }
    if (key == juce::KeyPress::escapeKey) {
        for (auto const& nonce : state.escTargets(nowMs())) answerApproval(juce::String(nonce), false);
        return true;
    }
    return std::nullopt;
}

void CanvasMcpLayer::paintApprovalChips(NVGcontext* nvg)
{
    mcpApprovalHits.clear();
    mcpApprovalFrame = {};
    if (!canvas.pd) return;
    auto& state = canvas.pd->getMcp()->session.approvals;
    auto const chips = state.visible(nowMs());
    if (chips.empty()) return;

    // The visible viewport in canvas coordinates (the space objects and the pill are drawn in). getViewArea() already
    // is that (view position and size divided by the zoom): used as it is, never divided by the zoom again.
    mcp::ApprovalRect view = toRect(canvas.getLocalBounds());
    if (canvas.viewport) {
        auto const a = canvas.viewport->getViewArea();
        view = mcp::approvalVisibleArea(a.getX(), a.getY(), a.getWidth(), a.getHeight());
    }

    // Server-built ghost preview: outline every target of every pending record that lives on this canvas.
    {
        std::set<std::string> targets;
        bool whole = false;
        for (auto const& c : chips)
            for (auto const& t : c.targets) {
                if (t == "*") whole = true;
                else targets.insert(t);
            }
        juce::Rectangle<int> all;
        bool first = true;
        NVGScopedState scopedGhost(nvg);
        for (auto* o : canvas.objects) {
            if (!o) continue;
            auto const b = o->getBounds();
            if (whole) {
                all = first ? b : all.getUnion(b);
                first = false;
            }
            if (targets.empty()) continue;
            auto* ptr = o->getPointer();
            if (!ptr) continue;
            auto const id = canvas.pd->getMcp()->getStableId(ptr);
            if (id.isEmpty() || targets.count(id.toStdString()) == 0) continue;
            auto const r = b.expanded(4).toFloat();
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, r.getX(), r.getY(), r.getWidth(), r.getHeight(), 5.0f);
            nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 90, 70, 34));
            nanovg::nvgFill(nvg);
            nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(255, 90, 70, 220));
            nanovg::nvgStrokeWidth(nvg, 1.6f);
            nanovg::nvgLineStyle(nvg, NVG_LINE_DASHED);
            nanovg::nvgDashLength(nvg, 5.0f);
            nanovg::nvgStroke(nvg);
            nanovg::nvgLineStyle(nvg, NVG_LINE_SOLID);
        }
        if (whole && !first) {
            auto const r = all.expanded(12).toFloat();
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, r.getX(), r.getY(), r.getWidth(), r.getHeight(), 10.0f);
            nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 90, 70, 18));
            nanovg::nvgFill(nvg);
            nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(255, 90, 70, 200));
            nanovg::nvgStrokeWidth(nvg, 2.0f);
            nanovg::nvgLineStyle(nvg, NVG_LINE_DASHED);
            nanovg::nvgDashLength(nvg, 8.0f);
            nanovg::nvgStroke(nvg);
            nanovg::nvgLineStyle(nvg, NVG_LINE_SOLID);
        }
    }

    // Anchor: under the selection pill, else under the selection, else none (the viewport centre).
    std::optional<mcp::ApprovalRect> anchor;
    if (mcpSelectionPill && mcpSelectionPill->isVisible() && !mcpSelectionPillFrame.isEmpty()) {
        anchor = toRect(mcpSelectionPillFrame);
    } else {
        juce::Rectangle<int> b;
        bool first = true;
        for (auto* o : canvas.getSelectionOfType<Object>()) {
            if (!o) continue;
            b = first ? o->getBounds() : b.getUnion(o->getBounds());
            first = false;
        }
        if (!first) anchor = toRect(b);
    }

    int const shown = std::min<int>(static_cast<int>(chips.size()), kMaxBars);
    int const more = static_cast<int>(chips.size()) - shown;
    // Placed for the FULL stack, whatever is pending now: a new chip is appended below without moving the ones already
    // on screen (no reflow under the cursor).
    int const fullH = kMaxBars * kBarH + (kMaxBars - 1) * kBarGap + 22;
    auto place = mcp::approvalBarRect(anchor, view, kBarW, fullH);
    place.h = shown * kBarH + (shown - 1) * kBarGap + (more > 0 ? 22 : 0);
    mcpApprovalFrame = juce::Rectangle<int>(place.x, place.y, place.w, place.h);

    int const half = (kBarW - 24) / 2;
    for (int i = 0; i < shown; ++i) {
        // Oldest first: new questions are appended below, nothing already on screen moves.
        auto const& c = chips[static_cast<std::size_t>(i)];
        int const bx = place.x;
        int const by = place.y + i * (kBarH + kBarGap);
        ApprovalHit hit;
        hit.nonce = juce::String(c.nonce);
        hit.frame = { bx, by, kBarW, kBarH };
        hit.yes = { bx + 8, by + 26, half, kBarH - 32 };
        hit.no = { bx + 8 + half + 8, by + 26, half, kBarH - 32 };
        mcpApprovalHits.push_back(hit);
        // Its on-screen time (Enter and clicks wait 400 ms) starts only when the whole bar is inside the view.
        state.markPaintedIfInside(c.nonce, mcp::ApprovalRect { bx, by, kBarW, kBarH }, view, nowMs());

        NVGScopedState scopedAppr(nvg);
        nanovg::nvgTranslate(nvg, static_cast<float>(bx), static_cast<float>(by));
        nanovg::nvgDrawRoundedRect(nvg, 0, 0, static_cast<float>(kBarW), static_cast<float>(kBarH),
            nanovg::nvgRGBA(28, 16, 16, 250), nanovg::nvgRGBA(255, 90, 70, 225), 10.0f);
        mcpnvg::nvgFontFace(nvg, "Inter");
        mcpnvg::nvgFontSize(nvg, 12.0f);
        mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 220, 210, 240));
        auto reason = juce::String(c.reason);
        if (reason.length() > 64) reason = reason.substring(0, 63) + juce::String::fromUTF8("\xe2\x80\xa6");
        mcpnvg::nvgText(nvg, 10.0f, 14.0f, reason.toRawUTF8(), nullptr);
        mcpnvg::nvgFontSize(nvg, 14.0f);
        mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nanovg::nvgDrawRoundedRect(nvg, 8, 26, static_cast<float>(half), static_cast<float>(kBarH - 32),
            nanovg::nvgRGBA(40, 170, 90, 235), nanovg::nvgRGBA(90, 225, 140, 255), 6.0f);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 255, 255, 255));
        mcpnvg::nvgText(nvg, 8 + half * 0.5f, 26 + (kBarH - 32) * 0.5f, juce::String::fromUTF8("\xe2\x9c\x93 Approve").toRawUTF8(), nullptr);
        nanovg::nvgDrawRoundedRect(nvg, 8 + half + 8, 26, static_cast<float>(half), static_cast<float>(kBarH - 32),
            nanovg::nvgRGBA(40, 44, 52, 235), nanovg::nvgRGBA(90, 98, 112, 255), 6.0f);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(210, 220, 235, 235));
        mcpnvg::nvgText(nvg, 8 + half + 8 + half * 0.5f, 26 + (kBarH - 32) * 0.5f, juce::String::fromUTF8("\xe2\x9c\x97 Cancel").toRawUTF8(), nullptr);
    }
    if (more > 0) {
        NVGScopedState scopedMore(nvg);
        mcpnvg::nvgFontFace(nvg, "Inter");
        mcpnvg::nvgFontSize(nvg, 12.0f);
        mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 200, 190, 230));
        auto const line = "+" + juce::String(more) + " more waiting";
        mcpnvg::nvgText(nvg, place.x + kBarW * 0.5f, place.y + place.h - 10.0f, line.toRawUTF8(), nullptr);
    }
}
