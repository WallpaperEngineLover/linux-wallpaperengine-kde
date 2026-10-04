#pragma once

#include "CRenderable.h"
#include "WallpaperEngine/Render/CObject.h"
#include "WallpaperEngine/Render/Objects/Effects/CPass.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"

#include "WallpaperEngine/Data/JSON.h"
#include "WallpaperEngine/Render/Shaders/Shader.h"

#include "../TextureProvider.h"
#include "PuppetClipping.h"
#include "PuppetRig.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <limits>
#include <optional>
#include <set>
#include <vector>

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Scripting;
namespace WallpaperEngine::Render::Objects::Effects {
class CMaterial;
class CPass;
} // namespace WallpaperEngine::Render::Objects::Effects

namespace WallpaperEngine::Render::Objects {
/** File offsets of the first flag 2 mesh */
struct PuppetBlendMesh {
    std::string material;
    uint32_t rows = 0;
    uint32_t format = 0;
    size_t vertices = 0;
    uint32_t vertexBytes = 0;
    size_t indices = 0;
    uint32_t indexBytes = 0;
};

/** Morph texture of the first mesh (sub_1401FBAE0) */
struct PuppetMorphTargets {
    float scale = 0.0f;
    uint32_t vertexCount = 0;
    std::vector<glm::vec4> texels;
    /** some target alpha is below 1 */
    bool alpha = false;
};

class CImage final : public CRenderable, public ScriptableObject {
    friend CObject;

public:
    CImage (Wallpapers::CScene& scene, const Image& image);
    ~CImage () override;

    void setup () override;
    void render () override;

    /** Refreshes the image's own texture plus any video a pass pulled in from a user texture slot */
    void updateTextures () const;
    /** Cursor hit test (sub_14019DBB0): the layer's quad as it is drawn, rotation and parallax included, ndc in the
     *  scene buffer's clip space. Fullscreen layers are always hit */
    [[nodiscard]] bool hitTest (const glm::vec2& ndc);
    /** A cursor event's localPosition: (u * width, (1 - v) * height) of the unscaled layer, from its top left, where
     *  the cursor meets the quad's plane (sub_14019DBB0), off the quad too. Zero when it doesn't meet the plane */
    [[nodiscard]] glm::vec2 cursorLocalPosition (const glm::vec2& ndc);
    void renderPassthroughChildren (const std::shared_ptr<const CFBO>& buffer);
    [[nodiscard]] const Image& getImage () const;
    [[nodiscard]] glm::vec2 getSize () const;
    /** Another object samples this layer's composite FBO, so its passes run even while it's hidden */
    void markAsDependency ();

    [[nodiscard]] GLuint getSceneSpacePosition () const;
    [[nodiscard]] GLuint getCopySpacePosition () const;
    [[nodiscard]] GLuint getPassSpacePosition () const;
    [[nodiscard]] GLuint getTexCoordCopy () const;
    [[nodiscard]] GLuint getTexCoordPass () const;

    [[nodiscard]] const float& getBrightness () const override;
    [[nodiscard]] const float& getUserAlpha () const override;
    [[nodiscard]] const float& getAlpha () const override;
    [[nodiscard]] const glm::vec3& getColor () const override;
    /** a solid layer instance whose user texture slot the user filled */
    [[nodiscard]] bool showsUserTextureOnSolidLayer () const;
    [[nodiscard]] const glm::vec4& getColor4 () const override;
    [[nodiscard]] const glm::vec3& getCompositeColor () const override;

    void pinpongFramebuffer (std::shared_ptr<const CFBO>* drawTo, std::shared_ptr<const TextureProvider>* asInput);

    /** A puppet attachment point's current animated position, rotation and scale, in this puppet's own
     * local mesh space (same space as PuppetAttachmentPoint's position/localTransform). A negative
     * scale component means the bone's transform includes a reflection (a mirrored bone). */
    struct AttachmentPointTransform {
	glm::vec3 position;
	float angle;
	glm::vec2 scale;
    };

    /**
     * @param name A named attachment point on this puppet's rig (see PuppetAttachmentPoint)
     * @return The point's current animated transform, or nullopt if there's no such point (or no puppet mesh)
     */
    [[nodiscard]] std::optional<AttachmentPointTransform>
    getAttachmentPointMeshTransform (const std::string& name) const;

