/*
 // The MCP layer of one Canvas (Plan 3 P3.2 S7). Moved verbatim out of Source/Canvas.cpp
 // (performRender blocks, the AI overlay helpers) and Source/Object.cpp (the AI state
 // mark); implicit Canvas members became `canvas.` accesses. See CanvasMcpLayer.h.
 */
#include <algorithm>
#include <cstdint>
#include <optional>
#include <tuple>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "Utility/Config.h"
#include "Utility/Fonts.h"
#include "Utility/NVGTextShim.h"

#include "Mcp/CanvasMcpLayer.h"
#include "Mcp/CanvasMcpAiDraw.h"
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
    mcpAlphiMouseTracker = std::make_unique<AlphiMouseTracker>(*this);
    // The pill's answer line reads the bridge's chat lines after the sidebar (one more reader, keyed by this layer).
    if (canvas.pd) {
        if (auto* bridge = canvas.pd->getMcp()->getMCPBridge())
            bridge->addChatListener(this, [this](juce::String const& role, juce::String const& text) { onChatLine(role, text); });
    }
}

CanvasMcpLayer::~CanvasMcpLayer()
{
    // Canvas::~Canvas resets the layer while the processor and the bridge are alive (the editors go first, McpHost.cpp).
    if (canvas.pd) {
        if (auto* bridge = canvas.pd->getMcp()->getMCPBridge())
            bridge->removeChatListener(this);
    }
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
    // A graph-on-parent box renders its inner canvas through this same path (every GOP module, every frame). The
    // AI overlays belong to the canvas's own view, never to the box drawn inside another one: skip them there.
    if (canvas.isGraph)
        return;
    // Big Alphi 1a0 review H2: what this render redraws (canvas coordinates), for Alphi's move and click gate.
    mcpAlphiRenderRegion = invalidRegion;
    mcpPaintRegion = invalidRegion; // this render's region for paintOverObjects' culling (never consumed)
    // Alphi drop 1 D11: with no free spot on the right edge his tab is drawn under the boxes (CanvasMcpAlphiSay.cpp).
    if (mcpAlphiTabUnder && !mcpAlphiTabFrameUnder.isEmpty())
        paintAlphiTab(nvg);
    // PRD overlay: titled regions drawn BEHIND the objects (structure at a glance). Owner 2026-11-20, "a combination": a
    // see-through colour per kind (source green, fx blue, loop amber, output red, group violet grey), no outline, with the
    // 3 px ink bar down the left edge and the name in ink at 85 % (12 px Inter-Medium, never under 11 screen px when zoomed
    // out; at the top right when a wire or a step disc would cover it on the left). The rectangles come from mcp::aiRegionRects (padded,
    // never crossing, never cutting a member box, stacked ones share the squeeze evenly and keep 8 px under the upper one's boxes).
    if (canvas.pd && shouldShowAIRegions() && !canvas.pd->getMcp()->session.getMcpRegions().empty()) {
        auto const regions = canvas.pd->getMcp()->session.getMcpRegions();
        // Every region that holds boxes: the bounding box of its live members (all deleted -> auto-prune); a region with
        // no targets keeps the rectangle it was given. Pd comments are left out of the bounds (a note lies beside the boxes it
        // describes and stretched the rectangle to itself); a region of nothing but comments keeps them.
        auto const boundsOfMembers = [this](juce::StringArray const& ids, bool withComments, juce::Rectangle<int>& out) {
            bool any = false;
            for (auto const& tid : ids) {
                auto* obj = findObjectByStableId(tid);
                if (!obj || (!withComments && obj->getType(false) == "comment"))
                    continue;
                out = any ? out.getUnion(obj->getObjectBounds()) : obj->getObjectBounds();
                any = true;
            }
            return any;
        };
        std::vector<mcp::ApprovalRect> members;
        std::vector<size_t> memberOf; // members[k] belongs to regions[memberOf[k]]
        for (size_t ri = 0; ri < regions.size(); ++ri) {
            auto const& r = regions[ri];
            if (r.targetIds.size() == 0)
                continue;
            juce::Rectangle<int> bounds;
            if (!boundsOfMembers(r.targetIds, false, bounds) && !boundsOfMembers(r.targetIds, true, bounds))
                continue;
            members.push_back({ bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight() });
            memberOf.push_back(ri);
        }
        auto const rects = mcp::aiRegionRects(members);

        float const zoom = canvas.viewport ? canvas.viewport->getViewScale() : getValue<float>(canvas.zoomScale);
        float const titleSize = mcp::aiRegionTitleSizeFor(zoom);
        auto const& steps = spotlightSteps(); // the order discs a name may have to step aside for (empty without a spotlight; cached)

        // What a name's side depends on besides its own rectangle: the wires and the discs. One pass over the wires, made at most
        // once per render and only when a name that can move is drawn (review 2026-11-19: the side used to be worked out on every
        // paint by measuring every wire in 4 px steps).
        std::optional<std::uint64_t> sceneKey;
        auto const sceneKeyOnce = [&]() {
            if (!sceneKey) {
                auto h = wiresKey();
                for (auto const& step : steps) {
                    h = mcp::aiHashMix(h, step.centre.x);
                    h = mcp::aiHashMix(h, step.centre.y);
                    h = mcp::aiHashMix(h, step.number);
                }
                sceneKey = h;
            }
            return *sceneKey;
        };
        std::vector<juce::Rectangle<int>> flippedBands; // the bands of the names that changed sides in this render

        auto const drawRegion = [&](std::size_t slot, float rx, float ry, float rw, float rh, juce::String const& title, juce::String const& kind) {
            std::string const kindWord = kind.toStdString();
            // Outside what this render redraws: nothing of it would land (its title may run past rw, and grows when zoomed out).
            if (!juce::Rectangle<float>(rx, ry, std::max(rw, 240.0f * titleSize / mcp::aiRegionTitleSize), std::max(rh, titleSize * 2.0f)).expanded(4.0f).intersects(invalidRegion.toFloat()))
                return;
            NVGScopedState scopedRegion(nvg);
            if (rw > 0.0f && rh > 0.0f) {
                // the see-through colour of its kind (no outline), then the 3 px ink bar down the left edge
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgRect(nvg, rx, ry, rw, rh);
                nanovg::nvgFillColor(nvg, mcpai::colour(mcp::aiRegionTint(kindWord), mcp::aiRegionFillAlpha));
                nanovg::nvgFill(nvg);
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgRect(nvg, rx, ry, mcp::aiRegionBarWidth, rh);
                nanovg::nvgFillColor(nvg, mcpai::colour(mcp::aiInk));
                nanovg::nvgFill(nvg);
            }
            if (title.isNotEmpty()) {
                mcpnvg::nvgFontSize(nvg, titleSize);
                mcpnvg::nvgFontFace(nvg, "Inter-Medium");
                mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
                float bounds[4];
                mcpnvg::nvgTextBounds(nvg, 0.0f, 0.0f, title.toRawUTF8(), nullptr, bounds);
                float const titleW = bounds[2] - bounds[0];
                float const titleH = bounds[3] - bounds[1];
                // Left, unless a wire or a step disc would lie on the name there and the right is clear. The answer is remembered
                // per region and asked again only when its inputs changed (the rectangle, the name's size, the wires, the discs).
                mcp::AiRectF const rect { rx, ry, rw, rh };
                auto key = mcp::aiHashSeed;
                for (float const v : { rx, ry, rw, rh, titleW, titleH })
                    key = mcp::aiHashMix(key, v);
                key = mcp::aiHashMix(key, rw > 0.0f ? sceneKeyOnce() : std::uint64_t { 0 }); // a title-only rectangle ignores the wires
                auto const side = mcpTitleSides.get(slot, key, [&] {
                    return mcp::aiRegionTitlePlaceBy(rect, titleW, titleH, [&](mcp::AiRectF spot) { return regionTitleCovered(spot, steps); }).right;
                });
                if (side.changed) // drawn on the other side before: this render is scissored to the damage and would tear it
                    flippedBands.push_back(juce::Rectangle<float>(rx, ry + mcp::aiRegionTitleTop, std::max(rw, titleW + 2.0f * mcp::aiRegionTitleInset + mcp::aiRegionTitleNudge), titleH).expanded(3.0f).getSmallestIntegerContainer());
                auto const place = mcp::aiRegionTitleAt(rect, titleW, side.right);
                nanovg::nvgFillColor(nvg, mcpai::colour(mcp::aiInk, mcp::aiRegionTitleAlpha));
                mcpnvg::nvgText(nvg, place.x, place.y, title.toRawUTF8(), nullptr);
            }
        };

        std::size_t slot = 0;
        for (size_t k = 0; k < rects.size(); ++k) // a region lying on an earlier one's boxes has an empty rectangle: its title only
            drawRegion(slot++, canvas.canvasOrigin.x + static_cast<float>(rects[k].x), canvas.canvasOrigin.y + static_cast<float>(rects[k].y),
                static_cast<float>(rects[k].w), static_cast<float>(rects[k].h), regions[memberOf[k]].title, regions[memberOf[k]].kind);
        for (auto const& r : regions) { // no targets: the rectangle as given
            if (r.targetIds.size() == 0)
                drawRegion(slot++, canvas.canvasOrigin.x + r.x, canvas.canvasOrigin.y + r.y, r.w, r.h, r.title, r.kind);
        }
        mcpTitleSides.trim(slot);
        for (auto const& band : flippedBands)
            repaintAfterRender(band);
    } else {
        mcpTitleSides.clear(); // no regions on show: what was remembered is not on the screen any more
    }

    // PRD Phase 2: pasted reference images render as glass backdrops behind cords/objects.
    renderReferenceImages(nvg, invalidRegion);
    renderVoiceTakes(nvg, invalidRegion);
    renderMidiTakes(nvg, invalidRegion);
}

