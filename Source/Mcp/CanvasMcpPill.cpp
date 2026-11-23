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
#include "Mcp/CanvasMcpAiDraw.h"
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
#include "Mcp/McpPillMotion.h"

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

        pillW = 380 + pillLaneChipW; // the prompt field keeps its width; the QUICK / DEEP chip sits before the send arrow
        pillH = 28;
        // Position purely in CANVAS content coords (same space as obj->getBounds()).
        pillX = bounds.getCentreX() - pillW / 2;
        pillY = bounds.getBottom() + 10;
        editorX = 30;
        editorW = pillW - 30 - 32 - pillLaneChipW; // [⚡] ...prompt field... [QUICK|DEEP] [↵]
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
        // The pill body is cream paper (design round 2026-11-19): ink text, an ink caret, a grey selection with ink text.
        auto const inkColour = juce::Colour(mcp::aiInk.r, mcp::aiInk.g, mcp::aiInk.b);
        auto const greyColour = juce::Colour(mcp::aiGrey.r, mcp::aiGrey.g, mcp::aiGrey.b);
        mcpSelectionPill->setColour(juce::TextEditor::textColourId, inkColour);
        mcpSelectionPill->setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        mcpSelectionPill->setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
        mcpSelectionPill->setColour(juce::TextEditor::highlightColourId, greyColour.withAlpha(0.35f));
        mcpSelectionPill->setColour(juce::TextEditor::highlightedTextColourId, inkColour);
        mcpSelectionPill->setColour(juce::CaretComponent::caretColourId, inkColour);
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
                repaintCanvasView();
                return;
            }
            if (mcpSelectionPill) mcpSelectionPill->setText(juce::String(), false);
            dismissSelectionPill(); // Escape closes and sends nothing (and gives a widget chip's edit mode back)
        };
        mcpSelectionPill->onTextChange = [this] {
            repaintCanvasView();
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
    mcpPillAnimator->startTimerHz(mcp::PillAnimTiming::animateHz); // the first tick decides (McpPillMotion.h)
    repaintCanvasView();
}

void CanvasMcpLayer::hideSelectionPill()
{
    mcpWidgetFocus = {}; // the pill is gone by another way than the chip's own (restoreRunMode ran first where it applies): the artist took over
    if (mcpPillAnimator) {
        mcpPillAnimator->stopTimer();
        mcpPillAnimator->last = {}; // the hide below repaints the whole pill spot itself
    }
    mcpPillDrawerMode = 0;
    mcpPillDrawerFrame = {};
    mcpPaletteFrame = {};
    if (mcpSelectionPill && mcpSelectionPill->isVisible()) {
        mcpSelectionPill->setVisible(false);
        repaintCanvasView();
    }
}

