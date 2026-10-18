#pragma once

#include <cmath>
#include <stdexcept>
#include <string>

namespace WallpaperEngine::Data::Utils {
/** std::stof for what a user types: "nan" and "inf" parse, pass every range check (a comparison with NaN is false) and
 * end up as a flat red frame (--speed nan) or a black one (--zoom nan). Throws std::invalid_argument like stof does for
 * text that isn't a number, so the callers' catch blocks take it */
inline float parseFiniteFloat (const std::string& text) {
    const float value = std::stof (text);

    if (!std::isfinite (value)) {
	throw std::invalid_argument ("not a finite number: " + text);
    }

    return value;
}
} // namespace WallpaperEngine::Data::Utils