    /** Per frame puppet update (WE's image update, sub_1401FDF90): animation, physics and the bone matrices, before
     *  the scripts run so they read and change this frame's pose */
    void updatePuppetPose ();

    /**
     * Bone access for IImageLayer scripts (wallpaper64 2.8.42 sub_140211070). Matrices are column-vector glm, the
     * transpose of WE's row-vector ones with the same 16 floats in memory. Scene matrices are object world * bone,
     * local ones relative to the parent bone. Only valid while hasPuppetPose(), for bones below the bone count.
     */
    [[nodiscard]] bool hasPuppetPose () const;
    [[nodiscard]] const std::vector<PuppetBone>& getPuppetBones () const { return this->m_rig.bones; }
    [[nodiscard]] PuppetRig& getRig () { return this->m_rig; }
    [[nodiscard]] const PuppetRig& getRig () const { return this->m_rig; }
    /** WE's image world (vtable slot 16, sub_1401FD3F0): the object's world moved by the alignment offset */
    [[nodiscard]] glm::mat4 worldMatrix () const;
    [[nodiscard]] int findPuppetBone (const std::string& name) const;
    [[nodiscard]] const glm::mat4& getPuppetBoneTransform (int bone) const;
    void setPuppetBoneTransform (int bone, const glm::mat4& transform);
    [[nodiscard]] const glm::mat4& getPuppetLocalBoneTransform (int bone) const;
    void setPuppetLocalBoneTransform (int bone, const glm::mat4& transform);
    void applyPuppetBonePhysicsImpulse (int bone, const glm::vec3& directional, const glm::vec3& angularDegrees);
    void resetPuppetBonePhysics (int bone);
    /** The animation layer with this serial for IAnimationLayer scripts, null once destroyed or when a scene layer
     *  plays no clip (WE never creates such a layer) */
    [[nodiscard]] PuppetActiveAnimation* findPuppetAnimationLayer (size_t serial);
    /**
     * IImageLayer animation layer calls (wallpaper64 2.8.42 sub_14020E910..sub_14020EF80), on the layer list in blend
     * order. Serials identify layers, nullopt when there is none
     */
    [[nodiscard]] size_t getPuppetAnimationLayerCount () const;
    [[nodiscard]] std::optional<size_t> getPuppetAnimationLayerAt (int64_t index) const;
    [[nodiscard]] std::optional<size_t> findPuppetAnimationLayerByName (const std::string& name) const;
    /** animation is a clip name (string) or a layer config (object); config's keys go over it */
    std::optional<size_t>
    createPuppetAnimationLayer (const Data::JSON::JSON& animation, const Data::JSON::JSON& config, bool autoRemove);
    /** removes every layer called name */
    bool destroyPuppetAnimationLayersByName (const std::string& name);
    bool destroyPuppetAnimationLayer (size_t serial);

    /**
     * ITextureAnimation (wallpaper64 2.8.42 sub_14020E670, members sub_1402131A0): a layer's own clock for its
     * animated texture. Attached it follows the texture's shared clock; pause, stop, setFrame and a rate other
     * than 1 detach it, join attaches it again. Frames are indices in the texture's frame list
     */
    struct TextureAnimation {
	/** +72 */
	bool detached = false;
	/** +224 */
	bool playing = true;
	/** +228 */
	float rate = 1.0f;
	/** +232/+236: the frame and the time spent in it */
	int frame = 0;
	float time = 0.0f;
    };

    /** Made on first use, only for an animated texture (texture flag 4), null otherwise */
    [[nodiscard]] TextureAnimation* getTextureAnimation ();
    /** The texture clock's frame and the time spent in it, what pausing a layer's animation starts from */
    [[nodiscard]] std::pair<int, float> sharedTextureFrame () const;
    [[nodiscard]] int getTextureFrameCount () const;
    [[nodiscard]] float getTextureDuration () const;
    /** sub_1401FDF90's tail: a detached, playing animation moves by frametime * rate, before scripts run */
    void updateTextureAnimation (float frametime);
    [[nodiscard]] std::optional<int> getTextureFrameOverride () const override;

protected:
    void setupPasses ();
    void rebuildActivePasses ();
    /** Fullscreen layers over the scene buffer keep their buffers at the output's size */
    [[nodiscard]] bool followsOutputSize () const;
    void addEffectPasses (const ImageEffect& effect);
    [[nodiscard]] bool effectVisibilityChanged () const;