void CanvasMcpLayer::paintOverObjects(NVGcontext* nvg)
{
    // A graph-on-parent box renders its inner canvas through this same path (every GOP module, every frame). The
    // AI overlays belong to the canvas's own view, never to the box drawn inside another one: skip them there.
    if (canvas.isGraph)
        return;
    // PRD overlay: the AI's tags (design round B2). A tag is a 1 px grey leader from the boxes, a glyph (square = a
    // measurement, circle = a change, triangle = look here, half circle = a question), and the text in ink with a halo
    // in the canvas colour. No chassis, no shadow, no bevel, no accent bar, no LED. The frame comes from
    // getAnnotationFrame, the same function the hover, the close cross and the click use.
    if (canvas.pd && shouldShowAIAnnotations() && !canvas.pd->getMcp()->session.getMcpAnnotations().empty()) {
        auto const canvasBg = nvgColour(getThemeColours(canvas).canvasBackgroundColour);
        auto const anns = canvas.pd->getMcp()->session.getMcpAnnotations();
        auto const scene = buildAnnotationScene(); // every box, once for all the tags of this paint
        for (size_t ai = 0; ai < anns.size(); ++ai) {
            auto const& a = anns[ai];
            if (a.text.isEmpty()) continue;
            auto const frame = getAnnotationFrame(ai, scene);
            auto const& tape = frame.tag;
            if (tape.isEmpty()) continue; // all target boxes deleted

            // Outside what this render redraws: skip the tag, its chips drawer (below, may be wider), the hover outline
            // around its boxes and the leader down to them.
            {
                auto cull = tape.withSize(tape.getWidth() + 240.0f, tape.getHeight() + 30.0f);
                if (!frame.group.isEmpty())
                    cull = cull.getUnion(frame.group);
                if (!mcpPaintRegion.isEmpty() && !cull.expanded(12.0f).intersects(mcpPaintRegion.toFloat()))
                    continue;
            }

            NVGScopedState scopedAnn(nvg);
            bool const isHovered = (static_cast<int>(ai) == mcpNoteHover);
            bool const isCloseHover = (isHovered && mcpNoteHoverClose);

            // 1. On hover only: the linked boxes get a 1 px grey outline (no coloured fill).
            if (isHovered && !frame.group.isEmpty()) {
                auto const g = frame.group.expanded(4.0f);
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgRoundedRect(nvg, g.getX(), g.getY(), g.getWidth(), g.getHeight(), mcp::aiRadius);
                mcpai::strokeOutline(nvg, mcp::aiGrey);
            }

            // 2. The leader: a 1 px grey line from the boxes to the tag.
            if (frame.hasLeader) {
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgMoveTo(nvg, frame.leaderFrom.x, frame.leaderFrom.y);
                nanovg::nvgLineTo(nvg, frame.leaderTo.x, frame.leaderTo.y);
                mcpai::strokeOutline(nvg, mcp::aiGrey);
            }

            // 3. The glyph (the artist's own notes have none: plain ink text) and the text with its halo.
            float const midY = tape.getCentreY();
            mcpai::drawGlyph(nvg, mcp::aiTagOf(a.kind.toStdString()), tape.getX() + mcp::aiTagGlyphSlotPx * 0.5f - 2.0f, midY);
            float const textLeft = tape.getX() + mcp::aiTagGlyphSlotPx;
            {
                NVGScopedState scopedText(nvg);
                // The frame cuts a text that is longer than it: never drawn past it, never over the close cross.
                nanovg::nvgScissor(nvg, textLeft - 1.0f, tape.getY(), std::max(20.0f, tape.getWidth() - mcp::aiTagGlyphSlotPx - annotationCloseSlotPx), tape.getHeight());
                mcpai::drawHaloText(nvg, textLeft, midY, a.text.toRawUTF8(), canvasBg, mcpai::colour(mcp::aiInk));
                nanovg::nvgResetScissor(nvg);
            }

            // 4. On hover only: a 1 px ink outline around the tag and the close cross in ink (grey until the pointer is on it).
            if (isHovered) {
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgRoundedRect(nvg, tape.getX() - 0.5f, tape.getY() - 0.5f, tape.getWidth() + 1.0f, tape.getHeight() + 1.0f, mcp::aiRadius);
                mcpai::strokeOutline(nvg, mcp::aiInk);
                float const cx = tape.getRight() - annotationCloseSlotPx * 0.5f;
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgMoveTo(nvg, cx - 3.0f, midY - 3.0f);
                nanovg::nvgLineTo(nvg, cx + 3.0f, midY + 3.0f);
                nanovg::nvgMoveTo(nvg, cx + 3.0f, midY - 3.0f);
                nanovg::nvgLineTo(nvg, cx - 3.0f, midY + 3.0f);
                nanovg::nvgStrokeColor(nvg, mcpai::colour(mcp::aiInk, isCloseHover ? 1.0f : 0.6f));
                nanovg::nvgStrokeWidth(nvg, isCloseHover ? mcp::aiLineWidthActive : mcp::aiLineWidth);
                nanovg::nvgStroke(nvg);
            }

            // 5. Pivot chips drawer (below a hovered tag): cream, 1 px ink outline, ink text, the chosen chip inverted.
            if (isHovered && mcpNoteEditIndex != static_cast<int>(ai)) {
                auto const chips = getAnnotationPivotChips(a.text, a.kind);
                float totalChipsW = 0.0f;
                for (auto const& c : chips) totalChipsW += getChipWidth(c.label) + 4.0f;
                float const drawerW = std::max(tape.getWidth(), totalChipsW + 4.0f);
                float const drawerX = tape.getX();
                float const drawerY = tape.getY() + tape.getHeight() + 2.0f;
                float const drawerH = 22.0f;

                nanovg::nvgBeginPath(nvg);
                nanovg::nvgRoundedRect(nvg, drawerX, drawerY, drawerW, drawerH, mcp::aiRadius);
                nanovg::nvgFillColor(nvg, mcpai::colour(mcp::aiCream));
                nanovg::nvgFill(nvg);
                mcpai::strokeOutline(nvg, mcp::aiInk);

                float curChipX = drawerX + 4.0f;
                float const chipY = drawerY + 2.5f;
                float const chipH = drawerH - 5.0f;
                for (size_t ci = 0; ci < chips.size(); ++ci) {
                    auto const& chip = chips[ci];
                    float const cw = getChipWidth(chip.label);
                    bool const chosen = (mcpNoteHoverChip == static_cast<int>(ci));

                    nanovg::nvgBeginPath(nvg);
                    nanovg::nvgRoundedRect(nvg, curChipX, chipY, cw, chipH, mcp::aiRadius);
                    if (chosen) {
                        nanovg::nvgFillColor(nvg, mcpai::colour(mcp::aiInk));
                        nanovg::nvgFill(nvg);
                    } else {
                        mcpai::strokeOutline(nvg, mcp::aiInk);
                    }
                    mcpnvg::nvgFontSize(nvg, 10.0f);
                    mcpnvg::nvgFontFace(nvg, "Inter");
                    mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                    nanovg::nvgFillColor(nvg, mcpai::colour(chosen ? mcp::aiCream : mcp::aiInk));
                    mcpnvg::nvgText(nvg, curChipX + cw * 0.5f, chipY + chipH * 0.5f, chip.label.toRawUTF8(), nullptr);

                    curChipX += cw + 4.0f;
                }
            }
        }
    }

    // The spotlight's order discs (design round B5): nothing unless a spotlight is on and holds boxes of this canvas.
    paintSpotlightOrder(nvg);

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
            // The pill body (design round 2026-11-19, "the pill joins tonight"): cream paper, a 1 px ink frame, ink text, the
            // same look as the input field of the Copilot panel. Drawing only: the keys, the focus and the lanes are untouched.
            // (The ink-mood strip below is dead code, mcpPillSketchMode is never set; it keeps its dark panel.)
            NVGScopedState scopedPanel(nvg);
            nanovg::nvgTranslate(nvg, static_cast<float>(panel.getX()), static_cast<float>(panel.getY()));
            bool const paper = !mcpPillSketchMode;
            if (paper)
                nanovg::nvgDrawRoundedRect(nvg, 0, 0, w, h, mcpai::colour(mcp::aiCream), mcpai::colour(mcp::aiInk), mcp::aiRadius);
            else
                nanovg::nvgDrawRoundedRect(nvg, 0, 0, w, h, nanovg::nvgRGBA(24, 26, 30, 250), nanovg::nvgRGBA(55, 60, 72, 220), 8.0f);

            // Audio-reactive border glow while recording voice / beatbox (red = recording)
            if (isVoiceRec) {
                float const lvl = voiceLvl;
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgRoundedRect(nvg, -1.0f, -1.0f, w + 2.0f, h + 2.0f, 9.0f);
                nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(255, 59, 48, static_cast<unsigned char>(130 + juce::jlimit(0.0f, 125.0f, lvl * 350.0f))));
                nanovg::nvgStrokeWidth(nvg, 1.5f + lvl * 3.5f);
                nanovg::nvgStroke(nvg);
            }

            // Subtle top highlight line (the dark strip only)
            if (!paper) {
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgMoveTo(nvg, 8.0f, 1.0f);
                nanovg::nvgLineTo(nvg, w - 8.0f, 1.0f);
                nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(255, 255, 255, 25));
                nanovg::nvgStrokeWidth(nvg, 1.0f);
                nanovg::nvgStroke(nvg);
            }

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
            // Left: the half circle, "asking / your turn" (the same mark as an AI question on the canvas)
            mcpai::drawGlyph(nvg, mcp::AiTag::Ask, 15.0f, h * 0.5f);

            // Right ~26px: the send arrow (Enter), a 1 px ink outline, no fill
            {
                float const subW = 20.0f;
                float const subH = 18.0f;
                float const subX = w - 25.0f;
                float const subY = (h - subH) * 0.5f;
                // Drawn as a shape, not the glyph: text with a character the UI font lacks is shaped on every
                // frame, and each shaping clones the Noto Emoji face for the preferred-fallback check (GUI CPU hunt).
                float const cx = subX + subW * 0.5f;
                float const cy = subY + subH * 0.5f;
                nanovg::nvgSave(nvg);
                nanovg::nvgStrokeColor(nvg, mcpai::colour(mcp::aiInk));
                nanovg::nvgStrokeWidth(nvg, mcp::aiLineWidth);
                nanovg::nvgLineCap(nvg, NVG_ROUND);
                nanovg::nvgLineJoin(nvg, NVG_ROUND);
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgMoveTo(nvg, cx + 5.0f, cy - 5.0f); // down from the top right
                nanovg::nvgLineTo(nvg, cx + 5.0f, cy + 1.5f);
                nanovg::nvgLineTo(nvg, cx - 5.0f, cy + 1.5f); // then left
                nanovg::nvgMoveTo(nvg, cx - 2.0f, cy - 1.5f); // arrow head
                nanovg::nvgLineTo(nvg, cx - 5.0f, cy + 1.5f);
                nanovg::nvgLineTo(nvg, cx - 2.0f, cy + 4.5f);
                nanovg::nvgStroke(nvg);
                nanovg::nvgRestore(nvg);
            }
                // quick / deep chip, before the send arrow: one click flips the lane the next sends take. A 1 px outlined
                // lower-case button, inverted (ink fill, cream text) while the deep lane is on.
                {
                    bool deepLane = false;
                    if (canvas.pd) {
                        if (auto* laneBridge = canvas.pd->getMcp()->getMCPBridge())
                            deepLane = laneBridge->isPillDeepLane();
                    }
                    auto const chip = pillLaneChipRect(panel).translated(-panel.getX(), -panel.getY()).toFloat();
                    nanovg::nvgBeginPath(nvg);
                    nanovg::nvgRoundedRect(nvg, chip.getX() + 0.5f, chip.getY() + 0.5f, chip.getWidth() - 1.0f, chip.getHeight() - 1.0f, mcp::aiRadius);
                    if (deepLane) {
                        nanovg::nvgFillColor(nvg, mcpai::colour(mcp::aiInk));
                        nanovg::nvgFill(nvg);
                    } else {
                        mcpai::strokeOutline(nvg, mcp::aiInk);
                    }
                    NVGScopedState scopedChip(nvg);
                    mcpnvg::nvgFontFace(nvg, "Inter");
                    mcpnvg::nvgFontSize(nvg, 10.0f);
                    mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                    nanovg::nvgFillColor(nvg, mcpai::colour(deepLane ? mcp::aiCream : mcp::aiInk));
                    mcpnvg::nvgText(nvg, chip.getCentreX(), chip.getCentreY() + 0.5f, deepLane ? "deep" : "quick", nullptr);
                }
            } // else: object-pill controls
        }

        // The pill body is cream paper with ink text (the dark strip is dead code), see the panel above.
        bool const paperOn = !mcpPillSketchMode;
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
                nanovg::nvgStrokeColor(nvg, paperOn ? mcpai::colour(mcp::aiInk) : nanovg::nvgRGBA(74, 158, 255, 230));
                nanovg::nvgStrokeWidth(nvg, paperOn ? mcp::aiLineWidth : 1.5f);
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
                nanovg::nvgFillColor(nvg, mcpai::colour(mcp::aiRed));
            } else if (mcpActiveLens.isNotEmpty()) {
                hint = "[" + mcpActiveLens.toUpperCase() + "] Ask AI on selection...";
                nanovg::nvgFillColor(nvg, mcpai::colour(mcp::aiGrey, mcpSelectionPill->hasKeyboardFocus(true) ? 0.6f : 1.0f));
            } else {
                hint = "Ask AI on selection... (Enter to mutate, Esc to cancel)";
                nanovg::nvgFillColor(nvg, mcpai::colour(mcp::aiGrey, mcpSelectionPill->hasKeyboardFocus(true) ? 0.6f : 1.0f));
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

    // The AI's newest chat line, one short line under the pill's place (CanvasMcpPill.cpp, McpPillAnswer.h).
    paintPillAnswer(nvg);

    // Big Alphi 1a0: Alphi's corner first (no box, chip stack, pill or drawer under it), so the chips are laid out
    // around the corner it takes (CanvasMcpFlow.cpp, McpAlphiDock.h).
    // Alphi drop 1 (review N3): a paint that is only Alphi's own motion skips the dock pass (CanvasMcpFlow.cpp).
    if (alphiDockPassNeeded())
        updateAlphiDock();
    // P12c approval chips: one per pending record, clamped inside the visible viewport (CanvasMcpApproval.cpp).
    paintApprovalChips(nvg);
    // P12a.1: Alphi, the flow-mode switch, in its corner of the view (CanvasMcpFlow.cpp). After the chips: they are laid
    // out around it, and when one could not move off it (a tiny view) Alphi is not drawn at all (review M2).
    paintAlphi(nvg);
    // Alphi drop 1 D6: his one line (or the busy text) beside the face, where it is free (CanvasMcpAlphiSay.cpp).
    paintAlphiStrip(nvg);
    alphiRememberZone();
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

    // PRD overlay: ghost/preview of PROPOSED (uncommitted) changes. Drawn above the patch, never part of it: the artist
    // sees the change before it's real. Design round B3: ink at 60 %, 1 px, dotted (dash 2), no fill; the label is ink at 60 %.
    if (canvas.pd && shouldShowAIGhosts() && !canvas.pd->getMcp()->session.getMcpGhosts().empty()) {
        constexpr float dotted = 2.0f;
        for (auto const& g : canvas.pd->getMcp()->session.getMcpGhosts()) {
            NVGScopedState scopedGhost(nvg);
            if (g.kind == 0) {
                float const gx = canvas.canvasOrigin.x + g.x;
                float const gy = canvas.canvasOrigin.y + g.y;
                float const gw = g.w > 0 ? g.w : 120.0f;
                float const gh = g.h > 0 ? g.h : 22.0f;
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgRoundedRect(nvg, gx, gy, gw, gh, getPlugDataLook(canvas).getObjectCornerRadius());
                nanovg::nvgStrokeColor(nvg, mcpai::colour(mcp::aiInk, mcp::aiGhostAlpha));
                nanovg::nvgStrokeWidth(nvg, mcp::aiLineWidth);
                nanovg::nvgLineStyle(nvg, NVG_LINE_DASHED);
                nanovg::nvgDashLength(nvg, dotted);
                nanovg::nvgStroke(nvg);
                nanovg::nvgLineStyle(nvg, NVG_LINE_SOLID);
                if (g.label.isNotEmpty()) {
                    mcpnvg::nvgFontSize(nvg, mcp::aiTagTextSize);
                    mcpnvg::nvgFontFace(nvg, "Inter");
                    mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                    nanovg::nvgFillColor(nvg, mcpai::colour(mcp::aiInk, mcp::aiGhostAlpha));
                    mcpnvg::nvgText(nvg, gx + 6.0f, gy + gh * 0.5f, g.label.toRawUTF8(), nullptr);
                }
            } else {
                float const x1 = canvas.canvasOrigin.x + g.x1, y1 = canvas.canvasOrigin.y + g.y1;
                float const x2 = canvas.canvasOrigin.x + g.x2, y2 = canvas.canvasOrigin.y + g.y2;
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgMoveTo(nvg, x1, y1);
                float const dx = std::abs(x2 - x1) * 0.5f + 20.0f;
                nanovg::nvgBezierTo(nvg, x1, y1 + dx, x2, y2 - dx, x2, y2);
                nanovg::nvgStrokeColor(nvg, mcpai::colour(mcp::aiInk, mcp::aiGhostAlpha));
                nanovg::nvgStrokeWidth(nvg, mcp::aiLineWidth);
                nanovg::nvgLineStyle(nvg, NVG_LINE_DASHED);
                nanovg::nvgDashLength(nvg, dotted);
                nanovg::nvgStroke(nvg);
                nanovg::nvgLineStyle(nvg, NVG_LINE_SOLID);
            }
        }
    }
}

