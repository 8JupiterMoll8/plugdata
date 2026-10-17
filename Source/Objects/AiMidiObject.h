/*
 // Copyright (c) 2026 Timothy Schoen & Jupiter Moll
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
 */

#pragma once

#include "ObjectBase.h"
#include "PluginProcessor.h"
#include "Canvas.h"
#include "PluginEditor.h"
#include "Pd/MCPBridge.h"
#include "Utility/NVGTextShim.h"
#include <vector>
#include <cmath>

struct t_fake_aimidi_note {
    int   pitch;
    int   vel;
    float time_ms;
    float dur_ms;
};

struct t_fake_aimidi {
    t_object        x_obj;
    t_glist        *x_glist;
    t_symbol       *x_bindname;
    int             x_w;
    int             x_h;
    int             x_zoom;
    int             x_recording;
    int             x_playing;
    float           x_bpm;
    int             x_note_count;
    int             x_max_notes;
    t_fake_aimidi_note *x_notes;
    char            x_take_id[64];
    double          x_rec_start_time;
    t_outlet       *x_out_note;
    t_outlet       *x_out_bang;
    t_outlet       *x_out_id;
};

class AiMidiObject final : public ObjectBase
    , public Timer {

    Value sizeProperty = SynchronousValue();
    Value bpmProperty  = SynchronousValue();

    float reelAngle = 0.0f;
    int playNoteIndex = 0;
    double playStartTime = 0.0;
    bool isAuditioning = false;
    float pulsePhase = 0.0f;

public:
    AiMidiObject(pd::WeakReference ptr, Object* object)
        : ObjectBase(ptr, object)
    {
        objectParameters.addParamSize(&sizeProperty);
        objectParameters.addParamFloat("BPM", cGeneral, &bpmProperty, 120.0f, true, 40.0f, 260.0f);

        // 60 Hz timer for tape reel rotation and playback sequencing
        startTimerHz(60);
    }

    ~AiMidiObject() override
    {
        stopTimer();
    }

    void onConstrainerCreate() override
    {
        constrainer->setMinimumWidth(180);
        constrainer->setMinimumHeight(48);
    }

    void update() override
    {
        if (auto midi = ptr.get<t_fake_aimidi>()) {
            sizeProperty.setValue(VarArray(midi->x_w, midi->x_h));
            bpmProperty.setValue(midi->x_bpm);
        }
    }

    Rectangle<int> getPdBounds() override
    {
        if (auto gobj = ptr.get<t_gobj>()) {
            auto* patch = cnv->patch.getRawPointer();
            int x = 0, y = 0, w = 0, h = 0;
            pd::Interface::getObjectBounds(patch, gobj.get(), &x, &y, &w, &h);
            return Rectangle<int>(x, y, w + 1, h + 1);
        }
        return {};
    }

    void setPdBounds(Rectangle<int> const b) override
    {
        if (auto midi = ptr.get<t_fake_aimidi>()) {
            auto* patch = cnv->patch.getRawPointer();
            pd::Interface::moveObject(patch, midi.cast<t_gobj>(), b.getX(), b.getY());
            midi->x_w = b.getWidth() - 1;
            midi->x_h = b.getHeight() - 1;
        }
    }

    void updateSizeProperty() override
    {
        setPdBounds(object->getObjectBounds());
        if (auto midi = ptr.get<t_fake_aimidi>()) {
            setParameterExcludingListener(sizeProperty, VarArray(var(midi->x_w), var(midi->x_h)));
        }
    }

    bool isLocked() const
    {
        auto const* topLevel = cnv;
        while (auto const* nextCanvas = topLevel->findParentComponentOfClass<Canvas>()) {
            topLevel = nextCanvas;
        }
        return getValue<bool>(topLevel->locked) || getValue<bool>(topLevel->commandLocked) || topLevel->isGraph;
    }

    void timerCallback() override
    {
        auto midi = ptr.get<t_fake_aimidi>();
        if (!midi) return;

        bool const rec = (midi->x_recording != 0);
        bool const play = isAuditioning || (midi->x_playing != 0);

        if (rec || play) {
            reelAngle += 0.12f;
            if (reelAngle > juce::MathConstants<float>::twoPi) {
                reelAngle -= juce::MathConstants<float>::twoPi;
            }
            pulsePhase += 0.1f;
            repaint();
        }

        // Audition playback sequencer
        if (isAuditioning && midi->x_note_count > 0 && midi->x_notes) {
            double const now = juce::Time::getMillisecondCounterHiRes();
            double const elapsed = now - playStartTime;

            while (playNoteIndex < midi->x_note_count &&
                   midi->x_notes[playNoteIndex].time_ms <= elapsed) {
                auto const& n = midi->x_notes[playNoteIndex];
                
                // Fire note on outlet 1
                t_atom at[2];
                SETFLOAT(&at[0], static_cast<t_float>(n.pitch));
                SETFLOAT(&at[1], static_cast<t_float>(n.vel));
                outlet_list(midi->x_out_note, gensym("list"), 2, at);

                playNoteIndex++;
            }

            // Loop when done
            if (playNoteIndex >= midi->x_note_count) {
                double const totalDur = midi->x_notes[midi->x_note_count - 1].time_ms + 500.0;
                if (elapsed >= totalDur) {
                    playNoteIndex = 0;
                    playStartTime = juce::Time::getMillisecondCounterHiRes();
                    outlet_bang(midi->x_out_bang);
                }
            }
        }
    }

    void mouseDown(MouseEvent const& e) override
    {
        if (!isLocked() || !e.mods.isLeftButtonDown()) return;

        int const w = getWidth();
        int const h = getHeight();
        auto const pos = e.getPosition();

        // Control buttons live on the right column
        int const btnX = w - 74;
        int const btnW = 66;
        int const btnH = std::max(14, (h - 16) / 3);

        auto recRect  = Rectangle<int>(btnX, 6, btnW, btnH);
        auto playRect = Rectangle<int>(btnX, 8 + btnH, btnW, btnH);
        auto aiRect   = Rectangle<int>(btnX, 10 + btnH * 2, btnW, btnH);

        auto midi = ptr.get<t_fake_aimidi>();
        if (!midi) return;

        if (recRect.contains(pos)) {
            // Toggle recording
            if (midi->x_recording) {
                // Stop recording & package take
                midi->x_recording = 0;
                packageAndRegisterTake();
                outlet_symbol(midi->x_out_id, gensym(midi->x_take_id));
                outlet_bang(midi->x_out_bang);
            } else {
                // Start recording
                isAuditioning = false;
                midi->x_playing = 0;
                midi->x_recording = 1;
                midi->x_note_count = 0;
                midi->x_rec_start_time = clock_getlogicaltime();
            }
            repaint();
        } else if (playRect.contains(pos)) {
            // Toggle audition play
            if (isAuditioning) {
                isAuditioning = false;
                midi->x_playing = 0;
            } else if (midi->x_note_count > 0) {
                midi->x_recording = 0;
                isAuditioning = true;
                playNoteIndex = 0;
                playStartTime = juce::Time::getMillisecondCounterHiRes();
            }
            repaint();
        } else if (aiRect.contains(pos)) {
            // Transmit take to AI Copilot
            transmitToAi();
        }
    }

    void packageAndRegisterTake()
    {
        auto midi = ptr.get<t_fake_aimidi>();
        if (!midi || !pd || midi->x_note_count <= 0) return;

        McpSession::McpMidiTake take;
        take.id = juce::String::fromUTF8(midi->x_take_id);
        take.bpm = static_cast<int>(midi->x_bpm);
        take.noteCount = midi->x_note_count;
        take.voice = "canvas";
        take.visible = true;

        double maxTime = 0.0;
        DynamicObject::Ptr root = new DynamicObject();
        root->setProperty("mode", "melody");
        root->setProperty("bpm", take.bpm);
        root->setProperty("voice", take.voice);

        Array<var> notesArr;
        for (int i = 0; i < midi->x_note_count; ++i) {
            auto const& n = midi->x_notes[i];
            DynamicObject::Ptr noteObj = new DynamicObject();
            noteObj->setProperty("pitch", n.pitch);
            noteObj->setProperty("vel", n.vel);
            noteObj->setProperty("start", static_cast<double>(n.time_ms) * 0.001);
            noteObj->setProperty("duration", static_cast<double>(n.dur_ms) * 0.001);
            notesArr.add(var(noteObj.get()));
            maxTime = std::max(maxTime, static_cast<double>(n.time_ms + n.dur_ms) * 0.001);
        }
        root->setProperty("notes", var(notesArr));
        take.json = juce::JSON::toString(var(root.get()));
        take.durationSec = maxTime;

        pd->getMcp()->session.addMcpMidiTake(std::move(take));
    }

    void transmitToAi()
    {
        auto midi = ptr.get<t_fake_aimidi>();
        if (!midi || !pd) return;

        packageAndRegisterTake();

        juce::String const takeId = juce::String::fromUTF8(midi->x_take_id);
        int const count = midi->x_note_count;
        int const bpm = static_cast<int>(midi->x_bpm);

        // Flash outlet bang & ID
        outlet_symbol(midi->x_out_id, gensym(midi->x_take_id));
        outlet_bang(midi->x_out_bang);

        if (auto* bridge = pd->getMcp()->getMCPBridge()) {
            juce::String prompt = "Incorporate MIDI take (" + takeId + ", " +
                                  juce::String(count) + " notes, " +
                                  juce::String(bpm) + " BPM) into the patch.";
            bridge->sendSelectionPrompt(prompt, {}, false);
        }
        repaint();
    }

    void render(NVGcontext* nvg) override
    {
        auto const b = getLocalBounds().toFloat();
        float const w = b.getWidth();
        float const h = b.getHeight();

        auto midi = ptr.get<t_fake_aimidi>();
        bool const rec = midi && (midi->x_recording != 0);
        bool const play = isAuditioning || (midi && midi->x_playing != 0);
        int const noteCount = midi ? midi->x_note_count : 0;
        int const bpm = midi ? static_cast<int>(midi->x_bpm) : 120;
        juce::String const takeId = midi ? juce::String::fromUTF8(midi->x_take_id) : "take_1";

        auto const outlineColour = object->isSelected() && !cnv->isGraph
            ? nvgColour(getThemeColours().objectSelectedOutlineColour)
            : (rec ? nanovg::nvgRGBA(255, 70, 70, 220) : (play ? nanovg::nvgRGBA(60, 220, 120, 220) : nanovg::nvgRGBA(180, 90, 255, 180)));

        // 1. Chassis: Deep slate cassette background
        NVGcolor bgGradTop = nanovg::nvgRGBA(24, 22, 34, 250);
        NVGcolor bgGradBot = nanovg::nvgRGBA(16, 15, 24, 255);
        NVGpaint bgPaint = nanovg::nvgLinearGradient(nvg, b.getX(), b.getY(), b.getX(), b.getBottom(), bgGradTop, bgGradBot);
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgRoundedRect(nvg, b.getX(), b.getY(), w, h, getPlugDataLook(*this).getObjectCornerRadius());
        nanovg::nvgFillPaint(nvg, bgPaint);
        nanovg::nvgFill(nvg);
        nanovg::nvgStrokeColor(nvg, outlineColour);
        nanovg::nvgStrokeWidth(nvg, 1.2f);
        nanovg::nvgStroke(nvg);

        // 2. Cassette Label Banner
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgRoundedRect(nvg, b.getX() + 6.0f, b.getY() + 5.0f, w - 88.0f, 16.0f, 3.0f);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(38, 35, 52, 230));
        nanovg::nvgFill(nvg);

        mcpnvg::nvgFontSize(nvg, 10.5f);
        mcpnvg::nvgFontFace(nvg, "Inter-Bold");
        mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(195, 150, 255, 240));
        mcpnvg::nvgText(nvg, b.getX() + 12.0f, b.getY() + 13.0f, "CASSETTE", nullptr);

        mcpnvg::nvgFontSize(nvg, 9.5f);
        mcpnvg::nvgFontFace(nvg, "Inter");
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(160, 160, 180, 200));
        mcpnvg::nvgText(nvg, b.getX() + 75.0f, b.getY() + 13.0f, takeId.toRawUTF8(), nullptr);

        // 3. Cassette Tape Window & Hubs
        float const winX = b.getX() + 10.0f;
        float const winY = b.getY() + 24.0f;
        float const winW = w - 96.0f;
        float const winH = std::max(18.0f, h - 30.0f);

        nanovg::nvgBeginPath(nvg);
        nanovg::nvgRoundedRect(nvg, winX, winY, winW, winH, 4.0f);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(10, 9, 14, 240));
        nanovg::nvgFill(nvg);
        nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(50, 48, 65, 180));
        nanovg::nvgStrokeWidth(nvg, 1.0f);
        nanovg::nvgStroke(nvg);

        // Left & Right Tape Hubs (with 3 spinning spokes)
        float const hubY = winY + winH * 0.5f;
        float const hubR = std::min(13.0f, winH * 0.42f);
        float const hub1X = winX + std::max(18.0f, winW * 0.25f);
        float const hub2X = winX + std::min(winW - 18.0f, winW * 0.75f);

        auto drawHub = [&](float cx, float cy, float angle) {
            // Outer wheel
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgCircle(nvg, cx, cy, hubR);
            nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(45, 42, 60, 255));
            nanovg::nvgFill(nvg);
            nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(120, 115, 150, 220));
            nanovg::nvgStrokeWidth(nvg, 1.0f);
            nanovg::nvgStroke(nvg);

            // Center hole
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgCircle(nvg, cx, cy, hubR * 0.4f);
            nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(12, 10, 18, 255));
            nanovg::nvgFill(nvg);

            // 3 Spokes
            for (int k = 0; k < 3; ++k) {
                float const a = angle + static_cast<float>(k) * (juce::MathConstants<float>::twoPi / 3.0f);
                float const sx = cx + std::cos(a) * hubR * 0.85f;
                float const sy = cy + std::sin(a) * hubR * 0.85f;
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgMoveTo(nvg, cx, cy);
                nanovg::nvgLineTo(nvg, sx, sy);
                nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(190, 185, 220, 190));
                nanovg::nvgStrokeWidth(nvg, 1.4f);
                nanovg::nvgStroke(nvg);
            }
        };

        drawHub(hub1X, hubY, reelAngle);
        drawHub(hub2X, hubY, -reelAngle);

        // Center readout: notes & tempo
        mcpnvg::nvgFontSize(nvg, 9.5f);
        mcpnvg::nvgFontFace(nvg, "Inter-Bold");
        mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nanovg::nvgFillColor(nvg, rec ? nanovg::nvgRGBA(255, 100, 100, 240) : (play ? nanovg::nvgRGBA(80, 230, 140, 240) : nanovg::nvgRGBA(180, 180, 205, 220)));
        juce::String status = rec ? "RECORDING..." : (juce::String(noteCount) + " notes | " + juce::String(bpm) + " BPM");
        mcpnvg::nvgText(nvg, winX + winW * 0.5f, hubY, status.toRawUTF8(), nullptr);

        // 4. Action Buttons (Right Column)
        int const btnX = static_cast<int>(w - 74.0f);
        int const btnW = 66;
        int const btnH = std::max(14, static_cast<int>((h - 16.0f) / 3.0f));

        auto drawBtn = [&](Rectangle<int> rect, juce::String label, NVGcolor col, bool active, bool isRec) {
            float rx = static_cast<float>(rect.getX());
            float ry = static_cast<float>(rect.getY());
            float rw = static_cast<float>(rect.getWidth());
            float rh = static_cast<float>(rect.getHeight());

            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, rx, ry, rw, rh, 3.5f);
            nanovg::nvgFillColor(nvg, active ? nanovg::nvgRGBA(col.r * 255, col.g * 255, col.b * 255, 60) : nanovg::nvgRGBA(28, 26, 38, 220));
            nanovg::nvgFill(nvg);
            nanovg::nvgStrokeColor(nvg, active ? col : nanovg::nvgRGBA(65, 60, 85, 180));
            nanovg::nvgStrokeWidth(nvg, active ? 1.4f : 1.0f);
            nanovg::nvgStroke(nvg);

            // LED dot on left
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgCircle(nvg, rx + 9.0f, ry + rh * 0.5f, 3.5f);
            if (active && isRec) {
                float pulse = 0.6f + 0.4f * std::sin(pulsePhase);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 40, 40, static_cast<unsigned char>(pulse * 255.0f)));
            } else {
                nanovg::nvgFillColor(nvg, active ? col : nanovg::nvgRGBA(80, 75, 100, 200));
            }
            nanovg::nvgFill(nvg);

            mcpnvg::nvgFontSize(nvg, 9.5f);
            mcpnvg::nvgFontFace(nvg, "Inter-Bold");
            mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            nanovg::nvgFillColor(nvg, active ? nanovg::nvgRGBA(250, 250, 255, 255) : nanovg::nvgRGBA(170, 165, 195, 210));
            mcpnvg::nvgText(nvg, rx + 16.0f, ry + rh * 0.5f, label.toRawUTF8(), nullptr);
        };

        drawBtn(Rectangle<int>(btnX, 6, btnW, btnH), "REC", nanovg::nvgRGBA(255, 60, 60, 255), rec, true);
        drawBtn(Rectangle<int>(btnX, 8 + btnH, btnW, btnH), "PLAY", nanovg::nvgRGBA(50, 220, 120, 255), play, false);
        drawBtn(Rectangle<int>(btnX, 10 + btnH * 2, btnW, btnH), "TO AI", nanovg::nvgRGBA(200, 100, 255, 255), false, false);
    }
};
