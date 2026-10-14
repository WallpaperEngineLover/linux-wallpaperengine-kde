#pragma once

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <filesystem>
#include <limits>
#include <optional>

#include "WallpaperEngine/Audio/AudioContext.h"
#include "WallpaperEngine/Input/MouseInput.h"

#include "WallpaperEngine/Render/CFBO.h"
#include "WallpaperEngine/Render/Helpers/ContextAware.h"
#include "WallpaperEngine/Render/RenderContext.h"

#include "WallpaperEngine/Data/Model/ImageAdjustments.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Media/MediaSource.h"

#include "FBOProvider.h"
#include "WallpaperState.h"

namespace WallpaperEngine::Application {
class WallpaperApplication;
}

namespace WallpaperEngine::Render {
class FrameMaterial;

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
	/** The group's screens, the bounds follow their current layout */
	std::vector<std::string> screens;
	/** Bounding box of the group's outputs (x, y, width, height) in global desktop coordinates, gaps included */
	glm::ivec4 totalBounds;
	/** Buffer pixels per desktop unit the group renders at, the highest of its outputs */
	double scale = 1.0;
    };

    virtual ~CWallpaper () override;

    void render (
	const glm::ivec4& viewport, const bool vflip, const glm::ivec2& globalPosition = { 0, 0 },
	const glm::ivec2& logicalSize = { 0, 0 }
    );

    /** Draws the last frame again */
    void present (
	const glm::ivec4& viewport, const glm::ivec2& globalPosition = { 0, 0 },
	const glm::ivec2& logicalSize = { 0, 0 }
    );

    /** False until a web page painted, the previous wallpaper stays on screen meanwhile */
    [[nodiscard]] virtual bool hasContent () const { return true; }

    virtual void setPause (bool newState);

    // --audio-screen/--ambient-volume: muted off the audio screen, ambientVolume replaces --volume for non-video
    // content; no-op by default
    virtual void setAudioPolicy (bool muted, std::optional<int> ambientVolume);

    [[nodiscard]] const AssetLocator& getAssetLocator () const;
    AudioContext& getAudioContext () const;
    [[nodiscard]] const WallpaperState& getState () const;

    // live, for the hotswap control file
    void setScalingMode (WallpaperState::TextureUVsScaling mode);

    // live, for the hotswap control file
    void setZoom (float zoom);

    // live, see WallpaperState::setOffset
    void setOffset (float offsetX, float offsetY);
    [[nodiscard]] const glm::vec4& getCornerColor () const { return this->m_cornerColor; }
    /** WE's alignment position, x, y and zoom live (see WallpaperState::setAlignment) */
    void setAlignment (const WallpaperState::Alignment& alignment);

    // color outside the wallpaper's bounds (letterboxing, zoomed out), live; only visible when clamp is border
    void setCornerColor (const glm::vec4& color);

    // WE's image filter, color options and flip, live like its ccsimple.json pass; unset fields take WE's defaults
    void setImageAdjustments (const ImageAdjustments& adjustments);
    [[nodiscard]] bool isFlippedHorizontally () const { return this->m_flipHorizontal; }
    [[nodiscard]] bool hasImageAdjustments () const;
    /** The viewport about to be drawn is a PQ surface (--hdr on an output in HDR mode) */
    void setOutputHDR (bool hdr, const glm::vec2& luminance = glm::vec2 (0.0f)) {
	this->m_outputHDR = hdr;
	this->m_outputLuminance = luminance;
    }
    [[nodiscard]] bool isOutputHDR () const { return this->m_outputHDR; }
    /** The output's reference white and peak in nits while it runs in HDR, zero when unknown */
    [[nodiscard]] const glm::vec2& getOutputLuminance () const { return this->m_outputLuminance; }
    /**
     * The last frame with the image adjustments applied, same size and orientation as the wallpaper's own
     * framebuffer (what screenshots read). Just the wallpaper's framebuffer when nothing is adjusted
     */
    [[nodiscard]] GLuint renderAdjustedFramebuffer ();

    [[nodiscard]] virtual GLuint getWallpaperFramebuffer () const;
    [[nodiscard]] virtual GLuint getWallpaperTexture () const;
    [[nodiscard]] std::shared_ptr<const CFBO> findFBO (const std::string& name) const;
    [[nodiscard]] std::shared_ptr<const CFBO> getFBO () const;

    void updateUVs (const glm::ivec4& viewport, const bool vflip);
    void setDestinationFramebuffer (GLuint framebuffer);
    void setSpanInfo (const SpanInfo& spanInfo);
    [[nodiscard]] const SpanInfo* getSpanInfo () const;
    /** Like MouseInput::position, but over the whole group for a span */
    [[nodiscard]] glm::dvec2 cursorPosition () const;
    [[nodiscard]] Input::MouseClickStatus cursorLeftClick () const;

    [[nodiscard]] virtual int getWidth () const = 0;
    [[nodiscard]] virtual int getHeight () const = 0;
    /** Size of the framebuffer the wallpaper renders into, only a scene can make this bigger than getWidth/getHeight */
    [[nodiscard]] virtual int getCanvasWidth () const { return this->getWidth (); }
    [[nodiscard]] virtual int getCanvasHeight () const { return this->getHeight (); }
    /** Scenes render at the output's size with the scaling mode folded into their projection, like WE does */
    [[nodiscard]] virtual bool rendersAtOutputSize () const { return false; }
    /** Whether the wallpaper uses pointer buttons, otherwise clicks can pass through to the desktop below */
    [[nodiscard]] virtual bool wantsPointerButtons () const { return false; }
    /** The part of the framebuffer the output shows as ustart, uend, vstart, vend */
    [[nodiscard]] glm::vec4 getOutputUVs () const;
    [[nodiscard]] int getFramebufferWidth () const;
    [[nodiscard]] int getFramebufferHeight () const;
    /** Size of the output the wallpaper was last rendered to, zero until the first frame */
    [[nodiscard]] const glm::ivec2& getScreenSize () const { return this->m_screenSize; }
    /** Screen key for script localStorage's screen store, empty in a window */
    void setStorageScreen (std::string screen) { this->m_storageScreen = std::move (screen); }
    [[nodiscard]] const std::string& getStorageScreen () const { return this->m_storageScreen; }
    [[nodiscard]] const std::filesystem::path& getSceneFile () const;
    /** This frame's --ssaa factor per axis */
    [[nodiscard]] int getSupersampling () const { return this->m_supersampling; }

    // maxRenderSize: the largest size a Web wallpaper renders at (a span's bounding box), ignored otherwise
    static std::unique_ptr<CWallpaper> fromWallpaper (
	const Wallpaper& wallpaper, RenderContext& context, AudioContext& audioContext,
	const std::filesystem::path& resolvedBackgroundPath, const WallpaperState::TextureUVsScaling& scalingMode,
	const uint32_t& clampMode, const glm::ivec2& maxRenderSize
    );