    void updateScreenSpacePosition ();
    [[nodiscard]] glm::mat4 ancestorTiltCorrection () const;
    void updateEffectTextureProjection ();
    void updateLightingTransform (const glm::mat4& sceneTransform);

    struct ResolvedTransform {
	glm::vec3 origin;
	glm::vec3 scale;
	float angle;
    };

    [[nodiscard]] ResolvedTransform resolveTransform (const WallpaperEngine::Data::Model::Object& object) const;
    /** Scene camera of this layer, the perspective layer camera when "perspective" is set */
    [[nodiscard]] glm::mat4 getViewProjection () const;

private:
    /** Composite pass that applies the scene's fog, when there is fog */
    Effects::CPass* m_fogPass = nullptr;

public:
    /**
     * Computes the object's own transform (origin/scale/angle) without walking the
     * parent chain. Used as the per-node step of resolveTransform.
     */
    [[nodiscard]] static ResolvedTransform localTransform (const WallpaperEngine::Data::Model::Object& object);

private:
    bool loadPuppetMesh (const glm::vec2& size);
    void updatePuppetPositionBuffer (const glm::vec2& size);
    [[nodiscard]] glm::mat4 puppetObjectWorld () const;
    /** Skins the puppet vertices with the current bone matrices and re-uploads them */
    void updatePuppetSkinning ();
    [[nodiscard]] ComboMap puppetVertexAlphaCombos () const;
    void updatePuppetVertexAlpha (const std::vector<float>& morphAlpha);
    void setupPuppetGeometryCallback (Effects::CPass* pass) const;
    /** The mask and clipping target passes of a puppet with clipping records, once the mesh pass exists */
    void setupPuppetClipping ();
    /** sub_1401FDF90 end: sorts the parts by order + their bone's animated draw order, rebuilds the indices */
    void updatePuppetDrawOrder ();
    /** The mesh pass's draw split up like WE's command list (sub_140208670) */
    void renderPuppetClipped (Effects::CPass* meshPass);
    /** sub_14020D6A0: one record's mask into _rt_FullAlphaMask, or multiplied into it through the intermediate */
    void renderPuppetClipMask (const Effects::CPass& meshPass, int record, int draw, bool clear, bool intermediate);
    void selectPuppetDraw (int draw);
    void loadPuppetBlendMesh (const std::vector<char>& data);
    /** sub_140209540 albedo buffer */
    void setupPuppetBlendMap ();
    ResolvedTransform updateGeometryBuffers ();
    [[nodiscard]] glm::vec2 resolveGeometrySize (float sceneWidth, float sceneHeight, glm::vec3& origin) const;
    void updateScenePosition (
	const glm::vec3& origin, const glm::vec2& size, const glm::vec3& scale, float sceneWidth, float sceneHeight
    );
    void uploadGeometryBuffers (const glm::vec2& size);
    [[nodiscard]] bool shouldRenderFinalPass (bool isLastPass) const;
    bool configurePassTarget (
	Effects::CPass* pass, std::shared_ptr<const CFBO>& drawTo,
	const std::shared_ptr<const TextureProvider>& asInput, std::shared_ptr<const TextureProvider>& effectInput,
	bool& inTargetEffectSequence
    );

