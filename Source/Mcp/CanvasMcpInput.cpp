/*
 // The MCP layer of one Canvas (Plan 3 P3.2 S8), part Input: the input and lifecycle hook bodies, cut out of the Canvas handlers they are called from (mouseDown, mouseDrag, mouseUp, keyPressed, destructor, synchronise, undo/redo).
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

bool CanvasMcpLayer::mouseDownEarly(MouseEvent const& e)
{
    auto* source = e.originalComponent;

    // Paste slot: click inside the frame = paste now; click outside = cancel it.
    if (mcpImageSlotActive && source == &canvas) {
        auto const pos = e.getEventRelativeTo(&canvas).getPosition();
        juce::Rectangle<int> const slot(mcpImageSlotPos.x - 160, mcpImageSlotPos.y - 110, 320, 220);
        if (slot.contains(pos)) {
            pasteImageFromClipboard();
        } else {
            mcpImageSlotActive = false;
            canvas.repaint();
            if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
        }
        return true;
    }

    // The pill/chisel and its chips float ABOVE everything: their clicks must beat
    // the pen, the reference images and the lasso. Without this, an armed pen
    // swallows [↩ Undo] / [🗑 Clear] clicks as new strokes.
    if (source == &canvas && !e.mods.isRightButtonDown() && handleNoteClick(e.getPosition())) return true;

    // Voice take chips float above the glass: ▶ audition / ✕ / drag.
    if (source == &canvas && !e.mods.isRightButtonDown() && handleVoiceTakeClick(e, e.getEventRelativeTo(&canvas).getPosition())) return true;
    if (source == &canvas && !e.mods.isRightButtonDown() && handleMidiTakeClick(e, e.getEventRelativeTo(&canvas).getPosition())) return true;

    // PRD Phase 2: reference images on the canvas glass — drag to move, right-click
    // for the menu. In edit mode an armed pen draws over them; in Studio mode they
    // stay grabbable (Alt forces drawing, even there).
    bool const forceDraw = e.mods.isAltDown();
    bool const penBlocksImages = !mcpStudioMode && (forceDraw || mcpSketchToolActive || mcpEraserToolActive);
    if (source == &canvas && !penBlocksImages) {
        if (handleReferenceImageClick(e, e.getEventRelativeTo(&canvas).getPosition(), e.mods.isRightButtonDown())) return true;
    }

    return false;
}

bool CanvasMcpLayer::mouseDownLeft(MouseEvent const& e)
{
    auto* source = e.originalComponent;

        // Type tool (T): click the glass to drop a console-tape note at the cursor.
        if (mcpTextToolActive && source == &canvas) {
            createTextNoteAt(e.getEventRelativeTo(&canvas).getPosition());
            return true;
        }

        // Multimodal Pen / Sketch overlay: Alt-drag, active pen, or eraser tool
        if (shouldShowAISketch() && (e.mods.isAltDown() || mcpSketchToolActive || mcpEraserToolActive)) {
            // Two lanes: with no selection, the ink is moodboard material (the
            // chisel takes over the glass). With a selection, the ink is a
            // surgical annotation — the selection IS kept and the pill stays.
            isSketching = true;
            mcpEraseGesture = mcpEraserToolActive && !e.mods.isAltDown();
            mcpSketchAlpha = 1.0f;
            if (mcpSketchFadeTimer) mcpSketchFadeTimer->stopTimer();
            mcpCurrentStroke.clear();
            if (!mcpEraseGesture) {
                auto const pt = e.getPosition();
                mcpCurrentStroke.push_back({static_cast<float>(pt.x), static_cast<float>(pt.y), juce::Time::getMillisecondCounterHiRes()});
            }
            canvas.repaint();
            if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
            return true;
        }

    return false;
}

bool CanvasMcpLayer::mouseDownOnGlass(MouseEvent const& e)
{
            // Empty glass click: let native PlugData handle deselect and lasso cleanly.
            // The Mood Strip only appears when Studio Mode is explicitly active.
            mcpMoodPillActive = false;
            mcpPillRequested = false;
            hideSelectionPill();

            // Pill / note clicks were already handled above (before the pen branch).
            if (false && canvas.pd && !canvas.pd->getMcp()->session.getMcpAnnotations().empty()) {
                auto const pt = e.getPosition().toFloat();
                auto anns = canvas.pd->getMcp()->session.getMcpAnnotations();
                for (int i = static_cast<int>(anns.size()) - 1; i >= 0; --i) {
                    auto const& a = anns[static_cast<size_t>(i)];
                    float const tx = canvas.canvasOrigin.x + a.x - 6.0f;
                    float const ty = canvas.canvasOrigin.y + a.y - 9.0f;
                    float const tw = a.text.length() * 7.0f + 30.0f; // estimated hit width
                    if (Rectangle<float>(tx, ty, tw, 18.0f).contains(pt)) {
                        // Click a note to reply; notes auto-fade, so there is no dismiss
                        // gesture (no "x" to fight with).
                        {
                            // PRD overlay: click a note -> reply inline (becomes your message to the AI)
                            if (!mcpNoteEditor) {
                                mcpNoteEditor = std::make_unique<juce::TextEditor>();
                                mcpNoteEditor->setMultiLine(false);
                                mcpNoteEditor->setReturnKeyStartsNewLine(false);
                                mcpNoteEditor->setWantsKeyboardFocus(true);
                                // Commit: write the text back onto the note and forward it to the AI.
                                auto commit = [this](bool send) {
                                    if (mcpNoteEditIndex < 0) return;
                                    juce::String txt = mcpNoteEditor ? mcpNoteEditor->getText().trim() : juce::String();
                                    juce::String targetId;
                                    if (canvas.pd) {
                                        juce::ScopedLock sl(canvas.pd->getMcp()->session.mcpOverlayLock);
                                        if (mcpNoteEditIndex < static_cast<int>(canvas.pd->getMcp()->session.mcpAnnotations.size())) {
                                            auto& ann = canvas.pd->getMcp()->session.mcpAnnotations[static_cast<size_t>(mcpNoteEditIndex)];
                                            ann.text = txt;
                                            ann.kind = "artist"; // it's the artist's note now
                                            targetId = ann.targetId;
                                        }
                                    }
                                    mcpNoteEditIndex = -1;
                                    if (mcpNoteEditor) mcpNoteEditor->setVisible(false);
                                    if (send && txt.isNotEmpty() && canvas.pd) {
                                        if (auto* br = canvas.pd->getMcp()->getMCPBridge()) br->sendArtistNote(targetId, txt);
                                    }
                                    canvas.repaint();
                                };
                                mcpNoteEditor->onReturnKey = [commit] { commit(true); };
                                // Never lose typing: also commit when focus leaves the field —
                                // but ignore the focus-loss that fires right after opening
                                // (the field doesn't have focus yet, so it would self-close).
                                mcpNoteEditor->onFocusLost = [this, commit] {
                                    if (juce::Time::getMillisecondCounter() - mcpNoteOpenedAt < 500) return;
                                    commit(true);
                                };
                                // Live feedback: write the field's text onto the NOTE as you type
                                // (the note is GPU-drawn, so it updates reliably every keystroke).
                                mcpNoteEditor->onTextChange = [this] {
                                    if (mcpNoteEditor && canvas.pd && mcpNoteEditIndex >= 0) {
                                        juce::ScopedLock sl(canvas.pd->getMcp()->session.mcpOverlayLock);
                                        if (mcpNoteEditIndex < static_cast<int>(canvas.pd->getMcp()->session.mcpAnnotations.size()))
                                            canvas.pd->getMcp()->session.mcpAnnotations[static_cast<size_t>(mcpNoteEditIndex)].text = mcpNoteEditor->getText();
                                    }
                                    canvas.repaint();
                                    if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
                                };
                                mcpNoteEditor->onEscapeKey = [this] {
                                    mcpNoteEditIndex = -1;
                                    if (mcpNoteEditor) mcpNoteEditor->setVisible(false);
                                };
                                mcpNoteEditor->setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff16161c));
                                mcpNoteEditor->setColour(juce::TextEditor::textColourId, juce::Colours::white);
                                mcpNoteEditor->setColour(juce::TextEditor::outlineColourId, juce::Colour(0xff4a9eff));
                                mcpNoteEditor->setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour(0xff4a9eff));
                                mcpNoteEditor->setColour(juce::TextEditor::highlightColourId, juce::Colour(0xff4a9eff));
                                mcpNoteEditor->setColour(juce::CaretComponent::caretColourId, juce::Colours::white);
                                canvas.addAndMakeVisible(*mcpNoteEditor);
                            }
                            mcpNoteEditIndex = i;
                            mcpNoteOpenedAt = juce::Time::getMillisecondCounter(); // arm the grace period
                            // Place the editor ABOVE the note so the note (and its "x") stay clickable.
                            mcpNoteEditor->setBounds(juce::roundToInt(canvas.canvasOrigin.x + a.x - 6.0f),
                                                     juce::roundToInt(canvas.canvasOrigin.y + a.y - 34.0f),
                                                     juce::jmax(140, juce::roundToInt(a.text.length() * 7.0f) + 40), 20);
                            mcpNoteEditor->setText(a.text, false);
                            mcpNoteEditor->setVisible(true);
                            mcpNoteEditor->toFront(true);
                            // Grab focus AFTER the click finishes — grabbing during mouseDown
                            // gets overridden by the canvas, so typed keys would go nowhere.
                            {
                                auto* ed = mcpNoteEditor.get();
                                juce::MessageManager::callAsync([ed] {
                                    if (ed) { ed->grabKeyboardFocus(); ed->selectAll(); }
                                });
                            }
                            canvas.repaint();
                            return true;
                        }
                    }
                }
            }
    return false;
}

bool CanvasMcpLayer::mouseDownRight(MouseEvent const& e)
{
        if (shouldShowAISketch() && (mcpSketchToolActive || mcpEraserToolActive)) {
            // Right-drag eraser — selection stays (surgical lane).
            isSketching = true;
            mcpEraseGesture = true; // right-drag always erases
            mcpSketchAlpha = 1.0f;
            if (mcpSketchFadeTimer) mcpSketchFadeTimer->stopTimer();
            mcpCurrentStroke.clear();
            canvas.repaint();
            if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
            return true;
        }
    return false;
}

bool CanvasMcpLayer::mouseDrag(MouseEvent const& e)
{
    // Voice take chip drag.
    if (mcpDraggingTakeId.isNotEmpty()) {
        auto const pt = e.getEventRelativeTo(&canvas).getPosition();
        auto const pdPos = pt - mcpTakeDragOffset - canvas.canvasOrigin;
        if (canvas.pd) canvas.pd->getMcp()->session.setMcpVoiceTakePos(mcpDraggingTakeId, static_cast<float>(pdPos.x), static_cast<float>(pdPos.y));
        canvas.repaint();
        if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
        return true;
    }

    // MIDI take chip drag.
    if (mcpDraggingMidiTakeId.isNotEmpty()) {
        auto const pt = e.getEventRelativeTo(&canvas).getPosition();
        auto const pdPos = pt - mcpMidiTakeDragOffset - canvas.canvasOrigin;
        if (canvas.pd) canvas.pd->getMcp()->session.setMcpMidiTakePos(mcpDraggingMidiTakeId, static_cast<float>(pdPos.x), static_cast<float>(pdPos.y));
        canvas.repaint();
        if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
        return true;
    }

    // PRD Phase 2: dragging a reference image across the canvas glass.
    if (mcpDraggingRefImageId.isNotEmpty()) {
        auto const pt = e.getEventRelativeTo(&canvas).getPosition();
        auto const pdPos = pt - mcpRefImageDragOffset - canvas.canvasOrigin;
        if (canvas.pd) canvas.pd->getMcp()->session.moveMcpReferenceImage(mcpDraggingRefImageId, static_cast<float>(pdPos.x), static_cast<float>(pdPos.y));
        canvas.repaint();
        if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
        return true;
    }

    if (isSketching) {
        auto const pt = e.getPosition();

        // ERASER MODE: dedicated eraser tool, Ctrl-drag, or Right-drag hit-tests strokes
        if (mcpEraseGesture || e.mods.isCtrlDown() || e.mods.isRightButtonDown()) {
            float eraseRadius = 25.0f;
            bool erasedAny = false;
            for (auto it = mcpCompletedStrokes.begin(); it != mcpCompletedStrokes.end(); ) {
                bool hit = false;
                for (auto const& p : *it) {
                    float dx = p.x - pt.x;
                    float dy = p.y - pt.y;
                    if (dx*dx + dy*dy < eraseRadius*eraseRadius) {
                        hit = true;
                        break;
                    }
                }
                if (hit) {
                    it = mcpCompletedStrokes.erase(it);
                    erasedAny = true;
                } else {
                    ++it;
                }
            }
            if (erasedAny) {
                updateSelectionPill();
                canvas.repaint();
                if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
            }
            return true;
        }

        mcpCurrentStroke.push_back({static_cast<float>(pt.x), static_cast<float>(pt.y), juce::Time::getMillisecondCounterHiRes()});
        canvas.repaint();
        if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
        return true;
    }

    return false;
}

bool CanvasMcpLayer::mouseUp(MouseEvent const& e)
{
    // Finish a voice take chip drag.
    if (mcpDraggingTakeId.isNotEmpty()) {
        mcpDraggingTakeId.clear();
        canvas.setMouseCursor(juce::MouseCursor::NormalCursor);
        canvas.repaint();
        if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
        return true;
    }

    // Finish a MIDI take chip drag.
    if (mcpDraggingMidiTakeId.isNotEmpty()) {
        mcpDraggingMidiTakeId.clear();
        canvas.setMouseCursor(juce::MouseCursor::NormalCursor);
        canvas.repaint();
        if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
        return true;
    }

    // PRD Phase 2: finish a reference-image drag.
    if (mcpDraggingRefImageId.isNotEmpty()) {
        mcpDraggingRefImageId.clear();
        canvas.setMouseCursor(juce::MouseCursor::NormalCursor);
        canvas.repaint();
        if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
        return true;
    }

    if (isSketching) {
        isSketching = false;
        mcpEraseGesture = false;
        if (mcpCurrentStroke.size() >= 2) {
            mcpCompletedStrokes.push_back(mcpCurrentStroke);
            juce::Array<juce::var> ptsVar;
            for (auto const& p : mcpCurrentStroke) {
                auto* obj = new juce::DynamicObject();
                obj->setProperty("x", p.x - static_cast<float>(canvas.canvasOrigin.x));
                obj->setProperty("y", p.y - static_cast<float>(canvas.canvasOrigin.y));
                obj->setProperty("t", p.t);
                ptsVar.add(juce::var(obj));
            }
            auto jsonStr = juce::JSON::toString(juce::var(ptsVar), true);
            bool const isFastPath = e.mods.isShiftDown();
            if (canvas.pd) {
                if (auto* br = canvas.pd->getMcp()->getMCPBridge()) {
                    auto const subpatch = canvas.patch.getPointer() ? juce::String(reinterpret_cast<uintptr_t>(canvas.patch.getPointer().get())) : juce::String("0");
                    br->sendSketchStroke(subpatch, jsonStr, isFastPath);
                }
            }
            mcpCurrentStroke.clear();
            // startSketchFadeTimer(); // PERSISTENT INK
        } else {
            mcpCurrentStroke.clear();
            // Empty canvas tap: zero popups, keep canvas 100% clean
            mcpMoodPillActive = false;
        }
        // Ink persists -> anchor the Moodboard Chisel under the fresh bbox.
        updateSelectionPill();
        canvas.repaint();
        if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
        return true;
    }

    return false;
}

void CanvasMcpLayer::objectCreatedByDoubleClick()
{
    // IMMUNITY: Kill any open mood strip and suppress AI pills during object creation
    mcpMoodPillActive = false;
    mcpPillRequested = false;
    hideSelectionPill();
    mcpSelectionPillHideUntil = juce::Time::getMillisecondCounter() + 1500;
}

std::optional<bool> CanvasMcpLayer::keyPressedEarly(KeyPress const& key)
{
    // Paste-slot fast path: Ctrl/Cmd+V pastes the clipboard image directly,
    // independent of the global Paste command and its focus rules.
    if (mcpImageSlotActive) {
        auto const kc = key.getKeyCode();
        bool const ctrlV = (key.getModifiers().isCtrlDown() || key.getModifiers().isCommandDown())
            && (kc == 'v' || kc == 'V' || kc == 22 /* Ctrl+V control code */ || key.getTextCharacter() == 'v');
        if (ctrlV) {
            pasteImageFromClipboard();
            return true;
        }
    }

    if (mcpSelectionPill && mcpSelectionPill->hasKeyboardFocus(true))
        return false;

    // Tier-2 approval: Enter approves the pending destructive op, Esc cancels.
    if (isApprovalRequested()) {
        if (key == juce::KeyPress::returnKey) {
            sendApproval(true);
            return true;
        }
        if (key == juce::KeyPress::escapeKey) {
            sendApproval(false);
            return true;
        }
    }

    return std::nullopt;
}

