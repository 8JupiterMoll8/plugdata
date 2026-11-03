// Unit tests for P11.9 item 1 (flow deletes only on the visible tab), JUCE-free:
//  - which canvases are on the artist's screen (McpVisibleTab.h onScreen): only a split view of a showing, not
//    minimised editor window; a plugin-mode editor shows its one canvas; a tab that is merely "current" in an editor
//    whose window is hidden is not on screen;
//  - what focus_tab may answer (focusOutcome): "focused" only when the asked tab is the editor's current canvas AND its
//    view is showing; otherwise it says it did not switch, and why.
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpVisibleTab.h"

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

using mcp::visible::EditorView;
using mcp::visible::Focus;

static void onScreenCases()
{
    std::printf("P11.9 on-screen canvases\n");
    int a = 0, b = 0, c = 0;
    void const* A = &a;
    void const* B = &b;
    void const* C = &c;

    // One window, one split showing A: A is on screen, B (another tab, not shown) is not.
    EditorView one;
    one.windowShowing = true;
    one.splits = { { A, true } };
    CHECK(mcp::visible::onScreen(A, { one }));
    CHECK(!mcp::visible::onScreen(B, { one }));

    // Split view: both halves are on screen.
    EditorView split = one;
    split.splits = { { A, true }, { B, true } };
    CHECK(mcp::visible::onScreen(A, { split }));
    CHECK(mcp::visible::onScreen(B, { split }));

    // A split whose view is not showing (the component tree says so) is not on screen.
    EditorView hiddenView = one;
    hiddenView.splits = { { A, false } };
    CHECK(!mcp::visible::onScreen(A, { hiddenView }));

    // The window minimised or hidden: nothing in it is on screen, though A is its "current" canvas.
    EditorView minimised = one;
    minimised.windowShowing = false;
    CHECK(!mcp::visible::onScreen(A, { minimised }));

    // Plugin mode shows its one canvas only, whatever the splits say.
    EditorView plugin = split;
    plugin.pluginModeCanvas = C;
    CHECK(mcp::visible::onScreen(C, { plugin }));
    CHECK(!mcp::visible::onScreen(A, { plugin }));

    // Two windows: on screen when any showing window shows it.
    CHECK(mcp::visible::onScreen(B, { minimised, split }));
    // No editor at all (headless, window closed): nothing is on screen.
    CHECK(!mcp::visible::onScreen(A, {}));
    CHECK(!mcp::visible::onScreen(nullptr, { one }));
}

static void focusCases()
{
    std::printf("P11.9 focus_tab outcome\n");
    int a = 0, b = 0;
    void const* A = &a;
    void const* B = &b;
    CHECK(mcp::visible::focusOutcome(A, A, true) == Focus::Switched);
    // The live check: the editor's current canvas did not become the asked tab.
    CHECK(mcp::visible::focusOutcome(A, B, true) == Focus::NotCurrent);
    CHECK(mcp::visible::focusOutcome(A, nullptr, true) == Focus::NotCurrent);
    // Current, but its view is not on screen (window minimised or hidden).
    CHECK(mcp::visible::focusOutcome(A, A, false) == Focus::NotShown);
    CHECK(std::string(mcp::visible::focusWords(Focus::Switched)) == "focused");
    CHECK(std::string(mcp::visible::focusWords(Focus::NotCurrent)).find("not switched") != std::string::npos);
    CHECK(std::string(mcp::visible::focusWords(Focus::NotShown)).find("not on screen") != std::string::npos);
}

int main()
{
    onScreenCases();
    focusCases();
    if (failures) {
        std::printf("visible_tab_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("visible_tab_test: all passed\n");
    return 0;
}
