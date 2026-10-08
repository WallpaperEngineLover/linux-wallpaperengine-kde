#pragma once

#include "TextureProvider.h"

namespace WallpaperEngine::Render {
/** WE's texture for anything it can't load (sub_1400EEF70): 6x6 yellow/black checker, point sampled */
class MissingTexture final : public TextureProvider {
public:
    MissingTexture ();
    ~MissingTexture () override;

    [[nodiscard]] GLuint getTextureID (uint32_t imageIndex) const override;
    [[nodiscard]] uint32_t getTextureWidth (uint32_t imageIndex) const override;
    [[nodiscard]] uint32_t getTextureHeight (uint32_t imageIndex) const override;
    [[nodiscard]] uint32_t getRealWidth () const override;
    [[nodiscard]] uint32_t getRealHeight () const override;
    [[nodiscard]] TextureFormat getFormat () const override;
    [[nodiscard]] uint32_t getFlags () const override;
    [[nodiscard]] const std::vector<FrameSharedPtr>& getFrames () const override;
    [[nodiscard]] const glm::vec4* getResolution () const override;
    [[nodiscard]] bool isAnimated () const override;
    [[nodiscard]] bool isReady () const override;

    void incrementUsageCount () const override;
    void decrementUsageCount () const override;
    void update () const override;

private:
    static constexpr uint32_t SIZE = 6;

    std::vector<FrameSharedPtr> m_frames;
    glm::vec4 m_resolution { SIZE, SIZE, SIZE, SIZE };
    GLuint m_textureID = GL_NONE;
};
} // namespace WallpaperEngine::Render
