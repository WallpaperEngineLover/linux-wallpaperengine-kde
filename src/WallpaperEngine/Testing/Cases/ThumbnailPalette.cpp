#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "WallpaperEngine/Media/ThumbnailPalette.h"

using WallpaperEngine::Media::computeThumbnailPalette;

namespace {
void fill (std::vector<uint8_t>& image, size_t width, size_t x0, size_t x1, size_t height, uint8_t r, uint8_t g, uint8_t b) {
    for (size_t y = 0; y < height; y++) {
	for (size_t x = x0; x < x1; x++) {
	    uint8_t* pixel = &image[(y * width + x) * 4];
	    pixel[0] = r;
	    pixel[1] = g;
	    pixel[2] = b;
	    pixel[3] = 255;
	}
    }
}
} // namespace

TEST_CASE ("Thumbnail palette orders colors by how much of the cover they cover") {
    constexpr size_t width = 100;
    constexpr size_t height = 50;
    std::vector<uint8_t> image (width * height * 4, 0);

    fill (image, width, 0, 60, height, 200, 30, 30);
    fill (image, width, 60, 90, height, 30, 30, 200);
    fill (image, width, 90, 100, height, 30, 200, 30);

    const auto palette = computeThumbnailPalette (image.data (), width, height);

    CHECK (palette.primary.r > 0.7f);
    CHECK (palette.primary.b < 0.2f);
    CHECK (palette.secondary.b > 0.7f);
    CHECK (palette.tertiary.g > 0.7f);
}

TEST_CASE ("Thumbnail palette picks a readable text color and a contrasting black or white") {
    constexpr size_t width = 32;
    constexpr size_t height = 32;
    std::vector<uint8_t> dark (width * height * 4, 0);
    std::vector<uint8_t> light (width * height * 4, 0);

    fill (dark, width, 0, width, height, 10, 10, 30);
    fill (light, width, 0, width, height, 240, 240, 220);

    const auto onDark = computeThumbnailPalette (dark.data (), width, height);
    const auto onLight = computeThumbnailPalette (light.data (), width, height);

    CHECK (onDark.highContrast.r == 1.0f);
    CHECK (onLight.highContrast.r == 0.0f);
    // a flat cover has one color only, secondary and tertiary fall back to it
    CHECK (onDark.secondary == onDark.primary);
}

TEST_CASE ("Thumbnail palette ignores transparent pixels and empty images") {
    std::vector<uint8_t> transparent (16 * 16 * 4, 0);

    const auto fallback = computeThumbnailPalette (transparent.data (), 16, 16);
    const auto empty = computeThumbnailPalette (nullptr, 0, 0);

    CHECK (fallback.primary == empty.primary);
}
