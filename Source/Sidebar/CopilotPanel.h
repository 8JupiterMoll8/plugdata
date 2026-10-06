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
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "Canvas.h"
#include "CanvasViewport.h"
#include "Object.h"
#include "Pd/MCPBridge.h"

class CopilotPanel final : public Component, private juce::Timer {
public:
    enum class PanelTab { Notebook, Studio, Chat };

    struct ChatMessage {
        juce::String role; // "user", "thought", "ai", "error"
        juce::String text;
        juce::Time time;
    };

    class HeaderTabButton final : public Component {
    public:
        juce::String text;
        juce::String icon; // PlugData icon-font glyph (emoji never render in the bundled fonts)
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
            } else {
                if (isMouseOver()) {
                    g.setColour(bgCol.contrasting(0.08f));
                    g.fillRoundedRectangle(b, 5.0f);
                }
            }

            auto const textColour = active ? activeCol.contrasting(0.2f) : textCol.withAlpha(0.65f);
            g.setColour(textColour);

            // Icon-font glyph + label centred as one unit (native PlugData pipeline).
            auto const font = Font(11.5f, active ? Font::bold : Font::plain);
            g.setFont(font);
            int const textW = font.getStringWidth(text);
            constexpr int iconW = 15;
            int const groupW = (icon.isNotEmpty() ? iconW + 4 : 0) + textW;
            int const startX = getLocalBounds().getCentreX() - groupW / 2;
            if (icon.isNotEmpty()) {
                Fonts::drawIcon(g, icon, { startX, 0, iconW, getHeight() }, textColour, 12, false);
                g.setFont(font);
                g.setColour(textColour);
                g.drawFittedText(text, { startX + iconW + 4, 0, textW + 10, getHeight() }, Justification::centredLeft, 1);
            } else {
                g.drawFittedText(text, getLocalBounds(), Justification::centred, 1);
            }
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
                    g.drawFittedText("x", getLocalBounds(), Justification::centred, 1);
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
                focusBtn.label = "Canvas";
                focusBtn.onClick = [this] { focusOnCanvas(); };
                addAndMakeVisible(focusBtn);

                askAiBtn.label = "Ask AI";
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

    // PRD Staging Bay — the brainstorm lane. Parked ideas (ink, reference images,
    // audio soon) live here; the pen arms from here; commit sends the visible
    // bundle to the AI. No selection pill involved — nothing to select in brainstorm.
    class StudioContent final : public Component {
    public:
        PluginProcessor* pd = nullptr;
        PluginEditor* editor = nullptr;
        CopilotPanel* parentPanel = nullptr;

        class MiniBtn final : public Component {
        public:
            juce::String label;
            juce::String icon; // PlugData icon-font glyph (preferred over emoji)
            juce::Colour iconColour = juce::Colours::transparentBlack; // optional override
            std::function<void()> onClick;
            bool activeState = false;

            void paint(Graphics& g) override
            {
                auto b = getLocalBounds().toFloat().reduced(0.5f);
                auto bgCol = findColour(PlugDataColour::sidebarBackgroundColourId);
                auto activeCol = findColour(PlugDataColour::toolbarActiveColourId);
                auto textCol = findColour(PlugDataColour::sidebarTextColourId);

                if (activeState) g.setColour(activeCol.withAlpha(0.3f));
                else if (isMouseOver()) g.setColour(activeCol.withAlpha(0.18f));
                else g.setColour(bgCol.contrasting(0.08f));
                g.fillRoundedRectangle(b, 4.0f);
                g.setColour(activeState ? activeCol.withAlpha(0.8f) : bgCol.contrasting(0.15f));
                g.drawRoundedRectangle(b, 4.0f, 1.0f);

                auto const col = iconColour.getAlpha() > 0
                    ? iconColour
                    : (activeState ? activeCol.contrasting(0.2f) : textCol.withAlpha(0.85f));
                if (icon.isNotEmpty()) {
                    Fonts::drawIcon(g, icon, getLocalBounds().reduced(2), col, 12);
                } else {
                    g.setFont(Font(10.5f, activeState ? Font::bold : Font::plain));
                    g.setColour(col);
                    g.drawFittedText(label, getLocalBounds(), Justification::centred, 1);
                }
            }
            void mouseUp(const MouseEvent&) override { if (onClick) onClick(); }
            void mouseEnter(const MouseEvent&) override { repaint(); }
            void mouseExit(const MouseEvent&) override { repaint(); }
        };

