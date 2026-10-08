#include "TextureCache.h"

#include "AlbumTexture.h"
#include "WallpaperEngine/FileSystem/Container.h"

#include "CTexture.h"
#include "WallpaperEngine/Assets/AssetLoadException.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Helpers/ContextAware.h"

#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Data/Parsers/TextureParser.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "GifAnimation.h"
#include "ImageDecoder.h"

#include <lz4.h>
#include <map>
#include <ranges>
#include <set>
#include <stb_image.h>

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include <nanosvg.h>
#include <nanosvgrast.h>
#pragma GCC diagnostic pop

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}

using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::FileSystem;
using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Data::Assets;

namespace {
struct MemoryReader {
    const uint8_t* data;
    size_t size;
    size_t position;
};

int memoryRead (void* opaque, uint8_t* buffer, int length) {
    auto* reader = static_cast<MemoryReader*> (opaque);

    if (reader->position >= reader->size) {
	return AVERROR_EOF;
    }

    const auto count = std::min (static_cast<size_t> (length), reader->size - reader->position);

    memcpy (buffer, reader->data + reader->position, count);
    reader->position += count;

    return static_cast<int> (count);
}

int64_t memorySeek (void* opaque, int64_t offset, int whence) {
    auto* reader = static_cast<MemoryReader*> (opaque);

    if (whence == AVSEEK_SIZE) {
	return static_cast<int64_t> (reader->size);
    }

    int64_t target = offset;

    if ((whence & ~AVSEEK_FORCE) == SEEK_CUR) {
	target += static_cast<int64_t> (reader->position);
    } else if ((whence & ~AVSEEK_FORCE) == SEEK_END) {
	target += static_cast<int64_t> (reader->size);
    }

    if (target < 0 || target > static_cast<int64_t> (reader->size)) {
	return -1;
    }

    reader->position = static_cast<size_t> (target);

    return target;
}

/** Reads the video stream dimensions of an in-memory container, {0, 0} if it cannot be probed */
std::pair<uint32_t, uint32_t> probeVideoSize (const uint8_t* data, size_t size) {
    MemoryReader reader { data, size, 0 };
    constexpr int ioBufferSize = 32768;

    auto* ioBuffer = static_cast<uint8_t*> (av_malloc (ioBufferSize));
    AVIOContext* io = avio_alloc_context (ioBuffer, ioBufferSize, 0, &reader, memoryRead, nullptr, memorySeek);
    AVFormatContext* format = avformat_alloc_context ();

    format->pb = io;

    std::pair<uint32_t, uint32_t> result { 0, 0 };

    // on failure avformat_open_input frees the format context itself but leaves the io context alone
    if (avformat_open_input (&format, nullptr, nullptr, nullptr) == 0) {
	if (avformat_find_stream_info (format, nullptr) >= 0) {
	    for (unsigned int i = 0; i < format->nb_streams; i++) {
		const auto* params = format->streams[i]->codecpar;

		if (params->codec_type == AVMEDIA_TYPE_VIDEO && params->width > 0 && params->height > 0) {
		    result = { static_cast<uint32_t> (params->width), static_cast<uint32_t> (params->height) };
		    break;
		}
	    }
	}

	avformat_close_input (&format);
    }

    av_freep (&io->buffer);
    avio_context_free (&io);

    return result;
}

std::string lowerExtension (const std::string& filename) {
    auto extension = std::filesystem::path (filename).extension ().string ();

    std::ranges::transform (extension, extension.begin (), [] (unsigned char c) { return std::tolower (c); });

    return extension;
}

bool isRawVideoExtension (const std::string& extension) {
    return extension == ".mp4" || extension == ".webm" || extension == ".mkv" || extension == ".mov"
	|| extension == ".m4v" || extension == ".avi";
}

/**
 * Wraps a plain image/video file (what "scenetexture" properties hold once the user picks a file, as opposed
 * to the .tex assets shipped inside the scene) into the same header the .tex parser would have produced
 */
TextureUniquePtr buildRawTexture (const std::string& filename, const std::string& contents) {
    const bool video = isRawVideoExtension (lowerExtension (filename));

    if (contents.empty ()) {
	sLog.exception ("Cannot load ", filename, ": file is empty");
    }

    int width = 0;
    int height = 0;

    if (video) {
	const auto [videoWidth, videoHeight]
	    = probeVideoSize (reinterpret_cast<const uint8_t*> (contents.data ()), contents.size ());

	// mpv resizes the output texture once it knows the real size, this is only the initial one
	width = videoWidth > 0 ? static_cast<int> (videoWidth) : 1920;
	height = videoHeight > 0 ? static_cast<int> (videoHeight) : 1080;
    } else {
	if (!decodedImageSize (contents.data (), contents.size (), width, height)) {
	    sLog.exception ("Cannot decode image ", filename, ": ", stbi_failure_reason ());
	}
    }

    // wallpaper64.exe 2.8.42 sub_1400EB570 animates a file by its extension, a GIF under any other name loads as a
    // still image. The texture is the GIF's logical screen and gets texture flag 1, point sampling
    const bool animatedGif = !video && lowerExtension (filename) == ".gif"
	&& GifAnimation::canvasSize (contents.data (), contents.size (), width, height);

    auto mipmap = std::make_shared<Mipmap> ();

    mipmap->width = width;
    mipmap->height = height;
    mipmap->uncompressedSize = static_cast<int> (contents.size ());
    mipmap->uncompressedData = std::make_unique<char[]> (contents.size ());
    memcpy (mipmap->uncompressedData.get (), contents.data (), contents.size ());

    auto result = std::make_unique<Texture> ();

    result->containerVersion = 3;
    result->format = TextureFormat_ARGB8888;
    result->flags = animatedGif ? TextureFlags_ClampUVs | TextureFlags_NoInterpolation : TextureFlags_ClampUVs;
    result->width = width;
    result->height = height;
    result->textureWidth = width;
    result->textureHeight = height;
    result->freeImageFormat = video ? FIF_MP4 : FIF_PNG;
    result->isVideoMp4 = video;
    result->isAnimatedGif = animatedGif;
    result->imageCount = 1;
    result->images.emplace (0, MipmapList { mipmap });

    return result;
}

TextureUniquePtr buildIconTexture (const std::filesystem::path& path) {
    constexpr int ICON_SIZE = 256;
    std::vector<unsigned char> pixels;
    int width = 0;
    int height = 0;

    if (lowerExtension (path.string ()) == ".svg") {
	NSVGimage* image = nsvgParseFromFile (path.c_str (), "px", 96.0f);

	if (image == nullptr || image->width <= 0.0f || image->height <= 0.0f) {
	    nsvgDelete (image);
	    sLog.exception ("Cannot parse icon ", path.string ());
	}

	const float scale = static_cast<float> (ICON_SIZE) / std::max (image->width, image->height);
	width = std::max (1, static_cast<int> (image->width * scale));
	height = std::max (1, static_cast<int> (image->height * scale));
	pixels.resize (static_cast<size_t> (width) * height * 4);

	NSVGrasterizer* rasterizer = nsvgCreateRasterizer ();
	nsvgRasterize (rasterizer, image, 0.0f, 0.0f, scale, pixels.data (), width, height, width * 4);
	nsvgDeleteRasterizer (rasterizer);
	nsvgDelete (image);
    } else {
	int channels = 0;
	stbi_uc* data = stbi_load (path.c_str (), &width, &height, &channels, 4);

	if (data == nullptr) {
	    sLog.exception ("Cannot decode icon ", path.string (), ": ", stbi_failure_reason ());
	}

	pixels.assign (data, data + static_cast<size_t> (width) * height * 4);
	stbi_image_free (data);
    }

    auto mipmap = std::make_shared<Mipmap> ();

    mipmap->width = width;
    mipmap->height = height;
    mipmap->uncompressedSize = static_cast<int> (pixels.size ());
    mipmap->uncompressedData = std::make_unique<char[]> (pixels.size ());
    memcpy (mipmap->uncompressedData.get (), pixels.data (), pixels.size ());

    auto result = std::make_unique<Texture> ();

    result->containerVersion = 3;
    result->format = TextureFormat_ARGB8888;
    result->flags = TextureFlags_ClampUVs;
    result->width = width;
    result->height = height;
    result->textureWidth = width;
    result->textureHeight = height;
    result->freeImageFormat = FIF_UNKNOWN;
    result->imageCount = 1;
    result->images.emplace (0, MipmapList { mipmap });

    return result;
}

struct TextureCondition {
    std::string name;
    std::optional<std::string> wanted;
};

std::optional<TextureCondition> parseTextureCondition (const std::string& json) {
    const auto parsed = JSON::parse (json, nullptr, false);

    if (!parsed.is_object () || !parsed.contains ("condition")) {
	return std::nullopt;
    }

    const auto& condition = parsed["condition"];

    if (condition.is_string ()) {
	return TextureCondition { .name = condition.get<std::string> () };
    }

    if (condition.is_object () && condition.contains ("name") && condition["name"].is_string ()
	&& condition.contains ("condition") && condition["condition"].is_string ()) {
	return TextureCondition { .name = condition["name"].get<std::string> (),
				  .wanted = condition["condition"].get<std::string> () };
    }

    return std::nullopt;
}

// sub_140174A60: a plain name needs a true bool property, the object form a string property equal to it
bool textureConditionHolds (const std::string& json, const Project& project) {
    const auto condition = parseTextureCondition (json);

    if (!condition.has_value ()) {
	return false;
    }

    const auto property = project.properties.find (condition->name);

    if (property == project.properties.end ()) {
	return false;
    }

    if (!condition->wanted.has_value ()) {
	return property->second->getType () == DynamicValue::Boolean && property->second->getBool ();
    }

    return property->second->getType () == DynamicValue::String && property->second->getString () == *condition->wanted;
}

// how many bytes of patch pixels blitTexturePatch reads, WE doesn't check it
size_t texturePatchSize (const uint32_t format, const uint32_t flags, const MipmapPatch& patch) {
    const size_t width = patch.width;
    const size_t lastBlockRow = (patch.height - 1) / 4 * 4;

    if (flags & 1) {
	return 4 * width * patch.height;
    }

    switch (format) {
	case TextureFormat_ARGB8888:
	    return 4 * width * patch.height;
	case TextureFormat_DXT5:
	case TextureFormat_DXT3:
	    return lastBlockRow * width + 4 * width;
	case TextureFormat_DXT1:
	    return ((lastBlockRow * width) >> 1) + 2 * width;
	case TextureFormat_RG88:
	    return 2 * width * patch.height;
	case TextureFormat_R8:
	    return width * patch.height;
	default:
	    return 0;
    }
}

// wallpaper64.exe 2.8.42 sub_14015C480
void blitTexturePatch (
    const uint32_t format, const uint32_t flags, const MipmapPatch& patch, const unsigned char* source,
    unsigned char* target, const uint32_t targetWidth
) {
    if (flags & 1) {
	if (format != TextureFormat_ARGB8888) {
	    return;
	}

	for (uint32_t row = 0; row < patch.height; row++) {
	    for (uint32_t column = 0; column < patch.width; column++) {
		uint32_t src;
		uint32_t dst;
		unsigned char* out = target + 4 * (column + patch.x + targetWidth * (row + patch.y));

		memcpy (&src, source + 4 * (column + row * patch.width), 4);
		memcpy (&dst, out, 4);

		const uint32_t alpha = src >> 24;
		const uint32_t green
		    = static_cast<uint16_t> ((dst & 0xFF00) + ((alpha * ((src & 0xFF00) - (dst & 0xFF00))) >> 8))
		    & 0xFF00;
		const uint32_t redBlue
		    = ((dst & 0xFF00FF) + ((alpha * ((src & 0xFF00FF) - (dst & 0xFF00FF))) >> 8)) & 0xFF00FF;
		const uint32_t result = (std::max (dst >> 24, alpha) << 24) | green | redBlue;

		memcpy (out, &result, 4);
	    }
	}

	return;
    }

    const size_t width = targetWidth;

    switch (format) {
	case TextureFormat_ARGB8888:
	    for (size_t row = 0; row < patch.height; row++) {
		memcpy (
		    target + 4 * (patch.x + width * (row + patch.y)), source + 4 * row * patch.width, 4 * patch.width
		);
	    }
	    break;
	case TextureFormat_DXT5:
	case TextureFormat_DXT3:
	    for (size_t row = 0; row < patch.height; row += 4) {
		memcpy (target + width * (row + patch.y) + 4 * patch.x, source + row * patch.width, 4 * patch.width);
	    }
	    break;
	case TextureFormat_DXT1:
	    for (size_t row = 0; row < patch.height; row += 4) {
		memcpy (
		    target + ((width * (row + patch.y) + 4 * patch.x) >> 1), source + ((row * patch.width) >> 1),
		    2 * patch.width
		);
	    }
	    break;
	case TextureFormat_RG88:
	    for (size_t row = 0; row < patch.height; row++) {
		memcpy (
		    target + 2 * (patch.x + width * (row + patch.y)), source + 2 * row * patch.width, 2 * patch.width
		);
	    }
	    break;
	case TextureFormat_R8:
	    for (size_t row = 0; row < patch.height; row++) {
		memcpy (target + patch.x + width * (row + patch.y), source + row * patch.width, patch.width);
	    }
	    break;
	default:
	    break;
    }
}

// TEXB0004 conditional patches: the parser (sub_14015C8D0) picks the patches of the conditions that hold, the texture
// job (sub_1400CE760) paints them over the decoded mipmap
void applyTexturePatches (Texture& texture, const Project& project) {
    if (texture.conditions.empty ()) {
	return;
    }

    std::set<uint32_t> groups;
    std::map<uint32_t, uint32_t> selected;

    for (const auto& condition : texture.conditions) {
	if (!textureConditionHolds (condition.json, project) || !groups.insert (condition.group).second) {
	    continue;
	}

	selected[condition.key] = condition.flags;
    }

    const bool native = texture.freeImageFormat != FIF_UNKNOWN;

    for (auto& mipmaps : texture.images | std::views::values) {
	for (const auto& mipmap : mipmaps) {
	    std::vector<std::pair<const MipmapPatch*, uint32_t>> patches;

	    for (const auto& patch : mipmap->patches) {
		if (const auto flags = selected.find (patch.key); flags != selected.end ()) {
		    patches.emplace_back (&patch, flags->second);
		}
	    }

	    // only the last replacing patch matters, WE keeps the one before it too (applied, then thrown away)
	    for (size_t index = patches.size (); index-- > 1;) {
		if (patches[index].second & 2) {
		    patches.erase (patches.begin (), patches.begin () + static_cast<long> (index - 1));
		    break;
		}
	    }

	    if (patches.empty ()) {
		continue;
	    }

	    std::vector<unsigned char> pixels;

	    if (native) {
		int width = 0;
		int height = 0;
		stbi_uc* decoded
		    = decodeImageRGBA (mipmap->uncompressedData.get (), mipmap->uncompressedSize, width, height);

		if (decoded == nullptr || width != static_cast<int> (mipmap->width)
		    || height != static_cast<int> (mipmap->height)) {
		    stbi_image_free (decoded);
		    continue;
		}

		pixels.assign (decoded, decoded + static_cast<size_t> (width) * height * 4);
		stbi_image_free (decoded);
	    } else {
		pixels.assign (
		    mipmap->uncompressedData.get (), mipmap->uncompressedData.get () + mipmap->uncompressedSize
		);
	    }

	    const uint32_t format = native ? TextureFormat_ARGB8888 : texture.format;

	    for (const auto& [patch, flags] : patches) {
		if (flags & 2) {
		    // the patch's raw data becomes the mipmap, it has to cover what the upload reads
		    if (patch->data.size () >= pixels.size ()) {
			pixels.assign (patch->data.begin (), patch->data.end ());
		    }
		    break;
		}

		if (patch->width == 0 || patch->height == 0 || patch->x + patch->width > mipmap->width
		    || patch->y + patch->height > mipmap->height || patch->data.empty ()) {
		    continue;
		}

		const size_t size = 4 * static_cast<size_t> (patch->width) * patch->height;
		std::vector<unsigned char> source;

		if (mipmap->compression == 1) {
		    source.resize (size);

		    if (LZ4_decompress_safe (
			    patch->data.data (), reinterpret_cast<char*> (source.data ()),
			    static_cast<int> (patch->data.size ()), static_cast<int> (size)
			)
			< 0) {
			continue;
		    }
		} else if (native) {
		    int width = 0;
		    int height = 0;
		    stbi_uc* decoded = decodeImageRGBA (patch->data.data (), patch->data.size (), width, height);

		    if (decoded == nullptr || width != static_cast<int> (patch->width)
			|| height != static_cast<int> (patch->height)) {
			stbi_image_free (decoded);
			continue;
		    }

		    source.assign (decoded, decoded + size);
		    stbi_image_free (decoded);
		} else {
		    source.assign (patch->data.begin (), patch->data.end ());
		}

		if (source.size () < texturePatchSize (format, flags, *patch)
		    || pixels.size ()
			< texturePatchSize (format, 0, { .width = mipmap->width, .height = mipmap->height })) {
		    continue;
		}

		blitTexturePatch (format, flags, *patch, source.data (), pixels.data (), mipmap->width);
	    }

	    if (native) {
		mipmap->composedPixels = std::move (pixels);
	    } else {
		mipmap->uncompressedSize = static_cast<int> (pixels.size ());
		mipmap->uncompressedData = std::make_unique<char[]> (pixels.size ());
		memcpy (mipmap->uncompressedData.get (), pixels.data (), pixels.size ());
	    }
	}
    }
}
} // namespace