void CanvasMcpLayer::paintViewportFrame(NVGcontext* nvg, float const zoom)
{
    // A graph-on-parent box renders its inner canvas through this same path (every GOP module, every frame). The
    // AI overlays belong to the canvas's own view, never to the box drawn inside another one: skip them there.
    if (canvas.isGraph)
        return;
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

void CanvasMcpLayer::repaintHudArea()
{
    // The HUD card sits centred at the bottom of the view (paintScreenOverlay): at most ~190 px high with its shadow
    // (descH is capped at 85). A HUD update ("Thinking... N s" once a second) repaints that band, not the canvas.
    if (!canvas.viewport) {
        canvas.repaint();
        return;
    }
    auto& vp = *canvas.viewport;
    vp.repaint(juce::Rectangle<int>(0, std::max(0, vp.getHeight() - hudBandHeight), vp.getWidth(), std::min(vp.getHeight(), hudBandHeight)));
}

void CanvasMcpLayer::paintScreenOverlay(NVGcontext* nvg)
{
    // A graph-on-parent box renders its inner canvas through this same path (every GOP module, every frame). The
    // AI overlays belong to the canvas's own view, never to the box drawn inside another one: skip them there.
    if (canvas.isGraph)
        return;
    // Cinematic Viewport HUD & Subtitle Overlay (Video & Tutorial Mode)
    // Rendered in screen coordinates (unscaled/unpanned) — zero cord collisions.
    if (canvas.pd) {
        auto hud = canvas.pd->getMcp()->session.getMcpHud();
        // Only when this render redraws the HUD's band (repaintHudArea): a small repaint elsewhere (Alphi, a meter)
        // skips the card and its text layout.
        bool hudInRender = true;
        if (canvas.viewport && canvas.editor) {
            auto& vp = *canvas.viewport;
            auto const render = vp.getLocalArea(canvas.editor, canvas.editor->nvgSurface.getInvalidArea());
            juce::Rectangle<int> const band(0, std::max(0, vp.getHeight() - hudBandHeight), vp.getWidth(), std::min(vp.getHeight(), hudBandHeight));
            hudInRender = render.intersects(band);
        }
        if (hud.active && hudInRender && shouldShowAIHud()) {
            // Design round B4: a cream card, no shadow, no gradient, no glass border; a 3 px ink bar on the left, ONE mark
            // (mcp::aiCardMark: empty ring = sent / waiting, red dot = built, blue square = measured, yellow triangle =
            // silent or an error, none otherwise), the title 13 px medium in ink, the second line 12 px grey. The card
            // keeps its place (centred at the bottom) and stays inside hudBandHeight, so repaintHudArea still covers it.
            NVGScopedState scopedHud(nvg);
            float const vw = static_cast<float>(canvas.viewport ? canvas.viewport->getWidth() : canvas.getWidth());
            float const vh = static_cast<float>(canvas.viewport ? canvas.viewport->getHeight() : canvas.getHeight());

            bool const hasPrompt = hud.prompt.isNotEmpty();
            bool const hasReceipt = hud.receipt.isNotEmpty();
            bool const hasTool = hud.tool.isNotEmpty();
            bool const hasChapter = hud.chapter.isNotEmpty();

            // Card title (Chapter, action name, or tool). The server's chapter words come in capitals ("SENT"): shown as a
            // sentence ("Sent"); a title the AI wrote itself keeps its case.
            juce::String cardTitle;
            if (hasChapter) cardTitle = hud.chapter;
            else if (hasTool) cardTitle = hud.tool;
            if (cardTitle.isNotEmpty() && cardTitle == cardTitle.toUpperCase())
                cardTitle = cardTitle.substring(0, 1).toUpperCase() + cardTitle.substring(1).toLowerCase();

            // Direct action statement
            juce::String cardDesc;
            if (hasPrompt) cardDesc = hud.prompt;
            else if (hasReceipt) cardDesc = hud.receipt;

            // Sanitize description text: strip newlines and unprintable characters that cause tofu boxes
            juce::String cleanDesc = cardDesc.replaceCharacter('\r', ' ');
            while (cleanDesc.contains("\n\n"))
                cleanDesc = cleanDesc.replace("\n\n", " \xE2\x80\xA2 ");
            cleanDesc = cleanDesc.replaceCharacter('\n', ' ').trim();

            // Sizing: floating centred card with title-safe margins; the text starts after the bar and the mark.
            constexpr float maxCardW = 760.0f;
            constexpr float barW = 3.0f;
            constexpr float markCentreX = 15.0f; // from the bar
            constexpr float textInset = 28.0f;   // from the bar
            constexpr float padY = 12.0f;
            constexpr float titleH = 18.0f;
            constexpr float rightPad = 16.0f;
            float const cardW = jmin(vw - 48.0f, maxCardW);
            float const cardX = (vw - cardW) * 0.5f;
            float const textLeft = cardX + barW + textInset;
            float const breakWidth = cardW - barW - textInset - rightPad;

            // Text height via nvgTextBoxBounds for the wrapped second line
            float descH = 0.0f;
            if (cleanDesc.isNotEmpty()) {
                mcpnvg::nvgFontSize(nvg, mcp::aiTagTextSize);
                mcpnvg::nvgFontFace(nvg, "Inter");
                float bounds[4];
                mcpnvg::nvgTextBoxBounds(nvg, 0, 0, breakWidth, cleanDesc.toRawUTF8(), nullptr, bounds);
                descH = jlimit(16.0f, 85.0f, bounds[3] - bounds[1]);
            }

            float cardH = padY * 2.0f + titleH;
            if (cardTitle.isNotEmpty() && cleanDesc.isNotEmpty())
                cardH = padY * 2.0f + titleH + 2.0f + descH;
            else if (cleanDesc.isNotEmpty())
                cardH = padY * 2.0f + descH;
            float const cardY = vh - cardH - 34.0f;

            // 1. The cream card and its ink bar.
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, cardX, cardY, cardW, cardH, mcp::aiRadius);
            nanovg::nvgFillColor(nvg, mcpai::colour(mcp::aiCream));
            nanovg::nvgFill(nvg);
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRect(nvg, cardX, cardY, barW, cardH);
            nanovg::nvgFillColor(nvg, mcpai::colour(mcp::aiInk));
            nanovg::nvgFill(nvg);

            // 2. Title row: the mark, the title, the tool in grey, telemetry / latency at the right.
            float const titleMidY = cardY + padY + titleH * 0.5f;
            if (cardTitle.isNotEmpty()) {
                mcpai::drawCardMark(nvg, mcp::aiCardMark(cardTitle.toStdString()), cardX + barW + markCentreX, titleMidY);

                mcpnvg::nvgFontSize(nvg, mcp::aiNameSize);
                mcpnvg::nvgFontFace(nvg, "Inter-Medium");
                mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                nanovg::nvgFillColor(nvg, mcpai::colour(mcp::aiInk));
                float const afterTitle = mcpnvg::nvgText(nvg, textLeft, titleMidY, cardTitle.toRawUTF8(), nullptr);

                if (hasTool && hud.tool != hud.chapter && hud.tool != cardTitle) {
                    mcpnvg::nvgFontSize(nvg, mcp::aiHintSize);
                    mcpnvg::nvgFontFace(nvg, "Inter");
                    nanovg::nvgFillColor(nvg, mcpai::colour(mcp::aiGrey));
                    mcpnvg::nvgText(nvg, afterTitle + 8.0f, titleMidY, hud.tool.toRawUTF8(), nullptr);
                }
            }

            juce::String const metaRight = hud.latency.isNotEmpty() ? hud.latency : hud.telemetry;
            if (metaRight.isNotEmpty()) {
                mcpnvg::nvgFontSize(nvg, mcp::aiHintSize);
                mcpnvg::nvgFontFace(nvg, "Inter-Tabular");
                mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
                nanovg::nvgFillColor(nvg, mcpai::colour(mcp::aiGrey));
                mcpnvg::nvgText(nvg, cardX + cardW - rightPad, titleMidY, metaRight.toRawUTF8(), nullptr);
            }

            // 3. The second line (word wrapped): 12 px grey.
            if (cleanDesc.isNotEmpty()) {
                float const descY = cardTitle.isNotEmpty() ? (cardY + padY + titleH + 2.0f) : (cardY + padY);
                mcpnvg::nvgFontSize(nvg, mcp::aiTagTextSize);
                mcpnvg::nvgFontFace(nvg, "Inter");
                mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
                nanovg::nvgFillColor(nvg, mcpai::colour(mcp::aiGrey));
                mcpnvg::nvgTextBox(nvg, textLeft, descY, breakWidth, cleanDesc.toRawUTF8(), nullptr);
            }
        }
    }
}