        class ItemRow final : public Component {
        public:
            juce::String title, sub;
            juce::String typeIcon; // icon-font glyph fallback
            int typeGlyph = -1;    // vector glyph: 0=pen 1=T 2=picture 3=mic 4=drum (same as the mood strip)
            juce::Colour typeColour = juce::Colours::white;
            std::function<void()> onToggleEye, onToggleLock, onDelete;
            bool eyeOn = true, locked = false, hasLock = false;

            MiniBtn eyeBtn, lockBtn, delBtn;

            // The mood strip's vector language, redrawn in JUCE so the bay reads
            // as the same instrument (not a different app).
            static void drawTypeGlyph(Graphics& g, int kind, Rectangle<int> b, Colour col)
            {
                g.setColour(col);
                float const cx = static_cast<float>(b.getCentreX());
                float const cy = static_cast<float>(b.getCentreY());
                float const s = 0.78f; // fit a 30px-strip glyph into the 16px bay slot

                switch (kind) {
                case 0: { // pen
                    Path p;
                    p.startNewSubPath(cx - 5.0f * s, cy + 5.0f * s);
                    p.lineTo(cx + 3.0f * s, cy - 5.0f * s);
                    p.lineTo(cx + 5.5f * s, cy - 2.5f * s);
                    p.lineTo(cx - 2.5f * s, cy + 5.5f * s);
                    p.closeSubPath();
                    g.strokePath(p, PathStrokeType(1.25f));
                    Path q;
                    q.startNewSubPath(cx - 5.0f * s, cy + 5.0f * s);
                    q.lineTo(cx - 6.5f * s, cy + 6.5f * s);
                    g.strokePath(q, PathStrokeType(1.25f));
                    break;
                }
                case 1: { // T (tape note)
                    g.setFont(Font(12.0f, Font::bold));
                    g.drawText("T", b, Justification::centred, false);
                    break;
                }
                case 2: { // picture frame
                    g.drawRoundedRectangle(Rectangle<float>(cx - 6.0f * s, cy - 5.0f * s, 12.0f * s, 10.0f * s), 1.6f, 1.2f);
                    Path m;
                    m.startNewSubPath(cx - 5.0f * s, cy + 3.0f * s);
                    m.lineTo(cx - 1.0f * s, cy - 1.0f * s);
                    m.lineTo(cx + 2.0f * s, cy + 1.5f * s);
                    m.lineTo(cx + 5.0f * s, cy - 2.0f * s);
                    g.strokePath(m, PathStrokeType(1.1f));
                    break;
                }
                case 3: { // melody (beamed notes — same as the strip)
                    float const hw = 5.2f * s;
                    float const hh = 3.8f * s;
                    g.fillEllipse(cx - 3.5f * s - hw * 0.5f, cy + 4.5f * s - hh * 0.5f, hw, hh);
                    g.fillEllipse(cx + 4.0f * s - hw * 0.5f, cy + 3.0f * s - hh * 0.5f, hw, hh);
                    Path st;
                    st.startNewSubPath(cx - 0.9f * s, cy + 4.4f * s);
                    st.lineTo(cx - 0.9f * s, cy - 5.5f * s);
                    st.lineTo(cx + 6.6f * s, cy - 6.8f * s);
                    st.lineTo(cx + 6.6f * s, cy + 2.9f * s);
                    g.strokePath(st, PathStrokeType(1.4f));
                    break;
                }
                case 4: { // drum (beat)
                    g.drawRoundedRectangle(Rectangle<float>(cx - 6.0f * s, cy - 0.5f * s, 12.0f * s, 6.5f * s), 2.0f, 1.2f);
                    g.drawEllipse(cx - 6.0f * s, cy - 2.5f * s, 12.0f * s, 4.0f * s, 1.2f);
                    Path bl;
                    bl.startNewSubPath(cx - 4.5f * s, cy - 7.0f * s);
                    bl.lineTo(cx + 1.0f * s, cy - 1.5f * s);
                    bl.startNewSubPath(cx + 4.5f * s, cy - 7.0f * s);
                    bl.lineTo(cx - 1.0f * s, cy - 1.5f * s);
                    g.strokePath(bl, PathStrokeType(1.1f));
                    break;
                }
                default:
                    break;
                }
            }

