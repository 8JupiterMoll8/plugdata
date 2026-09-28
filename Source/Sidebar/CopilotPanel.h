/*
 // Copyright (c) 2021-2026 Timothy Schoen & Contributors.
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
 */

#pragma once

#include <utility>
#include <vector>

#include "Utility/CachedStringWidth.h"
#include "Components/BouncingViewport.h"
#include "Components/SearchEditor.h"
#include "Components/Buttons.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "Canvas.h"
#include "Pd/MCPBridge.h"

class CopilotPanel final : public Component {
public:
    struct ChatMessage {
        juce::String role; // "user", "thought", "ai", "error"
        juce::String text;
        juce::Time time;
    };

    class ChatListContent final : public Component {
    public:
        std::vector<ChatMessage> items;

    private:
        // Minimal markdown so AI emphasis renders instead of showing raw markers.
        // Supports **bold**, `code`, *italic*, "- " bullets and "#" headings.
        static void appendInline(juce::AttributedString& as, juce::String const& s, juce::Font const& base, juce::Colour col, bool forceBold)
        {
            auto const mono = juce::Font(juce::Font::getDefaultMonospacedFontName(), base.getHeight(), juce::Font::plain);
            bool bold = forceBold, italic = false, code = false;
            juce::String run;
            auto flush = [&] {
                if (run.isEmpty()) return;
                juce::Font f = code ? mono : base;
                if (bold) f = f.boldened();
                if (italic) f = f.italicised();
                as.append(run, f, col);
                run.clear();
            };
            int const n = s.length();
            for (int i = 0; i < n; ++i) {
                auto const c = s[i];
                if (c == '*' && i + 1 < n && s[i + 1] == '*') { flush(); bold = !bold; ++i; continue; }
                if (c == '`') { flush(); code = !code; continue; }
                if (c == '*') { flush(); italic = !italic; continue; }
                run += c;
            }
            flush();
        }

        static void appendMarkdown(juce::AttributedString& as, juce::String const& text, juce::Font const& base, juce::Colour col)
        {
            auto const lines = juce::StringArray::fromLines(text);
            for (int li = 0; li < lines.size(); ++li) {
                auto line = lines[li];
                bool heading = false;
                if (line.startsWithChar('#')) {
                    heading = true;
                    int k = 0;
                    while (k < line.length() && (line[k] == '#' || line[k] == ' ')) ++k;
                    line = line.substring(k);
                } else if (line.startsWith("- ") || line.startsWith("+ ")) {
                    line = juce::String::charToString((juce::juce_wchar) 0x2022) + "  " + line.substring(2);
                }
                appendInline(as, line, base, col, heading);
                if (li < lines.size() - 1) as.append("\n", base, col);
            }
        }

    public:
        void paint(Graphics& g) override
        {
            auto const bgCol = findColour(PlugDataColour::sidebarBackgroundColourId);
            auto const activeCol = findColour(PlugDataColour::toolbarActiveColourId);
            auto const textCol = findColour(PlugDataColour::sidebarTextColourId);

            int y = 8;
            int const w = getWidth();
            if (w <= 20) return;

            for (size_t i = 0; i < items.size(); ++i) {
                auto const& m = items[i];
                bool const isUser = (m.role == "user");
                bool const isThought = (m.role == "thought");
                bool const isError = (m.role == "error");

                juce::AttributedString as;
                as.setWordWrap(juce::AttributedString::WordWrap::byWord);

                juce::Colour roleCol = activeCol;
                juce::String roleName = "✦ Copilot";
                if (isUser) {
                    roleCol = activeCol.contrasting(0.4f);
                    roleName = "You";
                } else if (isThought) {
                    roleCol = Colours::orange.withAlpha(0.9f);
                    roleName = "⚡ Working";
                } else if (isError) {
                    roleCol = Colours::red;
                    roleName = "⚠️ Alert";
                }

                as.append(roleName + "\n", Font(11.0f, Font::bold), roleCol);
                appendMarkdown(as, m.text, Font(13.0f, Font::plain), textCol.withAlpha(isThought ? 0.8f : 1.0f));

                juce::TextLayout layout;
                int const bubbleMaxWidth = std::max(60, w - 24);
                layout.createLayout(as, static_cast<float>(bubbleMaxWidth));

                int const bubbleH = static_cast<int>(layout.getHeight()) + 16;
                auto bubbleRect = juce::Rectangle<int>(12, y, bubbleMaxWidth, bubbleH);

                // Bubble background
                juce::Colour bubbleBg;
                if (isUser) {
                    bubbleBg = activeCol.withAlpha(0.15f);
                } else if (isThought) {
                    bubbleBg = Colours::orange.withAlpha(0.08f);
                } else if (isError) {
                    bubbleBg = Colours::red.withAlpha(0.15f);
                } else {
                    bubbleBg = bgCol.contrasting(0.06f);
                }

                g.setColour(bubbleBg);
                g.fillRoundedRectangle(bubbleRect.toFloat(), Corners::defaultCornerRadius);

                g.setColour(bubbleBg.contrasting(0.12f));
                g.drawRoundedRectangle(bubbleRect.toFloat().reduced(0.5f), Corners::defaultCornerRadius, 1.0f);

                // Draw layout
                layout.draw(g, juce::Rectangle<float>(bubbleRect.getX() + 8.0f, bubbleRect.getY() + 7.0f,
                                                      bubbleRect.getWidth() - 16.0f, layout.getHeight()));

                y += bubbleH + 8;
            }
        }

