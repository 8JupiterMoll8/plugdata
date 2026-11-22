/*
 // Copyright (c) 2021-2026 Timothy Schoen & Contributors.
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
 */
// The Copilot sidebar panel (design round 2026-11-19, picture P): cream paper, ink text, in every theme. A title row, three
// tabs, and the input docked at the bottom on every tab:
//   widgets  the module rack, one card per ai.* widget with its thumbnail, drag a card to the patch (CopilotModuleRack.h)
//   talk     the conversation as a notebook (CopilotTalkView.h; the lines are Mcp/McpCopilotTalk.h's TalkList): the artist's
//            words as a black block, the AI's answer as plain text under it, one ask and its answers one group
//   tapes    the takes and images the shelf bar counted before
// The chosen tab lives in memory only. A new AI or waiting line while another tab shows puts a small ink dot after the word
// "talk"; nothing here switches the tab by itself except the artist's own send (sendMessage). A click on a canvas tag adds NO line
// to the talk: tags stay on the canvas.
//
// Threads: everything here runs on the message thread. receiveMessage is called by the bridge's chat callback, which
// MCPBridge::postChatMessage reaches through MessageManager::callAsync (guarded by a SafePointer), and by the canvas on the
// artist's own sends; every write to the TalkList goes through it or addArtistLine.
#pragma once

#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "Utility/CachedStringWidth.h"
#include "Utility/Fonts.h"
#include "Components/BouncingViewport.h"
#include "Components/Buttons.h"
#include "Components/ObjectDragAndDrop.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "Canvas.h"
#include "CanvasViewport.h"
#include "Object.h"
#include "Pd/MCPBridge.h"
#include "Sidebar/CopilotLook.h"
#include "Sidebar/CopilotModuleRack.h"
#include "Sidebar/CopilotTalkView.h"

class CopilotPanel final : public Component, private juce::Timer {
public:
    enum class PanelTab { Widgets, Talk, Tapes };

    //==============================================================================
    // A tab: plain lower-case text; the chosen one sits on a slightly darker cream (the ink at a low alpha). The talk tab can
    // carry a small filled ink dot after its word: "something new is in there".
    class TabButton final : public Component {
    public:
        explicit TabButton(juce::String const& text)
            : label(text)
        {
            setMouseCursor(MouseCursor::PointingHandCursor);
        }

        std::function<void()> onClick;

        void setSelected(bool const shouldBeSelected)
        {
            if (selected == shouldBeSelected)
                return;
            selected = shouldBeSelected;
            repaint();
        }

        void setDot(bool const shouldShow)
        {
            if (dot == shouldShow)
                return;
            dot = shouldShow;
            repaint();
        }

        bool hasDot() const { return dot; }

        int getWantedWidth() const
        {
            return static_cast<int>(std::ceil(CachedFontStringWidth::get()->calculateStringWidth(Fonts::getDefaultFont().withHeight(textSize), label))) + 2 * hPad + dotRoom;
        }

        void paint(Graphics& g) override
        {
            auto const b = getLocalBounds().toFloat().reduced(0.0f, 2.0f);
            if (selected || hovered) {
                g.setColour(copilotlook::wash(selected ? 0.09f : 0.05f));
                g.fillRoundedRectangle(b, mcp::aiRadius);
            }
            auto const font = Fonts::getDefaultFont().withHeight(textSize);
            g.setFont(font);
            g.setColour(copilotlook::ink());
            auto const textW = static_cast<float>(CachedFontStringWidth::get()->calculateStringWidth(font, label));
            auto const textArea = Rectangle<float>(b.getX() + hPad, b.getY(), textW + 2.0f, b.getHeight());
            g.drawText(label, textArea, Justification::centredLeft, false);
            if (dot)
                g.fillEllipse(textArea.getX() + textW + 6.0f, b.getCentreY() - 2.5f, 5.0f, 5.0f);
        }

        void mouseEnter(MouseEvent const&) override { hovered = true; repaint(); }
        void mouseExit(MouseEvent const&) override { hovered = false; repaint(); }

        void mouseUp(MouseEvent const& e) override
        {
            if (!e.mouseWasDraggedSinceMouseDown() && contains(e.getPosition()) && onClick)
                onClick();
        }

    private:
        static constexpr float textSize = 13.0f;
        static constexpr int hPad = 9;
        static constexpr int dotRoom = 10; // the room of the dot after the word

        juce::String label;
        bool selected = false;
        bool hovered = false;
        bool dot = false;
    };

