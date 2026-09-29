#pragma once

#include "CRenderable.h"
#include "WallpaperEngine/Render/CObject.h"
#include "WallpaperEngine/Render/Objects/Effects/CPass.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"

#include "WallpaperEngine/Render/Shaders/Shader.h"

#include "../TextureProvider.h"
#include "PuppetPhysics.h"
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
/** A puppet skeleton bone, parsed from the MDLS section of the puppet .mdl */
struct PuppetBone {
    int parent = -1;
    /** Local bind-pose transform, relative to the parent bone (identity for a root bone's "world" reference) */
    glm::mat4 bindLocal { 1.0f };
    /** Inverse of the bone's bind-pose world transform, derived by walking the parent chain */
    glm::mat4 inverseBindWorld { 1.0f };
    PuppetBonePhysics physics {};
};

/** A single sampled TRS pose for one bone at one point in time, from the MDLA section */
struct PuppetKeyframe {
    glm::vec3 position {};
    glm::vec3 rotation {};
    glm::vec3 scale { 1.0f };
    /** rotation as WE blends it, Rz * Ry * Rx */
    glm::quat orientation { 1.0f, 0.0f, 0.0f, 0.0f };
};

/** A baked animation clip: one keyframe track per bone, sampled at a fixed rate */
struct PuppetAnimationClip {
    /** what animationlayers[].animation refers to */
    uint64_t id = 0;
    std::string name;
    std::string mode;
    float fps = 30.0f;
    uint32_t frameCount = 0;
    /** [boneIndex][sampleIndex], each track has frameCount+1 samples */
    std::vector<std::vector<PuppetKeyframe>> boneTracks;
    /** per bone, false when its track flags have bit 0 set: the clip leaves that bone alone */
    std::vector<bool> boneAnimated;
};

/** A named point on a puppet's rig that other objects can follow via scene.json's "attachment" field */
struct PuppetAttachmentPoint {
    std::string name;
    int boneIndex = -1;
    /** Transform of the point relative to its bone, in the same convention as PuppetBone::bindLocal */
    glm::mat4 localTransform { 1.0f };
};

/** One of a puppet's animationlayers[] entries, paired with the baked clip it plays */
struct PuppetActiveAnimation {
    PuppetAnimationClip clip;
    const WallpaperEngine::Data::Model::ImageAnimationLayer* layer = nullptr;
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
	/** the same attachment point's rotation in the rig's bind pose */
	float restAngle;
    };

    /**
     * @param name A named attachment point on this puppet's rig (see PuppetAttachmentPoint)
     * @return The point's current animated transform, or nullopt if there's no such point (or no puppet mesh)
     */
    [[nodiscard]] std::optional<AttachmentPointTransform>
    getAttachmentPointMeshTransform (const std::string& name) const;

protected:
    void setupPasses ();
    void rebuildActivePasses ();
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
	/** part of `angle` that should pivot around the puppet mesh's own center instead of the object origin */
	float meshPivotAngle = 0.0f;
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
    [[nodiscard]] std::vector<glm::mat4>
    composeBoneWorldTransformsWithPhysics (const std::vector<int>& parents, const std::vector<glm::mat4>& locals);
    /** Recomputes puppet vertex positions for the current animation time and re-uploads them */
    void updatePuppetSkinning ();
    void setupPuppetGeometryCallback (Effects::CPass* pass) const;
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

    std::vector<PuppetBone> m_puppetBones = {};
    /**
     * Every animationlayers[] entry that matched a baked clip. Wallpaper Engine puppets almost always
     * declare several "additive" layers (idle sway, blinking, hand movement, ...) that all play at once
     * on top of each other rather than one layer replacing another - see updatePuppetSkinning for how
     * they're composed.
     */
    std::vector<PuppetActiveAnimation> m_puppetActiveAnimations = {};
    std::vector<GLfloat> m_puppetSkinnedPositions = {};

    std::vector<PuppetAttachmentPoint> m_puppetAttachmentPoints = {};
    /** Per-bone current animated world transform, in the puppet's own local mesh space; starts out equal
     *  to the bind pose and is refreshed every frame by updatePuppetSkinning while animation is active */
    std::vector<glm::mat4> m_puppetBoneWorldAnimated = {};
    /** Bone physics state and last frame's scene transforms of the simulated bones, empty until the first frame */
    std::vector<PuppetBonePhysicsState> m_puppetPhysicsState = {};
    std::vector<glm::mat4> m_puppetPhysicsPreviousWorld = {};
    bool m_puppetHasPhysics = false;

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
    glm::vec3 m_sceneCenter = {};
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
    } m_materials;
};
} // namespace WallpaperEngine::Render::Objects