bool CanvasMcpLayer::keyPressedForCanvas(KeyPress const& key)
{
    // Paste slot: Esc cancels the waiting frame.
    if (mcpImageSlotActive && key.getKeyCode() == juce::KeyPress::escapeKey) {
        mcpImageSlotActive = false;
        canvas.repaint();
        if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
        return true;
    }

    // Moodboard tools escape hatch: Esc always disarms the tool first, so the
    // artist is never trapped drawing/typing when the chisel replaced the pill.
    if ((mcpSketchToolActive || mcpEraserToolActive || mcpTextToolActive) && key.getKeyCode() == juce::KeyPress::escapeKey) {
        setSketchToolActive(false);
        setEraserToolActive(false);
        setTextToolActive(false);
        return true;
    }

    // Moodboard hotkeys (Lane 1): T arms the type tool, Tab flips Pen <-> Eraser,
    // Z / Ctrl+Z pops the last ink stroke. Esc exits (handled above).
    // Only hijack hotkeys if the pen/eraser is ACTIVELY in hand or Studio mode is explicitly ON.
    // In regular patching, 't' must always create a native Toggle box!
    bool const moodboardArmed = mcpSketchToolActive || mcpEraserToolActive || mcpTextToolActive || mcpStudioMode;
    if (moodboardArmed) {
        if (key.getKeyCode() == 't' || key.getKeyCode() == 'T') {
            setTextToolActive(!mcpTextToolActive);
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::tabKey) {
            if (mcpSketchToolActive) setEraserToolActive(true);
            else setSketchToolActive(true);
            return true;
        }
        if (key.getKeyCode() == 'z' || key.getKeyCode() == 'Z') {
            if (!mcpCompletedStrokes.empty()) {
                undoLastStroke();
                return true;
            }
        }
    }

    // Studio brainstorm: Enter commits from the keyboard — no mouse travel to the
    // sidebar. Shift+Enter PARKS the intent in the bay without waking the AI.
    if (mcpStudioMode && key.getKeyCode() == juce::KeyPress::returnKey
        && (key.getModifiers().isCtrlDown() || key.getModifiers().isShiftDown() || key.getModifiers().isCommandDown())) {
        bool const cmdCombo = key.getModifiers().isCtrlDown() || key.getModifiers().isCommandDown();
        if (key.getModifiers().isShiftDown() && !cmdCombo) {
            parkSelectionPill(); // Shift = park (no AI)
        } else {
            commitStudioFromCanvas(); // Ctrl/Cmd+Enter = send
        }
        return true;
    }

    // Summon the AI pill prompt on selected object(s) when Spacebar (Leertaste) or "/" is pressed:
    if (key.getKeyCode() == juce::KeyPress::spaceKey || key.getTextCharacter() == '/' || key.getKeyCode() == '/') {
        if (!canvas.getSelectionOfType<Object>().empty() && !ObjectBase::isBeingEdited()) {
            mcpPillRequested = true;
            mcpPillDrawerMode = 0;
            mcpPaletteFrame = {};
            updateSelectionPill();
            if (mcpSelectionPill) {
                mcpSelectionPill->setText(juce::String(), false);
                mcpSelectionPill->grabKeyboardFocus();
                // If triggered by slash, open the command/skills palette directly
                if (key.getTextCharacter() == '/' || key.getKeyCode() == '/') {
                    showPillToolsMenu();
                }
            }
            return true;
        }
    }

    // Fast-focus / hotkey handling when the selection pill is visible:
    if (mcpSelectionPill && mcpSelectionPill->isVisible()) {
        if (key.getKeyCode() == juce::KeyPress::tabKey || key.getKeyCode() == juce::KeyPress::returnKey) {
            // Mood strip has no prompt field — focus keys do nothing there.
            if (mcpPillSketchMode && mcpCompletedStrokes.empty()) return true;
            mcpSelectionPill->grabKeyboardFocus();
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::escapeKey) {
            mcpPillRequested = false;
            dismissSelectionPill();
            return true;
        }
    }
    return false;
}