bool CanvasMcpLayer::decorateObject(NVGcontext* nvg, Object& object, juce::Rectangle<int> b)
{
    // The spotlight leaves every box as it is (owner 2026-11-20: the cream veil over the boxes off the path is gone; the
    // path reads by its ink wires and step discs, the cords off it at aiOffPathCordAlpha).
    if (!shouldShowAIState())
        return false;
    // PRD overlay: AI state marker drawn per-object (1=changed, 2=proposed, 3=error). Design round B3: the same shapes as
    // the tags, on the top-right corner, no coloured frame: 1 = a filled red circle (radius 3); 2 = an outline red
    // circle plus a dotted ink frame; 3 = a yellow triangle.
    int const aiState = canvas.pd->getMcp()->session.getAiOverlayState(object.getPointer());
    if (aiState != 0) {
        float const cx = static_cast<float>(b.getRight());
        float const cy = static_cast<float>(b.getY());
        NVGScopedState scopedAIState(nvg);
        if (aiState == 1) {
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgCircle(nvg, cx, cy, 3.0f);
            nanovg::nvgFillColor(nvg, mcpai::colour(mcp::aiRed));
            nanovg::nvgFill(nvg);
        } else if (aiState == 2) {
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, b.getX(), b.getY(), b.getWidth(), b.getHeight(), getPlugDataLook(object).getObjectCornerRadius());
            nanovg::nvgStrokeColor(nvg, mcpai::colour(mcp::aiInk, mcp::aiGhostAlpha));
            nanovg::nvgStrokeWidth(nvg, mcp::aiLineWidth);
            nanovg::nvgLineStyle(nvg, NVG_LINE_DASHED);
            nanovg::nvgDashLength(nvg, 2.0f);
            nanovg::nvgStroke(nvg);
            nanovg::nvgLineStyle(nvg, NVG_LINE_SOLID);
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgCircle(nvg, cx, cy, 2.5f);
            mcpai::strokeOutline(nvg, mcp::aiRed);
        } else {
            mcpai::drawGlyph(nvg, mcp::AiTag::Look, cx, cy);
        }
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

bool CanvasMcpLayer::spotlightHere() const
{
    if (canvas.isGraph || !canvas.pd)
        return false;
    auto const& session = canvas.pd->getMcp()->session;
    auto const version = session.getMcpOverlayTargetsVersion();
    auto const objects = static_cast<std::size_t>(canvas.objects.size());
    if (version != mcpSpotVersion || objects != mcpSpotObjects) {
        mcpSpotHere = false;
        if (session.hasMcpOverlayTargets()) {
            for (auto* obj : canvas.objects) {
                if (obj && session.isMcpTargeted(obj->getPointer())) {
                    mcpSpotHere = true;
                    break;
                }
            }
        }
        mcpSpotVersion = version;
        mcpSpotObjects = objects;
    }
    return mcpSpotHere;
}

NVGcolor CanvasMcpLayer::spotlightWire(Connection const* c, NVGcolor normal) const
{
    if (!c || !spotlightHere() || c->isSelected())
        return normal; // no host, no targets, another canvas, or a wire the artist holds: the normal colour
    if (isTargeted(c))
        return mcpai::colour(mcp::aiInk);
    return spotlightFade(c, normal);
}

NVGcolor CanvasMcpLayer::spotlightFade(Connection const* c, NVGcolor normal) const
{
    if (!c || !spotlightHere() || c->isSelected() || isTargeted(c))
        return normal;
    normal.a *= mcp::aiOffPathCordAlpha; // control and signal cords alike: a signal cord's dark dashes fade with its body
    return normal;
}

void CanvasMcpLayer::paintSpotlightActivity(NVGcontext* nvg, Connection const& c, float progress) const
{
    if (progress <= 0.0f || progress >= 1.0f || !spotlightHere() || !isTargeted(&c) || !canvas.shouldShowConnectionActivity())
        return;
    auto const& wire = c.getPath();
    float const length = wire.getLength();
    if (length < 1.0f)
        return;
    // Connection::render draws in the wire's own coordinates (its position is subtracted from the canvas path)
    auto const p = wire.getPointAlongPath(length * progress) - c.getPosition().toFloat();
    NVGScopedState scopedMark(nvg);
    nanovg::nvgBeginPath(nvg);
    nanovg::nvgRect(nvg, p.x - 3.0f, p.y - 3.0f, 6.0f, 6.0f);
    nanovg::nvgFillColor(nvg, mcpai::colour(mcp::aiBlue));
    nanovg::nvgFill(nvg);
}

std::uint64_t CanvasMcpLayer::wiresKey() const
{
    auto h = mcp::aiHashMix(mcp::aiHashSeed, static_cast<int>(canvas.connections.size()));
    auto const addr = [](void const* p) { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p)); };
    for (auto* c : canvas.connections) {
        if (!c)
            continue;
        h = mcp::aiHashMix(h, addr(c));
        h = mcp::aiHashMix(h, addr(c->outobj.get())); // a retyped box makes new wires: the same path from another box is another wire
        h = mcp::aiHashMix(h, addr(c->inobj.get()));
        h = mcp::aiHashMix(h, c->outIdx);
        h = mcp::aiHashMix(h, c->inIdx);
        for (juce::Path::Iterator it(c->getPath()); it.next();) { // the path's elements: no flattening, no measuring
            h = mcp::aiHashMix(h, static_cast<int>(it.elementType));
            if (it.elementType == juce::Path::Iterator::closePath)
                continue;
            h = mcp::aiHashMix(h, it.x1);
            h = mcp::aiHashMix(h, it.y1);
            if (it.elementType == juce::Path::Iterator::quadraticTo || it.elementType == juce::Path::Iterator::cubicTo) {
                h = mcp::aiHashMix(h, it.x2);
                h = mcp::aiHashMix(h, it.y2);
            }
            if (it.elementType == juce::Path::Iterator::cubicTo) {
                h = mcp::aiHashMix(h, it.x3);
                h = mcp::aiHashMix(h, it.y3);
            }
        }
    }
    return h;
}

