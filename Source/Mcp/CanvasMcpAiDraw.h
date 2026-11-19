/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/
#pragma once

// Small drawing helpers for the AI overlays (design round 2026-11-19): they turn the one style table (McpAiStyle.h)
// into NanoVG calls, so the canvas layer, the pill and the card draw the same shapes. Header-only, message thread
// (they record into the async NanoVG context like every other overlay paint). Nothing here starts a timer or asks for a
// repaint: they only draw.

#include <juce_gui_basics/juce_gui_basics.h>
#include "Utility/NVGUtils.h"
#include "Utility/NVGTextShim.h"
#include "Mcp/McpAiStyle.h"

namespace mcpai {

/** An AiRgba as a NanoVG colour; `alphaScale` multiplies its alpha (a ghost is 0.6). */
inline NVGcolor colour(mcp::AiRgba c, float alphaScale = 1.0f)
{
    return nanovg::nvgRGBA(c.r, c.g, c.b, static_cast<unsigned char>(juce::jlimit(0.0f, 255.0f, static_cast<float>(c.a) * alphaScale + 0.5f)));
}

/** A solid line. */
inline void strokeOutline(NVGcontext* nvg, mcp::AiRgba c, float width = mcp::aiLineWidth)
{
    nanovg::nvgStrokeColor(nvg, colour(c));
    nanovg::nvgStrokeWidth(nvg, width);
    nanovg::nvgStroke(nvg);
}

/** The shape a tag kind stands for, centred on (cx, cy):
 *  Fact = filled blue square, Guess = outline blue square, Changed = filled red circle, Proposed = outline red circle,
 *  Look = yellow triangle, Ask = half circle (the left half filled, ink), Step = ink disc with a cream number
 *  (`number` >= 1). None draws nothing. The size is mcp::aiGlyphSize(tag). */
inline void drawGlyph(NVGcontext* nvg, mcp::AiTag tag, float cx, float cy, int number = 0)
{
    using mcp::AiTag;
    float const size = mcp::aiGlyphSize(tag);
    if (size <= 0.0f)
        return;
    float const half = size * 0.5f;
    NVGScopedState scoped(nvg);
    switch (tag) {
    case AiTag::Fact:
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgRect(nvg, cx - half, cy - half, size, size);
        nanovg::nvgFillColor(nvg, colour(mcp::aiBlue));
        nanovg::nvgFill(nvg);
        break;
    case AiTag::Guess:
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgRect(nvg, cx - half + 0.5f, cy - half + 0.5f, size - 1.0f, size - 1.0f);
        strokeOutline(nvg, mcp::aiBlue);
        break;
    case AiTag::Changed:
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgCircle(nvg, cx, cy, half);
        nanovg::nvgFillColor(nvg, colour(mcp::aiRed));
        nanovg::nvgFill(nvg);
        break;
    case AiTag::Proposed:
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgCircle(nvg, cx, cy, half - 0.5f);
        strokeOutline(nvg, mcp::aiRed);
        break;
    case AiTag::Look:
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgMoveTo(nvg, cx, cy - half);
        nanovg::nvgLineTo(nvg, cx + half, cy + half - 0.5f);
        nanovg::nvgLineTo(nvg, cx - half, cy + half - 0.5f);
        nanovg::nvgClosePath(nvg);
        nanovg::nvgFillColor(nvg, colour(mcp::aiYellow));
        nanovg::nvgFill(nvg);
        nanovg::nvgStrokeColor(nvg, colour(mcp::aiInk, 0.6f));
        nanovg::nvgStrokeWidth(nvg, mcp::aiLineWidth);
        nanovg::nvgStroke(nvg);
        break;
    case AiTag::Ask:
        // the left half filled (an arc from the bottom, clockwise, through the left, to the top), the whole circle outlined
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgArc(nvg, cx, cy, half - 0.5f, juce::MathConstants<float>::halfPi, juce::MathConstants<float>::pi * 1.5f, NVG_HOLE); // NVG_HOLE sweeps clockwise in this nanovg
        nanovg::nvgClosePath(nvg);
        nanovg::nvgFillColor(nvg, colour(mcp::aiInk));
        nanovg::nvgFill(nvg);
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgCircle(nvg, cx, cy, half - 0.5f);
        strokeOutline(nvg, mcp::aiInk);
        break;
    case AiTag::Step:
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgCircle(nvg, cx, cy, half);
        nanovg::nvgFillColor(nvg, colour(mcp::aiInk));
        nanovg::nvgFill(nvg);
        if (number > 0) {
            mcpnvg::nvgFontFace(nvg, "Inter-Medium");
            mcpnvg::nvgFontSize(nvg, number > 9 ? 8.0f : 9.5f);
            mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nanovg::nvgFillColor(nvg, colour(mcp::aiCream));
            mcpnvg::nvgText(nvg, cx, cy + 0.5f, juce::String(number).toRawUTF8(), nullptr);
        }
        break;
    case AiTag::None:
        break;
    }
}

/** The one mark of a card (the same shapes and sizes as the tags): empty ring, red dot, blue square, yellow triangle. */
inline void drawCardMark(NVGcontext* nvg, mcp::AiCardMark mark, float cx, float cy)
{
    using mcp::AiCardMark;
    using mcp::AiTag;
    switch (mark) {
    case AiCardMark::Ring: {
        NVGScopedState scoped(nvg);
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgCircle(nvg, cx, cy, mcp::aiTagGlyphSize * 0.5f - 0.5f);
        strokeOutline(nvg, mcp::aiInk);
        break;
    }
    case AiCardMark::RedDot: drawGlyph(nvg, AiTag::Changed, cx, cy); break;
    case AiCardMark::BlueSquare: drawGlyph(nvg, AiTag::Fact, cx, cy); break;
    case AiCardMark::YellowTriangle: drawGlyph(nvg, AiTag::Look, cx, cy); break;
    case AiCardMark::None: break;
    }
}

/** Text with a halo: the text eight times at 1 px offsets in `halo` (the canvas background), then once in `ink`, so it
 *  reads where it crosses a wire. (x, y) is the left of the text, vertically centred; `face` and `size` as the shim. */
inline void drawHaloText(NVGcontext* nvg, float x, float y, char const* text, NVGcolor halo, NVGcolor ink, char const* face = "Inter",
    float size = mcp::aiTagTextSize)
{
    mcpnvg::nvgFontFace(nvg, face);
    mcpnvg::nvgFontSize(nvg, size);
    mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nanovg::nvgFillColor(nvg, halo);
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
            if (dx != 0 || dy != 0)
                mcpnvg::nvgText(nvg, x + static_cast<float>(dx), y + static_cast<float>(dy), text, nullptr);
    nanovg::nvgFillColor(nvg, ink);
    mcpnvg::nvgText(nvg, x, y, text, nullptr);
}

} // namespace mcpai