    GLuint m_sceneSpacePosition;
    GLuint m_copySpacePosition;
    GLuint m_passSpacePosition;
    GLuint m_texcoordCopy;
    GLuint m_texcoordPass;
    GLuint m_puppetSpacePosition = GL_NONE;
    GLuint m_puppetTexCoord = GL_NONE;
    GLuint m_puppetIndices = GL_NONE;
    GLsizei m_puppetIndexCount = 0;
    bool m_hasPuppetMesh = false;
    // the pass that draws the warped mesh, and whether it is the last one (straight into the scene FBO)
    Effects::CPass* m_puppetMeshPass = nullptr;
    bool m_puppetMeshLast = false;
    /** what the puppet geometry callback draws: the whole mesh, or one of the clipping draws */
    GLuint m_puppetDrawBuffer = GL_NONE;
    GLsizei m_puppetDrawOffset = 0;
    GLsizei m_puppetDrawCount = 0;
    /** the mesh pass draws again into what it already drew this frame */
    bool m_puppetDrawKeep = false;
    std::optional<PuppetClipping> m_puppetClipping = std::nullopt;
    GLuint m_puppetClipIndices = GL_NONE;
    /** mesh flag 8: the part ranges, the order the animated draw order put them in and the file's indices */
    std::vector<PuppetClipping::Part> m_puppetParts = {};
    std::vector<uint32_t> m_puppetPartOrder = {};
    std::vector<uint16_t> m_puppetMeshIndices = {};
    /** clippingmaskimage4 per record, each with its mask texture */
    std::vector<Effects::CPass*> m_puppetClipMaskPasses = {};
    /** the mesh pass's material with CLIPPINGUVS and CLIPPINGTARGET */
    Effects::CPass* m_puppetClipTargetPass = nullptr;
    /** flattexture of the intermediate mask multiplied into _rt_FullAlphaMask, for nested masks */
    Effects::CPass* m_puppetClipComposePass = nullptr;
    GLuint m_puppetClipComposePosition = GL_NONE;
    GLuint m_puppetClipComposeTexCoord = GL_NONE;
    glm::vec4 m_puppetClipRenderVar0 { 0.0f };
    glm::vec4 m_puppetClipClearColor { 0.0f };
    glm::mat4 m_puppetClipIdentity { 1.0f };
    float m_puppetClipComposeAlpha = 1.0f;
    /** mesh flag 2 blend map */
    struct {
	GLuint vertices = GL_NONE;
	GLuint indices = GL_NONE;
	GLsizei indexCount = 0;
	uint32_t format = 0;
	uint32_t stride = 0;
	/** BLENDROWCOUNT */
	uint32_t rows = 0;
	std::string material;
	std::shared_ptr<CFBO> albedo = nullptr;
	GLuint quad = GL_NONE;
	glm::mat4 projection { 1.0f };
	Effects::CPass* copyPass = nullptr;
	Effects::CPass* pass = nullptr;
    } m_blendMap;
    mutable bool m_puppetDrawDiagnosticLogged = false;
    mutable bool m_puppetDrawErrorChecked = false;
    bool m_puppetPositionDiagnosticLogged = false;
    mutable std::set<int> m_attachmentDiagnosticLogged = {};
    std::vector<GLfloat> m_puppetRawPositions = {};
    /** This object's current resolved scale, mirrored here so updatePuppetSkinning() (called after
     *  updateGeometryBuffers() each frame, see render()) can fold it into puppet vertex positions
     *  without needing resolveTransform() run twice */
    glm::vec3 m_puppetScale { 1.0f };
    std::vector<glm::uvec4> m_puppetBlendIndices = {};
    std::vector<glm::vec4> m_puppetBlendWeights = {};

    PuppetRig m_rig;
    std::vector<GLfloat> m_puppetSkinnedPositions = {};
    uint32_t m_puppetMeshFlags = 0;
    uint32_t m_puppetMeshFormat = 0;
    std::optional<PuppetMorphTargets> m_puppetMorph = std::nullopt;
    /** per vertex morph texel, nullopt if not morphed */
    std::vector<std::optional<uint32_t>> m_puppetMorphIndices = {};
    /** vertex alpha through SKINNING_ALPHA, see setupPuppetGeometryCallback */
    bool m_puppetVertexAlpha = false;
    GLuint m_puppetVertexAlphaWeights = GL_NONE;
    std::vector<GLfloat> m_puppetVertexAlphaData = {};