protected:
    CWallpaper (
	const Wallpaper& wallpaperData, RenderContext& context, AudioContext& audioContext,
	const WallpaperState::TextureUVsScaling& scalingMode, const uint32_t& clampMode
    );

    virtual void renderFrame (const glm::ivec4& viewport) = 0;

    void setupFramebuffers (bool depth = false, TextureFormat format = TextureFormat_ARGB8888);
    /** The framebuffer holds linear light with BT.2020 primaries, 1.0 being reference white (HDR video) */
    void setLinearInput (bool linear) { this->m_linearInput = linear; }
    /** FP16 scRGB like WE's HDR swapchain, clipped and sRGB encoded on SDR outputs */
    void setScRGBInput (bool scRGB) { this->m_scRGBInput = scRGB; }
    /** Draws the image adjustments itself with ccsimple.json */
    [[nodiscard]] virtual bool drawsImageAdjustments () const { return false; }
    /** RGBA8 or RGBA16F like WE's swapchain (sub_14012AC60) */
    void prepareBackBuffer (const glm::ivec2& size, GLenum format);
    /** ccsimple.json color options and image filter (sub_140181F30) */
    void renderImageAdjustments (const glm::ivec2& size, GLenum format);

    struct BackBuffer {
	GLuint texture = GL_NONE;
	GLuint framebuffer = GL_NONE;
	glm::ivec2 size {};
	GLenum format = GL_NONE;
    };
    BackBuffer m_backBuffer;
    bool m_presentBackBuffer = false;
    /** WE's COL combo (sub_140181F30) */
    [[nodiscard]] bool isColorEnabled () const { return this->m_colorEnabled; }
    /** brightness, contrast, saturation, hue */
    [[nodiscard]] const glm::vec4& getColorParams () const { return this->m_colorParams; }
    [[nodiscard]] GLuint getLutTexture () const { return this->m_lutTexture; }
    /** wcc_amt / 100 */
    [[nodiscard]] float getLutStrength () const { return this->m_lutStrength; }

    const Wallpaper& m_wallpaperData;

    [[nodiscard]] const Wallpaper& getWallpaperData () const;

    std::shared_ptr<const CFBO> m_sceneFBO = nullptr;

    GLuint m_vaoBuffer = GL_NONE;

