#pragma once

#include <optional>
#include <string>

#include "TextureProvider.h"

using namespace WallpaperEngine::Render;

namespace WallpaperEngine::Render {
class CFBO final : public TextureProvider {
public:
    CFBO (
	std::string name, const TextureFormat format, const uint32_t flags, const float scale, uint32_t realWidth,
	uint32_t realHeight, uint32_t textureWidth, uint32_t textureHeight,
	const glm::vec4& borderColor = { 0.0f, 0.0f, 0.0f, 1.0f }
    );
    ~CFBO () override;

    /**
     * Parses a "RRGGBB" or "RRGGBBAA" hex color (an optional leading '#' is stripped), or
     * std::nullopt if value isn't a valid hex color. Used for --corner-color and its hotswap key.
     */
    static std::optional<glm::vec4> parseColor (const std::string& value);

    /**
     * Updates the border color shown outside the wallpaper's bounds when its wrap mode is
     * GL_CLAMP_TO_BORDER (see TextureFlags_ClampUVsBorder). Safe to call live, no reload needed.
     */
    void setBorderColor (const glm::vec4& color) const;

    /** Adds a depth buffer, only 3D scenes draw into the scene buffer with depth testing */
    void attachDepthBuffer ();

    [[nodiscard]] const std::string& getName () const;
    [[nodiscard]] const float& getScale () const;
    [[nodiscard]] TextureFormat getFormat () const override;
    [[nodiscard]] uint32_t getFlags () const override;
    [[nodiscard]] GLuint getFramebuffer () const;
    [[nodiscard]] GLuint getDepthbuffer () const;
    [[nodiscard]] GLuint getTextureID (uint32_t imageIndex) const override;
    [[nodiscard]] uint32_t getTextureWidth (uint32_t imageIndex) const override;
    [[nodiscard]] uint32_t getTextureHeight (uint32_t imageIndex) const override;
    [[nodiscard]] uint32_t getRealWidth () const override;
    [[nodiscard]] uint32_t getRealHeight () const override;
    [[nodiscard]] const std::vector<FrameSharedPtr>& getFrames () const override;
    [[nodiscard]] const glm::vec4* getResolution () const override;
    [[nodiscard]] bool isAnimated () const override;
    [[nodiscard]] uint32_t getSpritesheetCols () const override;
    [[nodiscard]] uint32_t getSpritesheetRows () const override;
    [[nodiscard]] uint32_t getSpritesheetFrames () const override;
    [[nodiscard]] float getSpritesheetDuration () const override;

    void incrementUsageCount () const override;
    void decrementUsageCount () const override;
    void update () const override;
    bool isReady () const override;

private:
    GLuint m_framebuffer = GL_NONE;
    GLuint m_depthbuffer = GL_NONE;
    GLuint m_texture = GL_NONE;
    glm::vec4 m_resolution = {};
    float m_scale = 0;
    std::string m_name = "";
    TextureFormat m_format = TextureFormat_UNKNOWN;
    uint32_t m_flags = TextureFlags_NoFlags;
    /** Placeholder for frames, FBOs only have ONE */
    std::vector<FrameSharedPtr> m_frames = {};
};
} // namespace WallpaperEngine::Render
