#include "CTexture.h"
#include "WallpaperEngine/Logging/Log.h"

#include <lz4.h>

#define STB_IMAGE_IMPLEMENTATION
#include "RenderContext.h"

#include <stb_image.h>

using namespace WallpaperEngine::Render;

CTexture::CTexture (RenderContext& context, TextureUniquePtr header) :
    Helpers::ContextAware (context), m_header (std::move (header)) {
    this->setupResolution ();
    const GLint internalFormat = this->setupInternalFormat ();

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
	this->m_player->setUntimed ();
	return;
    }

    this->m_textureID = new GLuint[this->m_header->imageCount];
    glGenTextures (this->m_header->imageCount, this->m_textureID);

    for (const auto& [index, mipmaps] : this->m_header->images) {
	this->setupOpenGLParameters (index);

	int level = 0;

	for (const auto& mipmap : mipmaps) {
	    stbi_uc* handle = nullptr;
	    const void* dataptr = mipmap->uncompressedData.get ();
	    int width = mipmap->width;
	    int height = mipmap->height;
	    const uint32_t bufferSize = mipmap->uncompressedSize;
	    GLenum textureFormat = GL_RGBA;

	    if (this->m_header->freeImageFormat != FIF_UNKNOWN) {
		int fileChannels;

		dataptr = handle = stbi_load_from_memory (
		    reinterpret_cast<unsigned char*> (mipmap->uncompressedData.get ()), mipmap->uncompressedSize,
		    &width, &height, &fileChannels, 4
		);
	    } else {
		if (this->m_header->format == TextureFormat_R8) {
		    // 1 byte per pixel, so alignment must be set manually
		    glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
		    textureFormat = GL_RED;
		} else if (this->m_header->format == TextureFormat_RG88) {
		    // 2 bytes per pixel, so odd widths are not 4-byte aligned
		    glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
		    textureFormat = GL_RG;
		}
	    }

	    switch (internalFormat) {
		case GL_RGBA8:
		case GL_RG8:
		case GL_R8:
		    glTexImage2D (
			GL_TEXTURE_2D, level, internalFormat, width, height, 0, textureFormat, GL_UNSIGNED_BYTE, dataptr
		    );
		    break;
		case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
		case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
		case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
		    glCompressedTexImage2D (
			GL_TEXTURE_2D, level, internalFormat, width, height, 0, bufferSize, dataptr
		    );
		    break;
		default:
		    sLog.exception ("Cannot load texture, unknown format", this->m_header->format);
	    }

	    if (this->m_header->freeImageFormat != FIF_UNKNOWN) {
		stbi_image_free (handle);
	    }

	    level++;
	}
    }
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
    if (this->isAnimated ()) {
	this->m_resolution = { this->m_header->textureWidth, this->m_header->textureHeight, this->m_header->gifWidth,
			       this->m_header->gifHeight };
    } else {
	if (this->m_header->freeImageFormat != FIF_UNKNOWN) {
	    // wpengine-texture format always has one mipmap
	    const auto element = this->m_header->images.find (0)->second.begin ();

	    this->m_resolution
		= { (*element)->width, (*element)->height, this->m_header->width, this->m_header->height };
	} else {
	    this->m_resolution = { this->m_header->textureWidth, this->m_header->textureHeight, this->m_header->width,
				   this->m_header->height };
	}
    }
}

GLint CTexture::setupInternalFormat () const {
    if (this->m_header->freeImageFormat != FIF_UNKNOWN) {
	return GL_RGBA8;
    }

    switch (this->m_header->format) {
	case TextureFormat_DXT5:
	    return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
	case TextureFormat_DXT3:
	    return GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
	case TextureFormat_DXT1:
	    return GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
	case TextureFormat_ARGB8888:
	    return GL_RGBA8;
	case TextureFormat_R8:
	    return GL_R8;
	case TextureFormat_RG88:
	    return GL_RG8;
	default:
	    sLog.exception ("Cannot determine texture format");
    }
}

void CTexture::setupOpenGLParameters (const uint32_t textureID) const {
    // TODO: label elements too
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

uint32_t CTexture::getSpritesheetCols () const { return this->getHeader ().spritesheetCols; }

uint32_t CTexture::getSpritesheetRows () const { return this->getHeader ().spritesheetRows; }

uint32_t CTexture::getSpritesheetFrames () const { return this->getHeader ().spritesheetFrames; }

float CTexture::getSpritesheetDuration () const { return this->getHeader ().spritesheetDuration; }

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
}

bool CTexture::isReady () const { return true; }