    //==============================================================================
    // A small icon in ink on the cream (SmallIconButton takes the theme's colours, which are not ink on cream in a dark theme).
    class IconButton final : public Component
        , public SettableTooltipClient {
    public:
        explicit IconButton(juce::String const& iconToShow)
            : icon(iconToShow)
        {
            setMouseCursor(MouseCursor::PointingHandCursor);
        }

        std::function<void()> onClick;

        void paint(Graphics& g) override
        {
            Fonts::drawIcon(g, icon, getLocalBounds(), copilotlook::ink(hovered ? 1.0f : 0.65f), iconSize);
        }

        void mouseEnter(MouseEvent const&) override { hovered = true; repaint(); }
        void mouseExit(MouseEvent const&) override { hovered = false; repaint(); }

        void mouseUp(MouseEvent const& e) override
        {
            if (!e.mouseWasDraggedSinceMouseDown() && contains(e.getPosition()) && onClick)
                onClick();
        }

    private:
        static constexpr int iconSize = 12;
        juce::String icon;
        bool hovered = false;
    };

    //==============================================================================
    // The tapes tab: what the shelf bar counted (MIDI and voice takes, reference images). The shelf had no list behind its
    // count, so this tab is those counts as grey lines, or "no tapes yet".
    class TapesView final : public Component {
    public:
        void setCounts(int takes, int images)
        {
            if (takes == takeCount && images == imageCount)
                return;
            takeCount = takes;
            imageCount = images;
            repaint();
        }

        void paint(Graphics& g) override
        {
            g.setFont(Fonts::getDefaultFont().withHeight(13.0f));
            g.setColour(copilotlook::grey());
            juce::StringArray lines;
            if (takeCount > 0)
                lines.add(juce::String(takeCount) + (takeCount == 1 ? " take" : " takes"));
            if (imageCount > 0)
                lines.add(juce::String(imageCount) + (imageCount == 1 ? " image" : " images"));
            if (lines.isEmpty())
                lines.add("no tapes yet");
            int y = 12;
            for (auto const& line : lines) {
                g.drawText(line, Rectangle<int>(mcp::aiPadding, y, getWidth() - 2 * mcp::aiPadding, 18), Justification::centredLeft, true);
                y += 20;
            }
        }

    private:
        int takeCount = 0;
        int imageCount = 0;
    };

    //==============================================================================
    // The input, docked at the bottom on every tab: cream, 1 px ink outline, 2 px radius.
    class InputDockCapsule final : public Component {
    public:
        CopilotPanel* parent = nullptr;
        TextEditor editor;
        IconButton plusButton = IconButton(Icons::Add);
        IconButton sendButton = IconButton(Icons::Forward);

        explicit InputDockCapsule(CopilotPanel* p)
            : parent(p)
        {
            plusButton.setTooltip("Add context / Record take");
            plusButton.onClick = [this] {
                if (parent) parent->triggerPlusMenu();
            };
            addAndMakeVisible(plusButton);

            editor.getProperties().set("NoBackground", true);
            editor.getProperties().set("NoOutline", true);
            editor.setMultiLine(false);
            editor.setReturnKeyStartsNewLine(false);
            editor.setScrollbarsShown(false);
            editor.setTextToShowWhenEmpty("write to the AI", copilotlook::grey());
            editor.setFont(Fonts::getDefaultFont().withHeight(14.0f));
            editor.setColour(TextEditor::textColourId, copilotlook::ink());
            editor.setColour(TextEditor::backgroundColourId, Colours::transparentBlack);
            editor.setColour(TextEditor::outlineColourId, Colours::transparentBlack);
            editor.setColour(TextEditor::focusedOutlineColourId, Colours::transparentBlack);
            editor.setColour(TextEditor::highlightColourId, copilotlook::wash(0.18f));
            editor.setColour(TextEditor::highlightedTextColourId, copilotlook::ink());
            editor.setColour(CaretComponent::caretColourId, copilotlook::ink());
            editor.onReturnKey = [this] {
                if (parent) parent->sendMessage();
            };
            addAndMakeVisible(editor);

            sendButton.setTooltip("Send (Enter)");
            sendButton.onClick = [this] {
                if (parent) parent->sendMessage();
            };
            addAndMakeVisible(sendButton);
        }

        void paint(Graphics& g) override
        {
            auto const b = getLocalBounds().toFloat().reduced(0.5f);
            g.setColour(copilotlook::cream());
            g.fillRoundedRectangle(b, mcp::aiRadius);
            g.setColour(copilotlook::ink());
            g.drawRoundedRectangle(b, mcp::aiRadius, editor.hasKeyboardFocus(true) ? mcp::aiLineWidthActive : mcp::aiLineWidth);
        }

