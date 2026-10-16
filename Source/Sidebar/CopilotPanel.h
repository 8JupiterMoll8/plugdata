/*
 // Copyright (c) 2021-2026 Timothy Schoen & Contributors.
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
 */

#pragma once

#include <utility>
#include <vector>
#include <cmath>

#include "Utility/CachedStringWidth.h"
#include "Utility/Fonts.h"
#include "Components/BouncingViewport.h"
#include "Components/SearchEditor.h"
#include "Components/Buttons.h"
#include "Components/ObjectDragAndDrop.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "Canvas.h"
#include "CanvasViewport.h"
#include "Object.h"
#include "Pd/MCPBridge.h"

class CopilotPanel final : public Component, private juce::Timer {
public:
    enum class PanelTab { Unified, Notebook, Studio, Chat };

    struct ChatMessage {
        juce::String role; // "user", "thought", "ai", "error"
        juce::String text;
        juce::Time time;
    };

    // Drag a ready-made widget onto the canvas. Upstream's ObjectDragAndDrop is a
    // final singleton now, started with attachToMouse (same idiom as AddObjectMenu).
    class DraggableWidgetCard final : public Component {
    public:
        DraggableWidgetCard(PluginEditor* e, const juce::String& name, const juce::String& sub, juce::Colour dotCol, const juce::String& patchText)
            : editor(e), widgetName(name), subText(sub), accentDot(dotCol), patchString(patchText)
        {}

        void mouseDown(const MouseEvent&) override { dragStarted = false; }

        void mouseDrag(const MouseEvent& e) override
        {
            if (!dragStarted && e.getDistanceFromDragStart() > 3) {
                dragStarted = true;
                ObjectDragAndDrop::attachToMouse(editor, patchString);
            }
        }

        void paint(Graphics& g) override
        {
            auto b = getLocalBounds().toFloat().reduced(1.0f);
            auto const bgCol = findColour(PlugDataColour::sidebarBackgroundColourId);
            auto const activeCol = findColour(PlugDataColour::toolbarActiveColourId);
            bool const dark = (bgCol.getPerceivedBrightness() < 0.5f);
            bool const over = isMouseOver();

            auto cardBg = dark ? juce::Colour(0xff181a1f) : juce::Colour(0xfff6f8fa);
            auto cardBorder = over ? activeCol : (dark ? juce::Colour(0xff333842) : juce::Colour(0xffd0d7de));

            g.setColour(cardBg);
            g.fillRoundedRectangle(b, 6.0f);

            g.setColour(cardBorder);
            g.drawRoundedRectangle(b, 6.0f, over ? 1.5f : 1.0f);

            // LED dot indicator
            g.setColour(accentDot.withAlpha(over ? 1.0f : 0.85f));
            g.fillEllipse(b.getX() + 8.0f, b.getCentreY() - 3.5f, 7.0f, 7.0f);

            // Title
            auto font = Fonts::getBoldFont().withHeight(12.5f);
            g.setFont(font);
            g.setColour(dark ? juce::Colour(0xfff0f2f5) : juce::Colour(0xff181a1f));
            g.drawText(widgetName, juce::Rectangle<float>(b.getX() + 20.0f, b.getY() + 3.0f, b.getWidth() - 24.0f, 15.0f),
                       juce::Justification::centredLeft, true);

            // Subtitle
            auto subFont = Fonts::getDefaultFont().withHeight(10.5f);
            g.setFont(subFont);
            g.setColour(dark ? juce::Colour(0xff8b949e) : juce::Colour(0xff57606a));
            g.drawText(subText, juce::Rectangle<float>(b.getX() + 20.0f, b.getY() + 18.0f, b.getWidth() - 24.0f, 13.0f),
                       juce::Justification::centredLeft, true);
        }

        void mouseEnter(const MouseEvent&) override { repaint(); }
        void mouseExit(const MouseEvent&) override { repaint(); }

    private:
        PluginEditor* editor;
        bool dragStarted = false;
        juce::String widgetName;
        juce::String subText;
        juce::Colour accentDot;
        juce::String patchString;
    };

    class ShelfBar final : public Component {
    public:
        int takeCount = 0;
        int imageCount = 0;
        bool open = false;
        std::function<void()> onToggle;

        void paint(Graphics& g) override
        {
            auto const bgCol = findColour(PlugDataColour::sidebarBackgroundColourId);
            bool const dark = (bgCol.getPerceivedBrightness() < 0.5f);

            auto b = getLocalBounds().toFloat();
            g.setColour(dark ? juce::Colour(0xff21262d) : juce::Colour(0xfff6f8fa));
            g.fillRoundedRectangle(b, 4.0f);
            g.setColour(dark ? juce::Colour(0xff30363d) : juce::Colour(0xffd0d7de));
            g.drawRoundedRectangle(b.reduced(0.5f), 4.0f, 1.0f);

            auto font = Fonts::getBoldFont().withHeight(10.5f);
            g.setFont(font);
            g.setColour(dark ? juce::Colour(0xff8b949e) : juce::Colour(0xff57606a));

            juce::String label = (open ? "v " : "> ") + juce::String("TAPE SHELF (")
                               + juce::String(takeCount) + " takes, "
                               + juce::String(imageCount) + " images)";
            g.drawText(label, b.reduced(8, 0), juce::Justification::centredLeft, true);

            auto const actionCol = findColour(PlugDataColour::toolbarActiveColourId);
            g.setColour(actionCol);
            g.drawText(open ? "COLLAPSE" : "OPEN", b.reduced(8, 0), juce::Justification::centredRight, true);
        }

