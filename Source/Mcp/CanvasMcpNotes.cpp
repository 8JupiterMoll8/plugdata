/*
 // The MCP layer of one Canvas (Plan 3 P3.2 S8), part Notes: AI note geometry (live bounds, screen bounds), hover (mouseMove/mouseExit), hit test and click handling.
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

Object* CanvasMcpLayer::findObjectByStableId(juce::String const& targetId) const
{
    if (targetId.isEmpty() || !canvas.pd) return nullptr;
    for (auto* obj : canvas.objects) {
        if (!obj) continue;
        if (auto* ptr = obj->getPointer()) {
            if (canvas.pd->getMcp()->getStableId(ptr) == targetId)
                return obj;
        }
    }
    return nullptr;
}

bool CanvasMcpLayer::getAnnotationLiveBounds(juce::String const& targetId, juce::StringArray const& targetIds, float fallbackX, float fallbackY, float fallbackLx, float fallbackLy, bool fallbackHasLeader, float& outAx, float& outAy, float& outLx, float& outLy, bool& outHasLeader, juce::Rectangle<int>* outGroupBounds) const
{
    auto const ids = targetIds.size() > 0 ? targetIds : (targetId.isNotEmpty() ? juce::StringArray(targetId) : juce::StringArray());
    if (ids.size() > 0) {
        bool first = true;
        int minX = 0, minY = 0, maxX = 0, maxY = 0;
        for (auto const& tid : ids) {
            if (auto* obj = findObjectByStableId(tid)) {
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
        }
        if (!first) {
            if (outGroupBounds) {
                *outGroupBounds = juce::Rectangle<int>(minX, minY, maxX - minX, maxY - minY);
            }

            // Console Tape: pinned directly above the top edge of the module strip
            outLx = static_cast<float>(minX + 14);
            outLy = static_cast<float>(minY);
            outAx = static_cast<float>(minX);
            outAy = static_cast<float>(minY - 24);
            outHasLeader = true;

            // Stack multiple notes on the same group vertically upwards
            if (canvas.pd) {
                auto const allAnns = canvas.pd->getMcp()->session.getMcpAnnotations();
                int noteOrder = 0;
                for (auto const& prevA : allAnns) {
                    bool isSelf = false;
                    if (prevA.targetIds.size() > 0 && ids.size() > 0) {
                        if (prevA.targetIds == ids) isSelf = true;
                    } else if (prevA.targetId == targetId) {
                        isSelf = true;
                    }
                    if (isSelf) break;
                    // Check if targeting the same objects
                    bool sameTarget = false;
                    auto const prevIds = prevA.targetIds.size() > 0 ? prevA.targetIds : (prevA.targetId.isNotEmpty() ? juce::StringArray(prevA.targetId) : juce::StringArray());
                    for (auto const& ptid : prevIds) {
                        for (auto const& tid : ids) {
                            if (ptid == tid) { sameTarget = true; break; }
                        }
                        if (sameTarget) break;
                    }
                    if (sameTarget) ++noteOrder;
                }
                outAy -= noteOrder * 24.0f;
            }

            return true;
        }
        return false; // All target objects deleted -> auto-prune
    }

    if (outGroupBounds) *outGroupBounds = juce::Rectangle<int>();
    outAx = fallbackX;
    outAy = fallbackY;
    outLx = fallbackLx;
    outLy = fallbackLy;
    outHasLeader = fallbackHasLeader;
    return true;
}

bool CanvasMcpLayer::getAnnotationLiveBounds(juce::String const& targetId, float fallbackX, float fallbackY, float fallbackLx, float fallbackLy, bool fallbackHasLeader, float& outAx, float& outAy, float& outLx, float& outLy, bool& outHasLeader) const
{
    return getAnnotationLiveBounds(targetId, juce::StringArray(), fallbackX, fallbackY, fallbackLx, fallbackLy, fallbackHasLeader, outAx, outAy, outLx, outLy, outHasLeader, nullptr);
}

juce::Rectangle<float> CanvasMcpLayer::getAnnotationScreenBounds(size_t annIndex) const
{
    if (!canvas.pd) return {};
    auto const anns = canvas.pd->getMcp()->session.getMcpAnnotations();
    if (annIndex >= anns.size()) return {};
    auto const& a = anns[annIndex];
    float rawAx = 0.0f, rawAy = 0.0f, rawLx = 0.0f, rawLy = 0.0f;
    bool hasLead = false;
    juce::Rectangle<int> grpBounds;
    if (!getAnnotationLiveBounds(a.targetId, a.targetIds, a.x, a.y, a.leaderX, a.leaderY, a.hasLeader, rawAx, rawAy, rawLx, rawLy, hasLead, &grpBounds))
        return {};

    float const ax = canvas.canvasOrigin.x + rawAx;
    float const ay = canvas.canvasOrigin.y + rawAy;
    constexpr float tapeH = 20.0f;
    float const textW = a.text.length() * 6.8f + 40.0f;
    float const groupW = grpBounds.isEmpty() ? 140.0f : static_cast<float>(grpBounds.getWidth());
    float tapeW = 0.0f;
    if (!grpBounds.isEmpty() && groupW >= 90.0f) {
        tapeW = std::min(groupW, std::max(80.0f, textW));
    } else {
        tapeW = std::max(groupW, std::min(160.0f, textW));
    }
    return juce::Rectangle<float>(ax, ay, tapeW, tapeH);
}

void CanvasMcpLayer::mouseMove(MouseEvent const& e)
{
    // Hover feedback: brighten the note under the cursor & detect chip hovers.
    int hoverIdx = -1;
    bool hoverClose = false;
    int hoverChip = -1;

    if (canvas.pd && !canvas.pd->getMcp()->session.getMcpAnnotations().empty()) {
        auto const pt = e.getPosition().toFloat();
        auto const anns = canvas.pd->getMcp()->session.getMcpAnnotations();
        for (int i = static_cast<int>(anns.size()) - 1; i >= 0; --i) {
            auto const tb = getAnnotationScreenBounds(static_cast<size_t>(i));
            if (tb.isEmpty()) continue;

            auto const chips = CanvasMcpLayer::getAnnotationPivotChips(anns[static_cast<size_t>(i)].text, anns[static_cast<size_t>(i)].kind);
            float totalChipsW = 0.0f;
            for (auto const& c : chips) totalChipsW += CanvasMcpLayer::getChipWidth(c.label) + 4.0f;
            float const drawerW = std::max(tb.getWidth(), totalChipsW + 4.0f);
            constexpr float drawerH = 22.0f;
            juce::Rectangle<float> const combinedBounds(tb.getX(), tb.getY(), drawerW, tb.getHeight() + drawerH + 3.0f);

            if (combinedBounds.contains(pt)) {
                hoverIdx = i;
                hoverClose = (pt.y <= tb.getBottom() && pt.x >= tb.getRight() - 16.0f);

                if (pt.y > tb.getBottom() + 1.0f) {
                    float curChipX = tb.getX() + 4.0f;
                    float const chipY = tb.getY() + tb.getHeight() + 2.5f;
                    float const chipH = drawerH - 5.0f;
                    for (size_t ci = 0; ci < chips.size(); ++ci) {
                        float const cw = CanvasMcpLayer::getChipWidth(chips[ci].label);
                        juce::Rectangle<float> chipBox(curChipX, chipY, cw, chipH);
                        if (chipBox.contains(pt)) {
                            hoverChip = static_cast<int>(ci);
                            break;
                        }
                        curChipX += cw + 4.0f;
                    }
                }
                break;
            }
        }
    }
    if (hoverIdx != mcpNoteHover || hoverClose != mcpNoteHoverClose || hoverChip != mcpNoteHoverChip) {
        mcpNoteHover = hoverIdx;
        mcpNoteHoverClose = hoverClose;
        mcpNoteHoverChip = hoverChip;
        canvas.repaint();
        if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
    }
}

void CanvasMcpLayer::mouseExit(MouseEvent const& e)
{
    if (mcpNoteHover != -1 || mcpNoteHoverClose || mcpNoteHoverChip != -1) {
        mcpNoteHover = -1;
        mcpNoteHoverClose = false;
        mcpNoteHoverChip = -1;
        canvas.repaint();
        if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
    }
}

bool CanvasMcpLayer::isPointOverNote(Point<int> canvasPt) const
{
    if (mcpSelectionPill && mcpSelectionPill->isVisible()) {
        auto const panel = mcpSelectionPillFrame.isEmpty() ? mcpSelectionPill->getBounds() : mcpSelectionPillFrame;
        if (panel.contains(canvasPt)) return true;
        if (mcpPillDrawerMode != 0 && mcpPillDrawerFrame.contains(canvasPt)) return true;
        if (mcpPillDrawerMode == 3 && mcpPaletteFrame.contains(canvasPt)) return true;
    }
    if (!canvas.pd || canvas.pd->getMcp()->session.getMcpAnnotations().empty()) return false;
    auto const pt = canvasPt.toFloat();
    auto const anns = canvas.pd->getMcp()->session.getMcpAnnotations();
    for (size_t i = 0; i < anns.size(); ++i) {
        auto const tb = getAnnotationScreenBounds(i);
        if (tb.isEmpty()) continue;
        if (static_cast<int>(i) == mcpNoteHover) {
            auto const chips = CanvasMcpLayer::getAnnotationPivotChips(anns[i].text, anns[i].kind);
            float totalChipsW = 0.0f;
            for (auto const& c : chips) totalChipsW += CanvasMcpLayer::getChipWidth(c.label) + 4.0f;
            float const drawerW = std::max(tb.getWidth(), totalChipsW + 4.0f);
            juce::Rectangle<float> combined(tb.getX(), tb.getY(), drawerW, tb.getHeight() + 25.0f);
            if (combined.contains(pt)) return true;
        } else if (tb.contains(pt)) {
            return true;
        }
    }
    if (mcpNoteEditor && mcpNoteEditor->isVisible() && mcpNoteEditor->getBounds().contains(canvasPt))
        return true;
    return false;
}

bool CanvasMcpLayer::handleNoteClick(Point<int> canvasPt)
{
    // Tier-2 approval chips take priority — they float above the pill.
    if (!mcpApprovalFrame.isEmpty() && mcpApprovalFrame.contains(canvasPt)) {
        if (mcpApprovalYes.contains(canvasPt)) sendApproval(true);
        else if (mcpApprovalNo.contains(canvasPt)) sendApproval(false);
        return true;
    }

    if (mcpSelectionPill && mcpSelectionPill->isVisible()) {
        // Smart Command Palette clicks
        if (mcpPillDrawerMode == 3 && mcpPaletteFrame.contains(canvasPt)) {
            int const relY = canvasPt.y - (mcpPaletteFrame.getY() + 24);
            int const rowH = 30;
            if (relY >= 0) {
                int const clickedRow = relY / rowH;
                triggerPaletteItem(clickedRow);
                return true;
            }
        }

        // 1. Drawer clicks (Lenses / Tools)
        if (mcpPillDrawerMode != 0 && mcpPillDrawerFrame.contains(canvasPt)) {
            int const relX = canvasPt.x - mcpPillDrawerFrame.getX();
            int const dw = mcpPillDrawerFrame.getWidth();
            if (mcpPillDrawerMode == 1) {
                // Lenses: Free | Signal | Logic | Order
                float const itemW = (static_cast<float>(dw) - 8.0f) / 4.0f;
                int idx = static_cast<int>((relX - 4) / itemW);
                if (idx >= 0 && idx < 4) {
                    const char* lenses[] = { "", "signal", "logic", "order" };
                    juce::String lens(lenses[idx]);
                    mcpActiveLens = lens;
                    if (canvas.pd) {
                        if (auto* br = canvas.pd->getMcp()->getMCPBridge()) br->sendLens(lens.isEmpty() ? "free" : lens);
                    }
                    mcpPillDrawerMode = 0;
                    mcpPillDrawerFrame = {};
                    canvas.repaint();
                    if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
                    return true;
                }
            } else if (mcpPillDrawerMode == 2) {
                // Tools: Explain | Drive | Filter | Reverb | Pack GOP | Tidy
                const char* toolPrompts[] = {
                    "explain this",
                    "add tanh~ saturation",
                    "add lowpass filter",
                    "add reverb space",
                    "pack this into gop module",
                    "tidy layout"
                };
                float const itemW = (static_cast<float>(dw) - 8.0f) / 6.0f;
                int idx = static_cast<int>((relX - 4) / itemW);
                if (idx >= 0 && idx < 6) {
                    mcpPillDrawerMode = 0;
                    mcpPillDrawerFrame = {};
                    sendPillPrompt(toolPrompts[idx]);
                    dismissSelectionPill();
                    return true;
                }
            }
            return true;
        }

        // 2. Pill clicks
        auto const panel = mcpSelectionPillFrame.isEmpty() ? mcpSelectionPill->getBounds() : mcpSelectionPillFrame;
        if (panel.contains(canvasPt)) {
            int const relX = canvasPt.x - panel.getX();

            // Mood strip: [✏] [T] [🖼] [🎙 Melody] [🥁 Beat] [🎹 MIDI] — icons first, surfaces second.
            if (mcpPillSketchMode && mcpCompletedStrokes.empty()) {
                if (relX >= 6 && relX < 210) {
                    int const idx = (relX - 6) / 34;
                    switch (idx) {
                    case 0:
                        setSketchToolActive(!isSketchToolActive());
                        canvas.repaint();
                        if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
                        return true;
                    case 1:
                        createTextNoteAt(mcpMoodPillActive ? mcpMoodPillPos : juce::Point<int>(panel.getCentreX(), panel.getBottom()));
                        return true;
                    case 2:
                        // Open (or cancel) a paste slot — Ctrl+V fills it. Nothing
                        // is read from the clipboard automatically.
                        mcpImageSlotActive = !mcpImageSlotActive;
                        mcpImageSlotPos = mcpMoodPillActive
                            ? mcpMoodPillPos
                            : juce::Point<int>(panel.getCentreX(), panel.getBottom() + 130);
                        // Pull keyboard focus to the canvas so Ctrl+V can never be
                        // swallowed by a lingering tape-note editor.
                        canvas.grabKeyboardFocus();
                        canvas.repaint();
                        if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
                        return true;
                    case 3:
                        // Melody record: declared intent — sing (no guessing).
                        if (canvas.pd) {
                            if (auto* br = canvas.pd->getMcp()->getMCPBridge()) {
                                if (br->isVoiceCapturing()) {
                                    br->stopVoiceCaptureAndAnalyze();
                                } else {
                                    setSketchToolActive(false);
                                    setEraserToolActive(false);
                                    setTextToolActive(false);
                                    mcpTakeSpawnPos = mcpMoodPillActive ? mcpMoodPillPos : juce::Point<int>(panel.getCentreX(), panel.getBottom() + 170);
                                    br->startVoiceCapture(60, 1);
                                }
                                canvas.repaint();
                                if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
                            }
                        }
                        return true;
                    case 4:
                        // Beat record: declared intent — beatbox (no guessing).
                        if (canvas.pd) {
                            if (auto* br = canvas.pd->getMcp()->getMCPBridge()) {
                                if (br->isVoiceCapturing()) {
                                    br->stopVoiceCaptureAndAnalyze();
                                } else {
                                    setSketchToolActive(false);
                                    setEraserToolActive(false);
                                    setTextToolActive(false);
                                    mcpTakeSpawnPos = mcpMoodPillActive ? mcpMoodPillPos : juce::Point<int>(panel.getCentreX(), panel.getBottom() + 170);
                                    br->startVoiceCapture(60, 2);
                                }
                                canvas.repaint();
                                if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
                            }
                        }
                        return true;
                    case 5:
                        // MIDI record: declared intent — MIDI keyboard (exact notes).
                        if (canvas.pd) {
                            if (auto* br = canvas.pd->getMcp()->getMCPBridge()) {
                                if (br->isMidiCapturing()) {
                                    br->stopMidiCaptureAndStage();
                                } else {
                                    setSketchToolActive(false);
                                    setEraserToolActive(false);
                                    setTextToolActive(false);
                                    mcpTakeSpawnPos = mcpMoodPillActive ? mcpMoodPillPos : juce::Point<int>(panel.getCentreX(), panel.getBottom() + 170);
                                    br->startMidiCapture(120, "canvas");
                                }
                                canvas.repaint();
                                if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
                            }
                        }
                        return true;
                    }
                }
                return true; // the strip frame swallows clicks
            }

            // Sketch / Moodboard Chisel: [⚡ Build] | prompt | [T] [🖼] [↩ Undo] [🗑 Clear] [✏️ pen]
            if (mcpPillSketchMode) {
                if (relX <= 88) {
                    sendSketchBuild(mcpSelectionPill ? mcpSelectionPill->getText().trim() : juce::String());
                    return true;
                }
                if (relX >= panel.getWidth() - 238 && relX < panel.getWidth() - 210) {
                    setTextToolActive(!isTextToolActive());
                    canvas.repaint();
                    if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
                    return true;
                }
                if (relX >= panel.getWidth() - 210 && relX < panel.getWidth() - 182) {
                    pasteImageFromClipboard();
                    return true;
                }
                if (relX >= panel.getWidth() - 182 && relX < panel.getWidth() - 106) {
                    undoLastStroke();
                    return true;
                }
                if (relX >= panel.getWidth() - 106 && relX < panel.getWidth() - 32) {
                    clearAllInk();
                    return true;
                }
                if (relX >= panel.getWidth() - 32) {
                    // Pen toggle: put it down / pick it back up mid-sketch.
                    setSketchToolActive(!isSketchToolActive());
                    return true;
                }
                mcpSelectionPill->grabKeyboardFocus();
                return true;
            }



            // Right ~32px: ↵ Submit button
            if (relX >= panel.getWidth() - 32) {
                submitSelectionPill(false);
                return true;
            }
            // Middle: Text field focus!
            mcpSelectionPill->grabKeyboardFocus();
            return true;
        }
    }

    if (!canvas.pd || canvas.pd->getMcp()->session.getMcpAnnotations().empty()) return false;
    auto const pt = canvasPt.toFloat();
    auto anns = canvas.pd->getMcp()->session.getMcpAnnotations();
    for (int i = static_cast<int>(anns.size()) - 1; i >= 0; --i) {
        auto const& a = anns[static_cast<size_t>(i)];
        auto const tb = getAnnotationScreenBounds(static_cast<size_t>(i));
        if (tb.isEmpty()) continue;

        auto const chips = CanvasMcpLayer::getAnnotationPivotChips(a.text, a.kind);
        float totalChipsW = 0.0f;
        for (auto const& c : chips) totalChipsW += CanvasMcpLayer::getChipWidth(c.label) + 4.0f;
        float const drawerW = std::max(tb.getWidth(), totalChipsW + 4.0f);
        constexpr float drawerH = 22.0f;
        juce::Rectangle<float> const combinedBounds(tb.getX(), tb.getY(), drawerW, tb.getHeight() + drawerH + 3.0f);

        if (!combinedBounds.contains(pt)) continue;

        // 1. Rightmost ~16px of the tape header is the dismiss "x"
        juce::Rectangle<float> closeZone(tb.getRight() - 16.0f, tb.getY(), 16.0f, tb.getHeight());
        if (closeZone.contains(pt)) {
            if (mcpNoteEditor) mcpNoteEditor->setVisible(false);
            mcpNoteEditIndex = -1;
            juce::ScopedLock sl(canvas.pd->getMcp()->session.mcpOverlayLock);
            if (i < static_cast<int>(canvas.pd->getMcp()->session.mcpAnnotations.size()))
                canvas.pd->getMcp()->session.mcpAnnotations.erase(canvas.pd->getMcp()->session.mcpAnnotations.begin() + i);
            canvas.repaint();
            if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
            return true;
        }

        // 2. Click inside the Pivot Chips drawer (pt.y > tb.getBottom())
        if (pt.y > tb.getBottom() + 1.0f) {
            float curChipX = tb.getX() + 4.0f;
            float const chipY = tb.getY() + tb.getHeight() + 2.5f;
            float const chipH = drawerH - 5.0f;
            for (size_t ci = 0; ci < chips.size(); ++ci) {
                float const cw = CanvasMcpLayer::getChipWidth(chips[ci].label);
                juce::Rectangle<float> chipBox(curChipX, chipY, cw, chipH);
                if (chipBox.contains(pt)) {
                    auto const& chip = chips[ci];
                    if (chip.prompt == "__undo__") {
                        undo();
                        mcpNoteHover = -1;
                        mcpNoteHoverChip = -1;
                        canvas.repaint();
                        if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
                        return true;
                    }
                    if (chip.prompt == "__reply__") {
                        // Open direct inline reply editor right below the tape!
                        // Select target objects first so context is wired
                        if (a.targetIds.size() > 0 || a.targetId.isNotEmpty()) {
                            canvas.deselectAll();
                            auto const ids = a.targetIds.size() > 0 ? a.targetIds : juce::StringArray(a.targetId);
                            for (auto const& tid : ids) {
                                if (auto* obj = findObjectByStableId(tid)) canvas.setSelected(obj, true);
                            }
                        }
                        if (!mcpNoteEditor) {
                            mcpNoteEditor = std::make_unique<juce::TextEditor>();
                            mcpNoteEditor->setMultiLine(false);
                            mcpNoteEditor->setReturnKeyStartsNewLine(false);
                            mcpNoteEditor->setWantsKeyboardFocus(true);
                            mcpNoteEditor->setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff16161c));
                            mcpNoteEditor->setColour(juce::TextEditor::textColourId, juce::Colours::white);
                            mcpNoteEditor->setColour(juce::TextEditor::outlineColourId, juce::Colour(0xff4a9eff));
                            mcpNoteEditor->setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour(0xff4a9eff));
                            mcpNoteEditor->setColour(juce::TextEditor::highlightColourId, juce::Colour(0xff4a9eff));
                            mcpNoteEditor->setColour(juce::CaretComponent::caretColourId, juce::Colours::white);
                            canvas.addAndMakeVisible(*mcpNoteEditor);
                        }
                        mcpNoteEditIndex = i;
                        mcpNoteOpenedAt = juce::Time::getMillisecondCounter();
                        mcpNoteEditor->onReturnKey = [this, targetId = a.targetId, targetIds = a.targetIds] {
                            if (mcpNoteEditor && canvas.pd) {
                                juce::String const replyTxt = mcpNoteEditor->getText().trim();
                                if (replyTxt.isNotEmpty()) {
                                    if (targetIds.size() > 0 || targetId.isNotEmpty()) {
                                        canvas.deselectAll();
                                        auto const ids = targetIds.size() > 0 ? targetIds : juce::StringArray(targetId);
                                        for (auto const& tid : ids) {
                                            if (auto* obj = findObjectByStableId(tid)) canvas.setSelected(obj, true);
                                        }
                                    }
                                    sendPillPrompt(replyTxt, false);
                                    if (canvas.editor) {
                                        if (auto* cp = canvas.editor->getCopilotPanel())
                                            cp->receiveMessage("user", replyTxt);
                                    }
                                    if (auto* br = canvas.pd->getMcp()->getMCPBridge())
                                        br->sendArtistNote(targetId, replyTxt);
                                }
                            }
                            mcpNoteEditIndex = -1;
                            if (mcpNoteEditor) mcpNoteEditor->setVisible(false);
                            canvas.repaint();
                            if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
                        };
                        mcpNoteEditor->onEscapeKey = [this] {
                            mcpNoteEditIndex = -1;
                            if (mcpNoteEditor) mcpNoteEditor->setVisible(false);
                            canvas.repaint();
                            if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
                        };
                        mcpNoteEditor->onFocusLost = [this] {
                            if (juce::Time::getMillisecondCounter() - mcpNoteOpenedAt < 500) return;
                            mcpNoteEditIndex = -1;
                            if (mcpNoteEditor) mcpNoteEditor->setVisible(false);
                            canvas.repaint();
                            if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
                        };
                        mcpNoteEditor->setBounds(juce::roundToInt(tb.getX()),
                                                 juce::roundToInt(tb.getY() + tb.getHeight() + 2.0f),
                                                 juce::roundToInt(std::max(tb.getWidth(), 240.0f)), 22);
                        mcpNoteEditor->setText(juce::String(), false);
                        mcpNoteEditor->setTextToShowWhenEmpty("Reply to Copilot on this module... (Enter sends)", juce::Colour(0xff7088a0));
                        mcpNoteEditor->setVisible(true);
                        mcpNoteEditor->toFront(true);
                        {
                            auto* ed = mcpNoteEditor.get();
                            juce::MessageManager::callAsync([ed] { if (ed) ed->grabKeyboardFocus(); });
                        }
                        canvas.repaint();
                        if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
                        return true;
                    }

                    // Sound pivot chip clicked:
                    // 1. Select the module objects
                    if (a.targetIds.size() > 0 || a.targetId.isNotEmpty()) {
                        canvas.deselectAll();
                        auto const ids = a.targetIds.size() > 0 ? a.targetIds : juce::StringArray(a.targetId);
                        for (auto const& tid : ids) {
                            if (auto* obj = findObjectByStableId(tid)) canvas.setSelected(obj, true);
                        }
                    }
                    // 2. Dispatch prompt
                    sendPillPrompt(chip.prompt, false);
                    // 3. Log to Sidebar chat
                    if (canvas.editor) {
                        if (auto* cp = canvas.editor->getCopilotPanel())
                            cp->receiveMessage("user", chip.prompt);
                    }
                    mcpNoteHover = -1;
                    mcpNoteHoverChip = -1;
                    canvas.repaint();
                    if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
                    return true;
                }
                curChipX += cw + 4.0f;
            }
            return true;
        }

        // 3. Body click on tape header:
        // Select module objects & open note in Sidebar Master Notebook
        if (a.targetIds.size() > 0 || a.targetId.isNotEmpty()) {
            canvas.deselectAll();
            auto const ids = a.targetIds.size() > 0 ? a.targetIds : juce::StringArray(a.targetId);
            for (auto const& tid : ids) {
                if (auto* obj = findObjectByStableId(tid)) canvas.setSelected(obj, true);
            }
        }
        if (auto* copilotSidebar = canvas.editor ? canvas.editor->getSidebarForPanel(Sidebar::CopilotChatPanel) : nullptr) {
            copilotSidebar->showSidebar(true);
            copilotSidebar->showPanel(Sidebar::CopilotChatPanel);
            if (auto* cp = canvas.editor->getCopilotPanel()) {
                cp->showAnnotationNote(a.text, a.kind, a.targetIds);
            }
        }
        canvas.repaint();
        if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
        return true;
    }
    return false;
}
