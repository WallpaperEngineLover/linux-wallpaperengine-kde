#pragma once

#include <cstdint>

#include "WallpaperEngine/Data/JSON.h"

namespace WallpaperEngine::Data::Parsers {
// an effect instance's combo as WE reads it for conditions (sub_1401E63B0): a number, cut to an integer, and anything
// else counts as 0
inline int comboValue (const WallpaperEngine::Data::JSON::JSON& value) {
    if (value.is_number_float ()) {
	return static_cast<int> (value.get<double> ());
    }

    return value.is_number () ? static_cast<int> (value.get<int64_t> ()) : 0;
}

// a material pass's combos and an effect pass override's (merged into the pass by sub_140153200, read by
// sub_140154480): asInt without a type test, so null is 0, a real is cut and a bool is 0 or 1. Text, arrays and objects
// abort WE there, 0 here
inline int passComboValue (const WallpaperEngine::Data::JSON::JSON& value) {
    if (value.is_boolean ()) {
	return value.get<bool> () ? 1 : 0;
    }

    return comboValue (value);
}
}
