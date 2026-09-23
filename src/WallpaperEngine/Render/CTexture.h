#pragma once

#include "Helpers/ContextAware.h"
#include "TextureProvider.h"
#include "WallpaperEngine/Data/Assets/Texture.h"
#include "WallpaperEngine/VideoPlayback/MPV/GLPlayer.h"

#include <GL/glew.h>
#include <glm/vec4.hpp>
#include <memory>
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>
#include <vector>

namespace WallpaperEngine::Render {
class RenderContext;
using namespace WallpaperEngine::Data::Assets;
using namespace WallpaperEngine::VideoPlayback::MPV;
/**
 * A normal texture file in WallpaperEngine's format
 */
class CTexture final : public TextureProvider, public Helpers::ContextAware {
public:
    explicit CTexture (RenderContext& context, TextureUniquePtr header);
    ~CTexture () override;

    [[nodiscard]] GLuint getTextureID (uint32_t imageIndex) const override;
    [[nodiscard]] uint32_t getTextureWidth (uint32_t imageIndex) const override;
    [[nodiscard]] uint32_t getTextureHeight (uint32_t imageIndex) const override;
    [[nodiscard]] uint32_t getRealWidth () const override;
    [[nodiscard]] uint32_t getRealHeight () const override;
    [[nodiscard]] TextureFormat getFormat () const override;
    [[nodiscard]] uint32_t getFlags () const override;
    [[nodiscard]] const glm::vec4* getResolution () const override;
    [[nodiscard]] const std::vector<FrameSharedPtr>& getFrames () const override;
    [[nodiscard]] bool isAnimated () const override;
    [[nodiscard]] uint32_t getSpritesheetCols () const override;
    [[nodiscard]] uint32_t getSpritesheetRows () const override;
    [[nodiscard]] uint32_t getSpritesheetFrames () const override;
    [[nodiscard]] float getSpritesheetDuration () const override;

    /** For video CTextures, playback only starts once usage count goes above zero (initializes mpv if needed) */
    void incrementUsageCount () const override;
    /** For video CTextures, playback only stops once usage count reaches zero (de-initializes mpv if needed) */
    void decrementUsageCount () const override;
    void update () const override;
    bool isReady () const override;
    [[nodiscard]] GLPlayer* getPlayer () const override { return this->m_player.get (); }

private:
    [[nodiscard]] const Texture& getHeader () const;

    void setupResolution ();
    GLint setupInternalFormat () const;
    void setupOpenGLParameters (uint32_t textureID) const;

    TextureUniquePtr m_header;
    GLuint* m_textureID = nullptr;
    glm::vec4 m_resolution {};
    GLPlayerUniquePtr m_player;
};
} // namespace WallpaperEngine::Assets