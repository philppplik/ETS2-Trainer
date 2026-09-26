#include "mem_scan.h"

#include <windows.h>
#include <xmmintrin.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <thread>

namespace e2t::mem {
namespace {

constexpr std::size_t kWorkItemBytes = 1u << 20;   // positions per work item (load balancing)
constexpr std::size_t kSubChunkBytes = 64u << 10;  // bytes copied out per guarded read
constexpr std::size_t kMaxPatternBytes = 24;       // double[3]
constexpr std::size_t kScratchStride = kSubChunkBytes + 4096;  // per worker, page padded
constexpr unsigned kMaxWorkers = 16;
constexpr unsigned kMxcsrFlushToZero = 0x8000u;
constexpr unsigned kMxcsrDenormalsAreZero = 0x0040u;
constexpr std::size_t kFloatBytes = 4;
constexpr std::size_t kDoubleBytes = 8;

using HitList = std::vector<std::uintptr_t>;

struct WorkItem {
    std::uintptr_t begin;  // first position
    std::uintptr_t end;    // positions end (exclusive)
    std::uintptr_t limit;  // readable end of the region (patterns may extend past `end`)
};

// Garbage memory is full of denormals; flush them so the magnitude math stays fast.
class FlushDenormalsGuard {
public:
    FlushDenormalsGuard() noexcept : saved_(_mm_getcsr()) {
        _mm_setcsr(saved_ | kMxcsrFlushToZero | kMxcsrDenormalsAreZero);
    }
    ~FlushDenormalsGuard() { _mm_setcsr(saved_); }
    FlushDenormalsGuard(const FlushDenormalsGuard&) = delete;
    FlushDenormalsGuard& operator=(const FlushDenormalsGuard&) = delete;

private:
    unsigned saved_;
};

// Worker copy buffers live in their own allocation so the scan can exclude them.
class ScratchBlock {
public:
    explicit ScratchBlock(std::size_t bytes)
        : base_(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)),
          bytes_(bytes) {
        if (base_ == nullptr) {
            throw std::bad_alloc();
        }
    }
    ~ScratchBlock() { VirtualFree(base_, 0, MEM_RELEASE); }
    ScratchBlock(const ScratchBlock&) = delete;
    ScratchBlock& operator=(const ScratchBlock&) = delete;

    std::uint8_t* data() const noexcept { return static_cast<std::uint8_t*>(base_); }
    Range range() const noexcept {
        const auto begin = reinterpret_cast<std::uintptr_t>(base_);
        return {begin, begin + bytes_};
    }

private:
    void* base_;
    std::size_t bytes_;
};

struct TargetState {
    std::atomic<std::size_t> hits{0};
    std::atomic<bool> overflow{false};
};

struct ScanJob {
    const std::vector<WorkItem>& items;
    const std::vector<ScanTarget>& targets;
    std::uint8_t* scratch;
    std::unique_ptr<TargetState[]> states;
    std::vector<std::vector<HitList>> hits;  // [worker][target]
    std::atomic<std::size_t> nextItem{0};
    std::atomic<std::size_t> faults{0};