std::vector<CanvasMcpLayer::SpotlightStep> const& CanvasMcpLayer::spotlightSteps() const
{
    if (canvas.isGraph || !spotlightHere()) {
        mcpSteps.clear();
        mcpStepsValid = false;
        return mcpSteps;
    }
    // The discs depend on the targets and their order (the targets version covers both) and on where the wires run. Made again only
    // when one of them changed: this is asked twice per render (the region names step aside for the discs, then the discs are drawn)
    // and a wire's middle takes two measurements of its path.
    auto const key = mcp::aiHashMix(wiresKey(), static_cast<std::uint64_t>(canvas.pd->getMcp()->session.getMcpOverlayTargetsVersion()));
    if (mcpStepsValid && key == mcpStepsKey)
        return mcpSteps;
    mcpSteps.clear();
    auto const order = canvas.pd->getMcp()->session.getMcpOverlayOrder();
    auto const rankOf = [&order](Object const* obj) -> std::size_t {
        if (!obj)
            return order.size();
        for (std::size_t i = 0; i < order.size(); ++i)
            if (order[i] == obj->getPointer())
                return i;
        return order.size();
    };
    struct Step {
        std::size_t outRank, inRank;
        int outIdx, inIdx;
        juce::Point<float> middle;
    };
    std::vector<Step> steps;
    for (auto* c : canvas.connections) {
        if (!c || !isTargeted(c))
            continue;
        auto const outRank = rankOf(c->outobj.get());
        auto const inRank = rankOf(c->inobj.get());
        if (outRank >= order.size() || inRank >= order.size())
            continue; // a wire that only touches the path is not a step of it
        // The middle of the wire (live review 2026-11-19): two steps that leave the same outlet used to put their discs on one
        // spot, and the later covered the earlier (the picture showed 2, 3, 4 and no 1). A wire without a length keeps its start.
        auto const& wire = c->getPath();
        auto const length = wire.getLength();
        auto const middle = length >= 1.0f ? wire.getPointAlongPath(length * 0.5f) : c->getStartPoint();
        steps.push_back({ outRank, inRank, c->outIdx, c->inIdx, middle });
    }
    std::sort(steps.begin(), steps.end(), [](Step const& a, Step const& b) {
        return std::tie(a.outRank, a.inRank, a.outIdx, a.inIdx) < std::tie(b.outRank, b.inRank, b.outIdx, b.inIdx);
    });
    int n = 0;
    for (auto const& step : steps)
        mcpSteps.push_back({ step.middle, ++n }); // numbered from 1
    mcpStepsKey = key;
    mcpStepsValid = true;
    return mcpSteps;
}

