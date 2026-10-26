#pragma once

#include "WallpaperEngine/Data/JSON.h"
#include "WallpaperEngine/Render/Camera.h"
#include "WallpaperEngine/Render/ModelData.h"

#include "WallpaperEngine/Render/CWallpaper.h"
#include "WallpaperEngine/Render/FrameMaterial.h"
#include "WallpaperEngine/Render/LedFrameGrabber.h"
#include "WallpaperEngine/Render/ShadowMapping.h"
#include "WallpaperEngine/Render/Volumetrics.h"
#include "WallpaperEngine/Scripting/ScriptEngine.h"

#include <chrono>
#include <set>
#include <unordered_map>

namespace WallpaperEngine::Render {
class Camera;
class CObject;
namespace Objects {
    struct PuppetRopeEnvironment;
    class CLight;
    namespace Effects {
	class CPass;
    }
}
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

    [[nodiscard]] const Data::Model::Properties& getUserProperties () const;

    [[nodiscard]] Scripting::ScriptEngine& getScriptEngine () const;
    [[nodiscard]] Camera& getCamera () const;

    [[nodiscard]] const Scene& getScene () const;

    // _rt_MipMappedFrameBuffer, made on first use (sub_140181AF0, scene flag 0x800): last frame's scene with mips for
    // REFLECTION's roughness LOD
    std::shared_ptr<const CFBO> requireMipMappedFrameBuffer ();
    // _rt_Reflection, output sized with depth, made on first use (sub_140181AF0, scene flag 1)
    std::shared_ptr<const CFBO> requireReflectionFrameBuffer ();
    // _rt_FullAlphaMask(Intermediate), R8 at half the output size, made by the first puppet drawing one (sub_140208670)
    std::shared_ptr<const CFBO> requireAlphaMaskFrameBuffer (bool intermediate);
    /** An object whose program samples _rt_Reflection, it stays out of the reflection pass (object flag 8) */
    void addReflectionReceiver (int id);
    /** While the mirrored reflection pass draws, culling is inverted */
    [[nodiscard]] bool isRenderingReflection () const { return this->m_renderingReflection; }
    /** Keeps fbo at the output's size / divisor, like the buffers WE makes from the window client size */
    void followOutputSize (const std::shared_ptr<CFBO>& fbo, float divisor);
    void followOutputSize (const std::shared_ptr<CFBO>& fbo, const FBO& base);

    [[nodiscard]] int getWidth () const override;
    [[nodiscard]] int getHeight () const override;
    [[nodiscard]] int getCanvasWidth () const override;
    [[nodiscard]] int getCanvasHeight () const override;

    [[nodiscard]] float getTime () const;
    [[nodiscard]] float getDeltaTime () const;
    [[nodiscard]] float getFps () const;
    /** Seconds since the scene was loaded, what wallpaper64.exe keeps in its renderer (+320) and resets past 432000 */
    [[nodiscard]] float getSceneClock () const;
    /** Puppet rope gravity and wind */
    [[nodiscard]] Objects::PuppetRopeEnvironment getRopeEnvironment () const;
    [[nodiscard]] uint32_t getFrameCounter () const;

