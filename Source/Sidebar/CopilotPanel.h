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
#include "CanvasViewport.h"
#include "Object.h"
#include "Pd/MCPBridge.h"

class CopilotPanel final : public Component {
public:
    enum class PanelTab { Notebook, Chat };

    struct ChatMessage {
        juce::String role; // "user", "thought", "ai", "error"
        juce::String text;
        juce::Time time;
    };

    class HeaderTabButton final : public Component {
    public:
        juce::String text;
        bool active = false;
        std::function<void()> onClick;

        void paint(Graphics& g) override
        {
            auto b = getLocalBounds().toFloat().reduced(2.0f, 3.0f);
            auto activeCol = findColour(PlugDataColour::toolbarActiveColourId);
            auto bgCol = findColour(PlugDataColour::sidebarBackgroundColourId);
            auto textCol = findColour(PlugDataColour::sidebarTextColourId);

            if (active) {
                g.setColour(activeCol.withAlpha(0.22f));
                g.fillRoundedRectangle(b, 5.0f);
                g.setColour(activeCol.withAlpha(0.6f));
                g.drawRoundedRectangle(b, 5.0f, 1.2f);
                g.setColour(activeCol.contrasting(0.2f));
            } else {
                if (isMouseOver()) {
                    g.setColour(bgCol.contrasting(0.08f));
                    g.fillRoundedRectangle(b, 5.0f);
                }
                g.setColour(textCol.withAlpha(0.65f));
            }

            g.setFont(Font(11.5f, active ? Font::bold : Font::plain));
            g.drawFittedText(text, getLocalBounds(), Justification::centred, 1);
        }

        void mouseUp(const MouseEvent&) override { if (onClick) onClick(); }
        void mouseEnter(const MouseEvent&) override { repaint(); }
        void mouseExit(const MouseEvent&) override { repaint(); }
    };

    static juce::String resolveRegionTitle(const PluginProcessor::McpAnnotation& a,
                                           const std::vector<PluginProcessor::McpRegion>& regions)
    {
        // 1. Target ID matching
        for (const auto& r : regions) {
            if (a.targetId.isNotEmpty() && r.targetIds.contains(a.targetId)) {
                return r.title.startsWithChar('[') ? r.title : "[" + r.title + "]";
            }
            for (const auto& tid : a.targetIds) {
                if (r.targetIds.contains(tid)) {
                    return r.title.startsWithChar('[') ? r.title : "[" + r.title + "]";
                }
            }
        }
        // 2. Spatial proximity fallback: is (a.x, a.y) within or near region bounds?
        for (const auto& r : regions) {
            if (a.x >= r.x - 25 && a.x <= r.x + r.w + 25 &&
                a.y >= r.y - 45 && a.y <= r.y + r.h + 25) {
                return r.title.startsWithChar('[') ? r.title : "[" + r.title + "]";
            }
        }
        return "[GLOBAL TAPE]";
    }

    class NotebookContent final : public Component {
    public:
        PluginProcessor* pd = nullptr;
        PluginEditor* editor = nullptr;
        CopilotPanel* parentPanel = nullptr;

        class NotebookCardComponent final : public Component {
        public:
            PluginProcessor* pd = nullptr;
            PluginEditor* editor = nullptr;
            CopilotPanel* parentPanel = nullptr;

            PluginProcessor::McpAnnotation annotation;
            juce::String regionTitle;
            size_t annotationIndex = 0;
            bool isHighlighted = false;

            class ActionPill final : public Component {
            public:
                juce::String label;
                std::function<void()> onClick;

                void paint(Graphics& g) override
                {
                    auto b = getLocalBounds().toFloat().reduced(0.5f);
                    auto bgCol = findColour(PlugDataColour::sidebarBackgroundColourId);
                    auto activeCol = findColour(PlugDataColour::toolbarActiveColourId);
                    auto textCol = findColour(PlugDataColour::sidebarTextColourId);

                    if (isMouseOver()) {
                        g.setColour(activeCol.withAlpha(0.25f));
                    } else {
                        g.setColour(bgCol.contrasting(0.08f));
                    }
                    g.fillRoundedRectangle(b, 4.0f);
                    g.setColour(isMouseOver() ? activeCol.withAlpha(0.7f) : bgCol.contrasting(0.15f));
                    g.drawRoundedRectangle(b, 4.0f, 1.0f);

                    g.setFont(Font(10.5f, Font::plain));
                    g.setColour(isMouseOver() ? activeCol.contrasting(0.2f) : textCol.withAlpha(0.85f));
                    g.drawFittedText(label, getLocalBounds(), Justification::centred, 1);
                }
                void mouseUp(const MouseEvent&) override { if (onClick) onClick(); }
                void mouseEnter(const MouseEvent&) override { repaint(); }
                void mouseExit(const MouseEvent&) override { repaint(); }
            };

