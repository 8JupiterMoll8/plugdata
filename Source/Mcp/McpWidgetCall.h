/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// Phase 3 of the ai_widget plan (docs/plans/AI-WIDGET-TOOL-PLAN-2026-11-21.md in the MCP repo), capability `widget_call`: the core of
// `/pd/widget_call <corrId> <widget> <paddr|-> <argc> <atoms...>`. JUCE-free and Pd-free (unit-tested by Tests/mcp/widget_call_test.cpp
// through scripts/test-bridge-safety.sh).
//
// Pd side (MCPBridgeWidgetCall.h, the handler MCPBridge::pdWidgetCall in MCPBridgeCmdControl.cpp): under the Pd lock, on the bridge's
// message-thread context, a collector is bound to the widget's state name (`ai-draw-state`), the atoms are sent to the widget's in-name
// (`ai-draw-in`) the way /pd/send_checked sends (so a pd-lua widget's own pd.Receive hears them, a canvas [r] too), the collector is
// unbound again and the frames it caught are the answer. The collector exists only inside the call: the audio thread never reaches it.
//
// This file holds what needs no Pd and no allocation:
//   - the floor: the first word of the message (after an optional `to <paddr>`) is one of dump, ghost, ghostn (ai-midi's note answer, 2026-11-24), ghostname, unghost, op
//     (flow control, 2026-11-25: `op <rev> [confirm] <word> <args...>`, the one word through which Alphi operates a widget while flow is on; the server decides flow, the
//     word table and the approval, the widget checks flow, the rev and the destructive words again; here it is only let through, and ONLY ADDRESSED: an op with no
//     `to <paddr>` would reach every instance of the widget family on that in-name). Anything else (do, clear, flow, beat, ...) is refused here, in C++, whatever the
//     server's own checks say. The address is `p` and 1..16
//     lower-case hex digits, the way a widget writes its object's address;
//   - the log: fixed storage for at most 64 frames of at most 32 atoms (the control window's numbers). Recording never locks, never
//     allocates and never calls JUCE (scripts/test-bridge-safety.sh greps this file for it): it runs wherever Pd dispatches. The 65th
//     frame and the 33rd atom of a frame are dropped and the log says `truncated`; a symbol longer than its room (47 bytes) is cut, on a
//     UTF-8 character boundary (never inside a multibyte character: the reply is read as UTF-8), and flagged. A request word is refused
//     above the same 47 bytes (kWidgetCallMaxArgChars), so a label the widget echoes in a frame never comes back cut short;
//   - the sets: the frames are cut at each `end` frame (a widget's state is `... end <rev> <paddr>`); the end frame is the last frame of
//     its set. Frames after the last `end` are no state (a dump that was cut): they are counted, never served.

#pragma once

#include "McpWidgetAsk.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mcp {

constexpr int kWidgetCallMaxFrames = 64;     // frames kept per call (the 65th is dropped and flagged)
constexpr int kWidgetCallMaxAtoms = 32;      // atoms kept per frame (the 33rd is dropped and flagged)
constexpr int kWidgetCallSymLen = 48;        // bytes kept of a selector or a symbol atom, the terminating NUL included
constexpr int kWidgetCallMaxArgs = 256;      // atoms of the request message, before the `to <paddr>` the bridge may put in front
constexpr int kWidgetCallMaxArgChars = kWidgetCallSymLen - 1; // bytes of one symbol of the request: what a captured frame carries back whole
constexpr int kWidgetCallAddrChars = 17;     // `p` and 16 hex digits, the terminating NUL not counted

