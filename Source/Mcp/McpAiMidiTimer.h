// When [ai.midi]'s editor timer runs fast, and what its slow idle check looks at. Header-only and JUCE-free: no Pd, no JUCE, no
// allocation (Tests/mcp/ai_midi_timer_test.cpp).
//
// Before (live jam 2026-11-19, "PlugData very unresponsive, slow to close"): the widget ran a 60 Hz timer for as long as it
// existed, and every tick took the Pd lock (ptr.get<t_fake_aimidi>()) to look at three ints, even when nothing happened.
//
// Now the timer has two speeds and only the fast one takes the Pd lock:
//   Active (60 Hz): the widget records, plays, auditions or counts in. Its tick takes the lock, runs the count-in click, the
//                   playback sequencer and the repaint, exactly as before.
//   Idle (4 Hz):    nothing is going on. The tick reads a SNAPSHOT of the object's fields WITHOUT the Pd lock (ai_midi.c tells
//                   the editor nothing: a [play( / [stop( / [record( message only flips an int in the struct, so the editor has to
//                   look). A recording or playing flag wakes the fast timer; any other change of what the face shows repaints once.
// The lock-free read is a few aligned ints, a float and a short string. A torn read can only give a wrong snapshot, which costs
// one extra repaint or a 250 ms later wake-up; nothing is written through it.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mcp {

constexpr int kAiMidiActiveHz = 60;
constexpr int kAiMidiIdleHz = 4;

// What decides the speed: the object's flags (read under the Pd lock) and the editor's own state.
struct AiMidiActivity {
    bool recording = false;   // x_recording
    bool playFlag = false;    // x_playing (set by the PLAY button or by a [play( message)
    bool auditioning = false; // the editor's own playback loop runs
    bool countingIn = false;  // REC counts one bar in before it records
    bool clickOn = false;     // the click goes on while recording
};

constexpr bool aiMidiNeedsFastTimer(AiMidiActivity const& a)
{
    return a.recording || a.playFlag || a.auditioning || a.countingIn || a.clickOn;
}

// Everything the idle check compares: what the face shows, plus the two flags that wake the fast timer.
struct AiMidiSnapshot {
    int recording = 0;
    int playing = 0;
    int noteCount = 0;
    int speaker = 0;
    int bpmTenths = 0;
    std::uint32_t takeHash = 0;

    bool operator==(AiMidiSnapshot const& o) const
    {
        return recording == o.recording && playing == o.playing && noteCount == o.noteCount && speaker == o.speaker
            && bpmTenths == o.bpmTenths && takeHash == o.takeHash;
    }
    bool operator!=(AiMidiSnapshot const& o) const { return !(*this == o); }
};

// FNV-1a over at most `max` bytes, stopping at the first NUL: the take id ("take_7") as one number.
inline std::uint32_t aiMidiHashText(char const* text, std::size_t max)
{
    std::uint32_t h = 2166136261u;
    for (std::size_t i = 0; text && i < max && text[i] != '\0'; ++i) {
        h ^= static_cast<unsigned char>(text[i]);
        h *= 16777619u;
    }
    return h;
}

// `Midi` is t_fake_aimidi (or a test double): x_recording, x_playing, x_note_count, x_speaker, x_bpm, x_take_id[].
template<class Midi>
AiMidiSnapshot aiMidiSnapshotOf(Midi const& m)
{
    AiMidiSnapshot s;
    s.recording = m.x_recording != 0 ? 1 : 0;
    s.playing = m.x_playing != 0 ? 1 : 0;
    s.noteCount = static_cast<int>(m.x_note_count);
    s.speaker = m.x_speaker != 0 ? 1 : 0;
    float const bpm = m.x_bpm;
    s.bpmTenths = (bpm == bpm && bpm > 0.0f && bpm < 100000.0f) ? static_cast<int>(bpm * 10.0f + 0.5f) : 0;
    s.takeHash = aiMidiHashText(m.x_take_id, sizeof(m.x_take_id));
    return s;
}

// What the editor must do after a timer step.
struct AiMidiTimerStep {
    bool fast = false;      // the timer should be (or stay) at the fast speed
    bool modeChanged = false; // the speed changes now: restart the timer at the new rate
    bool repaint = false;
};

// The two-speed policy. The editor feeds it every idle check and every fast tick and does what it answers.
class AiMidiTimerPolicy {
public:
    bool fast() const { return fast_; }

    // Idle check (no Pd lock): look at the snapshot only.
    AiMidiTimerStep idleCheck(AiMidiSnapshot const& now)
    {
        if (now.recording != 0 || now.playing != 0) // a [record 1( or [play( message from Pd, or the PLAY button's flag
            return switchTo(true, now);
        bool const changed = !(now == last_);
        last_ = now;
        return { false, false, changed };
    }

    // After a fast tick, and after a button press (both hold the Pd lock): the activity decides the speed. The step in which
    // everything ended repaints once more, so the face shows the final state, then the timer goes idle.
    AiMidiTimerStep evaluate(AiMidiActivity const& a, AiMidiSnapshot const& now)
    {
        return switchTo(aiMidiNeedsFastTimer(a), now);
    }

private:
    AiMidiTimerStep switchTo(bool wantFast, AiMidiSnapshot const& now)
    {
        bool const changed = (wantFast != fast_);
        fast_ = wantFast;
        last_ = now;
        return { wantFast, changed, wantFast || changed };
    }

    bool fast_ = false;
    AiMidiSnapshot last_ {};
};

} // namespace mcp
