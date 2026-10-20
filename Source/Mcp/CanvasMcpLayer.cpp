/*
 // The MCP layer of one Canvas (Plan 3 P3.2 S7). Moved verbatim out of Source/Canvas.cpp
 // (performRender blocks, the AI overlay helpers) and Source/Object.cpp (the AI state
 // mark); implicit Canvas members became `canvas.` accesses. See CanvasMcpLayer.h.
 */
#include <juce_gui_basics/juce_gui_basics.h>
#include "Utility/Config.h"
#include "Utility/Fonts.h"
#include "Utility/NVGTextShim.h"

#include "Mcp/CanvasMcpLayer.h"
#include "Canvas.h"
#include "Object.h"
#include "Connection.h"
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "LookAndFeel.h"
#include "CanvasViewport.h"
#include "Mcp/McpHost.h"
#include "Pd/MCPBridge.h"

CanvasMcpLayer::CanvasMcpLayer(Canvas& c)
    : canvas(c)
{
}


std::vector<CanvasMcpLayer::AnnotationPivotChip> CanvasMcpLayer::getAnnotationPivotChips(const juce::String& text, const juce::String& /*kind*/)
{
    juce::String lower = text.toLowerCase();
    std::vector<AnnotationPivotChip> chips;

    if (lower.contains("cutoff") || lower.contains("filter") || lower.contains("vcf") || lower.contains("lop~") || lower.contains("synth") || lower.contains("core")) {
        chips.push_back({ "Dunkler", "Cutoff tiefer und sanfter filtern" });
        chips.push_back({ "Bissiger", "Mehr Obertöne, Drive und Filter-Bite" });
    } else if (lower.contains("decay") || lower.contains("envelope") || lower.contains("modulator") || lower.contains("vline~") || lower.contains("vca")) {
        chips.push_back({ "Kürzer", "Kürzere und knackigere Hüllkurve" });
        chips.push_back({ "Länger", "Längere Decay-Zeit für schwebenden Tail" });
    } else if (lower.contains("control") || lower.contains("macro") || lower.contains("bay") || lower.contains("freq")) {
        chips.push_back({ "Tief & Fett", "Tiefere Grundfrequenz und fetter Bass" });
        chips.push_back({ "Höher", "Höhere Lage für Melodie und Lead" });
    } else if (lower.contains("drive") || lower.contains("tanh~") || lower.contains("crunch") || lower.contains("grit")) {
        chips.push_back({ "Cleaner", "Weniger Sättigung und Verzerrung" });
        chips.push_back({ "Mehr Grit", "Mehr Drive und analoge Crunch-Sättigung" });
    } else {
        chips.push_back({ "Variieren", "Variiere die Klangfarbe und Parameter dieses Moduls" });
        chips.push_back({ "Extremer", "Extremere Modulation und Charakter" });
    }

    chips.push_back({ "Reply", "__reply__" });
    chips.push_back({ "Undo", "__undo__" });

    return chips;
}

float CanvasMcpLayer::getChipWidth(const juce::String& label)
{
    return std::max(46.0f, label.length() * 6.6f + 16.0f);
}

void CanvasMcpLayer::paintUnderObjects(NVGcontext* nvg, Rectangle<int> invalidRegion)
{
    // PRD overlay: titled regions drawn BEHIND the objects (structure at a glance).
    if (canvas.pd && shouldShowAIRegions() && !canvas.pd->getMcp()->session.getMcpRegions().empty()) {
        for (auto const& r : canvas.pd->getMcp()->session.getMcpRegions()) {
            float rx = 0.0f, ry = 0.0f, rw = 0.0f, rh = 0.0f;
            if (r.targetIds.size() > 0) {
                bool first = true;
                int minX = 0, minY = 0, maxX = 0, maxY = 0;
                for (auto const& tid : r.targetIds) {
                    if (auto* obj = findObjectByStableId(tid)) {
                        auto const b = obj->getObjectBounds();
                        if (first) {
                            minX = b.getX();
                            minY = b.getY();
                            maxX = b.getRight();
                            maxY = b.getBottom();
                            first = false;
                        } else {
                            minX = std::min(minX, b.getX());
                            minY = std::min(minY, b.getY());
                            maxX = std::max(maxX, b.getRight());
                            maxY = std::max(maxY, b.getBottom());
                        }
                    }
                }
                if (first) continue; // All member objects deleted -> auto-prune
                constexpr int padX = 18;
                constexpr int padYTop = 48;
                constexpr int padYBottom = 18;
                rx = canvas.canvasOrigin.x + static_cast<float>(minX - padX);
                ry = canvas.canvasOrigin.y + static_cast<float>(minY - padYTop);
                rw = static_cast<float>(maxX - minX + padX * 2);
                rh = static_cast<float>(maxY - minY + padYTop + padYBottom);
            } else {
                rx = canvas.canvasOrigin.x + r.x;
                ry = canvas.canvasOrigin.y + r.y;
                rw = r.w;
                rh = r.h;
            }

            NVGScopedState scopedRegion(nvg);
            juce::String const k = r.kind.isNotEmpty() ? r.kind : juce::String("group");
            int ar = 0x8a, ag = 0x93, ab = 0xa6;
            if (k == "source") { ar = 0x3d; ag = 0xdd; ab = 0x8a; }      // Emerald Green
            else if (k == "fx") { ar = 0x4a; ag = 0x9e; ab = 0xff; }     // Electric Cyan
            else if (k == "loop") { ar = 0xff; ag = 0xbe; ab = 0x50; }   // Warm Amber
            else if (k == "output") { ar = 0xff; ag = 0x5a; ab = 0x5a; } // Coral Red

            // 1. Module Chassis Backplate
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, rx, ry, rw, rh, 8.0f);
            nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(ar, ag, ab, 22));
            nanovg::nvgFill(nvg);
            nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(ar, ag, ab, 140));
            nanovg::nvgStrokeWidth(nvg, 1.2f);
            nanovg::nvgLineStyle(nvg, NVG_LINE_DASHED);
            nanovg::nvgDashLength(nvg, 6.0f);
            nanovg::nvgStroke(nvg);
            nanovg::nvgLineStyle(nvg, NVG_LINE_SOLID);

            // 2. Hardware Module Faceplate Title Badge
            if (r.title.isNotEmpty()) {
                mcpnvg::nvgFontSize(nvg, 11.0f);
                mcpnvg::nvgFontFace(nvg, "Inter");
                float bounds[4];
                mcpnvg::nvgTextBounds(nvg, rx + 10.0f, ry + 6.0f, r.title.toRawUTF8(), nullptr, bounds);
                float const tw = bounds[2] - bounds[0];

                // Dark chassis badge
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgRoundedRect(nvg, rx + 8.0f, ry + 5.0f, tw + 22.0f, 18.0f, 4.0f);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(16, 18, 22, 230));
                nanovg::nvgFill(nvg);
                nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(ar, ag, ab, 100));
                nanovg::nvgStrokeWidth(nvg, 1.0f);
                nanovg::nvgStroke(nvg);

                // Status Jewel LED
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgCircle(nvg, rx + 16.0f, ry + 14.0f, 2.5f);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(ar, ag, ab, 255));
                nanovg::nvgFill(nvg);

                // Title Text
                mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(ar, ag, ab, 245));
                mcpnvg::nvgText(nvg, rx + 23.0f, ry + 14.0f, r.title.toRawUTF8(), nullptr);
            }
        }
    }

    // PRD Phase 2: pasted reference images render as glass backdrops behind cords/objects.
    renderReferenceImages(nvg, invalidRegion);
    renderVoiceTakes(nvg, invalidRegion);
    renderMidiTakes(nvg, invalidRegion);
}

