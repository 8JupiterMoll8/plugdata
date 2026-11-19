/*
 // Copyright (c) 2021-2026 Timothy Schoen & Contributors.
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
 */
// The Copilot panel's talk tab, "who is who" (design round 2026-11-19, picture talk-who-is-who.png, the Proposal side): no
// bubbles. One artist ask and the answers after it are one GROUP (McpCopilotTalk.h talkGroups); groups are separated by a 1 px
// rule, ink at 18 %.
//   - The artist's line is a black (ink) block with cream text, 7 / 10 px padding, left aligned, at most 85 % of the width. Above
//     it, a small grey line "you · on <box>" (the box in ink) when the ask was about a selection or a widget.
//   - The AI's line is plain ink text right under it, with a small grey line on top: the mark shape and "alphi · <what>"
//     ("changed 4 · measured -14 dB", "explained", or just "alphi" when nothing is proved), once per run of the same fact
//     (talkHeaderShown). The marks come from the server's tool results of that turn, never from the words (talkMark).
//   - A waiting line ("Claude Code has not answered yet") is grey text after three dots, and the answer takes its place (the list
//     replaces it, TalkList). An empty talk is one grey line.
//   - A long answer is shown whole: the text wraps, nothing is cut (the server sends up to 2000 characters on a plain line).
//
// The lines are McpCopilotTalk.h's TalkList; this component owns it and writes it only on the message thread (CopilotPanel).
// The parsed and shaped text of each line is cached by the line's id until its text, mark, width or the UI font changes (paint
// runs at frame rate); where a line sits depends on its neighbours (group start, header), so the rows are placed on each paint
// from those cached sizes. Colours and sizes are the one AI style table (Mcp/McpAiStyle.h) through CopilotLook.h.
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
    static constexpr int markColumn = 18;            // the lead in front of a waiting line and of the thinking row
    static constexpr int topPad = 12;                // above the first group
    static constexpr int groupPadTop = 12;           // under the rule that starts a group
    static constexpr int groupPadBottom = 6;         // under the last line of a group, before the next rule
    static constexpr int aiGap = 10;                 // between the artist's block and the answer, and between two answer lines
    static constexpr float ruleAlpha = 0.18f;        // the rule between groups: ink at 18 %
    static constexpr float userWidthShare = 0.85f;   // the artist's block takes at most this much of the width
    static constexpr int userPadV = 7;               // the artist's block: 7 px above and below the text...
    static constexpr int userPadH = 10;              // ...and 10 px at both sides
    static constexpr float bodySize = 14.0f;         // the first paragraph of a line
    static constexpr float secondarySize = 12.0f;    // a second paragraph, the grey lines
    static constexpr int labelH = 16;                // a grey line: "you · on <box>" or the answer's header
    static constexpr int labelGap = 3;               // between a grey line and what it labels
    static constexpr int markGap = 6;                // between a header's mark and its words
    static constexpr int thinkingRowH = 22;

    mcp::TalkList list;
    bool isThinking = false;
    juce::String thinkingStatus = "Thinking...";
    float thinkingPhase = 0.0f;

    // Bounds of the thinking row from the last paint (empty when hidden), so its animation can repaint just that area.
    juce::Rectangle<int> thinkingBadgeBounds;

    /** One chat line; the text is cleaned for the fonts first. `mark` is the server's word for what the turn proved (an AI line
     *  only). Says what happened to the list (appended, replaced, nothing). */
    mcp::TalkChange addLine(juce::String const& role, juce::String const& text, juce::String const& context = {}, juce::String const& mark = {})
    {
        return list.add(role.toStdString(), CopilotMarkdown::sanitizeText(text).toStdString(), context.toStdString(), mark.toStdString());
    }

    //==========================================================================
    // Per-line layout cache: parsing the markdown and shaping the text (TextLayout::createLayout) is expensive and paint() runs
    // at frame rate, so each line keeps its parsed, shaped blocks until its key (text, mark, width, kind, UI font) changes.
    struct MeasuredBlock {
        CopilotMarkdown::ContentBlock block;
        juce::TextLayout layout;
        float height = 0.0f;
    };

    enum class Kind { User, Ai, Waiting, Error };

    struct LineLayout {
        std::string raw; // the line's text when built (compared without allocating)
        mcp::TalkWork work;
        float width = -1.0f;
        Kind kind = Kind::Ai;
        juce::String typeface; // the default font when built: a font change in Settings rebuilds the line
        juce::String context;  // User: the box name ("random 8"), empty when the ask was about nothing
        float prefixW = 0.0f;  // User: the width of "you · on "
        juce::String header;   // Ai / Error: "alphi · changed 4 · measured -14 dB"
        std::vector<MeasuredBlock> blocks;
        int textH = 0;
        float textW = 0.0f;    // the widest line of the laid out text (the artist's block is as wide as its text)
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
        auto const area = static_cast<float>(width - 2 * pad);
        if (kind == Kind::User)
            return std::max(40.0f, area * userWidthShare - static_cast<float>(2 * userPadH));
        if (kind == Kind::Waiting)
            return std::max(60.0f, area - static_cast<float>(markColumn));
        return std::max(60.0f, area);
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
        float widest = 0.0f;
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
                // The first paragraph is the line; a later plain paragraph is the quiet second voice (12 px grey). The artist's
                // block is one paragraph in cream on the ink.
                bool const second = index > 0 && b.type == CopilotMarkdown::ContentBlock::Paragraph && kind != Kind::User;
                juce::Font baseFont = second ? secondFont : bodyFont;
                if (b.type == CopilotMarkdown::ContentBlock::Title || b.type == CopilotMarkdown::ContentBlock::SectionHeader
                    || b.type == CopilotMarkdown::ContentBlock::SubHeader)
                    baseFont = boldFont;
                auto const textColour = kind == Kind::User ? copilotlook::cream() : ((second || kind == Kind::Waiting) ? copilotlook::grey() : copilotlook::ink());

                juce::AttributedString as;
                as.setWordWrap(juce::AttributedString::WordWrap::byWord);
                for (auto const& sp : b.spans) {
                    juce::Font f = sp.isCode ? monoFont : (sp.isBold ? boldFont.withHeight(baseFont.getHeight()) : baseFont);
                    if (sp.isItalic)
                        f = f.italicised();
                    as.append(sp.text, f, textColour);
                }
                float const availableW = std::max(20.0f, innerW - b.indentLeft);
                mb.layout.createLayout(as, availableW);
                mb.height = mb.layout.getHeight();
                widest = std::max(widest, mb.layout.getWidth() + b.indentLeft);
            }
            contentH += b.topMargin + mb.height + b.bottomMargin;
            measured.push_back(std::move(mb));
        }

        out.raw = line.text;
        out.work = line.work;
        out.width = static_cast<float>(width);
        out.kind = kind;
        out.context = contextOf(line);
        out.prefixW = out.context.isNotEmpty() ? CachedFontStringWidth::get()->calculateStringWidth(secondFont, juce::String::fromUTF8(prefixText)) : 0.0f;
        out.header = (kind == Kind::Ai || kind == Kind::Error) ? toString(mcp::talkHeaderText(line)) : juce::String();
        out.blocks = std::move(measured);
        out.textH = static_cast<int>(std::ceil(contentH));
        out.textW = widest;
    }

    // The box name an artist line was about, or empty (only an artist line has one).
    static juce::String contextOf(mcp::TalkLine const& line)
    {
        return kindOf(line) == Kind::User ? toString(line.context) : juce::String();
    }

    // The cached layout of a line; rebuilt only when its key changed.
    LineLayout const& layoutFor(mcp::TalkLine const& line, int width) const
    {
        auto& entry = layoutCache[line.id];
        auto const typeface = Fonts::getDefaultFont().getTypefaceName();
        if (entry.width != static_cast<float>(width) || entry.kind != kindOf(line) || entry.raw != line.text || entry.work != line.work
            || entry.typeface != typeface || entry.context != contextOf(line)) {
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
    // Where one line sits. A group starts at each artist line (and at the very first line); it begins with the rule that
    // separates it from the group before. An answer line sits aiGap under the line before it and wears its grey header when
    // mcp::talkHeaderShown says so.
    struct Row {
        int top = 0;             // the line's own top (below the rule and the group's top padding)
        int height = 0;          // the line's own height
        int ruleY = -1;          // where the rule above its group is drawn, -1 = none
        bool header = false;     // an answer line with its grey header
    };

    static int labelBlockH(bool shown) { return shown ? labelH + labelGap : 0; }

    // Places every line for `width`; returns the height of all of them (with the bottom padding of the last group).
    int placeRows(int width, std::vector<Row>* rows) const
    {
        auto const& lines = list.lines();
        if (rows) {
            rows->clear();
            rows->reserve(lines.size());
        }
        int y = topPad;
        for (size_t i = 0; i < lines.size(); ++i) {
            auto const& line = lines[i];
            auto const& cached = layoutFor(line, width);
            Row row;
            bool const groupStart = i == 0 || cached.kind == Kind::User;
            if (i > 0 && groupStart) {
                y += groupPadBottom;
                row.ruleY = y;
                y += 1 + groupPadTop;
            } else if (i > 0) {
                y += aiGap;
            }
            row.top = y;
            if (cached.kind == Kind::User) {
                row.height = labelBlockH(cached.context.isNotEmpty()) + cached.textH + 2 * userPadV;
            } else {
                row.header = cached.kind != Kind::Waiting && mcp::talkHeaderShown(lines, i);
                row.height = labelBlockH(row.header) + cached.textH;
            }
            y += row.height;
            if (rows)
                rows->push_back(row);
        }
        return y + groupPadBottom;
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

        std::vector<Row> rows;
        int const bottom = placeRows(w, &rows);
        auto const clip = g.getClipBounds();
        auto const& lines = list.lines();
        for (size_t i = 0; i < lines.size(); ++i) {
            auto const& row = rows[i];
            if (row.ruleY >= 0 && clip.getBottom() > row.ruleY && clip.getY() <= row.ruleY) {
                g.setColour(copilotlook::ink(ruleAlpha));
                g.fillRect(pad, row.ruleY, w - 2 * pad, 1);
            }
            if (juce::Rectangle<int>(0, row.top, w, row.height).intersects(clip))
                drawLine(g, lines[i], layoutFor(lines[i], w), row, w);
        }
        pruneLayoutCache();

        if (isThinking)
            drawThinking(g, bottom - groupPadBottom + aiGap, w);
    }

    int calculateTotalHeight(int width) const
    {
        if (list.empty() && !isThinking)
            return topPad + 28;
        int total = placeRows(width, nullptr);
        if (isThinking)
            total += aiGap + thinkingRowH;
        return total;
    }

private:
    static constexpr char const* prefixText = "you \xC2\xB7 on ";

    void drawThinking(Graphics& g, int y, int w)
    {
        auto const row = juce::Rectangle<int>(pad, y, w - 2 * pad, thinkingRowH);
        thinkingBadgeBounds = row;
        // A slow pulse of one small ink dot in the lead column, the status in grey: no chip, no frame.
        float const pulse = 0.5f + 0.5f * std::sin(thinkingPhase);
        g.setColour(copilotlook::ink(0.35f + 0.65f * pulse));
        g.fillEllipse(static_cast<float>(row.getX()) + 2.0f, static_cast<float>(row.getCentreY()) - 3.0f, 6.0f, 6.0f);
        g.setFont(Fonts::getDefaultFont().withHeight(13.0f));
        g.setColour(copilotlook::grey());
        g.drawText(thinkingStatus, row.withTrimmedLeft(markColumn), juce::Justification::centredLeft, true);
    }

    void drawLine(Graphics& g, mcp::TalkLine const& line, LineLayout const& cached, Row const& row, int w) const
    {
        if (cached.kind == Kind::User) {
            drawUserLine(g, cached, row, w);
            return;
        }

        float y = static_cast<float>(row.top);
        float const areaW = static_cast<float>(w - 2 * pad);
        float textX = static_cast<float>(pad);
        float textAreaW = areaW;

        if (row.header) {
            // The grey line on top: the mark shape, then "alphi · what" in 12 px grey.
            auto const mark = mcp::talkMark(line);
            float wordsX = static_cast<float>(pad);
            if (mark != mcp::TalkMark::None) {
                copilotlook::drawMark(g, mark, static_cast<float>(pad) + mcp::aiTagGlyphSize * 0.5f, y + static_cast<float>(labelH) * 0.5f);
                wordsX += mcp::aiTagGlyphSize + static_cast<float>(markGap);
            }
            g.setFont(Fonts::getDefaultFont().withHeight(secondarySize));
            g.setColour(copilotlook::grey());
            g.drawText(cached.header, juce::Rectangle<float>(wordsX, y, static_cast<float>(w - pad) - wordsX, static_cast<float>(labelH)), juce::Justification::centredLeft, true);
            y += static_cast<float>(labelBlockH(true));
        }

        if (cached.kind == Kind::Waiting) {
            // The dotted lead of a waiting line stands in front of its first line of text.
            copilotlook::drawDottedLead(g, static_cast<float>(pad), y + 10.0f);
            textX += static_cast<float>(markColumn);
            textAreaW -= static_cast<float>(markColumn);
        }

        float blockY = y;
        for (auto const& mb : cached.blocks) {
            blockY += mb.block.topMargin;
            drawBlock(g, mb, textX, blockY, textAreaW);
            blockY += mb.height + mb.block.bottomMargin;
        }
    }

    // The artist's line: "you · on <box>" in grey (the box in ink) above a black block with the words in cream.
    void drawUserLine(Graphics& g, LineLayout const& cached, Row const& row, int w) const
    {
        float y = static_cast<float>(row.top);
        float const areaW = static_cast<float>(w - 2 * pad);
        if (cached.context.isNotEmpty()) {
            g.setFont(Fonts::getDefaultFont().withHeight(secondarySize));
            g.setColour(copilotlook::grey());
            g.drawText(juce::String::fromUTF8(prefixText), juce::Rectangle<float>(static_cast<float>(pad), y, cached.prefixW + 2.0f, static_cast<float>(labelH)), juce::Justification::centredLeft, false);
            g.setFont(Fonts::getSemiBoldFont().withHeight(secondarySize));
            g.setColour(copilotlook::ink());
            g.drawText(cached.context, juce::Rectangle<float>(static_cast<float>(pad) + cached.prefixW, y, areaW - cached.prefixW, static_cast<float>(labelH)), juce::Justification::centredLeft, true);
            y += static_cast<float>(labelBlockH(true));
        }

        float const maxBlockW = std::max(60.0f, areaW * userWidthShare);
        float const blockW = std::min(maxBlockW, cached.textW + static_cast<float>(2 * userPadH) + 1.0f);
        auto const block = juce::Rectangle<float>(static_cast<float>(pad), y, blockW, static_cast<float>(cached.textH + 2 * userPadV));
        g.setColour(copilotlook::ink());
        g.fillRoundedRectangle(block, mcp::aiRadius);

        float blockY = y + static_cast<float>(userPadV);
        for (auto const& mb : cached.blocks) {
            blockY += mb.block.topMargin;
            mb.layout.draw(g, juce::Rectangle<float>(block.getX() + static_cast<float>(userPadH), blockY, block.getWidth() - static_cast<float>(2 * userPadH), mb.height));
            blockY += mb.height + mb.block.bottomMargin;
        }
    }

    void drawBlock(Graphics& g, MeasuredBlock const& mb, float x, float blockY, float areaW) const
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
                    auto const pill = juce::Rectangle<float>(bx + textLine.lineOrigin.x + rx.getStart() - 3.0f, blockY + textLine.lineOrigin.y - textLine.ascent - 1.0f,
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
