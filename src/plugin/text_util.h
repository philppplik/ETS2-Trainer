// Small string helpers for fixed-size UTF-8 buffers.
#pragma once

#include <cstddef>
#include <cstring>

namespace e2t {

// Copies src into dst (capacity bytes incl. terminator) without splitting a UTF-8 sequence.
inline void copyUtf8(char* dst, std::size_t capacity, const char* src) noexcept {
    if (dst == nullptr || capacity == 0) {
        return;
    }
    std::size_t length = src != nullptr ? std::strlen(src) : 0;
    if (length >= capacity) {
        length = capacity - 1;
        constexpr unsigned char kContinuationMask = 0xC0;
        constexpr unsigned char kContinuationByte = 0x80;
        while (length > 0 &&
               (static_cast<unsigned char>(src[length]) & kContinuationMask) == kContinuationByte) {
            --length;  // src[length] would be the first dropped byte: back off to a lead byte
        }
    }
    if (length > 0) {
        std::memcpy(dst, src, length);
    }
    dst[length] = '\0';
}

template <std::size_t N>
void copyUtf8(char (&dst)[N], const char* src) noexcept {
    copyUtf8(dst, N, src);
}

}  // namespace e2t
