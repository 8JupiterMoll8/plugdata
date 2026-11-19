// Unit tests for [ai.midi]'s two-speed editor timer (Source/Mcp/McpAiMidiTimer.h), JUCE-free:
//  - the fast timer is needed exactly while the widget records, plays, auditions, counts in or clicks;
//  - at rest the policy never asks for the Pd lock: an idle check is a snapshot read, and 1000 quiet checks give no wake-up and no repaint;
//  - a [play( or [record 1( message from Pd (a flag flipped in the struct, nobody tells the editor) is noticed by the next idle check
//    and wakes the fast timer; the fast timer drops back to idle one step after everything ended, with ONE last repaint;
//  - a change of what the face shows while idle (notes cleared by a message, bpm, take, speaker) repaints once, not again;
//  - the snapshot reads the object the way the editor does (flags, note count, bpm in tenths, a hash of the take id) and survives
//    a NaN bpm and an unterminated take id.
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpAiMidiTimer.h"

#include <cstdio>
#include <cstring>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

namespace {

struct FakeMidi {
    int x_recording = 0;
    int x_playing = 0;
    int x_note_count = 0;
    int x_speaker = 1;
    float x_bpm = 120.0f;
    char x_take_id[64] = "take_1";
};

mcp::AiMidiActivity none() { return {}; }

void fastTimerOnlyWhileSomethingRuns()
{
    CHECK(!mcp::aiMidiNeedsFastTimer(none()));
    mcp::AiMidiActivity a;
    a.recording = true;
    CHECK(mcp::aiMidiNeedsFastTimer(a));
    a = none();
    a.playFlag = true;
    CHECK(mcp::aiMidiNeedsFastTimer(a));
    a = none();
    a.auditioning = true;
    CHECK(mcp::aiMidiNeedsFastTimer(a));
    a = none();
    a.countingIn = true;
    CHECK(mcp::aiMidiNeedsFastTimer(a));
    a = none();
    a.clickOn = true;
    CHECK(mcp::aiMidiNeedsFastTimer(a));
    CHECK(mcp::kAiMidiIdleHz < mcp::kAiMidiActiveHz);
}

void quietIdleNeverWakesAndNeverRepaints()
{
    FakeMidi m;
    mcp::AiMidiTimerPolicy policy;
    auto const snap = mcp::aiMidiSnapshotOf(m);
    auto first = policy.idleCheck(snap); // the first look differs from the empty snapshot: one repaint, no wake-up
    CHECK(!first.fast && !first.modeChanged);
    int repaints = 0, wakes = 0;
    for (int i = 0; i < 1000; ++i) {
        auto const step = policy.idleCheck(mcp::aiMidiSnapshotOf(m));
        repaints += step.repaint ? 1 : 0;
        wakes += (step.fast || step.modeChanged) ? 1 : 0;
    }
    CHECK(repaints == 0);
    CHECK(wakes == 0);
    CHECK(!policy.fast());
}

void playMessageFromPdWakesAndTheEndGoesIdleWithOneRepaint()
{
    FakeMidi m;
    m.x_note_count = 5;
    mcp::AiMidiTimerPolicy policy;
    policy.idleCheck(mcp::aiMidiSnapshotOf(m));

    m.x_playing = 1; // [play( from Pd: only the flag changes
    auto const wake = policy.idleCheck(mcp::aiMidiSnapshotOf(m));
    CHECK(wake.fast && wake.modeChanged && wake.repaint);
    CHECK(policy.fast());

    // the fast ticks: the editor starts its audition (auditioning) while the flag is up
    mcp::AiMidiActivity run;
    run.playFlag = true;
    run.auditioning = true;
    for (int i = 0; i < 10; ++i) {
        auto const tick = policy.evaluate(run, mcp::aiMidiSnapshotOf(m));
        CHECK(tick.fast && !tick.modeChanged && tick.repaint);
    }

    m.x_playing = 0; // [stop( from Pd; the editor ends its audition and silences the voice, then asks
    auto const ended = policy.evaluate(none(), mcp::aiMidiSnapshotOf(m));
    CHECK(!ended.fast && ended.modeChanged && ended.repaint); // one last repaint, then idle
    CHECK(!policy.fast());

    auto const quiet = policy.idleCheck(mcp::aiMidiSnapshotOf(m));
    CHECK(!quiet.repaint && !quiet.fast && !quiet.modeChanged); // the final look was already painted
}

void recordMessageFromPdWakes()
{
    FakeMidi m;
    mcp::AiMidiTimerPolicy policy;
    policy.idleCheck(mcp::aiMidiSnapshotOf(m));
    m.x_recording = 1;
    CHECK(policy.idleCheck(mcp::aiMidiSnapshotOf(m)).fast);
    m.x_recording = 0;
    mcp::AiMidiActivity a;
    a.clickOn = true; // the click flag drops on the next tick; until then the timer stays fast
    CHECK(policy.evaluate(a, mcp::aiMidiSnapshotOf(m)).fast);
    CHECK(!policy.evaluate(none(), mcp::aiMidiSnapshotOf(m)).fast);
}

void aButtonPressWakesFromIdle()
{
    FakeMidi m;
    mcp::AiMidiTimerPolicy policy;
    policy.idleCheck(mcp::aiMidiSnapshotOf(m));
    mcp::AiMidiActivity count;
    count.countingIn = true; // REC pressed: the count-in has no flag in the struct yet
    auto const step = policy.evaluate(count, mcp::aiMidiSnapshotOf(m));
    CHECK(step.fast && step.modeChanged && step.repaint);
    // CLEAR pressed while it ran: everything off, one repaint, idle
    auto const cleared = policy.evaluate(none(), mcp::aiMidiSnapshotOf(m));
    CHECK(!cleared.fast && cleared.modeChanged && cleared.repaint);
}

void aChangeWhileIdleRepaintsOnce()
{
    FakeMidi m;
    m.x_note_count = 8;
    mcp::AiMidiTimerPolicy policy;
    policy.idleCheck(mcp::aiMidiSnapshotOf(m));
    m.x_note_count = 0; // [clear( from Pd
    auto const step = policy.idleCheck(mcp::aiMidiSnapshotOf(m));
    CHECK(step.repaint && !step.fast && !step.modeChanged);
    CHECK(!policy.idleCheck(mcp::aiMidiSnapshotOf(m)).repaint);

    m.x_bpm = 96.0f;
    CHECK(policy.idleCheck(mcp::aiMidiSnapshotOf(m)).repaint);
    CHECK(!policy.idleCheck(mcp::aiMidiSnapshotOf(m)).repaint);
    m.x_speaker = 0;
    CHECK(policy.idleCheck(mcp::aiMidiSnapshotOf(m)).repaint);
    std::strcpy(m.x_take_id, "take_9");
    CHECK(policy.idleCheck(mcp::aiMidiSnapshotOf(m)).repaint);
    CHECK(!policy.idleCheck(mcp::aiMidiSnapshotOf(m)).repaint);
}

void snapshotReadsTheObject()
{
    FakeMidi m;
    m.x_recording = 3;
    m.x_note_count = 12;
    m.x_bpm = 123.46f;
    auto const s = mcp::aiMidiSnapshotOf(m);
    CHECK(s.recording == 1 && s.playing == 0 && s.noteCount == 12 && s.speaker == 1);
    CHECK(s.bpmTenths == 1235);
    CHECK(s.takeHash == mcp::aiMidiHashText("take_1", 64));
    CHECK(mcp::aiMidiHashText("take_1", 64) != mcp::aiMidiHashText("take_2", 64));
    CHECK(mcp::aiMidiHashText("take_1", 4) == mcp::aiMidiHashText("take", 64)); // `max` bounds the read

    FakeMidi odd;
    odd.x_bpm = 0.0f / 0.0f; // NaN: no crash, no tenths
    CHECK(mcp::aiMidiSnapshotOf(odd).bpmTenths == 0);
    std::memset(odd.x_take_id, 'x', sizeof(odd.x_take_id)); // no terminator: the read stops at the buffer end
    (void)mcp::aiMidiSnapshotOf(odd);
    CHECK(mcp::aiMidiHashText(nullptr, 8) == 2166136261u);
}

} // namespace

int main()
{
    fastTimerOnlyWhileSomethingRuns();
    quietIdleNeverWakesAndNeverRepaints();
    playMessageFromPdWakesAndTheEndGoesIdleWithOneRepaint();
    recordMessageFromPdWakes();
    aButtonPressWakesFromIdle();
    aChangeWhileIdleRepaintsOnce();
    snapshotReadsTheObject();
    if (failures) {
        std::printf("ai_midi_timer_test: %d FAILED\n", failures);
        return 1;
    }
    std::printf("ai_midi_timer_test: all passed\n");
    return 0;
}