            class DismissBtn final : public Component {
            public:
                std::function<void()> onClick;
                void paint(Graphics& g) override
                {
                    auto textCol = findColour(PlugDataColour::sidebarTextColourId);
                    g.setColour(isMouseOver() ? Colours::red : textCol.withAlpha(0.45f));
                    g.setFont(Font(11.0f, Font::bold));
                    g.drawFittedText("✕", getLocalBounds(), Justification::centred, 1);
                }
                void mouseUp(const MouseEvent&) override { if (onClick) onClick(); }
                void mouseEnter(const MouseEvent&) override { repaint(); }
                void mouseExit(const MouseEvent&) override { repaint(); }
            };

            ActionPill focusBtn;
            ActionPill askAiBtn;
            DismissBtn dismissBtn;

            NotebookCardComponent(PluginProcessor* proc, PluginEditor* ed, CopilotPanel* parent,
                                  const PluginProcessor::McpAnnotation& ann, const juce::String& title, size_t idx)
                : pd(proc), editor(ed), parentPanel(parent), annotation(ann), regionTitle(title), annotationIndex(idx)
            {
                focusBtn.label = "🔍 Canvas";
                focusBtn.onClick = [this] { focusOnCanvas(); };
                addAndMakeVisible(focusBtn);

                askAiBtn.label = "💬 Ask AI";
                askAiBtn.onClick = [this] { askAi(); };
                addAndMakeVisible(askAiBtn);

                dismissBtn.onClick = [this] { dismiss(); };
                addAndMakeVisible(dismissBtn);
            }

            void focusOnCanvas()
            {
                if (!editor) return;
                if (auto* cnv = editor->getCurrentCanvas()) {
                    cnv->deselectAll();
                    juce::Rectangle<int> unionBounds;
                    bool first = true;
                    auto const ids = annotation.targetIds.size() > 0 ? annotation.targetIds : juce::StringArray(annotation.targetId);
                    for (auto const& tid : ids) {
                        if (auto* obj = cnv->findObjectByStableId(tid)) {
                            cnv->setSelected(obj, true);
                            auto const b = obj->getBounds();
                            if (first) { unionBounds = b; first = false; }
                            else { unionBounds = unionBounds.getUnion(b); }
                        }
                    }
                    if (!first && cnv->viewport) {
                        auto const scale = ::getValue<float>(cnv->zoomScale);
                        int const targetX = static_cast<int>(unionBounds.getCentreX() * scale - cnv->viewport->getWidth() * 0.5f);
                        int const targetY = static_cast<int>(unionBounds.getCentreY() * scale - cnv->viewport->getHeight() * 0.5f);
                        if (auto* cvp = dynamic_cast<CanvasViewport*>(cnv->viewport.get())) {
                            cvp->setViewPositionAnimated(juce::Point<int>(std::max(0, targetX), std::max(0, targetY)));
                        } else {
                            cnv->viewport->setViewPosition(std::max(0, targetX), std::max(0, targetY));
                        }
                    }
                    cnv->repaint();
                }
            }

            void askAi();
            void dismiss();

            void mouseUp(const MouseEvent& e) override
            {
                if (!focusBtn.getBounds().contains(e.getPosition()) &&
                    !askAiBtn.getBounds().contains(e.getPosition()) &&
                    !dismissBtn.getBounds().contains(e.getPosition())) {
                    focusOnCanvas();
                }
            }

