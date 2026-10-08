#pragma once

#include "PuppetIK.h"
#include "PuppetPhysics.h"
#include "PuppetRootMotion.h"

#include "WallpaperEngine/Data/JSON.h"
#include "WallpaperEngine/Data/Model/Object.h"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace WallpaperEngine::Data::Model {
struct Project;
}

namespace WallpaperEngine::Render::Objects {
/** A skeleton bone, parsed from the MDLS section of a .mdl */
struct PuppetBone {
    std::string name;
    int parent = -1;
    /** Local bind-pose transform, relative to the parent bone (identity for a root bone's "world" reference) */
    glm::mat4 bindLocal { 1.0f };
    /** Local rest pose, where animation and physics start from: MDLS's optional second matrix array, else bindLocal */
    glm::mat4 restLocal { 1.0f };
    /** Inverse of the bone's bind-pose world transform, derived by walking the parent chain */
    glm::mat4 inverseBindWorld { 1.0f };
    PuppetBonePhysics physics {};
    /** MDLS type, bit 2: posed by the IK solver */
    uint32_t type = 0;
    PuppetBoneIK ik {};
    /** blend rules (bone +216) */
    std::vector<PuppetConstraintRule> rules;
    /** IK chain this bone starts (bone +212) */
    int chain = -1;
    /** The bone's collision capsule (MDLS, what collisionmodel particles hit): extents, and its frame in bone space */
    bool hasCapsule = false;
    glm::vec3 capsuleExtents { 0.0f };
    glm::mat4 capsule { 1.0f };
};

/** MDLS v2+ extra: type 0 IK target, 1 pole */
struct PuppetExtra {
    uint32_t bone = 0;
    uint32_t type = 0;
    glm::mat4 restLocal { 1.0f };
};

/** A single sampled TRS pose for one bone at one point in time, from the MDLA section */
struct PuppetKeyframe {
    glm::vec3 position {};
    glm::vec3 rotation {};
    glm::vec3 scale { 1.0f };
    /** rotation as WE blends it, Rz * Ry * Rx */
    glm::quat orientation { 1.0f, 0.0f, 0.0f, 0.0f };
};

/** MDLA clip event, time in seconds (sub_1401A9F60) */
struct PuppetClipEvent {
    float time = 0.0f;
    std::string payload;
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
    /** per bone, the raw track flags; the float tracks below skip a bone when (flags & 3) == 1 */
    std::vector<uint32_t> boneFlags;
    /** MDLA v2, [extra][sample] */
    std::vector<std::vector<PuppetKeyframe>> extraTracks;
    std::vector<bool> extraAnimated;
    /** MDLA v6, [bone][sample]: what gets added to the parts' draw order of that bone (mesh flag 8) */
    std::vector<std::vector<float>> drawOrderTracks;
    /** MDLA v3, [bone][sample]: g_BonesAlpha */
    std::vector<std::vector<float>> boneAlphaTracks;
    /** MDLA v2, [constraint][sample]: blend rule weights, flags bit 0 skips */
    std::vector<std::vector<float>> constraintTracks;
    std::vector<uint32_t> constraintFlags;
    /** MDLA v3, [track][sample]: g_BlendMap */
    std::vector<std::vector<float>> blendTracks;
    /** clip +288 */
    std::vector<PuppetClipEvent> events;
    /** MDLA v5 clip box */
    glm::vec3 boundsMin = glm::vec3 (0.0f);
    glm::vec3 boundsMax = glm::vec3 (0.0f);

    /** a morph target's weight over the clip, one sample per frame like the bone tracks */
    struct MorphTrack {
	uint16_t target = 0;
	std::vector<float> samples;
    };
    /** MDLA v4+, per mesh of the model: whether the clip drives its morph weights and the tracks doing it */
    struct MeshMorphTracks {
	bool enabled = false;
	std::vector<MorphTrack> tracks;
    };
    std::vector<MeshMorphTracks> morphTracks;
    /** the clip's flags: 1 has the record below, 0x400 the record refers to this clip itself, 0x800/0x1000/0x2000
     *  root motion along x/y/z, 0x8000 root rotation (0x1F800 any root motion) */
    uint32_t flags = 0;

