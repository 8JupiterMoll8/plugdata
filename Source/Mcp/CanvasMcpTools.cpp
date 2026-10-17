/*
 // The MCP layer of one Canvas (Plan 3 P3.2 S8), part Tools: tools: sketch/eraser/type tools, studio mode, notes created by the type tool, sketch fade/bounds/JSON/build, image paste, voice/MIDI take and reference image clicks, approvals.
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

void CanvasMcpLayer::setSketchToolActive(bool active)
{
    mcpSketchToolActive = active;
    if (active) {
        mcpEraserToolActive = false; // one tool at a time
        mcpTextToolActive = false;
    }
    canvas.setMouseCursor((active || mcpEraserToolActive || mcpTextToolActive) ? MouseCursor::CrosshairCursor : MouseCursor::NormalCursor);
    // The chisel's presence follows the pen: arming shows it, dropping it hides
    // it (when the board is empty). Keeps the mode state always visible.
    updateSelectionPill();
    canvas.repaint();
    if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
}

void CanvasMcpLayer::setEraserToolActive(bool active)
{
    mcpEraserToolActive = active;
    if (active) {
        mcpSketchToolActive = false; // one tool at a time
        mcpTextToolActive = false;
    }
    canvas.setMouseCursor((active || mcpSketchToolActive) ? MouseCursor::CrosshairCursor : MouseCursor::NormalCursor);
    updateSelectionPill();
    canvas.repaint();
    if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
}

void CanvasMcpLayer::setTextToolActive(bool active)
{
    mcpTextToolActive = active;
    if (active) {
        mcpSketchToolActive = false; // one tool at a time
        mcpEraserToolActive = false;
        canvas.setMouseCursor(MouseCursor::IBeamCursor);
    } else if (!mcpSketchToolActive && !mcpEraserToolActive) {
        canvas.setMouseCursor(MouseCursor::NormalCursor);
    }
    updateSelectionPill();
    canvas.repaint();
    if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
}

bool CanvasMcpLayer::isSketchToolActive() const
{
    return mcpSketchToolActive;
}

bool CanvasMcpLayer::isSketchingActive() const
{
    return isSketching;
}

void CanvasMcpLayer::setStudioMode(bool active)
{
    mcpStudioMode = false;
    setSketchToolActive(false);
    updateSelectionPill();
    canvas.repaint();
    if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
}

void CanvasMcpLayer::setStudioInkVisible(bool visible)
{
    mcpStudioInkVisible = visible;
    canvas.repaint();
    if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
}

void CanvasMcpLayer::commitStudioFromCanvas()
{
    juce::String draft;
    if (canvas.editor) {
        if (auto* cp = canvas.editor->getCopilotPanel()) draft = cp->getStudioDraft();
    }
    sendSketchBuild(draft);
    if (canvas.editor) {
        if (auto* cp = canvas.editor->getCopilotPanel()) cp->clearStudioDraft();
    }
}

juce::StringArray CanvasMcpLayer::getSelectionStableIds()
{
    juce::StringArray ids;
    if (!canvas.pd) return ids;
    auto patchPtr = canvas.patch.getPointer();
    t_canvas* cnv = patchPtr ? patchPtr.get() : nullptr;
    for (auto* obj : canvas.getSelectionOfType<Object>()) {
        if (auto* ptr = obj->getPointer()) {
            auto tid = canvas.pd->getMcp()->getStableId(ptr);
            if (tid.isEmpty() && cnv) {
                tid = canvas.pd->getMcp()->getOrAdoptStableId(cnv, ptr);
            }
            if (tid.isNotEmpty()) ids.add(tid);
        }
    }
    return ids;
}

void CanvasMcpLayer::createTextNoteAt(Point<int> canvasPos)
{
    if (!canvas.pd) return;

    // Drop any abandoned empty artist note first so note indices never drift.
    {
        juce::ScopedLock sl(canvas.pd->getMcp()->session.mcpOverlayLock);
        std::erase_if(canvas.pd->getMcp()->session.mcpAnnotations, [](McpSession::McpAnnotation const& a) {
            return a.text.isEmpty() && a.kind == "artist";
        });

        McpSession::McpAnnotation ann;
        ann.x = static_cast<float>(canvasPos.x - canvas.canvasOrigin.x);
        ann.y = static_cast<float>(canvasPos.y - canvas.canvasOrigin.y);
        ann.kind = "artist";
        ann.t = juce::Time::getMillisecondCounterHiRes() / 1000.0;
        // Surgical lane: a selection turns the note into an attachment bound to it
        // (renders anchored beside the object with a leader tick).
        auto const selIds = getSelectionStableIds();
        if (selIds.size() > 0) {
            ann.targetId = selIds[0];
            ann.targetIds = selIds;
        }
        canvas.pd->getMcp()->session.mcpAnnotations.push_back(ann);
    }

    openNoteEditorForIndex(static_cast<int>(canvas.pd->getMcp()->session.getMcpAnnotations().size()) - 1);
}

void CanvasMcpLayer::openNoteEditorForIndex(int index)
{
    if (!canvas.pd) return;
    auto const anns = canvas.pd->getMcp()->session.getMcpAnnotations();
    if (index < 0 || index >= static_cast<int>(anns.size())) return;
    auto const ann = anns[static_cast<size_t>(index)];

    if (!mcpNoteEditor) {
        mcpNoteEditor = std::make_unique<juce::TextEditor>();
        mcpNoteEditor->setMultiLine(false);
        mcpNoteEditor->setReturnKeyStartsNewLine(false);
        mcpNoteEditor->setWantsKeyboardFocus(true);

        // Commit: pin the typed note. Empty text = discard it entirely.
        auto commit = [this] {
            if (mcpNoteEditIndex < 0) return;
            auto const txt = mcpNoteEditor ? mcpNoteEditor->getText().trim() : juce::String();
            if (canvas.pd) {
                juce::ScopedLock sl(canvas.pd->getMcp()->session.mcpOverlayLock);
                if (mcpNoteEditIndex < static_cast<int>(canvas.pd->getMcp()->session.mcpAnnotations.size())) {
                    if (txt.isEmpty()) {
                        canvas.pd->getMcp()->session.mcpAnnotations.erase(canvas.pd->getMcp()->session.mcpAnnotations.begin() + mcpNoteEditIndex);
                    } else {
                        auto& a = canvas.pd->getMcp()->session.mcpAnnotations[static_cast<size_t>(mcpNoteEditIndex)];
                        a.text = txt;
                        a.kind = "artist";
                    }
                }
            }
            mcpNoteEditIndex = -1;
            if (mcpNoteEditor) mcpNoteEditor->setVisible(false);
            canvas.repaint();
            if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
        };

        mcpNoteEditor->onReturnKey = [commit] { commit(); };
        mcpNoteEditor->onFocusLost = [this, commit] {
            if (juce::Time::getMillisecondCounter() - mcpNoteOpenedAt < 500) return;
            commit();
        };
        // Live feedback: write the field's text onto the note as you type.
        mcpNoteEditor->onTextChange = [this] {
            if (mcpNoteEditor && canvas.pd && mcpNoteEditIndex >= 0) {
                juce::ScopedLock sl(canvas.pd->getMcp()->session.mcpOverlayLock);
                if (mcpNoteEditIndex < static_cast<int>(canvas.pd->getMcp()->session.mcpAnnotations.size()))
                    canvas.pd->getMcp()->session.mcpAnnotations[static_cast<size_t>(mcpNoteEditIndex)].text = mcpNoteEditor->getText();
            }
            canvas.repaint();
            if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
        };
        mcpNoteEditor->onEscapeKey = [commit] { commit(); }; // Esc = finish editing

        mcpNoteEditor->setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff16161c));
        mcpNoteEditor->setColour(juce::TextEditor::textColourId, juce::Colours::white);
        mcpNoteEditor->setColour(juce::TextEditor::outlineColourId, juce::Colour(0xffffbe50));
        mcpNoteEditor->setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour(0xffffbe50));
        mcpNoteEditor->setColour(juce::TextEditor::highlightColourId, juce::Colour(0xffffbe50));
        mcpNoteEditor->setColour(juce::CaretComponent::caretColourId, juce::Colours::white);
        canvas.addAndMakeVisible(*mcpNoteEditor);
        if (!mcpSlotPasteKeys) mcpSlotPasteKeys = std::make_unique<SlotPasteKeyListener>(this);
        mcpNoteEditor->addKeyListener(mcpSlotPasteKeys.get());
    }

    mcpNoteEditIndex = index;
    mcpNoteOpenedAt = juce::Time::getMillisecondCounter();
    mcpNoteEditor->setBounds(juce::roundToInt(canvas.canvasOrigin.x + ann.x - 6.0f),
                             juce::roundToInt(canvas.canvasOrigin.y + ann.y - 26.0f),
                             220, 20);
    mcpNoteEditor->setText(ann.text, false);
    mcpNoteEditor->setVisible(true);
    mcpNoteEditor->toFront(true);

    auto* ed = mcpNoteEditor.get();
    juce::MessageManager::callAsync([ed] {
        if (ed) {
            ed->grabKeyboardFocus();
            ed->selectAll();
        }
    });

    canvas.repaint();
    if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
}

void CanvasMcpLayer::SketchFadeTimer::timerCallback()
{
    if (!canvas) return;
    canvas->mcpSketchAlpha -= 0.025f;
    if (canvas->mcpSketchAlpha <= 0.0f) {
        canvas->mcpSketchAlpha = 0.0f;
        canvas->mcpCompletedStrokes.clear();
        stopTimer();
    }
    canvas->canvas.repaint();
    if (canvas->canvas.editor) canvas->canvas.editor->nvgSurface.invalidateAll();
}

void CanvasMcpLayer::startSketchFadeTimer()
{
    mcpSketchAlpha = 1.0f;
    if (!mcpSketchFadeTimer) {
        mcpSketchFadeTimer = std::make_unique<SketchFadeTimer>(this);
    }
    mcpSketchFadeTimer->startTimerHz(40);
}

juce::Rectangle<float> CanvasMcpLayer::getSketchBoundsInCanvas() const
{
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    bool any = false;
    for (auto const& stroke : mcpCompletedStrokes) {
        for (auto const& p : stroke) {
            minX = std::min(minX, p.x);
            minY = std::min(minY, p.y);
            maxX = std::max(maxX, p.x);
            maxY = std::max(maxY, p.y);
            any = true;
        }
    }
    if (!any) return {};
    return { minX, minY, maxX - minX, maxY - minY };
}

juce::String CanvasMcpLayer::buildSketchJson() const
{
    juce::Array<juce::var> strokesVar;
    for (auto const& stroke : mcpCompletedStrokes) {
        juce::Array<juce::var> ptsVar;
        for (auto const& p : stroke) {
            auto* obj = new juce::DynamicObject();
            obj->setProperty("x", p.x - static_cast<float>(canvas.canvasOrigin.x));
            obj->setProperty("y", p.y - static_cast<float>(canvas.canvasOrigin.y));
            obj->setProperty("t", p.t);
            ptsVar.add(juce::var(obj));
        }
        strokesVar.add(juce::var(ptsVar));
    }
    return juce::JSON::toString(juce::var(strokesVar), true);
}

void CanvasMcpLayer::sendSketchBuild(const juce::String& prompt)
{
    if (!canvas.pd) return;
    // Visible-Only Ingestion: hidden ink is excluded from the brief (sent as []).
    if (mcpCompletedStrokes.empty() && canvas.pd->getMcp()->session.getMcpReferenceImages().empty()) return;

    // Committing = done drawing: disarm the pen so the artist can immediately
    // drag/tweak the freshly built patch without leaving ink everywhere.
    // (In Studio mode the drawer owns the pen — keep it up for the next idea.)
    if (!mcpStudioMode) setSketchToolActive(false);

    // Bounding box in PD patch coords — construct_patch_v4 speaks this space.
    juce::String bboxJson = "{}";
    if (mcpStudioInkVisible && !mcpCompletedStrokes.empty()) {
        auto const sb = getSketchBoundsInCanvas();
        auto* bboxObj = new juce::DynamicObject();
        bboxObj->setProperty("x", sb.getX() - static_cast<float>(canvas.canvasOrigin.x));
        bboxObj->setProperty("y", sb.getY() - static_cast<float>(canvas.canvasOrigin.y));
        bboxObj->setProperty("w", sb.getWidth());
        bboxObj->setProperty("h", sb.getHeight());
        bboxJson = juce::JSON::toString(juce::var(bboxObj), true);
    }

    // Park the artist's words in the sidebar log (canvas stays calm).
    if (canvas.editor) {
        if (auto* cp = canvas.editor->getCopilotPanel()) {
            cp->receiveMessage("user", prompt.isNotEmpty() ? prompt : juce::String("Build from moodboard"));
        }
    }

    auto const subpatch = canvas.patch.getPointer() ? juce::String(reinterpret_cast<uintptr_t>(canvas.patch.getPointer().get())) : juce::String("0");
    if (auto* br = canvas.pd->getMcp()->getMCPBridge()) {
        br->sendSketchBuild(subpatch, mcpStudioInkVisible ? buildSketchJson() : juce::String("[]"), bboxJson, prompt);
    }

    // ── Commit: the ink did its job and clears; the mood STAYS. ───────────────
    // Artist-first default (learned in the field): pictures, notes and takes are
    // moodboard furniture — they survive the build so you can A/B the new patch
    // against the reference it came from. Remove them explicitly with 🗑/🔒.
    mcpCompletedStrokes.clear();
    mcpCurrentStroke.clear();
    mcpLastSketchBounds = {};

    updateSelectionPill();
    canvas.repaint();
    if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
}

void CanvasMcpLayer::undoLastStroke()
{
    if (mcpCompletedStrokes.empty()) return;
    mcpCompletedStrokes.pop_back();
    updateSelectionPill();
    canvas.repaint();
    if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
}

void CanvasMcpLayer::clearAllInk()
{
    mcpCompletedStrokes.clear();
    mcpCurrentStroke.clear();
    mcpSketchAlpha = 1.0f;
    if (mcpSketchFadeTimer) mcpSketchFadeTimer->stopTimer();
    updateSelectionPill();
    canvas.repaint();
    if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
}

//===----------------------------------------------------------------------===//
// Phase 2 — Reference Images (pasted schematics on the canvas glass)
//===----------------------------------------------------------------------===//

namespace {
// JUCE 7.0's SystemClipboard only speaks text. Image paste therefore tries, in order:
//  1. Linux/X11 + Wayland clipboard tools (xclip / wl-paste), bounded by `timeout`;
//  2. a text-path heuristic — many apps copy "file:///path/img.png" as clipboard text.
juce::Image loadImageFromClipboard()
{
    auto const clipboardText = juce::SystemClipboard::getTextFromClipboard().trim();
    if (clipboardText.isNotEmpty() && clipboardText.length() < 4096) {
        juce::File file;
        if (clipboardText.startsWithIgnoreCase("file://"))
            file = juce::URL(clipboardText).getLocalFile();
        else if (juce::File::isAbsolutePath(clipboardText))
            file = juce::File(clipboardText);

        if (file.existsAsFile()) {
            if (auto image = juce::ImageFileFormat::loadFrom(file); image.isValid()) {
                return image;
            }
        }
    }

#if JUCE_LINUX
    auto const tmp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("plugdata_paste_" + juce::String(juce::Random::getSystemRandom().nextInt64()) + ".img");
    auto const quotedPath = "'" + tmp.getFullPathName().replace("'", "'\\''") + "'";
    juce::StringArray attempts;
    attempts.add("timeout 3 xclip -selection clipboard -t image/png -o > " + quotedPath);
    attempts.add("timeout 3 wl-paste --type image/png > " + quotedPath);
    attempts.add("timeout 3 wl-paste > " + quotedPath);
    for (auto const& command : attempts) {
        tmp.deleteFile();
        juce::ChildProcess proc;
        if (!proc.start(juce::StringArray { "/bin/sh", "-c", command })) {
            continue;
        }
        proc.waitForProcessToFinish(3500);
        if (proc.isRunning()) proc.kill();
        if (tmp.existsAsFile() && tmp.getSize() > 0) {
            if (auto image = juce::ImageFileFormat::loadFrom(tmp); image.isValid()) {
                tmp.deleteFile();
                return image;
            }
        }
    }
    tmp.deleteFile();
#endif

    return {};
}
} // namespace

bool CanvasMcpLayer::pasteImageFromClipboard()
{
    auto const clipboardImage = loadImageFromClipboard();
    if (!clipboardImage.isValid()) {
        // Slot is open but the clipboard has no image yet — coach, keep the slot.
        if (mcpImageSlotActive) {
            mcpMoodHint = "Clipboard has no image — copy one, then Ctrl+V here";
            mcpMoodHintUntil = juce::Time::getMillisecondCounter() + 2600;
            canvas.repaint();
            if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
        }
        return false;
    }
    if (!canvas.pd) return true; // consume the paste; there is nowhere to put it

    // Encode once: hash names the cache file, bytes are reused for the disk write.
    juce::MemoryOutputStream pngData;
    juce::PNGImageFormat pngFormat;
    if (!pngFormat.writeImageToStream(clipboardImage, pngData)) return true;
    auto const hash = juce::MD5(pngData.getData(), pngData.getDataSize()).toHexString();

    auto const refsDir = ProjectInfo::appDataDir.getChildFile("references");
    refsDir.createDirectory();
    auto const pngFile = refsDir.getChildFile(hash + ".png");
    if (!pngFile.existsAsFile()) {
        if (auto stream = std::unique_ptr<juce::FileOutputStream>(pngFile.createOutputStream())) {
            stream->write(pngData.getData(), pngData.getDataSize());
            stream->flush();
        }
    }

    // Fit the image to the paste slot when one is open, else to a sane default.
    float const maxW = mcpImageSlotActive ? 320.0f : 520.0f;
    float const maxH = mcpImageSlotActive ? 220.0f : 420.0f;
    float const fit = juce::jmin(1.0f, juce::jmin(maxW / static_cast<float>(clipboardImage.getWidth()), maxH / static_cast<float>(clipboardImage.getHeight())));
    float const w = static_cast<float>(clipboardImage.getWidth()) * fit;
    float const h = static_cast<float>(clipboardImage.getHeight()) * fit;

    auto const mousePosition = canvas.getMouseXYRelative() - canvas.canvasOrigin;

    McpSession::McpReferenceImage ref;
    ref.id = hash;
    ref.filePath = pngFile.getFullPathName();
    ref.title = "Pasted_" + hash.substring(0, 6) + ".png";
    // Surgical lane: pasting over a selection attaches the image to it.
    auto const selIds = getSelectionStableIds();
    if (selIds.size() > 0) ref.targetId = selIds[0];
    if (mcpImageSlotActive) {
        // Drop into the waiting slot, centered.
        ref.x = static_cast<float>(mcpImageSlotPos.x - canvas.canvasOrigin.x) - w * 0.5f;
        ref.y = static_cast<float>(mcpImageSlotPos.y - canvas.canvasOrigin.y) - h * 0.5f;
        mcpImageSlotActive = false; // slot consumed
    } else {
        ref.x = static_cast<float>(mousePosition.x) - w * 0.5f;
        ref.y = static_cast<float>(mousePosition.y) - h * 0.5f;
    }
    ref.width = w;
    ref.height = h;
    canvas.pd->getMcp()->session.addMcpReferenceImage(ref);

    // Bridge dispatch: /pd/reference/image <path> <x> <y> <w> <h> (PD patch coords)
    if (auto* br = canvas.pd->getMcp()->getMCPBridge()) {
        br->sendReferenceImage(ref.filePath, ref.x, ref.y, ref.width, ref.height);
    }

    // Pasting a reference = composing mode: put the pen down so the fresh image
    // is immediately draggable / lockable (re-arm with the ✏️ chip when done).
    // In Studio brainstorm mode the drawer owns the pen — keep drawing.
    if (!mcpStudioMode) setSketchToolActive(false);

    canvas.repaint();
    if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
    return true;
}

bool CanvasMcpLayer::handleVoiceTakeClick(MouseEvent const& e, Point<int> mousePos)
{
    if (!canvas.pd) return false;
    auto const takes = canvas.pd->getMcp()->session.getMcpVoiceTakes();
    for (auto it = takes.rbegin(); it != takes.rend(); ++it) {
        auto const& take = *it;
        if (!take.visible) continue;
        Rectangle<int> const b(static_cast<int>(canvas.canvasOrigin.x + take.x), static_cast<int>(canvas.canvasOrigin.y + take.y), 240, 38);
        if (!b.contains(mousePos)) continue;

        int const relX = mousePos.x - b.getX();
        auto* br = canvas.pd->getMcp()->getMCPBridge();

        if (relX >= 30 && relX <= 60) {
            // ▶ / ■ audition toggle
            if (br) br->playVoiceTake(take.id, take.wavPath);
            canvas.repaint();
            if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
            return true;
        }
        if (relX >= b.getWidth() - 30) {
            // ✕ remove (stop audition first if it's this one)
            if (br && br->getPlayingVoiceTakeId() == take.id) br->stopVoiceTake();
            canvas.pd->getMcp()->session.removeMcpVoiceTake(take.id);
            canvas.repaint();
            if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
            return true;
        }

        // Body: drag the chip
        mcpDraggingTakeId = take.id;
        mcpTakeDragOffset = mousePos - b.getPosition();
        canvas.setMouseCursor(juce::MouseCursor::DraggingHandCursor);
        return true;
    }
    return false;
}

bool CanvasMcpLayer::handleMidiTakeClick(MouseEvent const& e, Point<int> mousePos)
{
    if (!canvas.pd) return false;
    auto const takes = canvas.pd->getMcp()->session.getMcpMidiTakes();
    for (auto it = takes.rbegin(); it != takes.rend(); ++it) {
        auto const& take = *it;
        if (!take.visible) continue;
        Rectangle<int> const b(static_cast<int>(canvas.canvasOrigin.x + take.x), static_cast<int>(canvas.canvasOrigin.y + take.y), 240, 38);
        if (!b.contains(mousePos)) continue;

        int const relX = mousePos.x - b.getX();
        auto* br = canvas.pd->getMcp()->getMCPBridge();

        if (relX >= 26 && relX <= 50) {
            // ▶ / ⏸ audition toggle
            if (br) br->playMidiTake(take.id, take.json);
            canvas.repaint();
            if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
            return true;
        }

        if (relX >= b.getWidth() - 30) {
            // ✕ remove (stop playback first if it's currently playing)
            if (br && br->getPlayingMidiTakeId() == take.id) br->stopMidiTake();
            canvas.pd->getMcp()->session.removeMcpMidiTake(take.id);
            canvas.repaint();
            if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
            return true;
        }

        // Body: drag the chip
        mcpDraggingMidiTakeId = take.id;
        mcpMidiTakeDragOffset = mousePos - b.getPosition();
        canvas.setMouseCursor(juce::MouseCursor::DraggingHandCursor);
        return true;
    }
    return false;
}

bool CanvasMcpLayer::handleReferenceImageClick(MouseEvent const& e, Point<int> mousePos, bool isRightClick)
{
    if (!canvas.pd) return false;
    auto const images = canvas.pd->getMcp()->session.getMcpReferenceImages();
    for (auto it = images.rbegin(); it != images.rend(); ++it) {
        auto const& img = *it;
        // Include the title header in the hit zone so the tag doubles as a grab handle
        Rectangle<float> const b(static_cast<float>(canvas.canvasOrigin.x) + img.x - 4.0f,
                                 static_cast<float>(canvas.canvasOrigin.y) + img.y - 22.0f,
                                 img.width + 8.0f, img.height + 26.0f);
        if (!b.contains(mousePos.toFloat())) continue;

        if (!isRightClick) {
            if (img.isLocked) return true; // locked: swallow the click, don't lasso through it
            mcpDraggingRefImageId = img.id;
            mcpRefImageDragOffset = mousePos - Point<int>(juce::roundToInt(static_cast<float>(canvas.canvasOrigin.x) + img.x),
                                                          juce::roundToInt(static_cast<float>(canvas.canvasOrigin.y) + img.y));
            canvas.setMouseCursor(juce::MouseCursor::DraggingHandCursor);
            return true;
        }

        auto const imageId = img.id;
        auto const title = img.title.isNotEmpty() ? img.title : juce::File(img.filePath).getFileName();
        auto const locked = img.isLocked;

        juce::PopupMenu menu;
        menu.addSectionHeader("Reference Image");
        menu.addItem(1, "Analyze Circuit with AI");
        menu.addItem(2, locked ? "Unlock Position" : "Lock Position");
        menu.addSeparator();
        menu.addItem(3, "Delete Reference Image");
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(Rectangle<int>(e.getScreenPosition(), e.getScreenPosition().translated(1, 1))),
            [safeThis = juce::Component::SafePointer<Canvas>(&canvas), imageId, title](int result) {
                auto* canvas = safeThis.getComponent();
                if (!canvas || !canvas->pd) return;

                if (result == 1) {
                    auto const prompt = "[REFERENCE IMAGE] Analyze the pasted schematic \"" + title + "\" on the canvas and build a live patch that matches its circuit or panel.";
                    if (canvas->editor) {
                        if (auto* cp = canvas->editor->getCopilotPanel()) cp->receiveMessage("user", prompt);
                    }
                    canvas->getMcpLayer().sendPillPrompt(prompt, false);
                } else if (result == 2) {
                    for (auto const& im : canvas->pd->getMcp()->session.getMcpReferenceImages()) {
                        if (im.id == imageId) {
                            canvas->pd->getMcp()->session.setMcpReferenceLock(imageId, !im.isLocked);
                            break;
                        }
                    }
                    canvas->repaint();
                    if (canvas->editor) canvas->editor->nvgSurface.invalidateAll();
                } else if (result == 3) {
                    canvas->pd->getMcp()->session.removeMcpReferenceImage(imageId);
                    canvas->getMcpLayer().mcpRefImageCache.erase(imageId.toStdString());
                    canvas->repaint();
                    if (canvas->editor) canvas->editor->nvgSurface.invalidateAll();
                }
            });
        return true;
    }
    return false;
}

// The server signals a pending destructive op via the HUD chapter ("APPROVE?").
bool CanvasMcpLayer::isApprovalRequested() const
{
    if (!canvas.pd) return false;
    auto const hud = canvas.pd->getMcp()->session.getMcpHud();
    return hud.active && hud.chapter.containsIgnoreCase("APPROVE");
}

// Artist clicked [Approve] / [Cancel] (or pressed Enter / Esc). The pill prompt
// path carries it to the server, which holds the token inert until this yes.
void CanvasMcpLayer::sendApproval(bool approve)
{
    mcpApprovalFrame = {};
    mcpApprovalYes = {};
    mcpApprovalNo = {};
    sendPillPrompt(approve ? "approve" : "deny");
    canvas.repaint();
    if (canvas.editor) canvas.editor->nvgSurface.invalidateAll();
}