TextureCache::TextureCache (RenderContext& context) : Helpers::ContextAware (context) {
    this->m_currentThumbnail = std::make_shared<AlbumTexture> (this->getContext ());

#if !NDEBUG
    glObjectLabel (GL_TEXTURE, this->m_currentThumbnail->getTextureID (0), -1, "$mediaThumbnail");
#endif

    this->m_previousThumbnail = std::make_shared<AlbumTexture> (this->getContext ());

#if !NDEBUG
    glObjectLabel (GL_TEXTURE, this->m_previousThumbnail->getTextureID (0), -1, "$mediaPreviousThumbnail");
#endif

    this->m_currentThumbnail->load ();

    this->store ("$mediaThumbnail", this->m_currentThumbnail);
    this->store ("$mediaPreviousThumbnail", this->m_previousThumbnail);

    this->m_mediaCallback = this->getContext ().getMediaSource ().addAlbumArtListener (
	[this] (const Media::MediaSource::MediaInfo& data) {
	    if (this->m_currentThumbnail->isReady ()) {
		this->m_previousThumbnail->copyContents (*this->m_currentThumbnail);
	    }

	    this->m_currentThumbnail->load ();
	}
    );
}

TextureCache::~TextureCache () { this->m_mediaCallback (); }

std::shared_ptr<const TextureProvider>
TextureCache::findLoaded (const std::string& filename, const Project& project) const {
    if (const auto shared = this->m_sharedTextures.find (filename); shared != this->m_sharedTextures.end ()) {
	return shared->second;
    }

    const auto found = this->m_textureCache.find (std::make_pair (&project, filename));
    return found != this->m_textureCache.end () ? found->second : nullptr;
}

