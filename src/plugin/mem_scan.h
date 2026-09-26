// Parallel full-process value scanner and a parallel in-place candidate filter.
//
// The scan runs synchronously (the caller - the game thread - blocks until all workers are
// joined), so the game is frozen and the values in memory stay consistent with the telemetry
// of the current frame. Every region chunk is copied out with safeRead (SEH) before matching.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "mem_access.h"

namespace e2t::mem {

constexpr std::size_t kDefaultMaxHitsPerTarget = 4'000'000;

enum class ScanKind : std::uint8_t {
    FloatBits,              // 4-byte aligned float, exact bit pattern
    DoubleRoundingToFloat,  // 8-byte aligned double d with (float)d == target
    DoubleTripleBits,       // 8-byte aligned double[3], exact bit patterns
    FloatTripleMagnitude,   // 4-byte aligned float[3] with |v|^2 in [min, max]
    DoubleTripleMagnitude,  // 8-byte aligned double[3] with |v|^2 in [min, max]
};

struct ScanTarget {
    ScanKind kind = ScanKind::FloatBits;
    std::uint32_t floatBits = 0;
    DoubleRange doubleRange{};
    std::uint64_t tripleBits[3] = {0, 0, 0};
    double minMagnitudeSq = 0.0;
    double maxMagnitudeSq = 0.0;
    std::size_t maxHits = kDefaultMaxHitsPerTarget;

    static ScanTarget exactFloat(float value) noexcept;
    static ScanTarget doubleRoundingTo(float value) noexcept;
    static ScanTarget exactDoubleTriple(const double (&xyz)[3]) noexcept;
    static ScanTarget floatTripleMagnitude(double minMagnitude, double maxMagnitude) noexcept;
    static ScanTarget doubleTripleMagnitude(double minMagnitude, double maxMagnitude) noexcept;
};

struct ScanResult {
    std::vector<std::uintptr_t> hits;  // sorted; empty when overflow is set
    bool overflow = false;             // more than maxHits matches: retry with a better value
};

struct ScanStats {
    std::size_t regions = 0;
    std::uint64_t bytes = 0;
    std::size_t faults = 0;  // chunks that could not be read (freed/changed concurrently)
    unsigned threads = 0;
    double milliseconds = 0.0;
};

// Scans all scannable regions of this process (see writableRegions), excluding this module,
// the calling thread's stack and the scanner's own buffers. One pass serves all targets.
std::vector<ScanResult> scanProcess(const std::vector<ScanTarget>& targets, ScanStats* stats);

// Same matching over caller-provided ranges (used by the self-test).
std::vector<ScanResult> scanRanges(const std::vector<Range>& ranges,
                                   const std::vector<ScanTarget>& targets, ScanStats* stats);

unsigned workerCount() noexcept;

namespace detail {
constexpr std::size_t kParallelFilterThreshold = 32'768;

// Runs job(0..workers-1): workers-1 new threads plus the calling thread; joins all before
// returning and rethrows the first exception thrown by a job.
void runParallel(unsigned workers, const std::function<void(unsigned)>& job);

template <class T, class Predicate>
std::size_t compactSlice(std::vector<T>& items, std::size_t begin, std::size_t end,
                         Predicate& keep) {
    std::size_t out = begin;
    for (std::size_t i = begin; i < end; ++i) {
        if (keep(items[i])) {
            if (out != i) {
                items[out] = std::move(items[i]);
            }
            ++out;
        }
    }
    return out - begin;
}
}  // namespace detail

// Keeps the items for which keep(T&) returns true (stable). keep may update the item and must
// be thread-safe; large vectors are processed by workerCount() threads.
template <class T, class Predicate>
void parallelFilter(std::vector<T>& items, Predicate keep) {
    const std::size_t count = items.size();
    const unsigned workers = count < detail::kParallelFilterThreshold ? 1u : workerCount();
    if (workers <= 1) {
        items.resize(detail::compactSlice(items, 0, count, keep));
        return;
    }
    const std::size_t slice = (count + workers - 1) / workers;
    std::vector<std::size_t> kept(workers, 0);
    detail::runParallel(workers, [&](unsigned worker) {
        const std::size_t begin = std::min(count, worker * slice);
        const std::size_t end = std::min(count, begin + slice);
        kept[worker] = detail::compactSlice(items, begin, end, keep);
    });
    std::size_t write = kept[0];
    for (unsigned worker = 1; worker < workers; ++worker) {
        const std::size_t begin = std::min(count, worker * slice);
        std::move(items.begin() + static_cast<std::ptrdiff_t>(begin),
                  items.begin() + static_cast<std::ptrdiff_t>(begin + kept[worker]),
                  items.begin() + static_cast<std::ptrdiff_t>(write));
        write += kept[worker];
    }
    items.resize(write);
}

}  // namespace e2t::mem
