#include "CFBO.h"
#include "WallpaperEngine/Logging/Log.h"

using namespace WallpaperEngine::Render;

namespace {
// 3-channel formats get an alpha channel, RGB targets aren't guaranteed to be renderable
GLint internalFormat (const TextureFormat format) {
    switch (format) {
	case TextureFormat_RG88: return GL_RG8;
	case TextureFormat_R8: return GL_R8;
	case TextureFormat_RG1616f: return GL_RG16F;
	case TextureFormat_R16f: return GL_R16F;
	case TextureFormat_RGBA16161616f:
	case TextureFormat_RGB161616f: return GL_RGBA16F;
	case TextureFormat_RGBa1010102: return GL_RGB10_A2;
	default: return GL_RGBA8;
    }
}
} // namespace

CFBO::CFBO (
    std::string name, const TextureFormat format, const uint32_t flags, const float scale, uint32_t realWidth,
    uint32_t realHeight, uint32_t textureWidth, uint32_t textureHeight, const glm::vec4& borderColor
) : m_scale (scale), m_name (std::move (name)), m_format (format), m_flags (flags) {
    constexpr GLenum drawBuffers[1] = { GL_COLOR_ATTACHMENT0 };
    glGenFramebuffers (1, &this->m_framebuffer);
    glBindFramebuffer (GL_FRAMEBUFFER, this->m_framebuffer);
    glGenTextures (1, &this->m_texture);
    glBindTexture (GL_TEXTURE_2D, this->m_texture);
    glTexImage2D (
	GL_TEXTURE_2D, 0, internalFormat (format), textureWidth, textureHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr
    );
#if !NDEBUG
    glObjectLabel (GL_TEXTURE, this->m_texture, -1, this->m_name.c_str ());
#endif /* DEBUG */
    if (flags & TextureFlags_ClampUVs) {
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else if (flags & TextureFlags_ClampUVsBorder) {
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	// without this the border defaults to transparent black, which GL_LINEAR filtering smears
	// into the last edge texel; this keeps out-of-bounds areas (letterboxing, zoomed-out scaling) solid
	glTexParameterfv (GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, &borderColor.x);
    } else {
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }

    if (flags & TextureFlags_NoInterpolation) {
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    } else {
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }

    glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, 8.0f);

    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, this->m_texture, 0);
    glDrawBuffers (1, drawBuffers);

    if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE && internalFormat (format) != GL_RGBA8) {
	sLog.error ("FBO ", this->m_name, " can't render to format ", format, ", falling back to RGBA8");
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, textureWidth, textureHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	this->m_format = TextureFormat_ARGB8888;
    }

    if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
	sLog.exception ("Framebuffers are not properly set");
    }

    // must start transparent: the scene clear color is often opaque and would make empty layer
    // areas render as solid rectangles
    GLfloat previousClearColor[4] = {};
    glGetFloatv (GL_COLOR_CLEAR_VALUE, previousClearColor);
    glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
    glClear (GL_COLOR_BUFFER_BIT);
    glClearColor (previousClearColor[0], previousClearColor[1], previousClearColor[2], previousClearColor[3]);

    this->m_resolution = { textureWidth, textureHeight, realWidth, realHeight };

    const auto frame = std::make_shared<Frame> ();

    frame->frameNumber = 0;
    frame->frametime = 0;
    frame->height1 = textureHeight;
    frame->height2 = realHeight;
    frame->width1 = textureWidth;
    frame->width2 = realWidth;
    frame->x = 0;
    frame->y = 0;

    this->m_frames.push_back (frame);
}

CFBO::~CFBO () {
    glDeleteTextures (1, &this->m_texture);
    glDeleteFramebuffers (1, &this->m_framebuffer);
}

const std::string& CFBO::getName () const { return this->m_name; }

const float& CFBO::getScale () const { return this->m_scale; }

TextureFormat CFBO::getFormat () const { return this->m_format; }

uint32_t CFBO::getFlags () const { return this->m_flags; }

GLuint CFBO::getFramebuffer () const { return this->m_framebuffer; }

GLuint CFBO::getDepthbuffer () const { return this->m_depthbuffer; }

GLuint CFBO::getTextureID (uint32_t imageIndex) const { return this->m_texture; }

uint32_t CFBO::getTextureWidth (uint32_t imageIndex) const { return this->m_resolution.x; }

uint32_t CFBO::getTextureHeight (uint32_t imageIndex) const { return this->m_resolution.y; }

uint32_t CFBO::getRealWidth () const { return this->m_resolution.z; }

uint32_t CFBO::getRealHeight () const { return this->m_resolution.w; }

const std::vector<FrameSharedPtr>& CFBO::getFrames () const { return this->m_frames; }

const glm::vec4* CFBO::getResolution () const { return &this->m_resolution; }

bool CFBO::isAnimated () const { return false; }

uint32_t CFBO::getSpritesheetCols () const {
    return 0; // FBOs don't have spritesheets
}

uint32_t CFBO::getSpritesheetRows () const {
    return 0; // FBOs don't have spritesheets
}

uint32_t CFBO::getSpritesheetFrames () const {
    return 0; // FBOs don't have spritesheets
}

float CFBO::getSpritesheetDuration () const {
    return 0.0f; // FBOs don't have spritesheets
}

void CFBO::incrementUsageCount () const { }
void CFBO::decrementUsageCount () const { }
void CFBO::update () const { }
bool CFBO::isReady () const { return true; }

std::optional<glm::vec4> CFBO::parseColor (const std::string& value) {
    std::string hex = value;

    if (!hex.empty () && hex[0] == '#') {
	hex = hex.substr (1);
    }

    if (hex.size () != 6 && hex.size () != 8) {
	return std::nullopt;
    }

    unsigned long parsed;

    try {
	std::size_t consumed = 0;
	parsed = std::stoul (hex, &consumed, 16);

	if (consumed != hex.size ()) {
	    return std::nullopt;
	}
    } catch (const std::exception&) {
	return std::nullopt;
    }

    const bool hasAlpha = hex.size () == 8;
    const float r = static_cast<float> ((parsed >> (hasAlpha ? 24 : 16)) & 0xFF) / 255.0f;
    const float g = static_cast<float> ((parsed >> (hasAlpha ? 16 : 8)) & 0xFF) / 255.0f;
    const float b = static_cast<float> ((parsed >> (hasAlpha ? 8 : 0)) & 0xFF) / 255.0f;
    const float a = hasAlpha ? static_cast<float> (parsed & 0xFF) / 255.0f : 1.0f;

    return glm::vec4 { r, g, b, a };
}

void CFBO::setBorderColor (const glm::vec4& color) const {
    glBindTexture (GL_TEXTURE_2D, this->m_texture);
    glTexParameterfv (GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, &color.x);
}