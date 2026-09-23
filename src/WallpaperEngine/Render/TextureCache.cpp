#include "TextureCache.h"

#include "AlbumTexture.h"
#include "WallpaperEngine/FileSystem/Container.h"

#include "CTexture.h"
#include "WallpaperEngine/Assets/AssetLoadException.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Helpers/ContextAware.h"

#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Parsers/TextureParser.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <stb_image.h>

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

bool isRawImageExtension (const std::string& extension) {
    return extension == ".jpg" || extension == ".jpeg" || extension == ".png" || extension == ".bmp"
	|| extension == ".tga" || extension == ".gif";
}

bool isRawVideoExtension (const std::string& extension) {
    return extension == ".mp4" || extension == ".webm" || extension == ".mkv" || extension == ".mov"
	|| extension == ".m4v" || extension == ".avi";
}

/**
 * Wraps a plain image/video file (what "scenetexture" properties hold once the user picks a file, as opposed
 * to the .tex assets shipped inside the scene) into the same header the .tex parser would have produced
 */
TextureUniquePtr buildRawTexture (const std::string& filename, ReadStream& stream) {
    const auto extension = lowerExtension (filename);
    const bool video = isRawVideoExtension (extension);

    const std::string contents ((std::istreambuf_iterator<char> (stream)), std::istreambuf_iterator<char> ());

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
	int channels = 0;

	if (!stbi_info_from_memory (
		reinterpret_cast<const stbi_uc*> (contents.data ()), static_cast<int> (contents.size ()), &width,
		&height, &channels
	    )) {
	    sLog.exception ("Cannot decode image ", filename, ": ", stbi_failure_reason ());
	}
    }

    auto mipmap = std::make_shared<Mipmap> ();

    mipmap->width = width;
    mipmap->height = height;
    mipmap->uncompressedSize = static_cast<int> (contents.size ());
    mipmap->uncompressedData = std::make_unique<char[]> (contents.size ());
    memcpy (mipmap->uncompressedData.get (), contents.data (), contents.size ());

    auto result = std::make_unique<Texture> ();

    result->containerVersion = ContainerVersion_TEXB0003;
    result->format = TextureFormat_ARGB8888;
    result->flags = TextureFlags_ClampUVs;
    result->width = width;
    result->height = height;
    result->textureWidth = width;
    result->textureHeight = height;
    result->freeImageFormat = video ? FIF_MP4 : FIF_PNG;
    result->isVideoMp4 = video;
    result->imageCount = 1;
    result->images.emplace (0, MipmapList { mipmap });

    return result;
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

std::shared_ptr<const TextureProvider> TextureCache::resolve (const std::string& filename, const Project& requester) {
    if (const auto shared = this->m_sharedTextures.find (filename); shared != this->m_sharedTextures.end ()) {
	return shared->second;
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

	    auto metadataLoader = [&project] (const std::string& metaFilename) -> std::string {
		std::filesystem::path fullPath = std::filesystem::path ("materials") / metaFilename;
		return project.assetLocator->readString (fullPath);
	    };

	    parsedTexture = TextureParser::parse (stream, filename, metadataLoader);
	} catch (AssetLoadException&) {
	    const auto extension = lowerExtension (filename);

	    if (!isRawImageExtension (extension) && !isRawVideoExtension (extension)) {
		throw;
	    }

	    const auto raw = project.assetLocator->read (filename);

	    parsedTexture = buildRawTexture (filename, *raw);
	}
	auto texture = std::make_shared<CTexture> (this->getContext (), std::move (parsedTexture));
	texture->label (filename);

	this->m_textureCache.insert_or_assign (key, texture);

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
}
