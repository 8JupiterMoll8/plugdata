/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// P12c: the canvas approval chips of one Canvas. The state is the session's (McpApprovalState.h, set only by the
// server through /pd/approval_show|clear); this file draws one bar per pending record (line 1 the server's words, line 2 the names, ✓ Approve, ✗ Cancel),
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
// The bar's size in design (screen) pixels lives in McpApprovalState.h (ApprovalBarMetrics); drawn at a fixed screen
// size whatever the zoom (live check 2026-11-02). Line 1 never goes below minTitleFont: the bar widens instead.
using M = mcp::ApprovalBarMetrics;

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
    // Taken first: a double click or a held Enter cannot answer twice; the server clears it too
    // (McpHost::answerApprovalChip sends the answer to the server).
    if (!canvas.pd->getMcp()->answerApprovalChip(nonce.toStdString(), yes, nowMs())) return;
    mcpApprovalHits.clear();
    mcpApprovalFrame = {};
    repaintApprovals();
}

void CanvasMcpLayer::repaintApprovals()
{
    canvas.repaint();
    if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
}

void CanvasMcpLayer::ApprovalExpiryTimer::timerCallback()
{
    stopTimer();
    canvas->repaintApprovals();
}

bool CanvasMcpLayer::handleApprovalClick(juce::Point<int> canvasPt)
{
    if (!canvas.pd) return false;
    if (mcpApprovalFrame.isEmpty() || !mcpApprovalFrame.contains(canvasPt)) return false;
    auto& state = canvas.pd->getMcp()->session.approvals;
    for (auto const& hit : mcpApprovalHits) {
        bool const yes = hit.yes.contains(canvasPt);
        if (!yes && !hit.no.contains(canvasPt)) continue;
        // A chip that just appeared (or is not fully on screen) is not clickable yet: same 400 ms as Enter.
        if (state.clickAllowed(hit.nonce.toStdString(), nowMs())) answerApproval(hit.nonce, yes);
        else repaintApprovals(); // expired but still on the glass: ignored, and repainted away
        break;
    }
    return true; // a click on the stack never falls through to the canvas
}