void CanvasMcpLayer::paintOverObjects(NVGcontext* nvg)
{
    // PRD overlay: Console Tape (Eurorack Scribble Strip) annotations.
    // Pinned directly above the module strip, bounded to column width (0% horizontal spill).
    if (canvas.pd && shouldShowAIAnnotations() && !canvas.pd->getMcp()->session.getMcpAnnotations().empty()) {
        int noteIdx = 0;
        for (auto const& a : canvas.pd->getMcp()->session.getMcpAnnotations()) {
            if (a.text.isEmpty()) continue;
            float rawAx = 0.0f, rawAy = 0.0f, rawLx = 0.0f, rawLy = 0.0f;
            bool hasLead = false;
            juce::Rectangle<int> grpBounds;
            if (!getAnnotationLiveBounds(a.targetId, a.targetIds, a.x, a.y, a.leaderX, a.leaderY, a.hasLeader, rawAx, rawAy, rawLx, rawLy, hasLead, &grpBounds))
                continue;

            auto const tapeBounds = getAnnotationScreenBounds(static_cast<size_t>(noteIdx));
            if (tapeBounds.isEmpty()) continue;

            NVGScopedState scopedAnn(nvg);
            float const ax = tapeBounds.getX();
            float const ay = tapeBounds.getY();
            float const tapeW = tapeBounds.getWidth();
            float const tapeH = tapeBounds.getHeight();

            // Colour by kind so the artist can tell notes apart at a glance.
            juce::String const k = a.kind.isNotEmpty() ? a.kind : juce::String("info");
            int ar = 0x4a, ag = 0x9e, ab = 0xff; // cyan
            if (k == "change") { ar = 0x3d; ag = 0xdd; ab = 0x8a; }      // green
            else if (k == "warn") { ar = 0xff; ag = 0x5a; ab = 0x5a; }   // red
            else if (k == "artist") { ar = 0xff; ag = 0xbe; ab = 0x50; } // amber
            NVGcolor const accent = nanovg::nvgRGBA(ar, ag, ab, 255);
            bool const isHovered = (noteIdx == mcpNoteHover);
            bool const isCloseHover = (isHovered && mcpNoteHoverClose);

            // 1. Tactile Selection Region: illuminate with soft neon tint on hover
            if (isHovered && !grpBounds.isEmpty()) {
                constexpr int pad = 6;
                float const rx = canvas.canvasOrigin.x + static_cast<float>(grpBounds.getX() - pad);
                float const ry = canvas.canvasOrigin.y + static_cast<float>(grpBounds.getY() - pad);
                float const rw = static_cast<float>(grpBounds.getWidth() + pad * 2);
                float const rh = static_cast<float>(grpBounds.getHeight() + pad * 2);

                nanovg::nvgBeginPath(nvg);
                nanovg::nvgRoundedRect(nvg, rx, ry, rw, rh, 5.0f);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(ar, ag, ab, 22));
                nanovg::nvgFill(nvg);
                nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(ar, ag, ab, 170));
                nanovg::nvgStrokeWidth(nvg, 1.2f);
                nanovg::nvgLineStyle(nvg, NVG_LINE_DASHED);
                nanovg::nvgDashLength(nvg, 5.0f);
                nanovg::nvgStroke(nvg);
                nanovg::nvgLineStyle(nvg, NVG_LINE_SOLID);
            }

            // 2. Hardware Console Tape Anchor: vertical tick connecting tape to module
            if (hasLead && !grpBounds.isEmpty()) {
                float const tickX = ax + 14.0f;
                float const tickY1 = ay + tapeH;
                float const tickY2 = canvas.canvasOrigin.y + static_cast<float>(grpBounds.getY());
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgMoveTo(nvg, tickX, tickY1);
                nanovg::nvgLineTo(nvg, tickX, tickY2);
                nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(ar, ag, ab, isHovered ? 200 : 120));
                nanovg::nvgStrokeWidth(nvg, 1.0f);
                nanovg::nvgStroke(nvg);

                // Small anchor rivet dot at module top edge
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgCircle(nvg, tickX, tickY2, 2.0f);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(ar, ag, ab, isHovered ? 240 : 160));
                nanovg::nvgFill(nvg);
            }

            // 3. Drop Shadow for depth
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, ax, ay + 1.0f, tapeW, tapeH, 4.0f);
            nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(10, 10, 14, 180));
            nanovg::nvgFill(nvg);

            // 4. Console Tape Chassis (Matte dark Eurorack scribble strip)
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, ax, ay, tapeW, tapeH, 3.5f);
            nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(20, 21, 26, isHovered ? 250 : 230));
            nanovg::nvgFill(nvg);
            nanovg::nvgStrokeColor(nvg, isHovered ? accent : nanovg::nvgRGBA(ar, ag, ab, 140));
            nanovg::nvgStrokeWidth(nvg, isHovered ? 1.4f : 1.0f);
            nanovg::nvgStroke(nvg);

            // 5. Top bevel reflection (milled brushed aluminum edge)
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgMoveTo(nvg, ax + 3.0f, ay + 1.0f);
            nanovg::nvgLineTo(nvg, ax + tapeW - 3.0f, ay + 1.0f);
            nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(255, 255, 255, isHovered ? 50 : 25));
            nanovg::nvgStrokeWidth(nvg, 1.0f);
            nanovg::nvgStroke(nvg);

            // 6. Left accent bar
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, ax + 2.0f, ay + 2.5f, 2.5f, tapeH - 5.0f, 1.2f);
            nanovg::nvgFillColor(nvg, accent);
            nanovg::nvgFill(nvg);

            // 7. Jewel LED Diode on tape
            float const ledX = ax + 14.0f;
            float const ledY = ay + tapeH * 0.5f;

            // Soft glow
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgCircle(nvg, ledX, ledY, 5.0f);
            nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(ar, ag, ab, isHovered ? 60 : 30));
            nanovg::nvgFill(nvg);

            // Diode body
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgCircle(nvg, ledX, ledY, 2.8f);
            nanovg::nvgFillColor(nvg, accent);
            nanovg::nvgFill(nvg);

            // Specular glint
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgCircle(nvg, ledX - 0.8f, ledY - 0.8f, 0.8f);
            nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 255, 255, 220));
            nanovg::nvgFill(nvg);

            // 8. Scribble text
            mcpnvg::nvgFontSize(nvg, 11.5f);
            mcpnvg::nvgFontFace(nvg, "Inter");
            mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

            // Scissor so text NEVER overflows past right action buttons
            float const textLeft = ax + 24.0f;
            float const rightZoneW = 32.0f; // space for sidebar icon + close x
            float const maxTextW = std::max(20.0f, tapeW - 24.0f - rightZoneW);
            nanovg::nvgScissor(nvg, textLeft, ay, maxTextW, tapeH);
            nanovg::nvgFillColor(nvg, isHovered ? nanovg::nvgRGBA(255, 255, 255, 255) : nanovg::nvgRGBA(225, 228, 238, 230));
            mcpnvg::nvgText(nvg, textLeft, ay + tapeH * 0.5f, a.text.toRawUTF8(), nullptr);
            nanovg::nvgResetScissor(nvg);

            // 9. Right Icons: Notebook link ↗ & Dismiss ✕
            // Sidebar Notebook link icon (subtle ↗ arrow)
            float const iconX = ax + tapeW - 23.0f;
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgMoveTo(nvg, iconX - 2.5f, ay + tapeH * 0.5f + 2.5f);
            nanovg::nvgLineTo(nvg, iconX + 2.5f, ay + tapeH * 0.5f - 2.5f);
            nanovg::nvgMoveTo(nvg, iconX - 0.5f, ay + tapeH * 0.5f - 2.5f);
            nanovg::nvgLineTo(nvg, iconX + 2.5f, ay + tapeH * 0.5f - 2.5f);
            nanovg::nvgLineTo(nvg, iconX + 2.5f, ay + tapeH * 0.5f + 0.5f);
            nanovg::nvgStrokeColor(nvg, (!isCloseHover && isHovered) ? accent : nanovg::nvgRGBA(ar, ag, ab, 120));
            nanovg::nvgStrokeWidth(nvg, 1.1f);
            nanovg::nvgStroke(nvg);

            // Dismiss 'x'
            float const closeX = ax + tapeW - 9.0f;
            mcpnvg::nvgFontSize(nvg, 10.5f);
            mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nanovg::nvgFillColor(nvg, isCloseHover ? nanovg::nvgRGBA(255, 90, 90, 255) : (isHovered ? nanovg::nvgRGBA(255, 140, 140, 200) : nanovg::nvgRGBA(ar, ag, ab, 130)));
            mcpnvg::nvgText(nvg, closeX, ay + tapeH * 0.5f, "x", nullptr);

            // 10. Pivot Chips Drawer (blooms below hovered tape)
            if (isHovered && mcpNoteEditIndex != noteIdx) {
                auto const chips = getAnnotationPivotChips(a.text, a.kind);
                float totalChipsW = 0.0f;
                for (auto const& c : chips) totalChipsW += getChipWidth(c.label) + 4.0f;
                float const drawerW = std::max(tapeW, totalChipsW + 4.0f);
                float const drawerX = ax;
                float const drawerY = ay + tapeH + 2.0f;
                float const drawerH = 22.0f;

                // Subtle drop shadow
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgRoundedRect(nvg, drawerX, drawerY + 1.0f, drawerW, drawerH, 4.0f);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(8, 9, 12, 160));
                nanovg::nvgFill(nvg);

                // Smoked glass chassis with subtle accent stroke
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgRoundedRect(nvg, drawerX, drawerY, drawerW, drawerH, 3.5f);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(18, 19, 24, 245));
                nanovg::nvgFill(nvg);
                nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(ar, ag, ab, 90));
                nanovg::nvgStrokeWidth(nvg, 1.0f);
                nanovg::nvgStroke(nvg);

                // Render each chip
                float curChipX = drawerX + 4.0f;
                float const chipY = drawerY + 2.5f;
                float const chipH = drawerH - 5.0f;

                for (size_t ci = 0; ci < chips.size(); ++ci) {
                    auto const& chip = chips[ci];
                    float const cw = getChipWidth(chip.label);
                    bool const isThisChipHovered = (mcpNoteHoverChip == static_cast<int>(ci));

                    NVGcolor bgCol, strkCol, txtCol;
                    if (chip.prompt == "__undo__") {
                        if (isThisChipHovered) {
                            bgCol = nanovg::nvgRGBA(170, 45, 45, 240);
                            strkCol = nanovg::nvgRGBA(255, 90, 90, 255);
                            txtCol = nanovg::nvgRGBA(255, 255, 255, 255);
                        } else {
                            bgCol = nanovg::nvgRGBA(34, 22, 24, 210);
                            strkCol = nanovg::nvgRGBA(110, 40, 40, 160);
                            txtCol = nanovg::nvgRGBA(230, 140, 140, 220);
                        }
                    } else if (chip.prompt == "__reply__") {
                        if (isThisChipHovered) {
                            bgCol = nanovg::nvgRGBA(45, 115, 215, 240);
                            strkCol = nanovg::nvgRGBA(90, 175, 255, 255);
                            txtCol = nanovg::nvgRGBA(255, 255, 255, 255);
                        } else {
                            bgCol = nanovg::nvgRGBA(24, 32, 46, 210);
                            strkCol = nanovg::nvgRGBA(55, 95, 150, 160);
                            txtCol = nanovg::nvgRGBA(155, 200, 255, 220);
                        }
                    } else {
                        // Sound pivot chip
                        if (isThisChipHovered) {
                            bgCol = nanovg::nvgRGBA(ar, ag, ab, 220);
                            strkCol = accent;
                            txtCol = nanovg::nvgRGBA(14, 16, 20, 255);
                        } else {
                            bgCol = nanovg::nvgRGBA(28, 30, 38, 220);
                            strkCol = nanovg::nvgRGBA(65, 75, 95, 160);
                            txtCol = nanovg::nvgRGBA(215, 222, 235, 230);
                        }
                    }

                    nanovg::nvgBeginPath(nvg);
                    nanovg::nvgRoundedRect(nvg, curChipX, chipY, cw, chipH, 2.5f);
                    nanovg::nvgFillColor(nvg, bgCol);
                    nanovg::nvgFill(nvg);
                    nanovg::nvgStrokeColor(nvg, strkCol);
                    nanovg::nvgStrokeWidth(nvg, 1.0f);
                    nanovg::nvgStroke(nvg);

                    mcpnvg::nvgFontSize(nvg, 10.0f);
                    mcpnvg::nvgFontFace(nvg, "Inter");
                    mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                    nanovg::nvgFillColor(nvg, txtCol);
                    mcpnvg::nvgText(nvg, curChipX + cw * 0.5f, chipY + chipH * 0.5f, chip.label.toRawUTF8(), nullptr);

                    curChipX += cw + 4.0f;
                }
            }

            ++noteIdx;
        }
    }

    // PRD overlay: draw the inline note editor INTO the GPU frame (same trick as
    // Object's text editor) so what the artist types is visible live.
    if (mcpNoteEditor && mcpNoteEditor->isVisible()) {
        NVGScopedState scopedNote(nvg);
        auto const nb = mcpNoteEditor->getBounds();
        nanovg::nvgTranslate(nvg, static_cast<float>(nb.getX()), static_cast<float>(nb.getY()));
        nanovg::nvgDrawRoundedRect(nvg, 0, 0, static_cast<float>(nb.getWidth()), static_cast<float>(nb.getHeight()),
                           nanovg::nvgRGBA(22, 22, 28, 255), nanovg::nvgRGBA(74, 158, 255, 255), 4.0f);
        canvas.editor->getNanoLLGC()->renderComponent(*mcpNoteEditor);
    }

    // PRD Copilot: the floating selection prompt pill — glass panel drawn in NVG,
    // live TextEditor composited on top (same trick as the note editor).
    if (mcpSelectionPill && mcpSelectionPill->isVisible()) {
        auto const panel = mcpSelectionPillFrame.isEmpty() ? mcpSelectionPill->getBounds() : mcpSelectionPillFrame;
        float const w = static_cast<float>(panel.getWidth());
        float const h = static_cast<float>(panel.getHeight());
        bool isVoiceRec = false;
        float voiceLvl = 0.0f;
        if (canvas.pd) {
            if (auto* br = canvas.pd->getMcp()->getMCPBridge()) {
                isVoiceRec = br->isVoiceCapturing();
                voiceLvl = br->getVoiceLiveLevel();
            }
        }

        {
            // TE-lite hardware look: matte dark panel, top bevel, ✦ Lenses, / Tools, ▶ Send.
            NVGScopedState scopedPanel(nvg);
            nanovg::nvgTranslate(nvg, static_cast<float>(panel.getX()), static_cast<float>(panel.getY()));
            nanovg::nvgDrawRoundedRect(nvg, 0, 0, w, h, nanovg::nvgRGBA(24, 26, 30, 250), nanovg::nvgRGBA(55, 60, 72, 220), 8.0f);

            // Audio-reactive border glow while recording voice / beatbox
            if (isVoiceRec) {
                float const lvl = voiceLvl;
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgRoundedRect(nvg, -1.0f, -1.0f, w + 2.0f, h + 2.0f, 9.0f);
                nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(255, 59, 48, static_cast<unsigned char>(130 + juce::jlimit(0.0f, 125.0f, lvl * 350.0f))));
                nanovg::nvgStrokeWidth(nvg, 1.5f + lvl * 3.5f);
                nanovg::nvgStroke(nvg);
            }

            // Subtle top highlight line
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgMoveTo(nvg, 8.0f, 1.0f);
            nanovg::nvgLineTo(nvg, w - 8.0f, 1.0f);
            nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(255, 255, 255, 25));
            nanovg::nvgStrokeWidth(nvg, 1.0f);
            nanovg::nvgStroke(nvg);

            if (mcpPillSketchMode && mcpCompletedStrokes.empty()) {
                // Mood strip: icons first, surfaces second. [✏] [T] [🖼] [🎙 Melody] [🥁 Beat]
                float const btn = 30.0f;
                float const gap = 4.0f;
                float const pad = 6.0f;
                float const by = (h - btn) * 0.5f;
                bool const capturing = canvas.pd && canvas.pd->getMcp()->getMCPBridge() && canvas.pd->getMcp()->getMCPBridge()->isVoiceCapturing();
                int const captureMode = capturing ? canvas.pd->getMcp()->getMCPBridge()->getVoiceCaptureMode() : 0;
                bool const melodyRec = capturing && captureMode == 1;
                bool const beatRec = capturing && captureMode == 2;
                bool const midiRec = canvas.pd && canvas.pd->getMcp()->getMCPBridge() && canvas.pd->getMcp()->getMCPBridge()->isMidiCapturing();

                for (int i = 0; i < 6; ++i) {
                    float const bx = pad + i * (btn + gap);
                    bool const active = (i == 0 && mcpSketchToolActive) || (i == 1 && mcpTextToolActive) || (i == 3 && melodyRec) || (i == 4 && beatRec) || (i == 5 && midiRec);

                    NVGcolor bg = nanovg::nvgRGBA(38, 42, 50, 230);
                    NVGcolor border = nanovg::nvgRGBA(160, 175, 200, 100);
                    if (active) {
                        if (i == 5) {
                            bg = nanovg::nvgRGBA(140, 60, 230, 245);
                            border = nanovg::nvgRGBA(180, 100, 255, 255);
                        } else {
                            bg = nanovg::nvgRGBA(0, 190, 230, 245);
                            border = nanovg::nvgRGBA(0, 245, 255, 255);
                        }
                    }
                    nanovg::nvgDrawRoundedRect(nvg, bx, by, btn, btn, bg, border, 6.0f);

                    float const cx = bx + btn * 0.5f;
                    float const cy = by + btn * 0.5f;
                    NVGcolor const fg = active ? nanovg::nvgRGBA(12, 14, 18, 255) : nanovg::nvgRGBA(212, 224, 240, 235);

                    if (i == 0) { // pen
                        nanovg::nvgBeginPath(nvg);
                        nanovg::nvgMoveTo(nvg, cx - 5.0f, cy + 5.0f);
                        nanovg::nvgLineTo(nvg, cx + 3.0f, cy - 5.0f);
                        nanovg::nvgLineTo(nvg, cx + 5.5f, cy - 2.5f);
                        nanovg::nvgLineTo(nvg, cx - 2.5f, cy + 5.5f);
                        nanovg::nvgClosePath(nvg);
                        nanovg::nvgStrokeColor(nvg, fg);
                        nanovg::nvgStrokeWidth(nvg, 1.3f);
                        nanovg::nvgStroke(nvg);
                        nanovg::nvgBeginPath(nvg);
                        nanovg::nvgMoveTo(nvg, cx - 5.0f, cy + 5.0f);
                        nanovg::nvgLineTo(nvg, cx - 6.5f, cy + 6.5f);
                        nanovg::nvgStroke(nvg);
                    } else if (i == 1) { // text
                        mcpnvg::nvgFontFace(nvg, "Inter");
                        mcpnvg::nvgFontSize(nvg, 13.0f);
                        mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                        nanovg::nvgFillColor(nvg, fg);
                        mcpnvg::nvgText(nvg, cx, cy, "T", nullptr);
                    } else if (i == 2) { // image
                        nanovg::nvgBeginPath(nvg);
                        nanovg::nvgRoundedRect(nvg, cx - 6.0f, cy - 5.0f, 12.0f, 10.0f, 1.8f);
                        nanovg::nvgStrokeColor(nvg, fg);
                        nanovg::nvgStrokeWidth(nvg, 1.2f);
                        nanovg::nvgStroke(nvg);
                        nanovg::nvgBeginPath(nvg);
                        nanovg::nvgMoveTo(nvg, cx - 5.0f, cy + 3.0f);
                        nanovg::nvgLineTo(nvg, cx - 1.0f, cy - 1.0f);
                        nanovg::nvgLineTo(nvg, cx + 2.0f, cy + 1.5f);
                        nanovg::nvgLineTo(nvg, cx + 5.0f, cy - 2.0f);
                        nanovg::nvgStrokeColor(nvg, fg);
                        nanovg::nvgStrokeWidth(nvg, 1.1f);
                        nanovg::nvgStroke(nvg);
                    } else if (i == 3) { // melody (beamed notes)
                        nanovg::nvgBeginPath(nvg);
                        nanovg::nvgEllipse(nvg, cx - 3.5f, cy + 4.5f, 2.8f, 2.0f);
                        nanovg::nvgFillColor(nvg, fg);
                        nanovg::nvgFill(nvg);
                        nanovg::nvgBeginPath(nvg);
                        nanovg::nvgEllipse(nvg, cx + 4.0f, cy + 3.0f, 2.8f, 2.0f);
                        nanovg::nvgFillColor(nvg, fg);
                        nanovg::nvgFill(nvg);
                        nanovg::nvgBeginPath(nvg);
                        nanovg::nvgMoveTo(nvg, cx - 0.9f, cy + 4.4f);
                        nanovg::nvgLineTo(nvg, cx - 0.9f, cy - 5.5f);
                        nanovg::nvgLineTo(nvg, cx + 6.6f, cy - 6.8f);
                        nanovg::nvgLineTo(nvg, cx + 6.6f, cy + 2.9f);
                        nanovg::nvgStrokeColor(nvg, fg);
                        nanovg::nvgStrokeWidth(nvg, 1.4f);
                        nanovg::nvgStroke(nvg);

                        if (melodyRec) { // live level aura
                            float const lvl = canvas.pd->getMcp()->getMCPBridge()->getVoiceLiveLevel();
                            nanovg::nvgBeginPath(nvg);
                            nanovg::nvgRoundedRect(nvg, bx - 2.0f - lvl * 4.0f, by - 2.0f - lvl * 4.0f,
                                           btn + 4.0f + lvl * 8.0f, btn + 4.0f + lvl * 8.0f, 8.0f);
                            nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(255, 60, 45, static_cast<unsigned char>(120 + juce::jlimit(0.0f, 130.0f, lvl * 350.0f))));
                            nanovg::nvgStrokeWidth(nvg, 1.5f);
                            nanovg::nvgStroke(nvg);
                        }
                    } else if (i == 4) { // beat (drum glyph)
                        nanovg::nvgBeginPath(nvg);
                        nanovg::nvgRoundedRect(nvg, cx - 6.0f, cy - 0.5f, 12.0f, 6.5f, 2.0f);
                        nanovg::nvgStrokeColor(nvg, fg);
                        nanovg::nvgStrokeWidth(nvg, 1.2f);
                        nanovg::nvgStroke(nvg);
                        nanovg::nvgBeginPath(nvg);
                        nanovg::nvgEllipse(nvg, cx, cy - 0.5f, 6.0f, 2.0f);
                        nanovg::nvgStrokeColor(nvg, fg);
                        nanovg::nvgStrokeWidth(nvg, 1.2f);
                        nanovg::nvgStroke(nvg);
                        // crossed beaters
                        nanovg::nvgBeginPath(nvg);
                        nanovg::nvgMoveTo(nvg, cx - 4.5f, cy - 7.0f);
                        nanovg::nvgLineTo(nvg, cx + 1.0f, cy - 1.5f);
                        nanovg::nvgMoveTo(nvg, cx + 4.5f, cy - 7.0f);
                        nanovg::nvgLineTo(nvg, cx - 1.0f, cy - 1.5f);
                        nanovg::nvgStrokeColor(nvg, fg);
                        nanovg::nvgStrokeWidth(nvg, 1.1f);
                        nanovg::nvgStroke(nvg);

                        if (beatRec) { // live level aura
                            float const lvl = canvas.pd->getMcp()->getMCPBridge()->getVoiceLiveLevel();
                            nanovg::nvgBeginPath(nvg);
                            nanovg::nvgRoundedRect(nvg, bx - 2.0f - lvl * 4.0f, by - 2.0f - lvl * 4.0f,
                                           btn + 4.0f + lvl * 8.0f, btn + 4.0f + lvl * 8.0f, 8.0f);
                            nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(255, 60, 45, static_cast<unsigned char>(120 + juce::jlimit(0.0f, 130.0f, lvl * 350.0f))));
                            nanovg::nvgStrokeWidth(nvg, 1.5f);
                            nanovg::nvgStroke(nvg);
                        }
                    } else { // i == 5: MIDI (piano keyboard glyph)
                        nanovg::nvgBeginPath(nvg);
                        nanovg::nvgRoundedRect(nvg, cx - 7.0f, cy - 6.0f, 14.0f, 12.0f, 1.5f);
                        nanovg::nvgStrokeColor(nvg, fg);
                        nanovg::nvgStrokeWidth(nvg, 1.2f);
                        nanovg::nvgStroke(nvg);
                        nanovg::nvgBeginPath(nvg);
                        nanovg::nvgMoveTo(nvg, cx - 2.5f, cy - 6.0f);
                        nanovg::nvgLineTo(nvg, cx - 2.5f, cy + 6.0f);
                        nanovg::nvgMoveTo(nvg, cx + 2.0f, cy - 6.0f);
                        nanovg::nvgLineTo(nvg, cx + 2.0f, cy + 6.0f);
                        nanovg::nvgStrokeColor(nvg, fg);
                        nanovg::nvgStrokeWidth(nvg, 1.0f);
                        nanovg::nvgStroke(nvg);
                        nanovg::nvgBeginPath(nvg);
                        nanovg::nvgRect(nvg, cx - 4.5f, cy - 6.0f, 1.8f, 6.5f);
                        nanovg::nvgRect(nvg, cx + 0.2f, cy - 6.0f, 1.8f, 6.5f);
                        nanovg::nvgFillColor(nvg, fg);
                        nanovg::nvgFill(nvg);

                        if (midiRec) {
                            float const pulse = 0.65f + 0.35f * std::sin(static_cast<float>(juce::Time::getMillisecondCounter()) * 0.008f);
                            nanovg::nvgBeginPath(nvg);
                            nanovg::nvgRoundedRect(nvg, bx - 2.0f, by - 2.0f, btn + 4.0f, btn + 4.0f, 8.0f);
                            nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(180, 70, 255, static_cast<unsigned char>(140 + 100 * pulse)));
                            nanovg::nvgStrokeWidth(nvg, 1.6f);
                            nanovg::nvgStroke(nvg);
                        }
                    }
                }

                // Record slot: declared intent + live level while capturing.
                if (melodyRec || beatRec || midiRec) {
                    int const nNotes = (canvas.pd && canvas.pd->getMcp()->getMCPBridge()) ? canvas.pd->getMcp()->getMCPBridge()->getMidiCaptureNoteCount() : 0;
                    juce::String const recText = midiRec
                        ? ("Play MIDI keyboard - tap keys to stop (" + juce::String(nNotes) + " notes)")
                        : (melodyRec ? "Sing a melody - tap the note to stop" : "Beatbox - tap the drum to stop");
                    float const boxW = midiRec ? 340.0f : 330.0f;
                    float const boxX = (w - boxW) * 0.5f;
                    float const boxY = -44.0f;
                    NVGcolor const recBorder = midiRec ? nanovg::nvgRGBA(180, 80, 255, 220) : nanovg::nvgRGBA(255, 70, 58, 220);
                    nanovg::nvgDrawRoundedRect(nvg, boxX, boxY, boxW, 32.0f, nanovg::nvgRGBA(26, 18, 20, 242), recBorder, 7.0f);

                    float const pulse = 0.65f + 0.35f * std::sin(static_cast<float>(juce::Time::getMillisecondCounter()) * 0.008f);
                    float const lvl = juce::jlimit(0.0f, 1.0f, canvas.pd->getMcp()->getMCPBridge()->getVoiceLiveLevel() * 3.5f);
                    nanovg::nvgBeginPath(nvg);
                    nanovg::nvgCircle(nvg, boxX + 15.0f, boxY + 16.0f, 4.0f);
                    NVGcolor const dotColor = midiRec
                        ? nanovg::nvgRGBA(190, 80, 255, static_cast<unsigned char>(160 + 95 * pulse))
                        : nanovg::nvgRGBA(255, 60, 45, static_cast<unsigned char>(160 + 95 * pulse));
                    nanovg::nvgFillColor(nvg, dotColor);
                    nanovg::nvgFill(nvg);

                    if (!midiRec) {
                        // live level meter
                        nanovg::nvgDrawRoundedRect(nvg, boxX + 26.0f, boxY + 13.0f, 70.0f, 6.0f, nanovg::nvgRGBA(50, 30, 32, 230), nanovg::nvgRGBA(90, 50, 52, 120), 3.0f);
                        nanovg::nvgDrawRoundedRect(nvg, boxX + 26.0f, boxY + 13.0f, 4.0f + lvl * 66.0f, 6.0f, nanovg::nvgRGBA(255, 70, 55, 235), nanovg::nvgRGBA(255, 120, 100, 120), 3.0f);
                    }

                    mcpnvg::nvgFontFace(nvg, "Inter");
                    mcpnvg::nvgFontSize(nvg, 10.5f);
                    mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                    nanovg::nvgFillColor(nvg, midiRec ? nanovg::nvgRGBA(235, 220, 255, 240) : nanovg::nvgRGBA(255, 214, 208, 240));
                    float const tx = midiRec ? (boxX + 28.0f) : (boxX + 104.0f);
                    mcpnvg::nvgText(nvg, tx, boxY + 16.0f, recText.toRawUTF8(), nullptr);
                }

                // Hint chip under the strip
                if (mcpMoodHint.isNotEmpty() && juce::Time::getMillisecondCounter() < mcpMoodHintUntil) {
                    mcpnvg::nvgFontFace(nvg, "Inter");
                    mcpnvg::nvgFontSize(nvg, 11.0f);
                    mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                    float tb[4];
                    mcpnvg::nvgTextBounds(nvg, 0.0f, 0.0f, mcpMoodHint.toRawUTF8(), nullptr, tb);
                    float const hw = (tb[2] - tb[0]) + 16.0f;
                    nanovg::nvgDrawRoundedRect(nvg, 0.0f, h + 6.0f, hw, 20.0f, nanovg::nvgRGBA(16, 18, 22, 232), nanovg::nvgRGBA(255, 190, 80, 150), 5.0f);
                    nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 214, 150, 240));
                    mcpnvg::nvgText(nvg, 8.0f, h + 16.0f, mcpMoodHint.toRawUTF8(), nullptr);
                }
            } else if (mcpPillSketchMode) {
                // Moodboard Chisel: [⚡ Build] | prompt | [T] [🖼] [↩ Undo] [🗑 Clear] [✏️ pen]
                float const chipH = 22.0f;
                float const chipY = (h - chipH) * 0.5f;

                // ⚡ Build chip — the commit action (accent blue)
                {
                    float const bx = 6.0f;
                    float const bw = 80.0f;
                    nanovg::nvgDrawRoundedRect(nvg, bx, chipY, bw, chipH, nanovg::nvgRGBA(32, 108, 214, 245), nanovg::nvgRGBA(96, 178, 255, 255), 5.0f);
                    // vector lightning bolt
                    nanovg::nvgBeginPath(nvg);
                    nanovg::nvgMoveTo(nvg, bx + 15.0f, chipY + 3.5f);
                    nanovg::nvgLineTo(nvg, bx + 10.0f, chipY + 11.5f);
                    nanovg::nvgLineTo(nvg, bx + 14.0f, chipY + 11.5f);
                    nanovg::nvgLineTo(nvg, bx + 12.0f, chipY + 18.5f);
                    nanovg::nvgLineTo(nvg, bx + 19.0f, chipY + 9.5f);
                    nanovg::nvgLineTo(nvg, bx + 14.5f, chipY + 9.5f);
                    nanovg::nvgLineTo(nvg, bx + 17.0f, chipY + 3.5f);
                    nanovg::nvgClosePath(nvg);
                    nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 235, 130, 255));
                    nanovg::nvgFill(nvg);
                    mcpnvg::nvgFontFace(nvg, "Inter");
                    mcpnvg::nvgFontSize(nvg, 11.5f);
                    mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                    nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 255, 255, 255));
                    mcpnvg::nvgText(nvg, bx + 25.0f, chipY + chipH * 0.5f, "Build", nullptr);
                }

                // T attach-text chip (same tools as the surgical lane)
                {
                    float const tx = w - 234.0f;
                    float const tw2 = 24.0f;
                    nanovg::nvgDrawRoundedRect(nvg, tx, chipY, tw2, chipH,
                                       mcpTextToolActive ? nanovg::nvgRGBA(74, 158, 255, 240) : nanovg::nvgRGBA(38, 42, 50, 230),
                                       mcpTextToolActive ? nanovg::nvgRGBA(140, 195, 255, 255) : nanovg::nvgRGBA(160, 175, 200, 110), 4.0f);
                    mcpnvg::nvgFontFace(nvg, "Inter");
                    mcpnvg::nvgFontSize(nvg, 12.0f);
                    mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                    nanovg::nvgFillColor(nvg, mcpTextToolActive ? nanovg::nvgRGBA(255, 255, 255, 255) : nanovg::nvgRGBA(215, 225, 240, 235));
                    mcpnvg::nvgText(nvg, tx + tw2 * 0.5f, chipY + chipH * 0.5f, "T", nullptr);
                }

                // 🖼 attach-image chip (pastes the clipboard onto the mood/sketch)
                {
                    float const ix = w - 206.0f;
                    float const iw = 24.0f;
                    float const icx = ix + iw * 0.5f;
                    float const icy = chipY + chipH * 0.5f;
                    nanovg::nvgDrawRoundedRect(nvg, ix, chipY, iw, chipH, nanovg::nvgRGBA(38, 42, 50, 230), nanovg::nvgRGBA(160, 175, 200, 110), 4.0f);
                    nanovg::nvgBeginPath(nvg);
                    nanovg::nvgRoundedRect(nvg, icx - 5.0f, icy - 4.0f, 10.0f, 8.5f, 1.5f);
                    nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(200, 215, 235, 220));
                    nanovg::nvgStrokeWidth(nvg, 1.1f);
                    nanovg::nvgStroke(nvg);
                    nanovg::nvgBeginPath(nvg);
                    nanovg::nvgMoveTo(nvg, icx - 4.0f, icy + 2.5f);
                    nanovg::nvgLineTo(nvg, icx - 1.0f, icy - 0.5f);
                    nanovg::nvgLineTo(nvg, icx + 1.5f, icy + 1.5f);
                    nanovg::nvgLineTo(nvg, icx + 4.0f, icy + 0.5f);
                    nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(160, 230, 200, 200));
                    nanovg::nvgStrokeWidth(nvg, 1.0f);
                    nanovg::nvgStroke(nvg);
                }

                // ↩ Undo chip
                {
                    float const ux = w - 178.0f;
                    float const uw = 70.0f;
                    float const ucy = chipY + chipH * 0.5f;
                    nanovg::nvgDrawRoundedRect(nvg, ux, chipY, uw, chipH, nanovg::nvgRGBA(38, 42, 50, 235), nanovg::nvgRGBA(160, 175, 200, 110), 4.0f);
                    // back-arrow glyph
                    nanovg::nvgBeginPath(nvg);
                    nanovg::nvgMoveTo(nvg, ux + 18.0f, ucy);
                    nanovg::nvgLineTo(nvg, ux + 10.0f, ucy);
                    nanovg::nvgMoveTo(nvg, ux + 13.5f, ucy - 3.0f);
                    nanovg::nvgLineTo(nvg, ux + 10.0f, ucy);
                    nanovg::nvgLineTo(nvg, ux + 13.5f, ucy + 3.0f);
                    nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(150, 200, 255, 235));
                    nanovg::nvgStrokeWidth(nvg, 1.4f);
                    nanovg::nvgStroke(nvg);
                    mcpnvg::nvgFontFace(nvg, "Inter");
                    mcpnvg::nvgFontSize(nvg, 10.5f);
                    mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                    nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(215, 225, 240, 235));
                    mcpnvg::nvgText(nvg, ux + 23.0f, ucy, "Undo", nullptr);
                }

                // 🗑 Clear chip
                {
                    float const cxX = w - 104.0f;
                    float const cxW = 70.0f;
                    float const ccy = chipY + chipH * 0.5f;
                    nanovg::nvgDrawRoundedRect(nvg, cxX, chipY, cxW, chipH, nanovg::nvgRGBA(58, 30, 34, 235), nanovg::nvgRGBA(220, 110, 110, 140), 4.0f);
                    // trash glyph
                    float const tcx = cxX + 14.0f;
                    nanovg::nvgBeginPath(nvg);
                    nanovg::nvgRoundedRect(nvg, tcx - 3.5f, ccy - 3.0f, 7.0f, 8.5f, 1.5f);
                    nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(255, 175, 175, 240));
                    nanovg::nvgStrokeWidth(nvg, 1.3f);
                    nanovg::nvgStroke(nvg);
                    nanovg::nvgBeginPath(nvg);
                    nanovg::nvgMoveTo(nvg, tcx - 5.0f, ccy - 4.5f);
                    nanovg::nvgLineTo(nvg, tcx + 5.0f, ccy - 4.5f);
                    nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(255, 175, 175, 240));
                    nanovg::nvgStrokeWidth(nvg, 1.3f);
                    nanovg::nvgStroke(nvg);
                    mcpnvg::nvgFontFace(nvg, "Inter");
                    mcpnvg::nvgFontSize(nvg, 10.5f);
                    mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                    nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 200, 200, 240));
                    mcpnvg::nvgText(nvg, cxX + 24.0f, ccy, "Clear", nullptr);
                }

                // ✏️ Pen toggle — put the pen down / pick it back up without knowing Esc
                {
                    float const px = w - 30.0f;
                    float const pw = 24.0f;
                    float const pcy = chipY + chipH * 0.5f;
                    if (mcpSketchToolActive) {
                        nanovg::nvgDrawRoundedRect(nvg, px, chipY, pw, chipH, nanovg::nvgRGBA(0, 180, 220, 240), nanovg::nvgRGBA(0, 245, 255, 255), 4.0f);
                    } else {
                        nanovg::nvgDrawRoundedRect(nvg, px, chipY, pw, chipH, nanovg::nvgRGBA(38, 42, 50, 230), nanovg::nvgRGBA(160, 175, 200, 80), 4.0f);
                    }
                    float const pcx = px + pw * 0.5f;
                    nanovg::nvgBeginPath(nvg);
                    nanovg::nvgMoveTo(nvg, pcx - 4.5f, pcy + 4.5f);
                    nanovg::nvgLineTo(nvg, pcx + 2.5f, pcy - 4.5f);
                    nanovg::nvgLineTo(nvg, pcx + 5.0f, pcy - 2.0f);
                    nanovg::nvgLineTo(nvg, pcx - 2.0f, pcy + 5.0f);
                    nanovg::nvgClosePath(nvg);
                    nanovg::nvgStrokeColor(nvg, mcpSketchToolActive ? nanovg::nvgRGBA(255, 255, 255, 255) : nanovg::nvgRGBA(200, 215, 235, 220));
                    nanovg::nvgStrokeWidth(nvg, 1.2f);
                    nanovg::nvgStroke(nvg);
                    nanovg::nvgBeginPath(nvg);
                    nanovg::nvgMoveTo(nvg, pcx - 4.5f, pcy + 4.5f);
                    nanovg::nvgLineTo(nvg, pcx - 6.0f, pcy + 6.0f);
                    nanovg::nvgStrokeColor(nvg, mcpSketchToolActive ? nanovg::nvgRGBA(255, 255, 255, 255) : nanovg::nvgRGBA(0, 230, 255, 220));
                    nanovg::nvgStrokeWidth(nvg, 1.4f);
                    nanovg::nvgStroke(nvg);
                }
            } else {
            // Left: subtle generative AI sparkle icon ✨
            {
                float const cx = 15.0f;
                float const cy = h * 0.5f;
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgCircle(nvg, cx, cy, 6.0f);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(74, 158, 255, 30));
                nanovg::nvgFill(nvg);
                // 4-point sparkle star
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgMoveTo(nvg, cx, cy - 4.5f);
                nanovg::nvgQuadTo(nvg, cx, cy, cx + 4.5f, cy);
                nanovg::nvgQuadTo(nvg, cx, cy, cx, cy + 4.5f);
                nanovg::nvgQuadTo(nvg, cx, cy, cx - 4.5f, cy);
                nanovg::nvgQuadTo(nvg, cx, cy, cx, cy - 4.5f);
                nanovg::nvgClosePath(nvg);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(140, 195, 255, 220));
                nanovg::nvgFill(nvg);
            }

            // Right ~26px: sleek submit button ↵
            {
                float const subW = 20.0f;
                float const subH = 18.0f;
                float const subX = w - 25.0f;
                float const subY = (h - subH) * 0.5f;
                nanovg::nvgDrawRoundedRect(nvg, subX, subY, subW, subH, nanovg::nvgRGBA(74, 158, 255, 220), nanovg::nvgRGBA(140, 195, 255, 255), 4.0f);
                mcpnvg::nvgFontFace(nvg, "Inter");
                mcpnvg::nvgFontSize(nvg, 11.5f);
                mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 255, 255, 255));
                mcpnvg::nvgText(nvg, subX + subW * 0.5f, subY + subH * 0.5f, "↵", nullptr);
            }
            } // else: object-pill controls
        }

        // Ambient blinking caret when unfocused & empty to clearly signal interactive input affordance
        if (mcpSelectionPill && !mcpSelectionPill->hasKeyboardFocus(true) && mcpSelectionPill->getText().isEmpty()
            && !(mcpPillSketchMode && mcpCompletedStrokes.empty())) {
            bool const caretBlink = ((juce::Time::getMillisecondCounter() / 530) % 2) == 0;
            if (caretBlink) {
                NVGScopedState scopedCaret(nvg);
                float const caretX = panel.getX() + (mcpPillSketchMode ? 96.0f : 38.0f);
                float const caretY = panel.getY() + panel.getHeight() * 0.5f;
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgMoveTo(nvg, caretX, caretY - 7.0f);
                nanovg::nvgLineTo(nvg, caretX, caretY + 7.0f);
                nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(74, 158, 255, 230));
                nanovg::nvgStrokeWidth(nvg, 1.5f);
                nanovg::nvgStroke(nvg);
            }
        }

        // Crisp on-canvas placeholder when empty (subtle dim when focused)
        if (mcpSelectionPill && mcpSelectionPill->getText().isEmpty()
            && !(mcpPillSketchMode && mcpCompletedStrokes.empty())) {
            NVGScopedState scopedHint(nvg);
            mcpnvg::nvgFontFace(nvg, "Inter");
            mcpnvg::nvgFontSize(nvg, 12.0f);
            mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            juce::String hint;
            if (mcpPillSketchMode) {
                hint = mcpCompletedStrokes.empty()
                    ? "New Mood — paint, paste, type... then Build"
                    : "Describe the build, or hit Build...";
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(140, 155, 175, mcpSelectionPill->hasKeyboardFocus(true) ? 90 : 170));
            } else if (isVoiceRec) {
                hint = "Listening... (speak prompt or beatbox groove)";
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 110, 95, 240));
            } else if (mcpActiveLens.isNotEmpty()) {
                hint = "[" + mcpActiveLens.toUpperCase() + "] Ask AI on selection...";
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(140, 155, 175, mcpSelectionPill->hasKeyboardFocus(true) ? 90 : 170));
            } else {
                hint = "Ask AI on selection... (Enter to mutate, Esc to cancel)";
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(140, 155, 175, mcpSelectionPill->hasKeyboardFocus(true) ? 90 : 170));
            }
            mcpnvg::nvgText(nvg, panel.getX() + (mcpPillSketchMode ? 96.0f : 38.0f), panel.getY() + panel.getHeight() * 0.5f, hint.toRawUTF8(), nullptr);
        }

        {
            NVGScopedState scopedField(nvg);
            auto const fb = mcpSelectionPill->getBounds();
            nanovg::nvgTranslate(nvg, static_cast<float>(fb.getX()), static_cast<float>(fb.getY()));
            canvas.editor->getNanoLLGC()->renderComponent(*mcpSelectionPill);
        }

        // Native Horizontal Card Drawer or Smart Command Palette
        if (mcpPillDrawerMode != 0) {
            if (mcpPillDrawerMode == 3) {
                const_cast<CanvasMcpLayer*>(this)->initPaletteItems();
                float const pw = std::max(static_cast<float>(panel.getWidth()), 390.0f);
                float const rowH = 30.0f;
                int const totalCount = static_cast<int>(mcpFilteredPaletteIndices.size());
                int const visibleCount = std::min(8, totalCount);
                float const ph = 28.0f + visibleCount * rowH + 6.0f;
                float const px = static_cast<float>(panel.getX());
                float py = static_cast<float>(panel.getY() + panel.getHeight()) + 5.0f;
                if (py + ph > static_cast<float>(canvas.getHeight()) - 20.0f) {
                    py = static_cast<float>(panel.getY()) - ph - 5.0f;
                }
                const_cast<CanvasMcpLayer*>(this)->mcpPaletteFrame = juce::Rectangle<int>(roundToInt(px), roundToInt(py), roundToInt(pw), roundToInt(ph));

                NVGScopedState scopedPalette(nvg);
                nanovg::nvgTranslate(nvg, px, py);
                // Frosted dark card container
                nanovg::nvgDrawRoundedRect(nvg, 0, 0, pw, ph, nanovg::nvgRGBA(16, 18, 22, 252), nanovg::nvgRGBA(55, 62, 75, 240), 7.0f);

                // Subtle top highlight
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgMoveTo(nvg, 6.0f, 1.0f);
                nanovg::nvgLineTo(nvg, pw - 6.0f, 1.0f);
                nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(255, 255, 255, 25));
                nanovg::nvgStrokeWidth(nvg, 1.0f);
                nanovg::nvgStroke(nvg);

                // Header title
                mcpnvg::nvgFontFace(nvg, "Inter");
                mcpnvg::nvgFontSize(nvg, 10.0f);
                mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(140, 155, 180, 220));
                mcpnvg::nvgText(nvg, 12.0f, 14.0f, "ADD CONTEXT & ACTIONS (+)", nullptr);

                for (int i = 0; i < visibleCount; ++i) {
                    int const itemIdx = mcpFilteredPaletteIndices[static_cast<size_t>(i)];
                    auto const& item = mcpPaletteItems[static_cast<size_t>(itemIdx)];
                    float const ry = 26.0f + i * rowH;
                    bool const isSelected = (i == mcpPaletteSelectedIndex);

                    if (isSelected) {
                        nanovg::nvgDrawRoundedRect(nvg, 4.0f, ry, pw - 8.0f, rowH - 2.0f,
                                           nanovg::nvgRGBA(74, 158, 255, 55), nanovg::nvgRGBA(74, 158, 255, 210), 4.0f);
                    }

                    // Icon / Prefix
                    mcpnvg::nvgFontSize(nvg, 11.5f);
                    mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                    nanovg::nvgFillColor(nvg, isSelected ? nanovg::nvgRGBA(74, 180, 255, 255) : nanovg::nvgRGBA(150, 165, 190, 220));
                    mcpnvg::nvgText(nvg, 18.0f, ry + (rowH - 2.0f) * 0.5f, item.icon.toRawUTF8(), nullptr);

                    // Category badge chip (Action, Skill, Lens)
                    float const chipW = 42.0f;
                    float const chipH = 14.0f;
                    float const chipX = 30.0f;
                    float const chipY = ry + (rowH - 2.0f - chipH) * 0.5f;
                    NVGcolor chipBg = nanovg::nvgRGBA(40, 48, 62, 200);
                    NVGcolor chipFg = nanovg::nvgRGBA(160, 180, 210, 220);
                    if (item.category == "Action") {
                        chipBg = nanovg::nvgRGBA(30, 90, 60, 150);
                        chipFg = nanovg::nvgRGBA(100, 240, 160, 240);
                    } else if (item.category == "Skill") {
                        chipBg = nanovg::nvgRGBA(90, 40, 110, 150);
                        chipFg = nanovg::nvgRGBA(220, 140, 255, 240);
                    } else if (item.category == "Lens") {
                        chipBg = nanovg::nvgRGBA(30, 70, 110, 150);
                        chipFg = nanovg::nvgRGBA(100, 190, 255, 240);
                    }
                    nanovg::nvgDrawRoundedRect(nvg, chipX, chipY, chipW, chipH, chipBg, chipFg, 3.0f);
                    mcpnvg::nvgFontSize(nvg, 8.5f);
                    mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                    nanovg::nvgFillColor(nvg, chipFg);
                    mcpnvg::nvgText(nvg, chipX + chipW * 0.5f, chipY + chipH * 0.5f, item.category.toRawUTF8(), nullptr);

                    // Title
                    mcpnvg::nvgFontSize(nvg, 11.0f);
                    mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                    nanovg::nvgFillColor(nvg, isSelected ? nanovg::nvgRGBA(255, 255, 255, 255) : nanovg::nvgRGBA(215, 225, 240, 230));
                    mcpnvg::nvgText(nvg, chipX + chipW + 8.0f, ry + (rowH - 2.0f) * 0.5f, item.title.toRawUTF8(), nullptr);

                    // Description (dimmed)
                    mcpnvg::nvgFontSize(nvg, 10.0f);
                    nanovg::nvgFillColor(nvg, isSelected ? nanovg::nvgRGBA(180, 205, 240, 240) : nanovg::nvgRGBA(120, 135, 155, 200));
                    mcpnvg::nvgText(nvg, chipX + chipW + 130.0f, ry + (rowH - 2.0f) * 0.5f, item.desc.toRawUTF8(), nullptr);
                }
            } else {
                float const dw = w;
                float const dh = 30.0f;
                float const dx = static_cast<float>(panel.getX());
                float const dy = static_cast<float>(panel.getY() + panel.getHeight()) + 5.0f;
                mcpPillDrawerFrame = juce::Rectangle<int>(roundToInt(dx), roundToInt(dy), roundToInt(dw), roundToInt(dh));

                NVGScopedState scopedDrawer(nvg);
                nanovg::nvgTranslate(nvg, dx, dy);
                // Frosted dark card background with soft border
                nanovg::nvgDrawRoundedRect(nvg, 0, 0, dw, dh, nanovg::nvgRGBA(18, 20, 24, 250), nanovg::nvgRGBA(55, 62, 75, 230), 6.0f);

                // Subtle top highlight line
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgMoveTo(nvg, 6.0f, 1.0f);
                nanovg::nvgLineTo(nvg, dw - 6.0f, 1.0f);
                nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(255, 255, 255, 20));
                nanovg::nvgStrokeWidth(nvg, 1.0f);
                nanovg::nvgStroke(nvg);

                mcpnvg::nvgFontFace(nvg, "Inter");
                mcpnvg::nvgFontSize(nvg, 11.5f);
                mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

                if (mcpPillDrawerMode == 1) {
                    // Lenses: Doctor | Jam | Genesis | Modular | Free
                    const char* lenses[] = { "Doctor", "Jam", "Genesis", "Modular", "Free" };
                    const char* lensKeys[] = { "doctor", "jam", "genesis", "modular", "free" };
                    float const itemW = (dw - 8.0f) / 5.0f;
                    for (int i = 0; i < 5; ++i) {
                        float const ix = 4.0f + i * itemW;
                        float const iy = 3.0f;
                        float const iw = itemW - 3.0f;
                        float const ih = dh - 6.0f;
                        bool const isActive = (mcpActiveLens == lensKeys[i]) || (mcpActiveLens.isEmpty() && i == 4);
                        if (isActive) {
                            nanovg::nvgDrawRoundedRect(nvg, ix, iy, iw, ih, nanovg::nvgRGBA(255, 102, 0, 235), nanovg::nvgRGBA(255, 150, 60, 255), 4.0f);
                            nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(18, 18, 20, 255));
                        } else {
                            nanovg::nvgDrawRoundedRect(nvg, ix, iy, iw, ih, nanovg::nvgRGBA(30, 34, 42, 220), nanovg::nvgRGBA(50, 58, 70, 180), 4.0f);
                            nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(210, 220, 235, 230));
                        }
                        mcpnvg::nvgText(nvg, ix + iw * 0.5f, iy + ih * 0.5f, lenses[i], nullptr);
                    }
                } else if (mcpPillDrawerMode == 2) {
                    // Tools: Explain | Drive | Filter | Reverb | Pack GOP | Tidy
                    const char* tools[] = { "Explain", "Drive", "Filter", "Reverb", "Pack GOP", "Tidy" };
                    float const itemW = (dw - 8.0f) / 6.0f;
                    for (int i = 0; i < 6; ++i) {
                        float const ix = 4.0f + i * itemW;
                        float const iy = 3.0f;
                        float const iw = itemW - 3.0f;
                        float const ih = dh - 6.0f;
                        nanovg::nvgDrawRoundedRect(nvg, ix, iy, iw, ih, nanovg::nvgRGBA(30, 34, 42, 220), nanovg::nvgRGBA(50, 58, 70, 180), 4.0f);
                        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(210, 220, 235, 230));
                        mcpnvg::nvgText(nvg, ix + iw * 0.5f, iy + ih * 0.5f, tools[i], nullptr);
                    }
                }
            }
        } else {
            mcpPillDrawerFrame = {};
            mcpPaletteFrame = {};
        }
    }

    // Tier-2 approval chips — a destructive op waits for the artist's yes.
    // Drawn in CANVAS coords (same space as the pill) so clicks hit-test free.
    if (isApprovalRequested()) {
        juce::Rectangle<int> anchor;
        if (mcpSelectionPill && mcpSelectionPill->isVisible() && !mcpSelectionPillFrame.isEmpty()) {
            anchor = mcpSelectionPillFrame;
        } else {
            auto const sel = canvas.getSelectionOfType<Object>();
            juce::Rectangle<int> b;
            bool first = true;
            for (auto* o : sel) {
                if (!o) continue;
                auto const ob = o->getBounds();
                b = first ? ob : b.getUnion(ob);
                first = false;
            }
            if (!first) anchor = b;
            if (anchor.isEmpty())
                anchor = juce::Rectangle<int>(canvas.canvasOrigin.x - 210, canvas.canvasOrigin.y + 40, 420, 1);
        }
        constexpr int barW = 420;
        constexpr int barH = 46;
        int const bx = anchor.getCentreX() - barW / 2;
        int const by = anchor.getBottom() + 14;
        mcpApprovalFrame = juce::Rectangle<int>(bx, by, barW, barH);
        int const half = (barW - 24) / 2;
        mcpApprovalYes = juce::Rectangle<int>(bx + 8, by + 8, half, barH - 16);
        mcpApprovalNo = juce::Rectangle<int>(bx + 8 + half + 8, by + 8, half, barH - 16);

        NVGScopedState scopedAppr(nvg);
        nanovg::nvgTranslate(nvg, static_cast<float>(bx), static_cast<float>(by));
        nanovg::nvgDrawRoundedRect(nvg, 0, 0, static_cast<float>(barW), static_cast<float>(barH),
                           nanovg::nvgRGBA(28, 16, 16, 250), nanovg::nvgRGBA(255, 90, 70, 225), 10.0f);
        mcpnvg::nvgFontFace(nvg, "Inter");
        mcpnvg::nvgFontSize(nvg, 15.0f);
        mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        // Approve (green)
        nanovg::nvgDrawRoundedRect(nvg, 8, 8, static_cast<float>(half), static_cast<float>(barH - 16),
                           nanovg::nvgRGBA(40, 170, 90, 235), nanovg::nvgRGBA(90, 225, 140, 255), 6.0f);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 255, 255, 255));
        mcpnvg::nvgText(nvg, 8 + half * 0.5f, barH * 0.5f, "Approve", nullptr);
        // Cancel (grey)
        nanovg::nvgDrawRoundedRect(nvg, 8 + half + 8, 8, static_cast<float>(half), static_cast<float>(barH - 16),
                           nanovg::nvgRGBA(40, 44, 52, 235), nanovg::nvgRGBA(90, 98, 112, 255), 6.0f);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(210, 220, 235, 235));
        mcpnvg::nvgText(nvg, 8 + half + 8 + half * 0.5f, barH * 0.5f, "Cancel", nullptr);
    } else {
        mcpApprovalFrame = {};
        mcpApprovalYes = {};
        mcpApprovalNo = {};
    }
    // Multimodal Pen / Sketch Ink Overlay (Rendered directly on GPU in canvas coords)
    renderSketchOverlay(nvg);

    // Paste slot ghost: an empty dashed frame waiting for Ctrl+V. Explicit,
    // local, never auto-read from the clipboard.
    if (mcpImageSlotActive) {
        float const scx = static_cast<float>(mcpImageSlotPos.x);
        float const scy = static_cast<float>(mcpImageSlotPos.y);
        float const sx = scx - 160.0f;
        float const sy = scy - 110.0f;

        NVGScopedState scopedSlot(nvg);
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgRoundedRect(nvg, sx, sy, 320.0f, 220.0f, 8.0f);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(20, 24, 30, 150));
        nanovg::nvgFill(nvg);
        nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(124, 77, 255, 200));
        nanovg::nvgStrokeWidth(nvg, 1.5f);
        nanovg::nvgLineStyle(nvg, NVG_LINE_DASHED);
        nanovg::nvgDashLength(nvg, 7.0f);
        nanovg::nvgStroke(nvg);
        nanovg::nvgLineStyle(nvg, NVG_LINE_SOLID);

        mcpnvg::nvgFontFace(nvg, "Inter");
        mcpnvg::nvgFontSize(nvg, 13.0f);
        mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(198, 208, 226, 225));
        mcpnvg::nvgText(nvg, scx, scy - 8.0f, "Ctrl+V to paste image", nullptr);
        mcpnvg::nvgFontSize(nvg, 11.0f);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(150, 160, 180, 170));
        mcpnvg::nvgText(nvg, scx, scy + 12.0f, "(Esc cancels)", nullptr);
    }

    // PRD overlay: ghost/preview of PROPOSED (uncommitted) changes. Drawn above
    // the patch, never part of it — the artist sees the change before it's real.
    if (canvas.pd && shouldShowAIGhosts() && !canvas.pd->getMcp()->session.getMcpGhosts().empty()) {
        for (auto const& g : canvas.pd->getMcp()->session.getMcpGhosts()) {
            NVGScopedState scopedGhost(nvg);
            if (g.kind == 0) {
                float const gx = canvas.canvasOrigin.x + g.x;
                float const gy = canvas.canvasOrigin.y + g.y;
                float const gw = g.w > 0 ? g.w : 120.0f;
                float const gh = g.h > 0 ? g.h : 22.0f;
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgRoundedRect(nvg, gx, gy, gw, gh, getPlugDataLook(canvas).getObjectCornerRadius());
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(74, 158, 255, 55));
                nanovg::nvgFill(nvg);
                nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(74, 158, 255, 220));
                nanovg::nvgStrokeWidth(nvg, 1.5f);
                nanovg::nvgLineStyle(nvg, NVG_LINE_DASHED);
                nanovg::nvgDashLength(nvg, 6.0f);
                nanovg::nvgStroke(nvg);
                nanovg::nvgLineStyle(nvg, NVG_LINE_SOLID);
                if (g.label.isNotEmpty()) {
                    mcpnvg::nvgFontSize(nvg, 12.0f);
                    mcpnvg::nvgFontFace(nvg, "Inter");
                    mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                    nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(74, 158, 255, 255));
                    mcpnvg::nvgText(nvg, gx + 6.0f, gy + gh * 0.5f, g.label.toRawUTF8(), nullptr);
                }
            } else {
                float const x1 = canvas.canvasOrigin.x + g.x1, y1 = canvas.canvasOrigin.y + g.y1;
                float const x2 = canvas.canvasOrigin.x + g.x2, y2 = canvas.canvasOrigin.y + g.y2;
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgMoveTo(nvg, x1, y1);
                float const dx = std::abs(x2 - x1) * 0.5f + 20.0f;
                nanovg::nvgBezierTo(nvg, x1, y1 + dx, x2, y2 - dx, x2, y2);
                nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(74, 158, 255, 220));
                nanovg::nvgStrokeWidth(nvg, 2.0f);
                nanovg::nvgLineStyle(nvg, NVG_LINE_DASHED);
                nanovg::nvgDashLength(nvg, 6.0f);
                nanovg::nvgStroke(nvg);
                nanovg::nvgLineStyle(nvg, NVG_LINE_SOLID);
            }
        }
    }
}