void CanvasMcpLayer::paintSpotlightOrder(NVGcontext* nvg)
{
    for (auto const& step : spotlightSteps())
        mcpai::drawGlyph(nvg, mcp::AiTag::Step, step.centre.x, step.centre.y, step.number);
}

bool CanvasMcpLayer::regionTitleCovered(mcp::AiRectF spot, std::vector<SpotlightStep> const& steps) const
{
    constexpr float disc = mcp::aiStepDiscSize;
    for (auto const& step : steps)
        if (mcp::aiIntersects(spot, { step.centre.x - disc * 0.5f, step.centre.y - disc * 0.5f, disc, disc }))
            return true;
    // A wire is its flattened segments (one pass each, no measuring); only a wire whose bounds reach the spot is looked at.
    juce::Rectangle<float> const reach = juce::Rectangle<float>(spot.x, spot.y, spot.w, spot.h).expanded(mcp::aiRegionWireHalfWidth);
    for (auto* c : canvas.connections) {
        if (!c || !c->getBounds().toFloat().intersects(reach))
            continue;
        for (juce::PathFlatteningIterator it(c->getPath()); it.next();)
            if (mcp::aiWireSegmentOnTitle({ it.x1, it.y1 }, { it.x2, it.y2 }, spot))
                return true;
    }
    return false;
}