    /** flags & 1 (MDLA, sub_140261880): models play another clip's frames from frameStart on (sub_14021C480), and
     *  root motion measures the root bone against two of its frames */
    struct RootMotion {
	uint16_t clip = 0;
	uint32_t frameStart = 0;
	uint32_t frameEnd = 0;
	uint32_t startOffset = 0;
	int bone = -1;
	/** with flags & 0x1F800 */
	PuppetRootMotionFrames frames;
    };
    std::optional<RootMotion> rootMotion;
};

/** One mesh's morph target weights this frame (WE model state +96: a bit per active target and the weights) */
struct PuppetMorphWeights {
    uint64_t active = 0;
    std::vector<float> weights;
};

/** A named point on a puppet's rig that other objects can follow via scene.json's "attachment" field */
struct PuppetAttachmentPoint {
    std::string name;
    int boneIndex = -1;
    /** Transform of the point relative to its bone, in the same convention as PuppetBone::bindLocal */
    glm::mat4 localTransform { 1.0f };
};

/** One of an object's animationlayers[] entries, paired with the baked clip it plays and its own clock (WE's layer
 *  timeline, sub_1401A8C10 / sub_1401A9F60) */
struct PuppetActiveAnimation {
    PuppetAnimationClip clip;
    const WallpaperEngine::Data::Model::ImageAnimationLayer* layer = nullptr;
    /** layers made by createAnimationLayer()/playSingleAnimation() own their settings */
    WallpaperEngine::Data::Model::ImageAnimationLayerUniquePtr ownedLayer = nullptr;
    /** stable handle for scripts: the index in the object's animationlayers[] for scene layers, counted on from
     *  there for created ones */
    size_t serial = 0;
    /** fade flags (+208 bits 4 and 8), blendin drops once a non-single clip has faded in */
    bool blendIn = false;
    bool blendOut = false;
    /** playSingleAnimation(): removed once it ends (0x8000000) */
    bool autoRemove = false;
    /** seconds into the clip */
    float time = 0.0f;
    /** 1 mirror, 2 single, 0x2000000 frame set by a script, 0x20000000 paused, 0x40000000 stopped, sign bit mirror
     *  playing backwards */
    uint32_t flags = 0;
    /** reached its end in this frame's update, for IAnimationLayer.addEndedCallback() */
    bool ended = false;
    PuppetRootMotionState rootMotion;
};

/** What root motion moves: a model object (sub_140225900 writes its origin +296 and angles +320) */
class PuppetRootMotionHost {
public:
    virtual ~PuppetRootMotionHost () = default;
    /** "rootmotion" (model +784) */
    [[nodiscard]] virtual bool rootMotionEnabled () const = 0;
    /** the object's current world matrix (vtable slot 16), after any move of this frame */
    [[nodiscard]] virtual glm::mat4 rootMotionWorld () const = 0;
    virtual void rootMotionMove (const glm::vec3& origin) = 0;
    [[nodiscard]] virtual glm::vec3 rootMotionAngles () const = 0;
    virtual void rootMotionTurn (const glm::vec3& angles) = 0;
};

/** A visible layer's place in its clip this frame: the two frames around it and how far between them */
struct PuppetLayerSample {
    const PuppetAnimationClip* clip;
    uint32_t frame0;
    uint32_t frame1;
    float alpha;
    float weight;
    bool additive;
};

/** A bone in model space at a point of a clip, how WE's clip sampling builds it (sub_140267F00) */
[[nodiscard]] glm::mat4 samplePuppetBoneChain (
    const std::vector<PuppetBone>& bones, const PuppetAnimationClip& clip, int bone, uint32_t frame0, uint32_t frame1,
    float alpha
);

/**
 * The skeleton, animation layers and bone physics of a puppet image or a 3D model. Both run the same pose update in
 * wallpaper64.exe 2.8.42 (images sub_1401FDF90, models sub_14021C480): every bone starts at its rest pose, visible
 * layers blend their clips in, physics runs on the bones' scene transforms
 */
class PuppetRig {
public:
    /** Reads MDLS at mdlsOffset and the MDAT/MDLA sections it leads to (sub_140261880). Throws on a broken file */
    void load (const std::vector<char>& data, size_t mdlsOffset, uint32_t meshCount, const std::string& name);
    void clear ();

