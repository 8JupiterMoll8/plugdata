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
    if (mcpApprovalFrame.isEmpty() || !mcpApprovalFrame.contains(canvasPt)) return false;
    for (auto const& hit : mcpApprovalHits) {
        if (hit.yes.contains(canvasPt)) { answerApproval(hit.nonce, true); break; }
        if (hit.no.contains(canvasPt)) { answerApproval(hit.nonce, false); break; }
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

    // The visible viewport in canvas coordinates (the space objects and the pill are drawn in).
    float z = std::sqrt(std::abs(canvas.getTransform().getDeterminant()));
    if (z <= 0.0f) z = 1.0f;
    juce::Rectangle<int> view = canvas.getLocalBounds();
    if (canvas.viewport) {
        auto const a = canvas.viewport->getViewArea().toFloat();
        view = juce::Rectangle<float>(a.getX() / z, a.getY() / z, a.getWidth() / z, a.getHeight() / z).toNearestInt();
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
    int const stackH = shown * kBarH + (shown - 1) * kBarGap + (more > 0 ? 22 : 0);
    auto const place = mcp::approvalBarRect(anchor, toRect(view), kBarW, stackH);
    mcpApprovalFrame = juce::Rectangle<int>(place.x, place.y, place.w, place.h);

    int const half = (kBarW - 24) / 2;
    for (int i = 0; i < shown; ++i) {
        // Newest on top: the question just asked is the one the artist is looking for.
        auto const& c = chips[chips.size() - 1 - static_cast<std::size_t>(i)];
        int const bx = place.x;
        int const by = place.y + i * (kBarH + kBarGap);
        ApprovalHit hit;
        hit.nonce = juce::String(c.nonce);
        hit.frame = { bx, by, kBarW, kBarH };
        hit.yes = { bx + 8, by + 26, half, kBarH - 32 };
        hit.no = { bx + 8 + half + 8, by + 26, half, kBarH - 32 };
        mcpApprovalHits.push_back(hit);

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
    state.markPainted(nowMs()); // starts the on-screen time Enter waits for
}