    /** g_PointerPosition: the cursor over the output (y from the top), not mirrored by a horizontal flip */
    const glm::vec2* getPointerPosition () const;
    const glm::vec2* getPointerPositionLast () const;
    const glm::vec2* getMousePositionNormalized () const;
    const glm::vec2* getCursorScreenPosition () const;
    [[nodiscard]] glm::vec3 unprojectCursor () const;
    /** Script input.cursorWorldPosition: unprojectCursor in WE's world (y up from the bottom left, z 0 in 2D) */
    [[nodiscard]] glm::vec3 getCursorWorldPosition () const;
    /** Script input.cursorScreenPosition: output pixels from the top left, mirrored like the projection when flipped */
    [[nodiscard]] glm::vec2 getCursorPixelPosition () const;
    /** The part of the canvas the output shows as ustart, uend, vstart, vend (the whole canvas in 3D scenes) */
    [[nodiscard]] glm::vec4 getVisibleUVs () const;
    /** getVisibleUVs as the canvas units cut off each side: left, right, bottom, top (y up) */
    [[nodiscard]] glm::vec4 getVisibleMargins () const;
    /** The canvas part on screen in the centered y down layer space: x min, x max, y min, y max (letterbox bars left
     * out) */
    [[nodiscard]] glm::vec4 getVisibleCanvasRegion () const;
    [[nodiscard]] float getOutputAspect () const;
    [[nodiscard]] bool rendersAtOutputSize () const override { return true; }
    /** Pixel size of the output being drawn, what WE's window client area (and _rt_FullFrameBuffer) would be */
    [[nodiscard]] glm::ivec2 getOutputSize () const { return this->m_outputSize; }
    /** 1 and 0.5 over the client size (sub_14017F1B0), refreshed every frame */
    [[nodiscard]] const glm::vec2* getTexelSize () const { return &this->m_texelSize; }
    [[nodiscard]] const glm::vec2* getTexelSizeHalf () const { return &this->m_texelSizeHalf; }
    [[nodiscard]] bool isCursorLeftDown () const { return this->m_cursorLeftDown; }
    /** Position fed to shaders as g_ParallaxPosition: 0.5 +- the smoothed, influence-scaled mouse offset */
    const glm::vec2* getParallaxPosition () const;
    // parallax shift in the y-down scene units objects position in; like WE the whole parent chain moves rigidly by the
    // topmost ancestor's origin and depth
    [[nodiscard]] glm::vec2 getParallaxOffset (const Data::Model::Object& object) const;
    [[nodiscard]] static glm::mat4 objectLocalMatrix (const Data::Model::Object& object);
    /** T * Rz * Ry * Rx * S down the parent chain, WE's object world matrix (sub_1401850A0) */
    [[nodiscard]] glm::mat4 objectWorldMatrix (const Data::Model::Object& object) const;
    /** Without scale (sub_1401DD7D0) */
    [[nodiscard]] glm::mat4 objectFrameMatrix (const Data::Model::Object& object) const;
    /** ILayer.setParent (sub_1401DE750). False is WE's "Invalid parent configuration.", the object loses its parent */
    bool setObjectParent (CObject& object, const CObject* parent, int attachment, bool adjustTransforms);
    /** WE's children list order (object +408) */
    [[nodiscard]] std::vector<CObject*> childrenOf (int id) const;
    /** sub_14018B730: removed after this frame's scripts, lookups skip it until then */
    bool queueDestroyLayer (CObject& object);
    [[nodiscard]] bool isPendingDestroy (const CObject& object) const;
    [[nodiscard]] size_t getPendingDestroyCount () const { return this->m_pendingDestroy.size (); }
    [[nodiscard]] bool scriptsMayReparent () const;