std::optional<bool> CanvasMcpLayer::handleApprovalKey(juce::KeyPress const& key)
{
    if (!canvas.pd || !isApprovalRequested()) return std::nullopt;
    auto& state = canvas.pd->getMcp()->session.approvals;
    if (key == juce::KeyPress::returnKey) {
        // Exactly one pending record, its chip on screen long enough (M5); else Enter does nothing here. Enter never
        // touches flow mode (P12a.1: only a click on Alphi does).
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
    auto const now = nowMs();
    auto const chips = state.visible(now);
    if (chips.empty()) {
        if (mcpApprovalExpiryTimer) mcpApprovalExpiryTimer->stopTimer();
        return;
    }
    // The next C++ expiry repaints the canvas, so an expired chip does not stay drawn until some other repaint.
    if (auto const next = state.nextExpiryMs()) {
        if (!mcpApprovalExpiryTimer) mcpApprovalExpiryTimer = std::make_unique<ApprovalExpiryTimer>(this);
        mcpApprovalExpiryTimer->startTimer(static_cast<int>(std::clamp<std::int64_t>(*next - now + 5, 5, 60 * 60 * 1000)));
    }

    // The visible viewport in canvas coordinates (the space objects and the pill are drawn in). getViewArea() already
    // is that (view position and size divided by the zoom): used as it is, never divided by the zoom again.
    mcp::ApprovalRect view = toRect(canvas.getLocalBounds());
    if (canvas.viewport) {
        auto const a = canvas.viewport->getViewArea();
        view = mcp::approvalVisibleArea(a.getX(), a.getY(), a.getWidth(), a.getHeight());
    }
    // Server-built ghost preview: outline the targets of the records that are about THIS canvas (P12c review 5: a
    // record about another tab outlines nothing here; its chip says "(on tab X)").
    auto const thisCanvas = reinterpret_cast<std::uintptr_t>(canvas.patch.getRawPointer());
    {
        std::set<std::string> targets;
        bool whole = false;
        for (auto const& c : chips) {
            if (c.canvasId == 0 || c.canvasId != thisCanvas) continue;
            for (auto const& t : c.targets) {
                if (t == "*") whole = true;
                else targets.insert(t);
            }
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

    int const shown = std::min<int>(static_cast<int>(chips.size()), M::maxBars);
    int const more = static_cast<int>(chips.size()) - shown;
    // Line 1 of each chip (fixed words; which tab only as "another tab"), and a bar wide enough for every line 1 at
    // the minimum font: line 1 is never cut and never overflows (round 3 item 4). Measured in design pixels.
    auto titleOf = [&](mcp::ApprovalChip const& c) {
        auto t = c.title;
        if (c.canvasId != 0 && c.canvasId != thisCanvas)
            t += " (on another tab)";
        else if (c.canvasId == 0 && c.canvas != "hardware" && c.canvas != "snapshots")
            t += " (tab not open)";
        return t;
    };
    int barW = M::w;
    {
        NVGScopedState measure(nvg);
        mcpnvg::nvgFontFace(nvg, "Inter");
        mcpnvg::nvgFontSize(nvg, M::minTitleFont);
        float b[4] {};
        for (int i = 0; i < shown; ++i) {
            auto const t = titleOf(chips[static_cast<std::size_t>(i)]);
            barW = std::max(barW, static_cast<int>(std::ceil(mcpnvg::nvgTextBounds(nvg, 0, 0, t.c_str(), nullptr, b))) + 2 * M::textX + 4);
        }
    }
    // A fixed SCREEN size (design px / zoom in canvas units), shrunk only when the window cannot hold it, clamped
    // inside the view. The full stack's space is reserved when it fits: a new chip never moves the ones on screen.
    // P12a.1: laid out around Alphi (top left), so a ✓ never sits on the flow switch.
    float const zoom = canvas.viewport ? canvas.viewport->getViewScale() : getValue<float>(canvas.zoomScale);
    auto const layout = mcp::approvalStackLayout(anchor, view, zoom, barW, shown, more > 0, alphiKeepOut(view, zoom));
    if (layout.bars.empty()) return;
    auto const& place = layout.frame;
    mcpApprovalFrame = juce::Rectangle<int>(place.x, place.y, place.w, place.h);
    float const k = layout.scale;

    int const half = mcp::approvalButtonW(barW);
    float const btnH = static_cast<float>(M::btnH);
    for (int i = 0; i < shown; ++i) {
        // Oldest first: new questions are appended below, nothing already on screen moves.
        auto const& c = chips[static_cast<std::size_t>(i)];
        auto const& bl = layout.bars[static_cast<std::size_t>(i)];
        auto toJ = [](mcp::ApprovalRect const& r) { return juce::Rectangle<int>(r.x, r.y, r.w, r.h); };
        ApprovalHit hit;
        hit.nonce = juce::String(c.nonce);
        hit.frame = toJ(bl.frame);
        hit.yes = toJ(bl.yes);
        hit.no = toJ(bl.no);
        mcpApprovalHits.push_back(hit);
        // Its on-screen time (Enter and clicks wait 400 ms) starts only when the whole bar is inside the view.
        // Per chip, shared across views: this view's entry only (round 3 items 3, 6); a moved bar starts again.
        state.markPaintedIfInside(c.nonce, reinterpret_cast<std::uintptr_t>(this), bl.frame, view, nowMs());

        NVGScopedState scopedAppr(nvg);
        nanovg::nvgTranslate(nvg, static_cast<float>(bl.frame.x), static_cast<float>(bl.frame.y));
        nanovg::nvgScale(nvg, k, k); // from here on: design pixels
        nanovg::nvgDrawRoundedRect(nvg, 0, 0, static_cast<float>(barW), static_cast<float>(M::h),
            nanovg::nvgRGBA(28, 16, 16, 250), nanovg::nvgRGBA(255, 90, 70, 225), 9.0f);
        mcpnvg::nvgFontFace(nvg, "Inter");
        mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        // Line 1: the server's fixed words, never cut: the font shrinks until it fits (P12c review 5).
        auto const title = titleOf(c); // fixed words only; the tab's name is on line 2 (server-built)
        float const textW = static_cast<float>(barW - 2 * M::textX);
        float size = 12.0f;
        float bounds[4] {};
        for (; size > M::minTitleFont; size -= 0.5f) {
            mcpnvg::nvgFontSize(nvg, size);
            if (mcpnvg::nvgTextBounds(nvg, 0, 0, title.c_str(), nullptr, bounds) <= textW) break;
        }
        mcpnvg::nvgFontSize(nvg, std::max(size, M::minTitleFont)); // the bar is wide enough for line 1 at this size
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 228, 220, 250));
        mcpnvg::nvgText(nvg, static_cast<float>(M::textX), M::line1Y, title.c_str(), nullptr);
        // Line 2: the model-chosen names; the only line cut (ellipsis).
        if (!c.detail.empty()) {
            mcpnvg::nvgFontSize(nvg, 11.0f);
            auto const line2 = mcp::approvalEllipsize(c.detail, [&](std::string const& t) {
                return mcpnvg::nvgTextBounds(nvg, 0, 0, t.c_str(), nullptr, bounds) <= textW;
            });
            nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 190, 175, 210));
            mcpnvg::nvgText(nvg, static_cast<float>(M::textX), M::line2Y, line2.c_str(), nullptr);
        }
        mcpnvg::nvgFontSize(nvg, 14.0f);
        mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        float const yesX = static_cast<float>(M::btnX);
        float const noX = static_cast<float>(M::btnX + half + M::btnGap);
        float const btnY = static_cast<float>(M::btnY);
        nanovg::nvgDrawRoundedRect(nvg, yesX, btnY, static_cast<float>(half), btnH,
            nanovg::nvgRGBA(40, 170, 90, 235), nanovg::nvgRGBA(90, 225, 140, 255), 6.0f);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 255, 255, 255));
        mcpnvg::nvgText(nvg, yesX + half * 0.5f, btnY + btnH * 0.5f, juce::String::fromUTF8("\xe2\x9c\x93 Approve").toRawUTF8(), nullptr);
        nanovg::nvgDrawRoundedRect(nvg, noX, btnY, static_cast<float>(half), btnH,
            nanovg::nvgRGBA(40, 44, 52, 235), nanovg::nvgRGBA(90, 98, 112, 255), 6.0f);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(210, 220, 235, 235));
        mcpnvg::nvgText(nvg, noX + half * 0.5f, btnY + btnH * 0.5f, juce::String::fromUTF8("\xe2\x9c\x97 Cancel").toRawUTF8(), nullptr);
    }
    if (more > 0) {
        NVGScopedState scopedMore(nvg);
        nanovg::nvgTranslate(nvg, static_cast<float>(place.x), static_cast<float>(place.y + place.h));
        nanovg::nvgScale(nvg, k, k);
        mcpnvg::nvgFontFace(nvg, "Inter");
        mcpnvg::nvgFontSize(nvg, 12.0f);
        mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 200, 190, 230));
        auto const line = "+" + juce::String(more) + " more waiting";
        mcpnvg::nvgText(nvg, barW * 0.5f, -M::more * 0.5f, line.toRawUTF8(), nullptr);
    }
}
