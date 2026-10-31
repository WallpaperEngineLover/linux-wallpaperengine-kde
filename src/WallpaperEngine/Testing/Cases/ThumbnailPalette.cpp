#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include <stb_image_write.h>

#include "WallpaperEngine/Media/ThumbnailPalette.h"

using WallpaperEngine::Media::computeThumbnailPalette;
using WallpaperEngine::Media::loadThumbnailPalette;

namespace {
void fill (
    std::vector<uint8_t>& image, size_t width, size_t x0, size_t x1, size_t height, uint8_t r, uint8_t g, uint8_t b
) {
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

void fillRows (
    std::vector<uint8_t>& image, size_t width, size_t y0, size_t y1, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255
) {
    for (size_t y = y0; y < y1; y++) {
	for (size_t x = 0; x < width; x++) {
	    uint8_t* pixel = &image[(y * width + x) * 4];
	    pixel[0] = r;
	    pixel[1] = g;
	    pixel[2] = b;
	    pixel[3] = a;
	}
    }
}

void checkColor (const glm::vec3& color, int r, int g, int b) {
    CHECK_THAT (color.r, Catch::Matchers::WithinAbs (static_cast<float> (r) / 255.0f, 1e-3));
    CHECK_THAT (color.g, Catch::Matchers::WithinAbs (static_cast<float> (g) / 255.0f, 1e-3));
    CHECK_THAT (color.b, Catch::Matchers::WithinAbs (static_cast<float> (b) / 255.0f, 1e-3));
}
}

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

TEST_CASE ("Thumbnail palette averages the pixels of a bucket") {
    std::vector<uint8_t> image (16 * 16 * 4, 0);

    fillRows (image, 16, 0, 8, 200, 100, 50);
    fillRows (image, 16, 8, 16, 206, 104, 54);

    const auto palette = computeThumbnailPalette (image.data (), 16, 16);

    checkColor (palette.primary, 203, 102, 52);
    CHECK (palette.secondary == palette.primary);
    CHECK (palette.tertiary == palette.primary);
}

TEST_CASE ("Thumbnail palette reads every row and keeps the channels apart") {
    constexpr size_t width = 40;
    constexpr size_t height = 40;
    std::vector<uint8_t> image (width * height * 4, 0);

    fillRows (image, width, 0, 20, 220, 20, 40);
    fillRows (image, width, 20, 32, 30, 200, 90);
    fillRows (image, width, 32, 40, 60, 80, 240);

    const auto palette = computeThumbnailPalette (image.data (), width, height);

    checkColor (palette.primary, 220, 20, 40);
    checkColor (palette.secondary, 30, 200, 90);
    checkColor (palette.tertiary, 60, 80, 240);
}

TEST_CASE ("Thumbnail palette counts pixels from half opacity up") {
    constexpr size_t width = 20;
    constexpr size_t height = 20;
    std::vector<uint8_t> image (width * height * 4, 0);

    fillRows (image, width, 0, 6, 220, 20, 40, 128);
    fillRows (image, width, 6, 20, 20, 220, 40, 127);

    const auto palette = computeThumbnailPalette (image.data (), width, height);

    checkColor (palette.primary, 220, 20, 40);
    CHECK (palette.secondary == palette.primary);

    std::vector<uint8_t> faint (width * height * 4, 0);

    fillRows (faint, width, 0, 20, 220, 20, 40, 127);
    CHECK (
	computeThumbnailPalette (faint.data (), width, height).primary
	== computeThumbnailPalette (nullptr, 0, 0).primary
    );
}

TEST_CASE ("Thumbnail palette samples large covers on a grid of about 96 by 96") {
    constexpr size_t width = 288;
    constexpr size_t height = 288;
    std::vector<uint8_t> image (width * height * 4, 0);

    fillRows (image, width, 0, height, 20, 20, 220);

    for (size_t y = 0; y < height; y += 3) {
	for (size_t x = 0; x < width; x += 3) {
	    uint8_t* pixel = &image[(y * width + x) * 4];

	    pixel[0] = 220;
	    pixel[1] = 20;
	    pixel[2] = 20;
	}
    }

    const auto palette = computeThumbnailPalette (image.data (), width, height);

    checkColor (palette.primary, 220, 20, 20);
    CHECK (palette.secondary == palette.primary);

    std::vector<uint8_t> small (191 * 191 * 4, 0);

    fillRows (small, 191, 0, 191, 20, 20, 220);

    for (size_t x = 1; x < 191; x += 2) {
	for (size_t y = 0; y < 191; y++) {
	    uint8_t* pixel = &small[(y * 191 + x) * 4];

	    pixel[0] = 20;
	    pixel[1] = 220;
	    pixel[2] = 20;
	}
    }

    const auto fine = computeThumbnailPalette (small.data (), 191, 191);

    checkColor (fine.primary, 20, 20, 220);
    checkColor (fine.secondary, 20, 220, 20);
}

TEST_CASE ("Thumbnail palette skips colors close to one it already picked") {
    constexpr size_t width = 20;
    constexpr size_t height = 20;
    std::vector<uint8_t> image (width * height * 4, 0);

    fillRows (image, width, 0, 10, 200, 30, 30);
    fillRows (image, width, 10, 16, 215, 45, 45);
    fillRows (image, width, 16, 20, 30, 30, 200);

    const auto palette = computeThumbnailPalette (image.data (), width, height);

    checkColor (palette.primary, 200, 30, 30);
    checkColor (palette.secondary, 30, 30, 200);
    CHECK (palette.tertiary == palette.secondary);

    std::vector<uint8_t> apart (width * height * 4, 0);

    fillRows (apart, width, 0, 10, 200, 30, 30);
    fillRows (apart, width, 10, 16, 120, 120, 30);
    fillRows (apart, width, 16, 20, 30, 30, 200);

    const auto three = computeThumbnailPalette (apart.data (), width, height);

    checkColor (three.secondary, 120, 120, 30);
    checkColor (three.tertiary, 30, 30, 200);
}

TEST_CASE ("Thumbnail palette flips between white and black at mid grey") {
    for (const auto [grey, white] :
	 { std::pair { 117, true }, std::pair { 118, false }, std::pair { 5, true }, std::pair { 250, false } }) {
	std::vector<uint8_t> image (8 * 8 * 4, 0);

	fillRows (image, 8, 0, 8, grey, grey, grey);

	const auto palette = computeThumbnailPalette (image.data (), 8, 8);

	CAPTURE (grey);
	CHECK (palette.highContrast == (white ? glm::vec3 (1.0f) : glm::vec3 (0.0f)));
	CHECK (palette.text == palette.highContrast);
    }
}

TEST_CASE ("Thumbnail palette uses a cover color as text only when it reads well") {
    constexpr size_t width = 20;
    constexpr size_t height = 20;

    std::vector<uint8_t> image (width * height * 4, 0);
    fillRows (image, width, 0, 10, 10, 10, 60);
    fillRows (image, width, 10, 16, 240, 240, 200);
    fillRows (image, width, 16, 20, 150, 150, 220);

    const auto secondBest = computeThumbnailPalette (image.data (), width, height);

    checkColor (secondBest.text, 240, 240, 200);

    std::vector<uint8_t> swapped (width * height * 4, 0);
    fillRows (swapped, width, 0, 10, 10, 10, 60);
    fillRows (swapped, width, 10, 16, 150, 150, 220);
    fillRows (swapped, width, 16, 20, 240, 240, 200);

    const auto thirdBest = computeThumbnailPalette (swapped.data (), width, height);

    checkColor (thirdBest.text, 240, 240, 200);

    std::vector<uint8_t> muddy (width * height * 4, 0);
    fillRows (muddy, width, 0, 10, 10, 10, 60);
    fillRows (muddy, width, 10, 16, 60, 60, 140);
    fillRows (muddy, width, 16, 20, 30, 120, 80);

    const auto fallback = computeThumbnailPalette (muddy.data (), width, height);

    CHECK (fallback.text == fallback.highContrast);
    CHECK (fallback.text == glm::vec3 (1.0f));
}

TEST_CASE ("Thumbnail palette loads a cover file and falls back on a missing one") {
    const auto dir
	= std::filesystem::temp_directory_path () / ("lwe-thumbnail-palette-test-" + std::to_string (getpid ()));
    std::filesystem::create_directories (dir);

    std::vector<uint8_t> rgba (4 * 4 * 4, 255);

    for (int i = 0; i < 16; i++) {
	rgba[i * 4] = 220;
	rgba[i * 4 + 1] = 20;
	rgba[i * 4 + 2] = 40;
    }

    std::string png;
    stbi_write_png_to_func (
	[] (void* context, void* data, int size) {
	    static_cast<std::string*> (context)->append (static_cast<const char*> (data), size);
	},
	&png, 4, 4, 4, rgba.data (), 16
    );

    const auto file = dir / "cover.png";

    std::ofstream (file, std::ios::binary).write (png.data (), static_cast<std::streamsize> (png.size ()));

    checkColor (loadThumbnailPalette (file.string ()).primary, 220, 20, 40);
    CHECK (
	loadThumbnailPalette ((dir / "missing.png").string ()).primary
	== computeThumbnailPalette (nullptr, 0, 0).primary
    );

    std::filesystem::remove_all (dir);
}

TEST_CASE ("Thumbnail palette tells colors apart that differ in one channel only") {
    const int base[3] = { 5, 5, 5 };

    for (int channel = 0; channel < 3; channel++) {
	std::vector<uint8_t> image (20 * 20 * 4, 0);
	int other[3] = { base[0], base[1], base[2] };

	other[channel] = 220;
	fillRows (image, 20, 0, 12, base[0], base[1], base[2]);
	fillRows (image, 20, 12, 20, other[0], other[1], other[2]);

	const auto palette = computeThumbnailPalette (image.data (), 20, 20);

	CAPTURE (channel);
	checkColor (palette.primary, base[0], base[1], base[2]);
	checkColor (palette.secondary, other[0], other[1], other[2]);
    }
}
