// Unit tests: float helpers, guarded memory access, scanner, parallel filter, SDK glue.
#include <windows.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "../mem_scan.h"
#include "../plugin_telemetry.h"
#include "harness.h"
#include "test_framework.h"

namespace e2t::test {
namespace {

constexpr std::size_t kPage = 4096;
constexpr std::size_t kSubChunk = 64u << 10;
constexpr std::size_t kWorkItem = 1u << 20;

struct Pages {
    explicit Pages(std::size_t bytes, DWORD protect = PAGE_READWRITE)
        : base(static_cast<std::uint8_t*>(
              VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, protect))),
          size(bytes) {}
    ~Pages() {
        if (base != nullptr) {
            VirtualFree(base, 0, MEM_RELEASE);
        }
    }
    Pages(const Pages&) = delete;
    Pages& operator=(const Pages&) = delete;
    mem::Range range() const {
        const auto begin = reinterpret_cast<std::uintptr_t>(base);
        return {begin, begin + size};
    }
    std::uintptr_t at(std::size_t offset) const {
        return reinterpret_cast<std::uintptr_t>(base) + offset;
    }
    std::uint8_t* base;
    std::size_t size;
};

template <class T>
void plant(const Pages& pages, std::size_t offset, const T& value) {
    std::memcpy(pages.base + offset, &value, sizeof(T));
}

bool contains(const std::vector<std::uintptr_t>& hits, std::uintptr_t address) {
    for (const std::uintptr_t hit : hits) {
        if (hit == address) {
            return true;
        }
    }
    return false;
}

bool roundsTo(std::uint64_t bits, float value) {
    return static_cast<float>(mem::doubleFromBits(bits)) == value;
}

}  // namespace

E2T_TEST(unit_double_range_matches_float_rounding) {
    const float values[] = {kDistinctiveFuel, 0.0312345f, 1.0e-5f, 123456.7f, -42.4242f,
                            1.0f, 1.0000001f, 3.0e20f, -0.75f};
    std::uint32_t seed = 0xC0FFEEu;
    std::vector<float> samples(std::begin(values), std::end(values));
    for (int i = 0; i < 2000; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const std::uint32_t bits = (seed & 0x807FFFFFu) | ((0x60u + (seed >> 24) % 0x40u) << 23);
        samples.push_back(mem::floatFromBits(bits));
    }
    int failures = 0;
    for (const float value : samples) {
        const mem::DoubleRange range = mem::doubleRangeRoundingTo(value);
        const bool exact = roundsTo(range.lo, value) && roundsTo(range.hi, value) &&
                           !roundsTo(range.lo - 1, value) && !roundsTo(range.hi + 1, value) &&
                           range.contains(mem::doubleBits(static_cast<double>(value)));
        failures += exact ? 0 : 1;
    }
    CHECK(failures == 0);
    CHECK(!mem::doubleRangeRoundingTo(std::numeric_limits<float>::quiet_NaN()).contains(0));
}

E2T_TEST(unit_distinctive_values) {
    constexpr int kMaxZeros = 11;
    CHECK(mem::isDistinctiveFloat(kDistinctiveFuel, 1.0f, kMaxZeros));
    CHECK(mem::isDistinctiveFloat(0.0312345f, 1.0e-5f, kMaxZeros));
    CHECK(!mem::isDistinctiveFloat(400.0f, 1.0f, kMaxZeros));
    CHECK(!mem::isDistinctiveFloat(600.0f, 1.0f, kMaxZeros));
    CHECK(!mem::isDistinctiveFloat(0.5f, 1.0e-5f, kMaxZeros));
    CHECK(!mem::isDistinctiveFloat(1.0e-6f, 1.0e-5f, kMaxZeros));
    CHECK(!mem::isDistinctiveFloat(0.9f, 1.0f, kMaxZeros));
    CHECK(!mem::isDistinctiveFloat(std::numeric_limits<float>::infinity(), 1.0f, kMaxZeros));
    CHECK(mem::trailingZeroMantissaBits(1.0f) == 23);
}

