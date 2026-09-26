#pragma once

#include "WallpaperEngine/Render/Camera.h"

#include "WallpaperEngine/Render/CWallpaper.h"
#include "WallpaperEngine/Render/Volumetrics.h"
#include "WallpaperEngine/Scripting/ScriptEngine.h"

#include <set>

namespace WallpaperEngine::Render {
class Camera;
class CObject;
}

namespace WallpaperEngine::Render::Wallpapers {
using namespace WallpaperEngine::Data::Model;

class CScene final : public CWallpaper {
public:
    CScene (
	const Wallpaper& wallpaper, RenderContext& context, AudioContext& audioContext,
	const WallpaperState::TextureUVsScaling& scalingMode, const uint32_t& clampMode
    );

    ~CScene () override;

    /** The project's user properties (project.json "properties"), keyed by name */
    [[nodiscard]] const Data::Model::Properties& getUserProperties () const;

    [[nodiscard]] Scripting::ScriptEngine& getScriptEngine () const;
    [[nodiscard]] Camera& getCamera () const;

    [[nodiscard]] const Scene& getScene () const;

    [[nodiscard]] int getWidth () const override;
    [[nodiscard]] int getHeight () const override;
    [[nodiscard]] int getCanvasWidth () const override;
    [[nodiscard]] int getCanvasHeight () const override;

    // Used by CText/ScriptEngine; read from the same g_Time/g_TimeLast globals CParticle consumes via extern.
    [[nodiscard]] float getTime () const;
    [[nodiscard]] float getDeltaTime () const;
    [[nodiscard]] float getFps () const;
    /** Seconds since the scene was loaded, what wallpaper64.exe keeps in its renderer (+320) and resets past 432000 */
    [[nodiscard]] float getSceneClock () const;

    const glm::vec2* getMousePosition () const;
    const glm::vec2* getMousePositionLast () const;
    const glm::vec2* getMousePositionNormalized () const;
    [[nodiscard]] bool isCursorLeftDown () const { return this->m_cursorLeftDown; }
    /** Position fed to shaders as g_ParallaxPosition: 0.5 +- the smoothed, influence-scaled mouse offset */
    const glm::vec2* getParallaxPosition () const;
    /**
     * Parallax translation of an object in scene units, in the y-down space CImage/CText/CParticle position
     * themselves in. Like the real engine, the whole parent chain shifts as one rigid group using the topmost
     * ancestor's origin and parallaxDepth, a child's own depth is ignored.
     */
    [[nodiscard]] glm::vec2 getParallaxOffset (const Data::Model::Object& object) const;

    [[nodiscard]] const std::vector<CObject*>& getObjectsByRenderOrder () const;
    /** g_Fog* uniforms (sub_140186440). Height params come in two versions: WE's world (y up from the bottom) and
     *  the space 2D objects are laid out in here (y down from the center), the same in 3D scenes */
    struct FogUniforms {
	glm::vec3 distanceColor { 0.0f };
	glm::vec4 distanceParams { 0.0f };
	glm::vec3 heightColor { 0.0f };
	glm::vec4 heightParamsWorld { 0.0f };
	glm::vec4 heightParamsLocal { 0.0f };
	/** g_EyePosition in WE's world, what the fog of passes drawing in world space measures from */
	glm::vec3 eyeWorld { 0.0f };
	/** The same eye in the space 2D objects are laid out in */
	glm::vec3 eyeLocal { 0.0f };
    };
    /** HDR scene rendering (WE's renderer flag 0x2000): float buffers, HDR define, object brightness, HDR bloom */
    [[nodiscard]] bool isHDR () const { return this->m_hdr; }
    [[nodiscard]] bool hasDistanceFog () const { return this->m_fogDistance; }
    [[nodiscard]] bool hasHeightFog () const { return this->m_fogHeight; }
    [[nodiscard]] const FogUniforms& getFog () const { return this->m_fog; }
    /** Size the whole scene would have at the output's scale, what WE's render targets are sized by */
    [[nodiscard]] glm::ivec2 getOutputResolution () const;
    /** WE's world (scene units, y up from the bottom left in 2D) to clip space of the scene buffer */
    [[nodiscard]] glm::mat4 getWorldViewProjection () const;
    /** Position of the active camera object in WE world units (y up, relative to the default camera), zero without one */
    [[nodiscard]] const glm::vec2& getCameraEye () const { return this->m_cameraEye; }
    [[nodiscard]] const CObject* getObject (int id) const;
    [[nodiscard]] CObject* getObject (int id);
    /** True when any group above the object (through "parent") is hidden */
    [[nodiscard]] bool isHiddenByAncestor (const CObject& object) const;