void CanvasMcpLayer::repaintAfterRender(juce::Rectangle<int> area) const
{
    // The surface clears its damage when the frame is recorded: a repaint asked from inside the render is lost. Posted, it lands
    // after the frame and the next one redraws `area`.
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<Canvas>(&canvas), area] {
        if (!safe)
            return;
        safe->repaint(area);
        if (safe->editor)
            safe->editor->nvgSurface.invalidateArea(safe->editor->nvgSurface.getLocalArea(safe.getComponent(), area));
    });
}

void CanvasMcpLayer::updateOverlays(int const overlayState)
{
    // The spotlight's targets changed (set, or cleared by itself): the boxes draw into a cached command buffer, so each
    // repaints once. One shot, no timer. (The caller, Canvas::updateOverlays, repaints the canvas itself.)
    if (canvas.pd) {
        auto const version = canvas.pd->getMcp()->session.getMcpOverlayTargetsVersion();
        if (version != mcpSeenTargetsVersion) {
            mcpSeenTargetsVersion = version;
            for (auto* obj : canvas.objects)
                if (obj)
                    obj->repaint();
        }
    }
    showAiState = overlayState & AIState;
    showAiRegions = overlayState & AIRegions;
    showAiAnnotations = overlayState & AIAnnotations;
    showAiGhosts = overlayState & AIGhosts;
    showAiHud = overlayState & AIHud;
    showAiSketch = overlayState & AISketch;
    showAiReferences = overlayState & AIReferences;
}
