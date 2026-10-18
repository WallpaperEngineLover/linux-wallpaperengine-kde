#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdlib>
#include <string>
#include <vector>

#include <stb_image_write.h>

#include "WallpaperEngine/Render/GifAnimation.h"
#include "WallpaperEngine/Render/ImageDecoder.h"

using WallpaperEngine::Render::GifAnimation;
using Pixel = std::array<uint8_t, 4>;

namespace {
// hand-built GIFs: literal LZW codes, a clear code before the table would widen them
class GifWriter {
public:
    GifWriter (int width, int height, const std::vector<std::array<uint8_t, 3>>& palette, int background) {
	bytes = "GIF89a";
	u16 (width);
	u16 (height);
	bytes += static_cast<char> (0x80 | 1);
	bytes += static_cast<char> (background);
	bytes += '\0';

	for (size_t i = 0; i < 4; i++) {
	    const auto color = i < palette.size () ? palette[i] : std::array<uint8_t, 3> {};
	    bytes.append (reinterpret_cast<const char*> (color.data ()), 3);
	}
    }

    GifWriter& control (int disposal, int delayCentiseconds, int transparent = -1) {
	bytes += "\x21\xF9\x04";
	bytes += static_cast<char> ((disposal << 2) | (transparent >= 0 ? 1 : 0));
	u16 (delayCentiseconds);
	bytes += static_cast<char> (transparent >= 0 ? transparent : 0);
	bytes += '\0';
	return *this;
    }

    GifWriter&
    image (int x, int y, int width, int height, const std::vector<uint8_t>& indices, bool interlaced = false) {
	bytes += '\x2C';
	u16 (x);
	u16 (y);
	u16 (width);
	u16 (height);
	bytes += static_cast<char> (interlaced ? 0x40 : 0);

	constexpr int lzwSize = 2;
	constexpr int clear = 1 << lzwSize;
	constexpr int codeSize = lzwSize + 1;
	std::string packed;
	uint32_t bits = 0;
	int count = 0;

	const auto emit = [&] (int code) {
	    bits |= static_cast<uint32_t> (code) << count;
	    count += codeSize;

	    while (count >= 8) {
		packed += static_cast<char> (bits & 0xFF);
		bits >>= 8;
		count -= 8;
	    }
	};

	for (size_t i = 0; i < indices.size (); i++) {
	    if (i % (clear - 2) == 0) {
		emit (clear);
	    }

	    emit (indices[i]);
	}

	emit (clear + 1);

	if (count > 0) {
	    packed += static_cast<char> (bits & 0xFF);
	}

	bytes += static_cast<char> (lzwSize);

	for (size_t offset = 0; offset < packed.size (); offset += 255) {
	    const auto block = packed.substr (offset, 255);
	    bytes += static_cast<char> (block.size ());
	    bytes += block;
	}

	bytes += '\0';
	return *this;
    }

    std::string finish () { return bytes + '\x3B'; }

    std::string bytes;

private:
    void u16 (int value) {
	bytes += static_cast<char> (value & 0xFF);
	bytes += static_cast<char> ((value >> 8) & 0xFF);
    }
};

const std::vector<std::array<uint8_t, 3>> PALETTE = { { 0, 0, 0 }, { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 } };

Pixel pixelAt (const GifAnimation& gif, int x, int y) {
    const uint8_t* pixel = gif.pixels () + (static_cast<size_t> (y) * gif.width () + x) * 4;
    return { pixel[0], pixel[1], pixel[2], pixel[3] };
}

constexpr Pixel RED = { 255, 0, 0, 255 };
constexpr Pixel GREEN = { 0, 255, 0, 255 };
constexpr Pixel BLUE = { 0, 0, 255, 255 };
constexpr Pixel CLEAR = { 0, 0, 0, 0 };
}

