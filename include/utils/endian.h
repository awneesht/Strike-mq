#pragma once
/// StrikeMQ — Portable byte-swap helpers
/// Used by both protocol/ and storage/ layers.

#include <cstdint>

namespace strike {

inline uint16_t bswap16(uint16_t v) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap16(v);
#else
    return (v >> 8) | (v << 8);
#endif
}

inline uint32_t bswap32(uint32_t v) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap32(v);
#else
    return ((v >> 24)) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24));
#endif
}

inline uint64_t bswap64(uint64_t v) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap64(v);
#else
    return ((v >> 56)) | ((v >> 40) & 0xFF00ULL) |
           ((v >> 24) & 0xFF0000ULL) | ((v >> 8) & 0xFF000000ULL) |
           ((v << 8) & 0xFF00000000ULL) | ((v << 24) & 0xFF0000000000ULL) |
           ((v << 40) & 0xFF000000000000ULL) | ((v << 56));
#endif
}

} // namespace strike
