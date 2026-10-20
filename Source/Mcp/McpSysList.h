/*
 // Copyright (c) 2026 PlugData MCP Team
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

// The arguments of an mcp_* system command (Plan 3 P3.4 S7). Two sources:
// Pd's mcp_host receiver (atoms, symbols already interned by Pd) and the bridge
// (plain strings and numbers). The bridge used to build pd::Atom lists, so every
// correlation id it passed became a permanent Pd symbol (pd::Atom holds strings
// as t_symbol*), created off the Pd thread. A SysList keeps strings as strings.
// Handlers read it like the atom list they used to get: size(), [i].toString(),
// [i].getFloat(), with pd::Atom's conversions.
#pragma once

#include <initializer_list>
#include <vector>

#include <juce_core/juce_core.h>

#include "Pd/Instance.h"

namespace mcp {

struct SysArg {
    bool isNumber = false;
    float number = 0.0f;
    juce::String text;

    SysArg(float v) : isNumber(true), number(v) { }
    SysArg(int v) : isNumber(true), number(static_cast<float>(v)) { }
    SysArg(double v) : isNumber(true), number(static_cast<float>(v)) { }
    SysArg(juce::String s) : text(std::move(s)) { }
    SysArg(char const* s) : text(juce::String::fromUTF8(s)) { }

    juce::String toString() const { return isNumber ? juce::String(number) : text; } // as pd::Atom
    float getFloat() const { return isNumber ? number : 0.0f; }
};

class SysList {
public:
    SysList() = default;
    SysList(std::initializer_list<SysArg> init) : args(init) { }
    explicit SysList(std::vector<SysArg> v) : args(std::move(v)) { }
    explicit SysList(SmallArray<pd::Atom> const& atoms)
    {
        args.reserve(atoms.size());
        for (auto const& a : atoms) {
            if (a.isFloat())
                args.emplace_back(a.getFloat());
            else
                args.emplace_back(a.toString());
        }
    }

    int size() const { return static_cast<int>(args.size()); }
    SysArg const& operator[](int i) const { return args[static_cast<size_t>(i)]; }
    void add(SysArg a) { args.push_back(std::move(a)); }

private:
    std::vector<SysArg> args;
};

} // namespace mcp