        // The frame is 2 px while the field has the keyboard focus
        void focusOfChildComponentChanged(FocusChangeType) override { repaint(); }

        void resized() override
        {
            auto b = getLocalBounds().reduced(4, 2);
            plusButton.setBounds(b.removeFromLeft(28).reduced(2));
            sendButton.setBounds(b.removeFromRight(28).reduced(2));
            editor.setBounds(b.reduced(4, 2));
        }
    };

    //==============================================================================
    CopilotPanel(PluginProcessor* processor, PluginEditor* parentEditor)
        : pd(processor)
        , editor(parentEditor)
        , rack(parentEditor)
        , inputCapsule(this)
    {
        // Clear button: the talk starts over
        clearButton.setTooltip("Clear conversation history");
        clearButton.onClick = [this] {
            setThinking(false, "");
            talkView.list.clear();
            talkDot = false;
            updateTabs();
            updateTalkLayout();
            if (pd) {
                if (auto* bridge = pd->getMcp()->getMCPBridge()) {
                    bridge->sendReply("/chat/clear", {});
                }
            }
        };
        addAndMakeVisible(clearButton);

        // Header title
        headerTitleLabel.setText("Copilot", juce::dontSendNotification);
        headerTitleLabel.setFont(Fonts::getSemiBoldFont().withHeight(15.0f));
        headerTitleLabel.setColour(Label::textColourId, copilotlook::ink());
        headerTitleLabel.setInterceptsMouseClicks(false, false);
        addAndMakeVisible(headerTitleLabel);

        // The tab row: widgets, talk, tapes
        for (auto const& entry : tabTable) {
            auto& button = tabButtons[static_cast<size_t>(entry.tab)];
            button = std::make_unique<TabButton>(entry.label);
            button->onClick = [this, tab = entry.tab] { setTab(tab); };
            addAndMakeVisible(*button);
        }

        // The module rack: one card per ai.* widget, in groups, drag to patch (CopilotModuleRack.h)
        rackViewport.setScrollBarsShown(true, false, false, false);
        rackViewport.setViewedComponent(&rack, false);
        addAndMakeVisible(rackViewport);

        // Talk
        talkViewport.setScrollBarsShown(true, false, false, false);
        talkViewport.setViewedComponent(&talkView, false);
        addAndMakeVisible(talkViewport);

        // Tapes
        addAndMakeVisible(tapesView);

        // Input dock
        addAndMakeVisible(inputCapsule);

        // Register listener with MCPBridge if present
        if (pd) {
            if (auto* bridge = pd->getMcp()->getMCPBridge()) {
                // The line slot of the bridge: role, text and the server's mark (what the turn's tool results proved).
                bridge->setChatLineCallback([safeThis = SafePointer<CopilotPanel>(this)](const juce::String& role, const juce::String& text, const juce::String& mark) {
                    if (safeThis != nullptr) {
                        safeThis->receiveMessage(role, text, mark);
                    }
                });
            }
        }

        setTab(PanelTab::Talk);
        startTimerHz(2);
    }

    ~CopilotPanel() override
    {
        stopTimer();
        if (pd) {
            if (auto* bridge = pd->getMcp()->getMCPBridge()) {
                bridge->setChatLineCallback(nullptr);
            }
        }
    }

    // The tab the panel shows. Showing the talk takes the dot away.
    void setTab(PanelTab const newTab)
    {
        currentTab = newTab;
        if (newTab == PanelTab::Talk)
            talkDot = false;
        rackViewport.setVisible(newTab == PanelTab::Widgets);
        talkViewport.setVisible(newTab == PanelTab::Talk);
        tapesView.setVisible(newTab == PanelTab::Tapes);
        updateTabs();
        if (newTab == PanelTab::Talk)
            updateTalkLayout();
    }

    PanelTab getTab() const { return currentTab; }

    // The badge pulse: 10 Hz is smooth enough for a slow sine and costs a badge repaint each tick.
    static constexpr int thinkingTimerHz = 10;
    static constexpr float thinkingPhaseStep = 0.5f; // radians per tick (was 0.2 at 25 Hz: the same speed)

    void setThinking(bool active, const juce::String& status)
    {
        bool const wasThinking = talkView.isThinking;
        talkView.isThinking = active;
        talkView.thinkingStatus = status.isNotEmpty() ? status : "Thinking...";
        if (active) {
            if (!wasThinking || !isTimerRunning())
                startTimerHz(thinkingTimerHz);
        } else {
            startTimerHz(2);
        }
        // Only the status text of a running badge changed ("Thinking... N s" once a second): the badge keeps its
        // size and place, so repaint just it instead of re-laying-out and repainting the whole talk.
        if (active && wasThinking && !talkView.thinkingBadgeBounds.isEmpty()) {
            talkView.repaint(talkView.thinkingBadgeBounds);
            return;
        }
        updateTalkLayout();
    }