void CanvasMcpLayer::canvasDestroyed()
{
    // Drop this canvas's MCP undo/redo transactions. Without this the stacks
    // leak (<=50 full forward+inverse messages each) and, worse, a later canvas
    // could reuse the freed t_canvas* address and inherit a stale stack.
    if (!canvas.isGraph)
        canvas.pd->getMcp()->clearMcpTransactions(canvas.patch.getUncheckedPointer());
}

void CanvasMcpLayer::canvasSynchronised()
{
    for (auto* c : canvas.connections) {
        c->updatePath();
    }
}

bool CanvasMcpLayer::undo()
{
    // 1. If an AI transaction exists, undo that first (safe transactional rollback with semantic IDs)
    if (canvas.pd && canvas.pd->getMcp()->hasMcpTransaction(canvas.patch.getUncheckedPointer())) {
        canvas.pd->getMcp()->undoMcpTransaction(canvas.patch.getUncheckedPointer());
        return true;
    }

    // 2. Otherwise, if user has manual GUI edits on Pure Data's native stack, undo that
    if (canvas.patch.canUndo()) {
        canvas.patch.undo();
        return true;
    }

    // Nothing to undo
    return false;
}

bool CanvasMcpLayer::redo()
{
    // 1. If an AI transaction was undone, redo that first to recreate objects before redoing manual edits on them
    if (canvas.pd && canvas.pd->getMcp()->hasMcpRedoTransaction(canvas.patch.getUncheckedPointer())) {
        canvas.pd->getMcp()->redoMcpTransaction(canvas.patch.getUncheckedPointer());
        return true;
    }

    // 2. Otherwise, if user has manual GUI edits to redo, tell pd to redo
    if (canvas.patch.canRedo()) {
        canvas.patch.redo();
        return true;
    }
    return false;
}

