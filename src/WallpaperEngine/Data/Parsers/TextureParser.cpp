#include <cmath>
#include <cstdlib>
#include <cstring>

#include <lz4.h>
#include <nlohmann/json.hpp>

#include "TextureParser.h"
#include "WallpaperEngine/Data/Assets/Texture.h"
#include "WallpaperEngine/Logging/Log.h"

using namespace WallpaperEngine::Data::Assets;
using namespace WallpaperEngine::Data::Parsers;

TextureUniquePtr TextureParser::parse (const BinaryReader& file) {
    auto result = std::make_unique<Texture> ();
    const std::string magic = file.nextNullTerminatedString ();

    // sub_14015E580: TEXV0004 is an untagged v0 TEXI + v0 TEXB, TEXV0005 tagged sections whose last four
    // digits are the version, stopping at the first unknown tag
    if (magic == "TEXV0004") {
	parseTextureHeader (*result, file, 0);
	parseContainer (*result, file, 0);
	return result;
    }

    if (magic != "TEXV0005") {
	sLog.exception ("unexpected texture container type: ", magic);
    }

    while (file.base ().peek () != std::char_traits<char>::eof ()) {
	const std::string tag = file.nextNullTerminatedString ();
	const uint32_t version = tag.size () > 4 ? static_cast<uint32_t> (std::atoi (tag.c_str () + 4)) : 0;

	if (tag.starts_with ("TEXI")) {
	    parseTextureHeader (*result, file, version);
	} else if (tag.starts_with ("TEXB")) {
	    parseContainer (*result, file, version);
	} else if (tag.starts_with ("TEXS")) {
	    parseAnimations (*result, file, version);
	} else {
	    break;
	}
    }

    return result;
}

MipmapSharedPtr TextureParser::parseMipmap (const BinaryReader& file, const Texture& header) {
    auto result = std::make_shared<Mipmap> ();

    result->width = file.nextUInt32 ();
    result->height = file.nextUInt32 ();

    // wallpaper64.exe sub_14015C8D0, volume textures (image filter LUTs)
    if (header.flags & TextureFlags_Volume) {
	result->depth = file.nextUInt32 ();
    }

    if (header.containerVersion >= 2) {
	result->compression = file.nextUInt32 ();
	result->uncompressedSize = file.nextInt ();
    }

    result->compressedSize = file.nextInt ();

    if ((result->compression & 1) == 0) {
	// misnamed: in uncompressed files compressedSize actually holds the file length
	result->uncompressedSize = result->compressedSize;
    }

    result->uncompressedData = std::unique_ptr<char[]> (new char[result->uncompressedSize]);

    if (result->compression & 1) {
	result->compressedData = std::unique_ptr<char[]> (new char[result->compressedSize]);
	file.next (result->compressedData.get (), result->compressedSize);
	int bytes = LZ4_decompress_safe (
	    result->compressedData.get (), result->uncompressedData.get (), result->compressedSize,
	    result->uncompressedSize
	);

	if (bytes < 0) {
	    sLog.exception ("Cannot decompress texture data, LZ4_decompress_safe returned an error");
	}
    } else {
	file.next (result->uncompressedData.get (), result->uncompressedSize);
    }

    return result;
}

void TextureParser::parseMipmapPatches (Mipmap& mipmap, const BinaryReader& file) {
    const uint32_t groupCount = file.nextUInt32 ();

    for (uint32_t group = 0; group < groupCount; group++) {
	const uint32_t patchCount = file.nextUInt32 ();

	for (uint32_t index = 0; index < patchCount; index++) {
	    MipmapPatch patch;

	    std::ignore = file.nextUInt32 ();
	    patch.key = file.nextUInt32 ();
	    patch.x = file.nextUInt32 ();
	    patch.y = file.nextUInt32 ();
	    patch.width = file.nextUInt32 ();
	    patch.height = file.nextUInt32 ();
	    patch.format = file.nextInt ();
	    patch.data.resize (file.nextUInt32 ());
	    file.next (patch.data.data (), patch.data.size ());
	    mipmap.patches.push_back (std::move (patch));
	}
    }
}

FrameSharedPtr TextureParser::parseFrameV1 (const BinaryReader& file) {
    auto result = std::make_shared<Frame> ();

    result->frameNumber = file.nextUInt32 ();
    result->frametime = file.nextFloat ();
    result->x = static_cast<float> (file.nextUInt32 ());
    result->y = static_cast<float> (file.nextUInt32 ());
    result->width1 = static_cast<float> (file.nextUInt32 ());
    result->width2 = static_cast<float> (file.nextUInt32 ());
    result->height2 = static_cast<float> (file.nextUInt32 ());
    result->height1 = static_cast<float> (file.nextUInt32 ());

    return result;
}

FrameSharedPtr TextureParser::parseFrame (const BinaryReader& file) {
    auto result = std::make_shared<Frame> ();

    result->frameNumber = file.nextUInt32 ();
    result->frametime = file.nextFloat ();
    result->x = file.nextFloat ();
    result->y = file.nextFloat ();
    result->width1 = file.nextFloat ();
    result->width2 = file.nextFloat ();
    result->height2 = file.nextFloat ();
    result->height1 = file.nextFloat ();

    return result;
}