TEST_CASE ("GIF frames follow their own delays and start over after the trailer") {
    const auto bytes = GifWriter (2, 1, PALETTE, 0)
			   .control (1, 10)
			   .image (0, 0, 2, 1, { 1, 1 })
			   .control (1, 20)
			   .image (0, 0, 2, 1, { 2, 2 })
			   .control (1, 5)
			   .image (0, 0, 2, 1, { 3, 3 })
			   .finish ();
    auto gif = GifAnimation::open (bytes.data (), bytes.size ());

    REQUIRE (gif != nullptr);
    REQUIRE (gif->advance (0.0f));
    CHECK (gif->width () == 2);
    CHECK (gif->height () == 1);
    CHECK (pixelAt (*gif, 1, 0) == RED);

    // 100ms on the first frame
    CHECK_FALSE (gif->advance (0.06f));
    REQUIRE (gif->advance (0.05f));
    CHECK (pixelAt (*gif, 0, 0) == GREEN);

    // the 10ms overshoot carries over
    CHECK_FALSE (gif->advance (0.18f));
    REQUIRE (gif->advance (0.02f));
    CHECK (pixelAt (*gif, 0, 0) == BLUE);

    REQUIRE (gif->advance (0.05f));
    CHECK (pixelAt (*gif, 0, 0) == RED);
}

TEST_CASE ("GIF frames draw over the previous one and skip transparent pixels") {
    const auto bytes = GifWriter (2, 2, PALETTE, 0)
			   .control (1, 10)
			   .image (0, 0, 2, 2, { 1, 1, 1, 1 })
			   .control (1, 10, 0)
			   .image (0, 0, 2, 2, { 0, 3, 0, 0 })
			   .finish ();
    auto gif = GifAnimation::open (bytes.data (), bytes.size ());

    REQUIRE (gif->advance (0.0f));
    REQUIRE (gif->advance (0.1f));
    CHECK (pixelAt (*gif, 0, 0) == RED);
    CHECK (pixelAt (*gif, 1, 0) == BLUE);
    CHECK (pixelAt (*gif, 0, 1) == RED);
}

TEST_CASE ("GIF disposal 2 fills with the background entry as resourceutil64 stores it") {
    // WE copies the palette entry as BGRA, so red index 1 comes out blue
    const auto bytes = GifWriter (2, 2, PALETTE, 1)
			   .control (2, 10)
			   .image (0, 0, 1, 1, { 2 })
			   .control (1, 10)
			   .image (1, 1, 1, 1, { 3 })
			   .finish ();
    auto gif = GifAnimation::open (bytes.data (), bytes.size ());

    REQUIRE (gif->advance (0.0f));
    CHECK (pixelAt (*gif, 0, 0) == GREEN);
    CHECK (pixelAt (*gif, 1, 0) == BLUE);

    REQUIRE (gif->advance (0.1f));
    CHECK (pixelAt (*gif, 0, 0) == BLUE);
    CHECK (pixelAt (*gif, 1, 1) == BLUE);
}

TEST_CASE ("GIF disposal 2 without a background clears to transparent") {
    // background index 0 = none in resourceutil64
    const auto bytes = GifWriter (2, 1, PALETTE, 0)
			   .control (2, 10)
			   .image (0, 0, 2, 1, { 1, 2 })
			   .control (1, 10)
			   .image (1, 0, 1, 1, { 3 })
			   .finish ();
    auto gif = GifAnimation::open (bytes.data (), bytes.size ());

    REQUIRE (gif->advance (0.0f));
    REQUIRE (gif->advance (0.1f));
    CHECK (pixelAt (*gif, 0, 0) == CLEAR);
    CHECK (pixelAt (*gif, 1, 0) == BLUE);
}

TEST_CASE ("GIF disposal 3 restores the last frame that was kept") {
    const auto bytes = GifWriter (2, 2, PALETTE, 0)
			   .control (1, 10)
			   .image (0, 0, 2, 2, { 1, 1, 1, 1 })
			   .control (3, 10)
			   .image (0, 0, 1, 1, { 2 })
			   .control (1, 10)
			   .image (1, 1, 1, 1, { 3 })
			   .finish ();
    auto gif = GifAnimation::open (bytes.data (), bytes.size ());

    REQUIRE (gif->advance (0.0f));
    REQUIRE (gif->advance (0.1f));
    CHECK (pixelAt (*gif, 0, 0) == GREEN);

    REQUIRE (gif->advance (0.1f));
    CHECK (pixelAt (*gif, 0, 0) == RED);
    CHECK (pixelAt (*gif, 1, 1) == BLUE);
}