void CanvasMcpLayer::dismissSelectionPill()
{
    restoreRunMode(); // every artist dismissal (send, Escape, park, a click away): a widget chip put the canvas in edit mode, it goes back
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

// An ai.* widget's chip ("AI...") = select the widget, open the pill (McpWidgetFocus.h). The chip is pressed in run mode (a locked canvas
// hands the click to the widget) and the pill is an edit-mode thing: the canvas goes to edit mode for it. The order matters: the
// lock change deselects everything, so the selection comes after it.
bool CanvasMcpLayer::focusPillOn(Object* object, mcp::WidgetPillTarget const& target)
{
    if (!object || canvas.isGraph || !canvas.isShowing() || ObjectBase::isBeingEdited())
        return false;
    bool const wasLocked = getValue<bool>(canvas.locked);
    bool const wasPresentation = getValue<bool>(canvas.presentationMode);
    if (wasPresentation) {
        canvas.presentationMode = false;
        canvas.presentationMode.getValueSource().sendChangeMessage(true);
    }
    if (wasLocked) {
        canvas.locked = false;
        canvas.locked.getValueSource().sendChangeMessage(true);
    }
    canvas.deselectAll();
    canvas.setSelected(object, true);
    mcpSelectionPillHideUntil = 0;
    mcpPillSketchMode = false;
    mcpPillRequested = true;
    mcpPillDrawerMode = 0;
    mcpPaletteFrame = {};
    updateSelectionPill();
    if (!mcpSelectionPill || !mcpSelectionPill->isVisible()) {
        // nothing to type into: give the canvas its mode back, the widget asks at once (it hears no `focused`)
        mcpWidgetFocus = { true, wasLocked, wasPresentation, nullptr, {} };
        restoreRunMode();
        mcpPillRequested = false;
        return false;
    }
    mcpSelectionPill->setText(juce::String(), false);
    mcpSelectionPill->grabKeyboardFocus();
    mcpWidgetFocus = { true, wasLocked, wasPresentation, object->getPointer(), target };
    return true;
}

void CanvasMcpLayer::restoreRunMode()
{
    auto const focus = mcpWidgetFocus;
    mcpWidgetFocus = {};
    if (!focus.active)
        return;
    if (focus.wasLocked && !getValue<bool>(canvas.locked)) {
        canvas.locked = true;
        canvas.locked.getValueSource().sendChangeMessage(true);
    }
    if (focus.wasPresentation && !getValue<bool>(canvas.presentationMode)) {
        canvas.presentationMode = true;
        canvas.presentationMode.getValueSource().sendChangeMessage(true);
    }
    if (canvas.editor) canvas.editor->updateCommandStatus(); // the lock change refreshed the toolbar before presentation was back
}

// The widget the pill acts on when Enter finds no text: the one the chip focused (it knows its pd-lua object), else the askable widget
// the one selected box is or holds (a widget the artist selected by hand). Not a single widget = invalid.
mcp::WidgetPillTarget CanvasMcpLayer::selectedWidgetTarget(SmallArray<Object*> const& selected)
{
    if (selected.size() != 1 || !canvas.pd || !selected[0])
        return {};
    t_gobj* const box = selected[0]->getPointer();
    if (mcpWidgetFocus.active && mcpWidgetFocus.box == box && mcpWidgetFocus.target.valid())
        return mcpWidgetFocus.target;
    if (auto* bridge = canvas.pd->getMcp()->getMCPBridge())
        return bridge->widgetTargetOfBox(box);
    return {};
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

        canvas.pd->getMcp()->session.editMcpAnnotations(overlayKey(), [&ann](std::vector<McpSession::McpAnnotation>& notes) { notes.push_back(ann); });
    }

    if (mcpSelectionPill) mcpSelectionPill->setText(juce::String(), false);
    dismissSelectionPill();
    repaintCanvasView();
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
        repaintCanvasView();
        return;
    }

    // What Enter means (McpWidgetFocus.h): text = a normal pill message; no text and exactly one ai.* widget selected = "complete it" (the widget
    // is told to ask, as its chip did before the pill); anything else just closes.
    auto const selected = canvas.getSelectionOfType<Object>();
    mcp::WidgetPillTarget widgetTarget;
    if (prompt.isEmpty())
        widgetTarget = selectedWidgetTarget(selected);
    auto const outcome = mcp::pillAction(mcp::PillKey::Enter, prompt.isEmpty(), static_cast<int>(selected.size()), widgetTarget.valid());
    if (outcome == mcp::PillOutcome::CompleteWidget && canvas.pd) {
        auto* bridge = canvas.pd->getMcp()->getMCPBridge();
        if (!bridge || !bridge->askWidget(widgetTarget))
            canvas.pd->logMessage("AI: " + juce::String(widgetTarget.widget) + " is not listening (nothing is bound to its ai-in name); nothing was asked");
    }

    if (outcome == mcp::PillOutcome::SendPrompt && canvas.pd) {
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

        // Park the prompt in the Sidebar Copilot chat log (no clutter on canvas), with the box it was about ("random 8", "ai.tap",
        // "hsl +2"). The /chat/msg echo of this prompt is dropped by the talk list's duplicate rule.
        if (canvas.editor) {
            if (auto* cp = canvas.editor->getCopilotPanel()) {
                cp->addArtistLine(prompt, CopilotPanel::contextOfSelection(selected));
            }
        }

        // C++ truth: sendPillPrompt resolves the selection's stable tempIds from the
        // bridge-owned map — the server never guesses from its identity mirror.
        // The chip's lane: DEEP sends "queue" on the wire (the deep lane), QUICK "spawn" (the in-app lane).
        auto* laneBridge = canvas.pd->getMcp()->getMCPBridge();
        sendPillPrompt(prompt, queueForChat || (laneBridge != nullptr && laneBridge->isPillDeepLane()));
        mcpSelectionPill->setText(juce::String(), false);
    }

    dismissSelectionPill(); // (it gives a widget chip's edit mode back)
    repaintCanvasView();
}

