#include "AlbumTexture.h"

#include "RenderContext.h"
#include "WallpaperEngine/Assets/AssetLoadException.h"
#include "WallpaperEngine/Media/CoverImage.h"

#include <iterator>
#include <span>

using namespace WallpaperEngine::Render;

AlbumTexture::AlbumTexture (RenderContext& context) : Helpers::ContextAware (context) {
    this->m_resolution = glm::vec4 (1.0f, 1.0f, 1.0f, 1.0f);

    glGenTextures (1, &this->m_textureID);
    glBindTexture (GL_TEXTURE_2D, this->m_textureID);

    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, 8.0f);
}

AlbumTexture::~AlbumTexture () { glDeleteTextures (1, &this->m_textureID); }

GLuint AlbumTexture::getTextureID (uint32_t imageIndex) const { return this->m_textureID; }
uint32_t AlbumTexture::getTextureWidth (uint32_t imageIndex) const { return this->m_width; }
uint32_t AlbumTexture::getTextureHeight (uint32_t imageIndex) const { return this->m_height; }
uint32_t AlbumTexture::getRealWidth () const { return this->m_width; }
uint32_t AlbumTexture::getRealHeight () const { return this->m_height; }
TextureFormat AlbumTexture::getFormat () const { return TextureFormat_ARGB8888; }
uint32_t AlbumTexture::getFlags () const { return TextureFlags_NoFlags; }
const std::vector<FrameSharedPtr>& AlbumTexture::getFrames () const { return this->m_frames; }
const glm::vec4* AlbumTexture::getResolution () const { return &this->m_resolution; }
bool AlbumTexture::isAnimated () const { return false; }

void AlbumTexture::incrementUsageCount () const { }
void AlbumTexture::decrementUsageCount () const { }
void AlbumTexture::update () const { }

void AlbumTexture::copyContents (const TextureProvider& other) const noexcept {
    size_t bufferSize = static_cast<size_t> (other.getTextureWidth (0)) * other.getTextureHeight (0) * 4;

    uint8_t* buffer = new uint8_t[bufferSize];

    glBindTexture (GL_TEXTURE_2D, other.getTextureID (0));
    glGetTexImage (GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, buffer);

    glBindTexture (GL_TEXTURE_2D, this->m_textureID);
    glTexImage2D (
	GL_TEXTURE_2D, 0, GL_RGBA8, other.getTextureWidth (0), other.getTextureHeight (0), 0, GL_RGBA, GL_UNSIGNED_BYTE,
	buffer
    );

    delete[] buffer;

    this->m_width = other.getTextureWidth (0);
    this->m_height = other.getTextureHeight (0);
    this->m_resolution = *other.getResolution ();
}

void AlbumTexture::load () const {
    this->m_width = 0;
    this->m_height = 0;

    for (const auto& project : this->getContext ().getApp ().getBackgrounds () | std::views::values) {
	try {
	    auto contents = project->assetLocator->read ("$mediaThumbnail");
	    const std::string data ((std::istreambuf_iterator<char> (*contents)), std::istreambuf_iterator<char> ());
	    const auto image
		= Media::decodeCoverImage (std::span (reinterpret_cast<const uint8_t*> (data.data ()), data.size ()));

	    if (!image.has_value ()) {
		continue;
	    }

	    this->m_width = image->width;
	    this->m_height = image->height;
	    this->m_resolution = glm::vec4 (this->m_width, this->m_height, this->m_width, this->m_height);

	    uploadCoverTexture (this->m_textureID, *image);
	    return;
	} catch (AssetLoadException&) {
	    continue;
	}
    }
}

void WallpaperEngine::Render::uploadCoverTexture (GLuint texture, const Media::CoverImage& image) {
    // a bound PBO turns the pointer into an offset, row settings shift the rows
    GLint state[6];
    glGetIntegerv (GL_PIXEL_UNPACK_BUFFER_BINDING, &state[0]);
    glGetIntegerv (GL_UNPACK_ALIGNMENT, &state[1]);
    glGetIntegerv (GL_UNPACK_ROW_LENGTH, &state[2]);
    glGetIntegerv (GL_UNPACK_SKIP_ROWS, &state[3]);
    glGetIntegerv (GL_UNPACK_SKIP_PIXELS, &state[4]);
    glGetIntegerv (GL_TEXTURE_BINDING_2D, &state[5]);

    glBindBuffer (GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei (GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei (GL_UNPACK_SKIP_PIXELS, 0);

    glBindTexture (GL_TEXTURE_2D, texture);
    glTexImage2D (
	GL_TEXTURE_2D, 0, GL_RGBA, image.width, image.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data ()
    );

    glBindBuffer (GL_PIXEL_UNPACK_BUFFER, state[0]);
    glPixelStorei (GL_UNPACK_ALIGNMENT, state[1]);
    glPixelStorei (GL_UNPACK_ROW_LENGTH, state[2]);
    glPixelStorei (GL_UNPACK_SKIP_ROWS, state[3]);
    glPixelStorei (GL_UNPACK_SKIP_PIXELS, state[4]);
    glBindTexture (GL_TEXTURE_2D, state[5]);
}

bool AlbumTexture::isReady () const { return this->m_width > 0 && this->m_height > 0; }