    bool allTargetsOverflowed() const noexcept {
        for (std::size_t t = 0; t < targets.size(); ++t) {
            if (!states[t].overflow.load(std::memory_order_relaxed)) {
                return false;
            }
        }
        return true;
    }
};

// Number of aligned start positions p < positionBytes with p + pattern <= copied.
std::size_t positionCount(std::size_t positionBytes, std::size_t copied, std::size_t pattern,
                          std::size_t stride) noexcept {
    if (copied < pattern) {
        return 0;
    }
    const std::size_t byLimit = (positionBytes + stride - 1) / stride;
    const std::size_t byCopy = (copied - pattern) / stride + 1;
    return byLimit < byCopy ? byLimit : byCopy;
}

void matchFloatBits(const std::uint8_t* buffer, std::size_t count, std::uint32_t bits,
                    std::uintptr_t base, HitList& out) {
    for (std::size_t i = 0; i < count; ++i) {
        std::uint32_t word;
        std::memcpy(&word, buffer + i * kFloatBytes, sizeof(word));
        if (word == bits) {
            out.push_back(base + i * kFloatBytes);
        }
    }
}

void matchDoubleRange(const std::uint8_t* buffer, std::size_t count, const DoubleRange& range,
                      std::uintptr_t base, HitList& out) {
    const std::uint64_t span = range.hi - range.lo;
    for (std::size_t i = 0; i < count; ++i) {
        std::uint64_t word;
        std::memcpy(&word, buffer + i * kDoubleBytes, sizeof(word));
        if (word - range.lo <= span) {
            out.push_back(base + i * kDoubleBytes);
        }
    }
}

void matchDoubleTripleBits(const std::uint8_t* buffer, std::size_t count,
                           const std::uint64_t (&triple)[3], std::uintptr_t base, HitList& out) {
    for (std::size_t i = 0; i < count; ++i) {
        std::uint64_t words[3];
        std::memcpy(&words[0], buffer + i * kDoubleBytes, sizeof(words[0]));
        if (words[0] != triple[0]) {
            continue;
        }
        std::memcpy(&words[1], buffer + i * kDoubleBytes + kDoubleBytes, 2 * sizeof(words[0]));
        if (words[1] == triple[1] && words[2] == triple[2]) {
            out.push_back(base + i * kDoubleBytes);
        }
    }
}

void matchFloatTripleMagnitude(const std::uint8_t* buffer, std::size_t count, float minSq,
                               float maxSq, std::uintptr_t base, HitList& out) {
    for (std::size_t i = 0; i < count; ++i) {
        float v[3];
        std::memcpy(v, buffer + i * kFloatBytes, sizeof(v));
        const float magnitudeSq = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        if (magnitudeSq >= minSq && magnitudeSq <= maxSq) {
            out.push_back(base + i * kFloatBytes);
        }
    }
}

void matchDoubleTripleMagnitude(const std::uint8_t* buffer, std::size_t count, double minSq,
                                double maxSq, std::uintptr_t base, HitList& out) {
    for (std::size_t i = 0; i < count; ++i) {
        double v[3];
        std::memcpy(v, buffer + i * kDoubleBytes, sizeof(v));
        const double magnitudeSq = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        if (magnitudeSq >= minSq && magnitudeSq <= maxSq) {
            out.push_back(base + i * kDoubleBytes);
        }
    }
}

void matchTarget(const ScanTarget& target, const std::uint8_t* buffer, std::size_t positionBytes,
                 std::size_t copied, std::uintptr_t base, HitList& out) {
    switch (target.kind) {
        case ScanKind::FloatBits:
            matchFloatBits(buffer, positionCount(positionBytes, copied, kFloatBytes, kFloatBytes),
                           target.floatBits, base, out);
            break;
        case ScanKind::DoubleRoundingToFloat:
            matchDoubleRange(buffer,
                             positionCount(positionBytes, copied, kDoubleBytes, kDoubleBytes),
                             target.doubleRange, base, out);
            break;
        case ScanKind::DoubleTripleBits:
            matchDoubleTripleBits(
                buffer, positionCount(positionBytes, copied, 3 * kDoubleBytes, kDoubleBytes),
                target.tripleBits, base, out);
            break;
        case ScanKind::FloatTripleMagnitude:
            matchFloatTripleMagnitude(
                buffer, positionCount(positionBytes, copied, 3 * kFloatBytes, kFloatBytes),
                static_cast<float>(target.minMagnitudeSq),
                static_cast<float>(target.maxMagnitudeSq), base, out);
            break;
        case ScanKind::DoubleTripleMagnitude:
            matchDoubleTripleMagnitude(
                buffer, positionCount(positionBytes, copied, 3 * kDoubleBytes, kDoubleBytes),
                target.minMagnitudeSq, target.maxMagnitudeSq, base, out);
            break;
    }
}

void scanChunk(ScanJob& job, const std::uint8_t* buffer, std::size_t positionBytes,
               std::size_t copied, std::uintptr_t base, std::vector<HitList>& hits) {
    for (std::size_t t = 0; t < job.targets.size(); ++t) {
        TargetState& state = job.states[t];
        if (state.overflow.load(std::memory_order_relaxed)) {
            continue;
        }
        const std::size_t before = hits[t].size();
        matchTarget(job.targets[t], buffer, positionBytes, copied, base, hits[t]);
        const std::size_t added = hits[t].size() - before;
        if (added == 0) {
            continue;
        }
        const std::size_t total = state.hits.fetch_add(added, std::memory_order_relaxed) + added;
        if (total > job.targets[t].maxHits) {
            state.overflow.store(true, std::memory_order_relaxed);
            HitList().swap(hits[t]);
        }
    }
}

void scanWorker(ScanJob& job, unsigned worker) {
    const FlushDenormalsGuard denormals;
    std::uint8_t* buffer = job.scratch + static_cast<std::size_t>(worker) * kScratchStride;
    std::vector<HitList>& hits = job.hits[worker];
    for (;;) {
        if (job.allTargetsOverflowed()) {
            return;
        }
        const std::size_t index = job.nextItem.fetch_add(1, std::memory_order_relaxed);
        if (index >= job.items.size()) {
            return;
        }
        const WorkItem& item = job.items[index];
        for (std::uintptr_t position = item.begin; position < item.end;
             position += kSubChunkBytes) {
            const std::size_t positionBytes = std::min(kSubChunkBytes, item.end - position);
            const std::size_t copied = std::min(positionBytes + kMaxPatternBytes,
                                                item.limit - position);
            if (!safeRead(reinterpret_cast<const void*>(position), buffer, copied)) {
                job.faults.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            scanChunk(job, buffer, positionBytes, copied, position, hits);
        }
    }
}

std::vector<WorkItem> splitIntoWorkItems(const std::vector<Range>& ranges) {
    std::vector<WorkItem> items;
    for (const Range& range : ranges) {
        for (std::uintptr_t begin = range.begin; begin < range.end; begin += kWorkItemBytes) {
            const std::uintptr_t end = std::min(range.end, begin + kWorkItemBytes);
            items.push_back({begin, end, range.end});
        }
    }
    return items;
}

std::vector<ScanResult> mergeResults(ScanJob& job) {
    std::vector<ScanResult> results(job.targets.size());
    for (std::size_t t = 0; t < job.targets.size(); ++t) {
        if (job.states[t].overflow.load()) {
            results[t].overflow = true;
            continue;
        }
        std::size_t total = 0;
        for (const auto& workerHits : job.hits) {
            total += workerHits[t].size();
        }
        HitList& merged = results[t].hits;
        merged.reserve(total);
        for (auto& workerHits : job.hits) {
            merged.insert(merged.end(), workerHits[t].begin(), workerHits[t].end());
            HitList().swap(workerHits[t]);
        }
        std::sort(merged.begin(), merged.end());
    }
    return results;
}

std::vector<ScanResult> scanWithScratch(const std::vector<Range>& ranges,
                                        const std::vector<ScanTarget>& targets,
                                        const ScratchBlock& scratch, unsigned workers,
                                        ScanStats* stats) {
    const auto started = std::chrono::steady_clock::now();
    const std::vector<WorkItem> items = splitIntoWorkItems(ranges);
    ScanJob job{items, targets, scratch.data(), std::make_unique<TargetState[]>(targets.size()),
                std::vector<std::vector<HitList>>(workers, std::vector<HitList>(targets.size()))};
    detail::runParallel(workers, [&job](unsigned worker) { scanWorker(job, worker); });
    std::vector<ScanResult> results = mergeResults(job);

    if (stats != nullptr) {
        stats->regions = ranges.size();
        stats->bytes = 0;
        for (const Range& range : ranges) {
            stats->bytes += range.size();
        }
        stats->faults = job.faults.load();
        stats->threads = workers;
        stats->milliseconds = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - started)
                                  .count();
    }
    return results;
}

}  // namespace

