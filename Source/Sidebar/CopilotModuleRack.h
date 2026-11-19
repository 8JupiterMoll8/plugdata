/*
 // The Copilot sidebar's module rack: one card per ai.* widget, in groups (idea kit / rhythm / melody / sound / system),
 // drag a card to the patch. The data (name, one-line purpose, group, the object a drag creates) is Mcp/McpWidgetRack.h
 // (JUCE-free, unit-tested); this file is the list and the cards. Included by CopilotPanel.h.
 //
 // A card (design round 2026-11-19, picture P: cream, 1 px ink outline, no colour dots) shows the name, the purpose in up to
 // three lines and, when the file exists, a 56 px thumbnail on the right:
 //   <app data dir>/Abstractions/thumbs/<name>.png   (`~` and other odd characters in the name become `_`: ai.loop~ -> ai.loop_.png)
 // The folder is read when the rack is built and again each time it is shown; nothing is generated. All of it runs on the
 // message thread.
 */

#pragma once

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "Mcp/McpWidgetRack.h"
#include "Sidebar/CopilotLook.h"

class CopilotModuleRack final : public Component {
public:
    static constexpr int headerH = 22;
    static constexpr int cardH = 68;
    static constexpr int cardGap = 6;
    static constexpr int thumbSize = 56;
    static constexpr int sidePad = 12;

    // One card. Drag (after 3 px) pastes the widget at the drop point, like the object menu's drag and drop.
    class Card final : public Component {
    public:
        Card(PluginEditor* e, mcp::RackWidget const& w)
            : editor(e)
            , widget(w)
        {
            // In touch mode a drag scrolls the list; on a card it must start the drag to the patch instead.
            setViewportIgnoreDragFlag(true);
        }

        void loadThumb(File const& thumbsDir)
        {
            thumb = Image();
            auto const file = thumbsDir.getChildFile(String(mcp::rackThumbFileName(widget.name)));
            if (file.existsAsFile() && file.getSize() < 4 * 1024 * 1024)
                thumb = ImageFileFormat::loadFrom(file);
            repaint();
        }

        void mouseDown(MouseEvent const&) override { dragStarted = false; }

        void mouseDrag(MouseEvent const& e) override
        {
            if (!dragStarted && e.getDistanceFromDragStart() > 3) {
                dragStarted = true;
                ObjectDragAndDrop::attachToMouse(editor, String(mcp::rackPatchText(widget, looperNameForDrag())));
            }
        }

        void mouseEnter(MouseEvent const&) override { hovered = true; repaint(); }
        void mouseExit(MouseEvent const&) override { hovered = false; repaint(); }

        void paint(Graphics& g) override
        {
            auto b = getLocalBounds().toFloat().reduced(1.0f);
            bool const over = hovered; // from mouseEnter/Exit: isMouseOver() is an XQueryPointer inside paint

            // Cream with a 1 px ink outline and a 2 px corner; a hovered card has a 2 px outline.
            g.setColour(copilotlook::cream());
            g.fillRoundedRectangle(b, mcp::aiRadius);
            g.setColour(copilotlook::ink());
            g.drawRoundedRectangle(over ? b.reduced(0.5f) : b, mcp::aiRadius, over ? mcp::aiLineWidthActive : mcp::aiLineWidth);

            float const textLeft = b.getX() + static_cast<float>(mcp::aiPadding);
            float textRight = b.getRight() - static_cast<float>(mcp::aiPadding);
            if (thumb.isValid()) {
                auto const box = Rectangle<float>(b.getRight() - 6.0f - thumbSize, b.getCentreY() - thumbSize * 0.5f, static_cast<float>(thumbSize), static_cast<float>(thumbSize));
                Path clip;
                clip.addRoundedRectangle(box, mcp::aiRadius);
                g.saveState();
                g.reduceClipRegion(clip);
                g.drawImage(thumb, box, RectanglePlacement::centred | RectanglePlacement::onlyReduceInSize, false);
                g.restoreState();
                textRight = box.getX() - 8.0f;
            }

            g.setFont(Fonts::getSemiBoldFont().withHeight(mcp::aiNameSize));
            g.setColour(copilotlook::ink());
            g.drawText(String(std::string(widget.name)), Rectangle<float>(textLeft, b.getY() + 8.0f, textRight - textLeft, 16.0f),
                Justification::centredLeft, true);

            g.setFont(Fonts::getDefaultFont().withHeight(mcp::aiHintSize));
            g.setColour(copilotlook::grey());
            g.drawFittedText(String(std::string(widget.purpose)),
                Rectangle<float>(textLeft, b.getY() + 26.0f, textRight - textLeft, b.getHeight() - 32.0f).toNearestInt(),
                Justification::topLeft, 3, 1.0f);
        }