    /** The four old-style light slots as genericimage2 wants them (g_LightsPosition, g_LightsColorPremultiplied) */
    [[nodiscard]] const glm::vec3* getLightsPosition () const { return this->m_lightsPosition; }
    [[nodiscard]] const glm::vec4* getLightsColorPremultiplied () const { return this->m_lightsColorPremultiplied; }
    /** The same four slots unscaled, (color * intensity, radius), as the 3D shaders read them (g_LightsColorRadius) */
    [[nodiscard]] const glm::vec4* getLightsColorRadius () const { return this->m_lightsColorRadius; }
    /** The render order as scripts see it: without the synthesized bloom layer */
    [[nodiscard]] std::vector<CObject*> getLayers () const;
    [[nodiscard]] int getObjectIndex (const CObject* object) const;

    void setAudioPolicy (bool muted, std::optional<int> ambientVolume) override;

    /** Script play()/stop() request for a Sound object, remembered because scripts can ask before the CSound exists */
    void setSoundPlaying (int id, bool playing);
    [[nodiscard]] std::optional<bool> getSoundPlayRequest (int id) const;

    /** Creates a new image layer from a model json at runtime, appended to the render order. Backs
     *  the scripting API's thisScene.createLayer(). Returns nullptr if the model couldn't be set up. */
    Render::CObject* createLayer (const std::string& imagePath);

    /** Moves an existing layer to the given render-order slot. Backs thisScene.sortLayer(). */
    void sortLayer (CObject* object, int index);

protected:
    void appendLayer (CObject* object);
    void renderFrame (const glm::ivec4& viewport) override;
    void renderFrameSteps (const glm::ivec4& viewport);
    void updateMouse (const glm::ivec4& viewport);
    /** Hover/press tracking that turns pointer state into cursorEnter/cursorClick/... calls on scripted layers */
    void dispatchCursorEvents ();

    friend class CWallpaper;

private:
    /**
     * Grows the render canvas (symmetrically around the layout center, so object positions stay valid) until
     * every top-level image layer with a declared size fits, see --expand-canvas
     */
    void expandCanvasToContent (
	const Scene& scene, float width, float height, float& canvasWidth, float& canvasHeight
    ) const;

    /** Refreshes the light slots from the light objects after scripts ran, like sub_1401D5740 + the 0x5D uniform */
    void updateLights ();
    /** Camera object view, parallax camera and perspective layer camera for this frame (sub_1401891A0) */
    void updateCamera ();
    /** Scene camera paths without a camera object, advances them by dt and writes this frame's camera */
    void updateCameraPath (float dt, glm::vec3& eye, glm::vec3& center, glm::vec3& up, float& zoom);
    /** camerafade overlay over the finished scene */
    void renderCameraFade ();
    /** WE's HDR bloom (sub_140183610) and combine_hdr_upsample into the scene buffer */
    void renderHDRBloom ();
    void releaseHDRBloom ();
    void updateFog (const glm::vec3& eye);
    /** Mouse parallax smoothing, after updateCamera () since a camera object moves the parallax camera too */
    void updateParallax ();
    /** T * Rz * Ry * Rx * S down the parent chain, WE's object world matrix (sub_1401850A0) */
    [[nodiscard]] glm::mat4 objectWorldMatrix (const Object& object) const;
    [[nodiscard]] int nextFreeLightSlot () const;