    [[nodiscard]] const std::vector<CObject*>& getObjectsByRenderOrder () const;
    /** Seconds until the first image layer's next sprite frame (sub_140110630), nullopt without the flag */
    [[nodiscard]] std::optional<float> spriteSheetSyncDelay () const;
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
	glm::vec3 eyeLocal { 0.0f };
    };
    /** HDR scene rendering (WE's renderer flag 0x2000): float buffers, HDR define, object brightness, HDR bloom */
    [[nodiscard]] bool isHDR () const { return this->m_hdr; }
    [[nodiscard]] bool hasDistanceFog () const { return this->m_fogDistance; }
    [[nodiscard]] bool hasHeightFog () const { return this->m_fogHeight; }
    [[nodiscard]] const FogUniforms& getFog () const { return this->m_fog; }
    [[nodiscard]] glm::ivec2 getOutputResolution () const;
    /** WE's world (scene units, y up from the bottom left in 2D) to clip space of the scene buffer */
    [[nodiscard]] glm::mat4 getWorldViewProjection (bool perspectiveLayer = false) const;
    void cursorLine (const glm::vec2& ndc, bool perspectiveLayer, glm::vec3& origin, glm::vec3& direction) const;
    /** With WE's depth range (renderer +0x930), for model culling */
    [[nodiscard]] glm::mat4 getCullViewProjection () const;
    [[nodiscard]] const CObject* getObject (int id) const;
    [[nodiscard]] CObject* getObject (int id);
    /** Whether the quad (-half..half, z 0) drawn through mvp covers the clip space point ndc. A planar quad stays
     *  convex under projection, so this is WE's ray test (sub_14019D5A0) done after projecting */
    [[nodiscard]] static bool quadContainsPoint (const glm::mat4& mvp, const glm::vec2& half, const glm::vec2& ndc);
    [[nodiscard]] static std::optional<glm::vec2> quadPlanePoint (const glm::mat4& mvp, const glm::vec2& ndc);
    /** A passthrough layer's buffer while it draws the objects under it (sub_1401ECB20), passes that would draw onto
     *  the scene go there instead */
    struct LayerTarget {
	std::shared_ptr<const CFBO> fbo;
	glm::mat4 transform;
	/** copybackground off: alpha blends with MAX (renderer state flag 0x10) */
	bool alphaMax = false;
	glm::mat4 viewProjection;
	/** WE only recomputes the cached view projection lights read once something marks the renderer dirty (+458);
	 *  the layer doesn't, its first drawn child that isn't a light does */
	bool viewProjectionApplied = false;
    };
    [[nodiscard]] const LayerTarget* getLayerTarget () const;
    // the multisampled stand-in for the scene buffer while objects draw, else the scene buffer
    [[nodiscard]] GLuint getSceneDrawFramebuffer () const;
    /** Resolves the multisampled objects into the scene buffer, for anything about to read it (WE sub_1400D3310) */
    void resolveMultisample () const;
    void executeEffectFunction (const ImageEffect& effect, const std::string& name) const;
    [[nodiscard]] Objects::Effects::CPass* findEffectMaterial (const ImageEffect& effect, size_t passIndex) const;
    [[nodiscard]] bool isDrawnByPassthroughLayer (const CObject& object) const;
    void renderPassthroughChildren (int layerId, const LayerTarget& target);
    void renderLightVolume (const Objects::CLight& light, const glm::mat4& viewProjection);
    [[nodiscard]] bool isHiddenByAncestor (const CObject& object) const;

    /** The four old-style light slots as genericimage2 wants them (g_LightsPosition, g_LightsColorPremultiplied) */
    [[nodiscard]] const glm::vec3* getLightsPosition () const { return this->m_lightsPosition; }
    [[nodiscard]] const glm::vec4* getLightsColorPremultiplied () const { return this->m_lightsColorPremultiplied; }
    /** The same four slots unscaled, (color * intensity, radius), as the 3D shaders read them (g_LightsColorRadius) */
    [[nodiscard]] const glm::vec4* getLightsColorRadius () const { return this->m_lightsColorRadius; }
    /**
     * LightingV1 uniforms (sub_140190C80): up to 15 lights per type in the order lit materials index them, and the
     * counts their LIGHTS_* defines get. The shadow counts stay 0 with shadows off (--shadows disabled)
     */
    struct LightingV1 {
	int points = 0;
	int spots = 0;
	int tubes = 0;
	int directionals = 0;
	int spotCookies = 0;
	int spotShadowCookies = 0;
	int spotShadows = 0;
	int directionalShadows = 0;
	int pointShadows = 0;
	glm::vec4 pointColor[15] = {};
	glm::vec4 pointOrigin[15] = {};
	glm::vec4 spotColor[15] = {};
	glm::vec4 spotOrigin[15] = {};
	glm::vec4 spotDirection[15] = {};
	glm::vec4 spotExponent[15] = {};
	glm::vec4 tubeColor[15] = {};
	glm::vec4 tubeOriginA[15] = {};
	glm::vec4 tubeOriginB[15] = {};
	glm::vec4 directionalColor[15] = {};
	glm::vec4 directionalDirection[15] = {};
	/** spot shadow+cookie, cookie, shadow, then three cascades per shadowed directional light */
	glm::mat4 featureProjection[18] = {};
	glm::vec4 featureProjectionTransform[18] = {};
	glm::vec4 pointShadowProjection[3] = {};
	glm::vec4 pointShadowProjectionTransform[3] = {};

	[[nodiscard]] int features () const {
	    return spotShadowCookies + spotCookies + spotShadows + 3 * directionalShadows;
	}
    };
    [[nodiscard]] const LightingV1& getLightingV1 () const { return this->m_lightingV1; }
    /** WE's shadow setting (renderer +428): 0 off, 1 low .. 4 ultra, LIGHTS_SHADOW_MAPPING_QUALITY */
    [[nodiscard]] int getShadowQuality () const { return this->m_shadowQuality; }
    [[nodiscard]] const std::shared_ptr<const CFBO>& getShadowAtlas () const { return this->_rt_shadowAtlas; }
    [[nodiscard]] bool isShadowCasterVisible (const CObject& object) const;
    /** The texture lit materials sample for cookie spots ("_alias_lightCookie"), null without a cookie spot */
    [[nodiscard]] std::shared_ptr<const TextureProvider> getLightCookie () const;
    [[nodiscard]] std::shared_ptr<const TextureProvider> getMissingTexture () const;
    /** what D3D11 reads from an unbound resource */
    [[nodiscard]] GLuint getNullTexture () const;
    /** see CImage::usesPooledBuffer, uncreated objects are judged by their scene data */
    [[nodiscard]] bool hasNamedLayerBuffer (int id) const;
    [[nodiscard]] std::vector<CObject*> getLayers () const;
    [[nodiscard]] int getObjectIndex (const CObject* object) const;

    void setAudioPolicy (bool muted, std::optional<int> ambientVolume) override;
    [[nodiscard]] bool wantsPointerButtons () const override;

    /** Script play()/stop() request for a Sound object, remembered because scripts can ask before the CSound exists */
    void setSoundPlaying (int id, bool playing);
    [[nodiscard]] std::optional<bool> getSoundPlayRequest (int id) const;
    void pauseSound (int id);
    [[nodiscard]] bool isSoundPlaying (int id, bool startSilent) const;

    Render::CObject* createLayer (const std::string& imagePath);
    Render::CObject* createLayerFromConfig (Data::JSON::JSON config);

    void sortLayer (CObject* object, int index);

    // WE's static scene camera (scene +280 eye, +292 center, +304 up, +316 zoom): get/setCameraTransforms (engine slots
    // 13 / 14), used without a camera object or path (sub_1401891A0); the loader (sub_140186C90) runs init () before
    // setting eye/center/up from scene.json (or the 2D reset camera), so init () sees the defaults and only its zoom
    // survives
    struct StaticCamera {
	glm::vec3 eye { 2.0f };
	glm::vec3 center { 0.0f };
	glm::vec3 up { 0.0f, 1.0f, 0.0f };
	float zoom = 1.0f;
    };
    StaticCamera& getStaticCamera () { return this->m_staticCamera; }

    uint32_t createModelData (const ModelData::Config& config, ModelData::Error& error);
    void applyModelData (uint32_t token, const ModelData::Config& config, bool replace, ModelData::Error& error);
    void destroyModelData (uint32_t token);
    /** A model layer's reference (sub_14021AD10), released with releaseModelData */
    std::shared_ptr<ModelData::Model> acquireModelData (int token);
    void releaseModelData (uint32_t token);
    /** Read once per scene load and shared, like WE's model map (sub_1401D5A40) */
    [[nodiscard]] std::shared_ptr<const std::vector<char>> readModelFile (const std::string& filename);

