/*
 // Copyright (c) 2021-2026 Timothy Schoen & Contributors.
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
 */
// The Copilot panel's talk tab as a notebook (design round 2026-11-19, step C3, picture P): no bubbles, no borders, no role
// labels. The artist's lines sit on the right in 14 px ink, with a small grey outlined label "on ai.tap" under a line that was
// about a widget. The AI's lines sit on the left in 14 px ink after the mark of the line (the canvas tag shapes: look = yellow
// triangle, ask = half circle; only where the software knows it, McpCopilotTalk.h talkMark), a second paragraph in 12 px
// grey. A waiting line ("Claude Code has not answered yet") is grey text after three dots, and the answer takes its place
// (the list replaces it, TalkList). An empty talk is one grey line.
//
// The lines are McpCopilotTalk.h's TalkList; this component owns it and writes it only on the message thread (CopilotPanel).
// The parsed and shaped text of each line is cached by the line's id until its text, width or the UI font changes (paint
// runs at frame rate). Colours and sizes are the one AI style table (Mcp/McpAiStyle.h) through CopilotLook.h.
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Sidebar/CopilotLook.h"
#include "Sidebar/CopilotMarkdown.h"
#include "Utility/CachedStringWidth.h"
#include "Utility/Fonts.h"

class CopilotTalkView final : public Component {
public:
    static constexpr int pad = mcp::aiPadding;       // the air at both sides
    static constexpr int markColumn = 18;            // the mark's column in front of an AI line
    static constexpr int lineGap = 16;               // between two lines
    static constexpr int topPad = 12;                // above the first line
    static constexpr float userWidthShare = 0.85f;   // an artist line takes at most this much of the width
    static constexpr float bodySize = 14.0f;         // the first paragraph of a line
    static constexpr float secondarySize = 12.0f;    // a second paragraph, the context label
    static constexpr int contextLabelH = 16;
    static constexpr int contextLabelGap = 4;
    static constexpr int thinkingRowH = 22;

    mcp::TalkList list;
    bool isThinking = false;
    juce::String thinkingStatus = "Thinking...";
    float thinkingPhase = 0.0f;

    // Bounds of the thinking row from the last paint (empty when hidden), so its animation can repaint just that area.
    juce::Rectangle<int> thinkingBadgeBounds;

    /** One chat line; the text is cleaned for the fonts first. Says what happened to the list (appended, replaced, nothing). */
    mcp::TalkChange addLine(juce::String const& role, juce::String const& text, juce::String const& context = {})
    {
        return list.add(role.toStdString(), CopilotMarkdown::sanitizeText(text).toStdString(), context.toStdString());
    }

    //==========================================================================
    // Per-line layout cache: parsing the markdown and shaping the text (TextLayout::createLayout) is expensive and paint() runs
    // at frame rate, so each line keeps its parsed, shaped blocks until its key (text, width, kind, UI font) changes.
    struct MeasuredBlock {
        CopilotMarkdown::ContentBlock block;
        juce::TextLayout layout;
        float height = 0.0f;
    };

    enum class Kind { User, Ai, Waiting, Error };

    struct LineLayout {
        std::string raw; // the line's text when built (compared without allocating)
        float width = -1.0f;
        Kind kind = Kind::Ai;
        juce::String typeface; // the default font when built: a font change in Settings rebuilds the line
        bool hasContext = false;
        juce::String context;
        float contextW = 0.0f;
        std::vector<MeasuredBlock> blocks;
        int textH = 0;
        int rowH = 0;
    };

    static Kind kindOf(mcp::TalkLine const& line)
    {
        if (line.role == "user") return Kind::User;
        if (line.role == "error") return Kind::Error;
        return line.waiting ? Kind::Waiting : Kind::Ai;
    }

    // The width the text of a line may take.
    static float textWidthFor(Kind kind, int width)
    {
        if (kind == Kind::User)
            return std::max(60.0f, static_cast<float>(width - 2 * pad) * userWidthShare);
        return std::max(60.0f, static_cast<float>(width - pad - markColumn - pad));
    }