TextureMap TextureParser::parseTextureMap (const JSON& it) {
    if (!it.is_array ()) {
	return {};
    }

    TextureMap result = {};
    int textureIndex = -1;

    for (const auto& cur : it) {
	textureIndex++;

	if (cur.is_null ()) {
	    continue;
	}

	if (cur.is_object ()) {
	    const auto nameIt = cur.find ("name");
	    if (nameIt != cur.end () && nameIt->is_string ()) {
		result.emplace (textureIndex, nameIt->get<std::string> ());
	    }
	} else if (cur.is_string ()) {
	    std::string texName = cur;
	    if (!texName.empty ()) {
		result.emplace (textureIndex, texName);
	    }
	}
    }

    return result;
}

// unknown formats are RGBA8 at upload (sub_1400D2A20)
TextureFormat TextureParser::parseTextureFormat (uint32_t value) { return static_cast<TextureFormat> (value); }

// sub_14015C760
void TextureParser::parseTextureHeader (Texture& header, const BinaryReader& file, const uint32_t version) {
    header.format = parseTextureFormat (file.nextUInt32 ());
    header.flags = parseTextureFlags (file.nextUInt32 ());
    header.textureWidth = file.nextUInt32 ();
    header.textureHeight = file.nextUInt32 ();
    header.width = file.nextUInt32 ();
    header.height = file.nextUInt32 ();

    if (header.flags & TextureFlags_Volume) {
	header.depth = file.nextUInt32 ();
    }

    if (version >= 1) {
	std::ignore = file.nextUInt32 ();
    }
}

// sub_14015C8D0: v1 adds the image count, v2 per mip compression, v3 FreeImage format, v4 conditions
void TextureParser::parseContainer (Texture& header, const BinaryReader& file, const uint32_t version) {
    header.containerVersion = version;
    header.imageCount = version >= 1 ? file.nextUInt32 () : 1;

    if (version >= 3) {
	header.freeImageFormat = parseFIF (file.nextUInt32 ());

	// no power of two padding for encoded images
	if (header.freeImageFormat != FIF_UNKNOWN) {
	    header.textureWidth = header.width;
	    header.textureHeight = header.height;
	}
    }

    const uint32_t conditionCount = version >= 4 ? file.nextUInt32 () : 0;

    for (uint32_t index = 0; index < conditionCount; index++) {
	TextureCondition condition;

	condition.group = file.nextUInt32 ();
	condition.key = file.nextUInt32 ();
	condition.flags = file.nextUInt32 ();
	condition.json = file.nextNullTerminatedString ();
	header.conditions.push_back (std::move (condition));
    }

    for (uint32_t image = 0; image < header.imageCount; image++) {
	const uint32_t mipmapCount = file.nextUInt32 ();
	MipmapList mipmaps;

	for (uint32_t mipmap = 0; mipmap < mipmapCount; mipmap++) {
	    mipmaps.emplace_back (parseMipmap (file, header));

	    // present whenever there are conditions, even empty
	    if (conditionCount > 0) {
		parseMipmapPatches (*mipmaps.back (), file);
	    }
	}

	header.images.emplace (image, mipmaps);
    }
}

// sub_14015E1D0: v1 has integer frame rects, v3+ stores the frame size after the count
void TextureParser::parseAnimations (Texture& header, const BinaryReader& file, const uint32_t version) {
    header.animatedVersion = version;

    uint32_t frameCount = file.nextUInt32 ();

    if (version >= 3) {
	header.gifWidth = file.nextUInt32 ();
	header.gifHeight = file.nextUInt32 ();
    } else {
	header.gifWidth = header.width;
	header.gifHeight = header.height;
    }

    while (frameCount-- > 0) {
	if (version == 1) {
	    header.frames.push_back (parseFrameV1 (file));
	} else {
	    header.frames.push_back (parseFrame (file));
	}
    }
}

uint32_t TextureParser::parseTextureFlags (uint32_t value) { return value & TextureFlags_All; }

FIF TextureParser::parseFIF (uint32_t value) {
    switch (value) {
	case FIF_UNKNOWN:
	case FIF_BMP:
	case FIF_ICO:
	case FIF_JPEG:
	case FIF_JNG:
	case FIF_KOALA:
	case FIF_LBM:
	case FIF_MNG:
	case FIF_PBM:
	case FIF_PBMRAW:
	case FIF_PCD:
	case FIF_PCX:
	case FIF_PGM:
	case FIF_PGMRAW:
	case FIF_PNG:
	case FIF_PPM:
	case FIF_PPMRAW:
	case FIF_RAS:
	case FIF_TARGA:
	case FIF_TIFF:
	case FIF_WBMP:
	case FIF_PSD:
	case FIF_CUT:
	case FIF_XBM:
	case FIF_XPM:
	case FIF_DDS:
	case FIF_GIF:
	case FIF_HDR:
	case FIF_FAXG3:
	case FIF_SGI:
	case FIF_EXR:
	case FIF_J2K:
	case FIF_JP2:
	case FIF_PFM:
	case FIF_PICT:
	case FIF_RAW:
	case FIF_WEBP:
	case FIF_JXR:
	    return static_cast<FIF> (value);

	default:
	    sLog.exception ("unknown free image format: ", value);
    }
}