            ItemRow()
            {
                eyeBtn.icon = Icons::Eye;
                eyeBtn.onClick = [this] { if (onToggleEye) onToggleEye(); };
                addAndMakeVisible(eyeBtn);

                lockBtn.icon = Icons::Lock;
                lockBtn.onClick = [this] { if (onToggleLock) onToggleLock(); };
                addAndMakeVisible(lockBtn);

                delBtn.icon = Icons::Trash;
                delBtn.onClick = [this] { if (onDelete) onDelete(); };
                addAndMakeVisible(delBtn);
            }

            void paint(Graphics& g) override
            {
                auto const bgCol = findColour(PlugDataColour::sidebarBackgroundColourId);
                auto const textCol = findColour(PlugDataColour::sidebarTextColourId);

                auto b = getLocalBounds().toFloat().reduced(0.5f);
                g.setColour(bgCol.contrasting(0.05f));
                g.fillRoundedRectangle(b, 5.0f);
                g.setColour(bgCol.contrasting(0.13f));
                g.drawRoundedRectangle(b, 5.0f, 1.0f);

                // LED status dot: green = live in the brief, dim = hidden from AI
                float const ledY = (getHeight() - 6.0f) * 0.5f;
                g.setColour(eyeOn ? juce::Colour(0xff3ddd8a) : textCol.withAlpha(0.22f));
                g.fillEllipse(10.0f, ledY, 6.0f, 6.0f);

                // Artifact type glyph — SAME vectors as the mood strip (consistency)
                auto const glyphCol = typeColour.withAlpha(eyeOn ? 0.95f : 0.35f);
                if (typeGlyph >= 0) {
                    drawTypeGlyph(g, typeGlyph, { 22, 0, 16, getHeight() }, glyphCol);
                } else if (typeIcon.isNotEmpty()) {
                    Fonts::drawIcon(g, typeIcon, { 22, 0, 16, getHeight() }, glyphCol, 13);
                }

                g.setFont(Font(11.5f, Font::bold));
                g.setColour(textCol.withAlpha(eyeOn ? 0.92f : 0.4f));
                g.drawText(title, 44, 3, getWidth() - 150, 15, Justification::centredLeft, true);

                g.setFont(Font(10.0f, Font::plain));
                g.setColour(textCol.withAlpha(0.5f));
                g.drawText(sub, 44, 18, getWidth() - 150, 13, Justification::centredLeft, true);
            }

            void resized() override
            {
                auto b = getLocalBounds().reduced(5, 6);
                delBtn.setBounds(b.removeFromRight(28));
                b.removeFromRight(3);
                if (hasLock) {
                    lockBtn.setBounds(b.removeFromRight(28));
                    b.removeFromRight(3);
                    lockBtn.setVisible(true);
                } else {
                    lockBtn.setVisible(false);
                }
                eyeBtn.setBounds(b.removeFromRight(28));
            }
        };

        class BrainstormBar final : public Component {
        public:
            std::function<void()> onClick;
            bool on = false;
            juce::String onText = "Brainstorm ON - pen is live";
            juce::String offText = "Brainstorm OFF - click to arm pen";

            void paint(Graphics& g) override
            {
                auto const activeCol = findColour(PlugDataColour::toolbarActiveColourId);
                auto const bgCol = findColour(PlugDataColour::sidebarBackgroundColourId);
                auto const textCol = findColour(PlugDataColour::sidebarTextColourId);

                auto b = getLocalBounds().toFloat().reduced(0.5f);
                g.setColour(on ? activeCol.withAlpha(0.22f) : bgCol.contrasting(0.05f));
                g.fillRoundedRectangle(b, 5.0f);
                g.setColour(on ? activeCol.withAlpha(0.75f) : bgCol.contrasting(0.13f));
                g.drawRoundedRectangle(b, 5.0f, on ? 1.4f : 1.0f);

                g.setFont(Font(11.5f, Font::bold));
                g.setColour(on ? activeCol.contrasting(0.2f) : textCol.withAlpha(0.8f));
                g.drawFittedText(on ? onText : offText, getLocalBounds(), Justification::centred, 1);
            }
            void mouseUp(const MouseEvent&) override { if (onClick) onClick(); }
            void mouseEnter(const MouseEvent&) override { repaint(); }
            void mouseExit(const MouseEvent&) override { repaint(); }
        };

