#include "CTexture.h"
#include "WallpaperEngine/Logging/Log.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <lz4.h>
#include <thread>
#include <vector>

#include "ImageDecoder.h"

#define STB_IMAGE_IMPLEMENTATION
#include "RenderContext.h"

#include <stb_image.h>

using namespace WallpaperEngine::Render;

extern float g_Time;
extern float g_TimeLast;

CTexture::CTexture (RenderContext& context, TextureUniquePtr header) :
    Helpers::ContextAware (context), m_header (std::move (header)) {
    this->setupResolution ();
    const UploadFormat upload = this->uploadFormat ();

    GLint maxTextureSize = 0;
    glGetIntegerv (GL_MAX_TEXTURE_SIZE, &maxTextureSize);
    if (this->m_header->textureWidth > static_cast<uint32_t> (maxTextureSize)
	|| this->m_header->textureHeight > static_cast<uint32_t> (maxTextureSize)) {
	sLog.error (
	    "Texture ", this->m_header->textureWidth, "x", this->m_header->textureHeight,
	    " exceeds this GL context's GL_MAX_TEXTURE_SIZE (", maxTextureSize, "); it will fail to upload"
	);
    }

    // videos only get one framebuffer and one mipmap
    if (this->m_header->isVideoMp4 || this->m_header->flags & TextureFlags_Video) {
	if (this->m_header->images.empty () || this->m_header->images.begin ()->second.empty ()) {
	    sLog.exception ("Cannot load video texture, no mipmaps found");
	}

	this->m_textureID = new GLuint[1];
	glGenTextures (1, this->m_textureID);
	this->setupOpenGLParameters (0);

	const auto mipmap = *this->m_header->images.begin ()->second.begin ();

	this->m_player = std::make_unique<GLPlayer> (
	    this->getContext (), this->m_textureID[0],
	    std::make_unique<MemoryStreamProtocol> (mipmap->uncompressedData.get (), mipmap->uncompressedSize),
	    this->m_header->textureWidth, this->m_header->textureHeight
	);
	this->m_player->setMuted ();
	this->m_player->setVolume (0.0f);
	this->m_player->disableAudio ();
	return;
    }

    this->m_textureID = new GLuint[this->m_header->imageCount];
    glGenTextures (this->m_header->imageCount, this->m_textureID);

    // WE decodes the first frame when the texture is made, a GIF it can't play fails to load there, here it falls
    // back to the still image below
    if (this->m_header->isAnimatedGif) {
	const auto& mipmap = *this->m_header->images.begin ()->second.begin ();

	this->m_gif = GifAnimation::open (mipmap->uncompressedData.get (), mipmap->uncompressedSize);

	if (this->m_gif != nullptr && this->m_gif->advance (0.0f)) {
	    this->setupOpenGLParameters (0);
	    glTexImage2D (
		GL_TEXTURE_2D, 0, GL_RGBA8, this->m_gif->width (), this->m_gif->height (), 0, GL_RGBA, GL_UNSIGNED_BYTE,
		this->m_gif->pixels ()
	    );
	    this->m_gifTime = g_Time;
	    return;
	}

	this->m_gif.reset ();
    }

    this->uploadImages (upload);
}