            void paint(Graphics& g) override
            {
                auto const bgCol = findColour(PlugDataColour::sidebarBackgroundColourId);
                auto const activeCol = findColour(PlugDataColour::toolbarActiveColourId);
                auto const textCol = findColour(PlugDataColour::sidebarTextColourId);

                auto cardBounds = getLocalBounds().toFloat().reduced(0.5f);

                // Card background
                juce::Colour cardBg = bgCol.contrasting(0.06f);
                if (isHighlighted) cardBg = activeCol.withAlpha(0.12f);
                g.setColour(cardBg);
                g.fillRoundedRectangle(cardBounds, 6.0f);

                // Border
                g.setColour(isHighlighted ? activeCol.withAlpha(0.8f) : bgCol.contrasting(0.14f));
                g.drawRoundedRectangle(cardBounds, 6.0f, isHighlighted ? 1.5f : 1.0f);

                // Jewel LED color
                juce::Colour ledCol = juce::Colour(0xff4a9eff); // cyan
                if (annotation.kind == "warn") ledCol = juce::Colour(0xffffbe50); // amber
                else if (annotation.kind == "change") ledCol = juce::Colour(0xff3ddd8a); // green
                else if (annotation.kind == "artist") ledCol = juce::Colour(0xffb877db); // violet

                // Draw Jewel LED
                g.setColour(ledCol.withAlpha(0.3f));
                g.fillEllipse(9.0f, 9.0f, 9.0f, 9.0f);
                g.setColour(ledCol);
                g.fillEllipse(10.5f, 10.5f, 6.0f, 6.0f);

                // Region Title
                g.setFont(Font(11.5f, Font::bold));
                g.setColour(activeCol.contrasting(0.15f));
                g.drawText(regionTitle, 23, 6, getWidth() - 50, 16, Justification::centredLeft, true);

                // Note Text
                int const textW = std::max(20, getWidth() - 24);
                juce::AttributedString as;
                as.setWordWrap(juce::AttributedString::WordWrap::byWord);
                as.append(annotation.text, Font(12.0f, Font::plain), textCol.withAlpha(0.92f));
                juce::TextLayout tl;
                tl.createLayout(as, static_cast<float>(textW));
                tl.draw(g, juce::Rectangle<float>(12.0f, 26.0f, static_cast<float>(textW), tl.getHeight()));
            }

            void resized() override
            {
                dismissBtn.setBounds(getWidth() - 24, 4, 18, 18);
                int const btnY = getHeight() - 26;
                focusBtn.setBounds(12, btnY, 68, 18);
                askAiBtn.setBounds(84, btnY, 68, 18);
            }

            int calculateHeight(int cardW) const
            {
                int const textW = std::max(20, cardW - 24);
                juce::AttributedString as;
                as.setWordWrap(juce::AttributedString::WordWrap::byWord);
                as.append(annotation.text, Font(12.0f, Font::plain), Colours::white);
                juce::TextLayout tl;
                tl.createLayout(as, static_cast<float>(textW));
                // 28px header + text height + 6px gap + 22px action buttons + 8px bottom pad
                return 28 + static_cast<int>(tl.getHeight()) + 6 + 22 + 8;
            }
        };

        std::vector<std::unique_ptr<NotebookCardComponent>> cards;

        void refresh()
        {
            cards.clear();
            removeAllChildren();

            if (!pd) return;

            auto const annotations = pd->getMcpAnnotations();
            auto const regions = pd->getMcpRegions();

            for (size_t i = 0; i < annotations.size(); ++i) {
                auto const& a = annotations[i];
                auto const title = resolveRegionTitle(a, regions);
                auto card = std::make_unique<NotebookCardComponent>(pd, editor, parentPanel, a, title, i);
                addAndMakeVisible(card.get());
                cards.push_back(std::move(card));
            }

            updateLayout();
        }

        void highlightCard(const juce::String& text, const juce::StringArray& targetIds)
        {
            for (auto& card : cards) {
                bool match = (card->annotation.text == text);
                if (!match && targetIds.size() > 0) {
                    for (auto const& tid : targetIds) {
                        if (card->annotation.targetIds.contains(tid) || card->annotation.targetId == tid) {
                            match = true;
                            break;
                        }
                    }
                }
                card->isHighlighted = match;
                card->repaint();
            }
        }

        void updateLayout()
        {
            int const w = getWidth();
            if (w <= 20) return;

            int y = 8;
            int const cardW = std::max(40, w - 16);

            for (auto& card : cards) {
                int const h = card->calculateHeight(cardW);
                card->setBounds(8, y, cardW, h);
                y += h + 8;
            }

            setSize(w, std::max(y + 8, getParentComponent() ? getParentComponent()->getHeight() : 100));
            repaint();
        }

