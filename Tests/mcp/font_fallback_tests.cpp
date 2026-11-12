/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Font fallback cache tests (GUI CPU hunt 2026-11-12). A text with a character the UI font lacks was shaped on every
// repaint, and every shaping asked fontconfig for a fallback font (FcFontMatch, 3-7 ms) on the GUI and the OpenGL
// threads. patches/submodules/JUCE/0002 caches FreeTypeTypeface::createSystemFallback. This program counts FcFontMatch calls by defining the symbol
// here and forwarding to fontconfig's own (the JUCE code in the static libraries links against this definition).
//
//   cmake -S . -B build -DMCP_BRIDGE_TESTS=ON && cmake --build build --target font_fallback_tests
//   HOME=<scratch> build/font_fallback_tests
#include <dlfcn.h>
#include <fontconfig/fontconfig.h>

#include <atomic>
#include <cstdio>
#include <functional>
#include <thread>
#include <vector>

#include <juce_graphics/juce_graphics.h>
#include <juce_events/juce_events.h>

#ifndef PLUGDATA_FONTS_DIR
#    error "PLUGDATA_FONTS_DIR (Resources/Fonts) must be defined"
#endif

namespace {
std::atomic<int> fcFontMatchCalls { 0 };
}

extern "C" FcPattern* FcFontMatch(FcConfig* config, FcPattern* pattern, FcResult* result)
{
    using Fn = FcPattern* (*)(FcConfig*, FcPattern*, FcResult*);
    static Fn const real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "FcFontMatch"));
    ++fcFontMatchCalls;
    if (real == nullptr) {
        if (result != nullptr)
            *result = FcResultNoMatch;
        return nullptr;
    }
    return real(config, pattern, result);
}

