/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Alphi drop 1 (docs/plans/alphi-drop1-alive-steps.md in the MCP repo): what makes Alphi alive on one canvas, drawn
// from what really happens. Display only.
//  - D2/D3 the pulse: the eyes follow the output ceiling's published block mean square (after the breaker gain), with
//    a 30 ms attack / 150 ms release; calm = nothing measured (McpAlphi.h alphiPulse).
//  - D5 the gaze toward the selection (McpAlphi.h alphiGaze).
//  - the activity slot of session.alphiSay: sparks while building, dots while measuring; the line slot: the strip
//    beside the face (D6, McpAlphiStrip.h) and the 2 s dizzy spiral of a failed final (D8, AlphiLineClock).
//  - D4 the motion timer (~30 Hz): runs only while Alphi is drawn and something moves, repaints only his rect and
//    strip (alphiMotionArea; the paint then skips the dock pass, N3), and repaints when a say's ttl runs out (step W
//    repaints only on a new say).
//  - D11 the edge tab's drawing (its click is CanvasMcpFlow.cpp's).
// This file never touches the patch, the Pd lock, flow mode, Alphi's click gate or the dock, and never names the flow
// switch or the click on Alphi (scripts/test-bridge-safety.sh). It only reads the say board.

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
#include "Mcp/McpAlphi.h"
#include "Mcp/McpAlphiMotion.h"
#include "Mcp/McpAlphiSay.h"
#include "Mcp/McpAlphiStrip.h"

#include <cmath>

namespace {

constexpr float alphiPi = 3.14159265358979f;

mcp::ApprovalRect asRect(juce::Rectangle<int> const& j) { return { j.getX(), j.getY(), j.getWidth(), j.getHeight() }; }

NVGcolor colourOf(mcp::AlphiRgba c, float alpha = 1.0f)
{
    return nanovg::nvgRGBA(c.r, c.g, c.b, static_cast<unsigned char>(juce::jlimit(0.0f, 255.0f, c.a * alpha)));
}

// The strip's text colour per say kind.
mcp::AlphiRgba stripColour(mcp::AlphiSayKind kind)
{
    switch (kind) {
    case mcp::AlphiSayKind::Fail:
        return { 255, 150, 130, 255 };
    case mcp::AlphiSayKind::Busy:
        return { 170, 182, 200, 255 };
    case mcp::AlphiSayKind::Plain:
    default:
        return { 230, 232, 238, 255 };
    }
}

} // namespace

void CanvasMcpLayer::alphiMotionSample(std::int64_t nowMs)
{
    if (!canvas.pd)
        return;
    auto* host = canvas.pd->getMcp();
    // The pulse: a new block from the ceiling is fresh; no new block for 200 ms is calm (alphiPulse).
    auto const pulse = host->session.outputCeiling.readPulse();
    if (pulse.blocks != mcpAlphiPulseBlocks) {
        mcpAlphiPulseBlocks = pulse.blocks;
        mcpAlphiPulseFreshMs = nowMs;
    }
    auto const look = mcp::alphiLook(host->session.flowMode.isOn());
    mcpAlphiPulseTarget = mcp::alphiPulse(pulse.meanSquare, mcpAlphiPulseFreshMs > 0 ? nowMs - mcpAlphiPulseFreshMs : -1, look).level;
    float const dt = mcpAlphiPulseSampleMs > 0 ? static_cast<float>(nowMs - mcpAlphiPulseSampleMs) : 0.0f;
    mcpAlphiPulseLevel = mcp::alphiPulseFollow(mcpAlphiPulseLevel, mcpAlphiPulseTarget, dt);
    mcpAlphiPulseSampleMs = nowMs;
    // The line slot's clock (dizzy, fade).
    mcpAlphiLineClock.observe(host->session.alphiSay.line(nowMs), nowMs);
}

mcp::AlphiEyes CanvasMcpLayer::alphiEyesNow(mcp::AlphiLook const& look)
{
    mcpAlphiPulseDrawn = mcpAlphiPulseLevel;
    return mcp::alphiEyesAt(mcpAlphiPulseLevel, look);
}