ScanTarget ScanTarget::exactFloat(float value) noexcept {
    ScanTarget target;
    target.kind = ScanKind::FloatBits;
    target.floatBits = mem::floatBits(value);
    return target;
}

ScanTarget ScanTarget::doubleRoundingTo(float value) noexcept {
    ScanTarget target;
    target.kind = ScanKind::DoubleRoundingToFloat;
    target.doubleRange = doubleRangeRoundingTo(value);
    return target;
}

ScanTarget ScanTarget::exactDoubleTriple(const double (&xyz)[3]) noexcept {
    ScanTarget target;
    target.kind = ScanKind::DoubleTripleBits;
    for (int i = 0; i < 3; ++i) {
        target.tripleBits[i] = mem::doubleBits(xyz[i]);
    }
    return target;
}

ScanTarget ScanTarget::floatTripleMagnitude(double minMagnitude, double maxMagnitude) noexcept {
    ScanTarget target;
    target.kind = ScanKind::FloatTripleMagnitude;
    target.minMagnitudeSq = minMagnitude * minMagnitude;
    target.maxMagnitudeSq = maxMagnitude * maxMagnitude;
    return target;
}

ScanTarget ScanTarget::doubleTripleMagnitude(double minMagnitude, double maxMagnitude) noexcept {
    ScanTarget target = floatTripleMagnitude(minMagnitude, maxMagnitude);
    target.kind = ScanKind::DoubleTripleMagnitude;
    return target;
}