    glm::mat4 m_modelViewProjectionScreen = {};
    glm::mat4 m_modelViewProjectionPass = {};
    /** effect passes: their buffer's geometry mapped to where the layer is on screen */
    glm::mat4 m_effectModelViewProjectionCopy { 1.0f };
    /** WE's object world matrix (y up, scene pixels), shaders read g_LayerModelMatrix for the layer's scale */
    glm::mat4 m_layerModelMatrix { 1.0f };
    glm::mat4 m_effectModelViewProjectionPass { 1.0f };
    glm::mat4 m_modelViewProjectionCopy = {};
    glm::mat4 m_modelViewProjectionScreenInverse = {};
    glm::mat4 m_modelViewProjectionPassInverse = {};
    glm::mat4 m_modelViewProjectionCopyInverse = {};
    /** Maps the layer quad's own -1..1 space (+y = texture top) to screen clip space, effects like xray and
     *  cursorripple use its inverse to bring g_PointerPosition into the layer's texture space */
    glm::mat4 m_effectTextureProjection = glm::mat4 (1.0);
    glm::mat4 m_effectTextureProjectionInverse = glm::mat4 (1.0);
    glm::mat4 m_objectSpaceProjectionInverse = glm::mat4 (1.0);

    glm::mat4 m_modelMatrix = {};
    glm::mat4 m_viewProjectionMatrix = {};

    std::shared_ptr<const CFBO> m_mainFBO = nullptr;
    std::shared_ptr<const CFBO> m_subFBO = nullptr;
    std::shared_ptr<const CFBO> m_currentMainFBO = nullptr;
    std::shared_ptr<const CFBO> m_currentSubFBO = nullptr;

    const Image& m_image;
    mutable glm::vec3 m_colorCache {};
    mutable glm::vec4 m_color4Cache {};
    mutable float m_alphaCache = 1.0f;

    std::vector<Effects::CPass*> m_passes = {};
    std::vector<Effects::CPass*> m_allPasses = {};
    struct PassState {
	/** nullptr for passes that always render */
	const DynamicValue* visible;
	BlendingMode blending;
	bool fromEffect;
    };
    std::vector<PassState> m_allPassStates = {};
    std::vector<bool> m_activePassMask = {};
    bool m_hasActiveEffectPass = false;
    bool m_passesDrawToScreen = false;
    std::vector<MaterialPassUniquePtr> m_virtualPassess = {};

    glm::vec4 m_pos = {};
    /** The quad's size before the layer's scale, what m_pos spans */
    glm::vec2 m_displaySize = {};
    /** The object's origin in m_pos's space, what the rotation turns around: WE puts the alignment offset inside
     *  the rotated and scaled frame (sub_1401FD3F0), not around it */
    glm::vec3 m_scenePivot = {};
    std::optional<TextureAnimation> m_textureAnimation = std::nullopt;
    float m_textureAnimationClock = -1.0f;
    /** Lit passes: scene/copy space vertices -> WE world (y up, bottom left origin), see CPass::setLightingTransform */
    glm::mat4 m_lightingSceneModel { 1.0f };
    glm::mat4 m_lightingCopyModel { 1.0f };
    glm::mat3 m_lightingNormal { 1.0f };
    glm::mat4 m_lightingViewProjection { 1.0f };
    glm::vec2 m_size = {};

    // last m_pos/size uploaded to the GL geometry buffers; NaN forces the first upload
    glm::vec4 m_lastUploadedPos = glm::vec4 (std::numeric_limits<float>::quiet_NaN ());
    glm::vec2 m_lastUploadedGeometrySize = glm::vec2 (std::numeric_limits<float>::quiet_NaN ());

    bool m_initialized = false;
    bool m_isDependency = false;
    bool m_readByOtherLayer = false;
    /** A passthrough layer with objects under it, it draws them into its buffer after the base pass */
    bool m_hasPassthroughChildren = false;

    struct {
	struct {
	    MaterialUniquePtr material;
	    ImageEffectPassOverrideUniquePtr override;
	} colorBlending;
	std::vector<MaterialUniquePtr> compatibilityMaterials = {};
	std::vector<ImageEffectPassOverrideUniquePtr> compatibilityOverrides = {};
	MaterialUniquePtr clippingMask;
	MaterialUniquePtr clippingCompose;
	std::vector<ImageEffectPassOverrideUniquePtr> clippingOverrides = {};
	ImageEffectPassOverrideUniquePtr puppetVertexAlpha;
	MaterialUniquePtr blendMap;
	ImageEffectPassOverrideUniquePtr blendMapOverride;
    } m_materials;
};
} // namespace WallpaperEngine::Render::Objects