void CanvasMcpLayer::paintViewportFrame(NVGcontext* nvg, float const zoom)
{
    // Studio ink mode: a subtle neon frame around the visible viewport so the
    // armed pen is unmistakable — even after you look away. (Crosshair alone is
    // easy to miss.) Screen-constant width via /zoom.
    if ((mcpSketchToolActive || mcpEraserToolActive) && canvas.viewport && !::getValue<bool>(canvas.presentationMode) && zoom > 0.0f) {
        float const vx = static_cast<float>(canvas.viewport->getViewPositionX()) / zoom;
        float const vy = static_cast<float>(canvas.viewport->getViewPositionY()) / zoom;
        float const vw = static_cast<float>(canvas.viewport->getWidth()) / zoom;
        float const vh = static_cast<float>(canvas.viewport->getHeight()) / zoom;
        float const inset = 2.0f / zoom;

        NVGScopedState scopedArmed(nvg);
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgRect(nvg, vx + inset, vy + inset, vw - inset * 2.0f, vh - inset * 2.0f);
        nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(0, 230, 255, 22));
        nanovg::nvgStrokeWidth(nvg, 9.0f / zoom);
        nanovg::nvgStroke(nvg);

        nanovg::nvgBeginPath(nvg);
        nanovg::nvgRect(nvg, vx + inset, vy + inset, vw - inset * 2.0f, vh - inset * 2.0f);
        nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(0, 230, 255, 115));
        nanovg::nvgStrokeWidth(nvg, 1.5f / zoom);
        nanovg::nvgStroke(nvg);
    }
}

