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
        DraggableWidgetCard(PluginEditor* e, const juce::String& name, const juce::String& sub, juce::Colour dotCol, const juce::String& patchText)
            : ObjectDragAndDrop(e), widgetName(name), subText(sub), accentDot(dotCol), patchString(patchText)
        {}

        juce::String getObjectString() override { return patchString; }
        juce::String getPatchStringName() override { return widgetName; }

        void paint(Graphics& g) override
        {
            auto b = getLocalBounds().toFloat().reduced(1.0f);
            auto const bgCol = findColour(PlugDataColour::sidebarBackgroundColourId);
            auto const activeCol = findColour(PlugDataColour::toolbarActiveColourId);
            bool const dark = (bgCol.getPerceivedBrightness() < 0.5f);
            bool const over = isMouseOver();

            juce::Colour cardBg = dark
                ? (over ? juce::Colour(0xff262a34) : juce::Colour(0xff1a1d24))
                : (over ? juce::Colour(0xfff0f2f6) : juce::Colour(0xffffffff));

            juce::Colour borderCol = over ? activeCol.withAlpha(0.85f) : (dark ? juce::Colour(0xff353a47) : juce::Colour(0xffd2d6df));

            g.setColour(cardBg);
            g.fillRoundedRectangle(b, 6.0f);
            g.setColour(borderCol);
            g.drawRoundedRectangle(b, 6.0f, over ? 1.4f : 1.0f);

            // Glowing accent indicator dot
            g.setColour(accentDot.withAlpha(over ? 1.0f : 0.85f));
            g.fillEllipse(b.getX() + 9.0f, b.getY() + 9.0f, 6.0f, 6.0f);

            // Title
            g.setFont(Fonts::getBoldFont().withHeight(12.5f));
            g.setColour(dark ? juce::Colour(0xfff0f2f5) : juce::Colour(0xff181a1f));
            g.drawText(widgetName, b.getX() + 20.0f, b.getY() + 4.0f, b.getWidth() - 24.0f, 15.0f, Justification::left, true);

            // Subtitle
            g.setFont(Fonts::getDefaultFont().withHeight(10.0f));
            g.setColour(dark ? juce::Colour(0xff9aa0a6) : juce::Colour(0xff606670));
            g.drawText(subText, b.getX() + 20.0f, b.getY() + 18.0f, b.getWidth() - 24.0f, 13.0f, Justification::left, true);
        }

        void mouseEnter(const MouseEvent&) override { repaint(); }
        void mouseExit(const MouseEvent&) override { repaint(); }

    private:
        juce::String widgetName;
        juce::String subText;
        juce::Colour accentDot;
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
            bool const dark = (bgCol.getPerceivedBrightness() < 0.5f);

            g.setColour(dark ? (isMouseOver() ? juce::Colour(0xff252932) : juce::Colour(0xff1c1f26))
                             : (isMouseOver() ? juce::Colour(0xffe8ebf0) : juce::Colour(0xfff3f5f8)));
            g.fillRoundedRectangle(b, 5.0f);
            g.setColour(dark ? juce::Colour(0xff323742) : juce::Colour(0xffd5d8e0));
            g.drawRoundedRectangle(b, 5.0f, 1.0f);

            g.setFont(Fonts::getSemiBoldFont().withHeight(11.0f));
            g.setColour(dark ? juce::Colour(0xffcfd3dc) : juce::Colour(0xff2d3138));
            juce::String label = (open ? "v  " : ">  ") + juce::String("TAPE SHELF  (") +
                                 juce::String(takeCount) + " takes, " +
                                 juce::String(imageCount) + " images)";
            g.drawText(label, 10, 0, getWidth() - 65, getHeight(), Justification::centredLeft, true);

            g.setFont(Fonts::getBoldFont().withHeight(10.5f));
            g.setColour(activeCol);
            g.drawText(open ? "HIDE" : "BROWSE", getWidth() - 58, 0, 50, getHeight(), Justification::centredRight, true);
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
            auto const mono = Fonts::getMonospaceFont().withHeight(base.getHeight());
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
            bool const dark = (bgCol.getPerceivedBrightness() < 0.5f);

            int y = 8;
            int const w = getWidth();
            if (w <= 20) return;

            auto const bodyFont = Fonts::getDefaultFont().withHeight(13.5f);
            auto const roleFont = Fonts::getBoldFont().withHeight(11.5f);

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
                    roleCol = dark ? juce::Colour(0xff9ecbff) : juce::Colour(0xff0969da);
                    roleName = "You";
                } else if (isThought) {
                    roleCol = Colours::orange.withAlpha(0.95f);
                    roleName = "Working";
                } else if (isError) {
                    roleCol = Colours::red;
                    roleName = "Alert";
                }

                as.append(roleName + "\n", roleFont, roleCol);

                juce::Colour textCol = dark ? juce::Colour(0xfff0f2f5) : juce::Colour(0xff181a1f);
                if (isThought) textCol = dark ? juce::Colour(0xffbdc1c6) : juce::Colour(0xff57606a);

                appendMarkdown(as, m.text, bodyFont, textCol);

                juce::TextLayout layout;
                int const bubbleMaxWidth = std::max(60, w - 24);
                layout.createLayout(as, static_cast<float>(bubbleMaxWidth - 18));

                int const bubbleH = static_cast<int>(layout.getHeight()) + 18;
                auto bubbleRect = juce::Rectangle<int>(12, y, bubbleMaxWidth, bubbleH);

                // Bubble background
                juce::Colour bubbleBg;
                juce::Colour borderCol;
                if (isUser) {
                    bubbleBg = dark ? juce::Colour(0xff1e2638) : juce::Colour(0xffeef5ff);
                    borderCol = dark ? juce::Colour(0xff314260) : juce::Colour(0xffd0e2ff);
                } else if (isThought) {
                    bubbleBg = dark ? juce::Colour(0xff26201a) : juce::Colour(0xfffff8f0);
                    borderCol = dark ? juce::Colour(0xff453322) : juce::Colour(0xffffe0b2);
                } else if (isError) {
                    bubbleBg = dark ? juce::Colour(0xff331c1c) : juce::Colour(0xffffebee);
                    borderCol = dark ? juce::Colour(0xff552626) : juce::Colour(0xffffcdd2);
                } else {
                    bubbleBg = dark ? juce::Colour(0xff20232a) : juce::Colour(0xffffffff);
                    borderCol = dark ? juce::Colour(0xff333842) : juce::Colour(0xffd6dae2);
                }

                g.setColour(bubbleBg);
                g.fillRoundedRectangle(bubbleRect.toFloat(), 7.0f);

                g.setColour(borderCol);
                g.drawRoundedRectangle(bubbleRect.toFloat().reduced(0.5f), 7.0f, 1.0f);

                // Sleek left accent indicator bar on AI/Thought bubbles
                if (!isUser) {
                    g.setColour(roleCol);
                    g.fillRoundedRectangle(bubbleRect.getX() + 2.0f, bubbleRect.getY() + 5.0f, 3.5f, bubbleRect.getHeight() - 10.0f, 1.5f);
                }

                // Draw layout
                layout.draw(g, juce::Rectangle<float>(bubbleRect.getX() + 11.0f, bubbleRect.getY() + 8.0f,
                                                      bubbleRect.getWidth() - 18.0f, layout.getHeight()));

                y += bubbleH + 10;
            }
        }

        int calculateTotalHeight(int width) const
        {
            if (items.empty()) return 40;
            int total = 16;
            int const bubbleMaxWidth = std::max(60, width - 24);
            auto const bodyFont = Fonts::getDefaultFont().withHeight(13.5f);
            auto const roleFont = Fonts::getBoldFont().withHeight(11.5f);

            for (auto const& m : items) {
                juce::AttributedString as;
                as.setWordWrap(juce::AttributedString::WordWrap::byWord);
                as.append((m.role == "user" ? "You\n" : (m.role == "thought" ? "Working\n" : "Copilot\n")),
                          roleFont, Colours::white);
                appendMarkdown(as, m.text, bodyFont, Colours::white);

                juce::TextLayout layout;
                layout.createLayout(as, static_cast<float>(bubbleMaxWidth - 18));
                total += static_cast<int>(layout.getHeight()) + 28;
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
            auto b = getLocalBounds().toFloat().reduced(1.0f);
            auto const bgCol = findColour(PlugDataColour::sidebarBackgroundColourId);
            auto const activeCol = findColour(PlugDataColour::toolbarActiveColourId);
            bool const dark = (bgCol.getPerceivedBrightness() < 0.5f);

            juce::Colour capsuleBg = dark ? juce::Colour(0xff1e2129) : juce::Colour(0xffffffff);
            juce::Colour borderCol = editor.hasKeyboardFocus(true)
                ? activeCol.withAlpha(0.85f)
                : (dark ? juce::Colour(0xff363c48) : juce::Colour(0xffcfd4dd));

            g.setColour(capsuleBg);
            g.fillRoundedRectangle(b, 8.0f);
            g.setColour(borderCol);
            g.drawRoundedRectangle(b, 8.0f, editor.hasKeyboardFocus(true) ? 1.5f : 1.0f);
        }

        void resized() override
        {
            auto b = getLocalBounds().reduced(4, 3);
            plusButton.setBounds(b.removeFromLeft(26).reduced(1));
            b.removeFromLeft(3);
            sendButton.setBounds(b.removeFromRight(26).reduced(1));
            b.removeFromRight(3);
            editor.setBounds(b);
        }
    };

    explicit CopilotPanel(PluginProcessor* instance, PluginEditor* parentEditor)
        : pd(instance)
        , editor(parentEditor)
        , midiWidgetCard(parentEditor, "ai.midi", "MIDI Looper - Drag", juce::Colour(0xffb388ff), "#X obj 0 0 ai.midi 240 64;\n")
        , voiceWidgetCard(parentEditor, "ai.voice", "Mic Capsule - Drag", juce::Colour(0xff00e676), "#X obj 0 0 ai.voice 240 64;\n")
        , inputCapsule(this)
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
        headerTitleLabel.setFont(Fonts::getBoldFont().withHeight(15.0f));
        headerTitleLabel.setColour(Label::textColourId, findColour(PlugDataColour::sidebarTextColourId));
        addAndMakeVisible(headerTitleLabel);

        // Rack Label
        rackLabel.setText("MODULE RACK  *  DRAG TO PATCH", juce::dontSendNotification);
        rackLabel.setFont(Fonts::getSemiBoldFont().withHeight(9.5f));
        rackLabel.setColour(Label::textColourId, findColour(PlugDataColour::sidebarTextColourId).withAlpha(0.6f));
        addAndMakeVisible(rackLabel);

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

    void triggerPlusMenu()
    {
        PopupMenu menu;
        menu.addItem(1, "Record MIDI Take (Keys)");
        menu.addItem(2, "Record Voice Take (Mic)");
        menu.addItem(3, "Attach Reference Image");

        menu.showMenuAsync(PopupMenu::Options().withTargetComponent(&inputCapsule.plusButton), [this](int result) {
            if (result == 1) {
                // Record MIDI Take
                if (pd && pd->getMCPBridge()) {
                    pd->getMCPBridge()->startMidiCapture();
                }
            } else if (result == 2) {
                // Record Voice Take
                if (pd && pd->getMCPBridge()) {
                    pd->getMCPBridge()->startVoiceCapture(1);
                }
            } else if (result == 3) {
                // Attach Image from clipboard or file
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

        // 4. Bottom dock: Shelf bar (24px) + Input Capsule (44px)
        int const shelfH = 26;
        int const inputBarH = 46;
        auto bottomArea = bounds.removeFromBottom(shelfH + inputBarH + 10);

        // Input capsule
        inputCapsule.setBounds(bottomArea.removeFromBottom(inputBarH).reduced(10, 2));

        // Shelf Bar
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