    void timerCallback() override
    {
        if (talkView.isThinking) {
            talkView.thinkingPhase += thinkingPhaseStep;
            // Only the badge animates; repainting just it lets paint() cull every line.
            if (talkView.thinkingBadgeBounds.isEmpty())
                talkView.repaint();
            else
                talkView.repaint(talkView.thinkingBadgeBounds);
        }

        if (!pd) return;
        int takes = static_cast<int>(pd->getMcp()->session.getMcpMidiTakes().size() + pd->getMcp()->session.getMcpVoiceTakes().size());
        int images = static_cast<int>(pd->getMcp()->session.getMcpReferenceImages().size());
        tapesView.setCounts(takes, images);
    }

    void triggerPlusMenu()
    {
        PopupMenu menu;
        menu.addItem(1, "Record MIDI Take (Keys)");
        menu.addItem(2, "Record Voice Take (Mic)");
        menu.addItem(3, "Attach Reference Image");

        menu.showMenuAsync(PopupMenu::Options().withTargetComponent(&inputCapsule.plusButton), [this](int result) {
            if (result == 1) {
                if (pd && pd->getMcp()->getMCPBridge()) {
                    pd->getMcp()->getMCPBridge()->startMidiCapture();
                }
            } else if (result == 2) {
                if (pd && pd->getMcp()->getMCPBridge()) {
                    pd->getMcp()->getMCPBridge()->startVoiceCapture(60, 1); // 60 s, melody mode: (1) alone was read as maxSeconds = 1, so the take stopped after one second
                }
            } else if (result == 3) {
                if (editor) {
                    if (auto* cnv = editor->getCurrentCanvas()) {
                        cnv->getMcpLayer().pasteImageFromClipboard();
                    }
                }
            }
        });
    }

    juce::String getStudioDraft() const { return inputCapsule.editor.getText().trim(); }
    void clearStudioDraft() { inputCapsule.editor.setText("", false); }

    void paint(Graphics& g) override
    {
        g.fillAll(copilotlook::cream());
        g.setColour(copilotlook::grey().withAlpha(0.5f));
        g.fillRect(tabRule);
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        if (bounds.getWidth() <= 0) return;

        // 1. Header (height 32): the title and the clear button
        auto header = bounds.removeFromTop(32).reduced(mcp::aiPadding, 2);
        clearButton.setBounds(header.removeFromRight(24));
        header.removeFromRight(6);
        headerTitleLabel.setBounds(header);

        // 2. The tab row (height 30), then a 1 px rule
        auto tabRow = bounds.removeFromTop(30).reduced(mcp::aiPadding - 8, 0);
        for (auto& button : tabButtons)
            button->setBounds(tabRow.removeFromLeft(button->getWantedWidth()));
        tabRule = bounds.removeFromTop(1);

        // 3. Bottom dock: the input, on every tab
        auto const bottomArea = bounds.removeFromBottom(inputBarH + 2 * mcp::aiPadding);
        inputCapsule.setBounds(bottomArea.reduced(mcp::aiPadding));

        // 4. The middle: one of the three tab views fills it (the other two are hidden)
        rackViewport.setBounds(bounds);
        rack.setSize(std::max(10, rackViewport.getMaximumVisibleWidth()), CopilotModuleRack::getContentHeight());
        talkViewport.setBounds(bounds);
        tapesView.setBounds(bounds);
        updateTalkLayout();
        repaint();
    }

    void sendMessage()
    {
        auto text = inputCapsule.editor.getText().trim();
        if (text.isEmpty() || !pd) return;

        inputCapsule.editor.setText("", false);

        juce::StringArray targetIds;
        juce::String context;
        if (editor) {
            if (auto* cnv = editor->getCurrentCanvas()) {
                auto const selection = cnv->getSelectionOfType<Object>();
                for (auto* obj : selection) {
                    if (auto* ptr = obj->getPointer()) {
                        auto const tid = pd->getMcp()->getStableId(ptr);
                        if (tid.isNotEmpty()) targetIds.add(tid);
                    }
                }
                context = contextOfSelection(selection);
            }
        }

        // The artist wrote: show the talk (an own act, not the AI's), then the line with the widget it was about.
        setTab(PanelTab::Talk);
        addArtistLine(text, context);

        if (auto* bridge = pd->getMcp()->getMCPBridge()) {
            bridge->sendSelectionPrompt(text, targetIds, false);
        }
    }

