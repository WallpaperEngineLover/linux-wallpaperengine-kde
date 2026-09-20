#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "WallpaperEngine/Render/CWallpaper.h"

namespace WallpaperEngine::Render::Wallpapers {
class SplatSorter;

// Native renderer for the "SOG depth wallpaper" family: a Gaussian splat cloud (PlayCanvas SOG format) viewed through a camera at
// the photo's origin that sways with the mouse to fake depth. These items normally host a SuperSplat viewer in CEF, which drew
// torn patches under offscreen rendering, so the same data is rendered natively with the camera model ported from the wallpaper's JS.
class CSplat : public CWallpaper {
public:
    CSplat (
	const Wallpaper& wallpaper, RenderContext& context, AudioContext& audioContext,
	const WallpaperState::TextureUVsScaling& scalingMode, const uint32_t& clampMode
    );
    ~CSplat () override;

    // True for projects built on the SOG depth wallpaper base (they expose its sog* properties)
    static bool supports (const Project& project);

    [[nodiscard]] int getWidth () const override { return this->m_width; }
    [[nodiscard]] int getHeight () const override { return this->m_height; }

protected:
    void renderFrame (const glm::ivec4& viewport) override;

    friend class CWallpaper;

private:
    // Everything the wallpaper's camera needs from the splat cloud, in the viewer's world frame
    // (the photo frame rotated 180 degrees around Z, see loadCloud())
    struct SceneProfile {
	// direction from the origin camera through the center of the visible content
	glm::vec3 direction = { 0.0f, 0.0f, 1.0f };
	// 2%..98% quantile range of splat depth along z, and its median
	float depthMin = 1.0f;
	float depthMax = 2.0f;
	// horizontal/vertical angular size (radians) of the 2%..98% quantile range
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
	// z of the nearest face of the scene's bounding box, the plane the view must not leave
	float frontZ = 0.0f;
	// degrees, horizontal when the viewport is landscape, vertical otherwise
	float fov = 60.0f;
	glm::ivec2 viewport = {};
	float focusDepth = 0.0f;
    };

    struct Pose {
	glm::vec3 position = {};
	glm::vec3 target = {};
    };

    // One capsule of the clock's seven segment digits, in screen space (0..1, y down)
    struct ClockPoint {
	float x = 0.0f;
	float y = 0.0f;
	// > 0 horizontal capsule, < 0 vertical, 0 a round dot; in aspect-corrected screen height units
	float halfLength = 0.0f;
	// 1 fully formed, ramping up from 0 while appearing, negative while sinking away
	float strength = 0.0f;
    };

    static constexpr size_t CLOCK_POINT_CAPACITY = 32;

    void loadCloud ();
    void setupGL (const std::vector<float>& centers, const std::vector<float>& rotations, const std::vector<float>& scales);
    void resizeOutput (int width, int height);

    std::vector<ClockPoint> buildClockPoints (
	const std::string& text, float aspect, float size, float originX, float originY
    ) const;
    // Advances the clock's animation and refreshes the uniform data below; true if anything changed
    bool updateClock (float aspect);

    BaseCamera buildBaseCamera (int width, int height, float focusDepth) const;
    Pose posePlacement (const BaseCamera& base, glm::vec2 tilt, glm::vec2 orbit, float parallaxStrength) const;
    bool viewFitsFrontFace (const glm::vec3& position, const glm::vec3& target, const BaseCamera& base) const;

    [[nodiscard]] double numberProperty (const std::string& name, double fallback) const;
    [[nodiscard]] std::string stringProperty (const std::string& name) const;

    SceneProfile m_profile;
    BaseCamera m_base;
    bool m_hasBase = false;
    // Workshop presets are single photos that rarely match the screen's aspect ratio: show the
    // whole photo (bars at the sides on a wide screen) instead of the base page's crop-to-fill.
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

    int m_width = 16;
    int m_height = 17;

    std::string m_clockText;
    std::vector<ClockPoint> m_clockCurrent;
    std::vector<ClockPoint> m_clockPrevious;
    std::chrono::steady_clock::time_point m_clockTransitionStart = {};
    // the layout the current points were built for, so a settings change rebuilds them
    std::array<float, 5> m_clockLayout = {};
    std::array<glm::vec4, CLOCK_POINT_CAPACITY> m_clockPoints = {};
    // enabled, lift, radius, aspect
    glm::vec4 m_clockParams = {};
    // screen area (min x/y, max x/y) the clock effect can touch, so other splats skip the field test
    glm::vec4 m_clockBounds = {};
    float m_clockDistance = 0.0f;

    glm::vec2 m_tilt = {};
    float m_orbitPhase = 0.0f;
    std::chrono::steady_clock::time_point m_lastFrame = {};
    bool m_hasLastFrame = false;

    // what the current sort order was built for, so a new sort is only requested once the camera
    // has moved enough to matter
    glm::vec3 m_sortedPosition = { 1e9f, 1e9f, 1e9f };
    glm::vec3 m_sortedForward = {};
    bool m_hasOrder = false;

    // the frame currently in the output texture; nothing is redrawn while the camera stands still
    glm::mat4 m_drawnView = glm::mat4 (0.0f);
    glm::mat4 m_drawnProjection = glm::mat4 (0.0f);
    bool m_hasDrawn = false;
};
} // namespace WallpaperEngine::Render::Wallpapers
