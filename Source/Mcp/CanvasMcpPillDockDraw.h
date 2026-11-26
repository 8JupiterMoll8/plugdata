/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/
#pragma once

// The look of the docked selection pill (night/pill-docked), the owner's picture "ai-midi5 v16, the pill with references": a white, fully
// rounded pill with a thin ink outline (and a faint offset shadow), a small ink dot at the left, then CHIPS: the subject chip is paper with a
// 2 px yellow outline and a small red rounded note glyph in front of its label, chips are joined by a small "+". The typed words and the caret
// are the prompt field itself (a JUCE TextEditor drawn on top, see CanvasMcpPill.cpp). Geometry numbers come from McpPillDock.h, colours from
// McpAiStyle.h. Header-only, message thread, drawing only: like CanvasMcpAiDraw.h it starts no timer and asks for no repaint.

#include <juce_gui_basics/juce_gui_basics.h>
#include "Mcp/CanvasMcpAiDraw.h"
#include "Mcp/McpPillDock.h"

namespace mcpai {

constexpr float dockChipH = 16.0f;
constexpr float dockChipTextSize = 10.5f;
constexpr float dockPillWhite = 255.0f;
constexpr float dockRadiusMax = 12.0f; // fully rounded on the one-line strip (24 px), a rounded drawer when it has grown

/** The subject chip's label width at its font, in px (the same metrics the chip is drawn with). Message thread. */
inline float dockSubjectTextWidth(juce::String const& subject)
{
    mcpnvg::TextState ts;
    ts.size = dockChipTextSize;
    ts.face = "Inter-Medium";
    return mcpnvg::textWidth(mcpnvg::fontFor(ts), subject);
}

/** The pill, in its own coordinates (the caller translated to the frame's top-left). `w`, `h`: the frame; `rowH`: the strip's own height, the
 *  first row: the dot and the chips are centred in it however far the pill has grown. `subject` empty = no chip. `voiceLevel` < 0: no
 *  recording rim, else the level 0..1 of the pulse. */
inline void drawDockedPill(NVGcontext* nvg, float w, float h, float rowH, juce::String const& subject, float voiceLevel)
{
    float const r = std::min(h * 0.5f, dockRadiusMax);

    // a faint offset shadow, as in the picture
    nanovg::nvgBeginPath(nvg);
    nanovg::nvgRoundedRect(nvg, 1.0f, 2.0f, w, h, r);
    nanovg::nvgFillColor(nvg, colour(mcp::aiInk, 0.18f));
    nanovg::nvgFill(nvg);

    // the body: white, a thin ink outline
    nanovg::nvgBeginPath(nvg);
    nanovg::nvgRoundedRect(nvg, 0.5f, 0.5f, w - 1.0f, h - 1.0f, r);
    nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 255, 255, 255));
    nanovg::nvgFill(nvg);
    strokeOutline(nvg, mcp::aiInk);

    // a voice / beat capture in progress: the red rim pulses with the level (as the floating pill does)
    if (voiceLevel >= 0.0f) {
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgRoundedRect(nvg, -1.0f, -1.0f, w + 2.0f, h + 2.0f, r + 1.0f);
        nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(255, 59, 48, static_cast<unsigned char>(130 + juce::jlimit(0.0f, 125.0f, voiceLevel * 350.0f))));
        nanovg::nvgStrokeWidth(nvg, 1.5f + voiceLevel * 3.5f);
        nanovg::nvgStroke(nvg);
    }

    float const midY = rowH * 0.5f;

    // the small ink dot
    nanovg::nvgBeginPath(nvg);
    nanovg::nvgCircle(nvg, 12.0f, midY, 3.0f);
    nanovg::nvgFillColor(nvg, colour(mcp::aiInk));
    nanovg::nvgFill(nvg);

    if (subject.isEmpty())
        return;

    // the subject chip: paper, a 2 px yellow outline, a red rounded note glyph, the label in ink
    float const x = static_cast<float>(mcp::kPillDockChipsX);
    float const y = midY - dockChipH * 0.5f;
    float const textW = dockSubjectTextWidth(subject);
    float const chipW = static_cast<float>(mcp::pillSubjectChipWidth(textW));
    nanovg::nvgBeginPath(nvg);
    nanovg::nvgRoundedRect(nvg, x + 1.0f, y, chipW - 2.0f, dockChipH, dockChipH * 0.5f);
    nanovg::nvgFillColor(nvg, colour(mcp::aiCream));
    nanovg::nvgFill(nvg);
    strokeOutline(nvg, mcp::aiYellow, mcp::aiLineWidthActive);

    nanovg::nvgBeginPath(nvg);
    nanovg::nvgRoundedRect(nvg, x + 6.0f, y + 5.5f, 9.0f, 5.0f, 2.5f);
    nanovg::nvgFillColor(nvg, colour(mcp::aiRed));
    nanovg::nvgFill(nvg);

    NVGScopedState scopedText(nvg);
    mcpnvg::nvgFontFace(nvg, "Inter-Medium");
    mcpnvg::nvgFontSize(nvg, dockChipTextSize);
    mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nanovg::nvgFillColor(nvg, colour(mcp::aiInk));
    mcpnvg::nvgText(nvg, x + 19.0f, midY + 0.5f, subject.toRawUTF8(), nullptr);
}

} // namespace mcpai
