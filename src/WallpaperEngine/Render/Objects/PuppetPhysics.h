#pragma once

#include <cstdint>
#include <string>

#include <glm/glm.hpp>

namespace WallpaperEngine::Render::Objects {
/**
 * Jiggle physics of one puppet bone, read from the JSON after its MDLS record the way wallpaper64.exe 2.8.42
 * does (sub_140265C30). Angles are radians, stiffness/friction per second, rest target in bone space.
 */
struct PuppetBonePhysics {
    enum Flags : uint32_t {
	Simulate = 0x1,
	SimulateRigid = 0x2,
	Gravity = 0x4,
	AngleLimits = 0x8,
	TotalAngleLimit = 0x10,
	Rotation = 0x1000,
	Translation = 0x2000,
	InverseKinematics = 0x4000,
	LockRotationX = 0x100000,
	LockRotationY = 0x200000,
	LockRotationZ = 0x400000,
	LockTranslationX = 0x800000,
	LockTranslationY = 0x1000000,
	LockTranslationZ = 0x2000000,
    };

    uint32_t flags = 0;
    float rotationStiffness = 0.0f;
    float translationStiffness = 0.0f;
    float rotationFriction = 0.0f;
    float translationFriction = 0.0f;
    // stored as 1 - inertia / 100 like WE
    float rotationInertia = 1.0f;
    float translationInertia = 1.0f;
    glm::vec3 gravityDirection { 0.0f };
    float mass = 1000.0f;
    glm::vec3 target { 0.0f };
    float maxTranslation = 0.0f;
    glm::vec3 angleMin { 0.0f };
    glm::vec3 angleMax { 0.0f };
    float maxAngle = 0.0f;

    [[nodiscard]] bool simulated () const { return (flags & (Rotation | Translation)) != 0; }

    static PuppetBonePhysics parse (const std::string& json);
};

struct PuppetBonePhysicsState {
    glm::vec4 angularVelocity { 1.0f, 0.0f, 0.0f, 0.0f };
    glm::vec3 angles { 0.0f };
    glm::vec3 offset { 0.0f };
    glm::vec3 velocity { 0.0f };
};

// one frame of a bone's physics (sub_1401FDF90): world is this frame's scene transform before its physics,
// previousWorld last frame's after; returns what goes in front of the model-space transform
glm::mat4 stepPuppetBonePhysics (
    const PuppetBonePhysics& physics, PuppetBonePhysicsState& state, const glm::mat4& world,
    const glm::mat4& previousWorld, float dt, float objectScale
);

/** Quaternion (w, x, y, z) helpers shared with the IK solver: sub_140216070, sub_1402167C0, sub_140216280 */
glm::vec4 puppetSlerp (const glm::vec4& a, const glm::vec4& b, float t);
glm::vec4 puppetRotationArc (const glm::vec3& from, const glm::vec3& to);
glm::mat3 puppetQuatRows (const glm::vec4& q);

// applyBonePhysicsImpulse (sub_140210990): the directional impulse adds to the translation velocity, the angular one
// (degrees) multiplies onto the angular velocity
void applyPuppetBoneImpulse (
    PuppetBonePhysicsState& state, const glm::vec3& directional, const glm::vec3& angularDegrees
);
}