mcp::AlphiGaze CanvasMcpLayer::alphiGazeNow(mcp::ApprovalRect face) const
{
    // The selection's visible bounds (canvas coordinates), read on the message thread; no Pd lock.
    std::optional<mcp::ApprovalRect> sel;
    juce::Rectangle<int> bounds;
    for (auto* o : canvas.getSelectionOfType<Object>())
        if (o && o->isVisible())
            bounds = bounds.isEmpty() ? o->getBounds().reduced(Object::margin) : bounds.getUnion(o->getBounds().reduced(Object::margin));
    if (!bounds.isEmpty())
        sel = asRect(bounds);
    return mcp::alphiGaze(face, sel);
}

void CanvasMcpLayer::paintAlphiDizzyEye(NVGcontext* nvg, float x, float y, mcp::AlphiRgba colour, std::int64_t nowMs)
{
    // D8: a small spiral per eye, turning, for the 2 s after a failed final (design px, inside the face transform).
    float const rot = mcpAlphiLineClock.spiralTurns(nowMs) * 2.0f * alphiPi;
    nanovg::nvgBeginPath(nvg);
    constexpr int steps = 40;
    for (int i = 0; i <= steps; ++i) {
        float const t = static_cast<float>(i) / steps * 2.25f; // turns
        float const r = 0.4f + t * 1.6f;
        float const a = t * 2.0f * alphiPi + rot;
        float const px = x + std::cos(a) * r, py = y + std::sin(a) * r;
        if (i == 0)
            nanovg::nvgMoveTo(nvg, px, py);
        else
            nanovg::nvgLineTo(nvg, px, py);
    }
    nanovg::nvgStrokeColor(nvg, colourOf(colour));
    nanovg::nvgStrokeWidth(nvg, 1.1f);
    nanovg::nvgStroke(nvg);
}

void CanvasMcpLayer::paintAlphiFx(NVGcontext* nvg, std::int64_t nowMs)
{
    // Design px, inside the face transform (48 x 56). Only the activity slot drives it.
    if (!canvas.pd)
        return;
    auto const act = canvas.pd->getMcp()->session.alphiSay.activity(nowMs);
    if (!act)
        return;
    float const cx = mcp::AlphiMetrics::w * 0.5f;
    float const t = static_cast<float>(nowMs % 100000) / 1000.0f;
    if (act->phase == mcp::AlphiSayPhase::Building) {
        // Sparks around the antenna tip.
        for (int i = 0; i < 4; ++i) {
            float const a = t * 4.0f + static_cast<float>(i) * alphiPi * 0.5f;
            float const r = 6.5f + 1.5f * std::sin(t * 9.0f + static_cast<float>(i));
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgCircle(nvg, cx + std::cos(a) * r, 4.5f + std::sin(a) * r * 0.6f, 1.1f);
            nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 214, 90, 230));
            nanovg::nvgFill(nvg);
        }
    } else if (act->phase == mcp::AlphiSayPhase::Measuring) {
        // Three dots on the chin, lit in turn.
        int const lit = static_cast<int>(t * 3.0f) % 3;
        for (int i = 0; i < 3; ++i) {
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgCircle(nvg, cx - 6.0f + static_cast<float>(i) * 6.0f, 51.0f, 1.3f);
            nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(120, 200, 255, i == lit ? 255 : 90));
            nanovg::nvgFill(nvg);
        }
    }
}

void CanvasMcpLayer::paintAlphiStrip(NVGcontext* nvg)
{
    // Review M: the strip's hit frame follows only a render that redrew the old and the new strip; otherwise the drawn
    // one stays and one more repaint of both spots is asked (alphiStripAfterPaint).
    auto const prev = asRect(mcpAlphiStripFrame);
    auto const shown = paintAlphiStripNow(nvg);
    auto const u = mcp::alphiStripAfterPaint(prev, shown, asRect(mcpAlphiStripRegion));
    mcpAlphiStripFrame = juce::Rectangle<int>(u.frame.x, u.frame.y, u.frame.w, u.frame.h);
    if (!u.repaint)
        return;
    auto area = mcp::alphimotion::unite(prev, shown);
    if (area.w <= 0 || area.h <= 0)
        return;
    juce::Rectangle<int> const r = juce::Rectangle<int>(area.x, area.y, area.w, area.h).expanded(2);
    canvas.repaint(r);
    if (canvas.editor)
        canvas.editor->nvgSurface.invalidateArea(canvas.editor->nvgSurface.getLocalArea(&canvas, r));
}

