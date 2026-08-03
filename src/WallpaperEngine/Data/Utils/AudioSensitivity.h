#pragma once

#include <utility>

namespace WallpaperEngine::Data::Utils {
/**
 * Recomputes an audio-reactive script property's minvalue/maxvalue around their authored midpoint,
 * scaled by `multiplier`: 0 collapses the range to the midpoint (locked, no pulse), 1 is a no-op
 * (the wallpaper's original authored behavior), and >1 exaggerates the swing.
 */
inline std::pair<float, float> scaleAudioRange (float originalMin, float originalMax, float multiplier) {
    const float mid = (originalMin + originalMax) / 2.0f;
    const float half = (originalMax - originalMin) / 2.0f * multiplier;

    return { mid - half, mid + half };
}
} // namespace WallpaperEngine::Data::Utils