std::shared_ptr<const TextureProvider> TextureCache::resolve (const std::string& filename, const Project& requester) {
    if (const auto shared = this->m_sharedTextures.find (filename); shared != this->m_sharedTextures.end ()) {
	return shared->second;
    }

    if (constexpr std::string_view prefix = "$usershortcut:"; filename.starts_with (prefix)) {
	auto texture = std::make_shared<CTexture> (
	    this->getContext (), buildIconTexture (std::filesystem::path (filename.substr (prefix.size ())))
	);
	texture->label (filename);
	this->store (filename, texture);

	return texture;
    }

    const auto key = std::make_pair (&requester, filename);

    if (const auto found = this->m_textureCache.find (key); found != this->m_textureCache.end ()) {
	return found->second;
    }

    const auto load = [&] (const Project& project) {
	TextureUniquePtr parsedTexture;

	try {
	    const auto contents = project.assetLocator->texture (filename);
	    auto stream = BinaryReader (contents);

	    parsedTexture = TextureParser::parse (stream);
	    applyTexturePatches (*parsedTexture, project);
	} catch (AssetLoadException&) {
	    // WE picks the image decoder from the file contents (resourceutil64 FreeImage_GetFileTypeU), so
	    // e.g. a user-picked .jfif loads just like a .jpg
	    const std::string contents = project.assetLocator->readString (filename);
	    int width;
	    int height;

	    if (!isRawVideoExtension (lowerExtension (filename))
		&& !decodedImageSize (contents.data (), contents.size (), width, height)) {
		throw;
	    }

	    parsedTexture = buildRawTexture (filename, contents);
	}
	std::vector<std::string> conditionProperties;

	for (const auto& condition : parsedTexture->conditions) {
	    if (const auto parsed = parseTextureCondition (condition.json); parsed.has_value ()) {
		conditionProperties.push_back (parsed->name);
	    }
	}

	auto texture = std::make_shared<CTexture> (this->getContext (), std::move (parsedTexture));
	texture->label (filename);

	this->m_textureCache.insert_or_assign (key, texture);
	this->followConditions (texture, project, filename, conditionProperties);

	return texture;
    };

    try {
	return load (requester);
    } catch (AssetLoadException&) {
	// not shipped by the requesting project itself, it might still live in another loaded one
    }

    for (const auto& project : this->getContext ().getApp ().getBackgrounds () | std::views::values) {
	if (project.get () == &requester) {
	    continue;
	}

	try {
	    return load (*project);
	} catch (AssetLoadException&) {
	    // ignored, this happens if we're looking at the wrong background
	}
    }

    // TODO: fill in with a checkered pattern texture instead?
    throw AssetLoadException ("Cannot find file", filename, std::error_code ());
}