private:
    GLuint m_texCoordBuffer = GL_NONE;
    GLuint m_positionBuffer = GL_NONE;
    GLuint m_shader = GL_NONE;
    GLint g_Texture0 = GL_NONE;
    GLint g_Texture1 = GL_NONE;
    GLint g_Params = GL_NONE;
    GLint g_LutParams = GL_NONE;
    GLint u_ColorEnabled = GL_NONE;
    GLint u_LutEnabled = GL_NONE;
    GLint a_Position = GL_NONE;
    GLint a_TexCoord = GL_NONE;
    GLuint m_destFramebuffer = GL_NONE;
    void setupShaders ();
    // UVs last uploaded, NaN so the first render() uploads
    float m_uploadedUstart = std::numeric_limits<float>::quiet_NaN ();
    float m_uploadedUend = std::numeric_limits<float>::quiet_NaN ();
    float m_uploadedVstart = std::numeric_limits<float>::quiet_NaN ();
    float m_uploadedVend = std::numeric_limits<float>::quiet_NaN ();
    std::map<std::string, std::shared_ptr<const CFBO>> m_fbos = {};
    AudioContext& m_audioContext;
    WallpaperState m_state;
    glm::vec4 m_cornerColor = { 0.0f, 0.0f, 0.0f, 1.0f };
    /** ccsimple's g_Params: brightness, contrast and saturation multipliers and the hue shift */
    glm::vec4 m_colorParams = { 1.0f, 1.0f, 1.0f, 0.0f };
    bool m_colorEnabled = false;
    float m_lutStrength = 0.0f;
    std::string m_lutName;
    GLuint m_lutTexture = GL_NONE;
    std::unique_ptr<FrameMaterial> m_imageAdjustmentsMaterial;
    std::pair<bool, bool> m_imageAdjustmentsCombos { false, false };
    /** --clamp and --corner-color, only for drawing the frame to the output */
    GLuint m_outputSampler = GL_NONE;
    std::unique_ptr<CFBO> m_adjustedFBO;
    bool m_flipHorizontal = false;
    bool m_outputHDR = false;
    glm::vec2 m_outputLuminance = glm::vec2 (0.0f);
    bool m_linearInput = false;
    bool m_scRGBInput = false;
    GLint u_InputLinear = GL_NONE;
    GLint u_InputScRGB = GL_NONE;
    GLint u_OutputPQ = GL_NONE;
    GLint u_Supersample = GL_NONE;
    void loadLut (const std::string& name);
    /** Draws the texture with the image adjustments, box filtering supersample texels per axis */
    void drawOutputQuad (int supersample);
    /** --ssaa factor reduced to fit the GL size limits */
    [[nodiscard]] int supersamplingFor (const glm::ivec2& outputSize);
    int m_supersampling = 1;
    int m_supersamplingLogged = 0;
    std::optional<SpanInfo> m_spanInfo = std::nullopt;
    /** the cursor in the group's buffer pixels, y up */
    glm::dvec2 m_spanCursor = { 0, 0 };
    void updateSpanBounds ();
    void updateSpanCursor (const glm::ivec4& viewport, const glm::ivec2& globalPosition, const glm::ivec2& logicalSize);
    // renders once per frame when shared across span viewports
    uint32_t m_lastRenderedFrame = UINT32_MAX;
    glm::ivec2 m_screenSize = { 0, 0 };
    std::string m_storageScreen;
};
} // namespace WallpaperEngine::Render