    // The artist's own words from the canvas (the pill): the line, with the widget it was about ("ai.tap", or empty), before the
    // prompt goes out. The later /chat/msg echo of the same prompt is dropped by the talk list's duplicate rule.
    void addArtistLine(const juce::String& text, const juce::String& context)
    {
        jassert(MessageManager::getInstance()->isThisTheMessageThread());
        if (talkView.addLine("user", text, context) != mcp::TalkChange::None) {
            setThinking(true, "Thinking...");
            updateTalkLayout();
        }
    }

    // The label an artist line names its box by: the text of the first selected box, "+n" for the others of a bigger selection
    // ("random 8", "ai.tap", "hsl +2"); empty with nothing selected (McpCopilotTalk.h talkContextLabel). Also used by the pill.
    static juce::String contextOfSelection(SmallArray<Object*> const& selection)
    {
        if (selection.size() == 0)
            return {};
        auto const label = mcp::talkContextLabel(boxTextOf(selection[0]).toStdString(), static_cast<size_t>(selection.size()));
        return juce::String::fromUTF8(label.data(), static_cast<int>(label.size()));
    }

    // One chat line (role: user, ai, detail, error, waiting, thought). Message thread only. A waiting line is replaced in place
    // by the next ai, detail or error line (TalkList). `mark` is the server's word for what the turn's tool results proved for an
    // AI answer ("changed=4 measured=-18.0", "explained"), empty when nothing is proved. A canvas tag is never a line: the server
    // does not send one, a click on a tag adds none, and the list drops any role it does not know.
    void receiveMessage(const juce::String& role, const juce::String& text, const juce::String& mark = {})
    {
        jassert(MessageManager::getInstance()->isThisTheMessageThread());
        if (role == "thought") {
            setThinking(true, text);
        } else if (role == "ai" || role == "detail" || role == "error" || role == "waiting") {
            // "detail" is the long answer: the server sends it with role "detail" and the one short line with role "ai"; /chat/msg
            // reaches this panel (the canvas pill shows only the "ai" line).
            setThinking(false, "");
            auto const change = talkView.addLine(role, text, {}, mark);
            if (change != mcp::TalkChange::None && currentTab != PanelTab::Talk)
                setTalkDot(true);
            updateTalkLayout();
        } else if (role == "user") {
            // A line equal to the last user line is the echo of the artist's own prompt: dropped by the list.
            if (talkView.addLine("user", text) != mcp::TalkChange::None) {
                setThinking(true, "Thinking...");
                updateTalkLayout();
            }
        }
    }

    void updateTalkLayout()
    {
        int const targetH = talkView.calculateTotalHeight(talkViewport.getWidth());
        talkView.setSize(std::max(10, talkViewport.getWidth()), std::max(targetH, talkViewport.getHeight()));
        talkView.repaint();
        talkViewport.setViewPositionProportionately(0.0f, 1.0f);
    }

private:
    struct TabEntry {
        PanelTab tab;
        char const* label;
    };
    static constexpr int inputBarH = 40;

    // The text of a box as typed ("random 8", "hsl 128 15 ..."), or empty: what an artist line was about.
    static juce::String boxTextOf(Object* obj)
    {
        if (!obj || !obj->gui)
            return {};
        return obj->gui->getText().toString();
    }

    void setTalkDot(bool const shouldShow)
    {
        talkDot = shouldShow;
        updateTabs();
    }

    void updateTabs()
    {
        for (auto const& entry : tabTable) {
            auto& button = *tabButtons[static_cast<size_t>(entry.tab)];
            button.setSelected(entry.tab == currentTab);
            button.setDot(entry.tab == PanelTab::Talk && talkDot && currentTab != PanelTab::Talk);
        }
    }

    PluginProcessor* pd;
    PluginEditor* editor;

    Label headerTitleLabel;
    IconButton clearButton = IconButton(Icons::Trash);

    static constexpr TabEntry tabTable[3] = { { PanelTab::Widgets, "widgets" }, { PanelTab::Talk, "talk" }, { PanelTab::Tapes, "tapes" } };
    std::array<std::unique_ptr<TabButton>, 3> tabButtons;
    PanelTab currentTab = PanelTab::Talk;
    bool talkDot = false;
    Rectangle<int> tabRule;

    CopilotModuleRack rack;
    BouncingViewport rackViewport;

    BouncingViewport talkViewport;
    CopilotTalkView talkView;

    TapesView tapesView;
    InputDockCapsule inputCapsule;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CopilotPanel)
};