        BrainstormBar brainstormBar;
        BrainstormBar eraserBar;
        // Surgical lane: artifacts bound to the selected object(s).
        std::vector<std::unique_ptr<ItemRow>> attachedRows;
        juce::String attachedLabel;
        int attachedHeaderY = -1;
        int stagingHeaderY = 74;
        // Moodboard lane: loose parked artifacts.
        std::vector<std::unique_ptr<ItemRow>> rows;

        StudioContent()
        {
            brainstormBar.onClick = [this] {
                if (!editor) return;
                if (auto* cnv = editor->getCurrentCanvas()) {
                    cnv->setStudioMode(!cnv->isStudioMode());
                    refresh();
                }
            };
            addAndMakeVisible(brainstormBar);

            eraserBar.onText = "Eraser ON - drag over ink to wipe";
            eraserBar.offText = "Eraser OFF - click to arm eraser";
            eraserBar.onClick = [this] {
                if (!editor) return;
                if (auto* cnv = editor->getCurrentCanvas()) {
                    cnv->setEraserToolActive(!cnv->isEraserToolActive());
                    refresh();
                }
            };
            addAndMakeVisible(eraserBar);
        }

        static void repaintCanvas(PluginEditor* ed)
        {
            if (ed) {
                if (auto* cnv = ed->getCurrentCanvas()) {
                    cnv->repaint();
                    if (cnv->editor) cnv->editor->nvgSurface.renderAll();
                }
            }
        }

