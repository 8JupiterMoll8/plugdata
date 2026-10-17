/*
 // The GUI side of the MCP bridge (Plan 3 P3.2 S6). MCPBridge talks to the plugdata
 // GUI (editors, canvases, tabs, dialogs) only through this interface; the GUI
 // headers are included by Source/Mcp/GuiHost.cpp alone. No plugdata GUI type
 // appears in a signature: canvases are named by their t_canvas*, objects by t_gobj*.
 //
 // Threads. Every method is meant for the JUCE message thread. Callers that are
 // NOT on it today are marked "OFF-THREAD" below; they are listed in PROGRESS.md
 // for P3.4 and are left as they were (S6 is a move).
 */
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

extern "C" {
#include <m_pd.h>
}

class PluginProcessor;

class McpGuiHost {
public:
    virtual ~McpGuiHost() = default;

    // ── Canvas views ────────────────────────────────────────────────────
    // Selectable bounds of the first object component per gobj, in the view that
    // shows `cnv`; nullptr when no view shows it. Never opens a tab.
    // OFF-THREAD: the layout handlers call it from the OSC thread under sys_lock.
    using LiveBounds = std::unordered_map<t_gobj*, juce::Rectangle<int>>;
    virtual std::unique_ptr<LiveBounds> liveObjectBounds(t_canvas* cnv) = 0;

    // Zoom and viewport of the view that shows `cnv`; empty when none does.
    // OFF-THREAD: /pd/bounds (OSC thread).
    struct ViewState {
        float zoom = 1.0f;
        bool hasViewport = false;
        juce::Rectangle<float> viewArea;
    };
    virtual std::optional<ViewState> viewState(t_canvas* cnv) = 0;

    // Finds the view of `cnv`, opening a tab for it when no editor shows it.
    virtual bool ensureView(t_canvas* cnv) = 0;
    // ensureView, then repaint it. OFF-THREAD: /pd/ai_state (OSC thread).
    virtual void repaintView(t_canvas* cnv) = 0;
    // Repaints every canvas of every editor and invalidates the NVG surface.
    // OFF-THREAD: the MIDI take stop path (caller's thread).
    virtual void repaintAll() = 0;
    // Pans (never zooms) the view of `cnv` so freshly created objects are in sight,
    // when one of them landed outside it. Was batch_atomic's frameIfCreateOffscreen.
    virtual void frameCreatedObjects(t_canvas* cnv, std::vector<t_gobj*> const& created) = 0;
    // Overlay bit mask (Constants.h) of the view of `cnv`; ensureView semantics.
    virtual std::optional<int> overlayMask(t_canvas* cnv) = 0;
    virtual void setOverlayMask(t_canvas* cnv, int mask) = 0;
    virtual bool zoomToFit(t_canvas* cnv) = 0;
    // Optionally reselects `targets` (when non-null), then aligns the selection.
    // `mode` is the /pd/align keyword (left, right, hcentre, ...).
    virtual bool alignObjects(t_canvas* cnv, std::vector<t_gobj*> const* targets, juce::String const& mode) = 0;

    // Replaces the view's selection with `objects` (missing ones are skipped).
    struct SelectedObject {
        juce::String type;
        int index = 0;
        juce::Rectangle<int> bounds;
    };
    struct Selection {
        juce::String title;
        std::vector<SelectedObject> objects;
    };
    virtual std::optional<Selection> select(t_canvas* cnv, std::vector<t_gobj*> const& objects) = 0;

    // Encapsulate: number of distinct components for `objects`, or -1 when one of
    // them has no component. encapsulate() then wraps exactly those components.
    virtual int countViewObjects(t_canvas* cnv, std::vector<t_gobj*> const& objects) = 0;
    virtual void encapsulate(t_canvas* cnv, juce::String const& name, std::vector<t_gobj*> const& objects) = 0;

    // Connections of the view that touch `box`: src outlet -> dst inlet.
    struct ViewWire {
        t_gobj* src = nullptr;
        int outlet = 0;
        t_gobj* dst = nullptr;
        int inlet = 0;
    };
    virtual std::vector<ViewWire> viewWiresOf(t_canvas* cnv, t_gobj* box) = 0;

    // GUI undo/redo of an already open view (never opens a tab).
    enum class UndoResult { NoView, Empty, Done };
    virtual UndoResult undo(t_canvas* cnv) = 0;
    virtual bool redo(t_canvas* cnv) = 0; // false: no view

    // ── Screenshot ──────────────────────────────────────────────────────
    struct ScreenshotLabel {
        int x, y, w, h;    // pd coords
        juce::String text; // "name · class" or "class"
    };
    // Called with the t_canvas* of the view that was captured.
    using LabelSource = std::function<std::vector<ScreenshotLabel>(t_canvas* liveCnv)>;
    // Captures the view of `cnv` (or the focused canvas when `cnv` is null or has
    // no view) to a PNG temp file. Returns its path or "error:...".
    virtual juce::String screenshot(t_canvas* cnv, float scale, LabelSource const* labels, juce::String const& correlationId) = 0;
    virtual juce::Image captureGemWindow() = 0;

    // ── Focused canvas ──────────────────────────────────────────────────
    struct ViewportInfo {
        float zoom, panX, panY, width, height, renderScale;
    };
    virtual std::optional<ViewportInfo> focusedViewport() = 0;
    struct SketchInfo {
        juce::String strokesJson;
        juce::Rectangle<float> bounds; // relative to the canvas origin; empty when no sketch
        juce::StringArray selection;
    };
    virtual std::optional<SketchInfo> focusedSketch() = 0;

    // ── Tabs ────────────────────────────────────────────────────────────
    struct TabInfo {
        t_canvas* patch = nullptr;
        juce::String title;
        juce::File file;
        bool focused = false;
    };
    // OFF-THREAD: /pd/open_patch's duplicate check (OSC thread, under sys_lock).
    virtual std::vector<TabInfo> listTabs() = 0;
    virtual bool openPatch(juce::File const& file) = 0;
    // Keeps the first tab showing `file`, closes the others; logs each close.
    virtual void closeDuplicateTabs(juce::File const& file) = 0;
    // The first editor's first tab showing `file`.
    virtual t_canvas* findOpenPatch(juce::File const& file) = 0;
    // Matches by full path, file name or title. Returns the focused tab's title.
    virtual std::optional<juce::String> focusTab(juce::String const& target) = 0;
    struct CloseResult {
        enum Status { NotFound, Dirty, Closed } status = NotFound;
        juce::String title;
    };
    virtual CloseResult closeTab(juce::String const& target, bool force) = 0;
    // OFF-THREAD: /pd/save_as runs it on the Pd thread (enqueueFunctionAsync).
    virtual void setPatchFile(t_canvas* cnv, juce::File const& file) = 0;

    // ── Misc ────────────────────────────────────────────────────────────
    virtual bool showObjectReference(juce::String const& objectName) = 0;
    virtual bool openHelpPatch(juce::File const& helpFile) = 0;
    virtual void clearConsole() = 0;
    // OFF-THREAD: /pd/measure_text (OSC thread).
    virtual juce::Font defaultFont() = 0;
    // OFF-THREAD: /pd/perf (OSC thread).
    virtual float cpuUsage() = 0;
};

std::unique_ptr<McpGuiHost> createMcpGuiHost(PluginProcessor& processor);