void CanvasMcpLayer::paintScreenOverlay(NVGcontext* nvg)
{
    // Cinematic Viewport HUD & Subtitle Overlay (Video & Tutorial Mode)
    // Rendered in screen coordinates (unscaled/unpanned) — zero cord collisions.
    if (canvas.pd) {
        auto hud = canvas.pd->getMcp()->session.getMcpHud();
        if (hud.active && shouldShowAIHud()) {
            NVGScopedState scopedHud(nvg);
            float const vw = static_cast<float>(canvas.viewport ? canvas.viewport->getWidth() : canvas.getWidth());
            float const vh = static_cast<float>(canvas.viewport ? canvas.viewport->getHeight() : canvas.getHeight());

            bool const hasPrompt = hud.prompt.isNotEmpty();
            bool const hasReceipt = hud.receipt.isNotEmpty();
            bool const hasTool = hud.tool.isNotEmpty();
            bool const hasChapter = hud.chapter.isNotEmpty();

            // Card title (Chapter, action name, or tool)
            juce::String cardTitle;
            if (hasChapter) cardTitle = hud.chapter;
            else if (hasTool) cardTitle = hud.tool;

            // Direct action statement
            juce::String cardDesc;
            if (hasPrompt) cardDesc = hud.prompt;
            else if (hasReceipt) cardDesc = hud.receipt;

            // Sanitize description text: strip newlines and unprintable characters that cause tofu boxes
            juce::String cleanDesc = cardDesc.replaceCharacter('\r', ' ');
            while (cleanDesc.contains("\n\n"))
                cleanDesc = cleanDesc.replace("\n\n", " \xE2\x80\xA2 ");
            cleanDesc = cleanDesc.replaceCharacter('\n', ' ').trim();

            // Sizing: floating centered card with title-safe margins
            constexpr float maxCardW = 760.0f;
            float const cardW = jmin(vw - 48.0f, maxCardW);
            float const cardX = (vw - cardW) * 0.5f;
            float const innerPadX = 18.0f;
            float const breakWidth = cardW - (innerPadX * 2.0f);

            // Compute text height via nvgTextBoxBounds for multi-line wrapped text
            float descH = 0.0f;
            if (cleanDesc.isNotEmpty()) {
                mcpnvg::nvgFontSize(nvg, 14.5f);
                mcpnvg::nvgFontFace(nvg, "Inter");
                float bounds[4];
                mcpnvg::nvgTextBoxBounds(nvg, 0, 0, breakWidth, cleanDesc.toRawUTF8(), nullptr, bounds);
                descH = bounds[3] - bounds[1];
                descH = jlimit(18.0f, 85.0f, descH);
            }

            float cardH = 50.0f;
            if (cardTitle.isNotEmpty() && cleanDesc.isNotEmpty()) {
                cardH = 13.0f + 22.0f + 9.0f + descH + 14.0f;
            } else if (cleanDesc.isNotEmpty()) {
                cardH = 14.0f + descH + 14.0f;
            }
            float const cardY = vh - cardH - 34.0f;

            // Determine status accent color based on chapter
            juce::String const upperChapter = hud.chapter.toUpperCase();
            NVGcolor accentCol = nanovg::nvgRGBA(56, 189, 248, 255);    // Electric Cyan default (THINKING / WORKING / BUILDING)
            NVGcolor badgeBg = nanovg::nvgRGBA(56, 189, 248, 30);
            NVGcolor badgeBorder = nanovg::nvgRGBA(56, 189, 248, 110);

            if (upperChapter.contains("DONE") || upperChapter.contains("APPROVED") || upperChapter.contains("VOICE")) {
                accentCol = nanovg::nvgRGBA(16, 185, 129, 255);          // Emerald Green
                badgeBg = nanovg::nvgRGBA(16, 185, 129, 32);
                badgeBorder = nanovg::nvgRGBA(16, 185, 129, 120);
            } else if (upperChapter.contains("APPROVE")) {
                accentCol = nanovg::nvgRGBA(245, 158, 11, 255);          // Amber Gold
                badgeBg = nanovg::nvgRGBA(245, 158, 11, 40);
                badgeBorder = nanovg::nvgRGBA(245, 158, 11, 140);
            } else if (upperChapter.contains("ERROR") || upperChapter.contains("DENIED") || upperChapter.contains("FAIL")) {
                accentCol = nanovg::nvgRGBA(244, 63, 94, 255);           // Coral Red
                badgeBg = nanovg::nvgRGBA(244, 63, 94, 40);
                badgeBorder = nanovg::nvgRGBA(244, 63, 94, 140);
            }

            // 1. Soft Ambient Box-Gradient Shadow
            NVGpaint shadowPaint = nanovg::nvgBoxGradient(nvg, cardX, cardY + 5.0f, cardW, cardH, 14.0f, 24.0f, nanovg::nvgRGBA(0, 0, 0, 160), nanovg::nvgRGBA(0, 0, 0, 0));
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRect(nvg, cardX - 20.0f, cardY - 12.0f, cardW + 40.0f, cardH + 44.0f);
            nanovg::nvgFillPaint(nvg, shadowPaint);
            nanovg::nvgFill(nvg);

            // 2. Obsidian Glass Body with subtle linear gradient
            NVGpaint bodyPaint = nanovg::nvgLinearGradient(nvg, cardX, cardY, cardX, cardY + cardH,
                                                   nanovg::nvgRGBA(18, 22, 32, 246),
                                                   nanovg::nvgRGBA(10, 12, 17, 252));
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, cardX, cardY, cardW, cardH, 13.0f);
            nanovg::nvgFillPaint(nvg, bodyPaint);
            nanovg::nvgFill(nvg);

            // 3. Subtle Frosted Border
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, cardX, cardY, cardW, cardH, 13.0f);
            nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(255, 255, 255, 30));
            nanovg::nvgStrokeWidth(nvg, 1.0f);
            nanovg::nvgStroke(nvg);

            // 4. Specular Top Highlight
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, cardX + 1.5f, cardY + 1.0f, cardW - 3.0f, 1.5f, 1.0f);
            nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 255, 255, 45));
            nanovg::nvgFill(nvg);

            // 5. Header Row (LED Capsule Badge + Tool Tag + Meta)
            float const headerY = cardY + 13.0f;
            float nextHeaderX = cardX + innerPadX;

            if (cardTitle.isNotEmpty()) {
                mcpnvg::nvgFontSize(nvg, 11.5f);
                mcpnvg::nvgFontFace(nvg, "Inter-Bold");
                float titleBounds[4];
                mcpnvg::nvgTextBounds(nvg, 0, 0, cardTitle.toRawUTF8(), nullptr, titleBounds);
                float const pillTextW = titleBounds[2] - titleBounds[0];
                float const pillH = 22.0f;
                float const pillW = pillTextW + 28.0f;

                // Pill Background
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgRoundedRect(nvg, nextHeaderX, headerY, pillW, pillH, 11.0f);
                nanovg::nvgFillColor(nvg, badgeBg);
                nanovg::nvgFill(nvg);
                nanovg::nvgStrokeColor(nvg, badgeBorder);
                nanovg::nvgStrokeWidth(nvg, 1.0f);
                nanovg::nvgStroke(nvg);

                // Glowing LED Dot
                float const ledX = nextHeaderX + 10.0f;
                float const ledY = headerY + pillH * 0.5f;
                NVGpaint ledGlow = nanovg::nvgRadialGradient(nvg, ledX, ledY, 1.5f, 6.5f, accentCol, nanovg::nvgRGBA(0, 0, 0, 0));
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgCircle(nvg, ledX, ledY, 6.5f);
                nanovg::nvgFillPaint(nvg, ledGlow);
                nanovg::nvgFill(nvg);

                nanovg::nvgBeginPath(nvg);
                nanovg::nvgCircle(nvg, ledX, ledY, 3.0f);
                nanovg::nvgFillColor(nvg, accentCol);
                nanovg::nvgFill(nvg);

                // Title Text inside Pill
                mcpnvg::nvgFontSize(nvg, 11.0f);
                mcpnvg::nvgFontFace(nvg, "Inter-Bold");
                mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(248, 250, 252, 255));
                mcpnvg::nvgText(nvg, nextHeaderX + 18.0f, headerY + pillH * 0.5f + 0.5f, cardTitle.toRawUTF8(), nullptr);

                nextHeaderX += pillW + 10.0f;
            }

            // Secondary Tool Tag
            if (hasTool && hud.tool != cardTitle) {
                mcpnvg::nvgFontSize(nvg, 12.0f);
                mcpnvg::nvgFontFace(nvg, "Inter");
                mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(148, 163, 184, 210));
                juce::String const toolStr = "// " + hud.tool;
                mcpnvg::nvgText(nvg, nextHeaderX, headerY + 11.0f, toolStr.toRawUTF8(), nullptr);
            }

            // Right-aligned Telemetry / Latency
            juce::String const metaRight = hud.latency.isNotEmpty() ? hud.latency : hud.telemetry;
            if (metaRight.isNotEmpty()) {
                mcpnvg::nvgFontSize(nvg, 11.5f);
                mcpnvg::nvgFontFace(nvg, "Inter-Tabular");
                mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(100, 116, 139, 230));
                mcpnvg::nvgText(nvg, cardX + cardW - innerPadX, headerY + 11.0f, metaRight.toRawUTF8(), nullptr);
            }

            // 6. Direct Description with Word Wrapping (nvgTextBox)
            if (cleanDesc.isNotEmpty()) {
                float const descY = cardTitle.isNotEmpty() ? (headerY + 22.0f + 8.0f) : (cardY + 13.0f);
                mcpnvg::nvgFontSize(nvg, 14.5f);
                mcpnvg::nvgFontFace(nvg, "Inter");
                mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(226, 232, 240, 245));
                mcpnvg::nvgTextBox(nvg, cardX + innerPadX, descY, breakWidth, cleanDesc.toRawUTF8(), nullptr);
            }
        }
    }
}