    [[nodiscard]] PuppetActiveAnimation* findLayer (size_t serial);
    /** sub_1401FCC20: plays the clip whose id is the layer's "animation", false without one */
    bool addLayer (
	const Data::Model::ImageAnimationLayer& layer, Data::Model::ImageAnimationLayerUniquePtr owned, size_t serial,
	bool autoRemove
    );
    /** The object's animationlayers[], serials are their indices */
    void addSceneLayers (const std::vector<Data::Model::ImageAnimationLayerUniquePtr>& layers);
    [[nodiscard]] size_t getLayerCount () const;
    [[nodiscard]] std::optional<size_t> getLayerAt (int64_t index) const;
    [[nodiscard]] std::optional<size_t> findLayerByName (const std::string& name) const;
    std::optional<size_t> createLayer (
	const Data::JSON::JSON& animation, const Data::JSON::JSON& config, bool autoRemove,
	const Data::Model::Project& project
    );
    bool destroyLayersByName (const std::string& name);
    bool destroyLayer (size_t serial);
    /** Created layers removed since the last call, alive until their property scripts are dropped */
    std::vector<PuppetActiveAnimation> takeRemovedLayers ();
    /** Clip events fired this frame, in layer order */
    [[nodiscard]] std::vector<std::string> takeFiredEvents ();
    /** After the pose: every layer that ended runs dispatch (its ended callbacks), playSingleAnimation() ones go */
    void finishEndedLayers (const std::function<void (size_t)>& dispatch);

    /**
     * Steps the layer clocks and builds this frame's pose, objectWorld is the object's world matrix. Models pass
     * themselves as host: their clips can play another clip's frames and move the object by root motion
     */
    void updatePose (const glm::mat4& objectWorld, PuppetRootMotionHost* host = nullptr);
    void updateMorphWeights (const std::vector<PuppetLayerSample>& samples);
    /** sub_1401FDF90 for meshes with flag 8: every bone's draw order from MDLS, moved by the layers' v6 tracks */
    void updateDrawOrder (const std::vector<PuppetLayerSample>& samples);
    /** sub_1401FDF90, mesh flag 4 */
    void updateBoneAlpha (const std::vector<PuppetLayerSample>& samples);
    /** sub_1401FDF90, mesh flag 2 */
    void updateBlendMap (const std::vector<PuppetLayerSample>& samples);
    /** sub_1401FDF90 */
    void updateConstraintWeights (const std::vector<PuppetLayerSample>& samples);
    /** sub_140225900 for one model layer after its blend, rest is what the layers above leave of it */
    void applyRootMotion (
	PuppetActiveAnimation& layer, const PuppetLayerSample& sample, std::vector<glm::vec3>& positions,
	std::vector<glm::quat>& orientations, float rest, PuppetRootMotionHost& host
    );
    void
    composePose (const std::vector<int>& parents, const std::vector<glm::mat4>& locals, const glm::mat4& objectWorld);

    [[nodiscard]] bool hasPose () const;
    [[nodiscard]] int findBone (const std::string& name) const;
    /** First attachment point of that name, -1 without one (image/model vtable slot 14, sub_1401FD510/sub_1402248C0) */
    [[nodiscard]] int findAttachment (const std::string& name) const;
    /** An attachment point in model space, its bone's model matrix times the point (slot 15, sub_1401FD5C0/
     *  sub_140224970); nothing for an index out of range or before the bones exist */
    [[nodiscard]] std::optional<glm::mat4> attachmentMatrix (int index) const;
    [[nodiscard]] const glm::mat4& getBoneTransform (int bone) const;
    /** sub_1401FD690: first bone box in reverse hit order, name or index */
    [[nodiscard]] std::optional<std::string> imageHitBox (const glm::vec3& origin, const glm::vec3& direction) const;
    /** sub_140223810: the box the line enters first, local gets the entry point in its space */
    [[nodiscard]] std::optional<std::string> modelHitBox (
	const glm::vec3& origin, const glm::vec3& direction, const glm::mat4& objectWorld, glm::vec3& local
    ) const;
    [[nodiscard]] std::string hitBoxName (int bone) const;
    void setBoneTransform (int bone, const glm::mat4& transform, const glm::mat4& objectWorld);
    [[nodiscard]] const glm::mat4& getLocalBoneTransform (int bone) const;
    void setLocalBoneTransform (int bone, const glm::mat4& transform, const glm::mat4& objectWorld);
    void applyBonePhysicsImpulse (int bone, const glm::vec3& directional, const glm::vec3& angularDegrees);
    void resetBonePhysics (int bone);
    /** model space bone matrices times the inverse bind ones, what the vertices are skinned with */
    [[nodiscard]] std::vector<glm::mat4> skinMatrices () const;
    /** sub_140226A10: union of the visible layers' MDLA v5 boxes, false when none has one */
    [[nodiscard]] bool clipBounds (glm::vec3& min, glm::vec3& max) const;
    /** Where the file's MDMP section (morph targets) starts, 0 without one */
    [[nodiscard]] size_t getMorphSection () const { return this->morphSection; }

