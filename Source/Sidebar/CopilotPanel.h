/*
 // Copyright (c) 2021-2026 Timothy Schoen & Contributors.
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
 */

#pragma once

#include <utility>
#include <vector>

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

    class DraggableWidgetCard final : public ObjectDragAndDrop {
    public:
        DraggableWidgetCard(PluginEditor* e, const juce::String& name, const juce::String& iconStr, const juce::String& patchText)
            : ObjectDragAndDrop(e), widgetName(name), icon(iconStr), patchString(patchText)
        {}

        juce::String getObjectString() override { return patchString; }
        juce::String getPatchStringName() override { return widgetName; }

        void paint(Graphics& g) override
        {
            auto b = getLocalBounds().toFloat().reduced(1.0f);
            auto const bgCol = findColour(PlugDataColour::sidebarBackgroundColourId);
            auto const activeCol = findColour(PlugDataColour::toolbarActiveColourId);
            auto const textCol = findColour(PlugDataColour::sidebarTextColourId);

            bool const over = isMouseOver();
            g.setColour(over ? activeCol.withAlpha(0.22f) : bgCol.contrasting(0.06f));
            g.fillRoundedRectangle(b, 5.0f);
            g.setColour(over ? activeCol.withAlpha(0.8f) : bgCol.contrasting(0.15f));
            g.drawRoundedRectangle(b, 5.0f, 1.0f);

            g.setFont(Font(11.5f, Font::bold));
            g.setColour(over ? activeCol.contrasting(0.1f) : textCol.withAlpha(0.92f));
            g.drawFittedText(icon + " " + widgetName, getLocalBounds().reduced(4, 2), Justification::centred, 1);
        }

        void mouseEnter(const MouseEvent&) override { repaint(); }
        void mouseExit(const MouseEvent&) override { repaint(); }

    private:
        juce::String widgetName;
        juce::String icon;
        juce::String patchString;
    };

    class ShelfBar final : public Component {
    public:
        std::function<void()> onToggle;
        bool open = false;
        int takeCount = 0;
        int imageCount = 0;

        void paint(Graphics& g) override
        {
            auto b = getLocalBounds().toFloat().reduced(1.0f);
            auto const bgCol = findColour(PlugDataColour::sidebarBackgroundColourId);
            auto const activeCol = findColour(PlugDataColour::toolbarActiveColourId);
            auto const textCol = findColour(PlugDataColour::sidebarTextColourId);

            g.setColour(bgCol.contrasting(isMouseOver() ? 0.08f : 0.04f));
            g.fillRoundedRectangle(b, 4.0f);
            g.setColour(bgCol.contrasting(0.12f));
            g.drawRoundedRectangle(b, 4.0f, 1.0f);

            g.setFont(Font(10.5f, Font::bold));
            g.setColour(textCol.withAlpha(0.7f));
            juce::String label = (open ? "v " : "> ") + juce::String("SHELF (") +
                                 juce::String(takeCount) + " takes, " +
                                 juce::String(imageCount) + " images)";
            g.drawText(label, 8, 0, getWidth() - 55, getHeight(), Justification::centredLeft, true);

            g.setFont(Font(10.0f, Font::plain));
            g.setColour(activeCol.withAlpha(0.9f));
            g.drawText(open ? "close" : "browse", getWidth() - 50, 0, 44, getHeight(), Justification::centredRight, true);
        }

        void mouseUp(const MouseEvent&) override { if (onToggle) onToggle(); }
        void mouseEnter(const MouseEvent&) override { repaint(); }
        void mouseExit(const MouseEvent&) override { repaint(); }
    };

    class ChatListContent final : public Component {
    public:
        std::vector<ChatMessage> items;

    private:
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
                juce::String roleName = "Copilot";
                if (isUser) {
                    roleCol = activeCol.contrasting(0.4f);
                    roleName = "You";
                } else if (isThought) {
                    roleCol = Colours::orange.withAlpha(0.9f);
                    roleName = "Working";
                } else if (isError) {
                    roleCol = Colours::red;
                    roleName = "Alert";
                }

                as.append(roleName + "\n", Font(11.0f, Font::bold), roleCol);
                appendMarkdown(as, m.text, Font(13.0f, Font::plain), textCol.withAlpha(isThought ? 0.8f : 1.0f));

                juce::TextLayout layout;
                int const bubbleMaxWidth = std::max(60, w - 24);
                layout.createLayout(as, static_cast<float>(bubbleMaxWidth));

                int const bubbleH = static_cast<int>(layout.getHeight()) + 16;
                auto bubbleRect = juce::Rectangle<int>(12, y, bubbleMaxWidth, bubbleH);

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
                as.append((m.role == "user" ? "You\n" : (m.role == "thought" ? "Working\n" : "Copilot\n")),
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
        , midiWidgetCard(parentEditor, "ai.midi", "[MIDI]", "#X obj 0 0 ai.midi 240 64;\n")
        , voiceWidgetCard(parentEditor, "ai.voice", "[MIC]", "#X obj 0 0 ai.voice 240 64;\n")
    {
        // Clear Button
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

        // Header Title
        headerTitleLabel.setText("Copilot", juce::dontSendNotification);
        headerTitleLabel.setFont(Font(13.5f, Font::bold));
        headerTitleLabel.setColour(Label::textColourId, findColour(PlugDataColour::sidebarTextColourId));
        addAndMakeVisible(headerTitleLabel);

        // Widgets
        addAndMakeVisible(midiWidgetCard);
        addAndMakeVisible(voiceWidgetCard);

        // Viewport
        addAndMakeVisible(viewport);
        viewport.setViewedComponent(&chatContent, false);

        // Shelf Bar
        shelfBar.onToggle = [this] {
            shelfBar.open = !shelfBar.open;
            resized();
        };
        addAndMakeVisible(shelfBar);

        // Input Editor
        inputEditor.setTextToShowWhenEmpty("Describe what to build...", findColour(PlugDataColour::sidebarTextColourId).withAlpha(0.5f));
        inputEditor.onReturnKey = [this] {
            sendMessage();
        };
        addAndMakeVisible(inputEditor);

        // Send Button
        sendButton.setTooltip("Send prompt to Copilot");
        sendButton.onClick = [this] {
            sendMessage();
        };
        addAndMakeVisible(sendButton);

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
        chatContent.items.push_back({
            "ai",
            "Hey Jupiter! Ich bin dein Live Copilot. Frag mich was, lass uns Patches bauen oder Signalwege reparieren.",
            juce::Time::getCurrentTime()
        });

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

    void timerCallback() override
    {
        if (!pd) return;
        int takes = static_cast<int>(pd->getMcpMidiTakes().size() + pd->getMcpVoiceTakes().size());
        int images = static_cast<int>(pd->getMcpReferenceImages().size());
        if (takes != shelfBar.takeCount || images != shelfBar.imageCount) {
            shelfBar.takeCount = takes;
            shelfBar.imageCount = images;
            shelfBar.repaint();
        }
    }

    juce::String getStudioDraft() const { return inputEditor.getText().trim(); }
    void clearStudioDraft() { inputEditor.setText("", false); }
    void refreshNotebook() {}
    void refreshStudio() {}
    void deleteAnnotation(size_t) {}
    void askAiAboutRegion(const juce::String&, const juce::String&) {}

    void resized() override
    {
        auto bounds = getLocalBounds();
        if (bounds.getWidth() <= 0) return;

        // 1. Header (height 30)
        auto header = bounds.removeFromTop(30).reduced(8, 2);
        clearButton.setBounds(header.removeFromRight(24));
        header.removeFromRight(4);
        headerTitleLabel.setBounds(header);

        // 2. Drag-to-canvas widgets strip (height 32)
        auto widgetsStrip = bounds.removeFromTop(32).reduced(8, 2);
        int const cardW = (widgetsStrip.getWidth() - 6) / 2;
        midiWidgetCard.setBounds(widgetsStrip.removeFromLeft(cardW));
        widgetsStrip.removeFromLeft(6);
        voiceWidgetCard.setBounds(widgetsStrip);

        // 3. Bottom dock: Shelf bar (24px) + Input bar (40px)
        int const shelfH = 24;
        int const inputBarH = 40;
        auto bottomArea = bounds.removeFromBottom(shelfH + inputBarH + 6);

        // Input bar
        auto inputBar = bottomArea.removeFromBottom(inputBarH).reduced(6, 4);
        sendButton.setBounds(inputBar.removeFromRight(30));
        inputBar.removeFromRight(4);
        inputEditor.setBounds(inputBar);

        // Shelf Bar
        shelfBar.setBounds(bottomArea.removeFromBottom(shelfH).reduced(6, 1));

        // 4. Middle Viewport (Chat conversation)
        viewport.setBounds(bounds.reduced(2, 2));
        updateChatLayout();
    }

    void sendMessage()
    {
        auto text = inputEditor.getText().trim();
        if (text.isEmpty() || !pd) return;

        inputEditor.setText("", false);

        // Collect selection if any for Chat prompt
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

    void showAnnotationNote(const juce::String& text, const juce::String& kind, const juce::StringArray& targetIds)
    {
        receiveMessage("ai", text);
    }

    void receiveMessage(const juce::String& role, const juce::String& text)
    {
        if (role == "thought") {
            if (!chatContent.items.empty() && chatContent.items.back().role == "thought") {
                chatContent.items.back().text = text;
            } else {
                chatContent.items.push_back({ "thought", text, juce::Time::getCurrentTime() });
            }
        } else if (role == "ai" || role == "error") {
            if (!chatContent.items.empty() && chatContent.items.back().role == "thought") {
                chatContent.items.pop_back();
            }
            chatContent.items.push_back({ role, text, juce::Time::getCurrentTime() });
        } else if (role == "user") {
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

    Label headerTitleLabel;
    SmallIconButton clearButton = SmallIconButton(Icons::Trash);

    DraggableWidgetCard midiWidgetCard;
    DraggableWidgetCard voiceWidgetCard;

    BouncingViewport viewport;
    ChatListContent chatContent;

    ShelfBar shelfBar;

    SearchEditor inputEditor;
    SmallIconButton sendButton = SmallIconButton(Icons::Forward);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CopilotPanel)
};
