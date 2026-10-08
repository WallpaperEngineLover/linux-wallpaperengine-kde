#include "MissingTexture.h"

#include <array>

using namespace WallpaperEngine::Render;

MissingTexture::MissingTexture () {
    std::array<uint8_t, SIZE * SIZE * 4> pixels {};

    for (uint32_t i = 0; i < SIZE * SIZE; i++) {
	const uint8_t yellow = ((i / SIZE) & 1) != (i & 1) ? 255 : 0;

	pixels[i * 4 + 0] = yellow;
	pixels[i * 4 + 1] = yellow;
	pixels[i * 4 + 2] = 0;
	pixels[i * 4 + 3] = 255;
    }

    glGenTextures (1, &this->m_textureID);
    glBindTexture (GL_TEXTURE_2D, this->m_textureID);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, SIZE, SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data ());
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
}

MissingTexture::~MissingTexture () { glDeleteTextures (1, &this->m_textureID); }

GLuint MissingTexture::getTextureID (uint32_t imageIndex) const { return this->m_textureID; }
uint32_t MissingTexture::getTextureWidth (uint32_t imageIndex) const { return SIZE; }
uint32_t MissingTexture::getTextureHeight (uint32_t imageIndex) const { return SIZE; }
uint32_t MissingTexture::getRealWidth () const { return SIZE; }
uint32_t MissingTexture::getRealHeight () const { return SIZE; }
TextureFormat MissingTexture::getFormat () const { return TextureFormat_ARGB8888; }
uint32_t MissingTexture::getFlags () const { return TextureFlags_NoInterpolation; }
const std::vector<FrameSharedPtr>& MissingTexture::getFrames () const { return this->m_frames; }
const glm::vec4* MissingTexture::getResolution () const { return &this->m_resolution; }
bool MissingTexture::isAnimated () const { return false; }
bool MissingTexture::isReady () const { return true; }

void MissingTexture::incrementUsageCount () const { }
void MissingTexture::decrementUsageCount () const { }
void MissingTexture::update () const { }