bool CanvasMcpLayer::decorateObject(NVGcontext* nvg, Object& object, juce::Rectangle<int> b)
{
    if (!shouldShowAIState())
        return false;
    // PRD overlay: AI state marker drawn per-object (1=changed, 2=proposed, 3=error).
    int const aiState = canvas.pd->getMcp()->session.getAiOverlayState(object.getPointer());
    if (aiState != 0) {
        int const st = aiState;
        NVGcolor col = (st == 1) ? nanovg::nvgRGB(0x3d, 0xdd, 0x8a)
                    : (st == 2) ? nanovg::nvgRGB(0x4a, 0x9e, 0xff)
                                : nanovg::nvgRGB(0xff, 0x5a, 0x5a);
        NVGScopedState scopedAIState(nvg);
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgStrokeColor(nvg, col);
        nanovg::nvgStrokeWidth(nvg, 2.0f);
        nanovg::nvgRoundedRect(nvg, b.getX(), b.getY(), b.getWidth(), b.getHeight(), getPlugDataLook(object).getObjectCornerRadius());
        nanovg::nvgStroke(nvg);

        // state dot, top-right
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgFillColor(nvg, col);
        nanovg::nvgCircle(nvg, b.getRight() - 3.0f, b.getY() + 3.0f, 4.0f);
        nanovg::nvgFill(nvg);
    }
    return true;
}