E2T_TEST(unit_scan_finds_planted_patterns) {
    const Pages pages(2 * kWorkItem);
    const std::size_t floatAt = 4 * 1000;
    const std::size_t doubleAt = 8 * 3000;
    const std::size_t tripleAt = 8 * 5000;
    const std::size_t straddleChunkAt = kSubChunk - 4;  // float3 across a 64 KiB chunk boundary
    const std::size_t straddleItemAt = kWorkItem - 8;   // double3 across a work item boundary
    plant(pages, floatAt, kDistinctiveFuel);
    plant(pages, doubleAt, 347.318);  // (float)347.318 == 347.318f
    const double triple[3] = {1.5, -2.25, 1000000.125};
    plant(pages, tripleAt, triple);
    const float floatVector[3] = {12.0f, 16.0f, 0.0f};  // |v| = 20
    plant(pages, straddleChunkAt, floatVector);
    const double doubleVector[3] = {0.0, -12.0, 16.0};
    plant(pages, straddleItemAt, doubleVector);

    const std::vector<mem::ScanTarget> targets = {
        mem::ScanTarget::exactFloat(kDistinctiveFuel),
        mem::ScanTarget::doubleRoundingTo(kDistinctiveFuel),
        mem::ScanTarget::exactDoubleTriple(triple),
        mem::ScanTarget::floatTripleMagnitude(19.9, 20.1),
        mem::ScanTarget::doubleTripleMagnitude(19.9, 20.1),
    };
    mem::ScanStats stats;
    const auto results = mem::scanRanges({pages.range()}, targets, &stats);
    CHECK(results.size() == targets.size());
    if (results.size() != targets.size()) {
        return;
    }
    CHECK(results[0].hits.size() == 1 && contains(results[0].hits, pages.at(floatAt)));
    CHECK(results[1].hits.size() == 1 && contains(results[1].hits, pages.at(doubleAt)));
    CHECK(results[2].hits.size() == 1 && contains(results[2].hits, pages.at(tripleAt)));
    CHECK(contains(results[3].hits, pages.at(straddleChunkAt)));
    CHECK(contains(results[4].hits, pages.at(straddleItemAt)));
    CHECK(stats.bytes == pages.size && stats.faults == 0);
}

E2T_TEST(unit_scan_reports_overflow_per_target) {
    const Pages pages(kSubChunk);
    for (std::size_t offset = 0; offset < pages.size; offset += sizeof(float)) {
        plant(pages, offset, kDistinctiveFuel);
    }
    plant(pages, 0, 1.25);  // one double, found despite the float overflow in the same pass
    mem::ScanTarget crowded = mem::ScanTarget::exactFloat(kDistinctiveFuel);
    crowded.maxHits = 100;
    const auto results = mem::scanRanges(
        {pages.range()}, {crowded, mem::ScanTarget::doubleRoundingTo(1.25f)}, nullptr);
    CHECK(results.size() == 2);
    if (results.size() == 2) {
        CHECK(results[0].overflow && results[0].hits.empty());
        CHECK(!results[1].overflow && contains(results[1].hits, pages.at(0)));
    }
}

