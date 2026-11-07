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

    int playNoteIndex = 0;
    double playStartTime = 0.0;
    bool isAuditioning = false;

    // Live jam 2026-11-08 (UX): REC first counts one bar in, with a click, then records from that bar line.
    static constexpr int countInBeats = 4;
    int countInLeft = 0;          // beats still to count before recording starts (0 = no count-in running)
    double nextBeatMs = 0.0;      // wall time (JUCE hi-res ms) of the next click
    double recStartLogical = 0.0; // Pd logical time of the bar line the take starts on
    int clickBeat = 0;            // beats clicked since the count-in began (accent on each bar's first beat)
    bool recClickOn = false;      // keep clicking while recording

    enum Button { recButton = 0, playButton, clearButton, aiButton, numButtons };

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
        constrainer->setMinimumWidth(200);
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

    // Display geometry (2026-11-08 restyle: a glowing synth screen). Row A: the 7-segment take number + status;
    // row B: four tab buttons; row C: a 16-cell beat meter when the widget is tall enough.
    static constexpr int pad = 5;
    int rowAHeight() const { return juce::jlimit(18, 34, static_cast<int>((getHeight() - 2 * pad) * 0.45f)); }
    int rowBTop() const { return pad + rowAHeight() + 4; }
    int rowBHeight() const { return juce::jlimit(13, 20, getHeight() - rowBTop() - pad - (meterFits() ? 12 : 0)); }
    bool meterFits() const { return getHeight() >= 64; }

    /** The four tab buttons across the middle row: REC PLAY CLEAR TO-AI. */
    Rectangle<int> buttonRect(int const which) const
    {
        int const gap = 3;
        int const tabW = (getWidth() - 2 * pad - gap * (numButtons - 1)) / numButtons;
        return { pad + which * (tabW + gap), rowBTop(), tabW, rowBHeight() };
    }

    int buttonAt(Point<int> const pos) const
    {
        for (int i = 0; i < numButtons; ++i)
            if (buttonRect(i).contains(pos))
                return i;
        return -1;
    }

    // Live jam 2026-11-07: the buttons only answered in run mode. They now answer in edit mode too; a click anywhere
    // else on the widget still goes to the canvas (select, drag, resize), because hitTest lets it through.
    void lock(bool const) override
    {
        setInterceptsMouseClicks(true, false);
    }

    bool hitTest(int const x, int const y) override
    {
        return isLocked() || buttonAt({ x, y }) >= 0;
    }

    /** One beat of the click: a float on the [r aimidi_click] bus (1 = the bar's first beat, 0 = the others). */
    static void sendClick(bool const accent)
    {
        if (auto* thing = gensym("aimidi_click")->s_thing)
            pd_float(thing, accent ? 1.0f : 0.0f);
    }

    double beatMs(t_fake_aimidi const* midi) const
    {
        float const bpm = midi->x_bpm > 20.0f ? midi->x_bpm : 120.0f;
        return 60000.0 / static_cast<double>(bpm);
    }

    void startCountIn(t_fake_aimidi* midi)
    {
        isAuditioning = false;
        midi->x_playing = 0;
        countInLeft = countInBeats;
        clickBeat = 0;
        double const bar = beatMs(midi) * countInBeats;
        // The take's zero is the bar line after the count-in, in Pd's own clock (the notes are stamped in it).
        recStartLogical = clock_getsystimeafter(bar);
        nextBeatMs = juce::Time::getMillisecondCounterHiRes();
    }

    void stopRecording(t_fake_aimidi* midi)
    {
        countInLeft = 0;
        recClickOn = false;
        if (!midi->x_recording)
            return;
        t_atom off;
        SETFLOAT(&off, 0.0f);
        pd_typedmess(&midi->x_obj.ob_pd, gensym("record"), 1, &off); // stops, sends the take id and a bang
        packageAndRegisterTake();
    }

    /** Count-in clicks, then recording starts on the bar line; the click goes on while recording. */
    void tickCountInAndClick(t_fake_aimidi* midi)
    {
        if (countInLeft <= 0 && !recClickOn)
            return;
        double const now = juce::Time::getMillisecondCounterHiRes();
        if (now < nextBeatMs)
            return;
        if (countInLeft > 0) {
            sendClick(clickBeat % countInBeats == 0);
            ++clickBeat;
            --countInLeft;
            nextBeatMs += beatMs(midi);
            if (countInLeft == 0) {
                t_atom on;
                SETFLOAT(&on, 1.0f);
                pd_typedmess(&midi->x_obj.ob_pd, gensym("record"), 1, &on); // new take id, clears the notes
                midi->x_rec_start_time = recStartLogical;
                recClickOn = true;
            }
            return;
        }
        if (!midi->x_recording) { // stopped by a message ([stop(, [record 0()
            recClickOn = false;
            return;
        }
        sendClick(clickBeat % countInBeats == 0);
        ++clickBeat;
        nextBeatMs += beatMs(midi);
        if (nextBeatMs < now) // the timer fell far behind (window hidden): no burst of clicks
            nextBeatMs = now + beatMs(midi);
    }

    void timerCallback() override
    {
        auto midi = ptr.get<t_fake_aimidi>();
        if (!midi) return;

        tickCountInAndClick(midi.get());

        bool const rec = (midi->x_recording != 0);
        bool const play = isAuditioning || (midi->x_playing != 0);

        if (rec || play || countInLeft > 0) {
            repaint();
        }

        // Live jam 2026-11-07: a [play( / [stop( message (or Claude Code) drives the same loop as the PLAY button.
        if (midi->x_playing && !isAuditioning && midi->x_note_count > 0) {
            isAuditioning = true;
            playNoteIndex = 0;
            playStartTime = juce::Time::getMillisecondCounterHiRes();
        } else if (!midi->x_playing && isAuditioning) {
            isAuditioning = false;
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
        if (!e.mods.isLeftButtonDown()) return;
        int const which = buttonAt(e.getPosition());
        if (which < 0) return;

        auto midi = ptr.get<t_fake_aimidi>();
        if (!midi) return;

        switch (which) {
        case recButton:
            if (midi->x_recording || countInLeft > 0)
                stopRecording(midi.get()); // also cancels a count-in
            else
                startCountIn(midi.get());
            break;
        case playButton:
            if (isAuditioning) {
                isAuditioning = false;
                midi->x_playing = 0;
            } else if (midi->x_note_count > 0 && !midi->x_recording && countInLeft == 0) {
                midi->x_playing = 1; // keep the message flag in step with the button
                isAuditioning = true;
                playNoteIndex = 0;
                playStartTime = juce::Time::getMillisecondCounterHiRes();
            }
            break;
        case clearButton:
            countInLeft = 0;
            recClickOn = false;
            isAuditioning = false;
            pd_typedmess(&midi->x_obj.ob_pd, gensym("clear"), 0, nullptr);
            break;
        case aiButton:
            transmitToAi();
            break;
        default:
            break;
        }
        repaint();
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

    // ── Display palette ──
    static NVGcolor glass(unsigned char a = 255) { return nanovg::nvgRGBA(6, 9, 18, a); }
    static NVGcolor frame(unsigned char a = 255) { return nanovg::nvgRGBA(59, 91, 219, a); }
    static NVGcolor segOff() { return nanovg::nvgRGBA(20, 32, 70, 255); }
    static NVGcolor cyan(unsigned char a = 255) { return nanovg::nvgRGBA(68, 242, 232, a); }
    static NVGcolor ice(unsigned char a = 255) { return nanovg::nvgRGBA(220, 233, 255, a); }
    static NVGcolor salmon(unsigned char a = 255) { return nanovg::nvgRGBA(255, 107, 94, a); }

    /** A glowing outline: a wide faint stroke under a thin bright one. */
    static void glowStroke(NVGcontext* nvg, NVGcolor c, float width)
    {
        auto halo = c;
        halo.a *= 0.22f;
        nanovg::nvgStrokeColor(nvg, halo);
        nanovg::nvgStrokeWidth(nvg, width * 3.2f);
        nanovg::nvgStroke(nvg);
        nanovg::nvgStrokeColor(nvg, c);
        nanovg::nvgStrokeWidth(nvg, width);
        nanovg::nvgStroke(nvg);
    }

    /** One 7-segment digit (-1 = blank). Unlit segments stay faintly visible, like a real display. */
    static void drawDigit(NVGcontext* nvg, float x, float y, float dw, float dh, int digit, NVGcolor lit)
    {
        static constexpr unsigned char segs[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
        unsigned char const on = (digit >= 0 && digit <= 9) ? segs[digit] : 0;
        float const t = std::max(1.6f, dw * 0.16f); // stroke thickness
        float const m = dh * 0.5f;
        // a b c d e f g: top, top-right, bottom-right, bottom, bottom-left, top-left, middle
        float const lines[7][4] = {
            { x + t, y, x + dw - t, y }, { x + dw, y + t, x + dw, y + m - t * 0.5f },
            { x + dw, y + m + t * 0.5f, x + dw, y + dh - t }, { x + t, y + dh, x + dw - t, y + dh },
            { x, y + m + t * 0.5f, x, y + dh - t }, { x, y + t, x, y + m - t * 0.5f }, { x + t, y + m, x + dw - t, y + m },
        };
        for (int i = 0; i < 7; ++i) {
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgMoveTo(nvg, lines[i][0], lines[i][1]);
            nanovg::nvgLineTo(nvg, lines[i][2], lines[i][3]);
            nanovg::nvgLineCap(nvg, NVG_ROUND);
            if (on & (1 << i)) {
                glowStroke(nvg, lit, t);
            } else {
                nanovg::nvgStrokeColor(nvg, segOff());
                nanovg::nvgStrokeWidth(nvg, t);
                nanovg::nvgStroke(nvg);
            }
        }
    }

    static int takeNumber(juce::String const& takeId)
    {
        return takeId.fromLastOccurrenceOf("_", false, false).getIntValue() % 100;
    }

    /** How many of the meter's 16 cells are lit, and in which colour. */
    std::pair<int, NVGcolor> meterState(t_fake_aimidi const* midi, bool rec, bool play) const
    {
        if (countInLeft > 0)
            return { countInBeats - countInLeft, salmon() };
        if (rec)
            return { (std::max(0, clickBeat - countInBeats) % 16) + 1, salmon() };
        if (midi && midi->x_note_count > 0) {
            double const lastMs = midi->x_notes[midi->x_note_count - 1].time_ms + 500.0;
            if (play) {
                double const elapsed = juce::Time::getMillisecondCounterHiRes() - playStartTime;
                return { juce::jlimit(1, 16, static_cast<int>(elapsed / lastMs * 16.0) + 1), cyan() };
            }
            int const beats = static_cast<int>(lastMs / beatMs(midi) + 0.5);
            return { juce::jlimit(1, 16, beats), frame() };
        }
        return { 0, frame() };
    }

    void render(NVGcontext* nvg) override
    {
        auto const b = getLocalBounds().toFloat();
        float const w = b.getWidth();
        float const h = b.getHeight();

        auto midi = ptr.get<t_fake_aimidi>();
        bool const rec = midi && (midi->x_recording != 0);
        bool const play = isAuditioning || (midi && midi->x_playing != 0);
        bool const counting = countInLeft > 0;
        int const noteCount = midi ? midi->x_note_count : 0;
        int const bpm = midi ? static_cast<int>(midi->x_bpm) : 120;
        juce::String const takeId = midi ? juce::String::fromUTF8(midi->x_take_id) : "take_1";

        // Glass
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgRoundedRect(nvg, 0.5f, 0.5f, w - 1.0f, h - 1.0f, 4.0f);
        nanovg::nvgFillColor(nvg, glass());
        nanovg::nvgFill(nvg);
        nanovg::nvgStrokeColor(nvg, object->isSelected() && !cnv->isGraph ? nvgColour(getThemeColours().objectSelectedOutlineColour) : frame(150));
        nanovg::nvgStrokeWidth(nvg, 1.0f);
        nanovg::nvgStroke(nvg);

        // Scanlines: the faint texture of a lit display
        nanovg::nvgBeginPath(nvg);
        for (float y = 3.0f; y < h - 2.0f; y += 3.0f) {
            nanovg::nvgMoveTo(nvg, 2.0f, y);
            nanovg::nvgLineTo(nvg, w - 2.0f, y);
        }
        nanovg::nvgStrokeColor(nvg, nanovg::nvgRGBA(120, 160, 255, 10));
        nanovg::nvgStrokeWidth(nvg, 1.0f);
        nanovg::nvgStroke(nvg);

        // Row A: the number box (take number, or the count-in in salmon)
        float const aH = static_cast<float>(rowAHeight());
        float const digH = aH - 8.0f;
        float const digW = digH * 0.52f;
        float const boxW = digW * 2.0f + 14.0f;
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgRoundedRect(nvg, pad, pad, boxW, aH, 2.5f);
        glowStroke(nvg, counting || rec ? salmon(200) : frame(), 1.2f);
        auto const lit = counting || rec ? salmon() : cyan();
        int const shown = counting ? countInLeft : takeNumber(takeId);
        drawDigit(nvg, pad + 5.0f, pad + 4.0f, digW, digH, counting ? -1 : shown / 10, lit);
        drawDigit(nvg, pad + 9.0f + digW, pad + 4.0f, digW, digH, shown % 10, lit);

        // Row A: status in ice, tempo small in frame blue at the right
        juce::String const status = counting ? "counting in"
            : rec        ? juce::String(noteCount) + " notes"
            : play       ? "playing"
            : noteCount  ? juce::String(noteCount) + " notes"
                         : "empty, press REC";
        mcpnvg::nvgFontFace(nvg, "Inter-Bold");
        mcpnvg::nvgFontSize(nvg, juce::jlimit(11.0f, 18.0f, aH * 0.62f));
        mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nanovg::nvgFillColor(nvg, counting || rec ? salmon() : ice());
        mcpnvg::nvgText(nvg, pad + boxW + 8.0f, pad + aH * 0.5f, status.toRawUTF8(), nullptr);

        mcpnvg::nvgFontSize(nvg, 10.0f);
        mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
        nanovg::nvgFillColor(nvg, frame());
        mcpnvg::nvgText(nvg, w - pad - 2.0f, pad + 1.0f, (juce::String(bpm) + " bpm").toRawUTF8(), nullptr);

        // Row B: tab buttons, filled when active (like a selected page on the hardware)
        auto drawTab = [&](int which, char const* label, bool active, NVGcolor on) {
            auto const r = buttonRect(which).toFloat();
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgRect(nvg, r.getX() + 0.5f, r.getY() + 0.5f, r.getWidth() - 1.0f, r.getHeight() - 1.0f);
            if (active) {
                nanovg::nvgFillColor(nvg, on);
                nanovg::nvgFill(nvg);
                glowStroke(nvg, on, 1.0f);
            } else {
                nanovg::nvgStrokeColor(nvg, frame(220));
                nanovg::nvgStrokeWidth(nvg, 1.0f);
                nanovg::nvgStroke(nvg);
            }
            mcpnvg::nvgFontFace(nvg, "Inter-Bold");
            mcpnvg::nvgFontSize(nvg, juce::jlimit(9.0f, 12.0f, r.getHeight() * 0.62f));
            mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nanovg::nvgFillColor(nvg, active ? glass() : frame());
            mcpnvg::nvgText(nvg, r.getCentreX(), r.getCentreY(), label, nullptr);
        };
        drawTab(recButton, counting ? "COUNT" : "REC", rec || counting, salmon());
        drawTab(playButton, play ? "STOP" : "PLAY", play, cyan());
        drawTab(clearButton, "CLEAR", false, cyan());
        drawTab(aiButton, "TO AI", false, cyan());

        // Row C: the 16-cell meter
        if (meterFits()) {
            auto const [cells, colour] = meterState(midi.get(), rec, play);
            float const top = static_cast<float>(rowBTop() + rowBHeight() + 4);
            float const cellH = h - top - pad;
            float const cellW = (w - 2.0f * pad - 15.0f * 2.0f) / 16.0f;
            for (int i = 0; i < 16; ++i) {
                nanovg::nvgBeginPath(nvg);
                nanovg::nvgRect(nvg, pad + i * (cellW + 2.0f), top, cellW, cellH);
                nanovg::nvgFillColor(nvg, i < cells ? colour : segOff());
                nanovg::nvgFill(nvg);
            }
        }
    }
};
