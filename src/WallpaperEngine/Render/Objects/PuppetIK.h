#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

namespace WallpaperEngine::Render::Objects {
/** A bone's IK JSON settings (2.8.42 sub_140265C30) */
struct PuppetBoneIK {
    enum Flags : uint32_t {
	Gravity = 0x4,
	Grounded = 0x20,
	RotateToTarget = 0x40,
	SolveEnd = 0x80,
	FollowParent = 0x100,
	Enabled = 0x4000,
	Constrained = 0x8000,
    };

    uint32_t flags = 0;
    int depth = 2;
    float rotateDistance = 50.0f;
    /** "ikrminl"/"ikrmaxl" in radians, "ikce" joints only */
    float minAngle = 0.0f;
    float maxAngle = 0.0f;
    /** "m" gravity scale, "tf" friction */
    float mass = 1000.0f;
    float friction = 0.0f;

    static PuppetBoneIK parse (const std::string& json);
};

/** Scene gravity and wind (sub_1401988E0, sub_140198910) */
struct PuppetRopeEnvironment {
    glm::vec3 gravity = glm::vec3 (0.0f, -1.0f, 0.0f);
    bool wind = false;
    glm::vec3 windDirection = glm::vec3 (0.0f);
    float windStrength = 0.0f;
    /** wind noise runs over (clock, x / width * 10) */
    float clock = 0.0f;
    float width = 0.0f;

    [[nodiscard]] bool windAt (const glm::vec3& position, glm::vec3& force) const;
};

/** P+952, per bone rope joint in world space */
struct PuppetRopeJoint {
    glm::vec3 current = glm::vec3 (0.0f);
    glm::vec3 previous = glm::vec3 (0.0f);
    bool started = false;
};

/** MDLS v4 chain entry: the bones from the link bone to the end bone */
struct PuppetIKEntry {
    uint32_t endBone = 0;
    /** 1 targeted, 2 always bend, 4 rope */
    uint32_t flags = 0;
    float length = 0.0f;
    float minReach = 0.0f;
    std::vector<int> bones;
};

struct PuppetIKLink {
    uint32_t bone = 0;
    std::vector<PuppetIKEntry> entries;
};

/** puppet +272 */
struct PuppetIKChain {
    uint32_t root = 0;
    /** chain +32 */
    std::vector<uint32_t> extras;
    std::vector<PuppetIKLink> links;
};

/** MDLS constraint. Flags: 1 "origin", 2 "axis", 0x10000 "chainalignaxis" */
struct PuppetConstraintRule {
    uint32_t flags = 0;
    uint32_t weight = 0;
    uint32_t target = 0;
    /** flags & 2 only */
    float start = 0.0f;
    float range = 0.00000011920929f;
};

/** MDLS v4 IK data */
struct PuppetIKRig {
    std::vector<float> lengths;
    /** per bone: child -> rest direction */
    std::vector<std::unordered_map<uint32_t, glm::vec3>> childDirections;
    std::unordered_map<uint32_t, uint32_t> targets;
    std::unordered_map<uint32_t, uint32_t> poles;
    std::vector<PuppetIKChain> chains;
    /** extra +96, +104 */
    std::vector<uint32_t> extraBones;
    std::vector<std::vector<PuppetConstraintRule>> extraRules;
};

/** Blends via WE's euler angles, row lengths and position */
glm::mat4 blendPuppetTransform (const glm::mat4& from, const glm::mat4& to, float weight);

/** sub_14026F4C0 */
void applyPuppetChainConstraints (
    const PuppetIKRig& rig, const PuppetIKChain& chain, const std::vector<float>& weights,
    std::vector<glm::mat4>& model, std::vector<glm::mat4>& extras
);

/** sub_14026DF30: clamps an "ikce" joint's x/y direction to its limits */
void limitPuppetJointDirection (
    const PuppetBoneIK& settings, const glm::vec3& joint, const glm::vec3& parent, glm::vec3& direction
);

/** sub_14026EB60: Verlet rope step in world space, end pinned unless "ikfe" */
void stepPuppetRopeChain (
    const PuppetIKRig& rig, const PuppetIKChain& chain, const std::vector<PuppetBoneIK>& ik,
    std::vector<glm::mat4>& model, const std::vector<glm::mat4>& extras, const glm::mat4& world,
    std::vector<PuppetRopeJoint>& joints, const PuppetRopeEnvironment& environment, float dt
);

/** sub_140271910: up to 10 FABRIK passes, then joints rotate from bind pose onto the solved directions */
void solvePuppetIKChain (
    const PuppetIKRig& rig, const PuppetIKChain& chain, const std::vector<int>& parents,
    const std::vector<PuppetBoneIK>& ik, std::vector<glm::mat4>& model, const std::vector<glm::mat4>& extras,
    const std::vector<glm::mat4>& bindModel, const glm::mat4& world, std::vector<PuppetRopeJoint>& joints
);
} // namespace WallpaperEngine::Render::Objects
