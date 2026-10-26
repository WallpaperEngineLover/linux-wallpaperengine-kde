#pragma once

#include <bit>
#include <concepts>
#include <cstdint>

namespace WallpaperEngine::Data::Utils {
/** Both are the same bits: what a cache key or a "did it change" test wants, a NaN equals itself */
[[nodiscard]] inline bool sameFloat (const float a, const float b) noexcept {
    return std::bit_cast<uint32_t> (a) == std::bit_cast<uint32_t> (b);
}

[[nodiscard]] inline bool sameFloat (const double a, const double b) noexcept {
    return std::bit_cast<uint64_t> (a) == std::bit_cast<uint64_t> (b);
}

/** Not zero, in either direction (a NaN is neither) */
template <std::floating_point T> [[nodiscard]] constexpr bool isNonZero (const T value) noexcept {
    return value < T (0) || value > T (0);
}

template <std::floating_point T> [[nodiscard]] constexpr bool isZero (const T value) noexcept {
    return !isNonZero (value);
}
}