        void paint(Graphics& g) override
        {
            if (cards.empty()) {
                auto const textCol = findColour(PlugDataColour::sidebarTextColourId);
                g.setColour(textCol.withAlpha(0.45f));
                g.setFont(Font(12.5f, Font::plain));
                g.drawFittedText("Keine aktiven Console-Tapes im Patch.\n\nScribble-Tapes auf dem Canvas oder via Copilot erscheinen hier als Master-Track-Overview.",
                                 getLocalBounds().reduced(20), Justification::centred, 4);
            }
        }
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
        // Header Tab Buttons
        notebookTabBtn.text = "📋 Notebook (0)";
        notebookTabBtn.active = true;
        notebookTabBtn.onClick = [this] { setTab(PanelTab::Notebook); };
        addAndMakeVisible(notebookTabBtn);

        chatTabBtn.text = "💬 Copilot";
        chatTabBtn.active = false;
        chatTabBtn.onClick = [this] { setTab(PanelTab::Chat); };
        addAndMakeVisible(chatTabBtn);

        // Clear Button
        clearButton.setTooltip("Clear notebook notes or chat history");
        clearButton.onClick = [this] {
            if (currentTab == PanelTab::Notebook) {
                if (pd) {
                    {
                        juce::ScopedLock sl(pd->mcpOverlayLock);
                        pd->mcpAnnotations.clear();
                    }
                    if (editor && editor->getCurrentCanvas()) {
                        editor->getCurrentCanvas()->repaint();
                    }
                }
                refreshNotebook();
            } else {
                chatContent.items.clear();
                updateChatLayout();
                if (pd) {
                    if (auto* bridge = pd->getMCPBridge()) {
                        bridge->sendReply("/chat/clear", {});
                    }
                }
            }
        };
        addAndMakeVisible(clearButton);

        // Viewport
        addAndMakeVisible(viewport);
        notebookContent.pd = pd;
        notebookContent.editor = editor;
        notebookContent.parentPanel = this;
        viewport.setViewedComponent(&notebookContent, false);

        // Input Editor
        inputEditor.setTextToShowWhenEmpty("Add tape note to canvas selection...", findColour(PlugDataColour::sidebarTextColourId).withAlpha(0.5f));
        inputEditor.onReturnKey = [this] {
            sendMessage();
        };
        addAndMakeVisible(inputEditor);

        // Send Button
        sendButton.setTooltip("Submit note or send chat prompt");
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

        refreshNotebook();
    }

    ~CopilotPanel() override
    {
        if (pd) {
            if (auto* bridge = pd->getMCPBridge()) {
                bridge->setChatCallback(nullptr);
            }
        }
    }

    void setTab(PanelTab newTab)
    {
        currentTab = newTab;
        notebookTabBtn.active = (currentTab == PanelTab::Notebook);
        chatTabBtn.active = (currentTab == PanelTab::Chat);
        notebookTabBtn.repaint();
        chatTabBtn.repaint();

        if (currentTab == PanelTab::Notebook) {
            viewport.setViewedComponent(&notebookContent, false);
            inputEditor.setTextToShowWhenEmpty("Add tape note to canvas selection...", findColour(PlugDataColour::sidebarTextColourId).withAlpha(0.5f));
            refreshNotebook();
        } else {
            viewport.setViewedComponent(&chatContent, false);
            inputEditor.setTextToShowWhenEmpty("Ask Copilot or type prompt...", findColour(PlugDataColour::sidebarTextColourId).withAlpha(0.5f));
            updateChatLayout();
        }
    }

    void refreshNotebook()
    {
        notebookContent.pd = pd;
        notebookContent.editor = editor;
        notebookContent.parentPanel = this;
        notebookContent.refresh();

        int const count = pd ? static_cast<int>(pd->getMcpAnnotations().size()) : 0;
        notebookTabBtn.text = "📋 Notebook (" + juce::String(count) + ")";
        notebookTabBtn.repaint();

        int const targetH = notebookContent.getHeight();
        notebookContent.setSize(std::max(10, viewport.getWidth()), std::max(targetH, viewport.getHeight()));
    }

    void deleteAnnotation(size_t index)
    {
        if (!pd) return;
        {
            juce::ScopedLock sl(pd->mcpOverlayLock);
            if (index < pd->mcpAnnotations.size()) {
                pd->mcpAnnotations.erase(pd->mcpAnnotations.begin() + index);
            }
        }
        if (editor && editor->getCurrentCanvas()) {
            editor->getCurrentCanvas()->repaint();
        }
        refreshNotebook();
    }

