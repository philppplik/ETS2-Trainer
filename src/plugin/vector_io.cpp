#include "vector_io.h"

#include <cmath>
#include <cstring>

#include "mem_access.h"

namespace e2t {
namespace {

constexpr double kMaxPlausibleComponent = 1.0e4;  // m/s (or m for offsets): larger is garbage

}  // namespace

std::size_t vectorBytes(VectorEncoding encoding) noexcept {
    return encoding == VectorEncoding::Float32x3 ? 3 * sizeof(float) : 3 * sizeof(double);
}

bool decodeVector(const std::uint8_t* raw, VectorEncoding encoding, double (&out)[3]) noexcept {
    for (int i = 0; i < 3; ++i) {
        if (encoding == VectorEncoding::Float32x3) {
            std::uint32_t bits;
            std::memcpy(&bits, raw + i * sizeof(float), sizeof(bits));
            if (!mem::isFiniteFloatBits(bits)) {
                return false;
            }
            out[i] = static_cast<double>(mem::floatFromBits(bits));
        } else {
            std::uint64_t bits;
            std::memcpy(&bits, raw + i * sizeof(double), sizeof(bits));
            if (!mem::isFiniteDoubleBits(bits)) {
                return false;
            }
            out[i] = mem::doubleFromBits(bits);
        }
        if (std::fabs(out[i]) > kMaxPlausibleComponent) {
            return false;
        }
    }
    return true;
}

void encodeVector(const double (&v)[3], VectorEncoding encoding, std::uint8_t* raw) noexcept {
    for (int i = 0; i < 3; ++i) {
        if (encoding == VectorEncoding::Float32x3) {
            const float component = static_cast<float>(v[i]);
            std::memcpy(raw + i * sizeof(float), &component, sizeof(component));
        } else {
            std::memcpy(raw + i * sizeof(double), &v[i], sizeof(double));
        }
    }
}

bool readVector(std::uintptr_t address, VectorEncoding encoding, double (&out)[3]) noexcept {
    std::uint8_t raw[kMaxVectorBytes];
    return mem::safeRead(reinterpret_cast<const void*>(address), raw, vectorBytes(encoding)) &&
           decodeVector(raw, encoding, out);
}

bool readVectorMagnitude(std::uintptr_t address, VectorEncoding encoding,
                         float& magnitude) noexcept {
    double v[3];
    if (!readVector(address, encoding, v)) {
        return false;
    }
    magnitude = static_cast<float>(lengthOf(v));
    return true;
}

double lengthOf(const double (&v)[3]) noexcept {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

}  // namespace e2t