mcp::ApprovalRect CanvasMcpLayer::paintAlphiStripNow(NVGcontext* nvg)
{
    if (mcpAlphiFrame.isEmpty() || !canvas.pd || !canvas.viewport || !alphiShown())
        return {};
    auto const now = mcp::alphiSayNowMs();
    auto const& board = canvas.pd->getMcp()->session.alphiSay;
    auto const line = board.line(now);
    auto const act = board.activity(now);
    float const lineAlpha = line ? mcpAlphiLineClock.lineAlpha(now) : 0.0f;
    // What this paint shows, for the motion timer (a change, an expiry: one more repaint).
    mcpAlphiSayKeyDrawn = mcp::alphiSayKey(board, now);
    mcpAlphiDizzyDrawn = mcpAlphiLineClock.dizzy(now);
    mcpAlphiLineAlphaDrawn = lineAlpha;

    std::string text;
    mcp::AlphiRgba colour {};
    float alpha = 1.0f;
    if (line && lineAlpha > 0.0f) {
        text = line->text;
        colour = stripColour(line->kind);
        alpha = lineAlpha;
    } else if (act) {
        text = act->text;
        colour = stripColour(mcp::AlphiSayKind::Busy);
    }
    if (text.empty())
        return {};

    auto const [view, zoom] = alphiView();
    auto const face = asRect(mcpAlphiFrame);
    mcp::AlphiStripInput in;
    in.strip = mcp::alphiStripRect(face, mcpAlphiFaceCorner, view, zoom);
    in.view = view;
    in.zoom = zoom;
    in.mouse = alphiMouse();
    in.faceShown = true;
    // Never a dock input, never a cover, never in the chips' keep-out: it only reads them (N1).
    if (!mcp::alphiStripShown(in, mcpAlphiPaintBoxes, alphiDockCovers()))
        return {};
    auto const& s = in.strip;

    using S = mcp::AlphiStripMetrics;
    auto const lines = mcp::alphiStripLines(text);
    if (lines.empty())
        return {};
    float const k = static_cast<float>(s.w) / static_cast<float>(S::w); // canvas units per design px
    NVGScopedState scoped(nvg);
    nanovg::nvgTranslate(nvg, static_cast<float>(s.x), static_cast<float>(s.y));
    nanovg::nvgScale(nvg, k, k);
    mcpnvg::nvgFontFace(nvg, "Inter");
    mcpnvg::nvgFontSize(nvg, 11.0f);
    mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    float textW = 0.0f;
    for (auto const& l : lines) {
        float b[4] {};
        mcpnvg::nvgTextBounds(nvg, 0.0f, 0.0f, l.c_str(), nullptr, b);
        textW = std::max(textW, b[2] - b[0]);
    }
    float const bgW = juce::jlimit(40.0f, static_cast<float>(S::w), textW + 20.0f);
    bool const rightSide = mcpAlphiFaceCorner == mcp::AlphiCorner::bottomRight || mcpAlphiFaceCorner == mcp::AlphiCorner::topRight;
    float const bgX = rightSide ? static_cast<float>(S::w) - bgW : 0.0f; // hugs the face's side
    nanovg::nvgBeginPath(nvg);
    nanovg::nvgRoundedRect(nvg, bgX, 0.0f, bgW, static_cast<float>(S::h), 8.0f);
    nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(18, 20, 26, static_cast<unsigned char>(215.0f * alpha)));
    nanovg::nvgFill(nvg);
    nanovg::nvgFillColor(nvg, colourOf(colour, alpha));
    float const lineH = 14.0f;
    float const y0 = static_cast<float>(S::h) * 0.5f - (static_cast<float>(lines.size()) - 1.0f) * lineH * 0.5f;
    for (std::size_t i = 0; i < lines.size(); ++i)
        mcpnvg::nvgText(nvg, bgX + 10.0f, y0 + static_cast<float>(i) * lineH, lines[i].c_str(), nullptr);
    return s;
}

