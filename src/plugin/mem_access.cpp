#include "mem_access.h"

#include <windows.h>

#include <cmath>

namespace e2t::mem {
namespace {

constexpr DWORD kBaseProtectionMask = 0xFF;
constexpr DWORD kRejectedProtectionModifiers = PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE;
constexpr std::uint32_t kFloatSignBit = 0x80000000u;
constexpr std::uint32_t kFloatMantissaMask = 0x007FFFFFu;
constexpr std::uint32_t kFloatExponentMask = 0x7F800000u;
constexpr int kFloatMantissaBits = 23;
constexpr std::uint64_t kDoubleSignBit = 0x8000000000000000ull;
// Above this the rounding band touches FLT_MAX/inf; such values are never calibration targets.
constexpr float kMaxBandMagnitude = 1.0e30f;

// Plain function without destructible objects so that __try is allowed.
bool guardedCopy(void* destination, const void* source, std::size_t bytes) noexcept {
    __try {
        std::memcpy(destination, source, bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool pagesAreWritable(std::uintptr_t address, std::size_t bytes) noexcept {
    std::uintptr_t cursor = address;
    const std::uintptr_t end = address + bytes;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) == 0) {
            return false;
        }
        if (!isScannableProtection(info.State, info.Type, info.Protect)) {
            return false;
        }
        const std::uintptr_t next = reinterpret_cast<std::uintptr_t>(info.BaseAddress) +
                                    info.RegionSize;
        if (next <= cursor) {
            return false;
        }
        cursor = next;
    }
    return true;
}

void appendMinusExclusions(std::vector<Range>& out, Range region,
                           const std::vector<Range>& exclusions) {
    std::vector<Range> pieces{region};
    for (const Range& excluded : exclusions) {
        std::vector<Range> next;
        for (const Range& piece : pieces) {
            if (!excluded.overlaps(piece.begin, piece.size())) {
                next.push_back(piece);
                continue;
            }
            if (piece.begin < excluded.begin) {
                next.push_back({piece.begin, excluded.begin});
            }
            if (excluded.end < piece.end) {
                next.push_back({excluded.end, piece.end});
            }
        }
        pieces.swap(next);
    }
    for (const Range& piece : pieces) {
        if (!out.empty() && out.back().end == piece.begin) {
            out.back().end = piece.end;  // merge contiguous regions (patterns may straddle)
        } else if (piece.size() > 0) {
            out.push_back(piece);
        }
    }
}

std::uint64_t highestBitsRoundingTo(std::uint64_t start, float magnitude) noexcept {
    std::uint64_t bits = start;
    while (static_cast<float>(doubleFromBits(bits)) != magnitude) {
        --bits;
    }
    while (static_cast<float>(doubleFromBits(bits + 1)) == magnitude) {
        ++bits;
    }
    return bits;
}

std::uint64_t lowestBitsRoundingTo(std::uint64_t start, float magnitude) noexcept {
    std::uint64_t bits = start;
    while (static_cast<float>(doubleFromBits(bits)) != magnitude) {
        ++bits;
    }
    while (bits > 0 && static_cast<float>(doubleFromBits(bits - 1)) == magnitude) {
        --bits;
    }
    return bits;
}

}  // namespace

bool safeRead(const void* source, void* destination, std::size_t bytes) noexcept {
    if (source == nullptr || destination == nullptr) {
        return false;
    }
    return bytes == 0 || guardedCopy(destination, source, bytes);
}

bool safeWrite(void* destination, const void* source, std::size_t bytes) noexcept {
    if (destination == nullptr || source == nullptr || bytes == 0) {
        return false;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(destination);
    if (address + bytes < address) {
        return false;
    }
    if (currentThreadStack().overlaps(address, bytes) ||
        currentModuleRange().overlaps(address, bytes)) {
        return false;
    }
    if (!pagesAreWritable(address, bytes)) {
        return false;
    }
    return guardedCopy(destination, source, bytes);
}

bool isScannableProtection(std::uint32_t state, std::uint32_t type,
                           std::uint32_t protect) noexcept {
    return state == MEM_COMMIT && type == MEM_PRIVATE &&
           (protect & kBaseProtectionMask) == PAGE_READWRITE &&
           (protect & kRejectedProtectionModifiers) == 0;
}

Range moduleRangeOf(const void* addressInModule) noexcept {
    HMODULE module = nullptr;
    const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (!GetModuleHandleExW(flags, static_cast<LPCWSTR>(addressInModule), &module) ||
        module == nullptr) {
        return {};
    }
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return {};
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return {};
    }
    return {base, base + nt->OptionalHeader.SizeOfImage};
}

Range currentModuleRange() noexcept {
    static const Range range = moduleRangeOf(reinterpret_cast<const void*>(&currentModuleRange));
    return range;
}

Range currentThreadStack() noexcept {
    ULONG_PTR low = 0;
    ULONG_PTR high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    return {static_cast<std::uintptr_t>(low), static_cast<std::uintptr_t>(high)};
}

std::vector<Range> writableRegions(const std::vector<Range>& exclusions) {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    auto cursor = reinterpret_cast<std::uintptr_t>(system.lpMinimumApplicationAddress);
    const auto maximum = reinterpret_cast<std::uintptr_t>(system.lpMaximumApplicationAddress);

    std::vector<Range> regions;
    while (cursor < maximum) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) == 0) {
            break;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        const std::uintptr_t next = base + info.RegionSize;
        if (isScannableProtection(info.State, info.Type, info.Protect)) {
            appendMinusExclusions(regions, {base, next}, exclusions);
        }
        if (next <= cursor) {
            break;
        }
        cursor = next;
    }
    return regions;
}

std::vector<Range> defaultExclusions() {
    return {currentModuleRange(), currentThreadStack()};
}

DoubleRange doubleRangeRoundingTo(float value) noexcept {
    const std::uint32_t bits = floatBits(value);
    if (!isFiniteFloatBits(bits)) {
        return {};
    }
    const std::uint64_t sign = (bits & kFloatSignBit) != 0 ? kDoubleSignBit : 0;
    const float magnitude = floatFromBits(bits & ~kFloatSignBit);
    const double center = static_cast<double>(magnitude);
    const bool isNormal = (bits & kFloatExponentMask) != 0;
    if (!isNormal || magnitude > kMaxBandMagnitude) {
        return {doubleBits(center) | sign, doubleBits(center) | sign};
    }
    // Midpoints to the neighbouring floats are exact in double; probing fixes tie handling.
    const double up = static_cast<double>(std::nextafter(magnitude, HUGE_VALF));
    const double down = static_cast<double>(std::nextafter(magnitude, 0.0f));
    const std::uint64_t hi = highestBitsRoundingTo(doubleBits((center + up) * 0.5), magnitude);
    const std::uint64_t lo = lowestBitsRoundingTo(doubleBits((center + down) * 0.5), magnitude);
    return {lo | sign, hi | sign};
}

int trailingZeroMantissaBits(float value) noexcept {
    std::uint32_t mantissa = floatBits(value) & kFloatMantissaMask;
    if (mantissa == 0) {
        return kFloatMantissaBits;
    }
    int zeros = 0;
    while ((mantissa & 1u) == 0) {
        mantissa >>= 1;
        ++zeros;
    }
    return zeros;
}

bool isDistinctiveFloat(float value, float minValue, int maxTrailingZeroBits) noexcept {
    if (!isFiniteFloatBits(floatBits(value)) || !(value > minValue)) {
        return false;
    }
    return trailingZeroMantissaBits(value) <= maxTrailingZeroBits;
}

}  // namespace e2t::mem
