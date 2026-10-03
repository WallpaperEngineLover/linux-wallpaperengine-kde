#include "PuppetRootMotion.h"

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>
#undef GLM_ENABLE_EXPERIMENTAL

using namespace WallpaperEngine::Render::Objects;

namespace {
// a turn about y only: the third column flattened onto the ground as forward, y up, right = up x forward
glm::mat3 yawFrame (const glm::mat3& rotation) {
    const glm::vec3& column = rotation[2];
    const float inverseLength = 1.0f / std::sqrt (column.x * column.x + 0.0f + column.z * column.z);
    const glm::vec3 forward (column.x * inverseLength, inverseLength * 0.0f, column.z * inverseLength);

    return {
	glm::vec3 (forward.z - forward.y * 0.0f, forward.x * 0.0f - forward.z * 0.0f, forward.y * 0.0f - forward.x),
	glm::vec3 (0.0f, 1.0f, 0.0f), forward
    };
}
} // namespace

PuppetRootMotionStep WallpaperEngine::Render::Objects::stepPuppetRootMotion (
    const uint32_t flags, const PuppetRootMotionFrames& frames, const glm::mat4& current, const glm::mat4& start,
    const float time, const float weight, const float rest, const glm::mat3& world, PuppetRootMotionState& state,
    glm::vec3& rootPosition, glm::quat& rootOrientation
) {
    PuppetRootMotionStep step;
    const glm::vec3 translation (current[3]);
    const glm::vec3 firstTranslation (frames.first[3]);

    // the root's turn since the first frame, and what undoes it. WE multiplies the first frame by its own inverse here
    // (r8 still holds it from the call before), which leaves about the identity
    const glm::mat3 currentYaw = yawFrame (glm::mat3 (current) * frames.inverseFirst);
    const glm::mat3 firstYaw = yawFrame (glm::mat3 (frames.first) * frames.inverseFirst);
    const glm::mat3 correction = firstYaw * glm::inverse (currentYaw);

    // the blended root bone goes back to the first frame on the moving axes
    if ((flags & 0x800) != 0) {
	rootPosition.x += (firstTranslation.x - translation.x) * weight;
    }

    if ((flags & 0x1000) != 0) {
	rootPosition.y += (firstTranslation.y - translation.y) * weight;
    }

    if ((flags & 0x2000) != 0) {
	rootPosition.z += (firstTranslation.z - translation.z) * weight;
    }

    if ((flags & 0x8000) != 0) {
	rootOrientation = glm::quat_cast (correction) * rootOrientation;
    }

    if ((flags & 0xA800) == 0x8000) {
	rootPosition = correction * rootPosition;
    }

    if (!state.started) {
	state.previous = start;
    }

    // the clock went past the end since the last frame: count the way from the last frame back to the first one in
    glm::mat3 previous (state.previous);
    glm::vec3 wrap (0.0f);

    if (state.time > time) {
	wrap = glm::vec3 (frames.last[3]) - firstTranslation;
	previous = previous * (glm::inverse (glm::mat3 (frames.last)) * glm::mat3 (frames.first));
    }

    const glm::mat3 previousYaw = yawFrame (previous * frames.inverseFirst);

    if ((flags & 0x3800) != 0) {
	const glm::vec3 delta = wrap + (translation - glm::vec3 (state.previous[3]))
	    + (currentYaw * -firstTranslation - previousYaw * -firstTranslation);
	const glm::vec3 moved = correction * delta;
	const glm::vec3 offset (
	    (flags & 0x800) != 0 ? moved.x : 0.0f, (flags & 0x1000) != 0 ? moved.y : 0.0f,
	    (flags & 0x2000) != 0 ? moved.z : 0.0f
	);

	step.moves = true;
	step.offset = world * offset * rest * weight;
    }

    // the turn between last frame's root and this one's, as the y angle of glm::extractEulerAngleZYX
    if ((flags & 0x8000) != 0) {
	const glm::mat3 turn = glm::inverse (previousYaw) * currentYaw;

	step.turns = true;
	step.yaw
	    = std::atan2 (-turn[0][2], std::sqrt (turn[1][2] * turn[1][2] + turn[2][2] * turn[2][2])) * rest * weight;
    }

    state.previous = current;
    return step;
}

glm::vec3 WallpaperEngine::Render::Objects::turnPuppetRootMotion (const glm::vec3& angles, const float yaw) {
    if (std::abs (angles.z) + std::abs (angles.x) < 0.0001f) {
	return { angles.x, std::fmod (yaw + angles.y, 6.2831855f), angles.z };
    }

    const glm::mat4 turned = glm::rotate (glm::eulerAngleZYX (angles.z, angles.y, angles.x), yaw, glm::vec3 (0, 1, 0));
    glm::vec3 result;

    glm::extractEulerAngleZYX (turned, result.z, result.y, result.x);
    return result;
}
