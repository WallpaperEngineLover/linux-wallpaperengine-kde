#pragma once

#include <cstdint>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace WallpaperEngine::Render::Objects {
/** The root bone of a clip with root motion (MDLA flags & 1 and & 0x1F800) at its first and last frame */
struct PuppetRootMotionFrames {
    glm::mat4 first { 1.0f };
    glm::mat4 last { 1.0f };
    /** inverse of the first frame's rotation */
    glm::mat3 inverseFirst { 1.0f };
};

/** One layer's root motion state (WE layer +208 sign bit, +328 and +332) */
struct PuppetRootMotionState {
    /** it ran last frame */
    bool started = false;
    /** the layer clock then */
    float time = 0.0f;
    /** the root bone then */
    glm::mat4 previous { 1.0f };
};

/** How one step moves the object: an offset for its origin and a turn about the world y axis */
struct PuppetRootMotionStep {
    bool moves = false;
    glm::vec3 offset { 0.0f };
    bool turns = false;
    float yaw = 0.0f;
};

/**
 * One layer's root motion (wallpaper64.exe 2.8.42 sub_140225900, models only): the root bone's movement since the
 * last frame goes to the object, measured against the clip's first frame and turned into the object's frame, and the
 * blended root bone is pulled back to where the first frame has it. flags are the clip's: 0x800/0x1000/0x2000 move
 * along x/y/z, 0x8000 turns. current is the root bone this frame, start what the layer starts from (only read when
 * state.started is false), time the layer clock, weight the layer's blend weight, rest what the layers above leave of
 * it and world the object's world matrix
 */
[[nodiscard]] PuppetRootMotionStep stepPuppetRootMotion (
    uint32_t flags, const PuppetRootMotionFrames& frames, const glm::mat4& current, const glm::mat4& start, float time,
    float weight, float rest, const glm::mat3& world, PuppetRootMotionState& state, glm::vec3& rootPosition,
    glm::quat& rootOrientation
);

/**
 * The object's angles turned by yaw about the world y axis: added to the y angle (wrapped to one turn) while x and z
 * are about zero, otherwise through the rotation matrix (sub_140215020, sub_1401E2500, sub_1401E23D0)
 */
[[nodiscard]] glm::vec3 turnPuppetRootMotion (const glm::vec3& angles, float yaw);
} // namespace WallpaperEngine::Render::Objects
