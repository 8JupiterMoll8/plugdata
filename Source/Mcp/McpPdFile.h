// The file a Pd canvas is bound to, as juce::File (P11.8). Pure, no Pd lock: callers pass the strings they read.
#pragma once

#include <juce_core/juce_core.h>

namespace mcp {

// P11.8 item 3: a canvas dir is not always absolute (Pd keeps "." or "" for a patch that was never saved), and
// juce::File asserts on a relative path ("Illegal absolute path", juce_File.cpp:219, 390 times in one live session from
// McpHost::getCanvasBySymbol). The file `name` in `dir`, or an empty File when the dir is not absolute or the name is
// empty: such a canvas has no file to compare against.
inline juce::File fileInPdDir(juce::String const& dir, juce::String const& name)
{
    if (name.isEmpty() || dir.isEmpty() || !juce::File::isAbsolutePath(dir) || dir.startsWithChar('~'))
        return {};
    return juce::File(dir).getChildFile(name);
}

} // namespace mcp
