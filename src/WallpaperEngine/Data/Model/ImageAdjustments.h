#pragma once

#include <optional>
#include <string>

namespace WallpaperEngine::Data::Model {
/**
 * Wallpaper Engine's own per-wallpaper image settings from its browser's properties panel, in its slider units:
 * brightness, contrast, saturation and hue go 0-100 with 50 as neutral, filter strength goes 0-100.
 * Unset fields fall through to the next source (command line, then preset, then Wallpaper Engine's defaults).
 */
struct ImageAdjustments {
    /** wec_e, the color options only apply while this is on */
    std::optional<bool> colorEnabled;
    /** wec_brs */
    std::optional<float> brightness;
    /** wec_con */
    std::optional<float> contrast;
    /** wec_sa */
    std::optional<float> saturation;
    /** wec_hue */
    std::optional<float> hue;
    /** wcc_v, a lut from materials/lut without the extension, empty for none */
    std::optional<std::string> filter;
    /** wcc_amt */
    std::optional<float> filterStrength;
    /** alignmentfliph */
    std::optional<bool> flipHorizontal;

    [[nodiscard]] ImageAdjustments over (const ImageAdjustments& fallback) const {
	return {
	    .colorEnabled = colorEnabled.has_value () ? colorEnabled : fallback.colorEnabled,
	    .brightness = brightness.has_value () ? brightness : fallback.brightness,
	    .contrast = contrast.has_value () ? contrast : fallback.contrast,
	    .saturation = saturation.has_value () ? saturation : fallback.saturation,
	    .hue = hue.has_value () ? hue : fallback.hue,
	    .filter = filter.has_value () ? filter : fallback.filter,
	    .filterStrength = filterStrength.has_value () ? filterStrength : fallback.filterStrength,
	    .flipHorizontal = flipHorizontal.has_value () ? flipHorizontal : fallback.flipHorizontal,
	};
    }
};
} // namespace WallpaperEngine::Data::Model