void CanvasMcpLayer::paintAlphiTab(NVGcontext* nvg)
{
    // D11: the face's left part peeking from behind the right edge, one eye showing (design px 28 x 56).
    auto const frame = mcpAlphiTabUnder ? mcpAlphiTabFrameUnder : mcpAlphiTabFrame;
    if (frame.isEmpty() || !canvas.pd)
        return;
    using T = mcp::AlphiTabMetrics;
    float const k = static_cast<float>(frame.getWidth()) / static_cast<float>(T::w);
    auto const look = mcp::alphiLook(canvas.pd->getMcp()->session.flowMode.isOn());
    NVGScopedState scoped(nvg);
    nanovg::nvgTranslate(nvg, static_cast<float>(frame.getX()), static_cast<float>(frame.getY()));
    nanovg::nvgScale(nvg, k, k);
    nanovg::nvgScissor(nvg, 0.0f, 0.0f, static_cast<float>(T::w), static_cast<float>(T::h));
    nanovg::nvgDrawRoundedRect(nvg, 2.0f, 10.0f, 44.0f, 44.0f, nanovg::nvgRGBA(242, 241, 236, 255), nanovg::nvgRGBA(196, 196, 190, 255), 12.0f);
    nanovg::nvgDrawRoundedRect(nvg, 8.0f, 17.0f, 32.0f, 21.0f, nanovg::nvgRGBA(24, 26, 31, 255), nanovg::nvgRGBA(10, 11, 14, 255), 6.0f);
    nanovg::nvgBeginPath(nvg);
    if (look.sleepy)
        nanovg::nvgRoundedRect(nvg, 17.0f - look.eyeRadius * 1.3f, 26.4f, look.eyeRadius * 2.6f, 2.2f, 1.1f);
    else
        nanovg::nvgCircle(nvg, 17.0f, 27.5f, look.eyeRadius);
    nanovg::nvgFillColor(nvg, colourOf(look.eye));
    nanovg::nvgFill(nvg);
}

void CanvasMcpLayer::alphiMotionRepaint(bool sayChanged)
{
    if (mcpAlphiFrame.isEmpty() || !canvas.viewport)
        return;
    auto const [view, zoom] = alphiView();
    auto const face = asRect(mcpAlphiFrame);
    // Review L6: the strip's spot only while a strip is drawn or the say board changed.
    auto const spot = mcp::alphiStripRect(face, mcpAlphiFaceCorner, view, zoom);
    auto const area = mcp::alphiMotionRepaintArea(face, spot, !mcpAlphiStripFrame.isEmpty(), sayChanged, zoom);
    juce::Rectangle<int> const r(area.x, area.y, area.w, area.h);
    mcpAlphiMotionAsked = true; // N3: this paint is Alphi's own (CanvasMcpLayer::alphiDockPassNeeded)
    canvas.repaint(r);
    if (canvas.editor)
        canvas.editor->nvgSurface.invalidateArea(canvas.editor->nvgSurface.getLocalArea(&canvas, r));
}

void CanvasMcpLayer::alphiMotionStop()
{
    if (mcpAlphiMotionTimer)
        mcpAlphiMotionTimer->stopTimer();
}