void CTexture::uploadImages (const UploadFormat& upload) const {
    struct Decoded {
	const Mipmap* mipmap;
	stbi_uc* pixels = nullptr;
	int width = 0;
	int height = 0;
    };

    // decode on worker threads, upload here
    std::vector<Decoded> decoded;

    if (this->m_header->freeImageFormat != FIF_UNKNOWN) {
	for (const auto& [index, mipmaps] : this->m_header->images) {
	    for (const auto& mipmap : mipmaps) {
		if (mipmap->composedPixels.empty ()) {
		    decoded.push_back ({ .mipmap = mipmap.get (), .width = mipmap->width, .height = mipmap->height });
		}
	    }
	}
    }

    const auto decode = [] (Decoded& job) {
	job.pixels = decodeImageRGBA (
	    job.mipmap->uncompressedData.get (), job.mipmap->uncompressedSize, job.width, job.height
	);
    };

    if (decoded.size () == 1) {
	decode (decoded.front ());
    } else if (decoded.size () > 1) {
	std::atomic<size_t> next = 0;
	const auto worker = [&decoded, &next, &decode] () {
	    for (size_t i = next++; i < decoded.size (); i = next++) {
		decode (decoded[i]);
	    }
	};
	const size_t threadCount
	    = std::min<size_t> (decoded.size (), std::max (1u, std::thread::hardware_concurrency ()));
	std::vector<std::thread> threads;

	for (size_t i = 1; i < threadCount; i++) {
	    threads.emplace_back (worker);
	}

	worker ();

	for (auto& thread : threads) {
	    thread.join ();
	}
    }

    auto nextDecoded = decoded.begin ();
    GLint previousAlignment = 4;

    // tightly packed rows like WE's D3D pitch (sub_1400EC220)
    glGetIntegerv (GL_UNPACK_ALIGNMENT, &previousAlignment);
    glPixelStorei (GL_UNPACK_ALIGNMENT, 1);

    for (const auto& [index, mipmaps] : this->m_header->images) {
	this->setupOpenGLParameters (index);

	int level = 0;

	for (const auto& mipmap : mipmaps) {
	    stbi_uc* handle = nullptr;
	    const void* dataptr = mipmap->uncompressedData.get ();
	    size_t dataSize = std::max (mipmap->uncompressedSize, 0);
	    int width = mipmap->width;
	    int height = mipmap->height;

	    if (!mipmap->composedPixels.empty ()) {
		dataptr = mipmap->composedPixels.data ();
		dataSize = mipmap->composedPixels.size ();
	    } else if (this->m_header->freeImageFormat != FIF_UNKNOWN) {
		dataptr = handle = nextDecoded->pixels;
		width = nextDecoded->width;
		height = nextDecoded->height;
		dataSize = handle != nullptr ? static_cast<size_t> (width) * height * 4 : 0;
		++nextDecoded;
	    }

	    if (upload.compressed) {
		glCompressedTexImage2D (
		    GL_TEXTURE_2D, level, upload.internalFormat, width, height, 0, static_cast<GLsizei> (dataSize),
		    dataptr
		);
	    } else {
		// pad a short mipmap with zeros instead of reading past it
		const size_t expected = static_cast<size_t> (width) * height * upload.bytesPerPixel;
		std::vector<unsigned char> padded;

		if (dataSize < expected) {
		    padded.resize (expected, 0);

		    if (dataptr != nullptr && dataSize > 0) {
			memcpy (padded.data (), dataptr, dataSize);
		    }

		    dataptr = padded.data ();
		}

		glTexImage2D (
		    GL_TEXTURE_2D, level, upload.internalFormat, width, height, 0, upload.format, upload.type, dataptr
		);
	    }

	    if (handle != nullptr) {
		stbi_image_free (handle);
	    }

	    std::vector<unsigned char> ().swap (mipmap->composedPixels);

	    level++;
	}
    }

    glPixelStorei (GL_UNPACK_ALIGNMENT, previousAlignment);
}

bool CTexture::repaint (TextureUniquePtr header) {
    const auto& current = *this->m_header;

    if (this->m_player || this->m_gif || header->imageCount != current.imageCount || header->format != current.format
	|| header->textureWidth != current.textureWidth || header->textureHeight != current.textureHeight
	|| header->freeImageFormat != current.freeImageFormat) {
	return false;
    }

    this->m_header = std::move (header);
    this->uploadImages (this->uploadFormat ());
    return true;
}

void CTexture::label (const std::string& name) const {
#if !NDEBUG
    const bool video = this->m_header->isVideoMp4 || this->m_header->flags & TextureFlags_Video;
    const uint32_t count = video ? 1 : this->m_header->imageCount;

    for (uint32_t i = 0; i < count; i++) {
	const std::string text = count == 1 ? name : name + " [" + std::to_string (i) + "]";
	glObjectLabel (GL_TEXTURE, this->m_textureID[i], -1, text.c_str ());
    }
#endif /* DEBUG */
}

CTexture::~CTexture () {
    // release the player first so nothing else keeps using it via null references
    this->m_player.reset ();

    if (this->m_header->isVideoMp4 || this->m_header->flags & TextureFlags_Video) {
	glDeleteTextures (1, this->m_textureID);
    } else {
	glDeleteTextures (this->m_header->imageCount, this->m_textureID);
    }

    delete[] this->m_textureID;
}

void CTexture::setupResolution () {
    if (this->m_header->freeImageFormat != FIF_UNKNOWN) {
	// wpengine-texture format always has one mipmap
	const auto element = this->m_header->images.find (0)->second.begin ();

	this->m_resolution = { (*element)->width, (*element)->height, this->m_header->width, this->m_header->height };
    } else {
	this->m_resolution = { this->m_header->textureWidth, this->m_header->textureHeight, this->m_header->width,
			       this->m_header->height };
    }
}