// The address of a widget object as it writes it: `p` and 1..16 lower-case hex digits ("p55d0c8a3b2a0"). Compared as text, never read as a pointer.
inline bool widgetCallAddrOk(char const* s)
{
    if (!s || s[0] != 'p') return false;
    std::size_t const len = std::strlen(s);
    if (len < 2 || len > static_cast<std::size_t>(kWidgetCallAddrChars)) return false;
    for (std::size_t i = 1; i < len; ++i) {
        char const c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

// The words the AI may say to a widget through this verb: read it, propose on it, and (`op`, flow control) operate it while flow is on; nothing else.
inline bool widgetCallWordOk(char const* word)
{
    if (!word) return false;
    return std::strcmp(word, "dump") == 0 || std::strcmp(word, "ghost") == 0 || std::strcmp(word, "ghostn") == 0
        || std::strcmp(word, "ghostname") == 0 || std::strcmp(word, "unghost") == 0 || std::strcmp(word, "op") == 0;
}

// One atom of the request as the handler read it from the OSC message (a number, or a word kept by the caller for the length of the call).
struct WidgetCallArg {
    bool isSymbol = false;
    double num = 0.0;
    char const* sym = nullptr;
};

enum class WidgetCallRefusal : int { None = 0, NoWidget, Empty, TooManyAtoms, BadAtom, BadAddress, AddressTwice, Word, OpNeedsAddress };

inline char const* widgetCallRefusalText(WidgetCallRefusal r)
{
    switch (r) {
    case WidgetCallRefusal::None: return "";
    case WidgetCallRefusal::NoWidget: return "not an ai widget name (ai.<word>)";
    case WidgetCallRefusal::Empty: return "nothing to send (the message is empty)";
    case WidgetCallRefusal::TooManyAtoms: return "too many atoms in the message (at most 256)";
    case WidgetCallRefusal::BadAtom: return "an atom is not allowed (a number must be finite, a word is 1 to 47 bytes of printable characters)";
    case WidgetCallRefusal::BadAddress: return "the address is not p and 1 to 16 lower-case hex digits";
    case WidgetCallRefusal::AddressTwice: return "the address is given twice (as the address argument and as `to <paddr>` in the message)";
    case WidgetCallRefusal::Word: return "not an allowed widget word (only dump, ghost, ghostn, ghostname, unghost, op)";
    case WidgetCallRefusal::OpNeedsAddress: return "an op names one widget instance: it needs an address (to <paddr>), never every instance of the word";
    }
    return "refused";
}

struct WidgetCallPlan {
    WidgetCallRefusal refusal = WidgetCallRefusal::None;
    int bodyStart = 0;             // the index of the word (after the `to <paddr>` that was in the atoms, if any)
    bool addressInAtoms = false;   // the atoms already start with `to <paddr>`: the bridge puts nothing in front
};

// The floor. `paddr`: the verb's address argument (null or "-" = none). The word is checked on the INNER message; when `paddr` is given the
// bridge puts `to <paddr>` in front of the atoms itself, when the atoms already say `to <paddr>` it sends them as they are.
inline WidgetCallPlan widgetCallPlan(WidgetCallArg const* args, int argc, char const* paddr)
{
    WidgetCallPlan plan;
    bool const addressGiven = paddr && std::strcmp(paddr, "-") != 0;
    if (addressGiven && !widgetCallAddrOk(paddr)) {
        plan.refusal = WidgetCallRefusal::BadAddress;
        return plan;
    }
    if (argc <= 0 || !args) {
        plan.refusal = WidgetCallRefusal::Empty;
        return plan;
    }
    if (argc > kWidgetCallMaxArgs) {
        plan.refusal = WidgetCallRefusal::TooManyAtoms;
        return plan;
    }
    for (int i = 0; i < argc; ++i) {
        WidgetCallArg const& a = args[i];
        if (!a.isSymbol) {
            if (!std::isfinite(a.num)) {
                plan.refusal = WidgetCallRefusal::BadAtom;
                return plan;
            }
            continue;
        }
        if (!a.sym) {
            plan.refusal = WidgetCallRefusal::BadAtom;
            return plan;
        }
        std::size_t const len = std::strlen(a.sym);
        bool bad = len < 1 || len > static_cast<std::size_t>(kWidgetCallMaxArgChars);
        for (std::size_t k = 0; k < len && !bad; ++k) {
            auto const c = static_cast<unsigned char>(a.sym[k]);
            bad = c < 0x20 || c == 0x7f;
        }
        if (bad) {
            plan.refusal = WidgetCallRefusal::BadAtom;
            return plan;
        }
    }
    int at = 0;
    if (args[0].isSymbol && std::strcmp(args[0].sym, "to") == 0) {
        if (addressGiven) {
            plan.refusal = WidgetCallRefusal::AddressTwice;
            return plan;
        }
        if (argc < 2 || !args[1].isSymbol || !widgetCallAddrOk(args[1].sym)) {
            plan.refusal = WidgetCallRefusal::BadAddress;
            return plan;
        }
        plan.addressInAtoms = true;
        at = 2;
    }
    if (at >= argc) {
        plan.refusal = WidgetCallRefusal::Empty;
        return plan;
    }
    if (!args[at].isSymbol || !widgetCallWordOk(args[at].sym)) {
        plan.refusal = WidgetCallRefusal::Word;
        return plan;
    }
    // `op` operates ONE instance: with no address it would reach every widget of the family on that in-name.
    if (std::strcmp(args[at].sym, "op") == 0 && !addressGiven && !plan.addressInAtoms) {
        plan.refusal = WidgetCallRefusal::OpNeedsAddress;
        return plan;
    }
    plan.bodyStart = at;
    return plan;
}

struct WidgetCallAtom {
    bool isSymbol = false;
    double num = 0.0;
    char sym[kWidgetCallSymLen] = {};
};

struct WidgetCallFrame {
    int argc = 0;
    char selector[kWidgetCallSymLen] = {};
    WidgetCallAtom atoms[kWidgetCallMaxAtoms];
};

// A set of frames closed by an `end` frame: frames [first, first + count), the end frame last.
struct WidgetCallSet {
    int first = 0;
    int count = 0;
    bool hasRev = false;
    int rev = 0;             // the first atom of `end`, a whole number 0..9999999 (widgetAskRevOk)
    bool hasAddr = false;
    char addr[kWidgetCallAddrChars + 1] = {}; // the second atom of `end` when it is an address (widgetCallAddrOk)
};

// The frames one call caught. One producer at a time (whatever thread Pd dispatches on, under the Pd lock); the reader runs under the same lock
// after the collector is unbound. Fixed storage: beginFrame / addNumber / addSymbol never allocate, lock or call JUCE.
class WidgetCallLog {
public:
    void clear()
    {
        count_ = 0;
        open_ = false;
        truncated_ = false;
    }

    // A frame arrived: its selector (`d1`, `end`, `bang`, `list`, ...). Dropped and flagged when 64 frames are held.
    void beginFrame(char const* selector)
    {
        open_ = false;
        if (count_ >= kWidgetCallMaxFrames) {
            truncated_ = true;
            return;
        }
        WidgetCallFrame& f = frames_[count_];
        f.argc = 0;
        if (copyText(f.selector, selector ? selector : "")) truncated_ = true;
        ++count_;
        open_ = true;
    }

    void addNumber(double value)
    {
        WidgetCallAtom* a = nextAtom();
        if (!a) return;
        a->isSymbol = false;
        a->num = value;
        a->sym[0] = 0;
    }

    void addSymbol(char const* text)
    {
        WidgetCallAtom* a = nextAtom();
        if (!a) return;
        a->isSymbol = true;
        a->num = 0.0;
        if (copyText(a->sym, text ? text : "?")) truncated_ = true;
    }

    bool truncated() const { return truncated_; }
    int frameCount() const { return count_; }
    WidgetCallFrame const& frame(int i) const { return frames_[i]; }

    // The sets, in order (at most `maxSets`); `*unended` = the frames after the last `end` (served in no set). Reading only.
    int splitSets(WidgetCallSet* out, int maxSets, int* unended) const
    {
        int n = 0;
        int start = 0;
        for (int i = 0; i < count_ && n < maxSets; ++i) {
            WidgetCallFrame const& f = frames_[i];
            if (std::strcmp(f.selector, "end") != 0) continue;
            WidgetCallSet& s = out[n++];
            s = WidgetCallSet {};
            s.first = start;
            s.count = i - start + 1;
            if (f.argc >= 1 && !f.atoms[0].isSymbol && widgetAskRevOk(f.atoms[0].num)) {
                s.hasRev = true;
                s.rev = static_cast<int>(f.atoms[0].num);
            }
            if (f.argc >= 2 && f.atoms[1].isSymbol && widgetCallAddrOk(f.atoms[1].sym)) {
                s.hasAddr = true;
                std::strncpy(s.addr, f.atoms[1].sym, sizeof(s.addr) - 1);
                s.addr[sizeof(s.addr) - 1] = 0;
            }
            start = i + 1;
        }
        if (unended) *unended = count_ - start;
        return n;
    }

private:
    // The next free atom of the open frame; null (and `truncated`) when it holds 32, null when no frame is open.
    WidgetCallAtom* nextAtom()
    {
        if (!open_) return nullptr;
        WidgetCallFrame& f = frames_[count_ - 1];
        if (f.argc >= kWidgetCallMaxAtoms) {
            truncated_ = true;
            return nullptr;
        }
        return &f.atoms[f.argc++];
    }

    // `src` into a kWidgetCallSymLen buffer; true when it had to be cut. No printf: no locale, no formatting on the dispatch path.
    // The cut is on a UTF-8 character boundary: when the first byte left out is a continuation byte (0x80..0xBF) the cut lay inside a
    // multibyte character, so it steps back to that character's lead byte (at most 3 bytes: UTF-8 has no more continuation bytes) and the
    // whole character is left out. Text that is not UTF-8 (no lead byte within 3) is cut at the room, as it was.
    static bool copyText(char* dst, char const* src)
    {
        std::size_t i = 0;
        for (; src[i] && i < static_cast<std::size_t>(kWidgetCallSymLen - 1); ++i)
            dst[i] = src[i];
        if (!src[i]) {
            dst[i] = 0;
            return false;
        }
        std::size_t back = 0; // i is kWidgetCallSymLen - 1 here (> 3): src[i - back] is always inside the text
        while (back < 3 && isContinuationByte(src[i - back])) ++back;
        if (back > 0 && !isContinuationByte(src[i - back])) i -= back; // src[i - back] is the lead byte of the split character: cut before it
        dst[i] = 0;
        return true;
    }

    static bool isContinuationByte(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

    WidgetCallFrame frames_[kWidgetCallMaxFrames];
    int count_ = 0;
    bool open_ = false;
    bool truncated_ = false;
};

} // namespace mcp