protected:
    void appendLayer (CObject* object);
    /** The program check of sub_14018C720 / sub_1401D6400: the material's first pass shader against a vertex format */
    ModelData::Error checkModelDataMaterial (const std::string& material, uint32_t format);
    void loadStaticCamera ();
    void renderFrame (const glm::ivec4& viewport) override;
    void renderFrameSteps (const glm::ivec4& viewport);
    void feedLeds ();
    void updateMouse (const glm::ivec4& viewport);
    void dispatchCursorEvents ();

    friend class CWallpaper;

private:
    Render::CObject* buildLayer (Data::JSON::JSON layerJson);
    Render::CObject* createPlaceholderLayer ();

    void expandCanvasToContent (
	const Scene& scene, float width, float height, float& canvasWidth, float& canvasHeight
    ) const;

    /** Refreshes the light slots from the light objects after scripts ran, like sub_1401D5740 + the 0x5D uniform */
    void updateLights ();
    void updateLightingV1 ();
    [[nodiscard]] std::vector<const Light*> sortedLightingV1Lights () const;
    void updateCamera ();
    void updateCameraPath (float dt, glm::vec3& eye, glm::vec3& center, glm::vec3& up, float& zoom);
    /** sub_14017FA70: bloom and combine */
    void renderPostProcessing ();
    [[nodiscard]] glm::ivec2 backBufferSize () const;
    void renderCameraFade ();
    [[nodiscard]] bool drawsImageAdjustments () const override { return true; }
    void renderBloom ();
    void renderHDRBloom ();
    /** (white, peak - white) / 80, (1, 1) on SDR */
    [[nodiscard]] glm::vec2 combineRenderVar () const;
    /** Copies the finished scene into _rt_MipMappedFrameBuffer and rebuilds its mips, WE does it before bloom */
    void updateMipMappedFrameBuffer () const;
    void renderReflection ();
    bool prepareMultisample ();
    void releaseMultisample ();
    void paintLetterbox () const;
    void updateFog (const glm::vec3& eye);
    void updateParallax ();
    [[nodiscard]] int nextFreeLightSlot () const;
    void renderSceneObject (CObject* object);
    /** Draw call of an object under a passthrough layer, sub_1401ECA70 walks the rest of the subtree depth first */
    void renderPassthroughSubtree (int parentId, int depth);
    void renderPassthroughChild (CObject* object);
    [[nodiscard]] glm::mat4 parentChainMatrix (const Data::Model::Object& object, bool scaled) const;
    /** sub_1401DDB50 */
    [[nodiscard]] int passthroughDepth (const CObject& object, int level = 0) const;
    /** Back to front along the camera's view direction, WE's transparent sort (sub_1401865C0) */
    [[nodiscard]] std::vector<CObject*> sortedByDepth (std::vector<CObject*> objects) const;
    /** transparentsorting list membership, decided when the object is created (object flag 0x100, sub_14018FF60) */
    [[nodiscard]] bool isTransparentSorted (const CObject& object);

    Render::CObject* createObject (const Object& object);
    bool createObjectDependencies (const Object& object);
    /** Deletes every render object, the constructor does it too when it fails (the destructor doesn't run then) */
    void destroyObjects ();
    Render::CObject* dispatchObjectType (const Object& object);
    Render::CObject* constructObject (const Object& object);
    void addObjectToRenderOrder (const Object& object);

    std::unique_ptr<Scripting::ScriptEngine> m_scriptEngine;
    ObjectUniquePtr m_settingsScriptsData;
    std::unique_ptr<Scripting::ScriptableObject> m_settingsScripts;
    std::unique_ptr<Camera> m_camera;
    std::map<int, CObject*> m_objects = {};
    std::set<int> m_objectsInCreation = {};
    std::set<int> m_objectsInRenderOrderWalk = {};
    std::map<int, bool> m_soundPlayRequests = {};
    ModelData::Store m_modelData;
    std::map<std::string, std::shared_ptr<const std::vector<char>>> m_modelFiles = {};
    bool m_loading = true;
    StaticCamera m_staticCamera;
    bool m_staticCameraLoaded = false;
    /** checkModelDataMaterial results, compiling the material's shader every time would be slow */
    std::map<std::pair<std::string, uint32_t>, ModelData::Error> m_modelDataMaterialChecks = {};
    std::vector<CObject*> m_objectsByRenderOrder = {};
    std::vector<CObject*> m_sceneCameras = {};
    /** Where the active camera object puts the eye, WE world units (y up), zero without one */
    glm::vec2 m_cameraEye = {};
    int m_pathIndex = 0;
    int m_pathKey = 0;
    float m_pathTime = 0.0f;
    float m_cameraFade = 0.0f;
    FogUniforms m_fog;
    std::unique_ptr<Volumetrics> m_volumetrics;
    std::unique_ptr<LedFrameGrabber> m_ledGrabber;
    std::vector<uint8_t> m_ledFrame;
    std::chrono::steady_clock::time_point m_ledNext = {};
    bool m_fogDistance = false;
    bool m_fogHeight = false;
    bool m_hdr = false;
    bool m_displayHDR = false;
    /** sub_14017FA70, loaded on the first frame */
    struct FrameMaterials {
	std::unique_ptr<FrameMaterial> combine;
	std::unique_ptr<FrameMaterial> combineWithoutBloom;
	std::unique_ptr<FrameMaterial> downsampleQuarterBloom;
	std::unique_ptr<FrameMaterial> downsampleEighthBlurV;
	std::unique_ptr<FrameMaterial> blurHBloom;
	std::unique_ptr<FrameMaterial> hdrDownsampleBloom;
	std::unique_ptr<FrameMaterial> hdrDownsample;
	std::unique_ptr<FrameMaterial> hdrUpsample;
	std::unique_ptr<FrameMaterial> hdrUpsampleCubic;
	std::unique_ptr<FrameMaterial> fade;
    };
    std::unique_ptr<FrameMaterials> m_frameMaterials;
    /** sub_14017F1B0 */
    std::vector<std::shared_ptr<CFBO>> m_hdrBloomLevels;
    std::vector<DynamicValue*> m_scriptedValues = {};
    // backs createLayer() objects, must outlive them
    std::map<int, ObjectUniquePtr> m_dynamicObjectData = {};
    // ids of layers scripts create start here, past anything a scene file numbers
    static constexpr int DYNAMIC_LAYER_ID_BASE = 2000000000;
    int m_nextDynamicLayerId = DYNAMIC_LAYER_ID_BASE;
    std::map<std::string, std::string> m_createLayerAliases = {};
    glm::vec2 m_mousePosition = {};
    glm::vec2 m_pointerPosition = {};
    glm::vec2 m_pointerPositionLast = {};
    glm::vec2 m_mousePositionNormalized = {};
    glm::vec2 m_cursorScreen = {};
    /** Cursor in output pixels from the viewport's top left, unclamped and unflipped (WE's ScreenToClient point) */
    glm::vec2 m_mousePositionViewport = {};
    glm::ivec2 m_outputSize = {};
    glm::vec2 m_texelSize = { 1.0f, 1.0f };
    glm::vec2 m_texelSizeHalf = { 0.5f, 0.5f };
    glm::vec2 m_cameraParallax = {};
    glm::vec2 m_parallaxPosition = { 0.5f, 0.5f };
    glm::vec2 m_parallaxBias = {};
    bool m_cursorLeftDown = false;
    float m_startTime = 0.0f;
    uint32_t m_frameCounter { 0 };
    glm::vec3 m_lightsPosition[4] = {};
    glm::vec4 m_lightsColorPremultiplied[3] = {};
    glm::vec4 m_lightsColorRadius[4] = {};
    LightingV1 m_lightingV1;
    int m_shadowQuality = 0;
    std::vector<ShadowMapping::Entry> m_shadowEntries = {};
    std::unordered_map<const Data::Model::Light*, Volumetrics::Shadow> m_volumeShadows = {};
    std::unique_ptr<ShadowMapping> m_shadowMapping;
    glm::vec2 m_cursorLastScenePosition = {};
    std::set<int> m_cursorInside = {};
    std::set<int> m_cursorPressed = {};
    std::vector<LayerTarget> m_layerTargets = {};
    std::map<const CObject*, bool> m_transparentSorted = {};
    std::shared_ptr<const CFBO> _rt_shadowAtlas = nullptr;
    std::shared_ptr<const CFBO> _rt_MipMappedFrameBuffer = nullptr;
    std::shared_ptr<CFBO> _rt_Reflection = nullptr;
    std::shared_ptr<CFBO> _rt_FullAlphaMask = nullptr;
    std::shared_ptr<CFBO> _rt_FullAlphaMaskIntermediate = nullptr;
    std::set<int> m_reflectionReceivers = {};
    int m_msaaSamples = 0;
    bool m_msaaActive = false;
    GLuint m_msaaFramebuffer = GL_NONE;
    GLuint m_msaaColor = GL_NONE;
    GLuint m_msaaDepth = GL_NONE;
    glm::ivec2 m_msaaSize = {};
    bool m_renderingReflection = false;
    struct OutputSizedBuffer {
	std::weak_ptr<CFBO> fbo;
	float divisor = 1.0f;
	const FBO* base = nullptr;

	[[nodiscard]] glm::uvec2 sizeFor (glm::ivec2 output) const;
    };
    std::vector<OutputSizedBuffer> m_outputSizedBuffers = {};
    glm::ivec2 m_outputBufferSize = {};
    void resizeOutputBuffers (glm::ivec2 size);
    mutable std::optional<bool> m_scriptsMayReparent = std::nullopt;
    /** WE's order (object +408) */
    std::unordered_map<int, std::vector<int>> m_children = {};
    void attachChild (const CObject& child);
    void detachChild (const CObject& child);
    std::set<CObject*> m_pendingDestroy = {};
    std::vector<CObject*> m_destroyedObjects = {};
    /** layers a script created and destroyed, freed a couple of frames later (nothing else is left holding them) */
    struct DestroyedLayer {
	CObject* object;
	uint32_t frame;
    };
    std::vector<DestroyedLayer> m_releasableLayers = {};
    [[nodiscard]] bool isReleasable (const CObject& object) const;
    void releaseDestroyedLayers ();
    void releaseLayerModel (int id);
    void destroyPendingLayers ();
    mutable std::shared_ptr<const TextureProvider> m_missingTexture = nullptr;
    mutable GLuint m_nullTexture = GL_NONE;
};
}