        void mouseUp(const MouseEvent&) override { if (onToggle) onToggle(); }
        void mouseEnter(const MouseEvent&) override { repaint(); }
        void mouseExit(const MouseEvent&) override { repaint(); }
    };

    //==============================================================================
    // Antigravity Rich Markdown & Message Layout Engine
    //==============================================================================
    class ChatListContent final : public Component {
    public:
        std::vector<ChatMessage> items;
        bool isThinking = false;
        juce::String thinkingStatus = "Thinking...";
        float thinkingPhase = 0.0f;

        struct InlineSpan {
            juce::String text;
            bool isCode = false;
            bool isBold = false;
            bool isItalic = false;
        };

        struct ContentBlock {
            enum Type {
                Title,
                SectionHeader,
                SubHeader,
                Bullet,
                SubBullet,
                Paragraph,
                CodeBlock,
                Divider
            };

            Type type = Paragraph;
            std::vector<InlineSpan> spans;
            juce::StringArray codeLines;
            float indentLeft = 0.0f;
            float topMargin = 0.0f;
            float bottomMargin = 0.0f;
            bool hasCode = false;
        };

        static juce::String sanitizeText(juce::String s)
        {
            // Replace arrows that lack glyphs in standard Linux fonts
            s = s.replace(juce::String::fromUTF8("\xe2\x9e\x94"), " -> "); // ➔
            s = s.replace(juce::String::fromUTF8("\xe2\x9e\x9c"), " -> "); // ➜
            s = s.replace(juce::String::fromUTF8("\xe2\x86\x92"), " -> "); // →
            s = s.replace(juce::String::fromUTF8("\xe2\x87\x92"), " => "); // ⇒
            s = s.replace(juce::String::fromUTF8("\xe2\x87\xa8"), " -> "); // ⇨
            s = s.replace(juce::String::fromUTF8("\xe2\x86\x90"), " <- "); // ←
            s = s.replace(juce::String::fromUTF8("\xe2\x86\x94"), " <-> "); // ↔
            s = s.replace(juce::String::fromUTF8("\xe2\x96\xb8"), " > ");  // ▸
            s = s.replace(juce::String::fromUTF8("\xe2\x96\xba"), " > ");  // ►

            // Dashes
            s = s.replace(juce::String::fromUTF8("\xe2\x80\x94"), " - ");  // —
            s = s.replace(juce::String::fromUTF8("\xe2\x80\x93"), " - ");  // –
            s = s.replace(juce::String::fromUTF8("\xe2\x80\x95"), " - ");  // ―

            // Clean invisible & control characters
            s = s.replace(juce::String::fromUTF8("\xc2\xa0"), " ");
            s = s.replace(juce::String::fromUTF8("\xef\xbf\xbd"), "");

            return s;
        }

        static std::vector<InlineSpan> parseInline(const juce::String& str, bool forceBold = false)
        {
            std::vector<InlineSpan> spans;
            juce::String cur;
            bool bold = forceBold;
            bool italic = false;
            bool code = false;
            int const n = str.length();

            auto flush = [&](bool isC, bool isB, bool isI) {
                if (cur.isNotEmpty()) {
                    spans.push_back({ cur, isC, isB, isI });
                    cur.clear();
                }
            };

            for (int i = 0; i < n; ++i) {
                auto const c = str[i];

                // Bold toggle: **
                if (c == '*' && i + 1 < n && str[i + 1] == '*') {
                    flush(code, bold, italic);
                    bold = !bold;
                    ++i;
                    continue;
                }

                // Inline code toggle: `
                if (c == '`') {
                    flush(code, bold, italic);
                    code = !code;
                    continue;
                }

                // Pure Data object badge in brackets: [obj~]
                if (!code && c == '[') {
                    int closeIdx = str.indexOf(i + 1, "]");
                    if (closeIdx > i + 1 && closeIdx < i + 45) {
                        if (closeIdx + 1 >= n || str[closeIdx + 1] != '(') {
                            flush(code, bold, italic);
                            juce::String objText = str.substring(i, closeIdx + 1);
                            spans.push_back({ objText, true, false, false });
                            i = closeIdx;
                            continue;
                        }
                    }
                }

                // Italic: *
                if (c == '*' && !bold) {
                    flush(code, bold, italic);
                    italic = !italic;
                    continue;
                }

                cur += c;
            }
            flush(code, bold, italic);
            return spans;
        }

