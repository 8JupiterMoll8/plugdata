/*
 // Copyright (c) 2021-2026 Timothy Schoen & Contributors.
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
 */
// The Copilot talk's small markdown reader (moved unchanged from CopilotPanel.h's ChatListContent when the talk became a
// notebook, design round 2026-11-19, step C3): text -> blocks (title, headers, bullets, paragraphs, code, divider) of inline
// spans (bold, italic, code, a [pd object] badge). It only parses; CopilotTalkView.h lays out and draws. Message thread.
#pragma once

#include <vector>

struct CopilotMarkdown {
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

        // The ellipsis: an answer the server had to cut ends in it; three dots every font draws (design round 2026-11-19: a cut line
        // ended in an arrow-like mark on the owner's screen).
        s = s.replace(juce::String::fromUTF8("\xe2\x80\xa6"), "..."); // …

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
};