void TextureCache::followConditions (
    const std::shared_ptr<CTexture>& texture, const Project& project, const std::string& filename,
    const std::vector<std::string>& properties
) {
    auto entry = std::make_unique<ConditionalTexture> (ConditionalTexture {
	.texture = texture,
	.project = &project,
	.filename = filename,
    });

    for (const auto& name : properties) {
	if (const auto property = project.properties.find (name); property != project.properties.end ()) {
	    entry->listeners.push_back (property->second->listen ([dirty = &entry->dirty] (const DynamicValue&, auto) {
		*dirty = true;
	    }));
	}
    }

    if (!entry->listeners.empty ()) {
	this->m_conditionalTextures.push_back (std::move (entry));
    }
}

void TextureCache::repaintConditionalTextures () {
    const auto& backgrounds = this->getContext ().getApp ().getBackgrounds ();

    for (const auto& entry : this->m_conditionalTextures) {
	if (!entry->dirty) {
	    continue;
	}

	entry->dirty = false;
	const auto texture = entry->texture.lock ();
	const bool alive = std::ranges::any_of (backgrounds, [&entry] (const auto& background) {
	    return background.second.get () == entry->project;
	});

	if (texture == nullptr || !alive) {
	    continue;
	}

	// patches are picked while parsing (sub_14015C8D0), so parse again
	try {
	    const auto contents = entry->project->assetLocator->texture (entry->filename);
	    auto stream = BinaryReader (contents);
	    auto parsed = TextureParser::parse (stream);

	    applyTexturePatches (*parsed, *entry->project);

	    if (!texture->repaint (std::move (parsed))) {
		sLog.error ("Cannot repaint ", entry->filename, " for a property change, the texture layout differs");
	    }
	} catch (const std::exception& e) {
	    sLog.error ("Cannot repaint ", entry->filename, " for a property change: ", e.what ());
	}
    }
}

void TextureCache::store (const std::string& name, std::shared_ptr<const TextureProvider> texture) {
    this->m_sharedTextures.insert_or_assign (name, texture);
}

void TextureCache::prune () {
    const auto& backgrounds = this->getContext ().getApp ().getBackgrounds ();

    // entries of a gone project must not be inherited by a new one allocated at the same address
    std::erase_if (this->m_textureCache, [&backgrounds] (const auto& entry) {
	const auto alive = std::ranges::any_of (backgrounds, [&entry] (const auto& background) {
	    return background.second.get () == entry.first.first;
	});

	return !alive || entry.second.use_count () == 1;
    });

    std::erase_if (this->m_conditionalTextures, [&backgrounds] (const auto& entry) {
	const bool alive = std::ranges::any_of (backgrounds, [&entry] (const auto& background) {
	    return background.second.get () == entry->project;
	});

	return !alive || entry->texture.expired ();
    });
}
