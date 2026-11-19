// Unit tests for what ai.midi tells the hidden voice engine about one entry of its take (Source/Mcp/McpAiMidiVoice.h), JUCE-free:
//  - a note-off entry (vel 0, or below) is an off, and never asks for an auto-off;
//  - a note-on goes with its velocity clipped to 1..127 and the pitch clipped to 0..127;
//  - a note-on the take ends itself (a vel-0 entry of the same pitch comes before the next note-on of that pitch) asks for no auto-off,
//    so a held note keeps its length; one the take never ends (a bare float recorded as vel 100, an off that was lost) asks for an
//    auto-off after its own dur_ms (200 ms when that is unusable), so no note can hang;
//  - an index outside the take gives pitch -1 (nothing to send).
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpAiMidiVoice.h"

#include <cmath>
#include <cstdio>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

namespace {

struct Note {
    int pitch;
    int vel;
    float time_ms;
    float dur_ms;
};

using Take = std::vector<Note>;

mcp::AiMidiVoiceCall callFor(Take const& take, int i)
{
    return mcp::aiMidiVoiceCall(take.data(), static_cast<int>(take.size()), i);
}

void offEntriesAreOffs()
{
    Take const take { { 60, 100, 0, 200 }, { 60, 0, 500, 200 }, { 62, -5, 600, 200 } };
    auto const off = callFor(take, 1);
    CHECK(off.pitch == 60 && off.vel == 0 && off.autoOffMs == 0.0);
    auto const below = callFor(take, 2); // a velocity below zero is not a sound either
    CHECK(below.pitch == 62 && below.vel == 0 && below.autoOffMs == 0.0);
}

void velocityAndPitchAreClipped()
{
    Take const take { { 60, 1, 0, 200 }, { 61, 127, 10, 200 }, { 62, 300, 20, 200 }, { 200, 90, 30, 200 }, { -3, 90, 40, 200 } };
    CHECK(callFor(take, 0).vel == 1);
    CHECK(callFor(take, 1).vel == 127);
    CHECK(callFor(take, 2).vel == 127); // never louder than the engine's scale
    auto const high = callFor(take, 3);
    CHECK(high.pitch == 127 && high.vel == 90);
    auto const low = callFor(take, 4);
    CHECK(low.pitch == 0 && low.vel == 90);
}

void aNoteTheTakeEndsKeepsItsLength()
{
    // a held chord: both notes have their own offs, one after the other
    Take const take { { 60, 100, 0, 200 }, { 64, 100, 0, 200 }, { 60, 0, 1500, 200 }, { 64, 0, 1800, 200 } };
    CHECK(callFor(take, 0).autoOffMs == 0.0);
    CHECK(callFor(take, 1).autoOffMs == 0.0);
}

void aNoteTheTakeNeverEndsGetsItsOwnLength()
{
    // bare floats recorded as vel 100, no offs at all: 200 ms each
    Take const floats { { 60, 100, 0, 200 }, { 62, 100, 400, 200 } };
    auto const first = callFor(floats, 0);
    CHECK(first.pitch == 60 && first.vel == 100 && first.autoOffMs == 200.0);

    // the same pitch again before any off: the first on has no off of its own; the second does
    Take const retrigger { { 60, 100, 0, 150 }, { 60, 100, 300, 200 }, { 60, 0, 900, 200 } };
    CHECK(callFor(retrigger, 0).autoOffMs == 150.0);
    CHECK(callFor(retrigger, 1).autoOffMs == 0.0);

    // an off of ANOTHER pitch does not end it
    Take const other { { 60, 100, 0, 250 }, { 62, 100, 100, 200 }, { 62, 0, 400, 200 } };
    CHECK(callFor(other, 0).autoOffMs == 250.0);
    CHECK(callFor(other, 1).autoOffMs == 0.0);

    // an off that comes BEFORE the on is not its off
    Take const early { { 60, 0, 0, 200 }, { 60, 100, 300, 180 } };
    CHECK(callFor(early, 1).autoOffMs == 180.0);
}

void anUnusableLengthFallsBackToTheDefault()
{
    Take const take { { 60, 100, 0, 0 }, { 61, 100, 100, -50 }, { 62, 100, 200, NAN }, { 63, 100, 300, 1e9f } };
    for (int i = 0; i < 4; ++i)
        CHECK(callFor(take, i).autoOffMs == mcp::kAiMidiDefaultNoteMs);
    CHECK(mcp::kAiMidiDefaultNoteMs == 200.0);
}

void anIndexOutsideTheTakeSendsNothing()
{
    Take const take { { 60, 100, 0, 200 } };
    CHECK(callFor(take, -1).pitch == -1);
    CHECK(callFor(take, 1).pitch == -1);
    CHECK(mcp::aiMidiVoiceCall(static_cast<Note const*>(nullptr), 0, 0).pitch == -1);
    CHECK(mcp::aiMidiVoiceCall(static_cast<Note const*>(nullptr), 3, 0).pitch == -1);
}

} // namespace

int main()
{
    offEntriesAreOffs();
    velocityAndPitchAreClipped();
    aNoteTheTakeEndsKeepsItsLength();
    aNoteTheTakeNeverEndsGetsItsOwnLength();
    anUnusableLengthFallsBackToTheDefault();
    anIndexOutsideTheTakeSendsNothing();
    std::printf(failures ? "ai_midi_voice_test: %d FAILED\n" : "ai_midi_voice_test: OK\n", failures);
    return failures ? 1 : 0;
}
