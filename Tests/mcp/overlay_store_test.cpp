// Unit tests for the per-canvas AI overlay lists (Source/Mcp/McpOverlayStore.h), JUCE-free (PARKED 2026-11-22: "overlay regions
// leak across tabs"). Regions, tags and ghosts used to be ONE session-wide list: drawing regions on tab B (a copy of the patch
// on tab A, so the same tempIds) replaced the regions shown on tab A. Every list now belongs to one canvas, keyed by the canvas
// pointer (opaque, never dereferenced):
//  - two canvases that hold the very same tempIds keep their own lists: writing one never changes the other;
//  - a canvas nothing was written for has an empty list (and costs no entry); an empty write drops the entry;
//  - `edit` is the in-place access the tag editor needs (push, erase by index, change text) on THIS canvas's list only;
//  - `erase` (a canvas closed) and `prune` (canvases that are gone) drop only the named canvases, `clear` drops all;
//  - `flatten` is every canvas's list one after the other, in key order (the moodboard rail lists every tag).
// Build + run: scripts/test-bridge-safety.sh
#include "../../Source/Mcp/McpOverlayStore.h"

#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

namespace {

struct Region {
    std::string title;
    std::vector<std::string> targetIds;
    bool operator==(Region const& o) const { return title == o.title && targetIds == o.targetIds; }
};

int tabA = 0, tabB = 0, tabC = 0; // stand-ins for two or three canvases: only their addresses matter

void twoCanvasesWithTheSameIdsKeepSeparateLists()
{
    mcp::CanvasOverlayLists<Region> regions;
    Region const a1 { "2", { "osc_1", "lop_2" } };
    Region const a2 { "3", { "dly_3" } };
    // Tab B is a copy of tab A: the same tempIds, different titles.
    Region const b1 { "2 filter", { "osc_1", "lop_2" } };
    Region const b2 { "3 echo", { "dly_3" } };

    regions.set(&tabA, { a1, a2 });
    regions.set(&tabB, { b1, b2 });

    CHECK(regions.get(&tabA) == (std::vector<Region> { a1, a2 })); // B's write did not replace A's
    CHECK(regions.get(&tabB) == (std::vector<Region> { b1, b2 }));
    CHECK(regions.size(&tabA) == 2 && regions.size(&tabB) == 2 && regions.total() == 4);

    // Replacing A again leaves B alone (the order of the writes does not matter).
    regions.set(&tabA, { a2 });
    CHECK(regions.get(&tabA) == (std::vector<Region> { a2 }));
    CHECK(regions.get(&tabB) == (std::vector<Region> { b1, b2 }));
}

void aCanvasWithNothingIsEmptyAndCostsNoEntry()
{
    mcp::CanvasOverlayLists<Region> regions;
    CHECK(regions.get(&tabA).empty() && regions.empty(&tabA) && regions.size(&tabA) == 0);
    CHECK(regions.keys().empty() && regions.total() == 0);

    regions.set(&tabA, { Region { "x", {} } });
    CHECK(!regions.empty(&tabA) && regions.keys().size() == 1);
    regions.set(&tabA, {}); // the AI cleared its regions: the entry goes
    CHECK(regions.empty(&tabA) && regions.keys().empty());
    regions.set(nullptr, { Region { "no canvas", {} } }); // no canvas: nothing is stored
    CHECK(regions.keys().empty() && regions.get(nullptr).empty());
}

void editIsInPlaceOnOneCanvasOnly()
{
    mcp::CanvasOverlayLists<Region> tags;
    tags.set(&tabA, { Region { "one", {} }, Region { "two", {} } });
    tags.set(&tabB, { Region { "one", {} } });

    tags.edit(&tabA, [](std::vector<Region>& v) {
        v.push_back(Region { "three", {} });
        v[0].title = "changed";
        v.erase(v.begin() + 1);
    });
    CHECK(tags.get(&tabA) == (std::vector<Region> { Region { "changed", {} }, Region { "three", {} } }));
    CHECK(tags.get(&tabB) == (std::vector<Region> { Region { "one", {} } }));

    // edit hands back what the function returns, and a canvas edited down to nothing drops its entry.
    auto const n = tags.edit(&tabB, [](std::vector<Region>& v) { auto const count = v.size(); v.clear(); return count; });
    CHECK(n == 1 && tags.empty(&tabB) && tags.keys().size() == 1);
    // edit on a canvas with no list gives an empty vector to work on (a new tag), and keeps it.
    tags.edit(&tabC, [](std::vector<Region>& v) { CHECK(v.empty()); v.push_back(Region { "fresh", {} }); });
    CHECK(tags.size(&tabC) == 1);
    // edit without a canvas changes nothing.
    int calls = 0;
    tags.edit(nullptr, [&](std::vector<Region>& v) { ++calls; v.push_back(Region { "lost", {} }); });
    CHECK(tags.total() == 3 && calls == 1);
}

void closeAndPruneDropOnlyTheNamedCanvases()
{
    mcp::CanvasOverlayLists<Region> regions;
    regions.set(&tabA, { Region { "a", {} } });
    regions.set(&tabB, { Region { "b", {} } });
    regions.set(&tabC, { Region { "c", {} } });

    regions.erase(&tabB); // tab B closed: A and C keep theirs
    CHECK(regions.empty(&tabB) && regions.size(&tabA) == 1 && regions.size(&tabC) == 1);
    regions.erase(&tabB); // closing twice, or a canvas without a list: nothing happens
    regions.erase(nullptr);
    CHECK(regions.total() == 2);

    regions.prune({ &tabA, &tabB }); // two canvases that are gone (B already is)
    CHECK(regions.empty(&tabA) && regions.size(&tabC) == 1 && regions.keys().size() == 1);

    regions.clear();
    CHECK(regions.total() == 0 && regions.keys().empty());
}

void flattenListsEveryCanvasInKeyOrder()
{
    mcp::CanvasOverlayLists<Region> tags;
    tags.set(&tabA, { Region { "a1", {} }, Region { "a2", {} } });
    tags.set(&tabB, { Region { "b1", {} } });
    auto const all = tags.flatten();
    CHECK(all.size() == 3);
    // Every tag of every canvas is in (the order between canvases is the key order, within a canvas the list's).
    int a1 = -1, a2 = -1, b1 = -1;
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (all[i].title == "a1") a1 = static_cast<int>(i);
        if (all[i].title == "a2") a2 = static_cast<int>(i);
        if (all[i].title == "b1") b1 = static_cast<int>(i);
    }
    CHECK(a1 >= 0 && a2 == a1 + 1 && b1 >= 0);
}

void keysAreWhatPruneNeeds()
{
    mcp::CanvasOverlayLists<Region> tags;
    tags.set(&tabA, { Region { "a", {} } });
    tags.set(&tabC, { Region { "c", {} } });
    auto k = tags.keys();
    CHECK(k.size() == 2);
    bool hasA = false, hasC = false;
    for (auto const* key : k) { hasA = hasA || key == &tabA; hasC = hasC || key == &tabC; }
    CHECK(hasA && hasC);
}

} // namespace

int main()
{
    twoCanvasesWithTheSameIdsKeepSeparateLists();
    aCanvasWithNothingIsEmptyAndCostsNoEntry();
    editIsInPlaceOnOneCanvasOnly();
    closeAndPruneDropOnlyTheNamedCanvases();
    flattenListsEveryCanvasInKeyOrder();
    keysAreWhatPruneNeeds();
    if (failures != 0) {
        std::printf("overlay_store_test: %d FAILED\n", failures);
        return 1;
    }
    std::printf("overlay_store_test: OK\n");
    return 0;
}
