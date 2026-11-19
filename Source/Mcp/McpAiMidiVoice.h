// What [ai.midi] tells the hidden voice engine (ai-voices.pd, receiver `ai-voice-note`) about one entry of its take.
// Header-only and JUCE-free: no Pd, no JUCE, no allocation (Tests/mcp/ai_midi_voice_test.cpp).
//
// The take (the object's note list) holds note-ons (vel > 0) and, when the player released the key, note-offs (vel 0)
// as entries of their own, each with the dur_ms the recorder stamped (200). The engine needs an off for every on or its
// voice hangs. A note-on the take ends itself keeps its length: the take's own vel-0 entry is the off. A note-on the
// take never ends (a bare float, recorded as vel 100, has no off at all; an off the player's keyboard lost) gets an
// auto-off after its own dur_ms. The Pd side (ai_midi.c, patches/submodules/pd-else/0005) owns the clock for those.
#pragma once

#include <cmath>

namespace mcp {

// A note the recorder gave no usable length (or a silly one) lasts this long.
constexpr double kAiMidiDefaultNoteMs = 200.0;
constexpr double kAiMidiMaxNoteMs = 60000.0;

struct AiMidiVoiceCall {
    int pitch;         // 0..127, or -1 when there is nothing to send (an index outside the take)
    int vel;           // 0 = note-off; a note-on is 1..127
    double autoOffMs;  // > 0: a note-on the take never ends; the engine gets its off this long after the on. 0 = none
};

namespace detail {
inline int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
} // namespace detail

// Entry `i` of a take of `count` notes (any type with .pitch, .vel, .dur_ms), as one call to the engine.
template<class Note>
AiMidiVoiceCall aiMidiVoiceCall(Note const* notes, int count, int i)
{
    if (!notes || i < 0 || i >= count)
        return { -1, 0, 0.0 };
    int const pitch = detail::clampInt(static_cast<int>(notes[i].pitch), 0, 127);
    if (static_cast<int>(notes[i].vel) <= 0)
        return { pitch, 0, 0.0 };
    int const vel = detail::clampInt(static_cast<int>(notes[i].vel), 1, 127);
    // the take ends this note when the next entry of the same pitch is an off (vel 0)
    for (int j = i + 1; j < count; ++j) {
        if (detail::clampInt(static_cast<int>(notes[j].pitch), 0, 127) != pitch)
            continue;
        if (static_cast<int>(notes[j].vel) <= 0)
            return { pitch, vel, 0.0 };
        break; // another on of the same pitch comes first: this one was never ended
    }
    double const dur = static_cast<double>(notes[i].dur_ms);
    bool const usable = std::isfinite(dur) && dur > 0.0 && dur <= kAiMidiMaxNoteMs;
    return { pitch, vel, usable ? dur : kAiMidiDefaultNoteMs };
}

} // namespace mcp