    static juce::String toString(std::string const& utf8)
    {
        return juce::String::fromUTF8(utf8.data(), static_cast<int>(utf8.size()));
    }

    static void buildLineLayout(LineLayout& out, mcp::TalkLine const& line, int width)
    {
        auto const bodyFont = Fonts::getDefaultFont().withHeight(bodySize);
        auto const secondFont = Fonts::getDefaultFont().withHeight(secondarySize);
        auto const boldFont = Fonts::getSemiBoldFont().withHeight(bodySize);
        auto const monoFont = Fonts::getMonospaceFont().withHeight(12.5f);

        auto const kind = kindOf(line);
        auto const text = toString(line.text);
        float const innerW = textWidthFor(kind, width);

        // The artist's words are plain text, not markdown: one paragraph.
        std::vector<CopilotMarkdown::ContentBlock> blocks;
        if (kind == Kind::User) {
            CopilotMarkdown::ContentBlock b;
            b.type = CopilotMarkdown::ContentBlock::Paragraph;
            b.spans.push_back({ text, false, false, false });
            blocks.push_back(std::move(b));
        } else {
            blocks = CopilotMarkdown::parseBlocks(text);
        }

        float contentH = 0.0f;
        std::vector<MeasuredBlock> measured;
        measured.reserve(blocks.size());
        for (size_t index = 0; index < blocks.size(); ++index) {
            auto const& b = blocks[index];
            MeasuredBlock mb;
            mb.block = b;

            if (b.type == CopilotMarkdown::ContentBlock::Divider) {
                mb.height = 8.0f;
            } else if (b.type == CopilotMarkdown::ContentBlock::CodeBlock) {
                mb.height = static_cast<float>(b.codeLines.size()) * 16.0f + 16.0f;
            } else {
                // The first paragraph is the line; a later plain paragraph is the quiet second voice (12 px grey).
                bool const second = index > 0 && b.type == CopilotMarkdown::ContentBlock::Paragraph && kind != Kind::User;
                juce::Font baseFont = second ? secondFont : bodyFont;
                if (b.type == CopilotMarkdown::ContentBlock::Title || b.type == CopilotMarkdown::ContentBlock::SectionHeader
                    || b.type == CopilotMarkdown::ContentBlock::SubHeader)
                    baseFont = boldFont;
                auto const textColour = (second || kind == Kind::Waiting) ? copilotlook::grey() : copilotlook::ink();

                juce::AttributedString as;
                as.setWordWrap(juce::AttributedString::WordWrap::byWord);
                if (kind == Kind::User)
                    as.setJustification(juce::Justification::topRight);
                for (auto const& sp : b.spans) {
                    juce::Font f = sp.isCode ? monoFont : (sp.isBold ? boldFont.withHeight(baseFont.getHeight()) : baseFont);
                    if (sp.isItalic)
                        f = f.italicised();
                    as.append(sp.text, f, textColour);
                }
                float const availableW = std::max(20.0f, innerW - b.indentLeft);
                mb.layout.createLayout(as, availableW);
                mb.height = mb.layout.getHeight();
            }
            contentH += b.topMargin + mb.height + b.bottomMargin;
            measured.push_back(std::move(mb));
        }

        out.raw = line.text;
        out.width = static_cast<float>(width);
        out.kind = kind;
        out.hasContext = kind == Kind::User && !line.context.empty();
        out.context = out.hasContext ? "on " + toString(line.context) : juce::String();
        out.contextW = out.hasContext ? CachedFontStringWidth::get()->calculateStringWidth(secondFont.withHeight(11.0f), out.context) + 12.0f : 0.0f;
        out.blocks = std::move(measured);
        out.textH = static_cast<int>(std::ceil(contentH));
        out.rowH = out.textH + (out.hasContext ? contextLabelGap + contextLabelH : 0) + lineGap;
    }