    Render::CObject* createObject (const Object& object);
    void createObjectDependencies (const Object& object);
    Render::CObject* dispatchObjectType (const Object& object);
    void addObjectToRenderOrder (const Object& object);

    std::unique_ptr<Scripting::ScriptEngine> m_scriptEngine;
    std::unique_ptr<Camera> m_camera;
    ObjectUniquePtr m_bloomObjectData;
    CObject* m_bloomObject = nullptr;
    std::map<int, CObject*> m_objects = {};
    std::set<int> m_objectsInCreation = {};
    std::set<int> m_objectsInRenderOrderWalk = {};
    std::map<int, bool> m_soundPlayRequests = {};
    std::vector<CObject*> m_objectsByRenderOrder = {};
    /** Camera objects in creation order, the last visible one wins */
    std::vector<CObject*> m_sceneCameras = {};
    /** Where the active camera object puts the eye, WE world units (y up), zero without one */
    glm::vec2 m_cameraEye = {};
    int m_pathIndex = 0;
    int m_pathKey = 0;
    float m_pathTime = 0.0f;
    float m_cameraFade = 0.0f;
    FogUniforms m_fog;
    std::unique_ptr<Volumetrics> m_volumetrics;
    bool m_fogDistance = false;
    bool m_fogHeight = false;
    bool m_hdr = false;
    /** HDR bloom: mip chain at half the output resolution and down, see renderHDRBloom () */
    struct BloomLevel {
	GLuint texture = GL_NONE;
	GLuint framebuffer = GL_NONE;
	glm::ivec2 size {};
    };
    std::vector<BloomLevel> m_bloomLevels;
    BloomLevel m_hdrCopy;
    glm::ivec2 m_bloomResolution {};
    GLuint m_bloomDownsample = GL_NONE;
    GLuint m_bloomDownsampleThreshold = GL_NONE;
    GLuint m_bloomUpsample = GL_NONE;
    GLuint m_bloomUpsampleCubic = GL_NONE;
    GLuint m_bloomCombine = GL_NONE;
    GLuint m_fadeProgram = GL_NONE;
    std::vector<DynamicValue*> m_scriptedValues = {};
    // owns the synthesized model data backing createLayer()'d objects; must outlive the CObject
    // built from it (same pattern as m_bloomObjectData)
    std::vector<ObjectUniquePtr> m_dynamicObjectData = {};
    int m_nextDynamicLayerId = 2000000000;
    std::map<std::string, std::string> m_createLayerAliases = {};
    glm::vec2 m_mousePosition = {};
    glm::vec2 m_mousePositionLast = {};
    glm::vec2 m_mousePositionNormalized = {};
    /** Smoothed camera offset from the scene center as a fraction of the scene size, in mouse coordinates */
    glm::vec2 m_cameraParallax = {};
    glm::vec2 m_parallaxPosition = { 0.5f, 0.5f };
    /** Wallpaper position offset (WallpaperState) riding through the parallax path */
    glm::vec2 m_parallaxBias = {};
    bool m_cursorLeftDown = false;
    float m_startTime = 0.0f;
    glm::vec3 m_lightsPosition[4] = {};
    glm::vec4 m_lightsColorPremultiplied[3] = {};
    glm::vec4 m_lightsColorRadius[4] = {};
    glm::vec2 m_cursorLastScenePosition = {};
    // object ids the pointer is over / that the current press started on
    std::set<int> m_cursorInside = {};
    std::set<int> m_cursorPressed = {};
    std::shared_ptr<const CFBO> _rt_4FrameBuffer = nullptr;
    std::shared_ptr<const CFBO> _rt_8FrameBuffer = nullptr;
    std::shared_ptr<const CFBO> _rt_Bloom = nullptr;
    std::shared_ptr<const CFBO> _rt_shadowAtlas = nullptr;
};
} // namespace WallpaperEngine::Render::Wallpaper
