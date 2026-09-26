// Guarded access to this process' memory + float encoding helpers used by the scanner and the
// calibrators. Every raw read/write of foreign memory must go through safeRead/safeWrite.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace e2t::mem {

struct Range {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;  // exclusive

    std::size_t size() const noexcept { return end > begin ? end - begin : 0; }
    bool overlaps(std::uintptr_t address, std::size_t bytes) const noexcept {
        return address < end && address + bytes > begin;
    }
};

// SEH-protected copy; false if any byte could not be read.
bool safeRead(const void* source, void* destination, std::size_t bytes) noexcept;

// Writes only into committed, private, plain PAGE_READWRITE pages (no guard/nocache/
// write-combine), never into this module or the calling thread's stack. SEH-protected.
bool safeWrite(void* destination, const void* source, std::size_t bytes) noexcept;

template <class T>
bool readAt(std::uintptr_t address, T& out) noexcept {
    return safeRead(reinterpret_cast<const void*>(address), &out, sizeof(T));
}

template <class T>
bool writeAt(std::uintptr_t address, const T& value) noexcept {
    return safeWrite(reinterpret_cast<void*>(address), &value, sizeof(T));
}

// True for pages the scanner considers: MEM_COMMIT, MEM_PRIVATE and exactly PAGE_READWRITE.
bool isScannableProtection(std::uint32_t state, std::uint32_t type, std::uint32_t protect) noexcept;

Range moduleRangeOf(const void* addressInModule) noexcept;
Range currentModuleRange() noexcept;  // the module containing this code (DLL or test exe)
Range currentThreadStack() noexcept;

// Scannable regions of the whole user address space minus the given exclusions.
std::vector<Range> writableRegions(const std::vector<Range>& exclusions);
// This module + the calling thread's stack (our own copies of telemetry values live there).
std::vector<Range> defaultExclusions();

// ---- float helpers ------------------------------------------------------------------------

inline std::uint32_t floatBits(float value) noexcept {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
inline float floatFromBits(std::uint32_t bits) noexcept {
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
inline std::uint64_t doubleBits(double value) noexcept {
    std::uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
inline double doubleFromBits(std::uint64_t bits) noexcept {
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// Bit-level finiteness (no FP instructions, safe regardless of the thread's FP exception mask).
inline bool isFiniteFloatBits(std::uint32_t bits) noexcept {
    return (bits & 0x7F800000u) != 0x7F800000u;
}
inline bool isFiniteDoubleBits(std::uint64_t bits) noexcept {
    return (bits & 0x7FF0000000000000ull) != 0x7FF0000000000000ull;
}

// Inclusive range of double bit patterns that round (to nearest even) to one float value.
// Doubles of one sign are ordered like their bit patterns, so the set is contiguous.
struct DoubleRange {
    std::uint64_t lo = 1;
    std::uint64_t hi = 0;  // lo > hi: empty

    bool contains(std::uint64_t bits) const noexcept { return lo <= hi && bits - lo <= hi - lo; }
};
DoubleRange doubleRangeRoundingTo(float value) noexcept;

// Finite, >= minValue and at most maxTrailingZeroBits trailing zero mantissa bits, i.e. a
// value that is unlikely to appear by chance (400.0f is not distinctive, 347.318f is).
bool isDistinctiveFloat(float value, float minValue, int maxTrailingZeroBits) noexcept;
int trailingZeroMantissaBits(float value) noexcept;

}  // namespace e2t::mem
