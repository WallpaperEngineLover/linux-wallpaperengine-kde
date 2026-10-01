#pragma once

#include "PuppetPhysics.h"

#include "WallpaperEngine/Data/JSON.h"
#include "WallpaperEngine/Data/Model/Object.h"

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
    /** Inverse of the bone's bind-pose world transform, derived by walking the parent chain */
    glm::mat4 inverseBindWorld { 1.0f };
    PuppetBonePhysics physics {};
    /** The bone's collision capsule (MDLS, what collisionmodel particles hit): extents, and its frame in bone space */
    bool hasCapsule = false;
    glm::vec3 capsuleExtents { 0.0f };
    glm::mat4 capsule { 1.0f };
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
    /** After the pose: every layer that ended runs dispatch (its ended callbacks), playSingleAnimation() ones go */
    void finishEndedLayers (const std::function<void (size_t)>& dispatch);

    /** Steps the layer clocks and builds this frame's pose, objectWorld is the object's world matrix */
    void updatePose (const glm::mat4& objectWorld);
    void updateMorphWeights (const std::vector<PuppetLayerSample>& samples);
    void
    composePose (const std::vector<int>& parents, const std::vector<glm::mat4>& locals, const glm::mat4& objectWorld);

    [[nodiscard]] bool hasPose () const;
    [[nodiscard]] int findBone (const std::string& name) const;
    [[nodiscard]] const glm::mat4& getBoneTransform (int bone) const;
    void setBoneTransform (int bone, const glm::mat4& transform, const glm::mat4& objectWorld);
    [[nodiscard]] const glm::mat4& getLocalBoneTransform (int bone) const;
    void setLocalBoneTransform (int bone, const glm::mat4& transform, const glm::mat4& objectWorld);
    void applyBonePhysicsImpulse (int bone, const glm::vec3& directional, const glm::vec3& angularDegrees);
    void resetBonePhysics (int bone);
    /** model space bone matrices times the inverse bind ones, what the vertices are skinned with */
    [[nodiscard]] std::vector<glm::mat4> skinMatrices () const;
    /** Where the file's MDMP section (morph targets) starts, 0 without one */
    [[nodiscard]] size_t getMorphSection () const { return this->morphSection; }

    std::vector<PuppetBone> bones = {};
    std::vector<PuppetAnimationClip> clips = {};
    std::vector<PuppetAttachmentPoint> attachmentPoints = {};
    std::vector<PuppetActiveAnimation> layers = {};
    /** per mesh, rebuilt by every updatePose () (sub_14021C480) */
    std::vector<PuppetMorphWeights> morphWeights = {};
    size_t morphSection = 0;
    /** the bones in model space (WE images P+712), starts at the bind pose */
    std::vector<glm::mat4> boneModel = {};
    /** this frame's local matrices (P+784) and scene matrices (P+832), empty until the first update */
    std::vector<glm::mat4> boneLocal = {};
    std::vector<glm::mat4> boneScene = {};
    /** Bone physics state and last frame's scene transforms, empty until the first frame */
    std::vector<PuppetBonePhysicsState> physicsState = {};
    std::vector<glm::mat4> physicsPreviousScene = {};
    bool hasPhysics = false;
    size_t nextLayerSerial = 0;
    /** g_Time of the last clock step, so a scene drawn on several outputs steps once per frame */
    float clockTime = -1.0f;
    /** a script wrote bone matrices, the mesh has to be skinned from then on */
    bool poseScripted = false;
    /** the pose differs from the bind pose (animation, physics or scripts) */
    bool poseAnimated = false;
};
} // namespace WallpaperEngine::Render::Objects