        void refresh()
        {
            rows.clear();
            attachedRows.clear();
            attachedLabel.clear();
            removeAllChildren();
            addAndMakeVisible(brainstormBar);
            addAndMakeVisible(eraserBar);

            auto* cnv = editor ? editor->getCurrentCanvas() : nullptr;
            brainstormBar.on = cnv && cnv->isStudioMode();
            eraserBar.on = cnv && cnv->isEraserToolActive();

            // Surgical lane: stable ids of the current selection = attachment anchors.
            juce::StringArray selIds;
            if (cnv && pd) {
                for (auto* obj : cnv->getSelectionOfType<Object>()) {
                    if (auto* ptr = obj->getPointer()) {
                        auto tid = pd->getStableId(ptr);
                        if (tid.isEmpty()) {
                            if (auto patchPtr = cnv->patch.getPointer()) {
                                tid = pd->getOrAdoptStableId(patchPtr.get(), ptr);
                            }
                        }
                        if (tid.isNotEmpty()) {
                            selIds.add(tid);
                            if (attachedLabel.isEmpty()) attachedLabel = obj->getName();
                        }
                    }
                }
            }

            // Bound tape notes (annotations already carry taskId/targetIds).
            if (pd && !selIds.isEmpty()) {
                for (auto const& a : pd->getMcpAnnotations()) {
                    if (a.text.isEmpty() || !a.targetId.isNotEmpty()) continue;
                    if (!selIds.contains(a.targetId)) continue;
                    auto row = std::make_unique<ItemRow>();
                    row->title = "Note - " + a.text.substring(0, 20) + (a.text.length() > 20 ? juce::String(juce::CharPointer_UTF8("\xe2\x80\xa6")) : juce::String());
                    row->sub = "attached to " + (attachedLabel.isNotEmpty() ? attachedLabel : juce::String("selection"));
                    row->typeGlyph = 1; // T (matches strip)
                    row->typeColour = juce::Colour(0xffffbe50);
                    row->delBtn.iconColour = juce::Colour(0xffe05a5a);
                    auto const noteText = a.text;
                    auto const boundId = a.targetId;
                    row->onDelete = [this, noteText, boundId] {
                        if (!pd) return;
                        {
                            juce::ScopedLock sl(pd->mcpOverlayLock);
                            for (auto it = pd->mcpAnnotations.begin(); it != pd->mcpAnnotations.end(); ++it) {
                                if (it->text == noteText && it->targetId == boundId) {
                                    pd->mcpAnnotations.erase(it);
                                    break;
                                }
                            }
                        }
                        repaintCanvas(editor);
                        refresh();
                    };
                    addAndMakeVisible(row.get());
                    attachedRows.push_back(std::move(row));
                }
            }

            if (cnv && !cnv->mcpCompletedStrokes.empty()) {
                auto row = std::make_unique<ItemRow>();
                row->title = "Sketch - " + juce::String(static_cast<int>(cnv->mcpCompletedStrokes.size())) + " stroke(s)";
                row->sub = cnv->isStudioInkVisible() ? "in the brief (visible)" : "hidden - excluded from AI";
                row->eyeOn = cnv->isStudioInkVisible();
                row->typeGlyph = 0; // pen (matches strip)
                row->typeColour = juce::Colour(0xffffbe50);
                row->eyeBtn.iconColour = cnv->isStudioInkVisible() ? juce::Colours::transparentBlack : juce::Colour(0xffffbe50);
                row->delBtn.iconColour = juce::Colour(0xffe05a5a);
                row->onToggleEye = [this, cnv] {
                    cnv->setStudioInkVisible(!cnv->isStudioInkVisible());
                    refresh();
                };
                row->onDelete = [this, cnv] {
                    cnv->clearAllInk();
                    refresh();
                };
                addAndMakeVisible(row.get());
                rows.push_back(std::move(row));
            }

            if (pd) {
                auto const images = pd->getMcpReferenceImages();
                for (auto const& img : images) {
                    auto row = std::make_unique<ItemRow>();
                    auto const id = img.id;
                    row->title = "Image - " + (img.title.isNotEmpty() ? img.title : juce::File(img.filePath).getFileName());
                    bool const bound = img.targetId.isNotEmpty() && selIds.contains(img.targetId);
                    row->sub = juce::String(juce::roundToInt(img.width)) + "x" + juce::String(juce::roundToInt(img.height))
                             + (bound ? " | attached" : "") + (img.isLocked ? " | locked" : "") + (img.visible ? "" : " | hidden");
                    row->eyeOn = img.visible;
                    row->locked = img.isLocked;
                    row->hasLock = true;
                    row->typeGlyph = 2; // picture (matches strip)
                    row->typeColour = juce::Colour(0xff3ddd8a);
                    row->eyeBtn.iconColour = img.visible ? juce::Colours::transparentBlack : juce::Colour(0xffffbe50);
                    row->lockBtn.iconColour = img.isLocked ? juce::Colour(0xff3ddd8a) : juce::Colours::transparentBlack;
                    row->delBtn.iconColour = juce::Colour(0xffe05a5a);
                    row->onToggleEye = [this, id] {
                        for (auto const& im : pd->getMcpReferenceImages()) {
                            if (im.id == id) { pd->setMcpReferenceVisible(id, !im.visible); break; }
                        }
                        repaintCanvas(editor);
                        refresh();
                    };
                    row->onToggleLock = [this, id] {
                        for (auto const& im : pd->getMcpReferenceImages()) {
                            if (im.id == id) { pd->setMcpReferenceLock(id, !im.isLocked); break; }
                        }
                        refresh();
                    };
                    row->onDelete = [this, id] {
                        pd->removeMcpReferenceImage(id);
                        repaintCanvas(editor);
                        refresh();
                    };
                    addAndMakeVisible(row.get());
                    (bound ? attachedRows : rows).push_back(std::move(row));
                }
            }

            // Voice takes: beatbox / melody recordings (WAV + structured transcript).
            if (pd) {
                auto const takes = pd->getMcpVoiceTakes();
                for (auto const& take : takes) {
                    auto row = std::make_unique<ItemRow>();
                    auto const id = take.id;
                    row->title = "Take - " + take.mode + " | " + juce::String(take.durationSec, 1) + "s";
                    row->sub = juce::String(take.bpm) + " BPM | " + juce::String(take.hitCount) + " hits | "
                             + juce::String(take.noteCount) + " notes" + (take.wavPath.isEmpty() ? " | no file" : " | wav");
                    row->eyeOn = take.visible;
                    row->typeGlyph = (take.mode == "melody") ? 3 : 4; // mic/drum (matches strip)
                    row->typeColour = juce::Colour(0xff00e676);
                    row->eyeBtn.iconColour = take.visible ? juce::Colours::transparentBlack : juce::Colour(0xffffbe50);
                    row->delBtn.iconColour = juce::Colour(0xffe05a5a);
                    row->onToggleEye = [this, id] {
                        for (auto const& t : pd->getMcpVoiceTakes()) {
                            if (t.id == id) {
                                pd->setMcpVoiceTakeVisible(id, !t.visible);
                                break;
                            }
                        }
                        refresh();
                    };
                    row->onDelete = [this, id] {
                        pd->removeMcpVoiceTake(id);
                        refresh();
                    };
                    addAndMakeVisible(row.get());
                    rows.push_back(std::move(row));
                }
            }

            updateLayout();
        }