// WE uploads the raw mips as their DXGI format (sub_1400D2A20, sub_1400EB090): RGB888/RGB565 are RGBA8, RGB161616f
// RGBA16F, unknown RGBA8
CTexture::UploadFormat CTexture::uploadFormat () const {
    if (this->m_header->freeImageFormat != FIF_UNKNOWN) {
	return { GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, false };
    }

    switch (this->m_header->format) {
	case TextureFormat_DXT5:
	    return { GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, GL_NONE, GL_NONE, 0, true };
	case TextureFormat_DXT3:
	    return { GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, GL_NONE, GL_NONE, 0, true };
	case TextureFormat_DXT1:
	    return { GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, GL_NONE, GL_NONE, 0, true };
	case TextureFormat_BC7:
	    return { GL_COMPRESSED_RGBA_BPTC_UNORM, GL_NONE, GL_NONE, 0, true };
	case TextureFormat_RG88:
	    return { GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 2, false };
	case TextureFormat_R8:
	    return { GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1, false };
	case TextureFormat_RG1616f:
	    return { GL_RG16F, GL_RG, GL_HALF_FLOAT, 4, false };
	case TextureFormat_R16f:
	    return { GL_R16F, GL_RED, GL_HALF_FLOAT, 2, false };
	case TextureFormat_RGBa1010102:
	    return { GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, 4, false };
	case TextureFormat_RGBA16161616f:
	case TextureFormat_RGB161616f:
	    return { GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 8, false };
	case TextureFormat_R32f:
	    return { GL_R32F, GL_RED, GL_FLOAT, 4, false };
	case TextureFormat_RGBA16161616:
	case TextureFormat_RGB161616:
	    return { GL_RGBA16, GL_RGBA, GL_UNSIGNED_SHORT, 8, false };
	case TextureFormat_RGBA16161616S:
	case TextureFormat_RGB161616S:
	    return { GL_RGBA16_SNORM, GL_RGBA, GL_SHORT, 8, false };
	default:
	    break;
    }

    // depth, typeless R32 and unknown formats can't be sampled in D3D
    if (this->m_header->format >= 22 && this->m_header->format <= 27) {
	sLog.exception (
	    "Cannot load texture, format ", static_cast<uint32_t> (this->m_header->format), " is not sampleable"
	);
    }

    return { GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, false };
}

void CTexture::setupOpenGLParameters (const uint32_t textureID) const {
    glBindTexture (GL_TEXTURE_2D, this->m_textureID[textureID]);

    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, this->m_header->images[textureID].size () - 1);

    if (this->m_header->flags & TextureFlags_ClampUVs) {
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }

    if (this->m_header->flags & TextureFlags_NoInterpolation) {
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    } else {
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    }

    glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, 8.0f);
}

GLuint CTexture::getTextureID (const uint32_t imageIndex) const {
    if (imageIndex >= this->m_header->imageCount) {
	return this->m_textureID[0];
    }

    return this->m_textureID[imageIndex];
}

uint32_t CTexture::getTextureWidth (const uint32_t imageIndex) const {
    if (imageIndex >= this->m_header->imageCount) {
	return this->getHeader ().textureWidth;
    }

    return (*this->m_header->images[imageIndex].begin ())->width;
}

uint32_t CTexture::getTextureHeight (const uint32_t imageIndex) const {
    if (imageIndex >= this->m_header->imageCount) {
	return this->getHeader ().textureHeight;
    }

    return (*this->m_header->images[imageIndex].begin ())->height;
}

uint32_t CTexture::getRealWidth () const {
    return this->isAnimated () ? this->getHeader ().gifWidth : this->getHeader ().width;
}

uint32_t CTexture::getRealHeight () const {
    return this->isAnimated () ? this->getHeader ().gifHeight : this->getHeader ().height;
}

TextureFormat CTexture::getFormat () const { return this->getHeader ().format; }

uint32_t CTexture::getFlags () const { return this->getHeader ().flags; }

const Texture& CTexture::getHeader () const { return *this->m_header; }

const glm::vec4* CTexture::getResolution () const { return &this->m_resolution; }

const std::vector<FrameSharedPtr>& CTexture::getFrames () const { return this->getHeader ().frames; }

bool CTexture::isAnimated () const { return this->getHeader ().isAnimated (); }

void CTexture::incrementUsageCount () const {
    if (this->m_player) {
	this->m_player->incrementUsageCount ();
    }
}

void CTexture::decrementUsageCount () const {
    if (this->m_player) {
	this->m_player->decrementUsageCount ();
    }
}

void CTexture::update () const {
    if (this->m_player) {
	this->m_player->render ();
    }

    // wallpaper64.exe 2.8.42 sub_1400EE9E0: once per frame AdvanceGIF with the frame's duration, a new frame is
    // copied over the whole texture
    if (this->m_gif && this->m_gifTime != g_Time) {
	this->m_gifTime = g_Time;

	if (this->m_gif->advance (std::max (g_Time - g_TimeLast, 0.0f))) {
	    glBindTexture (GL_TEXTURE_2D, this->m_textureID[0]);
	    glTexSubImage2D (
		GL_TEXTURE_2D, 0, 0, 0, this->m_gif->width (), this->m_gif->height (), GL_RGBA, GL_UNSIGNED_BYTE,
		this->m_gif->pixels ()
	    );
	}
    }
}

bool CTexture::isReady () const { return true; }