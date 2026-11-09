#pragma once

#include <cmath>
#include <cstring>
#include <limits>

#include "WallpaperEngine/Data/Utils/DecimalNumber.h"

namespace WallpaperEngine::Scripting {
/** JS parseFloat on a float property: leading white space, then a decimal number or Infinity, whatever follows is left
 * for the caller. Too big for a float is Infinity, like the double it would be in JS cast down */
inline bool readDecimalPrefix (const char*& position, float& number) {
    while (*position == ' ' || (*position >= '\t' && *position <= '\r')) {
	position++;
    }

    const char* cursor = position;
    const bool negative = *cursor == '-';

    if (*cursor == '+' || *cursor == '-') {
	cursor++;
    }

    if (std::strncmp (cursor, "Infinity", 8) == 0) {
	number = negative ? -std::numeric_limits<float>::infinity () : std::numeric_limits<float>::infinity ();
	position = cursor + 8;
	return true;
    }

    double value = 0.0;

    if (!Data::Utils::readDecimalNumber (position, value)) {
	return false;
    }

    // a double past what a float holds has no defined conversion
    number = std::fabs (value) > static_cast<double> (std::numeric_limits<float>::max ())
	? std::copysign (std::numeric_limits<float>::infinity (), static_cast<float> (value < 0.0 ? -1.0 : 1.0))
	: static_cast<float> (value);

    return true;
}
}