bool CanvasMcpLayer::shouldShowAIState() const
{
    return showAiState && !canvas.presentationMode.getValue();
}

bool CanvasMcpLayer::shouldShowAIRegions() const
{
    if (canvas.presentationMode.getValue()) return false;
    if (canvas.pd && !canvas.pd->getMcp()->session.getMcpRegions().empty()) return true;
    return showAiRegions;
}

bool CanvasMcpLayer::shouldShowAIAnnotations() const
{
    if (canvas.presentationMode.getValue()) return false;
    if (canvas.pd && !canvas.pd->getMcp()->session.getMcpAnnotations().empty()) return true;
    return showAiAnnotations;
}

bool CanvasMcpLayer::shouldShowAIGhosts() const
{
    return showAiGhosts && !canvas.presentationMode.getValue();
}

bool CanvasMcpLayer::shouldShowAIHud() const
{
    return showAiHud && !canvas.presentationMode.getValue();
}

bool CanvasMcpLayer::shouldShowAISketch() const
{
    return false; // Canvas pen is disabled — canvas stays clean Pure Data
}

bool CanvasMcpLayer::shouldShowAIReferences() const
{
    if (canvas.presentationMode.getValue() || canvas.isGraph) return false;
    return showAiReferences;
}

void CanvasMcpLayer::renderSketchOverlay(NVGcontext* nvg)
{
    if (!shouldShowAISketch()) return;

    auto drawStroke = [&](const std::vector<SketchPoint>& stroke, float alpha) {
        if (stroke.size() < 2) return;

        // 1. Outer Glow (Electric Cyan, soft wide stroke)
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgMoveTo(nvg, stroke[0].x, stroke[0].y);
        for (size_t i = 1; i < stroke.size(); ++i) {
            nanovg::nvgLineTo(nvg, stroke[i].x, stroke[i].y);
        }
        nanovg::nvgLineCap(nvg, NVG_ROUND);
        nanovg::nvgLineJoin(nvg, NVG_ROUND);
        nanovg::nvgStrokeWidth(nvg, 7.0f);
        nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(0, 230, 255, static_cast<unsigned char>(alpha * 120.0f)));
        nanovg::nvgStroke(nvg);

        // 2. Cyan Body
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgMoveTo(nvg, stroke[0].x, stroke[0].y);
        for (size_t i = 1; i < stroke.size(); ++i) {
            nanovg::nvgLineTo(nvg, stroke[i].x, stroke[i].y);
        }
        nanovg::nvgLineCap(nvg, NVG_ROUND);
        nanovg::nvgLineJoin(nvg, NVG_ROUND);
        nanovg::nvgStrokeWidth(nvg, 3.5f);
        nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(0, 245, 255, static_cast<unsigned char>(alpha * 220.0f)));
        nanovg::nvgStroke(nvg);

        // 3. Hot White Center Core
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgMoveTo(nvg, stroke[0].x, stroke[0].y);
        for (size_t i = 1; i < stroke.size(); ++i) {
            nanovg::nvgLineTo(nvg, stroke[i].x, stroke[i].y);
        }
        nanovg::nvgLineCap(nvg, NVG_ROUND);
        nanovg::nvgLineJoin(nvg, NVG_ROUND);
        nanovg::nvgStrokeWidth(nvg, 1.5f);
        nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(255, 255, 255, static_cast<unsigned char>(alpha * 240.0f)));
        nanovg::nvgStroke(nvg);
    };

    if (mcpSketchAlpha > 0.001f && mcpStudioInkVisible) {
        for (auto const& s : mcpCompletedStrokes) {
            drawStroke(s, mcpSketchAlpha);
        }
    }

    if (isSketching && !mcpCurrentStroke.empty()) {
        drawStroke(mcpCurrentStroke, 1.0f);
    }
}

