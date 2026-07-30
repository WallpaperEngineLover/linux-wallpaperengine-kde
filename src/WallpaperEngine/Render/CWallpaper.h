#pragma once

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <optional>

#include "WallpaperEngine/Audio/AudioContext.h"

#include "WallpaperEngine/Render/CFBO.h"
#include "WallpaperEngine/Render/Helpers/ContextAware.h"
#include "WallpaperEngine/Render/RenderContext.h"

#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Media/MediaSource.h"

#include "FBOProvider.h"
#include "WallpaperState.h"

namespace WallpaperEngine::Application {
class WallpaperApplication;
}

namespace WallpaperEngine::WebBrowser {
class WebBrowserContext;
}

namespace WallpaperEngine::Render {
namespace Helpers {
    class ContextAware;
}

using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Audio;
using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::FileSystem;

class CWallpaper : public Helpers::ContextAware, public FBOProvider, public TypeCaster {
    friend class WallpaperEngine::Application::WallpaperApplication;

public:
    /** One wallpaper shared and rendered across multiple viewports */
    struct SpanInfo {
	/** Bounding box of the entire span group (x, y, width, height) in global desktop coordinates */
	glm::ivec4 totalBounds;
    };

    virtual ~CWallpaper () override;

    void render (
	const glm::ivec4& viewport, const bool vflip, const glm::ivec2& globalPosition = { 0, 0 },
	const glm::ivec2& logicalSize = { 0, 0 }
    );

    virtual void setPause (bool newState);

    [[nodiscard]] const AssetLocator& getAssetLocator () const;
    AudioContext& getAudioContext () const;
    [[nodiscard]] const WallpaperState& getState () const;

    /** Changes the scaling mode live, without reloading the wallpaper (used by the hotswap control file) */
    void setScalingMode (WallpaperState::TextureUVsScaling mode);

    /** Changes the manual zoom factor live, without reloading the wallpaper (used by the hotswap control file) */
    void setZoom (float zoom);

    /**
     * Changes the color shown outside the wallpaper's bounds (Center/Fit letterboxing, zoomed-out
     * scaling) live, without reloading the wallpaper. Only visible when the clamp mode is border
     * (the default) rather than clamp/repeat.
     */
    void setCornerColor (const glm::vec4& color);

    [[nodiscard]] virtual GLuint getWallpaperFramebuffer () const;
    [[nodiscard]] virtual GLuint getWallpaperTexture () const;
    [[nodiscard]] std::shared_ptr<const CFBO> findFBO (const std::string& name) const;
    [[nodiscard]] std::shared_ptr<const CFBO> getFBO () const;

    void updateUVs (const glm::ivec4& viewport, const bool vflip);
    void setDestinationFramebuffer (GLuint framebuffer);
    void setSpanInfo (const SpanInfo& spanInfo);
    [[nodiscard]] const SpanInfo* getSpanInfo () const;

    [[nodiscard]] virtual int getWidth () const = 0;
    [[nodiscard]] virtual int getHeight () const = 0;

    static std::unique_ptr<CWallpaper> fromWallpaper (
	const Wallpaper& wallpaper, RenderContext& context, AudioContext& audioContext,
	WebBrowser::WebBrowserContext* browserContext, const WallpaperState::TextureUVsScaling& scalingMode,
	const uint32_t& clampMode
    );

protected:
    CWallpaper (
	const Wallpaper& wallpaperData, RenderContext& context, AudioContext& audioContext,
	const WallpaperState::TextureUVsScaling& scalingMode, const uint32_t& clampMode
    );

    virtual void renderFrame (const glm::ivec4& viewport) = 0;

    void setupFramebuffers ();

    const Wallpaper& m_wallpaperData;

    [[nodiscard]] const Wallpaper& getWallpaperData () const;

    std::shared_ptr<const CFBO> m_sceneFBO = nullptr;

    GLuint m_vaoBuffer = GL_NONE;

private:
    GLuint m_texCoordBuffer = GL_NONE;
    GLuint m_positionBuffer = GL_NONE;
    GLuint m_shader = GL_NONE;
    GLint g_Texture0 = GL_NONE;
    GLint a_Position = GL_NONE;
    GLint a_TexCoord = GL_NONE;
    GLuint m_destFramebuffer = GL_NONE;
    void setupShaders ();
    std::map<std::string, std::shared_ptr<const CFBO>> m_fbos = {};
    AudioContext& m_audioContext;
    WallpaperState m_state;
    glm::vec4 m_cornerColor = { 0.0f, 0.0f, 0.0f, 1.0f };
    std::optional<SpanInfo> m_spanInfo = std::nullopt;
    // Avoids redundant renderFrame calls when the same wallpaper is shared across viewports (span mode)
    uint32_t m_lastRenderedFrame = UINT32_MAX;
};
} // namespace WallpaperEngine::Render
