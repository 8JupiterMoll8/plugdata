/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
 */
// The Copilot panel's look (design round 2026-11-19, picture P): cream paper, one ink, colour only where it has one meaning.
// Every value is the one AI style table (Mcp/McpAiStyle.h); this header only turns it into JUCE colours and draws the few
// shapes the panel shares: the mark of a talk line (the same shapes and sizes as the canvas tags) and a dotted lead. Header
// only, message thread (paint calls only: nothing here starts a timer or asks for a repaint).
#pragma once

#include "Mcp/McpAiStyle.h"
#include "Mcp/McpCopilotTalk.h"

namespace copilotlook {

/** An AiRgba as a JUCE colour; `alphaScale` multiplies its alpha (a ghost is 0.6). */
inline juce::Colour colour(mcp::AiRgba c, float alphaScale = 1.0f)
{
    return juce::Colour(c.r, c.g, c.b, static_cast<juce::uint8>(juce::jlimit(0.0f, 255.0f, static_cast<float>(c.a) * alphaScale + 0.5f)));
}

inline juce::Colour cream() { return colour(mcp::aiCream); }
inline juce::Colour ink(float alpha = 1.0f) { return colour(mcp::aiInk, alpha); }
inline juce::Colour grey() { return colour(mcp::aiGrey); }

/** The chosen tab and a hovered card: the ink at a low alpha over the cream (a slightly darker cream, no colour of its own). */
inline juce::Colour wash(float alpha = 0.09f) { return ink(alpha); }

/** The tag a talk mark stands for (talk marks are the canvas tags: look = yellow triangle, ask = half circle, changed = red circle,
 *  explained = blue outline square (a guess tag: tags only, nothing changed), measured = blue square (a fact)). */
constexpr mcp::AiTag tagOfMark(mcp::TalkMark mark)
{
    switch (mark) {
    case mcp::TalkMark::Look: return mcp::AiTag::Look;
    case mcp::TalkMark::Ask: return mcp::AiTag::Ask;
    case mcp::TalkMark::Changed: return mcp::AiTag::Changed;
    case mcp::TalkMark::Explained: return mcp::AiTag::Guess;
    case mcp::TalkMark::Measured: return mcp::AiTag::Fact;
    // ai_widget plan, phase 3: the artist's answer to a widget proposal wears an existing shape until the owner draws its own: used = the
    // red circle (the idea is in the patch now), refused = the blue outline square (a tag, nothing changed)
    case mcp::TalkMark::Used: return mcp::AiTag::Changed;
    case mcp::TalkMark::Refused: return mcp::AiTag::Guess;
    case mcp::TalkMark::None: break;
    }
    return mcp::AiTag::None;
}

/** The mark of a talk line, centred on (cx, cy); nothing for TalkMark::None. Same shapes and size (9) as the canvas tags:
 *  look = yellow triangle with a 60 % ink outline, ask = a circle with its left half filled, in ink, changed = filled red circle,
 *  explained = blue outline square, measured = filled blue square. */
inline void drawMark(juce::Graphics& g, mcp::TalkMark mark, float cx, float cy)
{
    float const size = mcp::aiGlyphSize(tagOfMark(mark));
    if (size <= 0.0f)
        return;
    float const half = size * 0.5f;
    switch (mark) {
    case mcp::TalkMark::Changed:
    case mcp::TalkMark::Used:
        g.setColour(colour(mcp::aiRed));
        g.fillEllipse(cx - half, cy - half, size, size);
        break;
    case mcp::TalkMark::Explained:
    case mcp::TalkMark::Refused:
        g.setColour(colour(mcp::aiBlue));
        g.drawRect(cx - half + 0.5f, cy - half + 0.5f, size - 1.0f, size - 1.0f, mcp::aiLineWidth);
        break;
    case mcp::TalkMark::Measured:
        g.setColour(colour(mcp::aiBlue));
        g.fillRect(cx - half, cy - half, size, size);
        break;
    case mcp::TalkMark::Look: {
        juce::Path triangle;
        triangle.addTriangle(cx, cy - half, cx + half, cy + half - 0.5f, cx - half, cy + half - 0.5f);
        g.setColour(colour(mcp::aiYellow));
        g.fillPath(triangle);
        g.setColour(colour(mcp::aiInk, 0.6f));
        g.strokePath(triangle, juce::PathStrokeType(mcp::aiLineWidth));
        break;
    }
    case mcp::TalkMark::Ask: {
        auto const r = half - 0.5f;
        juce::Path leftHalf; // JUCE angles run clockwise from 12 o'clock: pi to 2 pi is the left half
        leftHalf.addPieSegment(cx - r, cy - r, 2.0f * r, 2.0f * r, juce::MathConstants<float>::pi, juce::MathConstants<float>::twoPi, 0.0f);
        g.setColour(colour(mcp::aiInk));
        g.fillPath(leftHalf);
        g.drawEllipse(cx - r, cy - r, 2.0f * r, 2.0f * r, mcp::aiLineWidth);
        break;
    }
    case mcp::TalkMark::None: break;
    }
}

/** The lead of a waiting line: three small grey dots in the mark column, vertically centred on `cy`. */
inline void drawDottedLead(juce::Graphics& g, float x, float cy)
{
    g.setColour(grey());
    for (int i = 0; i < 3; ++i)
        g.fillRect(x + 5.0f * static_cast<float>(i), cy - 1.0f, 2.0f, 2.0f);
}

} // namespace copilotlook