        static std::vector<ContentBlock> parseBlocks(const juce::String& rawText)
        {
            std::vector<ContentBlock> blocks;
            auto const cleanText = sanitizeText(rawText);
            auto const lines = juce::StringArray::fromLines(cleanText);
            bool inCodeBlock = false;
            ContentBlock currentCodeBlock;

            for (int li = 0; li < lines.size(); ++li) {
                auto rawLine = lines[li];
                auto trimmed = rawLine.trim();

                // ``` Code Block toggle
                if (trimmed.startsWith("```")) {
                    if (inCodeBlock) {
                        blocks.push_back(currentCodeBlock);
                        inCodeBlock = false;
                    } else {
                        inCodeBlock = true;
                        currentCodeBlock = ContentBlock();
                        currentCodeBlock.type = ContentBlock::CodeBlock;
                        currentCodeBlock.topMargin = 6.0f;
                        currentCodeBlock.bottomMargin = 6.0f;
                    }
                    continue;
                }

                if (inCodeBlock) {
                    currentCodeBlock.codeLines.add(rawLine);
                    continue;
                }

                if (trimmed.isEmpty()) continue;

                // Horizontal Divider
                if (trimmed == "---" || trimmed == "***" || trimmed == "___") {
                    ContentBlock b;
                    b.type = ContentBlock::Divider;
                    b.topMargin = 6.0f;
                    b.bottomMargin = 6.0f;
                    blocks.push_back(b);
                    continue;
                }

                // Title: starts with # or emoji
                if (trimmed.startsWith("# ") || trimmed.startsWithChar((juce::juce_wchar)0x2728) || // ✨
                    trimmed.startsWithChar((juce::juce_wchar)0x1f680) || // 🚀
                    trimmed.startsWithChar((juce::juce_wchar)0x1f4a1) || // 💡
                    trimmed.startsWithChar((juce::juce_wchar)0x1f389)) { // 🎉
                    ContentBlock b;
                    b.type = ContentBlock::Title;
                    b.topMargin = 2.0f;
                    b.bottomMargin = 5.0f;
                    auto content = trimmed.startsWith("# ") ? trimmed.substring(2) : trimmed;
                    b.spans = parseInline(content, true);
                    for (auto const& s : b.spans) if (s.isCode) b.hasCode = true;
                    blocks.push_back(b);
                    continue;
                }

                // Section Header: "1. Typography" or "## Header"
                bool isNumberedHeader = false;
                if (trimmed.startsWith("## ")) {
                    isNumberedHeader = true;
                } else if (trimmed.length() >= 3 && juce::CharacterFunctions::isDigit(trimmed[0])) {
                    int dotIdx = trimmed.indexOf(". ");
                    if (dotIdx > 0 && dotIdx <= 3) isNumberedHeader = true;
                }
                if (isNumberedHeader) {
                    ContentBlock b;
                    b.type = ContentBlock::SectionHeader;
                    b.topMargin = 8.0f;
                    b.bottomMargin = 3.0f;
                    auto content = trimmed.startsWith("## ") ? trimmed.substring(3) : trimmed;
                    b.spans = parseInline(content, true);
                    for (auto const& s : b.spans) if (s.isCode) b.hasCode = true;
                    blocks.push_back(b);
                    continue;
                }

                // SubHeader: "### Header" or "What Was Fixed:"
                if (trimmed.startsWith("### ") || (trimmed.endsWith(":") && trimmed.length() < 60 && !trimmed.startsWith("- ") && !trimmed.startsWith("* ") && !trimmed.startsWith("+ "))) {
                    ContentBlock b;
                    b.type = ContentBlock::SubHeader;
                    b.topMargin = 6.0f;
                    b.bottomMargin = 3.0f;
                    auto content = trimmed.startsWith("### ") ? trimmed.substring(4) : trimmed;
                    b.spans = parseInline(content, true);
                    for (auto const& s : b.spans) if (s.isCode) b.hasCode = true;
                    blocks.push_back(b);
                    continue;
                }

                // Bullet points: - , * , + , •
                bool isBullet = false;
                int bulletMarkerLen = 0;
                int leadingSpaces = 0;
                while (leadingSpaces < rawLine.length() && (rawLine[leadingSpaces] == ' ' || rawLine[leadingSpaces] == '\t')) {
                    leadingSpaces += (rawLine[leadingSpaces] == '\t' ? 2 : 1);
                }
                auto lineWithoutIndent = rawLine.substring(leadingSpaces);
                if (lineWithoutIndent.startsWith("- ") || lineWithoutIndent.startsWith("* ") || lineWithoutIndent.startsWith("+ ")) {
                    isBullet = true;
                    bulletMarkerLen = 2;
                } else if (lineWithoutIndent.startsWith(juce::String::charToString((juce::juce_wchar)0x2022) + " ")) {
                    isBullet = true;
                    bulletMarkerLen = 2;
                }

                if (isBullet) {
                    ContentBlock b;
                    bool const isSub = (leadingSpaces >= 2);
                    b.type = isSub ? ContentBlock::SubBullet : ContentBlock::Bullet;
                    b.indentLeft = isSub ? 28.0f : 14.0f;
                    b.topMargin = 2.0f;
                    b.bottomMargin = 3.0f;

                    auto bulletContent = lineWithoutIndent.substring(bulletMarkerLen).trim();
                    // Antigravity bold prefix pattern: "Title: Description"
                    int colonIdx = bulletContent.indexOf(": ");
                    if (colonIdx > 0 && colonIdx < 35 && !bulletContent.startsWith("**")) {
                        auto title = bulletContent.substring(0, colonIdx + 1);
                        auto body = bulletContent.substring(colonIdx + 1);
                        auto titleSpans = parseInline(title, true);
                        auto bodySpans = parseInline(body, false);
                        b.spans.insert(b.spans.end(), titleSpans.begin(), titleSpans.end());
                        b.spans.insert(b.spans.end(), bodySpans.begin(), bodySpans.end());
                    } else {
                        b.spans = parseInline(bulletContent, false);
                    }
                    for (auto const& s : b.spans) if (s.isCode) b.hasCode = true;
                    blocks.push_back(b);
                    continue;
                }

                // Regular Paragraph
                ContentBlock b;
                b.type = ContentBlock::Paragraph;
                b.topMargin = 2.0f;
                b.bottomMargin = 4.0f;
                b.spans = parseInline(trimmed, false);
                for (auto const& s : b.spans) if (s.isCode) b.hasCode = true;
                blocks.push_back(b);
            }

            if (inCodeBlock && !currentCodeBlock.codeLines.isEmpty()) {
                blocks.push_back(currentCodeBlock);
            }

            return blocks;
        }

