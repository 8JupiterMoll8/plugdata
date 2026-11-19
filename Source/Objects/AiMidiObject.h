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
#include "Mcp/McpAiMidiVoice.h"
#include "Mcp/McpAiMidiTimer.h"
#include "Mcp/McpAiMidiFace.h"
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
    // The speaker (patches/submodules/pd-else/0005): the C struct goes on after this field with the voice bookkeeping, which only
    // ai_midi.c touches; the editor reads this one and sends `speaker`, `_voice` and `_voiceoff` messages.
    int             x_speaker;
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

    // The timer has two speeds (Source/Mcp/McpAiMidiTimer.h): 60 Hz only while the widget records, plays, auditions or counts in
    // (its tick takes the Pd lock); otherwise a 4 Hz idle check that reads the object WITHOUT the lock and wakes the fast timer when
    // a [play( / [record 1( message from Pd flipped a flag. Live jam 2026-11-19: the old always-on 60 Hz lock made PlugData sluggish.
    mcp::AiMidiTimerPolicy timerPolicy;

    enum Button { recButton = 0, playButton, clearButton, aiButton, soundButton, numButtons };
    static_assert(static_cast<int>(recButton) == static_cast<int>(mcp::kAiMidiRec) && static_cast<int>(playButton) == static_cast<int>(mcp::kAiMidiPlay)
            && static_cast<int>(clearButton) == static_cast<int>(mcp::kAiMidiClear) && static_cast<int>(aiButton) == static_cast<int>(mcp::kAiMidiAi)
            && static_cast<int>(soundButton) == static_cast<int>(mcp::kAiMidiSound) && static_cast<int>(numButtons) == static_cast<int>(mcp::kAiMidiNumButtons),
        "the buttons are numbered like the face layout's (Source/Mcp/McpAiMidiFace.h)");

