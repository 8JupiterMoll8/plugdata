/*
 // The MCP layer of one Canvas (Plan 3 P3.2 S8), part Pill: the selection pill: show/hide/dismiss/park/submit, its key listeners and animator, lenses/tools menus, the smart palette, prompt dispatch.
 // Moved verbatim from Source/Canvas.cpp; implicit Canvas members became `canvas.`
 // accesses. See CanvasMcpLayer.h.
 */
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_cryptography/juce_cryptography.h>
#include "Utility/Config.h"
#include "Utility/Fonts.h"
#include "Utility/NVGTextShim.h"

#include "Mcp/CanvasMcpLayer.h"
#include "Sidebar/Sidebar.h"
#include "Sidebar/CopilotPanel.h"
#include "Canvas.h"
#include "Object.h"
#include "Connection.h"
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "LookAndFeel.h"
#include "CanvasViewport.h"
#include "Objects/ObjectBase.h"
#include "Pd/MCPBridge.h"
#include "Mcp/McpHost.h"

void CanvasMcpLayer::updateSelectionPill()
{
    // IMMUNITY: Never show the selection pill while ANY object or comment text is being edited!
    // Native Pure Data object naming + autocomplete suggestor must have 100% exclusive priority.
    if (ObjectBase::isBeingEdited()) {
        mcpPillSketchMode = false;
        mcpPillRequested = false;
        hideSelectionPill();
        return;
    }

    if (canvas.isGraph || getValue<bool>(canvas.locked) || canvas.presentationMode.getValue()) {
        mcpPillSketchMode = false;
        mcpPillRequested = false;
        hideSelectionPill();
        return;
    }

    auto selectedObjects = canvas.getSelectionOfType<Object>();
    // The chisel owns the canvas while ink exists, the pen/eraser is up, or a
    // fresh mood palette was opened; otherwise hide.
    if (selectedObjects.empty() && mcpCompletedStrokes.empty() && !mcpSketchToolActive && !mcpEraserToolActive && !mcpMoodPillActive) {
        mcpPillSketchMode = false;
        mcpPillRequested = false;
        hideSelectionPill();
        return;
    }

    // Object mode: only show the pill if the artist explicitly requested it (e.g. hit "/"),
    // or if Studio mode is active, or if ink was sketched over the selection.
    // This stops the 480px pill from obstructing cords during everyday box dragging!
    if (!selectedObjects.empty() && !mcpPillRequested && !mcpStudioMode && mcpCompletedStrokes.empty()) {
        hideSelectionPill();
        return;
    }

    // Brief grace window after a send/dismiss so an immediate sidebar/selection
    // refresh can't re-pop the pill. Time-based (not keyed to the selection), so
    // re-selecting the SAME object later always brings it back.
    if (juce::Time::getMillisecondCounter() < mcpSelectionPillHideUntil) return;

    // Keep canvas 100% clean — never pop up floating chisel bar on empty canvas!
    if (selectedObjects.empty()) {
        mcpPillSketchMode = false;
        hideSelectionPill();
        return;
    }
    bool const sketchMode = false;
    mcpPillSketchMode = false;

    int pillH = 32;
    int pillW = 0;
    int pillX = 0;
    int pillY = 0;
    int editorX = 0;
    int editorW = 0;

    bool sketchStrip = false;
    if (sketchMode) {
        // Prefer the live ink bbox; fall back to the last one so the chisel stays
        // parked after [🗑 Clear]; fall back again to the fresh mood-palette click.
        auto const liveBounds = getSketchBoundsInCanvas();
        if (!liveBounds.isEmpty()) mcpLastSketchBounds = liveBounds;
        juce::Rectangle<float> sb = !liveBounds.isEmpty() ? liveBounds : mcpLastSketchBounds;
        if (sb.isEmpty() && mcpMoodPillActive) {
            sb = juce::Rectangle<float>(static_cast<float>(mcpMoodPillPos.x), static_cast<float>(mcpMoodPillPos.y), 1.0f, 1.0f);
        }
        if (sb.isEmpty()) {
            mcpPillSketchMode = false;
            hideSelectionPill();
            return;
        }
        if (mcpCompletedStrokes.empty()) {
            mcpPillSketchMode = false;
            hideSelectionPill();
            return;
        }
        sketchStrip = false;
        pillW = 520;
        pillX = juce::roundToInt(sb.getCentreX()) - pillW / 2;
        pillY = juce::roundToInt(sb.getBottom()) + 14;
        editorX = 92; // [⚡ Build] chip
        editorW = pillW - 92 - 238; // ...prompt... [T] [🖼] [↩ Undo] [🗑 Clear] [✏️ pen]
        mcpPillDrawerMode = 0;
        mcpPillDrawerFrame = {};
        mcpPaletteFrame = {};
    } else {
        // Object::getBounds() is already in Canvas component coordinates, so the pill
        // scrolls/zooms with the patch for free (canvasOrigin is constant).
        Rectangle<int> bounds;
        bool first = true;
        for (auto const* obj : selectedObjects) {
            if (!obj) continue;
            auto const ob = obj->getBounds();
            bounds = first ? ob : bounds.getUnion(ob);
            first = false;
        }
        if (first) {
            hideSelectionPill();
            return;
        }

        pillW = 380;
        pillH = 28;
        // Position purely in CANVAS content coords (same space as obj->getBounds()).
        pillX = bounds.getCentreX() - pillW / 2;
        pillY = bounds.getBottom() + 10;
        editorX = 30;
        editorW = pillW - 30 - 32; // [⚡] ...prompt field... [↵]
    }

    mcpSelectionPillFrame = juce::Rectangle<int>(pillX, pillY, pillW, pillH);
    if (!sketchMode) {
        mcpPillDrawerMode = 0;
        mcpPillDrawerFrame = {};
        mcpPaletteFrame = {};
    }

    if (!mcpSelectionPill) {
        mcpSelectionPill = std::make_unique<juce::TextEditor>();
        mcpSelectionPill->getProperties().set("NoBackground", true);
        mcpSelectionPill->getProperties().set("NoOutline", true);
        mcpSelectionPill->setMultiLine(false);
        mcpSelectionPill->setReturnKeyStartsNewLine(false);
        mcpSelectionPill->setWantsKeyboardFocus(true);
        mcpSelectionPill->setColour(juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
        mcpSelectionPill->setColour(juce::TextEditor::textColourId, juce::Colours::white);
        mcpSelectionPill->setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        mcpSelectionPill->setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
        mcpSelectionPill->setColour(juce::TextEditor::highlightColourId, juce::Colour(0xff4a9eff));
        mcpSelectionPill->setColour(juce::CaretComponent::caretColourId, juce::Colours::white);
        mcpSelectionPill->setFont(juce::Font("Inter", 13.0f, juce::Font::plain));
        mcpSelectionPill->applyFontToAllText(juce::Font("Inter", 13.0f, juce::Font::plain));
        mcpSelectionPill->setBorder(juce::BorderSize<int>(0, 0, 0, 0));
        mcpSelectionPill->setIndents(2, 0);
        mcpSelectionPill->setJustification(juce::Justification::centredLeft);

        mcpSelectionPill->onReturnKey = [this] { submitSelectionPill(false); };
        mcpSelectionPillKeys = std::make_unique<SelectionPillKeyListener>(this);
        mcpSelectionPill->addKeyListener(mcpSelectionPillKeys.get());
        if (!mcpSlotPasteKeys) mcpSlotPasteKeys = std::make_unique<SlotPasteKeyListener>(this);
        mcpSelectionPill->addKeyListener(mcpSlotPasteKeys.get());
        mcpSelectionPill->onEscapeKey = [this] {
            if (mcpPillDrawerMode != 0) {
                mcpPillDrawerMode = 0;
                mcpPillDrawerFrame = {};
                mcpPaletteFrame = {};
                canvas.repaint();
                if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
                return;
            }
            if (mcpSelectionPill) mcpSelectionPill->setText(juce::String(), false);
            dismissSelectionPill();
        };
        mcpSelectionPill->onTextChange = [this] {
            canvas.repaint();
            if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
        };
        mcpSelectionPill->onFocusLost = [this] {
            // Keep a typed prompt, but if it's empty and drawer is closed get out of the way.
            if (mcpSelectionPill && mcpSelectionPill->getText().trim().isEmpty() && mcpPillDrawerMode == 0)
                dismissSelectionPill();
        };
        canvas.addAndMakeVisible(*mcpSelectionPill);
    }

    // Object mode: room on the left for + and on the right for 🎙️ Mic.
    // Sketch mode: room for the [⚡ Build] chip and the [↩ Undo] [🗑 Clear] pair.
    // edH = 30 gives generous headroom and descender room so text is never clipped at the bottom.
    int const edH = 30;
    int const edY = pillY + (pillH - edH) / 2;
    if (sketchStrip) {
        // Mood strip: no prompt field. Park the editor off-cells (still "visible"
        // so hiding it can't trigger a focus-loss dismiss).
        mcpSelectionPill->setBounds(pillX - 60, pillY, 1, 1);
    } else {
        mcpSelectionPill->setBounds(pillX + editorX, edY, editorW, edH);
    }
    mcpSelectionPill->setVisible(true);
    // toFront(false), NOT true: bringing it forward must never steal keyboard
    // focus, or typing 'o'/'m'/'c'/'b' on the canvas would land in the prompt
    // instead of creating/editing objects.
    mcpSelectionPill->toFront(false);
    if (!mcpPillAnimator) {
        mcpPillAnimator = std::make_unique<SelectionPillAnimator>(this);
    }
    mcpPillAnimator->startTimerHz(6);
    canvas.repaint();
    if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
}

void CanvasMcpLayer::hideSelectionPill()
{
    if (mcpPillAnimator) {
        mcpPillAnimator->stopTimer();
    }
    mcpPillDrawerMode = 0;
    mcpPillDrawerFrame = {};
    mcpPaletteFrame = {};
    if (mcpSelectionPill && mcpSelectionPill->isVisible()) {
        mcpSelectionPill->setVisible(false);
        canvas.repaint();
        if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
    }
}

void CanvasMcpLayer::dismissSelectionPill()
{
    mcpPillDrawerMode = 0;
    mcpPillDrawerFrame = {};
    mcpPaletteFrame = {};
    mcpMoodPillActive = false;
    mcpPillRequested = false;
    // Artist dismissed (send / Esc / empty focus-out): suppress re-show for a
    // short window so the immediate refresh can't re-pop it. Time-based, so
    // re-selecting the same object later always brings it back.
    mcpSelectionPillHideUntil = juce::Time::getMillisecondCounter() + 700;
    hideSelectionPill();
}

// Shift+Enter: park, don't wake the AI. The typed intent becomes a tape note
// (bound to the selection when there is one); ink and images stay staged.
void CanvasMcpLayer::parkSelectionPill()
{
    if (!canvas.pd) {
        dismissSelectionPill();
        return;
    }

    auto const prompt = mcpSelectionPill ? mcpSelectionPill->getText().trim() : juce::String();
    if (prompt.isNotEmpty()) {
        McpSession::McpAnnotation ann;
        ann.kind = "artist";
        ann.text = prompt;
        ann.t = juce::Time::getMillisecondCounterHiRes() / 1000.0;

        auto const selIds = getSelectionStableIds();
        if (selIds.size() > 0) {
            ann.targetId = selIds[0];
            ann.targetIds = selIds;
        } else {
            auto sb = getSketchBoundsInCanvas();
            if (sb.isEmpty() && mcpMoodPillActive) {
                ann.x = static_cast<float>(mcpMoodPillPos.x - canvas.canvasOrigin.x);
                ann.y = static_cast<float>(mcpMoodPillPos.y - canvas.canvasOrigin.y);
            } else {
                ann.x = sb.getCentreX() - static_cast<float>(canvas.canvasOrigin.x);
                ann.y = sb.getCentreY() - static_cast<float>(canvas.canvasOrigin.y);
            }
        }

        juce::ScopedLock sl(canvas.pd->getMcp()->session.mcpOverlayLock);
        canvas.pd->getMcp()->session.mcpAnnotations.push_back(ann);
    }

    if (mcpSelectionPill) mcpSelectionPill->setText(juce::String(), false);
    dismissSelectionPill();
    canvas.repaint();
    if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
}

void CanvasMcpLayer::submitSelectionPill(bool queueForChat)
{
    mcpPillDrawerMode = 0;
    mcpPillDrawerFrame = {};
    mcpPaletteFrame = {};
    if (!mcpSelectionPill) return;

    auto const prompt = mcpSelectionPill->getText().trim();

    // Sketch mode: Enter / Shift+Enter = Commit & Build the staged ink.
    if (mcpPillSketchMode) {
        sendSketchBuild(prompt);
        mcpSelectionPill->setText(juce::String(), false);
        // Keep the chisel parked so [↩ Undo] / [🗑 Clear] stay one tap away.
        updateSelectionPill();
        canvas.repaint();
        if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
        return;
    }

    if (prompt.isNotEmpty() && canvas.pd) {
        // Find selection stable tempIds & compute group bounding box
        juce::StringArray targetIds;
        bool first = true;
        int minX = 0, minY = 0, maxX = 0, maxY = 0;
        auto patchPtr = canvas.patch.getPointer();
        t_canvas* cnv = patchPtr ? patchPtr.get() : nullptr;
        for (auto* obj : canvas.getSelectionOfType<Object>()) {
            if (auto* ptr = obj->getPointer()) {
                auto tid = canvas.pd->getMcp()->getStableId(ptr);
                if (tid.isEmpty() && cnv) {
                    tid = canvas.pd->getMcp()->getOrAdoptStableId(cnv, ptr);
                }
                if (tid.isNotEmpty()) targetIds.add(tid);
            }
            auto const b = obj->getObjectBounds();
            if (first) {
                minX = b.getX(); minY = b.getY();
                maxX = b.getRight(); maxY = b.getBottom();
                first = false;
            } else {
                minX = std::min(minX, b.getX());
                minY = std::min(minY, b.getY());
                maxX = std::max(maxX, b.getRight());
                maxY = std::max(maxY, b.getBottom());
            }
        }

        // Park the prompt in the Sidebar Copilot chat log (no clutter on canvas)
        if (canvas.editor) {
            if (auto* cp = canvas.editor->getCopilotPanel()) {
                cp->receiveMessage("user", prompt);
            }
        }

        // C++ truth: sendPillPrompt resolves the selection's stable tempIds from the
        // bridge-owned map — the server never guesses from its identity mirror.
        sendPillPrompt(prompt, queueForChat);
        mcpSelectionPill->setText(juce::String(), false);
    }

    dismissSelectionPill();
    canvas.repaint();
    if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
}

bool CanvasMcpLayer::SlotPasteKeyListener::keyPressed(const juce::KeyPress& key, juce::Component*)
{
    if (!canvas || !canvas->mcpImageSlotActive) return false;
    auto const kc = key.getKeyCode();
    bool const ctrlV = (key.getModifiers().isCtrlDown() || key.getModifiers().isCommandDown())
        && (kc == 'v' || kc == 'V' || kc == 22 /* Ctrl+V control code */);
    if (ctrlV) {
        canvas->pasteImageFromClipboard();
        return true;
    }
    return false;
}

bool CanvasMcpLayer::SelectionPillKeyListener::keyPressed(const juce::KeyPress& key, juce::Component*)
{    if (canvas && canvas->mcpPillDrawerMode == 3) {
        auto const count = static_cast<int>(canvas->mcpFilteredPaletteIndices.size());
        if (key.getKeyCode() == juce::KeyPress::upKey && count > 0) {
            canvas->mcpPaletteSelectedIndex = (canvas->mcpPaletteSelectedIndex - 1 + count) % count;
            canvas->canvas.repaint();
            if (canvas->canvas.editor) canvas->canvas.editor->nvgSurface.invalidateAll();
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::downKey && count > 0) {
            canvas->mcpPaletteSelectedIndex = (canvas->mcpPaletteSelectedIndex + 1) % count;
            canvas->canvas.repaint();
            if (canvas->canvas.editor) canvas->canvas.editor->nvgSurface.invalidateAll();
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::returnKey && !key.getModifiers().isShiftDown() && count > 0) {
            canvas->triggerPaletteItem(canvas->mcpPaletteSelectedIndex);
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::escapeKey) {
            canvas->mcpPillDrawerMode = 0;
            canvas->mcpPaletteFrame = {};
            canvas->canvas.repaint();
            if (canvas->canvas.editor) canvas->canvas.editor->nvgSurface.invalidateAll();
            return true;
        }
    }
    // Shift+Enter = Park: keep the artifacts + typed intent in the bay without
    // waking the AI. (Plain Enter is the send — TextEditor fires onReturnKey.)
    if (key.getKeyCode() == juce::KeyPress::returnKey && key.getModifiers().isShiftDown()) {
        if (canvas) canvas->parkSelectionPill();
        return true;
    }
    // "/" opens the on-demand smart command palette if input is empty
    if (key.getTextCharacter() == '/' || key.getKeyCode() == '/') {
        if (canvas && canvas->mcpSelectionPill && canvas->mcpSelectionPill->getText().trim().isEmpty()) {
            canvas->showPillToolsMenu();
            return true;
        }
    }
    // "*" (the ✦ key) opens the Lenses drawer if input is empty
    if (key.getTextCharacter() == '*') {
        if (canvas && canvas->mcpSelectionPill && canvas->mcpSelectionPill->getText().trim().isEmpty()) {
            canvas->showPillLensesMenu();
            return true;
        }
    }
    return false;
}

void CanvasMcpLayer::SelectionPillAnimator::timerCallback()
{
    if (canvas && canvas->mcpSelectionPill && canvas->mcpSelectionPill->isVisible()) {
        canvas->canvas.repaint();
        if (canvas->canvas.editor) canvas->canvas.editor->nvgSurface.invalidateAll();
    } else {
        stopTimer();
    }
}

void CanvasMcpLayer::showPillLensesMenu()
{
    if (!mcpSelectionPill || !mcpSelectionPill->isVisible()) return;
    mcpPillDrawerMode = (mcpPillDrawerMode == 1 ? 0 : 1);
    if (mcpPillDrawerMode != 0) {
        auto const p = mcpSelectionPillFrame.isEmpty() ? mcpSelectionPill->getBounds() : mcpSelectionPillFrame;
        mcpPillDrawerFrame = juce::Rectangle<int>(p.getX(), p.getBottom() + 5, p.getWidth(), 30);
    } else {
        mcpPillDrawerFrame = {};
    }
    canvas.repaint();
    if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
}

void CanvasMcpLayer::configurePillLookAndFeel()
{
}

void CanvasMcpLayer::showPillToolsMenu()
{
    showPillPalette();
}

void CanvasMcpLayer::initPaletteItems()
{
    if (!mcpPaletteItems.empty()) return;

    // ⚡ Actions (FastPath - 0 Tokens, 5 ms)
    mcpPaletteItems.push_back({ "tidy", "Tidy Layout", "Action", "Begradigt Kabel & Spalten (0 Tokens)", "⚡", "[@action:tidy]" });
    mcpPaletteItems.push_back({ "pack-gop", "Pack into GOP", "Action", "Erstellt Eurorack-Modul mit Faceplate", "📦", "[@action:pack-gop]" });
    mcpPaletteItems.push_back({ "array10", "10x Osc Array", "Action", "Boilerplate-Bank mit Gain-Schutz", "🔢", "[@action:array10]" });
    mcpPaletteItems.push_back({ "add-knobs", "Add Controls", "Action", "Regler fuer Signal-Inlets erzeugen", "🎛️", "[@action:add-knobs]" });
    mcpPaletteItems.push_back({ "undo", "Undo Step", "Action", "Letzte Aenderung sofort rueckgaengig", "↩️", "[@action:undo]" });

    // 🧬 Skills (AI Sounddesign)
    mcpPaletteItems.push_back({ "kick-drum", "Punchy 909 Kick", "Skill", "Pitch-Sweep & [tanh~] Saettigung", "🥁", "[@skill:kick-drum]" });
    mcpPaletteItems.push_back({ "acid-bass", "Acid 303 Bass", "Skill", "Resonanz-Filter mit Slide & Envelope", "🎛️", "[@skill:acid-bass]" });
    mcpPaletteItems.push_back({ "ambient-verb", "Ambient Space Verb", "Skill", "Schwebende Hallfahne mit Resonator", "🌌", "[@skill:ambient-verb]" });
    mcpPaletteItems.push_back({ "tape-drive", "Lo-Fi Tape Saturator", "Skill", "Analoger Bandschmutz mit Wow/Flutter", "📻", "[@skill:tape-drive]" });
    mcpPaletteItems.push_back({ "genesis", "Chaos Genesis", "Skill", "Experimentelle ELSE-Kreuzmodulation", "🧬", "[@skill:genesis]" });

    // ✦ Lenses (Personas)
    mcpPaletteItems.push_back({ "doctor", "Doctor Lens", "Lens", "Röntgenblick auf Stille, Fehler & Klicks", "🩺", "[@lens:doctor]" });
    mcpPaletteItems.push_back({ "jam", "Jam Lens", "Lens", "Schneller, kreativer Co-Produzent", "⚡", "[@lens:jam]" });
    mcpPaletteItems.push_back({ "modular", "Modular Lens", "Lens", "Eurorack CV & Voltage Routing", "🔌", "[@lens:modular]" });

    filterPaletteItems({});
}

void CanvasMcpLayer::filterPaletteItems(const juce::String& query)
{
    mcpFilteredPaletteIndices.clear();
    auto q = query.trim().toLowerCase();
    if (q.startsWithChar('/')) q = q.substring(1).trim();

    for (size_t i = 0; i < mcpPaletteItems.size(); ++i) {
        if (q.isEmpty() ||
            mcpPaletteItems[i].title.toLowerCase().contains(q) ||
            mcpPaletteItems[i].desc.toLowerCase().contains(q) ||
            mcpPaletteItems[i].category.toLowerCase().contains(q)) {
            mcpFilteredPaletteIndices.push_back(static_cast<int>(i));
        }
    }
    mcpPaletteSelectedIndex = 0;
}

void CanvasMcpLayer::showPillPalette()
{
    if (!mcpSelectionPill || !mcpSelectionPill->isVisible()) return;
    initPaletteItems();
    mcpPillDrawerMode = (mcpPillDrawerMode == 3 ? 0 : 3);
    if (mcpPillDrawerMode == 3) {
        filterPaletteItems({});
    } else {
        mcpPaletteFrame = {};
    }
    canvas.repaint();
    if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
}

void CanvasMcpLayer::triggerPaletteItem(int index)
{
    if (index >= 0 && index < static_cast<int>(mcpFilteredPaletteIndices.size())) {
        auto const& item = mcpPaletteItems[static_cast<size_t>(mcpFilteredPaletteIndices[static_cast<size_t>(index)])];
        mcpPillDrawerMode = 0;
        mcpPaletteFrame = {};

        juce::String currentText = mcpSelectionPill ? mcpSelectionPill->getText().trim() : juce::String();

        // 1-Click FastPath convenience:
        // If the text editor was completely empty and user selected a pure action (like Tidy or Undo):
        if (currentText.isEmpty() && (item.category == "Action" && item.id != "array10")) {
            sendPillPrompt(item.prompt, false);
            dismissSelectionPill();
            return;
        }

        // Composable Tag Insertion:
        // Append tag so user can either hit Enter immediately or keep typing!
        juce::String newText = currentText.isEmpty() ? (item.prompt + " ") : (currentText + " " + item.prompt + " ");
        if (mcpSelectionPill) {
            mcpSelectionPill->setText(newText, true);
            mcpSelectionPill->setCaretPosition(newText.length());
            mcpSelectionPill->grabKeyboardFocus();
        }
        canvas.repaint();
        if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
    }
}

void CanvasMcpLayer::sendPillPrompt(const juce::String& prompt, bool queueForChat)
{
    if (prompt.isEmpty() || !canvas.pd) return;
    juce::StringArray targetIds;
    auto patchPtr = canvas.patch.getPointer();
    t_canvas* cnv = patchPtr ? patchPtr.get() : nullptr;
    for (auto* obj : canvas.getSelectionOfType<Object>()) {
        if (auto* ptr = obj->getPointer()) {
            auto tid = canvas.pd->getMcp()->getStableId(ptr);
            if (tid.isEmpty() && cnv) {
                tid = canvas.pd->getMcp()->getOrAdoptStableId(cnv, ptr);
            }
            if (tid.isNotEmpty()) targetIds.add(tid);
        }
    }
    if (auto* br = canvas.pd->getMcp()->getMCPBridge()) br->sendSelectionPrompt(prompt, targetIds, queueForChat);
}