        void addMessage(const juce::String& role, const juce::String& text)
        {
            if (!items.empty() && items.back().role == role && items.back().text.trim() == text.trim()) {
                return;
            }
            while (!items.empty() && items.back().role == "thought") {
                items.pop_back();
            }
            items.push_back({ role, sanitizeText(text), juce::Time::getCurrentTime() });
        }

        void paint(Graphics& g) override
        {
            auto const bgCol = findColour(PlugDataColour::sidebarBackgroundColourId);
            auto const activeCol = findColour(PlugDataColour::toolbarActiveColourId);
            bool const dark = (bgCol.getPerceivedBrightness() < 0.5f);

            int y = 8;
            int const w = getWidth();
            if (w <= 20) return;

            auto const bodyFont = Fonts::getDefaultFont().withHeight(13.0f);
            auto const boldFont = Fonts::getBoldFont().withHeight(13.0f);
            auto const monoFont = Fonts::getMonospaceFont().withHeight(12.0f);
            auto const titleFont = Fonts::getBoldFont().withHeight(14.5f);
            auto const secHeaderFont = Fonts::getBoldFont().withHeight(13.5f);
            auto const roleFont = Fonts::getBoldFont().withHeight(11.5f);

            juce::Colour const defaultTextCol = dark ? juce::Colour(0xfff0f2f5) : juce::Colour(0xff181a1f);
            juce::Colour const codeCol = dark ? juce::Colour(0xff79c0ff) : juce::Colour(0xff0969da);

            int const bubbleMaxWidth = std::max(60, w - 24);
            float const innerContentWidth = static_cast<float>(bubbleMaxWidth - 28);

            for (size_t i = 0; i < items.size(); ++i) {
                auto const& m = items[i];
                if (m.role == "thought") continue; // Thought is displayed as live status bar

                bool const isUser = (m.role == "user");
                bool const isError = (m.role == "error");

                juce::Colour roleCol = activeCol;
                juce::String roleName = "Copilot";
                if (isUser) {
                    roleCol = dark ? juce::Colour(0xff9ecbff) : juce::Colour(0xff0969da);
                    roleName = "You";
                } else if (isError) {
                    roleCol = Colours::red;
                    roleName = "Alert";
                }

                auto blocks = parseBlocks(m.text);

                // Precompute bubble content height
                float contentH = 0.0f;
                struct MeasuredBlock {
                    ContentBlock block;
                    juce::TextLayout layout;
                    float height = 0.0f;
                };
                std::vector<MeasuredBlock> measured;
                measured.reserve(blocks.size());

                for (auto const& b : blocks) {
                    MeasuredBlock mb;
                    mb.block = b;

                    if (b.type == ContentBlock::Divider) {
                        mb.height = 8.0f;
                    } else if (b.type == ContentBlock::CodeBlock) {
                        mb.height = b.codeLines.size() * 16.0f + 16.0f;
                    } else {
                        juce::AttributedString as;
                        as.setWordWrap(juce::AttributedString::WordWrap::byWord);

                        juce::Font baseF = bodyFont;
                        if (b.type == ContentBlock::Title) baseF = titleFont;
                        else if (b.type == ContentBlock::SectionHeader) baseF = secHeaderFont;
                        else if (b.type == ContentBlock::SubHeader) baseF = boldFont;

                        for (auto const& sp : b.spans) {
                            juce::Font f = sp.isCode ? monoFont : (sp.isBold ? baseF.boldened() : baseF);
                            if (sp.isItalic) f = f.italicised();

                            juce::Colour c = defaultTextCol;
                            if (sp.isCode) c = codeCol;
                            else if (sp.isBold) {
                                c = (b.type == ContentBlock::SectionHeader) ? codeCol : (dark ? Colours::white : juce::Colour(0xff181a1f));
                            }
                            as.append(sp.text, f, c);
                        }

                        float const availableW = std::max(20.0f, innerContentWidth - b.indentLeft);
                        mb.layout.createLayout(as, availableW);
                        mb.height = mb.layout.getHeight();
                    }

                    contentH += b.topMargin + mb.height + b.bottomMargin;
                    measured.push_back(std::move(mb));
                }

                int const headerAreaH = 20;
                int const bubbleH = static_cast<int>(contentH) + headerAreaH + 16;
                auto bubbleRect = juce::Rectangle<int>(12, y, bubbleMaxWidth, bubbleH);

                // Bubble background
                juce::Colour bubbleBg;
                juce::Colour borderCol;
                if (isUser) {
                    bubbleBg = dark ? juce::Colour(0xff1b2438) : juce::Colour(0xffeef5ff);
                    borderCol = dark ? juce::Colour(0xff2d3d5f) : juce::Colour(0xffd0e2ff);
                } else if (isError) {
                    bubbleBg = dark ? juce::Colour(0xff331c1c) : juce::Colour(0xffffebee);
                    borderCol = dark ? juce::Colour(0xff552626) : juce::Colour(0xffffcdd2);
                } else {
                    bubbleBg = dark ? juce::Colour(0xff1e222b) : juce::Colour(0xffffffff);
                    borderCol = dark ? juce::Colour(0xff323846) : juce::Colour(0xffd6dae2);
                }

                g.setColour(bubbleBg);
                g.fillRoundedRectangle(bubbleRect.toFloat(), 8.0f);

                g.setColour(borderCol);
                g.drawRoundedRectangle(bubbleRect.toFloat().reduced(0.5f), 8.0f, 1.0f);

                // Sleek left accent indicator bar on AI/Error bubbles
                if (!isUser) {
                    g.setColour(roleCol);
                    g.fillRoundedRectangle(bubbleRect.getX() + 2.0f, bubbleRect.getY() + 6.0f, 3.5f, bubbleRect.getHeight() - 12.0f, 1.5f);
                }

                // Role Header
                g.setFont(roleFont);
                g.setColour(roleCol);
                g.drawText(roleName, juce::Rectangle<int>(bubbleRect.getX() + 14, bubbleRect.getY() + 8, bubbleRect.getWidth() - 28, 14),
                           juce::Justification::centredLeft, true);

                // Render each block
                float blockY = static_cast<float>(bubbleRect.getY() + headerAreaH + 8);
                float const blockBaseX = static_cast<float>(bubbleRect.getX() + 14);

                for (auto const& mb : measured) {
                    blockY += mb.block.topMargin;
                    float const currentBlockX = blockBaseX + mb.block.indentLeft;
                    float const currentBlockW = std::max(20.0f, innerContentWidth - mb.block.indentLeft);

                    if (mb.block.type == ContentBlock::Divider) {
                        g.setColour(dark ? juce::Colour(0x35ffffff) : juce::Colour(0x18000000));
                        g.drawHorizontalLine(static_cast<int>(blockY + 4.0f), blockBaseX, blockBaseX + innerContentWidth);
                    } else if (mb.block.type == ContentBlock::CodeBlock) {
                        auto card = juce::Rectangle<float>(currentBlockX, blockY, currentBlockW, mb.height);
                        g.setColour(dark ? juce::Colour(0xff161b22) : juce::Colour(0xfff6f8fa));
                        g.fillRoundedRectangle(card, 5.0f);
                        g.setColour(dark ? juce::Colour(0xff30363d) : juce::Colour(0xffd0d7de));
                        g.drawRoundedRectangle(card, 5.0f, 1.0f);

                        float codeLineY = blockY + 8.0f;
                        g.setFont(monoFont);
                        g.setColour(dark ? juce::Colour(0xffa5d6ff) : juce::Colour(0xff0550ae));
                        for (int ci = 0; ci < mb.block.codeLines.size(); ++ci) {
                            g.drawText(mb.block.codeLines[ci],
                                       juce::Rectangle<float>(currentBlockX + 8.0f, codeLineY, currentBlockW - 16.0f, 16.0f),
                                       juce::Justification::centredLeft, false);
                            codeLineY += 16.0f;
                        }
                    } else {
                        // Bullet dots
                        if (mb.block.type == ContentBlock::Bullet) {
                            float dotX = currentBlockX - 9.0f;
                            float dotY = blockY + 7.0f;
                            g.setColour(roleCol.withAlpha(0.85f));
                            g.fillEllipse(dotX - 2.0f, dotY - 2.0f, 4.0f, 4.0f);
                        } else if (mb.block.type == ContentBlock::SubBullet) {
                            float dotX = currentBlockX - 9.0f;
                            float dotY = blockY + 7.0f;
                            g.setColour(roleCol.withAlpha(0.65f));
                            g.drawEllipse(dotX - 1.8f, dotY - 1.8f, 3.6f, 3.6f, 1.0f);
                        }

                        // Antigravity inline code pills (badges)
                        if (mb.block.hasCode) {
                            for (int li = 0; li < mb.layout.getNumLines(); ++li) {
                                auto const& line = mb.layout.getLine(li);
                                for (auto* run : line.runs) {
                                    if (run->font.getTypefaceName() == monoFont.getTypefaceName() || run->colour == codeCol) {
                                        auto const rx = run->getRunBoundsX();
                                        if (rx.getLength() > 0.5f) {
                                            float px = currentBlockX + line.lineOrigin.x + rx.getStart();
                                            float py = blockY + line.lineOrigin.y - line.ascent;
                                            float pw = rx.getLength();
                                            float ph = line.ascent + line.descent;

                                            auto pill = juce::Rectangle<float>(px - 3.5f, py - 1.0f, pw + 7.0f, ph + 2.0f);
                                            g.setColour(dark ? juce::Colour(0xff2b313a) : juce::Colour(0xffe8ebf0));
                                            g.fillRoundedRectangle(pill, 3.5f);
                                            g.setColour(dark ? juce::Colour(0x35ffffff) : juce::Colour(0x20000000));
                                            g.drawRoundedRectangle(pill, 3.5f, 0.75f);
                                        }
                                    }
                                }
                            }
                        }

                        // Draw layout glyphs
                        mb.layout.draw(g, juce::Rectangle<float>(currentBlockX, blockY, currentBlockW, mb.height));
                    }

                    blockY += mb.height + mb.block.bottomMargin;
                }

                y += bubbleH + 10;
            }

            // Antigravity Live Working / Thinking Indicator
            if (isThinking) {
                int const badgeW = std::min(180, w - 24);
                int const badgeH = 26;
                auto badgeRect = juce::Rectangle<float>(12.0f, static_cast<float>(y), static_cast<float>(badgeW), static_cast<float>(badgeH));

                g.setColour(dark ? juce::Colour(0xff22272e) : juce::Colour(0xfffff8f0));
                g.fillRoundedRectangle(badgeRect, 13.0f);
                g.setColour(dark ? juce::Colour(0xff444c56) : juce::Colour(0xffffe0b2));
                g.drawRoundedRectangle(badgeRect.reduced(0.5f), 13.0f, 1.0f);

                // Animated pulsing amber dot
                float pulse = 0.5f + 0.5f * std::sin(thinkingPhase);
                g.setColour(Colours::orange.withAlpha(0.5f + 0.5f * pulse));
                g.fillEllipse(badgeRect.getX() + 9.0f, badgeRect.getY() + 9.0f, 8.0f, 8.0f);

                // Status text
                g.setFont(Fonts::getDefaultFont().withHeight(12.0f));
                g.setColour(dark ? juce::Colour(0xffc9d1d9) : juce::Colour(0xff57606a));
                g.drawText(thinkingStatus, juce::Rectangle<float>(badgeRect.getX() + 23.0f, badgeRect.getY(), badgeRect.getWidth() - 28.0f, badgeRect.getHeight()),
                           juce::Justification::centredLeft, true);

                y += badgeH + 10;
            }
        }

