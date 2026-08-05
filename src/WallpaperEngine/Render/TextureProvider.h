#pragma once

#include <memory>
#include <vector>

#include <GL/glew.h>
#include <glm/vec4.hpp>

#include "WallpaperEngine/Data/Assets/Texture.h"
#include "WallpaperEngine/Data/Model/Types.h"

namespace WallpaperEngine::Render {
using namespace WallpaperEngine::Data::Assets;
/**
 * Base interface that describes the minimum information required for the engine
 * to display a texture
 */
class TextureProvider {
public:
    virtual ~TextureProvider () = default;

    [[nodiscard]] virtual GLuint getTextureID (uint32_t imageIndex) const = 0;
    [[nodiscard]] virtual uint32_t getTextureWidth (uint32_t imageIndex) const = 0;
    [[nodiscard]] virtual uint32_t getTextureHeight (uint32_t imageIndex) const = 0;
    [[nodiscard]] virtual uint32_t getRealWidth () const = 0;
    [[nodiscard]] virtual uint32_t getRealHeight () const = 0;
    [[nodiscard]] virtual TextureFormat getFormat () const = 0;
    [[nodiscard]] virtual uint32_t getFlags () const = 0;
    [[nodiscard]] virtual const std::vector<FrameSharedPtr>& getFrames () const = 0;
    [[nodiscard]] virtual const glm::vec4* getResolution () const = 0;
    [[nodiscard]] virtual bool isAnimated () const = 0;
    /** 0 if not a spritesheet */
    [[nodiscard]] virtual uint32_t getSpritesheetCols () const = 0;
    /** 0 if not a spritesheet */
    [[nodiscard]] virtual uint32_t getSpritesheetRows () const = 0;
    /** 0 if not a spritesheet */
    [[nodiscard]] virtual uint32_t getSpritesheetFrames () const = 0;
    [[nodiscard]] virtual float getSpritesheetDuration () const = 0;
    virtual bool isReady () const = 0;

    /** For video CTextures, playback only starts once usage count goes above zero (initializes mpv if needed) */
    virtual void incrementUsageCount () const = 0;
    /** For video CTextures, playback only stops once usage count reaches zero (de-initializes mpv if needed) */
    virtual void decrementUsageCount () const = 0;
    virtual void update () const = 0;
};
} // namespace WallpaperEngine::Render