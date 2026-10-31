#include "LedLayout.h"

#include <algorithm>

namespace WallpaperEngine::Led {
std::vector<LedPixel> layoutDevice (const OpenRgb::Device& device) {
    std::vector<LedPixel> layout (device.ledCount);
    uint32_t rows = 0;

    for (const auto& zone : device.zones) {
	rows += zone.matrix.empty () ? 1 : std::max (zone.matrixHeight, 1u);
    }

    uint32_t firstLed = 0;
    uint32_t firstRow = 0;

    for (const auto& zone : device.zones) {
	const uint32_t height = zone.matrix.empty () ? 1 : std::max (zone.matrixHeight, 1u);
	const auto place = [&] (const uint32_t led, const uint32_t column, const uint32_t row, const uint32_t columns) {
	    if (led >= zone.leds || firstLed + led >= layout.size () || columns == 0) {
		return;
	    }

	    const double xn = (column + 0.5) / columns;
	    const double yn = (firstRow + row + 0.5) / rows;

	    layout[firstLed + led] = {
		.x = std::min (static_cast<int> (kFrameWidth * xn), kFrameWidth - 1),
		.y = std::min (static_cast<int> (kFrameHeight * yn), kFrameHeight - 1),
	    };
	};

	if (zone.matrix.empty ()) {
	    for (uint32_t led = 0; led < zone.leds; led++) {
		place (led, led, 0, zone.leds);
	    }
	} else {
	    for (uint32_t row = 0; row < zone.matrixHeight; row++) {
		for (uint32_t column = 0; column < zone.matrixWidth; column++) {
		    place (zone.matrix[row * zone.matrixWidth + column], column, row, zone.matrixWidth);
		}
	    }
	}

	firstLed += zone.leds;
	firstRow += height;
    }

    return layout;
}

void applyBrightness (const std::span<const uint8_t> in, const std::span<uint8_t> out, const Brightness& brightness) {
    const size_t pixels = std::min (in.size (), out.size ()) / 3;

    for (size_t i = 0; i < pixels; i++) {
	const uint8_t* source = in.data () + i * 3;
	uint8_t* target = out.data () + i * 3;
	float scale = brightness.factor;

	if (brightness.boost) {
	    const int strongest = std::max ({ source[0], source[1], source[2] });

	    if (strongest == 0) {
		target[0] = target[1] = target[2] = 0;
		continue;
	    }

	    scale *= 255.0f / static_cast<float> (strongest);
	}

	for (int channel = 0; channel < 3; channel++) {
	    target[channel] = static_cast<uint8_t> (
		std::clamp (static_cast<int> (static_cast<float> (source[channel]) * scale), 0, 255)
	    );
	}
    }
}

void sampleDevice (
    const std::span<const uint8_t> frame, const std::span<const LedPixel> layout, std::vector<uint8_t>& out
) {
    out.resize (layout.size () * 3);

    for (size_t i = 0; i < layout.size (); i++) {
	const size_t at = (static_cast<size_t> (layout[i].y) * kFrameWidth + layout[i].x) * 3;

	out[i * 3] = frame[at];
	out[i * 3 + 1] = frame[at + 1];
	out[i * 3 + 2] = frame[at + 2];
    }
}
}