        int calculateTotalHeight(int width) const
        {
            if (items.empty() && !isThinking) return 40;
            int total = 16;
            int const bubbleMaxWidth = std::max(60, width - 24);
            float const innerContentWidth = static_cast<float>(bubbleMaxWidth - 28);

            auto const bodyFont = Fonts::getDefaultFont().withHeight(13.0f);
            auto const boldFont = Fonts::getBoldFont().withHeight(13.0f);
            auto const monoFont = Fonts::getMonospaceFont().withHeight(12.0f);
            auto const titleFont = Fonts::getBoldFont().withHeight(14.5f);
            auto const secHeaderFont = Fonts::getBoldFont().withHeight(13.5f);

            for (auto const& m : items) {
                if (m.role == "thought") continue;

                auto blocks = parseBlocks(m.text);
                float contentH = 0.0f;

                for (auto const& b : blocks) {
                    if (b.type == ContentBlock::Divider) {
                        contentH += b.topMargin + 8.0f + b.bottomMargin;
                    } else if (b.type == ContentBlock::CodeBlock) {
                        contentH += b.topMargin + (b.codeLines.size() * 16.0f + 16.0f) + b.bottomMargin;
                    } else {
                        juce::AttributedString as;
                        as.setWordWrap(juce::AttributedString::WordWrap::byWord);

                        juce::Font baseF = bodyFont;
                        if (b.type == ContentBlock::Title) baseF = titleFont;
                        else if (b.type == ContentBlock::SectionHeader) baseF = secHeaderFont;
                        else if (b.type == ContentBlock::SubHeader) baseF = boldFont;

                        for (auto const& sp : b.spans) {
                            juce::Font f = sp.isCode ? monoFont : (sp.isBold ? baseF.boldened() : baseF);
                            as.append(sp.text, f, Colours::white);
                        }

                        juce::TextLayout layout;
                        float const availableW = std::max(20.0f, innerContentWidth - b.indentLeft);
                        layout.createLayout(as, availableW);
                        contentH += b.topMargin + layout.getHeight() + b.bottomMargin;
                    }
                }

                int const headerAreaH = 20;
                total += static_cast<int>(contentH) + headerAreaH + 16 + 10;
            }

            if (isThinking) {
                total += 36;
            }

            return total;
        }
    };