    // The cached layout of a line; rebuilt only when its key changed.
    LineLayout const& layoutFor(mcp::TalkLine const& line, int width) const
    {
        auto& entry = layoutCache[line.id];
        auto const typeface = Fonts::getDefaultFont().getTypefaceName();
        bool const hasContext = !line.context.empty();
        if (entry.width != static_cast<float>(width) || entry.kind != kindOf(line) || entry.raw != line.text || entry.typeface != typeface
            || entry.hasContext != hasContext) {
            buildLineLayout(entry, line, width);
            entry.typeface = typeface;
        }
        return entry;
    }

    void pruneLayoutCache() const
    {
        if (layoutCache.size() <= list.size() + 16)
            return;
        std::unordered_set<std::uint64_t> alive;
        for (auto const& line : list.lines())
            alive.insert(line.id);
        for (auto it = layoutCache.begin(); it != layoutCache.end();)
            it = alive.count(it->first) > 0 ? std::next(it) : layoutCache.erase(it);
    }

    //==========================================================================
    void paint(Graphics& g) override
    {
        int const w = getWidth();
        thinkingBadgeBounds = {};
        if (w <= 2 * pad)
            return;

        if (list.empty() && !isThinking) {
            g.setFont(Fonts::getDefaultFont().withHeight(13.0f));
            g.setColour(copilotlook::grey());
            g.drawText("nothing said yet", juce::Rectangle<int>(pad, topPad, w - 2 * pad, 18), juce::Justification::centredLeft, true);
            return;
        }

        int y = topPad;
        auto const clip = g.getClipBounds();
        for (auto const& line : list.lines()) {
            auto const& cached = layoutFor(line, w);
            if (juce::Rectangle<int>(0, y, w, cached.rowH).intersects(clip))
                drawLine(g, line, cached, y, w);
            y += cached.rowH;
        }
        pruneLayoutCache();

        if (isThinking)
            drawThinking(g, y, w);
    }

    int calculateTotalHeight(int width) const
    {
        if (list.empty() && !isThinking)
            return topPad + 28;
        int total = topPad;
        for (auto const& line : list.lines())
            total += layoutFor(line, width).rowH;
        if (isThinking)
            total += thinkingRowH + lineGap;
        return total;
    }

private:
    void drawThinking(Graphics& g, int y, int w)
    {
        auto const row = juce::Rectangle<int>(pad, y, w - 2 * pad, thinkingRowH);
        thinkingBadgeBounds = row;
        // A slow pulse of one small ink dot in the mark column, the status in grey: no chip, no frame.
        float const pulse = 0.5f + 0.5f * std::sin(thinkingPhase);
        g.setColour(copilotlook::ink(0.35f + 0.65f * pulse));
        g.fillEllipse(static_cast<float>(row.getX()) + 2.0f, static_cast<float>(row.getCentreY()) - 3.0f, 6.0f, 6.0f);
        g.setFont(Fonts::getDefaultFont().withHeight(13.0f));
        g.setColour(copilotlook::grey());
        g.drawText(thinkingStatus, row.withTrimmedLeft(markColumn), juce::Justification::centredLeft, true);
    }

    void drawLine(Graphics& g, mcp::TalkLine const& line, LineLayout const& cached, int y, int w) const
    {
        bool const user = cached.kind == Kind::User;
        float const textX = user ? static_cast<float>(pad) : static_cast<float>(pad + markColumn);
        float const areaW = user ? static_cast<float>(w - 2 * pad) : static_cast<float>(w - pad - markColumn - pad);

        // The mark (or the dotted lead of a waiting line) stands in front of the first line of text.
        if (!user) {
            float const markY = static_cast<float>(y) + 10.0f;
            if (cached.kind == Kind::Waiting)
                copilotlook::drawDottedLead(g, static_cast<float>(pad), markY);
            else
                copilotlook::drawMark(g, mcp::talkMark(line), static_cast<float>(pad) + 5.0f, markY);
        }

        float blockY = static_cast<float>(y);
        for (auto const& mb : cached.blocks) {
            blockY += mb.block.topMargin;
            drawBlock(g, mb, textX, blockY, areaW, user);
            blockY += mb.height + mb.block.bottomMargin;
        }

        // Under an artist line about a widget: a small grey outlined label, right-aligned.
        if (cached.hasContext) {
            float const labelY = static_cast<float>(y + cached.textH + contextLabelGap);
            auto const label = juce::Rectangle<float>(static_cast<float>(w - pad) - cached.contextW, labelY, cached.contextW, static_cast<float>(contextLabelH));
            g.setColour(copilotlook::grey());
            g.drawRoundedRectangle(label.reduced(0.5f), mcp::aiRadius, mcp::aiLineWidth);
            g.setFont(Fonts::getDefaultFont().withHeight(11.0f));
            g.drawText(cached.context, label, juce::Justification::centred, false);
        }
    }