E2T_TEST(unit_safe_access_guards) {
    float value = 0.0f;
    void* released = VirtualAlloc(nullptr, kPage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(released != nullptr);
    VirtualFree(released, 0, MEM_RELEASE);
    CHECK(!mem::safeRead(released, &value, sizeof(value)));
    CHECK(!mem::safeWrite(released, &value, sizeof(value)));

    const Pages readOnly(kPage, PAGE_READONLY);
    CHECK(mem::safeRead(readOnly.base, &value, sizeof(value)));
    CHECK(!mem::safeWrite(readOnly.base, &value, sizeof(value)));

    const Pages guarded(kPage, PAGE_READWRITE | PAGE_GUARD);
    CHECK(!mem::safeWrite(guarded.base, &value, sizeof(value)));

    float onStack = 1.0f;
    const float replacement = 2.0f;
    CHECK(!mem::safeWrite(&onStack, &replacement, sizeof(replacement)));  // own stack
    CHECK(onStack == 1.0f);

    const Pages writable(kPage);
    CHECK(mem::writeAt(writable.at(16), replacement));
    float readBack = 0.0f;
    CHECK(mem::readAt(writable.at(16), readBack) && readBack == replacement);

    const mem::Range stack = mem::currentThreadStack();
    bool overlapsStack = false;
    for (const mem::Range& region : mem::writableRegions(mem::defaultExclusions())) {
        overlapsStack = overlapsStack || stack.overlaps(region.begin, region.size());
    }
    CHECK(!overlapsStack);
}

E2T_TEST(unit_parallel_filter_is_stable) {
    std::vector<std::uint32_t> items(300'000);
    for (std::uint32_t i = 0; i < items.size(); ++i) {
        items[i] = i;
    }
    mem::parallelFilter(items, [](std::uint32_t& item) { return item % 3 == 0; });
    bool ordered = items.size() == 100'000;
    for (std::size_t i = 0; ordered && i < items.size(); ++i) {
        ordered = items[i] == i * 3;
    }
    CHECK(ordered);
    std::vector<int> small = {1, 2, 3, 4};
    mem::parallelFilter(small, [](int& item) { return item > 2; });
    CHECK(small.size() == 2 && small[0] == 3 && small[1] == 4);
}

E2T_TEST(unit_scan_process_finds_heap_value_not_stack) {
    const float distinctive = 271.828183f;
    const auto onHeap = std::make_unique<float>(distinctive);
    volatile float onStack = distinctive;
    const auto results = mem::scanProcess({mem::ScanTarget::exactFloat(distinctive)}, nullptr);
    CHECK(results.size() == 1);
    if (results.size() == 1) {
        CHECK(contains(results[0].hits, reinterpret_cast<std::uintptr_t>(onHeap.get())));
        CHECK(!contains(results[0].hits,
                        reinterpret_cast<std::uintptr_t>(const_cast<float*>(&onStack))));
    }
}

E2T_TEST(unit_truck_configuration_and_bridge_telemetry) {
    scs::NamedValue attributes[8] = {};
    const auto setString = [](scs::NamedValue& a, const char* name, const char* text) {
        a.name = name;
        a.index = scs::kU32Nil;
        a.value.type = scs::kValueString;
        a.value.asString = text;
    };
    setString(attributes[0], scs::config::kAttrBrandId, "scania");
    setString(attributes[1], scs::config::kAttrId, "s_2016");
    setString(attributes[2], scs::config::kAttrBrand, "Scania");
    setString(attributes[3], scs::config::kAttrName, "S");
    attributes[4].name = scs::config::kAttrFuelCapacity;
    attributes[4].value.type = scs::kValueFloat;
    attributes[4].value.asFloat = 1400.0f;
    attributes[5].name = scs::config::kAttrForwardGears;
    attributes[5].value.type = scs::kValueU32;
    attributes[5].value.asU32 = 12;

    static TelemetryState state;  // static like the plugin's state
    state = TelemetryState{};
    plugin::applyConfiguration({scs::config::kTruck, attributes}, state);
    CHECK(state.hasTruck);
    CHECK(std::strcmp(state.truckId, "scania.s_2016") == 0);
    CHECK(state.fuelCapacity == 1400.0f && state.gearsForward == 12);

    state.paused = false;
    state.trailerConnected = true;
    Telemetry telemetry{};
    plugin::fillBridgeTelemetry(state, false, telemetry);
    CHECK(telemetry.flags == (kFlagHasTruck | kFlagTrailerAttached));
    CHECK(std::strcmp(telemetry.truckBrand, "Scania") == 0);

    plugin::applyConfiguration({scs::config::kTruck, &attributes[7]}, state);  // empty: removed
    CHECK(!state.hasTruck && state.truckId[0] == '\0');
}

}  // namespace e2t::test