    // Antigravity-Style Input Capsule
    class InputDockCapsule final : public Component {
    public:
        CopilotPanel* parent = nullptr;
        SearchEditor editor;
        SmallIconButton plusButton = SmallIconButton(Icons::Add);
        SmallIconButton sendButton = SmallIconButton(Icons::Forward);

        InputDockCapsule(CopilotPanel* p) : parent(p)
        {
            plusButton.setTooltip("Add context / Record take");
            plusButton.onClick = [this] {
                if (parent) parent->triggerPlusMenu();
            };
            addAndMakeVisible(plusButton);

            editor.setTextToShowWhenEmpty("Ask Copilot, @ to mention, / for actions...", findColour(PlugDataColour::sidebarTextColourId).withAlpha(0.6f));
            editor.setFont(Fonts::getDefaultFont().withHeight(13.5f));
            editor.setColour(SearchEditor::textColourId, findColour(PlugDataColour::sidebarTextColourId));
            editor.setColour(SearchEditor::outlineColourId, Colours::transparentBlack);
            editor.setColour(SearchEditor::focusedOutlineColourId, Colours::transparentBlack);
            editor.setColour(SearchEditor::backgroundColourId, Colours::transparentBlack);

            editor.onReturnKey = [this] {
                if (parent) parent->sendMessage();
            };
            addAndMakeVisible(editor);

            sendButton.setTooltip("Send prompt (Enter)");
            sendButton.onClick = [this] {
                if (parent) parent->sendMessage();
            };
            addAndMakeVisible(sendButton);
        }