        int calculateTotalHeight(int width) const
        {
            if (items.empty()) return 40;
            int total = 16;
            int const bubbleMaxWidth = std::max(60, width - 24);

            for (auto const& m : items) {
                juce::AttributedString as;
                as.setWordWrap(juce::AttributedString::WordWrap::byWord);
                as.append((m.role == "user" ? "You\n" : (m.role == "thought" ? "⚡ Working\n" : "✦ Copilot\n")),
                          Font(11.0f, Font::bold), Colours::white);
                appendMarkdown(as, m.text, Font(13.0f, Font::plain), Colours::white);

                juce::TextLayout layout;
                layout.createLayout(as, static_cast<float>(bubbleMaxWidth));
                total += static_cast<int>(layout.getHeight()) + 24;
            }
            return total;
        }
    };

    explicit CopilotPanel(PluginProcessor* instance, PluginEditor* parentEditor)
        : pd(instance)
        , editor(parentEditor)
    {
        // Add chat viewport
        addAndMakeVisible(viewport);
        chatContent.setSize(200, 100);
        viewport.setViewedComponent(&chatContent, false);

        // Input Editor
        inputEditor.setTextToShowWhenEmpty("Ask Copilot or type prompt...", findColour(PlugDataColour::sidebarTextColourId).withAlpha(0.5f));
        inputEditor.onReturnKey = [this] {
            sendMessage();
        };
        addAndMakeVisible(inputEditor);

        // Send Button
        sendButton.setTooltip("Send prompt to AI Copilot");
        sendButton.onClick = [this] {
            sendMessage();
        };
        addAndMakeVisible(sendButton);

        // Clear Chat Button
        clearButton.setTooltip("Clear conversation history");
        clearButton.onClick = [this] {
            chatContent.items.clear();
            updateChatLayout();
            if (pd) {
                if (auto* bridge = pd->getMCPBridge()) {
                    bridge->sendReply("/chat/clear", {});
                }
            }
        };
        addAndMakeVisible(clearButton);

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

        // Welcome greeting
        chatContent.items.push_back({
            "ai",
            "Hey Jupiter! Ich bin dein Live Copilot. Frag mich was, lass uns Patches bauen oder Signalwege reparieren.",
            juce::Time::getCurrentTime()
        });
        updateChatLayout();
    }

    ~CopilotPanel() override
    {
        if (pd) {
            if (auto* bridge = pd->getMCPBridge()) {
                bridge->setChatCallback(nullptr);
            }
        }
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        if (bounds.getWidth() <= 0) return;

        // Top Header bar
        auto header = bounds.removeFromTop(32).reduced(4, 2);
        clearButton.setBounds(header.removeFromRight(28));

        // Bottom Input bar (height 36)
        auto inputBar = bounds.removeFromBottom(40).reduced(6, 4);
        sendButton.setBounds(inputBar.removeFromRight(30));
        inputBar.removeFromRight(4);
        inputEditor.setBounds(inputBar);

        // Middle Viewport
        viewport.setBounds(bounds);
        updateChatLayout();
    }

    void sendMessage()
    {
        auto text = inputEditor.getText().trim();
        if (text.isEmpty() || !pd) return;

        inputEditor.setText("", false);

        // Collect selection if any
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

        // Add user message to display
        chatContent.items.push_back({ "user", text, juce::Time::getCurrentTime() });

        // Add thinking status
        chatContent.items.push_back({ "thought", "Thinking...", juce::Time::getCurrentTime() });
        updateChatLayout();

        // Dispatch to MCP server
        if (auto* bridge = pd->getMCPBridge()) {
            bridge->sendSelectionPrompt(text, targetIds, false);
        }
    }

    void receiveMessage(const juce::String& role, const juce::String& text)
    {
        // If it's a thought update, update or append
        if (role == "thought") {
            if (!chatContent.items.empty() && chatContent.items.back().role == "thought") {
                chatContent.items.back().text = text;
            } else {
                chatContent.items.push_back({ "thought", text, juce::Time::getCurrentTime() });
            }
        } else if (role == "ai" || role == "error") {
            // Replace any pending thought bubble
            if (!chatContent.items.empty() && chatContent.items.back().role == "thought") {
                chatContent.items.pop_back();
            }
            chatContent.items.push_back({ role, text, juce::Time::getCurrentTime() });
        } else if (role == "user") {
            // Check if this was already added locally (e.g. from the pill)
            if (chatContent.items.empty() || chatContent.items.back().text != text) {
                chatContent.items.push_back({ "user", text, juce::Time::getCurrentTime() });
                chatContent.items.push_back({ "thought", "Thinking...", juce::Time::getCurrentTime() });
            }
        }

        updateChatLayout();
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

    BouncingViewport viewport;
    ChatListContent chatContent;

    SearchEditor inputEditor;
    SmallIconButton sendButton = SmallIconButton(Icons::Forward);
    SmallIconButton clearButton = SmallIconButton(Icons::Trash);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CopilotPanel)
};
