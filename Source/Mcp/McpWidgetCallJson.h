/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// The reply of /pd/widget_call as JSON text (capability `widget_call`; the core: McpWidgetCall.h). JUCE-free and Pd-free, built AFTER the
// collector is unbound (it allocates; nothing here runs while the collector is bound). Tests/mcp/widget_call_test.cpp.
//
//   { "widget": "ai.draw", "listeners": 2,
//     "sets": [ { "rev": 4, "at": "p55d0c8a3b2a0", "frames": [ ["d1", 4, 2, 0.5], ..., ["end", 4, "p55d0c8a3b2a0"] ] }, ... ],
//     "truncated": false, "unended": 0 [, "refused": "<why>"] [, "error": "<why>"] }
//
// `listeners` is how many things listen on the widget's in-name (null: not known, the Pd context did not answer). `rev` / `at` are null
// when the widget's `end` frame carries none (an old widget). `unended` counts the frames after the last `end` (served in no set).
// `truncated` is true when a frame or an atom was cut, when the 65th frame was dropped, or when the reply was cut to stay under
// kWidgetCallMaxReplyBytes (one OSC message must fit one datagram): then the sets that fit are served whole and the rest is left out.

#pragma once

#include "../Pd/MCPJson.h"
#include "McpWidgetCall.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace mcp {

constexpr int kWidgetCallMaxReplyBytes = 40000; // the sets of one reply, in bytes of JSON text (the envelope comes on top)

struct WidgetCallReplyInfo {
    char const* widget = "";
    int listeners = 0;        // < 0: not known (written as null)
    char const* refused = ""; // non-empty: the floor (or a fail-closed check) said no; nothing was sent
    char const* error = "";   // non-empty: the call did not complete
};

// A number as a JSON number: a whole one without a point, a fraction with at most 7 significant digits (a float32 from Pd: 0.1, not
// 0.10000000149), an exponent form for the very large; JSON has no nan or inf, those are `null`. Never locale dependent.
inline void widgetCallNumberText(double v, char* out, std::size_t outLen)
{
    if (!out || outLen == 0) return;
    if (!std::isfinite(v)) {
        std::snprintf(out, outLen, "null");
        return;
    }
    if (v == std::floor(v) && std::fabs(v) < 1e15) {
        std::snprintf(out, outLen, "%.0f", v == 0.0 ? 0.0 : v);
        return;
    }
    std::snprintf(out, outLen, "%.7g", v);
    for (char* p = out; *p; ++p)
        if (*p == ',') *p = '.';
}

namespace calldetail {

inline void appendString(std::string& out, char const* text)
{
    out += '"';
    out += json::jsonEscape(text);
    out += '"';
}

inline void appendFrame(std::string& out, WidgetCallFrame const& f)
{
    out += '[';
    appendString(out, f.selector);
    for (int a = 0; a < f.argc; ++a) {
        out += ',';
        WidgetCallAtom const& at = f.atoms[a];
        if (at.isSymbol) {
            appendString(out, at.sym);
        } else {
            char num[40];
            widgetCallNumberText(at.num, num, sizeof(num));
            out += num;
        }
    }
    out += ']';
}

inline std::string setJson(WidgetCallLog const& log, WidgetCallSet const& s)
{
    std::string out = "{\"rev\":";
    out += s.hasRev ? std::to_string(s.rev) : std::string("null");
    out += ",\"at\":";
    if (s.hasAddr) appendString(out, s.addr);
    else out += "null";
    out += ",\"frames\":[";
    for (int i = 0; i < s.count; ++i) {
        if (i) out += ',';
        appendFrame(out, log.frame(s.first + i));
    }
    out += "]}";
    return out;
}

} // namespace calldetail

inline std::string widgetCallReplyJson(WidgetCallReplyInfo const& info, WidgetCallLog const* log)
{
    std::string out = "{\"widget\":";
    calldetail::appendString(out, info.widget ? info.widget : "");
    out += ",\"listeners\":";
    out += info.listeners < 0 ? std::string("null") : std::to_string(info.listeners);
    out += ",\"sets\":[";
    bool cut = log && log->truncated();
    int unended = 0;
    if (log) {
        WidgetCallSet sets[kWidgetCallMaxFrames];
        int const n = log->splitSets(sets, kWidgetCallMaxFrames, &unended);
        std::size_t used = 0;
        for (int i = 0; i < n; ++i) {
            std::string const one = calldetail::setJson(*log, sets[i]);
            if (used + one.size() + 1 > static_cast<std::size_t>(kWidgetCallMaxReplyBytes)) {
                cut = true;
                break;
            }
            if (i) out += ',';
            out += one;
            used += one.size() + 1;
        }
    }
    out += "],\"truncated\":";
    out += cut ? "true" : "false";
    out += ",\"unended\":";
    out += std::to_string(unended);
    if (info.refused && info.refused[0]) {
        out += ",\"refused\":";
        calldetail::appendString(out, info.refused);
    }
    if (info.error && info.error[0]) {
        out += ",\"error\":";
        calldetail::appendString(out, info.error);
    }
    out += '}';
    return out;
}

} // namespace mcp