void CanvasMcpLayer::renderReferenceImages(NVGcontext* nvg, Rectangle<int> invalidRegion)
{
    if (!canvas.pd || !shouldShowAIReferences()) return; // 👁 layer toggle + performance mode
    auto const images = canvas.pd->getMcp()->session.getMcpReferenceImages();
    if (images.empty()) return;

    for (auto const& img : images) {
        if (!img.visible) continue; // Visible-Only Ingestion Law

        // Decode with JUCE and upload as ARGB, like PictureObject. The old
        // ::nvgCreateImage(filename) path is not allowed any more: with the async
        // recorder only the render thread may touch the real GPU context.
        auto& entry = mcpRefImageCache.try_emplace(img.id.toStdString()).first->second;
        if (!entry.texture.isValid() || entry.ctx != nvg) {
            auto const decoded = juce::ImageFileFormat::loadFrom(juce::File(img.filePath));
            if (decoded.isValid())
                entry.texture.loadJUCEImage(nvg, decoded.convertedToFormat(juce::Image::ARGB));
            entry.ctx = nvg;
        }
        if (!entry.texture.isValid()) continue;
        int const imageHandle = entry.texture.getImageId();

        Rectangle<float> const b(static_cast<float>(canvas.canvasOrigin.x) + img.x,
                                 static_cast<float>(canvas.canvasOrigin.y) + img.y,
                                 img.width, img.height);
        if (!invalidRegion.intersects(b.getSmallestIntegerContainer())) continue;

        NVGScopedState scopedImage(nvg);

        // Light paper matte behind the image — dark screenshots on a dark canvas
        // would otherwise blend into the background and look "missing".
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgRoundedRect(nvg, b.getX() - 6.0f, b.getY() - 6.0f, b.getWidth() + 12.0f, b.getHeight() + 12.0f, 9.0f);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(228, 230, 236, 240));
        nanovg::nvgFill(nvg);

        // The image itself — straight through nvgImagePattern.
        {
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, b.getX(), b.getY(), b.getWidth(), b.getHeight(), 4.0f);
            nanovg::nvgFillPaint(nvg, nanovg::nvgImagePattern(nvg, b.getX(), b.getY(), b.getWidth(), b.getHeight(), 0.0f, imageHandle, img.alpha));
            nanovg::nvgFill(nvg);
        }

        // Hardware frame: cool slate normally, warm amber when locked
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgRoundedRect(nvg, b.getX() - 1.0f, b.getY() - 1.0f, b.getWidth() + 2.0f, b.getHeight() + 2.0f, 5.0f);
        nanovg::nvgStrokeColor(nvg, img.isLocked ? nanovg::nvgRGBA(255, 190, 80, 210) : nanovg::nvgRGBA(150, 165, 190, 140));
        nanovg::nvgStrokeWidth(nvg, img.isLocked ? 1.6f : 1.2f);
        nanovg::nvgStroke(nvg);

        // Title tag above the image (+ lock glyph)
        juce::String const tag = img.title.isNotEmpty() ? img.title : juce::File(img.filePath).getFileName();
        mcpnvg::nvgFontFace(nvg, "Inter");
        mcpnvg::nvgFontSize(nvg, 10.0f);
        mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        float textBounds[4];
        mcpnvg::nvgTextBounds(nvg, 0.0f, 0.0f, tag.toRawUTF8(), nullptr, textBounds);
        float const tagW = (textBounds[2] - textBounds[0]) + (img.isLocked ? 34.0f : 18.0f);
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgRoundedRect(nvg, b.getX(), b.getY() - 20.0f, tagW, 16.0f, 3.5f);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(16, 18, 22, 225));
        nanovg::nvgFill(nvg);
        nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(120, 135, 160, 120));
        nanovg::nvgStrokeWidth(nvg, 1.0f);
        nanovg::nvgStroke(nvg);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(205, 216, 232, 235));
        mcpnvg::nvgText(nvg, b.getX() + 8.0f, b.getY() - 12.0f, tag.toRawUTF8(), nullptr);

        if (img.isLocked) {
            float const lx = b.getX() + tagW - 12.0f;
            float const ly = b.getY() - 11.5f;
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, lx - 3.5f, ly - 0.5f, 7.0f, 5.5f, 1.0f);
            nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 190, 80, 235));
            nanovg::nvgFill(nvg);
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgArc(nvg, lx, ly - 2.0f, 2.6f, juce::MathConstants<float>::pi, 0.0f, NVG_HOLE);
            nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(255, 190, 80, 235));
            nanovg::nvgStrokeWidth(nvg, 1.2f);
            nanovg::nvgStroke(nvg);
        }
    }
}

