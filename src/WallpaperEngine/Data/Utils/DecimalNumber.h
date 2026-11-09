#pragma once

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>
#include <string_view>
#include <system_error>

namespace WallpaperEngine::Data::Utils {
/** Reads a decimal number at position: an optional sign, digits with an optional fraction and an optional exponent.
 * No blanks, no hex, no "inf" or "nan", and no locale. A number too big or too small for a double comes out as
 * +-Infinity or 0, judged by its decimal magnitude. Moves position past what it read, false (and position untouched)
 * when there is no number */
inline bool readDecimalNumber (const char*& position, double& number) {
    const char* cursor = position;
    const bool negative = *cursor == '-';

    if (*cursor == '+' || *cursor == '-') {
	cursor++;
    }

    const char* start = cursor;
    // decimal exponent of the first non zero digit, counted as in scientific notation
    long magnitude = 0;
    bool seenDigit = false;
    long integerDigits = 0;
    long leadingZeros = 0;

    for (; std::isdigit (static_cast<unsigned char> (*cursor)); cursor++) {
	if (*cursor != '0' || seenDigit) {
	    seenDigit = true;
	    integerDigits++;
	}
    }

    bool any = cursor != start;

    if (*cursor == '.') {
	const char* fraction = ++cursor;

	for (; std::isdigit (static_cast<unsigned char> (*cursor)); cursor++) {
	    if (*cursor == '0' && !seenDigit) {
		leadingZeros++;
	    } else {
		seenDigit = true;
	    }
	}

	any = any || cursor != fraction;
    }

    if (!any) {
	return false;
    }

    magnitude = integerDigits > 0 ? integerDigits - 1 : -(leadingZeros + 1);

    if (*cursor == 'e' || *cursor == 'E') {
	const char* exponent = cursor + 1;
	const bool negativeExponent = *exponent == '-';

	if (*exponent == '+' || *exponent == '-') {
	    exponent++;
	}

	if (std::isdigit (static_cast<unsigned char> (*exponent))) {
	    long value = 0;

	    for (; std::isdigit (static_cast<unsigned char> (*exponent)); exponent++) {
		// far past any double, the digits after it change nothing
		value = std::min (value * 10 + (*exponent - '0'), 100000L);
	    }

	    magnitude += negativeExponent ? -value : value;
	    cursor = exponent;
	}
    }

    // from_chars takes no leading '+'
    const char* begin = *position == '+' ? position + 1 : position;
    double value = 0.0;
    const auto result = std::from_chars (begin, cursor, value);

    if (result.ec == std::errc::result_out_of_range) {
	value = seenDigit && magnitude > 0 ? std::numeric_limits<double>::infinity () : 0.0;
	value = negative ? -value : value;
    } else if (result.ec != std::errc ()) {
	return false;
    }

    number = value;
    position = cursor;

    return true;
}
}
