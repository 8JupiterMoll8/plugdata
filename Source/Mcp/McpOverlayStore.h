// The AI's regions, tags and ghosts, one list PER CANVAS (PARKED 2026-11-22, "overlay regions leak across tabs"), the JUCE-free
// part. They were ONE session-wide list: regions drawn on tab B (a copy of the patch on tab A, so the very same tempIds) replaced
// the regions tab A showed, because every canvas painted that one list and found its ids in its own boxes. Now each list belongs to
// the canvas it was written for. The key is the canvas pointer, opaque and never dereferenced (a t_canvas* from the bridge, the
// same pointer from a Canvas's patch in the paint); the store has no lock: McpSession wraps it in its overlay lock.
//
//  - get / size / empty: a canvas nothing was written for has an empty list;
//  - set: replaces a canvas's list; an empty list drops the entry; no canvas (nullptr) stores nothing;
//  - edit(key, fn): fn works on the canvas's list in place (push, erase by index, change a text); a list edited down to nothing drops
//    its entry. With no canvas fn gets a scratch vector (the caller needs no null check) and nothing is kept;
//  - erase / prune: a canvas closed, canvases that are gone (a freed canvas's address can be handed to a new one, so a stale list
//    must go with its canvas); clear: every list;
//  - flatten: every canvas's list one after the other (the moodboard rail lists every tag).
// Tested by Tests/mcp/overlay_store_test.cpp.
#pragma once

#include <cstddef>
#include <map>
#include <type_traits>
#include <utility>
#include <vector>

namespace mcp {

template <typename T>
class CanvasOverlayLists {
public:
    using Key = void const*;

    std::vector<T> get(Key canvas) const
    {
        auto const it = lists.find(canvas);
        return it != lists.end() ? it->second : std::vector<T>();
    }
    std::size_t size(Key canvas) const
    {
        auto const it = lists.find(canvas);
        return it != lists.end() ? it->second.size() : 0;
    }
    bool empty(Key canvas) const { return size(canvas) == 0; }
    std::size_t total() const
    {
        std::size_t n = 0;
        for (auto const& kv : lists) n += kv.second.size();
        return n;
    }

    void set(Key canvas, std::vector<T> items)
    {
        if (!canvas) return;
        if (items.empty()) lists.erase(canvas);
        else lists[canvas] = std::move(items);
    }

    template <typename F>
    auto edit(Key canvas, F&& fn) -> decltype(fn(std::declval<std::vector<T>&>()))
    {
        using R = decltype(fn(std::declval<std::vector<T>&>()));
        if (!canvas) {
            std::vector<T> scratch;
            return fn(scratch);
        }
        auto& list = lists[canvas];
        if constexpr (std::is_void_v<R>) {
            fn(list);
            if (list.empty()) lists.erase(canvas);
        } else {
            R result = fn(list);
            if (list.empty()) lists.erase(canvas);
            return result;
        }
    }

    void erase(Key canvas) { lists.erase(canvas); }
    void prune(std::vector<Key> const& gone)
    {
        for (auto const* canvas : gone) lists.erase(canvas);
    }
    void clear() { lists.clear(); }

    std::vector<Key> keys() const
    {
        std::vector<Key> out;
        out.reserve(lists.size());
        for (auto const& kv : lists) out.push_back(kv.first);
        return out;
    }
    std::vector<T> flatten() const
    {
        std::vector<T> out;
        for (auto const& kv : lists) out.insert(out.end(), kv.second.begin(), kv.second.end());
        return out;
    }

private:
    std::map<Key, std::vector<T>> lists; // ordered by pointer: flatten is deterministic within one run
};

} // namespace mcp