        void paint(Graphics& g) override
        {
            auto b = getLocalBounds().toFloat().reduced(0.5f);
            auto const bgCol = findColour(PlugDataColour::sidebarBackgroundColourId);
            auto const activeCol = findColour(PlugDataColour::toolbarActiveColourId);
            bool const dark = (bgCol.getPerceivedBrightness() < 0.5f);

            bool const hasFocus = editor.hasKeyboardFocus(true);

            auto capsuleBg = dark ? juce::Colour(0xff181a1f) : juce::Colour(0xffffffff);
            auto capsuleBorder = hasFocus ? activeCol : (dark ? juce::Colour(0xff333842) : juce::Colour(0xffd0d7de));

            g.setColour(capsuleBg);
            g.fillRoundedRectangle(b, 10.0f);

            g.setColour(capsuleBorder);
            g.drawRoundedRectangle(b, 10.0f, hasFocus ? 1.5f : 1.0f);
        }

        void resized() override
        {
            auto b = getLocalBounds().reduced(4, 2);
            plusButton.setBounds(b.removeFromLeft(28).reduced(2));
            sendButton.setBounds(b.removeFromRight(28).reduced(2));
            editor.setBounds(b.reduced(4, 2));
        }
    };

    CopilotPanel(PluginProcessor* processor, PluginEditor* parentEditor)
        : pd(processor)
        , editor(parentEditor)
        , midiWidgetCard(parentEditor, "ai.midi", "MIDI Looper - Drag", juce::Colour(0xffb388ff), "#X obj 0 0 ai.midi 240 64;\n")
        , voiceWidgetCard(parentEditor, "ai.voice", "Mic Capsule - Drag", juce::Colour(0xff00e676), "#X obj 0 0 ai.voice 240 64;\n")
        , inputCapsule(this)
    {
        // Clear Button
        clearButton.setTooltip("Clear conversation history");
        clearButton.onClick = [this] {
            setThinking(false, "");
            chatContent.items.clear();
            updateChatLayout();
            if (pd) {
                if (auto* bridge = pd->getMCPBridge()) {
                    bridge->sendReply("/chat/clear", {});
                }
            }
        };
        addAndMakeVisible(clearButton);

        // Header Title
        headerTitleLabel.setText("Copilot", juce::dontSendNotification);
        headerTitleLabel.setFont(Fonts::getBoldFont().withHeight(15.0f));
        headerTitleLabel.setColour(Label::textColourId, findColour(PlugDataColour::sidebarTextColourId));
        addAndMakeVisible(headerTitleLabel);

        // Rack Label
        rackLabel.setText("MODULE RACK  -  DRAG TO PATCH", juce::dontSendNotification);
        rackLabel.setFont(Fonts::getBoldFont().withHeight(10.0f));
        rackLabel.setColour(Label::textColourId, findColour(PlugDataColour::sidebarTextColourId).withAlpha(0.6f));
        addAndMakeVisible(rackLabel);

        // Drag to patch widgets
        addAndMakeVisible(midiWidgetCard);
        addAndMakeVisible(voiceWidgetCard);

        // Chat Viewport
        viewport.setScrollBarsShown(true, false, false, false);
        viewport.setViewedComponent(&chatContent, false);
        addAndMakeVisible(viewport);

        // Shelf Bar
        shelfBar.onToggle = [this] {
            shelfBar.open = !shelfBar.open;
            resized();
        };
        addAndMakeVisible(shelfBar);

        // Input Capsule Dock
        addAndMakeVisible(inputCapsule);

        // Register listener with MCPBridge if present
        if (pd) {
            if (auto* bridge = pd->getMCPBridge()) {
                bridge->setChatCallback([safeThis = SafePointer<CopilotPanel>(this)](const juce::String& role, const juce::String& text) {
                    if (safeThis != nullptr) {
                        safeThis->receiveMessage(role, text);
                    }
                });
            }
        }

        // Welcome greeting in chat
        chatContent.addMessage("ai", "Hey Jupiter! Ich bin dein Live Copilot. Frag mich was, lass uns Patches bauen oder Signalwege reparieren.");

        startTimerHz(2);
    }

    ~CopilotPanel() override
    {
        stopTimer();
        if (pd) {
            if (auto* bridge = pd->getMCPBridge()) {
                bridge->setChatCallback(nullptr);
            }
        }
    }

    void setTab(PanelTab) {}

    void setThinking(bool active, const juce::String& status)
    {
        chatContent.isThinking = active;
        chatContent.thinkingStatus = status.isNotEmpty() ? status : "Thinking...";
        if (active) {
            startTimerHz(25);
        } else {
            startTimerHz(2);
        }
        updateChatLayout();
    }

    void timerCallback() override
    {
        if (chatContent.isThinking) {
            chatContent.thinkingPhase += 0.2f;
            chatContent.repaint();
        }

        if (!pd) return;
        int takes = static_cast<int>(pd->getMcpMidiTakes().size() + pd->getMcpVoiceTakes().size());
        int images = static_cast<int>(pd->getMcpReferenceImages().size());
        if (takes != shelfBar.takeCount || images != shelfBar.imageCount) {
            shelfBar.takeCount = takes;
            shelfBar.imageCount = images;
            shelfBar.repaint();
        }
    }