void CanvasMcpLayer::alphiMotionWake()
{
    // The 10 Hz tick (CanvasMcpFlow.cpp): wake the motion timer when the ceiling published a new block, the level
    // crossed -60 dBFS, the say board changed (a new say or an expiry), the line moves, or the gaze moved.
    if (mcpAlphiFrame.isEmpty() || !canvas.pd) {
        alphiMotionStop();
        return;
    }
    auto const now = mcp::alphiSayNowMs();
    alphiMotionSample(now);
    auto const& board = canvas.pd->getMcp()->session.alphiSay;
    auto const gaze = alphiGazeNow(asRect(mcpAlphiFrame));
    bool const gazeMoved = std::fabs(gaze.dx - mcpAlphiGazeDrawn.dx) > 0.25f || std::fabs(gaze.dy - mcpAlphiGazeDrawn.dy) > 0.25f;
    bool const wake = mcp::alphiPulseWake(mcpAlphiPulseDrawn, mcpAlphiPulseLevel)
        || mcp::alphiSayKey(board, now) != mcpAlphiSayKeyDrawn || mcpAlphiLineClock.moving(now)
        || mcpAlphiLineClock.dizzy(now) != mcpAlphiDizzyDrawn || mcpAlphiLineClock.lineAlpha(now) != mcpAlphiLineAlphaDrawn
        || board.activity(now).has_value() || gazeMoved;
    if (!wake)
        return;
    if (!mcpAlphiMotionTimer)
        mcpAlphiMotionTimer = std::make_unique<AlphiMotionTimer>(this);
    if (!mcpAlphiMotionTimer->isTimerRunning())
        mcpAlphiMotionTimer->startTimer(mcp::AlphiMotionTiming::intervalMs);
}

void CanvasMcpLayer::alphiMotionTick()
{
    if (!canvas.pd || !alphiShown() || !canvas.isShowing() || mcpAlphiFrame.isEmpty()) {
        alphiMotionStop();
        return;
    }
    auto const now = mcp::alphiSayNowMs();
    alphiMotionSample(now);
    auto const& board = canvas.pd->getMcp()->session.alphiSay;
    mcp::AlphiMotionInput in;
    in.shown = true;
    in.covered = alphiCoveredNow(mcpAlphiFrame);
    in.target = mcpAlphiPulseLevel;
    in.drawn = mcpAlphiPulseDrawn;
    in.busy = board.activity(now).has_value();
    in.lineMoving = mcpAlphiLineClock.moving(now);
    auto const gaze = alphiGazeNow(asRect(mcpAlphiFrame));
    bool const gazeMoved = std::fabs(gaze.dx - mcpAlphiGazeDrawn.dx) > 0.25f || std::fabs(gaze.dy - mcpAlphiGazeDrawn.dy) > 0.25f;
    bool const sayChanged = mcp::alphiSayKey(board, now) != mcpAlphiSayKeyDrawn || mcpAlphiLineClock.dizzy(now) != mcpAlphiDizzyDrawn
        || mcpAlphiLineClock.lineAlpha(now) != mcpAlphiLineAlphaDrawn;
    if (!in.covered && (mcp::alphiMotionRun(in) || sayChanged || gazeMoved)) {
        // While busy only the sparks / dots move: at most busyRepaintMs apart (~12 Hz), and the measuring dots only
        // when the lit dot changes. The pulse, the line, the say and the gaze still repaint at the full rate.
        bool const onlyBusy = in.busy && !sayChanged && !gazeMoved && !in.lineMoving
            && std::fabs(in.target - in.drawn) < mcp::AlphiMotionTiming::minStep && !(in.target <= 0.0f && in.drawn > 0.0f);
        if (onlyBusy) {
            auto const act = board.activity(now);
            int const key = act && act->phase == mcp::AlphiSayPhase::Measuring
                ? static_cast<int>(static_cast<float>(now % 100000) / 1000.0f * 3.0f) % 3 // paintAlphiFx's lit dot
                : -1;
            bool const dotsSame = key >= 0 && key == mcpAlphiBusyKeyDrawn;
            if (dotsSame || now - mcpAlphiBusyRepaintMs < busyRepaintMs)
                return;
            mcpAlphiBusyKeyDrawn = key;
            mcpAlphiBusyRepaintMs = now;
        }
        mcpAlphiGazeDrawn = gaze;
        alphiMotionRepaint(sayChanged);
        return;
    }
    // Nothing to draw: keep following while the level still moves toward the ceiling's value, else stop (the 10 Hz
    // tick wakes it again).
    if (in.covered || std::fabs(mcpAlphiPulseLevel - mcpAlphiPulseTarget) < mcp::AlphiMotionTiming::minStep)
        alphiMotionStop();
}