    void drawBlock(Graphics& g, MeasuredBlock const& mb, float x, float blockY, float areaW, bool user) const
    {
        using Block = CopilotMarkdown::ContentBlock;
        float const bx = x + mb.block.indentLeft;
        float const bw = std::max(20.0f, areaW - mb.block.indentLeft);

        if (mb.block.type == Block::Divider) {
            g.setColour(copilotlook::grey().withAlpha(0.5f));
            g.drawHorizontalLine(static_cast<int>(blockY + 4.0f), x, x + areaW);
            return;
        }
        if (mb.block.type == Block::CodeBlock) {
            auto const card = juce::Rectangle<float>(bx, blockY, bw, mb.height);
            g.setColour(copilotlook::wash(0.06f));
            g.fillRoundedRectangle(card, mcp::aiRadius);
            g.setFont(Fonts::getMonospaceFont().withHeight(12.0f));
            g.setColour(copilotlook::ink());
            float codeLineY = blockY + 8.0f;
            for (int ci = 0; ci < mb.block.codeLines.size(); ++ci) {
                g.drawText(mb.block.codeLines[ci], juce::Rectangle<float>(bx + 8.0f, codeLineY, bw - 16.0f, 16.0f), juce::Justification::centredLeft, false);
                codeLineY += 16.0f;
            }
            return;
        }

        // Bullet dots: a small ink dot, filled (bullet) or outlined (sub bullet)
        if (mb.block.type == Block::Bullet) {
            g.setColour(copilotlook::ink());
            g.fillEllipse(bx - 11.0f, blockY + 5.0f, 4.0f, 4.0f);
        } else if (mb.block.type == Block::SubBullet) {
            g.setColour(copilotlook::ink());
            g.drawEllipse(bx - 11.0f, blockY + 5.2f, 3.6f, 3.6f, 1.0f);
        }

        // Inline code gets a faint pill behind it (the mono typeface marks the run)
        if (mb.block.hasCode) {
            auto const monoName = Fonts::getMonospaceFont().getTypefaceName();
            for (int li = 0; li < mb.layout.getNumLines(); ++li) {
                auto const& textLine = mb.layout.getLine(li);
                for (auto* run : textLine.runs) {
                    if (run->font.getTypefaceName() != monoName)
                        continue;
                    auto const rx = run->getRunBoundsX();
                    if (rx.getLength() <= 0.5f)
                        continue;
                    float const lineX = user ? textLine.lineOrigin.x + (areaW - mb.layout.getWidth()) : textLine.lineOrigin.x;
                    auto const pill = juce::Rectangle<float>(bx + lineX + rx.getStart() - 3.0f, blockY + textLine.lineOrigin.y - textLine.ascent - 1.0f,
                        rx.getLength() + 6.0f, textLine.ascent + textLine.descent + 2.0f);
                    g.setColour(copilotlook::wash(0.08f));
                    g.fillRoundedRectangle(pill, mcp::aiRadius);
                }
            }
        }

        mb.layout.draw(g, juce::Rectangle<float>(bx, blockY, bw, mb.height));
    }

    mutable std::unordered_map<std::uint64_t, LineLayout> layoutCache;
};