        void updateLayout()
        {
            int const w = getWidth();
            if (w <= 20) return;

            int y = 8;
            int const rowW = std::max(40, w - 16);

            brainstormBar.setBounds(8, y, rowW, 30);
            y += 34;
            eraserBar.setBounds(8, y, rowW, 26);
            y += 34;

            // Surgical section: artifacts bound to the current selection.
            if (!attachedRows.empty()) {
                attachedHeaderY = y;
                y += 24;
                for (auto& row : attachedRows) {
                    row->setBounds(8, y, rowW, 42);
                    y += 46;
                }
                y += 6;
            } else {
                attachedHeaderY = -1;
            }

            // Moodboard section: loose parked artifacts.
            stagingHeaderY = y;
            y += 24;
            for (auto& row : rows) {
                row->setBounds(8, y, rowW, 42);
                y += 46;
            }

            setSize(w, std::max(y + 8, getParentComponent() ? getParentComponent()->getHeight() : 100));
            repaint();
        }

        void paint(Graphics& g) override
        {
            auto const textCol = findColour(PlugDataColour::sidebarTextColourId);

            if (!attachedRows.empty() && attachedHeaderY >= 0) {
                g.setColour(juce::Colour(0xff4a9eff).withAlpha(0.92f));
                g.setFont(Font(13.0f, Font::bold));
                g.drawText("Attached to " + (attachedLabel.isNotEmpty() ? attachedLabel : juce::String("selection")),
                           12, attachedHeaderY, getWidth() - 24, 22, Justification::centredLeft, true);
            }

            g.setColour(textCol.withAlpha(0.92f));
            g.setFont(Font(14.0f, Font::bold));
            g.drawText("Staging Bay", 12, stagingHeaderY, getWidth() - 24, 22, Justification::centredLeft, true);

            if (rows.empty() && attachedRows.empty()) {
                g.setColour(textCol.withAlpha(0.4f));
                g.setFont(Font(12.0f, Font::plain));
                g.drawFittedText("Nothing parked yet.\n\nArm the pen and scribble, paste a schematic (Ctrl+V) - audio chips land here soon.\nCommit when the idea is clear.",
                                 getLocalBounds().withTrimmedTop(stagingHeaderY + 26).reduced(16), Justification::centredTop, 5);
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
    {
        // Header Tab Buttons
        notebookTabBtn.text = "Notebook (0)";
        notebookTabBtn.icon = Icons::Documentation;
        notebookTabBtn.active = true;
        notebookTabBtn.onClick = [this] { setTab(PanelTab::Notebook); };
        addAndMakeVisible(notebookTabBtn);

        chatTabBtn.text = "Copilot";
        chatTabBtn.icon = Icons::Message;
        chatTabBtn.active = false;
        chatTabBtn.onClick = [this] { setTab(PanelTab::Chat); };
        addAndMakeVisible(chatTabBtn);

        studioTabBtn.text = "Studio";
        studioTabBtn.icon = Icons::Sparkle;
        studioTabBtn.active = false;
        studioTabBtn.onClick = [this] { setTab(PanelTab::Studio); };
        addAndMakeVisible(studioTabBtn);

        // Clear Button
        clearButton.setTooltip("Clear notebook notes, parked ideas, or chat history");
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
            } else if (currentTab == PanelTab::Studio) {
                if (editor) {
                    if (auto* cnv = editor->getCurrentCanvas()) cnv->clearAllInk();
                }
                refreshStudio();
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
        studioContent.pd = pd;
        studioContent.editor = editor;
        studioContent.parentPanel = this;
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
        // NOTE: no refreshStudio() here — during construction the editor's tab
        // system does not exist yet, and StudioContent reaches for the active canvas.
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
        // Studio mode follows its drawer tab: the pen arms on entry, rests on exit.
        if (editor && newTab != currentTab) {
            if (auto* cnv = editor->getCurrentCanvas()) {
                if (newTab == PanelTab::Studio) cnv->setStudioMode(true);
                else if (currentTab == PanelTab::Studio) cnv->setStudioMode(false);
            }
        }

        currentTab = newTab;
        notebookTabBtn.active = (currentTab == PanelTab::Notebook);
        studioTabBtn.active = (currentTab == PanelTab::Studio);
        chatTabBtn.active = (currentTab == PanelTab::Chat);
        notebookTabBtn.repaint();
        studioTabBtn.repaint();
        chatTabBtn.repaint();

        if (currentTab == PanelTab::Notebook) {
            stopTimer();
            viewport.setViewedComponent(&notebookContent, false);
            inputEditor.setTextToShowWhenEmpty("Add tape note to canvas selection...", findColour(PlugDataColour::sidebarTextColourId).withAlpha(0.5f));
            refreshNotebook();
        } else if (currentTab == PanelTab::Studio) {
            viewport.setViewedComponent(&studioContent, false);
            inputEditor.setTextToShowWhenEmpty("Describe the idea... Enter commits to AI", findColour(PlugDataColour::sidebarTextColourId).withAlpha(0.5f));
            refreshStudio();
            startTimerHz(2); // live: new strokes, pastes and drops appear as parked rows
        } else {
            stopTimer();
            viewport.setViewedComponent(&chatContent, false);
            inputEditor.setTextToShowWhenEmpty("Ask Copilot or type prompt...", findColour(PlugDataColour::sidebarTextColourId).withAlpha(0.5f));
            updateChatLayout();
        }
    }

    void timerCallback() override
    {
        if (currentTab == PanelTab::Studio) refreshStudio();
    }

    void refreshStudio()
    {
        studioContent.pd = pd;
        studioContent.editor = editor;
        studioContent.parentPanel = this;
        studioContent.refresh();

        int const targetH = studioContent.getHeight();
        studioContent.setSize(std::max(10, viewport.getWidth()), std::max(targetH, viewport.getHeight()));
    }

    // Commit the visible moodboard: ink bundle + reference images (in the screenshot)
    // + the typed brief. The server stages strokes silently until this moment.
    void commitStudio()
    {
        if (!pd || !editor) return;
        auto* cnv = editor->getCurrentCanvas();
        if (!cnv) return;

        auto const text = inputEditor.getText().trim();
        inputEditor.setText("", false);
        cnv->sendSketchBuild(text);
        refreshStudio();
    }

    // For the canvas keyboard shortcut (Ctrl/Shift+Enter) — commit without
    // leaving the drawing surface, carrying the drawer's typed brief along.
    juce::String getStudioDraft() const { return inputEditor.getText().trim(); }
    void clearStudioDraft() { inputEditor.setText("", false); }

    void refreshNotebook()
    {
        notebookContent.pd = pd;
        notebookContent.editor = editor;
        notebookContent.parentPanel = this;
        notebookContent.refresh();

        int const count = pd ? static_cast<int>(pd->getMcpAnnotations().size()) : 0;
        notebookTabBtn.text = "Notebook (" + juce::String(count) + ")";
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
        int const tabW = header.getWidth() / 3;
        notebookTabBtn.setBounds(header.removeFromLeft(tabW));
        studioTabBtn.setBounds(header.removeFromLeft(tabW));
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
        } else if (currentTab == PanelTab::Studio) {
            studioContent.setSize(viewport.getWidth(), studioContent.getHeight());
            studioContent.updateLayout();
        } else {
            updateChatLayout();
        }
    }

    void sendMessage()
    {
        // Studio: the input + send button become the Commit action. Text optional —
        // ink and reference images alone can be the brief.
        if (currentTab == PanelTab::Studio) {
            commitStudio();
            return;
        }

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
        juce::String header = "Module Note:";
        if (kind == "change") header = "Change Record:";
        else if (kind == "warn") header = "Warning:";
        else if (kind == "artist") header = "Artist Note:";

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
    HeaderTabButton studioTabBtn;
    HeaderTabButton chatTabBtn;
    SmallIconButton clearButton = SmallIconButton(Icons::Trash);

    BouncingViewport viewport;
    NotebookContent notebookContent;
    StudioContent studioContent;
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
