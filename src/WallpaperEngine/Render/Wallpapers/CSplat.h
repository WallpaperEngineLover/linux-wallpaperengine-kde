#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "WallpaperEngine/Render/CWallpaper.h"

namespace WallpaperEngine::Render::Wallpapers {
class SplatSorter;

// native renderer for "SOG depth wallpaper" items: a PlayCanvas SOG Gaussian splat cloud seen from the photo's origin,
// swaying with the mouse; their SuperSplat CEF viewer tore under offscreen rendering, the camera model is ported from
// the wallpaper's JS
class CSplat : public CWallpaper {
public:
    CSplat (
	const Wallpaper& wallpaper, RenderContext& context, AudioContext& audioContext,
	const WallpaperState::TextureUVsScaling& scalingMode, const uint32_t& clampMode
    );
    ~CSplat () override;

    static bool supports (const Project& project);

    [[nodiscard]] int getWidth () const override { return this->m_width; }
    [[nodiscard]] int getHeight () const override { return this->m_height; }
    [[nodiscard]] float getAspectRatio () const override { return 0.0f; }

protected:
    void renderFrame (const glm::ivec4& viewport) override;

    friend class CWallpaper;

private:
    // what the camera needs from the cloud, in the viewer's frame (rotated 180 degrees around z, see loadCloud())
    struct SceneProfile {
	glm::vec3 direction = { 0.0f, 0.0f, 1.0f };
	float depthMin = 1.0f;
	float depthMax = 2.0f;
	glm::vec2 angularSize = { 1.0f, 1.0f };
	glm::vec3 boundsCenter = {};
	glm::vec3 boundsHalfExtents = {};
    };

    struct BaseCamera {
	glm::vec3 target = {};
	glm::vec3 position = {};
	glm::vec3 front = {};
	glm::vec3 right = {};
	glm::vec3 up = {};
	float distance = 1.0f;
	float parallaxDepthFactor = 0.0f;
	float frontZ = 0.0f;
	float fov = 60.0f;
	glm::ivec2 viewport = {};
	float focusDepth = 0.0f;
	float fovProperty = 60.0f;
    };

    struct Pose {
	glm::vec3 position = {};
	glm::vec3 target = {};
    };

    struct ClockPoint {
	float x = 0.0f;
	float y = 0.0f;
	// > 0 horizontal capsule, < 0 vertical, 0 a round dot; in aspect-corrected screen height units
	float halfLength = 0.0f;
	// 1 fully formed, ramping up from 0 while appearing, negative while sinking away
	float strength = 0.0f;
    };

    static constexpr size_t CLOCK_POINT_CAPACITY = 32;
    static constexpr int RIPPLE_SIZE = 96;

    struct TrailPoint {
	float x = 0.0f;
	float y = 0.0f;
	std::chrono::steady_clock::time_point time = {};
	float velocityX = 0.0f;
	float velocityY = 0.0f;
    };

    void loadCloud ();
    void
    setupGL (const std::vector<float>& centers, const std::vector<float>& rotations, const std::vector<float>& scales);
    void resizeOutput (int width, int height);

    std::vector<ClockPoint>
    buildClockPoints (const std::string& text, float aspect, float size, float originX, float originY) const;
    bool updateClock (float aspect);

    void resetRipple ();
    void addRippleImpulse (const TrailPoint& point, const TrailPoint& previous, float energy);
    void stepRipple ();
    void uploadRipple ();
    void trackMouseTrail (const glm::dvec2& normalized, std::chrono::steady_clock::time_point now);

    BaseCamera buildBaseCamera (int width, int height, float focusDepth) const;
    Pose posePlacement (const BaseCamera& base, glm::vec2 tilt, glm::vec2 orbit, float parallaxStrength) const;
    bool viewFitsFrontFace (const glm::vec3& position, const glm::vec3& target, const BaseCamera& base) const;

    [[nodiscard]] double numberProperty (const std::string& name, double fallback) const;
    [[nodiscard]] std::string stringProperty (const std::string& name) const;

    SceneProfile m_profile;
    BaseCamera m_base;
    bool m_hasBase = false;
    // presets are single photos: show all of it (bars on wide screens) instead of the base page's crop-to-fill
    bool m_containFraming = false;

    std::unique_ptr<SplatSorter> m_sorter;
    uint32_t m_cloudCount = 0;
    uint32_t m_textureWidth = 0;
    uint32_t m_drawCount = 0;

    GLuint m_program = GL_NONE;
    GLuint m_vao = GL_NONE;
    GLuint m_cornerBuffer = GL_NONE;
    GLuint m_orderBuffer = GL_NONE;
    GLuint m_centerTexture = GL_NONE;
    GLuint m_rotationTexture = GL_NONE;
    GLuint m_scaleTexture = GL_NONE;

    GLint u_Centers = -1;
    GLint u_Rotations = -1;
    GLint u_Scales = -1;
    GLint u_View = -1;
    GLint u_Projection = -1;
    GLint u_Viewport = -1;
    GLint u_Focal = -1;
    GLint u_TextureWidth = -1;
    GLint u_ClockPoints = -1;
    GLint u_ClockParams = -1;
    GLint u_ClockBounds = -1;
    GLint u_ClockDistance = -1;
    GLint u_Ripple = -1;
    GLint u_RippleParams = -1;
    GLuint m_rippleTexture = GL_NONE;

    int m_width = 16;
    int m_height = 17;

    std::string m_clockText;
    std::vector<ClockPoint> m_clockCurrent;
    std::vector<ClockPoint> m_clockPrevious;
    std::chrono::steady_clock::time_point m_clockTransitionStart = {};
    std::array<float, 5> m_clockLayout = {};
    std::array<glm::vec4, CLOCK_POINT_CAPACITY> m_clockPoints = {};
    glm::vec4 m_clockParams = {};
    glm::vec4 m_clockBounds = {};
    float m_clockDistance = 0.0f;

    std::array<std::vector<float>, 4> m_rippleForces;
    std::array<std::vector<float>, 4> m_rippleNext;
    std::vector<uint8_t> m_ripplePixels;
    float m_rippleEnergy = 0.0f;
    int m_ripplePhase = 0;
    // the shader only looks at the field while this is set (WE page: tianyinRippleParams.x)
    bool m_rippleActive = false;
    // one more frame is needed after the field went quiet, to draw it without
    bool m_rippleWasActive = false;
    std::optional<TrailPoint> m_trailLast;
    std::optional<glm::dvec2> m_trailCursor;

    glm::vec2 m_tilt = {};
    float m_orbitPhase = 0.0f;
    std::chrono::steady_clock::time_point m_lastFrame = {};
    bool m_hasLastFrame = false;

    glm::vec3 m_sortedPosition = { 1e9f, 1e9f, 1e9f };
    glm::vec3 m_sortedForward = {};
    bool m_hasOrder = false;

    // the frame currently in the output texture; nothing is redrawn while the camera stands still
    glm::mat4 m_drawnView = glm::mat4 (0.0f);
    glm::mat4 m_drawnProjection = glm::mat4 (0.0f);
    bool m_hasDrawn = false;
};
}