// A click on the QUICK / DEEP chip: flip the lane here at once (the chip answers the click), then tell the server by the lane
// word, the same words the artist can type; the server switches its sticky lane and pushes it back (/chat/lane).
void CanvasMcpLayer::togglePillLane()
{
    if (!canvas.pd)
        return;
    auto* bridge = canvas.pd->getMcp()->getMCPBridge();
    if (!bridge)
        return;
    bool const deep = !bridge->isPillDeepLane();
    bridge->setPillDeepLane(deep);
    bridge->sendLaneChoice(deep);
    repaintCanvasView();
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
            canvas->repaintCanvasView();
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::downKey && count > 0) {
            canvas->mcpPaletteSelectedIndex = (canvas->mcpPaletteSelectedIndex + 1) % count;
            canvas->repaintCanvasView();
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::returnKey && !key.getModifiers().isShiftDown() && count > 0) {
            canvas->triggerPaletteItem(canvas->mcpPaletteSelectedIndex);
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::escapeKey) {
            canvas->mcpPillDrawerMode = 0;
            canvas->mcpPaletteFrame = {};
            canvas->repaintCanvasView();
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
    if (!(canvas && canvas->mcpSelectionPill && canvas->mcpSelectionPill->isVisible())) {
        stopTimer();
        last = {};
        return;
    }
    // Only the pill animates here (the caret blink, the record pulse, the hint's expiry): repaint the pill, its
    // record box above, its hint below, not the whole editor (that redrew the sidebars, toolbar and statusbar 6 times a
    // second while anything was selected). Alphi calm (McpPillMotion.h): and only while one of those moves, on a canvas
    // that is on screen; otherwise the timer drops to a slow look-in (a state check, no repaint).
    auto& layer = *canvas;
    mcp::PillAnimInput in;
    in.showing = layer.canvas.isShowing();
    in.moodStrip = layer.mcpPillSketchMode && layer.mcpCompletedStrokes.empty();
    in.focused = layer.mcpSelectionPill->hasKeyboardFocus(true);
    in.textEmpty = layer.mcpSelectionPill->getText().isEmpty();
    if (layer.canvas.pd) {
        if (auto* bridge = layer.canvas.pd->getMcp()->getMCPBridge()) {
            in.voiceCapturing = bridge->isVoiceCapturing();
            in.midiCapturing = bridge->isMidiCapturing();
        }
    }
    in.nowMs = static_cast<std::int64_t>(juce::Time::getMillisecondCounter());
    in.hintUntilMs = layer.mcpMoodHint.isNotEmpty() ? static_cast<std::int64_t>(layer.mcpMoodHintUntil) : 0;
    auto const plan = mcp::pillAnimPlan(in);
    int const wantMs = 1000 / (plan.run ? mcp::PillAnimTiming::animateHz : mcp::PillAnimTiming::watchHz);
    if (getTimerInterval() != wantMs)
        startTimer(wantMs);
    // The tick on which the animation stops or shrinks repaints once more with the room the last plan drew in: nothing
    // else asks for it (a capture can end from the bridge, the field's focus can leave with its text kept), and until it
    // runs the red record rim or a visible caret would stay on screen.
    auto const step = mcp::pillAnimStep(last, plan);
    last = plan;
    if (!step.repaint)
        return;
    auto area = layer.mcpSelectionPillFrame.isEmpty() ? layer.mcpSelectionPill->getBounds() : layer.mcpSelectionPillFrame;
    if (step.growX > mcp::PillAnimTiming::caretGrow) {
        // The record box and the hint chip lie outside the pill; the drawer and palette stay in the repaint as before.
        for (auto const& f : { layer.mcpPillDrawerFrame, layer.mcpPaletteFrame })
            if (!f.isEmpty())
                area = area.getUnion(f);
    }
    area = area.expanded(step.growX, step.growY);
    layer.canvas.repaint(area);
    if (auto* ed = layer.canvas.editor) {
        auto& surface = ed->nvgSurface;
        auto dirty = surface.getLocalArea(&layer.canvas, area);
        if (layer.canvas.viewport)
            dirty = dirty.getIntersection(surface.getLocalArea(layer.canvas.viewport.get(), layer.canvas.viewport->getLocalBounds()));
        if (!dirty.isEmpty())
            surface.invalidateArea(dirty);
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
    repaintCanvasView();
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
    // P6.2 U6: the AI's whole last pill turn (the server refuses when anything else changed the canvas since).
    mcpPaletteItems.push_back({ "undo-turn", "Undo AI Turn", "Action", "Alles vom letzten KI-Zug zurueck", "⏪", "[@action:undo-turn]" });
    // P12a.1: no "Flow Mode" item: flow mode is the click on Alphi (the robot face in a corner of the canvas), nothing else.

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
    repaintCanvasView();
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
        repaintCanvasView();
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

// ── The pill's answer line (McpPillAnswer.h) ─────────────────────────────────────────────────────────────────────────
// The newest AI chat line, shown as one short line under the pill's place for ~8 s. The pill itself hides on a send, so
// the line hangs on the pill's LAST frame (kept after the pill hides), not on the editor. Layout and the fade clock are
// message-thread only; the text is cut to one pixel line here (the plain-text cleaning is McpPillAnswer.h).
namespace {
constexpr float answerChipH = 20.0f;
constexpr float answerChipPadX = 11.0f;
constexpr int answerHoldTickMs = 250; // while the line holds: only a tick to notice the fade start
constexpr int answerFadeTickMs = 50;  // while it fades: smooth enough, ~20 Hz on a 20 px strip

std::int64_t answerNowMs()
{
    return juce::Time::currentTimeMillis();
}

// Whole text if it fits `room` pixels, else the longest start of it that fits with "..." (binary search on code points).
juce::String fitAnswerToWidth(NVGcontext* nvg, juce::String const& text, float room)
{
    auto const widthOf = [nvg](juce::String const& s) {
        float b[4];
        mcpnvg::nvgTextBounds(nvg, 0.0f, 0.0f, s.toRawUTF8(), nullptr, b);
        return b[2] - b[0];
    };
    if (widthOf(text) <= room)
        return text;
    int lo = 0;
    int hi = text.length();
    while (lo < hi) {
        int const mid = (lo + hi + 1) / 2;
        if (widthOf(text.substring(0, mid) + "...") <= room)
            lo = mid;
        else
            hi = mid - 1;
    }
    return text.substring(0, lo).trimEnd() + "...";
}
} // namespace

void CanvasMcpLayer::onChatLine(juce::String const& role, juce::String const& text)
{
    // A canvas that never laid out a pill (a graph, one that is locked from the start) has no place for the line: no state, no timer.
    if (mcpSelectionPillFrame.isEmpty())
        return;
    // A waiting line ("Claude Code has not answered yet") shows like an ai line; the answer that follows replaces it.
    auto const shownRole = role == "waiting" ? juce::String("ai") : role;
    if (!mcpPillAnswer.onChat(shownRole.toStdString(), text.toStdString(), answerNowMs()))
        return;
    repaintPillAnswer(); // the new line, or the erased one
    if (mcpPillAnswer.line().empty()) {
        if (mcpPillAnswerTimer)
            mcpPillAnswerTimer->stopTimer();
        return;
    }
    if (!mcpPillAnswerTimer)
        mcpPillAnswerTimer = std::make_unique<PillAnswerTimer>(this);
    mcpPillAnswerTimer->startTimer(answerHoldTickMs);
}

void CanvasMcpLayer::PillAnswerTimer::timerCallback()
{
    if (!canvas) {
        stopTimer();
        return;
    }
    float const alpha = canvas->mcpPillAnswer.alpha(answerNowMs());
    canvas->repaintPillAnswer(); // at alpha 0 this erases the last drawn chip
    if (alpha <= 0.0f)
        stopTimer();
    else
        startTimer(alpha >= 1.0f ? answerHoldTickMs : answerFadeTickMs);
}

void CanvasMcpLayer::repaintPillAnswer()
{
    if (mcpSelectionPillFrame.isEmpty())
        return;
    auto const f = mcpSelectionPillFrame;
    auto const area = juce::Rectangle<int>(f.getX() - 4, f.getBottom() + 2, std::max(f.getWidth(), 380) + 8, static_cast<int>(answerChipH) + 14);
    canvas.repaint(area);
    if (auto* ed = canvas.editor) {
        auto& surface = ed->nvgSurface;
        auto dirty = surface.getLocalArea(&canvas, area);
        if (canvas.viewport)
            dirty = dirty.getIntersection(surface.getLocalArea(canvas.viewport.get(), canvas.viewport->getLocalBounds()));
        if (!dirty.isEmpty())
            surface.invalidateArea(dirty);
    }
}

void CanvasMcpLayer::paintPillAnswer(NVGcontext* nvg)
{
    float const alpha = mcpPillAnswer.alpha(answerNowMs());
    if (alpha <= 0.0f || mcpSelectionPillFrame.isEmpty())
        return;
    // An open drawer or palette sits right under the pill: no chip on top of it.
    if (mcpSelectionPill && mcpSelectionPill->isVisible() && mcpPillDrawerMode != 0)
        return;

    auto const frame = mcpSelectionPillFrame;
    float const maxW = static_cast<float>(std::max(frame.getWidth(), 380));
    NVGScopedState scoped(nvg);
    mcpnvg::nvgFontFace(nvg, "Inter");
    mcpnvg::nvgFontSize(nvg, 11.5f);
    mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    auto const shown = fitAnswerToWidth(nvg, juce::String::fromUTF8(mcpPillAnswer.line().c_str()), maxW - answerChipPadX * 2.0f - 6.0f);
    float const x = static_cast<float>(frame.getX());
    float const y = static_cast<float>(frame.getBottom()) + 6.0f;
    // Design round B4: no dark chip. The line is a small filled ink circle (5 px) and the text in ink with a halo in the
    // canvas colour (so it reads over a wire); both fade with the line. A real tag shape waits for the server to say the
    // kind (open question 4).
    float const midY = y + answerChipH * 0.5f;
    nanovg::nvgBeginPath(nvg);
    nanovg::nvgCircle(nvg, x + 3.5f, midY, 2.5f);
    nanovg::nvgFillColor(nvg, mcpai::colour(mcp::aiInk, alpha));
    nanovg::nvgFill(nvg);
    auto halo = nvgColour(getThemeColours(canvas).canvasBackgroundColour);
    halo.a *= alpha;
    mcpai::drawHaloText(nvg, x + 12.0f, midY, shown.toRawUTF8(), halo, mcpai::colour(mcp::aiInk, alpha), "Inter", 11.5f);
}