    private:
        // ai.loop~ is named by its first argument and the name is its shared bus: pick one no open tab uses. Only the looper
        // needs it; the other widgets ignore the name.
        std::string looperNameForDrag() const
        {
            if (widget.object.find("{name}") == std::string_view::npos)
                return {};
            std::set<std::string> used;
            if (editor) {
                for (auto* cnv : editor->getCanvases()) {
                    if (!cnv)
                        continue;
                    for (auto* object : cnv->objects) {
                        if (!object->gui)
                            continue;
                        auto const tokens = StringArray::fromTokens(object->gui->getText().toString(), false);
                        if (tokens.size() >= 2 && tokens[0] == "ai.loop~")
                            used.insert(tokens[1].toStdString());
                    }
                }
            }
            return mcp::rackFreeLooperName([&used](std::string const& name) { return used.count(name) > 0; });
        }

        PluginEditor* editor;
        mcp::RackWidget widget;
        Image thumb;
        bool hovered = false;
        bool dragStarted = false;
    };

    explicit CopilotModuleRack(PluginEditor* e)
    {
        for (auto const& w : mcp::kRackWidgets) {
            auto card = std::make_unique<Card>(e, w);
            addAndMakeVisible(*card);
            cards.push_back(std::move(card));
        }
        reloadThumbs();
    }

    // The list's own height: a header per group and a card per widget; independent of the width.
    static int getContentHeight()
    {
        int h = 4;
        for (auto const group : mcp::kRackGroups) {
            int count = 0;
            for (auto const& w : mcp::kRackWidgets)
                count += (w.group == group) ? 1 : 0;
            if (count > 0)
                h += headerH + count * (cardH + cardGap) + 4;
        }
        return h;
    }

    static File getThumbsDir() { return ProjectInfo::appDataDir.getChildFile("Abstractions").getChildFile("thumbs"); }

    void reloadThumbs()
    {
        auto const dir = getThumbsDir();
        for (auto& card : cards)
            card->loadThumb(dir);
    }

    void visibilityChanged() override
    {
        if (isShowing())
            reloadThumbs();
    }

    void resized() override
    {
        headers.clear();
        int y = 4;
        for (auto const group : mcp::kRackGroups) {
            bool any = false;
            for (size_t i = 0; i < mcp::kRackWidgetCount; ++i) {
                if (mcp::kRackWidgets[i].group != group)
                    continue;
                if (!any) {
                    headers.push_back({ group, y });
                    y += headerH;
                    any = true;
                }
                cards[i]->setBounds(sidePad, y, std::max(10, getWidth() - sidePad * 2), cardH);
                y += cardH + cardGap;
            }
            if (any)
                y += 4;
        }
    }

    void paint(Graphics& g) override
    {
        g.setFont(Fonts::getDefaultFont().withHeight(mcp::aiHintSize));
        g.setColour(copilotlook::grey());
        for (auto const& header : headers)
            g.drawText(String(std::string(mcp::rackGroupTitle(header.group))).toLowerCase(), Rectangle<int>(sidePad, header.y, getWidth() - sidePad * 2, headerH),
                Justification::centredLeft, false);
    }

private:
    struct Header {
        mcp::RackGroup group;
        int y;
    };

    std::vector<std::unique_ptr<Card>> cards;
    std::vector<Header> headers;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CopilotModuleRack)
};
