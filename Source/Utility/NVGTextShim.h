/*
 // Copyright (c) 2026 PlugData MCP fork
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
 */

#pragma once

// nanovg-style text calls for the MCP overlays (fork-only).
//
// Upstream moved NanoVG to an async command recorder (nanovg_async.h) and stopped
// registering fontstash fonts: text is drawn as SDF glyphs through
// NVGGraphicsContext. The recorder has no producer for nvgText/nvgFontSize/...,
// and the bare ::nvg* text API needs the real context, which only the render
// thread may touch. The overlay code was written against the old immediate API,
// so this shim keeps those call sites, `mcpnvg::nvgText(nvg, ...)` and friends:
//
//   * font size / face / align are tracked per context on the producer (message)
//     thread, like nanovg's own state;
//   * drawing records a deferred SDF text run at the current transform, coloured
//     by the current fill colour (same as ::nvgText);
//   * measuring uses JUCE font metrics on the message thread (no GPU access).
//
// Known gap: nanovg's nvgSave/nvgRestore also save/restore the font state. This
// shim does not, so set size/face/align before each text block (the overlays do).

#include <juce_gui_basics/juce_gui_basics.h>
#include <nanovg_async.h>
#include <unordered_map>

#include "Utility/Fonts.h"
#include "Utility/NVGGraphicsContext.h"

namespace mcpnvg {

struct TextState {
    float size = 12.0f;
    juce::String face = "Inter";
    int align = NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE;
};

// Producer-thread only (the message thread records every frame).
inline TextState& state(NVGcontext* nvg)
{
    static std::unordered_map<NVGcontext*, TextState> states;
    return states[nvg];
}

inline juce::Font fontFor(TextState const& s)
{
    juce::Font font = Fonts::getDefaultFont();
    if (s.face == "Inter-Bold")
        font = Fonts::getBoldFont();
    else if (s.face == "Inter-Medium" || s.face == "Inter-SemiBold")
        font = Fonts::getSemiBoldFont();
    else if (s.face == "Inter-Tabular")
        font = Fonts::getTabularNumbersFont();
    else if (s.face.startsWith("icon_font"))
        font = Fonts::getIconFont();
    // fontstash sizes are ascent + descent in pixels, which is JUCE's font height
    return font.withHeight(juce::jmax(1.0f, s.size));
}

inline float textWidth(juce::Font const& font, juce::String const& text)
{
    return juce::GlyphArrangement().getStringWidth(font, text);
}

// Baseline y for a nanovg vertical alignment at y.
inline float baselineFor(juce::Font const& font, int align, float y)
{
    if (align & NVG_ALIGN_TOP)
        return y + font.getAscent();
    if (align & NVG_ALIGN_MIDDLE)
        return y + (font.getAscent() - font.getDescent()) * 0.5f;
    if (align & NVG_ALIGN_BOTTOM)
        return y - font.getDescent();
    return y;
}

// Left edge for a nanovg horizontal alignment at x.
inline float leftFor(int align, float x, float width)
{
    if (align & NVG_ALIGN_CENTER)
        return x - width * 0.5f;
    if (align & NVG_ALIGN_RIGHT)
        return x - width;
    return x;
}

inline void nvgFontSize(NVGcontext* nvg, float size) { state(nvg).size = size; }
inline void nvgFontFace(NVGcontext* nvg, char const* face) { state(nvg).face = juce::String::fromUTF8(face); }
inline void nvgTextAlign(NVGcontext* nvg, int align) { state(nvg).align = align; }

inline juce::String toString(char const* string, char const* end)
{
    if (string == nullptr)
        return {};
    return end ? juce::String::fromUTF8(string, static_cast<int>(end - string)) : juce::String::fromUTF8(string);
}

// Returns the x after the last glyph, like ::nvgText.
inline float nvgText(NVGcontext* nvg, float x, float y, char const* string, char const* end = nullptr)
{
    auto const& s = state(nvg);
    auto const text = toString(string, end);
    auto const font = fontFor(s);
    auto const width = textWidth(font, text);
    auto const left = leftFor(s.align, x, width);
    NVGGraphicsContext::enqueueBaselineText(nvg, font, text, { left, baselineFor(font, s.align, y) }, juce::Justification::left);
    return left + width;
}

// bounds = [xmin, ymin, xmax, ymax]; returns the advance width, like ::nvgTextBounds.
inline float nvgTextBounds(NVGcontext* nvg, float x, float y, char const* string, char const* end, float* bounds)
{
    auto const& s = state(nvg);
    auto const font = fontFor(s);
    auto const width = textWidth(font, toString(string, end));
    auto const left = leftFor(s.align, x, width);
    auto const top = baselineFor(font, s.align, y) - font.getAscent();
    if (bounds) {
        bounds[0] = left;
        bounds[1] = top;
        bounds[2] = left + width;
        bounds[3] = top + font.getHeight();
    }
    return width;
}

// Word-wrapped lines of `text` at `breakRowWidth`, as (text, width) pairs.
inline juce::Array<std::pair<juce::String, float>> wrapLines(juce::Font const& font, juce::String const& text, float breakRowWidth)
{
    juce::Array<std::pair<juce::String, float>> lines;
    for (auto const& paragraph : juce::StringArray::fromLines(text)) {
        juce::String line;
        for (auto const& word : juce::StringArray::fromTokens(paragraph, " ", "")) {
            auto const candidate = line.isEmpty() ? word : line + " " + word;
            if (line.isNotEmpty() && textWidth(font, candidate) > breakRowWidth) {
                lines.add({ line, textWidth(font, line) });
                line = word;
            } else {
                line = candidate;
            }
        }
        lines.add({ line, textWidth(font, line) });
    }
    return lines;
}

inline void nvgTextBox(NVGcontext* nvg, float x, float y, float breakRowWidth, char const* string, char const* end = nullptr)
{
    auto const& s = state(nvg);
    auto const font = fontFor(s);
    auto baseline = baselineFor(font, s.align, y);
    for (auto const& [line, width] : wrapLines(font, toString(string, end), breakRowWidth)) {
        // nanovg aligns each wrapped row horizontally inside the break width
        auto left = x;
        if (s.align & NVG_ALIGN_CENTER)
            left = x + (breakRowWidth - width) * 0.5f;
        else if (s.align & NVG_ALIGN_RIGHT)
            left = x + breakRowWidth - width;
        NVGGraphicsContext::enqueueBaselineText(nvg, font, line, { left, baseline }, juce::Justification::left);
        baseline += font.getHeight();
    }
}

inline void nvgTextBoxBounds(NVGcontext* nvg, float x, float y, float breakRowWidth, char const* string, char const* end, float* bounds)
{
    auto const& s = state(nvg);
    auto const font = fontFor(s);
    auto const lines = wrapLines(font, toString(string, end), breakRowWidth);
    float maxWidth = 0.0f;
    for (auto const& [line, width] : lines)
        maxWidth = juce::jmax(maxWidth, width);
    auto const top = baselineFor(font, s.align, y) - font.getAscent();
    if (bounds) {
        bounds[0] = x;
        bounds[1] = top;
        bounds[2] = x + maxWidth;
        bounds[3] = top + font.getHeight() * static_cast<float>(lines.size());
    }
}

} // namespace mcpnvg