namespace {

int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

juce::Typeface::Ptr loadFontFile(char const* fileName)
{
    juce::MemoryBlock data;
    auto const file = juce::File(PLUGDATA_FONTS_DIR).getChildFile(fileName);
    if (!file.loadFileAsData(data))
        return {};
    return juce::Typeface::createSystemTypefaceFor(data.getData(), data.getSize());
}

juce::String const blackCircle = juce::String::charToString(static_cast<juce::juce_wchar>(0x25CF)); // ● (not in Roboto Mono)
juce::String const undoArrow = juce::String::charToString(static_cast<juce::juce_wchar>(0x21B6));   // ↶ (not in Inter)
juce::String const noFontHasIt = juce::String::charToString(static_cast<juce::juce_wchar>(0x10FFFD)); // plane 16 private use

// The second lookup of the same (font, characters) asks fontconfig nothing and returns the same typeface.
void testSecondLookupIsCached(juce::Typeface::Ptr const& mono)
{
    std::printf("cache: second createSystemFallback for the same text does not call FcFontMatch\n");
    auto const before = fcFontMatchCalls.load();
    auto const first = mono->createSystemFallback(blackCircle, {});
    auto const afterFirst = fcFontMatchCalls.load();
    auto const second = mono->createSystemFallback(blackCircle, {});
    auto const afterSecond = fcFontMatchCalls.load();

    CHECK(afterFirst > before); // the first lookup really asked fontconfig (the counter works)
    CHECK(afterSecond == afterFirst);
    CHECK(first == second);
    CHECK(first != nullptr);
    std::printf("  FcFontMatch calls: first %d, second %d\n", afterFirst - before, afterSecond - afterFirst);

    // A different text is a different key.
    auto const beforeOther = fcFontMatchCalls.load();
    (void)mono->createSystemFallback(undoArrow, {});
    CHECK(fcFontMatchCalls.load() > beforeOther);
}

// A run nobody can draw is cached too (else such a text would still ask fontconfig on every repaint).
void testNegativeAnswerIsCached(juce::Typeface::Ptr const& mono)
{
    std::printf("cache: a text no font covers is asked once\n");
    auto const before = fcFontMatchCalls.load();
    auto const first = mono->createSystemFallback(noFontHasIt, {});
    auto const afterFirst = fcFontMatchCalls.load();
    for (int i = 0; i < 20; ++i)
        CHECK(mono->createSystemFallback(noFontHasIt, {}) == first);
    CHECK(afterFirst > before);
    CHECK(fcFontMatchCalls.load() == afterFirst);
}

// The path the repaint takes: shaping (Font::findSuitableFontForText) of a text with a missing glyph.
void testShapingPathIsCached(juce::Typeface::Ptr const& mono)
{
    std::printf("cache: repeated shaping of a glyph text calls FcFontMatch at most once\n");
    juce::Font const font { juce::FontOptions(mono).withHeight(14.0f) };
    auto const text = juce::String("AI drives ") + undoArrow + " " + blackCircle;
    auto const before = fcFontMatchCalls.load();
    for (int i = 0; i < 50; ++i) {
        juce::GlyphArrangement arrangement;
        arrangement.addLineOfText(font, text, 0.0f, 0.0f);
        CHECK(arrangement.getNumGlyphs() > 0);
    }
    auto const calls = fcFontMatchCalls.load() - before;
    std::printf("  FcFontMatch calls over 50 shapings: %d\n", calls);
    CHECK(calls <= 4); // ↶ and ● may each be asked once (with and without family); never per shaping
}

// Both threads that draw text (GUI and OpenGL) call it at once, starting from keys nobody asked before, so both
// threads miss and store the same keys at the same time.
void testConcurrentLookups(juce::Typeface::Ptr const& mono)
{
    std::printf("cache: two threads miss and store the same new keys at once and agree\n");
    juce::StringArray texts;
    for (juce::juce_wchar c = 0x2200; c < 0x2220; ++c) // mathematical operators: not asked by any earlier case
        texts.add(juce::String::charToString(c));

    std::atomic<bool> go { false };
    std::vector<juce::Typeface::Ptr> resultsA(static_cast<size_t>(texts.size())), resultsB(resultsA.size());
    std::atomic<int> unstable { 0 };
    auto worker = [&](std::vector<juce::Typeface::Ptr>& out) {
        while (!go.load())
            std::this_thread::yield();
        for (int round = 0; round < 100; ++round)
            for (int i = 0; i < texts.size(); ++i) {
                auto const r = mono->createSystemFallback(texts[i], {});
                auto& slot = out[static_cast<size_t>(i)];
                if (round == 0)
                    slot = r;
                else if (r != slot)
                    ++unstable;
            }
    };
    auto const before = fcFontMatchCalls.load();
    std::thread a(worker, std::ref(resultsA)), b(worker, std::ref(resultsB));
    go = true;
    a.join();
    b.join();
    auto const concurrentCalls = fcFontMatchCalls.load() - before;
    std::printf("  FcFontMatch calls for %d new keys from two threads: %d\n", texts.size(), concurrentCalls);

    CHECK(unstable.load() == 0);
    CHECK(concurrentCalls >= texts.size());           // they really were misses
    CHECK(concurrentCalls <= 2 * 2 * texts.size());   // at most both threads, with and without family, once per key
    auto const afterThreads = fcFontMatchCalls.load();
    for (int i = 0; i < texts.size(); ++i) {
        auto const i_ = static_cast<size_t>(i);
        CHECK(resultsA[i_] == resultsB[i_]);
        CHECK(mono->createSystemFallback(texts[i], {}) == resultsA[i_]);
    }
    CHECK(fcFontMatchCalls.load() == afterThreads);
}

// The cache is capped (1024 keys, then it starts over): past the cap the answers stay correct and are cached again.
void testCacheCap(juce::Typeface::Ptr const& mono)
{
    constexpr int keys = 1100;
    std::printf("cache: %d distinct keys (past the 1024 cap) stay correct\n", keys);
    int missingGlyph = 0;
    for (int i = 0; i < keys; ++i) {
        auto const cp = static_cast<juce::juce_wchar>(0x4E00 + i); // CJK ideographs
        auto const r = mono->createSystemFallback(juce::String::charToString(cp), {});
        if (r == nullptr || !r->getNominalGlyphForCodepoint(cp).has_value())
            ++missingGlyph;
    }
    std::printf("  keys without a glyph in their fallback: %d of %d\n", missingGlyph, keys);
    CHECK(missingGlyph == 0 || missingGlyph == keys); // a machine without a CJK font answers "none" for all, consistently

    // The newest keys (after the reset at 1024) are cached again: asking them twice costs nothing.
    auto const before = fcFontMatchCalls.load();
    for (int i = keys - 50; i < keys; ++i)
        (void)mono->createSystemFallback(juce::String::charToString(static_cast<juce::juce_wchar>(0x4E00 + i)), {});
    CHECK(fcFontMatchCalls.load() == before);
}

// The widget and AI-text symbols on a UI font that has none of them resolve through the cached system fallback:
// the first shaping of each asks fontconfig once (twice at most: JUCE retries without the family when the first
// match lacks the glyph), every later shaping asks nothing. Noto Emoji is the base here because no earlier case
// used it, so its cache keys are fresh.
void testWidgetSymbolsUseCachedSystemFallback(juce::Typeface::Ptr const& emoji)
{
    std::printf("fallback: ● ○ → ✓ ↵ ↶ resolve through the cached system fallback\n");
    juce::Font const font { juce::FontOptions(emoji).withHeight(14.0f) };

    for (auto const c : { 0x25CF, 0x25CB, 0x2192, 0x2713, 0x21B5, 0x21B6 }) {
        auto const text = juce::String::charToString(static_cast<juce::juce_wchar>(c));
        auto const before = fcFontMatchCalls.load();
        auto const found = font.findSuitableFontForText(text);
        auto const first = fcFontMatchCalls.load() - before;
        for (int i = 0; i < 50; ++i)
            (void)font.findSuitableFontForText(text);
        auto const later = fcFontMatchCalls.load() - before - first;
        std::printf("  U+%04X: FcFontMatch first %d, next 50 lookups %d, font %s\n", c, first, later, found.getTypefaceName().toRawUTF8());
        CHECK(first >= 1 && first <= 2);
        CHECK(later == 0);
        CHECK(found.getTypefacePtr() != nullptr && found.getTypefacePtr()->getNominalGlyphForCodepoint(static_cast<juce::juce_wchar>(c)).has_value());
    }
}

// Not a check: the cost per shaping lookup after the cache, preferred family (Inter, a memory font) vs the cached
// system fallback. JUCE's StoredMemoryFonts builds a fresh typeface (new FreeType face) for every preferred-family
// check, so a preferred family is not free either; this prints both, which is why Fonts.h does not put Inter first.
void benchmarkLookupPaths(juce::Typeface::Ptr const& mono, juce::Typeface::Ptr const& inter)
{
    constexpr int rounds = 500;
    auto timeIt = [&](juce::Font const& font, juce::String const& text) {
        (void)font.findSuitableFontForText(text); // warm
        auto const start = juce::Time::getMillisecondCounterHiRes();
        for (int i = 0; i < rounds; ++i)
            (void)font.findSuitableFontForText(text);
        return (juce::Time::getMillisecondCounterHiRes() - start) * 1000.0 / rounds;
    };
    juce::Font plain { juce::FontOptions(mono).withHeight(14.0f) };
    juce::Font interFirst = plain;
    interFirst.setPreferredFallbackFamilies({ inter->getName() });
    std::printf("bench: findSuitableFontForText per call, system fallback cached: %.1f us; Inter preferred: %.1f us\n",
        timeIt(plain, blackCircle), timeIt(interFirst, blackCircle));
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;

    auto const mono = loadFontFile("RobotoMonoVariable.ttf");
    auto const inter = loadFontFile("InterVariable.ttf");
    auto const emoji = loadFontFile("NotoEmoji.ttf");
    CHECK(mono != nullptr);
    CHECK(inter != nullptr);
    CHECK(emoji != nullptr);
    if (mono == nullptr || inter == nullptr || emoji == nullptr) {
        std::printf("font_fallback_tests: could not load the bundled fonts from %s\n", PLUGDATA_FONTS_DIR);
        return 1;
    }

    testSecondLookupIsCached(mono);
    testNegativeAnswerIsCached(mono);
    testShapingPathIsCached(mono);
    testConcurrentLookups(mono);
    testWidgetSymbolsUseCachedSystemFallback(emoji);
    testCacheCap(mono);
    benchmarkLookupPaths(mono, inter);

    if (failures == 0)
        std::printf("font_fallback_tests: all passed\n");
    else
        std::printf("font_fallback_tests: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
