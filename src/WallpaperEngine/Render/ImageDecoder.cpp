#include "ImageDecoder.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace {
uint16_t read16 (const uint8_t* p, bool bigEndian) { return bigEndian ? (p[0] << 8) | p[1] : (p[1] << 8) | p[0]; }

uint32_t read32 (const uint8_t* p, bool bigEndian) {
    return bigEndian ? (uint32_t (p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]
		     : (uint32_t (p[3]) << 24) | (p[2] << 16) | (p[1] << 8) | p[0];
}

int orientationFromTiff (const uint8_t* tiff, size_t size) {
    if (size < 8) {
	return 1;
    }

    bool bigEndian;

    if (tiff[0] == 'I' && tiff[1] == 'I') {
	bigEndian = false;
    } else if (tiff[0] == 'M' && tiff[1] == 'M') {
	bigEndian = true;
    } else {
	return 1;
    }

    const uint32_t ifd = read32 (tiff + 4, bigEndian);

    if (ifd > size - 2) {
	return 1;
    }

    const uint16_t count = read16 (tiff + ifd, bigEndian);

    for (uint16_t i = 0; i < count; i++) {
	const size_t entry = ifd + 2 + i * 12ul;

	if (entry + 12 > size) {
	    break;
	}

	// 0x0112 Orientation, SHORT
	if (read16 (tiff + entry, bigEndian) == 0x0112 && read16 (tiff + entry + 2, bigEndian) == 3) {
	    const int value = read16 (tiff + entry + 8, bigEndian);
	    return value >= 1 && value <= 8 ? value : 1;
	}
    }

    return 1;
}

int jpegOrientation (const uint8_t* data, size_t size) {
    if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) {
	return 1;
    }

    size_t offset = 2;

    while (offset + 4 <= size) {
	if (data[offset] != 0xFF) {
	    return 1;
	}

	const uint8_t marker = data[offset + 1];

	// fill bytes
	if (marker == 0xFF) {
	    offset++;
	    continue;
	}

	// start of scan or end of image, no more metadata
	if (marker == 0xDA || marker == 0xD9) {
	    return 1;
	}

	const size_t length = (data[offset + 2] << 8) | data[offset + 3];

	if (length < 2 || offset + 2 + length > size) {
	    return 1;
	}

	const uint8_t* segment = data + offset + 4;
	const size_t segmentSize = length - 2;

	if (marker == 0xE1 && segmentSize > 6 && memcmp (segment, "Exif\0\0", 6) == 0) {
	    return orientationFromTiff (segment + 6, segmentSize - 6);
	}

	offset += 2 + length;
    }

    return 1;
}

int orientationOf (const void* data, size_t size) { return jpegOrientation (static_cast<const uint8_t*> (data), size); }

bool swapsAxes (int orientation) { return orientation >= 5; }
} // namespace

namespace WallpaperEngine::Render {
stbi_uc* decodeImageRGBA (const void* data, size_t size, int& width, int& height) {
    int channels;
    stbi_uc* pixels = stbi_load_from_memory (
	static_cast<const stbi_uc*> (data), static_cast<int> (size), &width, &height, &channels, 4
    );

    if (pixels == nullptr) {
	return nullptr;
    }

    const int orientation = orientationOf (data, size);

    if (orientation == 1) {
	return pixels;
    }

    const int srcWidth = width;
    const int srcHeight = height;
    const int dstWidth = swapsAxes (orientation) ? srcHeight : srcWidth;
    const int dstHeight = swapsAxes (orientation) ? srcWidth : srcHeight;
    auto* rotated = static_cast<stbi_uc*> (malloc (static_cast<size_t> (dstWidth) * dstHeight * 4));

    if (rotated == nullptr) {
	return pixels;
    }

    const auto* src = reinterpret_cast<const uint32_t*> (pixels);
    auto* dst = reinterpret_cast<uint32_t*> (rotated);

    for (int y = 0; y < dstHeight; y++) {
	for (int x = 0; x < dstWidth; x++) {
	    int sx, sy;

	    switch (orientation) {
		case 2:
		    sx = srcWidth - 1 - x;
		    sy = y;
		    break;
		case 3:
		    sx = srcWidth - 1 - x;
		    sy = srcHeight - 1 - y;
		    break;
		case 4:
		    sx = x;
		    sy = srcHeight - 1 - y;
		    break;
		case 5:
		    sx = y;
		    sy = x;
		    break;
		case 6:
		    sx = y;
		    sy = srcHeight - 1 - x;
		    break;
		case 7:
		    sx = srcWidth - 1 - y;
		    sy = srcHeight - 1 - x;
		    break;
		default:
		    sx = srcWidth - 1 - y;
		    sy = x;
		    break;
	    }

	    dst[static_cast<size_t> (y) * dstWidth + x] = src[static_cast<size_t> (sy) * srcWidth + sx];
	}
    }

    stbi_image_free (pixels);
    width = dstWidth;
    height = dstHeight;
    return rotated;
}

bool decodedImageSize (const void* data, size_t size, int& width, int& height) {
    int channels;

    if (!stbi_info_from_memory (
	    static_cast<const stbi_uc*> (data), static_cast<int> (size), &width, &height, &channels
	)) {
	return false;
    }

    if (swapsAxes (orientationOf (data, size))) {
	std::swap (width, height);
    }

    return true;
}
} // namespace WallpaperEngine::Render
