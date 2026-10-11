#include "CoverImage.h"

#include "WallpaperEngine/Splat/SogLoader.h"

#include <cstring>
#include <fstream>
#include <string>

#include "stb_image.h"

using namespace WallpaperEngine::Media;

namespace {
constexpr int MAX_COVER_SIDE = 4096;

int hexValue (char c) {
    if (c >= '0' && c <= '9') {
	return c - '0';
    }

    if (c >= 'a' && c <= 'f') {
	return c - 'a' + 10;
    }

    if (c >= 'A' && c <= 'F') {
	return c - 'A' + 10;
    }

    return -1;
}

bool isWebP (std::span<const uint8_t> bytes) {
    return bytes.size () >= 12 && std::memcmp (bytes.data (), "RIFF", 4) == 0
	&& std::memcmp (bytes.data () + 8, "WEBP", 4) == 0;
}
} // namespace

std::optional<std::filesystem::path> WallpaperEngine::Media::localCoverPath (std::string_view url) {
    constexpr std::string_view scheme = "file://";

    if (!url.starts_with (scheme)) {
	return std::nullopt;
    }

    url.remove_prefix (scheme.size ());

    if (url.starts_with ("localhost/")) {
	url.remove_prefix (9);
    }

    if (!url.starts_with ('/')) {
	return std::nullopt;
    }

    std::string path;
    path.reserve (url.size ());

    for (size_t i = 0; i < url.size (); i++) {
	char c = url[i];

	if (c == '?' || c == '#') {
	    break;
	}

	if (c == '%') {
	    const int high = i + 2 < url.size () ? hexValue (url[i + 1]) : -1;
	    const int low = i + 2 < url.size () ? hexValue (url[i + 2]) : -1;

	    if (high < 0 || low < 0) {
		return std::nullopt;
	    }

	    c = static_cast<char> (high << 4 | low);
	    i += 2;
	}

	if (c == '\0') {
	    return std::nullopt;
	}

	path += c;
    }

    return std::filesystem::path (path);
}

std::optional<CoverImage> WallpaperEngine::Media::decodeCoverImage (std::span<const uint8_t> bytes) {
    if (bytes.empty () || bytes.size () > MAX_COVER_BYTES) {
	return std::nullopt;
    }

    CoverImage image;

    if (isWebP (bytes)) {
	try {
	    auto decoded
		= Splat::decodeWebP (std::string (reinterpret_cast<const char*> (bytes.data ()), bytes.size ()));

	    image.width = decoded.width;
	    image.height = decoded.height;
	    image.rgba = std::move (decoded.rgba);
	} catch (const std::exception&) {
	    return std::nullopt;
	}
    } else {
	int channels = 0;

	// check the size before decoding
	if (!stbi_info_from_memory (
		bytes.data (), static_cast<int> (bytes.size ()), &image.width, &image.height, &channels
	    )
	    || image.width <= 0 || image.height <= 0 || image.width > MAX_COVER_SIDE || image.height > MAX_COVER_SIDE) {
	    return std::nullopt;
	}

	auto* pixels = stbi_load_from_memory (
	    bytes.data (), static_cast<int> (bytes.size ()), &image.width, &image.height, &channels, 4
	);

	if (pixels == nullptr) {
	    return std::nullopt;
	}

	image.rgba.assign (pixels, pixels + static_cast<size_t> (image.width) * image.height * 4);
	stbi_image_free (pixels);
    }

    if (image.width <= 0 || image.height <= 0 || image.width > MAX_COVER_SIDE || image.height > MAX_COVER_SIDE
	|| image.rgba.size () != static_cast<size_t> (image.width) * image.height * 4) {
	return std::nullopt;
    }

    return image;
}

std::optional<CoverImage> WallpaperEngine::Media::loadCoverImage (const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size (path, error);

    if (error || size == 0 || size > MAX_COVER_BYTES) {
	return std::nullopt;
    }

    std::ifstream file (path, std::ios::binary);
    std::vector<uint8_t> bytes (size);

    if (!file.read (reinterpret_cast<char*> (bytes.data ()), static_cast<std::streamsize> (size))) {
	return std::nullopt;
    }

    return decodeCoverImage (bytes);
}