TEST_CASE ("Interlaced GIF rows land in pass order") {
    // passes write rows 0, 4, then 2, 6, then 1, 3, 5, 7
    const auto bytes
	= GifWriter (1, 8, PALETTE, 0).control (1, 10).image (0, 0, 1, 8, { 1, 1, 2, 2, 3, 3, 3, 3 }, true).finish ();
    auto gif = GifAnimation::open (bytes.data (), bytes.size ());

    REQUIRE (gif->advance (0.0f));

    const std::array<Pixel, 8> rows = { RED, BLUE, GREEN, BLUE, RED, BLUE, GREEN, BLUE };

    for (int y = 0; y < 8; y++) {
	CHECK (pixelAt (*gif, 0, y) == rows[y]);
    }
}

TEST_CASE ("Broken GIFs fail without decoding or allocating the claimed canvas") {
    CHECK (GifAnimation::open ("PNG", 3) == nullptr);

    const auto huge = std::string ("GIF89a\xff\xff\xff\xff\x80\x00\x00", 13) + std::string (12, '\0') + "\x3B";
    const auto empty = std::string ("GIF89a\0\0\0\0\x80\x00\x00", 13) + std::string (12, '\0') + "\x3B";

    for (const auto& bytes : { std::string ("GIF89a"), huge, empty }) {
	auto gif = GifAnimation::open (bytes.data (), bytes.size ());

	REQUIRE (gif != nullptr);
	CHECK_FALSE (gif->advance (0.0f));
    }

    // a frame reaching past the logical screen
    const auto outside = GifWriter (2, 2, PALETTE, 0).control (1, 10).image (1, 1, 2, 2, { 1, 1, 1, 1 }).finish ();
    auto gif = GifAnimation::open (outside.data (), outside.size ());

    CHECK_FALSE (gif->advance (0.0f));

    // a raster that starts with a data code instead of a clear code
    auto noClear = GifWriter (1, 1, PALETTE, 0).control (1, 10).bytes;
    noClear += std::string ("\x2C\0\0\0\0\x01\0\x01\0\0\x02\x01\x01\0\x3B", 16);
    gif = GifAnimation::open (noClear.data (), noClear.size ());

    CHECK_FALSE (gif->advance (0.0f));
}

namespace {
constexpr int BLOCK = 16;
const std::array<std::array<uint8_t, 3>, 6> BLOCK_COLORS
    = { { { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 255 }, { 0, 0, 0 }, { 255, 255, 0 } } };

// 3x2 solid blocks, row by row
std::string blockJpeg () {
    std::vector<uint8_t> rgb (3 * BLOCK * 2 * BLOCK * 3);

    for (int y = 0; y < 2 * BLOCK; y++) {
	for (int x = 0; x < 3 * BLOCK; x++) {
	    const auto& color = BLOCK_COLORS[(y / BLOCK) * 3 + x / BLOCK];
	    std::copy (color.begin (), color.end (), rgb.begin () + (y * 3 * BLOCK + x) * 3);
	}
    }

    std::string out;
    stbi_write_jpg_to_func (
	[] (void* context, void* data, int size) {
	    static_cast<std::string*> (context)->append (static_cast<const char*> (data), size);
	},
	&out, 3 * BLOCK, 2 * BLOCK, 3, rgb.data (), 100
    );
    return out;
}

// APP1 Exif after SOI, one Orientation (0x0112) SHORT entry
std::string withOrientation (const std::string& jpeg, int orientation, bool bigEndian) {
    std::string tiff = bigEndian ? std::string ("MM\0\x2A\0\0\0\x08\0\x01\x01\x12\0\x03\0\0\0\x01", 18)
				 : std::string ("II\x2A\0\x08\0\0\0\x01\0\x12\x01\x03\0\x01\0\0\0", 18);

    if (bigEndian) {
	tiff += '\0';
	tiff += static_cast<char> (orientation);
    } else {
	tiff += static_cast<char> (orientation);
	tiff += '\0';
    }

    tiff += std::string (6, '\0');

    const std::string payload = std::string ("Exif\0\0", 6) + tiff;
    const size_t length = payload.size () + 2;

    return jpeg.substr (0, 2) + "\xFF\xE1" + static_cast<char> (length >> 8) + static_cast<char> (length & 0xFF)
	+ payload + jpeg.substr (2);
}

bool near (const uint8_t* pixel, const std::array<uint8_t, 3>& color) {
    for (int c = 0; c < 3; c++) {
	if (std::abs (pixel[c] - color[c]) > 24) {
	    return false;
	}
    }

    return pixel[3] == 255;
}
}

