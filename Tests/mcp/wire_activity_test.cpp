// Unit tests for the /pd/wire_activity window (MCPBridgeWireActivity.h): JUCE-free, run by scripts/test-bridge-safety.sh.
#include "../../Source/Pd/MCPBridgeWireActivity.h"

#include <cstdio>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

using namespace mcp::wire;

namespace {
uint32_t fakeNow = 1000;
uint32_t clock() { return fakeNow; }
int wires[400];
char selFloat, selBang, symHi;
} // namespace

int main()
{
    // Nothing armed: record does nothing.
    {
        Table t;
        CHECK(!t.armed());
        t.record(&wires[0], &selFloat, ValueKind::Float, 1.0f, nullptr);
        Reading r;
        CHECK(!t.read(&wires[0], r));
    }

    // Armed over three wires (one duplicate, one null): counts, last value, time; an idle wire reads 0.
    {
        Table t;
        void const* keys[] = { &wires[5], &wires[1], nullptr, &wires[9], &wires[5] };
        fakeNow = 1000;
        CHECK(t.arm(keys, 5, fakeNow, 500, clock) == 3);
        CHECK(t.armed() && t.watched() == 3);
        fakeNow = 1100;
        t.record(&wires[1], &selFloat, ValueKind::Float, 0.25f, nullptr);
        fakeNow = 1200;
        t.record(&wires[1], &selFloat, ValueKind::Float, 0.75f, nullptr);
        t.record(&wires[9], &symHi, ValueKind::Symbol, 0.0f, &symHi);
        t.record(&wires[2], &selFloat, ValueKind::Float, 9.0f, nullptr); // not watched
        t.disarm();
        CHECK(!t.armed());
        t.record(&wires[1], &selFloat, ValueKind::Float, 5.0f, nullptr); // after the close: ignored
        Reading r;
        CHECK(t.read(&wires[1], r) && r.count == 2 && r.lastKind == ValueKind::Float && r.lastFloat == 0.75f);
        CHECK(r.lastSelector == &selFloat && r.lastAtMs == 200);
        CHECK(t.read(&wires[9], r) && r.count == 1 && r.lastKind == ValueKind::Symbol && r.lastSymbol == &symHi);
        CHECK(t.read(&wires[5], r) && r.count == 0 && r.lastKind == ValueKind::None && r.lastSelector == nullptr);
        CHECK(!t.read(&wires[2], r));
    }

    // The window ends by itself after windowMs; a bang has no value; the window length is clamped.
    {
        Table t;
        void const* keys[] = { &wires[3] };
        fakeNow = 5000;
        t.arm(keys, 1, fakeNow, 100, clock);
        CHECK(t.windowMs() == 100);
        fakeNow = 5050;
        t.record(&wires[3], &selBang, ValueKind::None, 0.0f, nullptr);
        fakeNow = 5101;
        t.record(&wires[3], &selFloat, ValueKind::Float, 3.0f, nullptr); // past the window
        Reading r;
        CHECK(t.read(&wires[3], r) && r.count == 1 && r.lastSelector == &selBang && r.lastKind == ValueKind::None);
        t.arm(keys, 1, fakeNow, 1, clock);
        CHECK(t.windowMs() == kMinWindowMs);
        t.arm(keys, 1, fakeNow, 100000, clock);
        CHECK(t.windowMs() == kMaxWindowMs);
        // Re-arm resets the readings.
        CHECK(t.read(&wires[3], r) && r.count == 0);
        // No clock or no keys: not armed.
        CHECK(t.arm(keys, 1, fakeNow, 100, nullptr) == 1 && !t.armed());
        CHECK(t.arm(keys, 0, fakeNow, 100, clock) == 0 && !t.armed());
    }

    // More than kMaxWires keys: the first kMaxWires kept, every one of them found.
    {
        Table t;
        std::vector<void const*> keys;
        for (auto& w : wires) keys.push_back(&w);
        fakeNow = 0;
        CHECK(t.arm(keys.data(), static_cast<int>(keys.size()), 0, 500, clock) == kMaxWires);
        int found = 0;
        for (int i = 0; i < kMaxWires; ++i) found += t.find(&wires[i]) >= 0;
        CHECK(found == kMaxWires);
        CHECK(t.find(&wires[kMaxWires + 1]) < 0);
    }

    if (failures == 0)
        std::printf("wire_activity_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
