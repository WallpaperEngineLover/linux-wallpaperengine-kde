#pragma once

#include <utility>

namespace WallpaperEngine::Data::Utils {
// scales minvalue/maxvalue around their midpoint: 0 locks it, 1 changes nothing, >1 exaggerates the swing
inline std::pair<float, float> scaleAudioRange (float originalMin, float originalMax, float multiplier) {
    const float mid = (originalMin + originalMax) / 2.0f;
    const float half = (originalMax - originalMin) / 2.0f * multiplier;

    return { mid - half, mid + half };
}
}
