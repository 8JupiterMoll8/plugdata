// The chip -> selection pill plumbing, the parts that need neither Pd nor JUCE (Source/Mcp/McpWidgetFocus.h).
// The artist's decision (2026-11-18): there is ONE way to talk to the AI about something on the canvas: select it, the
// selection pill opens, type, Enter. A widget's chip ("AI...") is a shortcut for that:
//  - typed text + Enter = a normal pill message (with the widget in the selection);
//  - Enter on NOTHING with exactly one ai.* widget selected = "complete it" (the widget asks, as its chip did before);
//  - Escape = close, nothing sent.
// pillEnterAction is that table; widgetOfBoxName tells which box is an askable widget (a class, its pd-lua shadow class, the
// legacy dashed class or the wrapper abstraction's file), so a widget the artist selected by hand completes the same way.
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpWidgetFocus.h"

#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

using mcp::PillOutcome;
using mcp::PillKey;

namespace {

void enterAndEscape()
{
    // typed text + Enter: a normal pill message, whatever the selection holds (a widget or not, one box or many)
    CHECK(mcp::pillAction(PillKey::Enter, false, 1, true) == PillOutcome::SendPrompt);
    CHECK(mcp::pillAction(PillKey::Enter, false, 1, false) == PillOutcome::SendPrompt);
    CHECK(mcp::pillAction(PillKey::Enter, false, 3, false) == PillOutcome::SendPrompt);
    // Enter on nothing: complete the widget, only when the selection is exactly one widget
    CHECK(mcp::pillAction(PillKey::Enter, true, 1, true) == PillOutcome::CompleteWidget);
    CHECK(mcp::pillAction(PillKey::Enter, true, 1, false) == PillOutcome::Dismiss); // one box, not a widget: nothing to send
    CHECK(mcp::pillAction(PillKey::Enter, true, 2, true) == PillOutcome::Dismiss);  // two boxes: no single widget to complete
    CHECK(mcp::pillAction(PillKey::Enter, true, 0, false) == PillOutcome::Dismiss);
    // Escape: close and send nothing, whatever was typed or selected
    for (bool empty : { true, false })
        for (int n : { 0, 1, 2 })
            for (bool w : { true, false })
                CHECK(mcp::pillAction(PillKey::Escape, empty, n, w) == PillOutcome::Dismiss);
}

void boxNames()
{
    // the dotted class, its pd-lua shadow class, the legacy dashed class, the wrapper abstraction's file
    CHECK(mcp::widgetOfBoxName("ai.tap") == "ai.tap");
    CHECK(mcp::widgetOfBoxName("ai.tap:gfx") == "ai.tap");
    CHECK(mcp::widgetOfBoxName("ai-tap") == "ai.tap");
    CHECK(mcp::widgetOfBoxName("ai-draw:gfx") == "ai.draw");
    CHECK(mcp::widgetOfBoxName("ai.dice.pd") == "ai.dice");
    CHECK(mcp::widgetOfBoxName("ai.knob.pd") == "ai.knob");
    for (char const* w : { "sketch", "knob", "dice", "grid", "paint", "strip", "listen", "tap", "draw" }) {
        CHECK(mcp::widgetOfBoxName((std::string("ai.") + w).c_str()) == std::string("ai.") + w);
        CHECK(mcp::widgetOfBoxName((std::string("ai-") + w + ":gfx").c_str()) == std::string("ai.") + w);
    }
    // not askable widgets: other boxes, the widgets without an ask chip, the loopers (their word names an instance), junk
    CHECK(mcp::widgetOfBoxName("osc~").empty());
    CHECK(mcp::widgetOfBoxName("pd").empty());
    CHECK(mcp::widgetOfBoxName("ai.image").empty());
    CHECK(mcp::widgetOfBoxName("ai-cam").empty());
    CHECK(mcp::widgetOfBoxName("ai-loop2").empty());
    CHECK(mcp::widgetOfBoxName("ai.loop~.pd").empty());
    CHECK(mcp::widgetOfBoxName("ai.loop_0").empty());
    CHECK(mcp::widgetOfBoxName("ai.tap.pd.pd").empty());
    CHECK(mcp::widgetOfBoxName("ai.tap:gfx:gfx").empty());
    CHECK(mcp::widgetOfBoxName("").empty());
    CHECK(mcp::widgetOfBoxName(nullptr).empty());
    CHECK(mcp::widgetOfBoxName("ai.").empty());
    CHECK(mcp::widgetOfBoxName("AI.tap").empty());
}

void luaChildNames()
{
    // inside a wrapper abstraction (ai.dice.pd holds [ai-dice]) the pd-lua object is the box named for the widget
    CHECK(mcp::isWidgetObjectName("ai-dice", "ai.dice"));
    CHECK(mcp::isWidgetObjectName("ai-dice:gfx", "ai.dice"));
    CHECK(mcp::isWidgetObjectName("ai.dice", "ai.dice"));
    CHECK(mcp::isWidgetObjectName("ai.dice:gfx", "ai.dice"));
    CHECK(!mcp::isWidgetObjectName("ai-knob", "ai.dice"));
    CHECK(!mcp::isWidgetObjectName("r", "ai.dice"));
    CHECK(!mcp::isWidgetObjectName("ai-dice.pd", "ai.dice"));
    CHECK(!mcp::isWidgetObjectName(nullptr, "ai.dice"));
}

} // namespace

int main()
{
    enterAndEscape();
    boxNames();
    luaChildNames();
    if (failures) {
        std::printf("widget_focus_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("widget_focus_test: ok\n");
    return 0;
}
