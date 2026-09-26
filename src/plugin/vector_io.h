// Guarded read/write of 3-component vectors stored as float[3] or double[3].
#pragma once

#include <cstddef>
#include <cstdint>

namespace e2t {

enum class VectorEncoding : std::uint8_t { Float32x3, Float64x3 };

constexpr std::size_t kMaxVectorBytes = 3 * sizeof(double);

std::size_t vectorBytes(VectorEncoding encoding) noexcept;

// Rejects non-finite or implausibly large components using bit tests before any FP math, so
// garbage memory can never raise a floating-point exception.
bool decodeVector(const std::uint8_t* raw, VectorEncoding encoding, double (&out)[3]) noexcept;
void encodeVector(const double (&v)[3], VectorEncoding encoding, std::uint8_t* raw) noexcept;

bool readVector(std::uintptr_t address, VectorEncoding encoding, double (&out)[3]) noexcept;
bool readVectorMagnitude(std::uintptr_t address, VectorEncoding encoding,
                         float& magnitude) noexcept;
double lengthOf(const double (&v)[3]) noexcept;

}  // namespace e2t