public:
    AiMidiObject(pd::WeakReference ptr, Object* object)
        : ObjectBase(ptr, object)
    {
        objectParameters.addParamSize(&sizeProperty);
        objectParameters.addParamFloat("BPM", cGeneral, &bpmProperty, 120.0f, true, 40.0f, 260.0f);

        // Idle at first: 4 Hz, no Pd lock. The fast timer starts when a take records, plays or counts in.
        startTimerHz(mcp::kAiMidiIdleHz);
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

    // Display geometry (2026-11-19, the Bauhaus family look): every rectangle comes from mcp::aiMidiFaceLayout (Source/Mcp/McpAiMidiFace.h),
    // for the drawing AND for the click frames, so the two cannot drift apart.
    mcp::AiMidiFace face() const { return mcp::aiMidiFaceLayout(getWidth(), getHeight()); }

    /** The five buttons: rec play clear sound in a row, and the AI chip (what TO AI did) at the bottom right. */
    Rectangle<int> buttonRect(int const which) const
    {
        auto const r = face().button[which];
        return { r.x, r.y, r.w, r.h };
    }

    int buttonAt(Point<int> const pos) const
    {
        auto const f = face();
        for (int i = 0; i < numButtons; ++i)
            if (f.button[i].contains(pos.x, pos.y))
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

    /** The speaker: take entry `index` also goes to the hidden voice engine, through the object's own `_voice <pitch> <vel> <dur ms>`
        message (ai_midi.c keeps what sounds and ends it: a note-off, the clock for a note the take never ends, every stop). The outlet
        has just sent the same entry; this changes nothing about it. Pd lock held (the caller's ptr.get). */
    static void soundEntry(t_fake_aimidi* midi, int const index)
    {
        if (!midi->x_speaker)
            return;
        auto const call = mcp::aiMidiVoiceCall(midi->x_notes, midi->x_note_count, index);
        if (call.pitch < 0)
            return;
        t_atom at[3];
        SETFLOAT(&at[0], static_cast<t_float>(call.pitch));
        SETFLOAT(&at[1], static_cast<t_float>(call.vel));
        SETFLOAT(&at[2], static_cast<t_float>(call.autoOffMs));
        pd_typedmess(&midi->x_obj.ob_pd, gensym("_voice"), 3, at);
    }

    /** Every note the speaker started ends now (the playback stopped, wrapped or started over). Harmless when nothing sounds. */
    static void silenceVoice(t_fake_aimidi* midi)
    {
        pd_typedmess(&midi->x_obj.ob_pd, gensym("_voiceoff"), 0, nullptr);
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
        silenceVoice(midi);
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

    /** The editor's own state plus the object's flags, for the two-speed timer. Pd lock held (the caller's ptr.get). */
    mcp::AiMidiActivity activityOf(t_fake_aimidi const* midi) const
    {
        mcp::AiMidiActivity a;
        a.recording = midi->x_recording != 0;
        a.playFlag = midi->x_playing != 0;
        a.auditioning = isAuditioning;
        a.countingIn = countInLeft > 0;
        a.clickOn = recClickOn;
        return a;
    }

    /** Do what the timer policy answered: the new speed (restart the timer at the new rate) and the repaint. */
    void applyTimerStep(mcp::AiMidiTimerStep const& step)
    {
        if (step.modeChanged)
            startTimerHz(step.fast ? mcp::kAiMidiActiveHz : mcp::kAiMidiIdleHz);
        if (step.repaint)
            repaint();
    }

    /** After a button press (Pd lock held): the fast timer starts when the press began something, and ends when it ended everything. */
    void syncTimer(t_fake_aimidi const* midi)
    {
        applyTimerStep(timerPolicy.evaluate(activityOf(midi), mcp::aiMidiSnapshotOf(*midi)));
    }

    /** The 4 Hz idle check. No Pd lock: ai_midi.c never tells the editor about a [play( / [stop( / [record( message, it only
        flips an int in the struct, so the editor looks at the struct. Only reads; the aligned ints cannot tear, and a stale
        read costs one extra repaint or a 250 ms later wake-up. The object can only vanish under the message thread's own
        remove call (never between this check and the read), and getRaw() already says no once Pd has cleared the reference. */
    void idleCheck()
    {
        auto const* midi = ptr.getRaw<t_fake_aimidi>();
        if (!midi)
            return;
        applyTimerStep(timerPolicy.idleCheck(mcp::aiMidiSnapshotOf(*midi)));
    }

    /** Live jam 2026-11-07: a [play( / [stop( message (or Claude Code) drives the same loop as the PLAY button. */
    void followPdPlayFlag(t_fake_aimidi* midi)
    {
        if (midi->x_playing && !isAuditioning && midi->x_note_count > 0) {
            isAuditioning = true;
            playNoteIndex = 0;
            playStartTime = juce::Time::getMillisecondCounterHiRes();
        } else if (!midi->x_playing && isAuditioning) {
            isAuditioning = false;
            silenceVoice(midi); // a [stop( message ended it: nothing keeps ringing
        }
    }

    /** The audition sequencer: sends the take's notes out of outlet 1 (and to the speaker) in their own time, then loops. */
    void runAudition(t_fake_aimidi* midi)
    {
        if (!isAuditioning || midi->x_note_count <= 0 || !midi->x_notes)
            return;
        double const now = juce::Time::getMillisecondCounterHiRes();
        double const elapsed = now - playStartTime;

        while (playNoteIndex < midi->x_note_count && midi->x_notes[playNoteIndex].time_ms <= elapsed) {
            auto const& n = midi->x_notes[playNoteIndex];

            // Fire note on outlet 1
            t_atom at[2];
            SETFLOAT(&at[0], static_cast<t_float>(n.pitch));
            SETFLOAT(&at[1], static_cast<t_float>(n.vel));
            outlet_list(midi->x_out_note, gensym("list"), 2, at);
            soundEntry(midi, playNoteIndex);

            playNoteIndex++;
        }

        // Loop when done
        if (playNoteIndex >= midi->x_note_count) {
            double const totalDur = midi->x_notes[midi->x_note_count - 1].time_ms + 500.0;
            if (elapsed >= totalDur) {
                silenceVoice(midi); // the loop starts over: no note carries into the next round
                playNoteIndex = 0;
                playStartTime = juce::Time::getMillisecondCounterHiRes();
                outlet_bang(midi->x_out_bang);
            }
        }
    }

    /** The 60 Hz tick (Pd lock held): count-in click, the Pd play flag, the audition, then the repaint and the speed. */
    void activeTick()
    {
        auto midi = ptr.get<t_fake_aimidi>();
        if (!midi)
            return;

        tickCountInAndClick(midi.get());
        followPdPlayFlag(midi.get());
        runAudition(midi.get());
        syncTimer(midi.get()); // repaints while anything runs; the step it all ended: one last repaint, then idle
    }

    void timerCallback() override
    {
        if (timerPolicy.fast())
            activeTick();
        else
            idleCheck();
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
                silenceVoice(midi.get());
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
        case soundButton: {
            // The speaker on or off: the box saves differently (`speaker 0` only while off), so the patch is changed.
            t_atom on;
            SETFLOAT(&on, midi->x_speaker ? 0.0f : 1.0f);
            pd_typedmess(&midi->x_obj.ob_pd, gensym("speaker"), 1, &on);
            if (auto* patch = cnv->patch.getRawPointer())
                canvas_dirty(patch, 1);
            break;
        }
        default:
            break;
        }
        syncTimer(midi.get()); // REC / PLAY start the fast timer; CLEAR ends it
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
            // The take's own source rides on the prompt (take id, notes, bpm): the server does not have to match the words.
            bridge->sendMidiTakePrompt(prompt, takeId, count, bpm);
        }
        repaint();
    }

    // ── Drawing: black on cream, 1 px lines, red only for "recording", no glow ──
    // The colours and sizes are the family's one style table (Source/Mcp/McpAiStyle.h).
    static NVGcolor rgb(mcp::AiRgba const c, unsigned char const a = 255) { return nanovg::nvgRGBA(c.r, c.g, c.b, a); }
    static NVGcolor cream() { return rgb(mcp::aiCream); }
    static NVGcolor ink(unsigned char const a = 255) { return rgb(mcp::aiInk, a); }
    static NVGcolor grey(unsigned char const a = 255) { return rgb(mcp::aiGrey, a); }
    static NVGcolor red() { return rgb(mcp::aiRed); }

    /** What the face shows, read once under the Pd lock. */
    struct FaceState {
        bool rec = false, play = false, counting = false, speaker = false;
        int noteCount = 0, bpm = 120, takeNumber = 1;
        double loopMs = 0.0, spanMs = 2000.0, beatMs = 500.0;
        double headMs = -1.0; // the playhead or the recording head in take time; < 0 = none
    };

    FaceState faceStateOf(t_fake_aimidi const* midi) const
    {
        FaceState st;
        if (!midi)
            return st;
        st.rec = midi->x_recording != 0;
        st.play = isAuditioning || midi->x_playing != 0;
        st.counting = countInLeft > 0;
        st.speaker = midi->x_speaker != 0;
        st.noteCount = midi->x_note_count;
        st.bpm = static_cast<int>(midi->x_bpm);
        st.takeNumber = st.counting ? countInLeft : mcp::aiMidiTakeNumber(midi->x_take_id);
        st.beatMs = beatMs(midi);
        st.loopMs = mcp::aiMidiLoopMs(midi->x_notes, midi->x_note_count);
        double recHeadMs = 0.0;
        if (st.rec && !st.counting) {
            recHeadMs = std::max(0.0, clock_gettimesince(midi->x_rec_start_time));
            st.headMs = recHeadMs;
        } else if (isAuditioning && st.loopMs > 0.0) {
            st.headMs = std::min(st.loopMs, juce::Time::getMillisecondCounterHiRes() - playStartTime);
        }
        st.spanMs = mcp::aiMidiSpanMs(st.loopMs, recHeadMs, st.beatMs);
        return st;
    }

    void drawPlate(NVGcontext* nvg) const
    {
        auto const b = getLocalBounds().toFloat();
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgRoundedRect(nvg, 0.5f, 0.5f, b.getWidth() - 1.0f, b.getHeight() - 1.0f, mcp::kAiMidiRadius);
        nanovg::nvgFillColor(nvg, cream());
        nanovg::nvgFill(nvg);
        nanovg::nvgStrokeColor(nvg, object->isSelected() && !cnv->isGraph ? nvgColour(getThemeColours().objectSelectedOutlineColour) : grey());
        nanovg::nvgStrokeWidth(nvg, mcp::kAiMidiLine);
        nanovg::nvgStroke(nvg);
    }

    /** The 24 px header strip: the name, a grey hint (with the red dot while recording), the tempo at the right. */
    void drawHeader(NVGcontext* nvg, mcp::AiMidiFace const& f, FaceState const& st) const
    {
        float const midY = static_cast<float>(f.header.h) * 0.5f;
        float const left = static_cast<float>(mcp::kAiMidiPad);
        mcpnvg::nvgFontFace(nvg, "Inter-Medium");
        mcpnvg::nvgFontSize(nvg, mcp::kAiMidiNameSize);
        mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nanovg::nvgFillColor(nvg, ink());
        float x = mcpnvg::nvgText(nvg, left, midY, "ai.midi", nullptr) + 8.0f;

        if (st.rec && !st.counting) { // the one red thing in the header: "recording"
            nanovg::nvgBeginPath(nvg);
            nanovg::nvgCircle(nvg, x + 3.0f, midY, 3.0f);
            nanovg::nvgFillColor(nvg, red());
            nanovg::nvgFill(nvg);
            x += 12.0f;
        }
        auto const hint = mcp::aiMidiHintText(st.counting, st.rec, st.play, st.noteCount);
        mcpnvg::nvgFontFace(nvg, "Inter");
        mcpnvg::nvgFontSize(nvg, mcp::kAiMidiHintSize);
        nanovg::nvgFillColor(nvg, grey());
        float const hintEnd = mcpnvg::nvgText(nvg, x, midY, hint.c_str(), nullptr);

        auto const tempo = juce::String(st.bpm) + " bpm";
        float bounds[4] = {};
        float const right = static_cast<float>(f.header.w - mcp::kAiMidiPad);
        mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        mcpnvg::nvgTextBounds(nvg, right, midY, tempo.toRawUTF8(), nullptr, bounds);
        if (bounds[0] > hintEnd + 8.0f) // only when it does not touch the hint
            mcpnvg::nvgText(nvg, right, midY, tempo.toRawUTF8(), nullptr);
    }

    /** The take number as a large plain numeral (the count-in shows the beats left). Red only while recording. */
    void drawNumeral(NVGcontext* nvg, mcp::AiMidiFace const& f, FaceState const& st) const
    {
        if (f.numeral.empty())
            return;
        mcpnvg::nvgFontFace(nvg, "Inter-Medium");
        mcpnvg::nvgFontSize(nvg, f.numeralSize);
        mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nanovg::nvgFillColor(nvg, st.rec && !st.counting ? red() : ink());
        auto const text = juce::String(st.takeNumber);
        mcpnvg::nvgText(nvg, static_cast<float>(f.numeral.x), static_cast<float>(f.numeral.y) + static_cast<float>(f.numeral.h) * 0.5f, text.toRawUTF8(), nullptr);
    }

    /** One vertical hairline (1 px) or head (2 px) at x from y0 to y1. */
    static void vLine(NVGcontext* nvg, float x, float y0, float y1, float width, NVGcolor c)
    {
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgMoveTo(nvg, x, y0);
        nanovg::nvgLineTo(nvg, x, y1);
        nanovg::nvgStrokeColor(nvg, c);
        nanovg::nvgStrokeWidth(nvg, width);
        nanovg::nvgStroke(nvg);
    }

    /** The take as small ink blocks (a tiny piano roll: one bar = one cell of the grid). While playing the part already played is ink
        and the rest grey, behind a 2 px head; while recording the head is red. */
    void drawTake(NVGcontext* nvg, mcp::AiMidiFace const& f, FaceState const& st, t_fake_aimidi const* midi) const
    {
        auto const& r = f.take;
        if (r.empty())
            return;
        float const x0 = static_cast<float>(r.x), y0 = static_cast<float>(r.y);
        float const y1 = static_cast<float>(r.bottom());
        // the tape: a grey rule under the blocks and a hairline at every bar
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgMoveTo(nvg, x0, y1 - 0.5f);
        nanovg::nvgLineTo(nvg, x0 + static_cast<float>(r.w), y1 - 0.5f);
        nanovg::nvgStrokeColor(nvg, grey());
        nanovg::nvgStrokeWidth(nvg, mcp::kAiMidiLine);
        nanovg::nvgStroke(nvg);
        double const barMs = st.beatMs * 4.0;
        int const bars = static_cast<int>(st.spanMs / std::max(1.0, barMs) + 0.5);
        if (bars > 1 && bars <= 32)
            for (int i = 1; i < bars; ++i)
                vLine(nvg, std::floor(x0 + static_cast<float>(i) * static_cast<float>(r.w) / static_cast<float>(bars)) + 0.5f, y0, y1, mcp::kAiMidiLine, grey(110));

        float const headX = st.headMs >= 0.0 ? mcp::aiMidiHeadX(st.headMs, st.spanMs, r.w) : -1.0f;
        if (midi && midi->x_notes && st.noteCount > 0) {
            for (int pass = 0; pass < 2; ++pass) { // pass 0: ink (played, or everything); pass 1: grey (still to come while playing)
                nanovg::nvgBeginPath(nvg);
                mcp::aiMidiForEachBlock(midi->x_notes, st.noteCount, st.spanMs, r.w, r.h, [&](mcp::AiMidiBlock const& b) {
                    bool const ahead = st.play && !st.rec && headX >= 0.0f && b.x > headX;
                    if (ahead == (pass == 1))
                        nanovg::nvgRect(nvg, x0 + b.x, y0 + b.y, b.w, b.h);
                });
                nanovg::nvgFillColor(nvg, pass == 0 ? ink() : grey());
                nanovg::nvgFill(nvg);
            }
        }
        if (headX >= 0.0f) // the one 2 px line: the active thing
            vLine(nvg, x0 + headX + 0.5f, y0, y1, mcp::kAiMidiActiveLine, st.rec ? red() : ink());
    }

    /** A 1 px outlined lower-case button; "on" is inverted (ink fill, cream text); `dim` = nothing to do yet (grey). */
    static void drawButton(NVGcontext* nvg, mcp::AiMidiRect const& r, char const* label, bool const on, bool const dim = false)
    {
        nanovg::nvgBeginPath(nvg);
        nanovg::nvgRoundedRect(nvg, static_cast<float>(r.x) + 0.5f, static_cast<float>(r.y) + 0.5f, static_cast<float>(r.w) - 1.0f, static_cast<float>(r.h) - 1.0f, mcp::kAiMidiRadius);
        if (on) {
            nanovg::nvgFillColor(nvg, ink());
            nanovg::nvgFill(nvg);
        } else {
            nanovg::nvgStrokeColor(nvg, dim ? grey() : ink());
            nanovg::nvgStrokeWidth(nvg, mcp::kAiMidiLine);
            nanovg::nvgStroke(nvg);
        }
        mcpnvg::nvgFontFace(nvg, "Inter");
        mcpnvg::nvgTextAlign(nvg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        float size = mcp::kAiMidiHintSize;
        mcpnvg::nvgFontSize(nvg, size);
        float bounds[4] = {};
        float const room = static_cast<float>(r.w) - 4.0f;
        float const width = mcpnvg::nvgTextBounds(nvg, 0.0f, 0.0f, label, nullptr, bounds);
        if (width > room && width > 0.0f) { // a narrow widget: the word shrinks to fit its box
            size = std::max(8.0f, size * room / width);
            mcpnvg::nvgFontSize(nvg, size);
        }
        nanovg::nvgFillColor(nvg, on ? cream() : (dim ? grey() : ink()));
        mcpnvg::nvgText(nvg, static_cast<float>(r.x) + static_cast<float>(r.w) * 0.5f, static_cast<float>(r.y) + static_cast<float>(r.h) * 0.5f, label, nullptr);
    }

    void drawButtons(NVGcontext* nvg, mcp::AiMidiFace const& f, FaceState const& st) const
    {
        bool const empty = st.noteCount <= 0;
        drawButton(nvg, f.button[recButton], "rec", st.rec || st.counting); // inverted while it counts in or records; the header and the number say which
        drawButton(nvg, f.button[playButton], st.play ? "stop" : "play", st.play, empty && !st.play);
        drawButton(nvg, f.button[clearButton], "clear", false, empty);
        drawButton(nvg, f.button[soundButton], "sound", st.speaker);
        drawButton(nvg, f.button[aiButton], "AI\xE2\x80\xA6", false); // the chip: what TO AI did
    }

    void render(NVGcontext* nvg) override
    {
        auto const f = face();
        auto midi = ptr.get<t_fake_aimidi>();
        auto const st = faceStateOf(midi.get());

        drawPlate(nvg);
        drawHeader(nvg, f, st);
        drawNumeral(nvg, f, st);
        drawTake(nvg, f, st, midi.get());
        drawButtons(nvg, f, st);
    }
};
