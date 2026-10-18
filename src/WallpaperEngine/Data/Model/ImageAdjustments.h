#pragma once

#include <optional>
#include <string>

namespace WallpaperEngine::Data::Model {
// WE's per-wallpaper image settings in its slider units (0-100, 50 neutral; filter strength 0-100)
// unset fields fall through: command line, preset, WE's defaults
struct ImageAdjustments {
    /** wec_e, the color options only apply while this is on */
    std::optional<bool> colorEnabled;
    std::optional<float> brightness;
    std::optional<float> contrast;
    std::optional<float> saturation;
    std::optional<float> hue;
    /** wcc_v, a lut from materials/lut without the extension, empty for none */
    std::optional<std::string> filter;
    std::optional<float> filterStrength;
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
}