bool CanvasMcpLayer::canUndo()
{
    if (canvas.pd && canvas.pd->getMcp()->hasMcpTransaction(canvas.patch.getUncheckedPointer())) return true;
    return canvas.patch.canUndo();
}

// Object::mouseDown/mouseUp/mouseDrag (moved from Source/Object.cpp, `cnv` is `canvas`):
// AI notes over an object take its click, and an armed pen draws over objects.
bool CanvasMcpLayer::objectMouseDown(MouseEvent const& e)
{
    // PRD overlay: if an AI note is drawn over this object, the note wins the click
    // (x = dismiss, body = edit). Forward the click to the canvas note handler.
    if (!e.mods.isRightButtonDown() && handleNoteClick(e.getEventRelativeTo(&canvas).getPosition()))
        return true;

    // Multimodal Pen / Sketch overlay: Alt-drag or active sketch tool draws on canvas
    if (!e.mods.isRightButtonDown() && shouldShowAISketch() && (e.mods.isAltDown() || isSketchToolActive())) {
        canvas.mouseDown(e.getEventRelativeTo(&canvas));
        return true;
    }
    return false;
}

bool CanvasMcpLayer::objectMouseUp(MouseEvent const& e, bool const wasLockedOnMouseDown)
{
    if (shouldShowAISketch() && (isSketchingActive() || (isSketchToolActive() && !wasLockedOnMouseDown))) {
        canvas.mouseUp(e.getEventRelativeTo(&canvas));
        return true;
    }
    return false;
}

