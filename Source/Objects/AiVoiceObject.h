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

// Mirror of the Pd struct in Libraries/pd-else/Source/Control/ai_voice.c —
// field order MUST match exactly (the GUI reads/writes the live Pd memory).
struct t_fake_aivoice_note {
    int   pitch;
    int   vel;
    float time_ms;
    float dur_ms;
};

struct t_fake_aivoice {
    t_object        x_obj;
    t_glist        *x_glist;
    t_symbol       *x_bindname;
    int             x_w;
    int             x_h;
    int             x_zoom;
    int             x_recording;
    int             x_playing;
    float           x_bpm;
    int             x_mode;
    int             x_note_count;
    int             x_max_notes;
    t_fake_aivoice_note *x_notes;
    char            x_take_id[64];
    char            x_wav[MAXPDSTRING];
    float           x_duration;
    double          x_rec_start_time;
    t_outlet       *x_out_note;
    t_outlet       *x_out_bang;
    t_outlet       *x_out_id;
};

class AiVoiceObject final : public ObjectBase
    , public Timer {

    Value sizeProperty = SynchronousValue();
    Value bpmProperty  = SynchronousValue();

    float meterLevel = 0.0f;   // smoothed VU (from the bridge's live level)
    float meterPeak = 0.0f;    // decaying peak-hold
    float pulsePhase = 0.0f;
    double recStartMs = 0.0;
    int lastRecState = 0;      // state observer: drives the bridge on ANY 0->1/1->0

public:
    AiVoiceObject(pd::WeakReference ptr, Object* object)
        : ObjectBase(ptr, object)
    {
        objectParameters.addParamSize(&sizeProperty);
        objectParameters.addParamFloat("BPM", cGeneral, &bpmProperty, 120.0f, true, 40.0f, 260.0f);

        // 60 Hz: VU ballistics + record blink.
        startTimerHz(60);
    }

    ~AiVoiceObject() override
    {
        stopTimer();
    }

    void onConstrainerCreate() override
    {
        constrainer->setMinimumWidth(180);
        constrainer->setMinimumHeight(56);
    }

    void update() override
    {
        if (auto voice = ptr.get<t_fake_aivoice>()) {
            sizeProperty.setValue(VarArray(voice->x_w, voice->x_h));
            bpmProperty.setValue(voice->x_bpm);
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
        if (auto voice = ptr.get<t_fake_aivoice>()) {
            auto* patch = cnv->patch.getRawPointer();
            pd::Interface::moveObject(patch, voice.cast<t_gobj>(), b.getX(), b.getY());
            voice->x_w = b.getWidth() - 1;
            voice->x_h = b.getHeight() - 1;
        }
    }

    void updateSizeProperty() override
    {
        setPdBounds(object->getObjectBounds());
        if (auto voice = ptr.get<t_fake_aivoice>()) {
            setParameterExcludingListener(sizeProperty, VarArray(var(voice->x_w), var(voice->x_h)));
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

    MCPBridge* bridge() const
    {
        return pd ? pd->getMCPBridge() : nullptr;
    }

    void timerCallback() override
    {
        auto voice = ptr.get<t_fake_aivoice>();
        if (!voice) return;

        bool const rec = (voice->x_recording != 0);

        // ── State observer: ONE path drives the bridge ───────────────────────
        // The Pd external cannot call the bridge, and the REC button only sets
        // the Pd state. So watch the state here: any 0->1 (button OR a patch
        // message `record 1`) arms the capture; any 1->0 analyzes + stages the
        // take. This is what makes the object properly patchable.
        if (voice->x_recording != lastRecState) {
            if (auto* b = bridge()) {
                if (voice->x_recording) {
                    b->startVoiceCapture(60, voice->x_mode);
                } else {
                    b->stopVoiceCaptureAndAnalyze();
                    outlet_symbol(voice->x_out_id, gensym(voice->x_take_id));
                    outlet_bang(voice->x_out_bang);
                }
            }
            lastRecState = voice->x_recording;
        }

        // VU ballistics: fast attack, slow release, decaying peak-hold.
        if (auto* b = bridge()) {
            float const live = b->getVoiceLiveLevel();
            meterLevel = (live > meterLevel) ? live : (meterLevel * 0.86f + live * 0.14f);
        } else {
            meterLevel *= 0.9f;
        }
        meterPeak = juce::jmax(meterLevel, meterPeak * 0.97f);

        if (rec) {
            pulsePhase += 0.12f;
            if (pulsePhase > juce::MathConstants<float>::twoPi) {
                pulsePhase -= juce::MathConstants<float>::twoPi;
            }
            repaint();
        } else if (meterLevel > 0.001f || meterPeak > 0.001f) {
            repaint();
        }
    }

    void mouseDown(MouseEvent const& e) override
    {
        if (!isLocked() || !e.mods.isLeftButtonDown()) return;

        int const w = getWidth();
        int const h = getHeight();
        auto const pos = e.getPosition();

        // Control buttons live on the right column (same geometry as the cassette).
        int const btnX = w - 74;
        int const btnW = 66;
        int const btnH = std::max(14, (h - 16) / 3);

        auto recRect  = Rectangle<int>(btnX, 6, btnW, btnH);
        auto playRect = Rectangle<int>(btnX, 8 + btnH, btnW, btnH);
        auto aiRect   = Rectangle<int>(btnX, 10 + btnH * 2, btnW, btnH);

        auto voice = ptr.get<t_fake_aivoice>();
        if (!voice) return;

        if (recRect.contains(pos)) {
            // Only flip the Pd state — the timer's state observer arms the
            // capture and stages the take, so the REC button and a patch
            // message `record 1` behave identically (one path, no double-arm).
            if (voice->x_recording) {
                voice->x_recording = 0;
                voice->x_rec_start_time = 0.0;
            } else {
                voice->x_playing = 0;
                voice->x_recording = 1;
                voice->x_note_count = 0;
                voice->x_duration = 0.0f;
                voice->x_wav[0] = '\0';
                voice->x_rec_start_time = clock_getlogicaltime();
                recStartMs = juce::Time::getMillisecondCounterHiRes();
            }
            repaint();
        } else if (playRect.contains(pos)) {
            // Audition the take through the patch's own outlets (non-destructive).
            if (voice->x_note_count > 0 && voice->x_notes) {
                voice->x_playing = 1;
                voice->x_recording = 0;
                t_atom at[2];
                SETFLOAT(&at[0], static_cast<t_float>(voice->x_notes[0].pitch));
                SETFLOAT(&at[1], static_cast<t_float>(voice->x_notes[0].vel));
                outlet_list(voice->x_out_note, gensym("list"), 2, at);
                outlet_bang(voice->x_out_bang);
            }
            repaint();
        } else if (aiRect.contains(pos)) {
            transmitToAi();
        }
    }

    void transmitToAi()
    {
        auto voice = ptr.get<t_fake_aivoice>();
        if (!voice || !pd) return;

        juce::String const takeId = juce::String::fromUTF8(voice->x_take_id);
        int const notes = voice->x_note_count;
        juce::String const wav = juce::String::fromUTF8(voice->x_wav);
        juce::String const mode = (voice->x_mode == 1) ? "beat" : "melody";

        // Flash outlet bang & ID — the patch hears the transmit too.
        outlet_symbol(voice->x_out_id, gensym(voice->x_take_id));
        outlet_bang(voice->x_out_bang);

        if (auto* b = bridge()) {
            juce::String prompt = "Incorporate voice take (" + takeId + ", " + mode;
            if (notes > 0) prompt += ", " + juce::String(notes) + " notes";
            if (wav.isNotEmpty()) prompt += ", wav: " + wav;
            prompt += ") into the patch.";
            b->sendSelectionPrompt(prompt, {}, false);
        }
        repaint();
    }

    void render(NVGcontext* nvg) override
    {
        auto const b = getLocalBounds().toFloat();
        float const w = b.getWidth();
        float const h = b.getHeight();

        auto voice = ptr.get<t_fake_aivoice>();
        bool const rec = voice && (voice->x_recording != 0);
        bool const play = voice && (voice->x_playing != 0);
        int const noteCount = voice ? voice->x_note_count : 0;
        int const bpm = voice ? static_cast<int>(voice->x_bpm) : 120;
        juce::String const takeId = voice ? juce::String::fromUTF8(voice->x_take_id) : "take_1";
        bool const hasWav = voice && (voice->x_wav[0] != '\0');

        auto const outlineColour = object->isSelected() && !cnv->isGraph
            ? cnv->selectedOutlineCol
            : (rec ? nanovg::nvgRGBA(255, 70, 70, 220) : (play ? nanovg::nvgRGBA(60, 220, 120, 220) : nanovg::nvgRGBA(180, 90, 255, 180)));

        // 1. Chassis — same family panel as the cassette.
        NVGcolor bgGradTop = nanovg::nvgRGBA(24, 22, 34, 250);
        NVGcolor bgGradBot = nanovg::nvgRGBA(16, 15, 24, 255);
        NVGpaint bgPaint = nanovg::nvgLinearGradient(nvg, b.getX(), b.getY(), b.getX(), b.getBottom(), bgGradTop, bgGradBot);
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgRoundedRect(nvg, b.getX(), b.getY(), w, h, Corners::objectCornerRadius);
        nanovg::nvgFillPaint(nvg, bgPaint);
        nanovg::nvgFill(nvg);
        nanovg::nvgStrokeColor(nvg, outlineColour);
        nanovg::nvgStrokeWidth(nvg, 1.2f);
        nanovg::nvgStroke(nvg);

        // 2. Label banner: AI VOICE + take id + mode.
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgRoundedRect(nvg, b.getX() + 6.0f, b.getY() + 5.0f, w - 88.0f, 16.0f, 3.0f);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(38, 35, 52, 230));
        nanovg::nvgFill(nvg);

        mcpnvg::nvgFontSize(nvg, 10.5f);
        mcpnvg::nvgFontFace(nvg, "Inter-Bold");
        mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(195, 150, 255, 240));
        mcpnvg::nvgText(nvg, b.getX() + 12.0f, b.getY() + 13.0f, "AI VOICE", nullptr);

        mcpnvg::nvgFontSize(nvg, 9.5f);
        mcpnvg::nvgFontFace(nvg, "Inter");
        nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(160, 160, 180, 200));
        mcpnvg::nvgText(nvg, b.getX() + 72.0f, b.getY() + 13.0f, takeId.toRawUTF8(), nullptr);

        // 3. Mic capsule window: VU meter + peak-hold + status line.
        float const winX = b.getX() + 10.0f;
        float const winY = b.getY() + 26.0f;
        float const winW = w - 94.0f;
        float const winH = h - 34.0f;
        if (winW > 20.0f && winH > 10.0f) {
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, winX, winY, winW, winH, 3.0f);
            nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(10, 9, 16, 255));
            nanovg::nvgFill(nvg);
            nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(60, 55, 80, 200));
            nanovg::nvgStrokeWidth(nvg, 1.0f);
            nanovg::nvgStroke(nvg);

            // VU bar (green → amber → red by level)
            float const barH = juce::jmax(4.0f, winH * 0.34f);
            float const barY = winY + winH - barH - 6.0f;
            float const barMaxW = winW - 12.0f;
            float const lvl = juce::jlimit(0.0f, 1.0f, meterLevel);
            NVGcolor vu = lvl < 0.7f ? nanovg::nvgRGBA(60, 220, 120, 230)
                        : (lvl < 0.9f ? nanovg::nvgRGBA(240, 190, 70, 235) : nanovg::nvgRGBA(255, 80, 80, 240));
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, winX + 6.0f, barY, juce::jmax(1.0f, barMaxW * lvl), barH, 2.0f);
            nanovg::nvgFillColor(nvg, vu);
            nanovg::nvgFill(nvg);

            // Peak-hold tick
            float const pk = juce::jlimit(0.0f, 1.0f, meterPeak);
            if (pk > 0.02f) {
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgRect(nvg, winX + 6.0f + barMaxW * pk - 1.0f, barY - 2.0f, 2.0f, barH + 4.0f);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 255, 255, 190));
                nanovg::nvgFill(nvg);
            }

            // Status line: rec blink / notes / mode
            mcpnvg::nvgFontSize(nvg, 9.5f);
            mcpnvg::nvgFontFace(nvg, "Inter");
            mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
            if (rec) {
                float const blink = 0.5f + 0.5f * std::sin(pulsePhase);
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(255, 70, 70, static_cast<int>(120 + 135 * blink)));
                mcpnvg::nvgText(nvg, winX + 8.0f, winY + 6.0f, "● REC", nullptr);
            } else {
                juce::String status = juce::String(noteCount) + " notes";
                if (hasWav) status += " · wav";
                status += " | " + juce::String(bpm) + " BPM";
                nanovg::nvgFillColor(nvg, nanovg::nvgRGBA(150, 150, 170, 210));
                mcpnvg::nvgText(nvg, winX + 8.0f, winY + 6.0f, status.toRawUTF8(), nullptr);
            }
        }

        // 4. Buttons: REC / PLAY / TRANSMIT (right column).
        int const btnX = static_cast<int>(w) - 74;
        int const btnW = 66;
        int const btnH = std::max(14, (static_cast<int>(h) - 16) / 3);
        auto drawBtn = [&](int y, char const* label, bool active, NVGcolor activeCol) {
            float const bx = b.getX() + static_cast<float>(btnX);
            float const by = b.getY() + static_cast<float>(y);
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRoundedRect(nvg, bx, by, static_cast<float>(btnW), static_cast<float>(btnH), 3.0f);
            nanovg::nvgFillColor(nvg, active ? activeCol : nanovg::nvgRGBA(38, 35, 52, 230));
            nanovg::nvgFill(nvg);
            nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(70, 65, 95, 220));
            nanovg::nvgStrokeWidth(nvg, 1.0f);
            nanovg::nvgStroke(nvg);

            mcpnvg::nvgFontSize(nvg, 9.0f);
            mcpnvg::nvgFontFace(nvg, "Inter-Bold");
            mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nanovg::nvgFillColor(nvg, active ? nanovg::nvgRGBA(20, 18, 28, 255) : nanovg::nvgRGBA(190, 185, 210, 230));
            mcpnvg::nvgText(nvg, bx + static_cast<float>(btnW) * 0.5f, by + static_cast<float>(btnH) * 0.5f, label, nullptr);
        };
        drawBtn(6, rec ? "STOP" : "REC", rec, nanovg::nvgRGBA(255, 80, 80, 235));
        drawBtn(8 + btnH, "PLAY", play, nanovg::nvgRGBA(60, 220, 120, 235));
        drawBtn(10 + btnH * 2, "TRANSMIT", false, nanovg::nvgRGBA(195, 150, 255, 235));
    }
};