    void triggerPlusMenu()
    {
        PopupMenu menu;
        menu.addItem(1, "Record MIDI Take (Keys)");
        menu.addItem(2, "Record Voice Take (Mic)");
        menu.addItem(3, "Attach Reference Image");

        menu.showMenuAsync(PopupMenu::Options().withTargetComponent(&inputCapsule.plusButton), [this](int result) {
            if (result == 1) {
                if (pd && pd->getMCPBridge()) {
                    pd->getMCPBridge()->startMidiCapture();
                }
            } else if (result == 2) {
                if (pd && pd->getMCPBridge()) {
                    pd->getMCPBridge()->startVoiceCapture(1);
                }
            } else if (result == 3) {
                if (editor) {
                    if (auto* cnv = editor->getCurrentCanvas()) {
                        cnv->pasteImageFromClipboard();
                    }
                }
            }
        });
    }

    juce::String getStudioDraft() const { return inputCapsule.editor.getText().trim(); }
    void clearStudioDraft() { inputCapsule.editor.setText("", false); }
    void refreshNotebook() {}
    void refreshStudio() {}
    void deleteAnnotation(size_t) {}
    void askAiAboutRegion(const juce::String&, const juce::String&) {}

    void resized() override
    {
        auto bounds = getLocalBounds();
        if (bounds.getWidth() <= 0) return;

        // 1. Header (height 32)
        auto header = bounds.removeFromTop(32).reduced(10, 2);
        clearButton.setBounds(header.removeFromRight(24));
        header.removeFromRight(6);
        headerTitleLabel.setBounds(header);

        // 2. Rack title (height 14)
        auto rLabel = bounds.removeFromTop(14).reduced(10, 0);
        rackLabel.setBounds(rLabel);

        // 3. Drag-to-canvas widgets strip (height 38)
        auto widgetsStrip = bounds.removeFromTop(38).reduced(10, 2);
        int const cardW = (widgetsStrip.getWidth() - 8) / 2;
        midiWidgetCard.setBounds(widgetsStrip.removeFromLeft(cardW));
        widgetsStrip.removeFromLeft(8);
        voiceWidgetCard.setBounds(widgetsStrip);

        // 4. Bottom dock: Shelf bar (26px) + Input Capsule (46px)
        int const shelfH = 26;
        int const inputBarH = 46;
        auto bottomArea = bounds.removeFromBottom(shelfH + inputBarH + 10);

        inputCapsule.setBounds(bottomArea.removeFromBottom(inputBarH).reduced(10, 2));
        shelfBar.setBounds(bottomArea.removeFromBottom(shelfH).reduced(10, 2));

        // 5. Middle Viewport (Chat conversation)
        viewport.setBounds(bounds.reduced(4, 2));
        updateChatLayout();
    }

    void sendMessage()
    {
        auto text = inputCapsule.editor.getText().trim();
        if (text.isEmpty() || !pd) return;

        inputCapsule.editor.setText("", false);

        juce::StringArray targetIds;
        if (editor) {
            if (auto* cnv = editor->getCurrentCanvas()) {
                for (auto* obj : cnv->getSelectionOfType<Object>()) {
                    if (auto* ptr = obj->getPointer()) {
                        auto const tid = pd->getStableId(ptr);
                        if (tid.isNotEmpty()) targetIds.add(tid);
                    }
                }
            }
        }

        chatContent.addMessage("user", text);
        setThinking(true, "Thinking...");

        if (auto* bridge = pd->getMCPBridge()) {
            bridge->sendSelectionPrompt(text, targetIds, false);
        }
    }

    void showAnnotationNote(const juce::String& text, const juce::String& kind, const juce::StringArray& targetIds)
    {
        receiveMessage("ai", text);
    }

    void receiveMessage(const juce::String& role, const juce::String& text)
    {
        if (role == "thought") {
            setThinking(true, text);
        } else if (role == "ai" || role == "error") {
            setThinking(false, "");
            chatContent.addMessage(role, text);
            updateChatLayout();
        } else if (role == "user") {
            bool isDuplicate = false;
            for (auto it = chatContent.items.rbegin(); it != chatContent.items.rend(); ++it) {
                if (it->role == "user") {
                    if (it->text.trim() == text.trim()) isDuplicate = true;
                    break;
                }
            }
            if (!isDuplicate) {
                chatContent.addMessage("user", text);
                setThinking(true, "Thinking...");
                updateChatLayout();
            }
        }
    }

    void updateChatLayout()
    {
        int const targetH = chatContent.calculateTotalHeight(viewport.getWidth());
        chatContent.setSize(std::max(10, viewport.getWidth()), std::max(targetH, viewport.getHeight()));
        chatContent.repaint();
        viewport.setViewPositionProportionately(0.0f, 1.0f);
    }

private:
    PluginProcessor* pd;
    PluginEditor* editor;

    Label headerTitleLabel;
    Label rackLabel;
    SmallIconButton clearButton = SmallIconButton(Icons::Trash);

    DraggableWidgetCard midiWidgetCard;
    DraggableWidgetCard voiceWidgetCard;

    BouncingViewport viewport;
    ChatListContent chatContent;

    ShelfBar shelfBar;
    InputDockCapsule inputCapsule;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CopilotPanel)
};