    void askAiAboutRegion(const juce::String& region, const juce::String& noteText)
    {
        setTab(PanelTab::Chat);
        inputEditor.setText("About " + region + ": ");
        inputEditor.grabKeyboardFocus();
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        if (bounds.getWidth() <= 0) return;

        // Top Header bar
        auto header = bounds.removeFromTop(32).reduced(4, 2);
        clearButton.setBounds(header.removeFromRight(26));
        header.removeFromRight(4);
        int const tabW = header.getWidth() / 2;
        notebookTabBtn.setBounds(header.removeFromLeft(tabW));
        chatTabBtn.setBounds(header);

        // Bottom Input bar (height 36)
        auto inputBar = bounds.removeFromBottom(40).reduced(6, 4);
        sendButton.setBounds(inputBar.removeFromRight(30));
        inputBar.removeFromRight(4);
        inputEditor.setBounds(inputBar);

        // Middle Viewport
        viewport.setBounds(bounds);
        if (currentTab == PanelTab::Notebook) {
            notebookContent.setSize(viewport.getWidth(), notebookContent.getHeight());
            notebookContent.updateLayout();
        } else {
            updateChatLayout();
        }
    }

    void sendMessage()
    {
        auto text = inputEditor.getText().trim();
        if (text.isEmpty() || !pd) return;

        inputEditor.setText("", false);

        if (currentTab == PanelTab::Notebook) {
            // Collect canvas selection if any
            juce::StringArray targetIds;
            float minX = 1e9f, minY = 1e9f;
            if (editor) {
                if (auto* cnv = editor->getCurrentCanvas()) {
                    for (auto* obj : cnv->getSelectionOfType<Object>()) {
                        if (auto* ptr = obj->getPointer()) {
                            auto const tid = pd->getStableId(ptr);
                            if (tid.isNotEmpty()) targetIds.add(tid);
                        }
                        minX = std::min(minX, static_cast<float>(obj->getX()));
                        minY = std::min(minY, static_cast<float>(obj->getY()));
                    }
                }
            }

            // Add new annotation
            {
                juce::ScopedLock sl(pd->mcpOverlayLock);
                PluginProcessor::McpAnnotation ann;
                ann.text = text;
                ann.kind = "artist";
                ann.targetIds = targetIds;
                if (targetIds.size() > 0) ann.targetId = targetIds[0];
                ann.x = (minX < 1e8f) ? minX : 100.0f;
                ann.y = (minY < 1e8f) ? std::max(20.0f, minY - 24.0f) : 100.0f;
                ann.t = juce::Time::getMillisecondCounterHiRes() * 0.001;
                pd->mcpAnnotations.push_back(ann);
            }

            if (editor && editor->getCurrentCanvas()) {
                editor->getCurrentCanvas()->repaint();
            }
            refreshNotebook();

            if (auto* bridge = pd->getMCPBridge()) {
                DynamicObject::Ptr d = new DynamicObject();
                d->setProperty("text", text);
                d->setProperty("kind", "artist");
                Array<var> ids;
                for (auto const& tid : targetIds) ids.add(tid);
                d->setProperty("targetIds", var(ids));
                bridge->sendReply("/canvas/note", juce::JSON::toString(var(d.get())));
            }
            return;
        }

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
        juce::String header = "📌 Module Note";
        if (kind == "change") header = "✨ Change Record";
        else if (kind == "warn") header = "⚠️ Warning";
        else if (kind == "artist") header = "✍️ Artist Note";

        juce::String msg = header + ":\n" + text;
        if (chatContent.items.empty() || chatContent.items.back().text != msg) {
            chatContent.items.push_back({
                "thought",
                msg,
                juce::Time::getCurrentTime()
            });
        }

        setTab(PanelTab::Notebook);
        refreshNotebook();
        notebookContent.highlightCard(text, targetIds);
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

    PanelTab currentTab = PanelTab::Notebook;

    HeaderTabButton notebookTabBtn;
    HeaderTabButton chatTabBtn;
    SmallIconButton clearButton = SmallIconButton(Icons::Trash);

    BouncingViewport viewport;
    NotebookContent notebookContent;
    ChatListContent chatContent;

    SearchEditor inputEditor;
    SmallIconButton sendButton = SmallIconButton(Icons::Forward);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CopilotPanel)
};

inline void CopilotPanel::NotebookContent::NotebookCardComponent::askAi()
{
    if (parentPanel) {
        parentPanel->askAiAboutRegion(regionTitle, annotation.text);
    }
}

inline void CopilotPanel::NotebookContent::NotebookCardComponent::dismiss()
{
    if (parentPanel) {
        parentPanel->deleteAnnotation(annotationIndex);
    }
}
