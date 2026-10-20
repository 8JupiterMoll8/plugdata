/*
 // The fork's own GUI objects (Plan 3 P3.2 S8). The cases moved verbatim from the
 // factory switch in Source/Objects/ObjectBase.cpp; the include block mirrors that
 // file's, which the header-only object classes rely on.
 */
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "Utility/Config.h"
#include "Utility/Fonts.h"

#include "Objects/ObjectBase.h"

extern "C" {

#include <m_pd.h>
#include <g_canvas.h>
#include <m_imp.h>
#include <g_all_guis.h>
#include <g_undo.h>

#if _MSC_VER
__declspec(dllimport) void canvas_setgraph(t_glist* x, int flag, int nogoprect);
__declspec(dllimport) void canvas_click(t_canvas* x, t_floatarg xpos, t_floatarg ypos, t_floatarg shift, t_floatarg ctrl, t_floatarg alt);
#else
void canvas_setgraph(t_glist* x, int flag, int nogoprect);
void canvas_click(t_canvas* x, t_floatarg xpos, t_floatarg ypos, t_floatarg shift, t_floatarg ctrl, t_floatarg alt);
#endif
}

#include "AllGuis.h"
#include "Object.h"
#include "Iolet.h"
#include "Canvas.h"
#include "Components/SuggestionComponent.h"
#include "PluginEditor.h"
#include "LookAndFeel.h"
#include "TabComponent.h"
#include "Pd/Patch.h"
#include "Sidebar/Sidebar.h"
#include "Utility/CachedStringWidth.h"

#include "Dialogs/Dialogs.h"

#include "Objects/IEMHelper.h"
#include "Objects/AtomHelper.h"

// The header-only object classes lean on each other; same order as ObjectBase.cpp.
#include "Objects/TextObject.h"
#include "Objects/ToggleObject.h"
#include "Objects/MessageObject.h"
#include "Objects/BangObject.h"
#include "Objects/ButtonObject.h"
#include "Objects/RadioObject.h"
#include "Objects/SliderObject.h"
#include "Objects/KnobObject.h"
#include "Objects/GraphOnParent.h"
#include "Objects/ArrayObject.h"
#include "Objects/KeyboardObject.h"
#include "Objects/MessboxObject.h"
#include "Objects/MousePadObject.h"
#include "Objects/ParticlePadObject.h"
#include "Objects/AiMidiObject.h"
#include "Objects/AiVoiceObject.h"
#include "Mcp/ForkObjects.h"

ObjectBase* createForkObjectGui(hash32 const name, pd::WeakReference ptr, Object* parent)
{
    switch (name) {
        case hash("particlepad"):
        case hash("particles"):
            return new ParticlePadObject(ptr, parent);
        case hash("ai.midi"):
        case hash("aimidi"):
            return new AiMidiObject(ptr, parent);
        case hash("ai.voice"):
        case hash("aivoice"):
            return new AiVoiceObject(ptr, parent);
        default:
            return nullptr;
    }
}