bool CanvasMcpLayer::objectMouseDrag(MouseEvent const& e)
{
    if (shouldShowAISketch() && (e.mods.isAltDown() || isSketchToolActive() || isSketchingActive())) {
        canvas.mouseDrag(e.getEventRelativeTo(&canvas));
        return true;
    }
    return false;
}

// Dialogs::showCanvasRightClickMenu (moved from Source/Dialogs/Dialogs.cpp): the "Ask AI"
// entry opens the selection pill. Its id is the layer's own, so the dialog's enum stays upstream.
void CanvasMcpLayer::addContextMenuItems(PopupMenu& popupMenu, bool const hasSelection)
{
    if (hasSelection) {
        popupMenu.addItem(askAIMenuId, "Ask AI... (/)", true);
        popupMenu.addSeparator();
    }
}

bool CanvasMcpLayer::handleContextMenuResult(int const result)
{
    if (result == askAIMenuId) {
        mcpPillRequested = true;
        updateSelectionPill();
        if (mcpSelectionPill) {
            mcpSelectionPill->grabKeyboardFocus();
        }
        return true;
    }
    return false;
}

bool CanvasMcpLayer::canRedo()
{
    if (canvas.pd && canvas.pd->getMcp()->hasMcpRedoTransaction(canvas.patch.getUncheckedPointer())) return true;
    return canvas.patch.canRedo();
}