TEST_CASE ("JPEGs come out in their EXIF orientation and size probing agrees") {
    const auto jpeg = blockJpeg ();
    REQUIRE (jpeg.size () > 100);

    // source block per position, row by row over the oriented image
    const std::array<std::array<int, 6>, 8> expected { {
	{ { 0, 1, 2, 3, 4, 5 } },
	{ { 2, 1, 0, 5, 4, 3 } },
	{ { 5, 4, 3, 2, 1, 0 } },
	{ { 3, 4, 5, 0, 1, 2 } },
	{ { 0, 3, 1, 4, 2, 5 } },
	{ { 3, 0, 4, 1, 5, 2 } },
	{ { 5, 2, 4, 1, 3, 0 } },
	{ { 2, 5, 1, 4, 0, 3 } },
    } };

    for (const bool bigEndian : { false, true }) {
	for (int orientation = 1; orientation <= 8; orientation++) {
	    INFO ("orientation " << orientation << (bigEndian ? " big endian" : " little endian"));

	    const auto tagged = withOrientation (jpeg, orientation, bigEndian);
	    int width = 0;
	    int height = 0;
	    int probeWidth = 0;
	    int probeHeight = 0;
	    stbi_uc* pixels = WallpaperEngine::Render::decodeImageRGBA (tagged.data (), tagged.size (), width, height);

	    REQUIRE (pixels != nullptr);
	    REQUIRE (
		WallpaperEngine::Render::decodedImageSize (tagged.data (), tagged.size (), probeWidth, probeHeight)
	    );
	    CHECK (probeWidth == width);
	    CHECK (probeHeight == height);

	    const bool swapped = orientation >= 5;
	    const int columns = swapped ? 2 : 3;

	    CHECK (width == columns * BLOCK);
	    CHECK (height == (swapped ? 3 : 2) * BLOCK);

	    for (int block = 0; block < 6; block++) {
		const int x = (block % columns) * BLOCK + BLOCK / 2;
		const int y = (block / columns) * BLOCK + BLOCK / 2;

		CHECK (near (
		    pixels + (static_cast<size_t> (y) * width + x) * 4, BLOCK_COLORS[expected[orientation - 1][block]]
		));
	    }

	    stbi_image_free (pixels);
	}
    }
}

TEST_CASE ("Broken or missing EXIF data leaves the image as it is") {
    const auto jpeg = blockJpeg ();

    std::string badTiff = withOrientation (jpeg, 6, false);
    // neither II nor MM
    badTiff[2 + 4 + 6] = 'X';

    std::string badValue = withOrientation (jpeg, 9, false);

    for (const auto& bytes : { jpeg, badTiff, badValue }) {
	int width = 0;
	int height = 0;
	stbi_uc* pixels = WallpaperEngine::Render::decodeImageRGBA (bytes.data (), bytes.size (), width, height);

	REQUIRE (pixels != nullptr);
	CHECK (width == 3 * BLOCK);
	CHECK (height == 2 * BLOCK);
	CHECK (near (pixels + (BLOCK / 2 * width + BLOCK / 2) * 4, BLOCK_COLORS[0]));
	stbi_image_free (pixels);
    }
}