// Phase 3 — Voice take chips: ▶ audition, mini-waveform, duration, ✕ remove.
void CanvasMcpLayer::renderVoiceTakes(NVGcontext* nvg, Rectangle<int> invalidRegion)
{
    if (!canvas.pd || canvas.presentationMode.getValue()) return;
    auto takes = canvas.pd->getMcp()->session.getMcpVoiceTakes();
    if (takes.empty()) return;

    auto* br = canvas.pd->getMcp()->getMCPBridge();
    juce::String const playingId = br ? br->getPlayingVoiceTakeId() : juce::String();

    constexpr float chipW = 240.0f;
    constexpr float chipH = 38.0f;
    int slot = 0;

    for (auto& take : takes) {
        if (!take.visible) continue;

        if (!take.positioned) {
            // First sight: park near where the record strip was (or canvas centre).
            float const px = mcpTakeSpawnPos.x != 0 ? static_cast<float>(mcpTakeSpawnPos.x)
                                                    : static_cast<float>(canvas.canvasOrigin.x + 40);
            float const py = mcpTakeSpawnPos.y != 0 ? static_cast<float>(mcpTakeSpawnPos.y)
                                                    : static_cast<float>(canvas.canvasOrigin.y + 40);
            canvas.pd->getMcp()->session.setMcpVoiceTakePos(take.id, px - static_cast<float>(canvas.canvasOrigin.x),
                                   py - static_cast<float>(canvas.canvasOrigin.y) + 46.0f * static_cast<float>(slot));
            take.x = px - static_cast<float>(canvas.canvasOrigin.x);
            take.y = py - static_cast<float>(canvas.canvasOrigin.y) + 46.0f * static_cast<float>(slot);
            take.positioned = true;
        }
        ++slot;

        Rectangle<float> const b(static_cast<float>(canvas.canvasOrigin.x) + take.x,
                                 static_cast<float>(canvas.canvasOrigin.y) + take.y,
                                 chipW, chipH);
        if (!invalidRegion.intersects(b.getSmallestIntegerContainer())) continue;

        bool const playing = playingId == take.id;
        bool const melody = take.mode.startsWithIgnoreCase("melody");
        NVGcolor const accent = melody ? nanovg::nvgRGBA(255, 190, 60, playing ? 255 : 210)
                                       : nanovg::nvgRGBA(80, 220, 140, playing ? 255 : 210);

        NVGScopedState scopedChip(nvg);

        // Cassette body: deep slate + per-type accent border (amber = melody, green = beat)
        nanovg::nvgDrawRoundedRect(nvg, b.getX(), b.getY(), b.getWidth(), b.getHeight(),
                           nanovg::nvgRGBA(18, 22, 28, 242), accent, 6.0f);

        // Type glyph: beamed notes (melody) or drum (beat) — the strip's language
        {
            float const gx = b.getX() + 18.0f;
            float const gy = b.getCentreY();
            nanovg::nvgStrokeColor(nvg, accent);
            nanovg::nvgFillColor(nvg, accent);
            if (melody) {
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgEllipse(nvg, gx - 3.0f, gy + 3.5f, 2.4f, 1.7f);
                nanovg::nvgFill(nvg);
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgEllipse(nvg, gx + 3.2f, gy + 2.2f, 2.4f, 1.7f);
                nanovg::nvgFill(nvg);
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgMoveTo(nvg, gx - 0.7f, gy + 3.4f);
                nanovg::nvgLineTo(nvg, gx - 0.7f, gy - 4.5f);
                nanovg::nvgLineTo(nvg, gx + 5.6f, gy - 5.7f);
                nanovg::nvgLineTo(nvg, gx + 5.6f, gy + 2.1f);
                nanovg::nvgStrokeWidth(nvg, 1.3f);
                nanovg::nvgStroke(nvg);
            } else {
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgRoundedRect(nvg, gx - 5.0f, gy - 0.5f, 10.0f, 5.5f, 1.7f);
                nanovg::nvgStrokeWidth(nvg, 1.1f);
                nanovg::nvgStroke(nvg);
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgEllipse(nvg, gx, gy - 0.5f, 5.0f, 1.7f);
                nanovg::nvgStrokeWidth(nvg, 1.1f);
                nanovg::nvgStroke(nvg);
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgMoveTo(nvg, gx - 3.8f, gy - 6.0f);
                nanovg::nvgLineTo(nvg, gx + 0.8f, gy - 1.2f);
                nanovg::nvgMoveTo(nvg, gx + 3.8f, gy - 6.0f);
                nanovg::nvgLineTo(nvg, gx - 0.8f, gy - 1.2f);
                nanovg::nvgStrokeWidth(nvg, 1.0f);
                nanovg::nvgStroke(nvg);
            }
        }

        // ▶ / ⏸ (playing) — pause bars while the take is auditioning
        float const pcx = b.getX() + 46.0f;
        float const pcy = b.getCentreY();
        if (playing) {
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, pcx - 4.5f, pcy - 5.0f, 3.4f, 10.0f, 1.2f);
            nanovg::nvgFillColor(nvg, accent);
            nanovg::nvgFill(nvg);
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, pcx + 1.1f, pcy - 5.0f, 3.4f, 10.0f, 1.2f);
            nanovg::nvgFillColor(nvg, accent);
            nanovg::nvgFill(nvg);
        } else {
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgMoveTo(nvg, pcx - 4.5f, pcy - 6.5f);
            nanovg::nvgLineTo(nvg, pcx + 6.5f, pcy);
            nanovg::nvgLineTo(nvg, pcx - 4.5f, pcy + 6.5f);
            nanovg::nvgClosePath(nvg);
            nanovg::nvgFillColor(nvg, accent);
            nanovg::nvgFill(nvg);
        }

        // Mini-waveform (64 peak buckets)
        float const wfX = b.getX() + 62.0f;
        float const wfW = b.getWidth() - 62.0f - 72.0f;
        float const wfCy = pcy;
        constexpr float wfMaxH = 22.0f;
        if (take.peaks.size() >= 2) {
            float const barW = wfW / static_cast<float>(take.peaks.size());
            nanovg::nvgBeginPath(nvg);
            for (size_t k = 0; k < take.peaks.size(); ++k) {
                float const h = juce::jmax(1.5f, juce::jlimit(0.0f, 1.0f, take.peaks[k]) * wfMaxH);
                nanovg::nvgRect(nvg, wfX + static_cast<float>(k) * barW, wfCy - h * 0.5f, juce::jmax(0.8f, barW - 0.4f), h);
            }
            nanovg::nvgFillColor(nvg, accent);
            nanovg::nvgFill(nvg);
        } else {
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgMoveTo(nvg, wfX, wfCy);
            nanovg::nvgLineTo(nvg, wfX + wfW, wfCy);
            nanovg::nvgStrokeColor(nvg, accent);
            nanovg::nvgStrokeWidth(nvg, 1.0f);
            nanovg::nvgStroke(nvg);
        }

        // Duration m:ss
        int const totalSec = juce::roundToInt(take.durationSec);
        juce::String const dur = juce::String(totalSec / 60) + ":" + juce::String(totalSec % 60).paddedLeft('0', 2);
        mcpnvg::nvgFontFace(nvg, "Inter");
        mcpnvg::nvgFontSize(nvg, 10.0f);
        mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(230, 220, 200, 220));
        mcpnvg::nvgText(nvg, b.getX() + b.getWidth() - 66.0f, pcy, dur.toRawUTF8(), nullptr);

        // ✕ remove
        float const xx = b.getRight() - 14.0f;
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgMoveTo(nvg, xx - 4.0f, pcy - 4.0f);
        nanovg::nvgLineTo(nvg, xx + 4.0f, pcy + 4.0f);
        nanovg::nvgMoveTo(nvg, xx + 4.0f, pcy - 4.0f);
        nanovg::nvgLineTo(nvg, xx - 4.0f, pcy + 4.0f);
        nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(220, 150, 150, 230));
        nanovg::nvgStrokeWidth(nvg, 1.4f);
        nanovg::nvgStroke(nvg);
    }
}

void CanvasMcpLayer::renderMidiTakes(NVGcontext* nvg, Rectangle<int> invalidRegion)
{
    if (!canvas.pd || canvas.presentationMode.getValue()) return;
    auto takes = canvas.pd->getMcp()->session.getMcpMidiTakes();
    if (takes.empty()) return;

    auto* br = canvas.pd->getMcp()->getMCPBridge();
    juce::String const playingId = br ? br->getPlayingMidiTakeId() : juce::String();

    constexpr float chipW = 240.0f;
    constexpr float chipH = 38.0f;
    int slot = 0;

    for (auto& take : takes) {
        if (!take.visible) continue;

        if (!take.positioned) {
            float const px = mcpTakeSpawnPos.x != 0 ? static_cast<float>(mcpTakeSpawnPos.x)
                                                    : static_cast<float>(canvas.canvasOrigin.x + 40);
            float const py = mcpTakeSpawnPos.y != 0 ? static_cast<float>(mcpTakeSpawnPos.y)
                                                    : static_cast<float>(canvas.canvasOrigin.y + 40);
            canvas.pd->getMcp()->session.setMcpMidiTakePos(take.id, px - static_cast<float>(canvas.canvasOrigin.x),
                                  py - static_cast<float>(canvas.canvasOrigin.y) + 46.0f * static_cast<float>(slot));
            take.x = px - static_cast<float>(canvas.canvasOrigin.x);
            take.y = py - static_cast<float>(canvas.canvasOrigin.y) + 46.0f * static_cast<float>(slot);
            take.positioned = true;
        }
        ++slot;

        Rectangle<float> const b(static_cast<float>(canvas.canvasOrigin.x) + take.x,
                                 static_cast<float>(canvas.canvasOrigin.y) + take.y,
                                 chipW, chipH);
        if (!invalidRegion.intersects(b.getSmallestIntegerContainer())) continue;

        bool const playing = (playingId == take.id);
        NVGcolor const accent = nanovg::nvgRGBA(180, 90, 255, playing ? 255 : 220); // synth royal purple (bright glow while playing)

        NVGScopedState scopedChip(nvg);

        // Cassette body: deep slate + royal purple border
        nanovg::nvgDrawRoundedRect(nvg, b.getX(), b.getY(), b.getWidth(), b.getHeight(),
                           nanovg::nvgRGBA(20, 18, 28, 245), accent, 6.0f);

        // 🎹 Piano keyboard glyph (at x = 16)
        {
            float const gx = b.getX() + 16.0f;
            float const gy = b.getCentreY();
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, gx - 7.0f, gy - 6.0f, 14.0f, 12.0f, 1.5f);
            nanovg::nvgStrokeColor(nvg, accent);
            nanovg::nvgStrokeWidth(nvg, 1.2f);
            nanovg::nvgStroke(nvg);
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgMoveTo(nvg, gx - 2.5f, gy - 6.0f);
            nanovg::nvgLineTo(nvg, gx - 2.5f, gy + 6.0f);
            nanovg::nvgMoveTo(nvg, gx + 2.0f, gy - 6.0f);
            nanovg::nvgLineTo(nvg, gx + 2.0f, gy + 6.0f);
            nanovg::nvgStrokeColor(nvg, accent);
            nanovg::nvgStrokeWidth(nvg, 1.0f);
            nanovg::nvgStroke(nvg);
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRect(nvg, gx - 4.5f, gy - 6.0f, 1.8f, 6.5f);
            nanovg::nvgRect(nvg, gx + 0.2f, gy - 6.0f, 1.8f, 6.5f);
            nanovg::nvgFillColor(nvg, accent);
            nanovg::nvgFill(nvg);
        }

        // ▶ / ⏸ Audition toggle button (at x = 38)
        float const pcx = b.getX() + 38.0f;
        float const pcy = b.getCentreY();
        if (playing) {
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, pcx - 4.5f, pcy - 5.0f, 3.4f, 10.0f, 1.2f);
            nanovg::nvgFillColor(nvg, accent);
            nanovg::nvgFill(nvg);
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, pcx + 1.1f, pcy - 5.0f, 3.4f, 10.0f, 1.2f);
            nanovg::nvgFillColor(nvg, accent);
            nanovg::nvgFill(nvg);
        } else {
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgMoveTo(nvg, pcx - 4.5f, pcy - 6.0f);
            nanovg::nvgLineTo(nvg, pcx + 6.0f, pcy);
            nanovg::nvgLineTo(nvg, pcx - 4.5f, pcy + 6.0f);
            nanovg::nvgClosePath(nvg);
            nanovg::nvgFillColor(nvg, accent);
            nanovg::nvgFill(nvg);
        }

        // Title and notes (at x = 54)
        float const tx = b.getX() + 54.0f;
        float const ty = b.getY() + 14.0f;
        mcpnvg::nvgFontFace(nvg, "Inter-Medium");
        mcpnvg::nvgFontSize(nvg, 11.5f);
        mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(235, 225, 250, 240));
        juce::String const title = take.id + " · " + juce::String(take.noteCount) + " notes";
        mcpnvg::nvgText(nvg, tx, ty, title.toRawUTF8(), nullptr);

        // Subtitle: voice binding + duration (at x = 54)
        float const subY = b.getY() + 27.0f;
        mcpnvg::nvgFontFace(nvg, "Inter");
        mcpnvg::nvgFontSize(nvg, 9.5f);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(180, 165, 205, 200));
        juce::String const sub = "voice: " + take.voice + " · " + juce::String(take.durationSec, 1) + "s";
        mcpnvg::nvgText(nvg, tx, subY, sub.toRawUTF8(), nullptr);

        // ✕ Remove button on the far right
        float const xx = b.getRight() - 14.0f;
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgMoveTo(nvg, xx - 4.0f, pcy - 4.0f);
        nanovg::nvgLineTo(nvg, xx + 4.0f, pcy + 4.0f);
        nanovg::nvgMoveTo(nvg, xx + 4.0f, pcy - 4.0f);
        nanovg::nvgLineTo(nvg, xx - 4.0f, pcy + 4.0f);
        nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(200, 160, 220, 210));
        nanovg::nvgStrokeWidth(nvg, 1.3f);
        nanovg::nvgStroke(nvg);
    }
}

void CanvasMcpLayer::setOverlayMask(int mask)
{
    if (auto overlays = SettingsFile::getInstance()->getProperty<DynamicObject>("overlays")) {
        auto const key = (canvas.locked.getValue() || canvas.commandLocked.getValue()) ? "lock" : "edit";
        overlays->setProperty(key, mask);
        SettingsFile::getInstance()->triggerSettingsChange("overlays");
    }
    canvas.updateOverlays();
}

bool CanvasMcpLayer::hasOverlayTargets() const
{
    return canvas.pd && canvas.pd->getMcp()->session.hasMcpOverlayTargets();
}

bool CanvasMcpLayer::isTargeted(Object const* obj) const
{
    if (!canvas.pd || !canvas.pd->getMcp()->session.hasMcpOverlayTargets()) return true;
    if (!obj) return false;
    return canvas.pd->getMcp()->session.isMcpTargeted(obj->getPointer());
}

bool CanvasMcpLayer::isTargeted(Connection const* c) const
{
    if (!canvas.pd || !canvas.pd->getMcp()->session.hasMcpOverlayTargets()) return true;
    if (!c) return false;
    t_gobj* const outPtr = c->outobj ? c->outobj->getPointer() : nullptr;
    t_gobj* const inPtr = c->inobj ? c->inobj->getPointer() : nullptr;
    return canvas.pd->getMcp()->session.isMcpConnectionTargeted(outPtr, inPtr);
}

void CanvasMcpLayer::updateOverlays(int const overlayState)
{
    showAiState = overlayState & AIState;
    showAiRegions = overlayState & AIRegions;
    showAiAnnotations = overlayState & AIAnnotations;
    showAiGhosts = overlayState & AIGhosts;
    showAiHud = overlayState & AIHud;
    showAiSketch = overlayState & AISketch;
    showAiReferences = overlayState & AIReferences;
}
