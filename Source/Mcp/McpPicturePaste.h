/*
 // A picture pasted onto a canvas (Ctrl+V): the clipboard read and the paste into the patch's `refs/` folder. Moved out of
 // CanvasMcpTools.cpp (it had grown past 700 lines). The JUCE-free rules (names, sizes, the pasted object text) are in
 // McpRefsName.h (Tests/mcp/refs_name_test.cpp); this part needs JUCE and the canvas. Message thread only.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

class Canvas;

namespace mcp::paste {

// The picture on the system clipboard, or an invalid image. JUCE 7's SystemClipboard only speaks text, so:
//  1. text on the clipboard is the artist's text: the only text that becomes a picture is a path to an image file
//     (`file:///x.png`, `/x.png`), and for any other text this returns at once, WITHOUT starting a clipboard tool (a tool waits
//     up to 3.5 s on the message thread, and cannot be answered while PlugData itself owns the clipboard);
//  2. no text (a real image clipboard, or an empty one): Linux xclip / wl-paste, each bounded by `timeout` and reaped.
juce::Image imageFromClipboard();

// May this canvas take the picture as a file in a `refs/` folder? Only a normal saved user patch in a writable folder on an
// editable canvas: not an unsaved patch, not a patch under the app data dir (Documentation, Abstractions, Extra, Patches,
// ...), not a read-only folder, and not a locked / presentation canvas (the paste would unlock it).
bool canTakeRefsPicture(Canvas& canvas);

// Saves `picture` as <patch folder>/refs/<patch>-<n>.png and pastes one `[pic -open refs/<file>]` box at the mouse (the middle of
// the view when the mouse is outside it), then tells the AI (/pd/reference/image) and the artist (a chat line). False, with
// nothing changed, when canTakeRefsPicture says no or the file cannot be written: the caller then keeps the picture on the glass.
bool pasteAsPicBox(Canvas& canvas, juce::Image const& picture);

} // namespace mcp::paste