unsigned workerCount() noexcept {
    const unsigned hardware = std::thread::hardware_concurrency();
    return std::max(1u, std::min(hardware, kMaxWorkers));
}

std::vector<ScanResult> scanProcess(const std::vector<ScanTarget>& targets, ScanStats* stats) {
    if (targets.empty()) {
        return {};
    }
    const unsigned workers = workerCount();
    const ScratchBlock scratch(static_cast<std::size_t>(workers) * kScratchStride);
    std::vector<Range> exclusions = defaultExclusions();
    exclusions.push_back(scratch.range());
    return scanWithScratch(writableRegions(exclusions), targets, scratch, workers, stats);
}

std::vector<ScanResult> scanRanges(const std::vector<Range>& ranges,
                                   const std::vector<ScanTarget>& targets, ScanStats* stats) {
    if (targets.empty()) {
        return {};
    }
    const unsigned workers = workerCount();
    const ScratchBlock scratch(static_cast<std::size_t>(workers) * kScratchStride);
    return scanWithScratch(ranges, targets, scratch, workers, stats);
}

namespace detail {

void runParallel(unsigned workers, const std::function<void(unsigned)>& job) {
    std::exception_ptr failure;
    std::mutex failureMutex;
    auto guarded = [&](unsigned index) {
        try {
            job(index);
        } catch (...) {
            const std::lock_guard<std::mutex> lock(failureMutex);
            if (!failure) {
                failure = std::current_exception();
            }
        }
    };

    std::vector<std::thread> threads;
    unsigned started = 0;
    try {
        threads.reserve(workers > 0 ? workers - 1 : 0);
        for (unsigned index = 1; index < workers; ++index) {
            threads.emplace_back(guarded, index);
            ++started;
        }
    } catch (...) {
        // Could not create all threads: the calling thread runs the remaining slices below.
    }
    guarded(0);
    for (unsigned index = 1 + started; index < workers; ++index) {
        guarded(index);
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace detail

}  // namespace e2t::mem