    std::vector<PuppetBone> bones = {};
    std::vector<PuppetAnimationClip> clips = {};
    std::vector<PuppetAttachmentPoint> attachmentPoints = {};
    std::vector<PuppetActiveAnimation> layers = {};
    /** per mesh, rebuilt by every updatePose () (sub_14021C480) */
    std::vector<PuppetMorphWeights> morphWeights = {};
    /** models clamp blended layer weights to 0..1, images don't */
    bool morphBlendClamped = true;
    /** mesh flag 4, instance +616 */
    bool boneAlphaEnabled = false;
    std::vector<float> boneAlpha = {};
    /** instance +848, four g_BlendMap rows */
    std::array<float, 16> blendMap = {};
    size_t morphSection = 0;
    /** MDLS v3+ per bone order (instance +496), added to the order of every part of that bone */
    std::vector<int> boneDrawOrder = {};
    /** walked from the end */
    std::vector<int> boneHitOrder = {};
    /** set by the image for a mesh with flag 8: the pose update then keeps drawOrder (instance +640) */
    bool drawOrderEnabled = false;
    std::vector<float> drawOrder = {};
    /** a layer wrote drawOrder this frame, the parts get sorted again */
    bool drawOrderTouched = false;
    std::vector<PuppetExtra> extras = {};
    PuppetIKRig ik = {};
    /** bind pose in model space (P+808) */
    std::vector<glm::mat4> bindModel = {};
    /** MDLE0002 local bind matrices */
    std::vector<glm::mat4> layerImageBindLocal = {};
    /** sub_1401D6D30: MDLE0002 locals if any, else MDLS's */
    [[nodiscard]] std::vector<glm::mat4> layerImageBindModel () const;
    /** P+736 */
    std::vector<glm::mat4> extraModel = {};
    bool hasIK = false;
    /** the bones in model space (WE images P+712), starts at the bind pose */
    std::vector<glm::mat4> boneModel = {};
    /** this frame's local matrices (P+784) and scene matrices (P+832), empty until the first update */
    std::vector<glm::mat4> boneLocal = {};
    std::vector<glm::mat4> boneScene = {};
    /** Bone physics state and last frame's scene transforms, empty until the first frame */
    std::vector<PuppetBonePhysicsState> physicsState = {};
    std::vector<glm::mat4> physicsPreviousScene = {};
    /** P+952 */
    std::vector<PuppetRopeJoint> ropeJoints = {};
    /** puppet +592 */
    std::vector<float> constraintWeights = {};
    PuppetRopeEnvironment ropeEnvironment = {};
    bool hasPhysics = false;
    /** The rest pose isn't the bind pose, the mesh needs skinning even when nothing moves */
    bool hasRestPose = false;
    size_t nextLayerSerial = 0;
    size_t removeLayers (const std::function<bool (const PuppetActiveAnimation&)>& predicate);
    std::vector<PuppetActiveAnimation> removedLayers = {};
    std::vector<std::string> firedEvents = {};
    /** g_Time of the last clock step, so a scene drawn on several outputs steps once per frame */
    float clockTime = -1.0f;
    /** a script wrote bone matrices, the mesh has to be skinned from then on */
    bool poseScripted = false;
    /** the pose differs from the bind pose (animation, physics or scripts) */
    bool poseAnimated = false;
};
} // namespace WallpaperEngine::Render::Objects
