/*
 // A picture pasted onto a canvas: the clipboard read and the paste into the patch's refs/ folder. See McpPicturePaste.h.
 */
#include <juce_gui_basics/juce_gui_basics.h>
#include "Utility/Config.h"

#include "Mcp/McpPicturePaste.h"
#include "Mcp/McpRefsName.h"
#include "Canvas.h"
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "CanvasViewport.h"
#include "Pd/MCPBridge.h"
#include "Mcp/McpHost.h"

namespace mcp::paste {

namespace {
constexpr int maxClipboardPathChars = 4096;
#if JUCE_LINUX
constexpr int toolWaitMs = 3500; // a tool is `timeout 3`: 3 s, plus a little
constexpr int reapWaitMs = 500;  // after SIGKILL: the time waitpid gets to collect the shell
#endif

// Clipboard text as a picture: only a path to an image file ("file:///x.png" or "/x.png") is one.
juce::Image imageFromClipboardPath(juce::String const& clipboardText)
{
    if (clipboardText.length() >= maxClipboardPathChars)
        return {};
    juce::File file;
    if (clipboardText.startsWithIgnoreCase("file://"))
        file = juce::URL(clipboardText).getLocalFile();
    else if (juce::File::isAbsolutePath(clipboardText))
        file = juce::File(clipboardText);
    if (!file.existsAsFile())
        return {};
    return juce::ImageFileFormat::loadFrom(file); // invalid when the file is not a picture
}

#if JUCE_LINUX
// One clipboard tool, run through the shell into `tmp`. It is waited for at most toolWaitMs; a tool that is still running then is
// killed AND reaped (ChildProcess::isRunning is a waitpid, so waiting again after kill() collects the shell: no zombie).
juce::Image imageFromClipboardTool(juce::String const& command, juce::File const& tmp)
{
    tmp.deleteFile();
    juce::ChildProcess proc;
    if (!proc.start(juce::StringArray { "/bin/sh", "-c", command }))
        return {};
    if (!proc.waitForProcessToFinish(toolWaitMs)) {
        proc.kill();
        proc.waitForProcessToFinish(reapWaitMs);
    }
    if (!tmp.existsAsFile() || tmp.getSize() <= 0)
        return {};
    return juce::ImageFileFormat::loadFrom(tmp);
}

juce::Image imageFromClipboardTools()
{
    auto const tmp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("plugdata_paste_" + juce::String(juce::Random::getSystemRandom().nextInt64()) + ".img");
    auto const quotedPath = "'" + tmp.getFullPathName().replace("'", "'\\''") + "'";
    juce::StringArray const attempts {
        "timeout 3 xclip -selection clipboard -t image/png -o > " + quotedPath,
        "timeout 3 wl-paste --type image/png > " + quotedPath,
        "timeout 3 wl-paste > " + quotedPath
    };
    juce::Image result;
    for (auto const& command : attempts) {
        result = imageFromClipboardTool(command, tmp);
        if (result.isValid())
            break;
    }
    tmp.deleteFile();
    return result;
}
#endif

// Where the picture lands: at the mouse; with the mouse outside the visible part of the canvas, in the middle of what the artist sees.
juce::Point<int> pasteSpot(Canvas& canvas)
{
    auto mouse = canvas.getMouseXYRelative();
    if (canvas.viewport) {
        auto const view = canvas.viewport->getViewArea().toNearestInt();
        if (!view.contains(mouse))
            mouse = view.getCentre();
    }
    return mouse;
}

// The PNG on disk under `target`: only for a file that is not there, and no half-written file stays behind.
bool writePng(juce::File const& target, juce::Image const& image)
{
    juce::MemoryOutputStream pngData;
    if (!juce::PNGImageFormat().writeImageToStream(image, pngData) || target.exists())
        return false;
    bool created = false;
    bool written = false;
    {
        juce::FileOutputStream out(target);
        created = !out.failedToOpen();
        if (created && out.write(pngData.getData(), pngData.getDataSize())) {
            out.flush();
            written = out.getStatus().wasOk();
        }
    }
    if (!written && created)
        target.deleteFile();
    return written;
}
} // namespace

juce::Image imageFromClipboard()
{
    auto const clipboardText = juce::SystemClipboard::getTextFromClipboard().trim();
    if (clipboardText.isNotEmpty())
        return imageFromClipboardPath(clipboardText); // text: never a clipboard tool (message thread, up to 3 x 3.5 s)
#if JUCE_LINUX
    return imageFromClipboardTools();
#else
    return {};
#endif
}

bool canTakeRefsPicture(Canvas& canvas)
{
    // dragAndDropPaste unlocks the canvas and leaves presentation mode: a locked canvas keeps the glass picture instead.
    if (getValue<bool>(canvas.locked) || getValue<bool>(canvas.presentationMode))
        return false;
    auto const patchFile = canvas.patch.getCurrentFile();
    if (!patchFile.existsAsFile())
        return false;
    // The app's own folders (help patches, libraries, versions) are not the artist's patches; the artist's own projects
    // live under the app data dir too (e.g. ~/Documents/plugdata/Alphi Ideas/...), so only these system folders refuse.
    for (auto const* sys : { "Documentation", "Abstractions", "Extra", "Externals", "Library", "Versions", "Toolchain", "Deken" }) {
        if (patchFile.isAChildOf(ProjectInfo::appDataDir.getChildFile(sys)))
            return false;
    }
    return patchFile.getParentDirectory().hasWriteAccess();
}

bool pasteAsPicBox(Canvas& canvas, juce::Image const& picture)
{
    if (!canTakeRefsPicture(canvas))
        return false;
    auto const patchFile = canvas.patch.getCurrentFile();
    auto const refsDir = patchFile.getParentDirectory().getChildFile("refs");
    if (!refsDir.createDirectory().wasOk())
        return false;

    auto const name = mcp::refsFileName(patchFile.getFileNameWithoutExtension().toStdString(),
        [&refsDir](std::string const& candidate) { return refsDir.getChildFile(juce::String(candidate)).exists(); });
    if (name.empty())
        return false;

    auto const [fitW, fitH] = mcp::refsFitSize(picture.getWidth(), picture.getHeight());
    if (fitW <= 0 || fitH <= 0)
        return false;
    auto const stored = (fitW == picture.getWidth() && fitH == picture.getHeight())
        ? picture
        : picture.rescaled(fitW, fitH, juce::Graphics::highResamplingQuality);
    auto const target = refsDir.getChildFile(juce::String(name));
    if (!writePng(target, stored))
        return false;

    auto const mouse = pasteSpot(canvas);
    canvas.dragAndDropPaste(juce::String(mcp::refsPatchText(name)), mouse - canvas.canvasOrigin, fitW, fitH);

    // The AI is told a picture arrived by the same event as a glass reference image (/pd/reference/image <path> x y w h);
    // the artist reads it on the pill and in the sidebar.
    if (canvas.pd) {
        if (auto* bridge = canvas.pd->getMcp()->getMCPBridge()) {
            auto const at = mouse - canvas.canvasOrigin;
            bridge->sendReferenceImage(target.getFullPathName(), static_cast<float>(at.x - fitW / 2), static_cast<float>(at.y - fitH / 2),
                static_cast<float>(fitW), static_cast<float>(fitH));
            bridge->postChatMessage("ai", "Picture saved as refs/" + juce::String(name) + " and shown in a [pic] box.");
        }
    }
    return true;
}

} // namespace mcp::paste
