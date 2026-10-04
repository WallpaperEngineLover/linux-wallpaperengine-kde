#include "PuppetPhysics.h"

#include "WallpaperEngine/Data/JSON.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

using JSON = WallpaperEngine::Data::JSON::JSON;

// Ported from wallpaper64.exe 2.8.42. The math keeps WE's row-vector convention: a 3x3 is three rows, points
// transform as v * M, and quaternions are w, x, y, z.

namespace WallpaperEngine::Render::Objects {
namespace {
    constexpr float kDegToRad = 0.017453292f;
    constexpr float kRadToDeg = 57.29578f;
    constexpr float kPi = 3.1415927f;
    constexpr float kTwoPi = 6.2831855f;

    struct Rows {
	glm::vec3 r[3];
    };

    // w, x, y, z in the vec4's x, y, z, w
    using Quat = glm::vec4;

    const Quat kIdentity { 1.0f, 0.0f, 0.0f, 0.0f };

    glm::vec3 mulRow (const glm::vec3& v, const Rows& m) { return v.x * m.r[0] + v.y * m.r[1] + v.z * m.r[2]; }

    // sub_140215380
    Quat multiply (const Quat& a, const Quat& b) {
	return {
	    b.x * a.x - b.y * a.y - b.z * a.z - b.w * a.w,
	    b.y * a.x + a.y * b.x + b.w * a.z - a.w * b.z,
	    b.z * a.x + a.z * b.x + a.w * b.y - b.w * a.y,
	    b.w * a.x + a.w * b.x + b.z * a.y - a.z * b.y,
	};
    }

    // sub_140216070
    Quat slerp (const Quat& a, Quat b, float t) {
	float cosine = glm::dot (a, b);

	if (cosine < 0.0f) {
	    b = -b;
	    cosine = -cosine;
	}

	if (cosine > 0.99999988f) {
	    return a * (1.0f - t) + b * t;
	}

	const float angle = std::acos (cosine);
	const float sine = std::sin (angle);

	return (a * std::sin ((1.0f - t) * angle) + b * std::sin (angle * t)) / sine;
    }

    // sub_140217AC0
    Quat normalize (const Quat& q) {
	const float length = std::sqrt (glm::dot (q, q));
	return length > 0.0f ? q * (1.0f / length) : kIdentity;
    }

    // sub_1402167C0, the shortest rotation taking direction `from` onto `to`
    Quat rotationArc (const glm::vec3& from, const glm::vec3& to) {
	const float cosine = glm::dot (from, to);

	if (cosine >= 0.99999988f) {
	    return kIdentity;
	}

	if (cosine < -0.99999988f) {
	    glm::vec3 axis (-from.y, from.x, 0.0f);

	    if (glm::dot (axis, axis) < 0.00000011920929f) {
		axis = glm::vec3 (0.0f, -from.z, from.y);
	    }

	    axis *= 1.0f / std::sqrt (glm::dot (axis, axis));
	    return { std::cos (1.5707964f), axis.x, axis.y, axis.z };
	}

	const float s = std::sqrt ((cosine + 1.0f) + (cosine + 1.0f));
	const glm::vec3 axis = glm::cross (from, to) * (1.0f / s);

	return { s * 0.5f, axis.x, axis.y, axis.z };
    }

    // quaternion of the Euler angles, as built inline all over sub_1401FDF90
    Quat fromEuler (const glm::vec3& angles) {
	const glm::vec3 half = angles * 0.5f;
	const float c1 = std::cos (half.x), s1 = std::sin (half.x);
	const float c2 = std::cos (half.y), s2 = std::sin (half.y);
	const float c3 = std::cos (half.z), s3 = std::sin (half.z);

	return {
	    s2 * s1 * s3 + c2 * c1 * c3,
	    c2 * s1 * c3 - s2 * c1 * s3,
	    c2 * s1 * s3 + s2 * c1 * c3,
	    c2 * c1 * s3 - s2 * s1 * c3,
	};
    }

    Rows eulerRows (const glm::vec3& angles) {
	const float cx = std::cos (angles.x), sx = std::sin (angles.x);
	const float cy = std::cos (angles.y), sy = std::sin (angles.y);
	const float cz = std::cos (angles.z), sz = std::sin (angles.z);

	return { {
	    { cy * cz, cy * sz, -sy },
	    { sy * cz * sx - cx * sz, sy * sz * sx + cx * cz, sx * cy },
	    { cx * cz * sy + sx * sz, cx * sz * sy - sx * cz, cx * cy },
	} };
    }

    // sub_140216280
    Rows quatRows (const Quat& q) {
	const float w = q.x, x = q.y, y = q.z, z = q.w;

	return { {
	    { 1.0f - 2.0f * (z * z + y * y), 2.0f * (z * w + x * y), 2.0f * (x * z - y * w) },
	    { 2.0f * (x * y - z * w), 1.0f - 2.0f * (z * z + x * x), 2.0f * (x * w + y * z) },
	    { 2.0f * (y * w + x * z), 2.0f * (y * z - x * w), 1.0f - 2.0f * (y * y + x * x) },
	} };
    }

    glm::vec3 eulerFromRows (const Rows& n) {
	const float z = std::atan2 (n.r[0].y, n.r[0].x);
	const float y = std::atan2 (-n.r[0].z, std::sqrt (n.r[2].z * n.r[2].z + n.r[1].z * n.r[1].z));
	const float sz = std::sin (z), cz = std::cos (z);
	const float x = std::atan2 (sz * n.r[2].x - cz * n.r[2].y, cz * n.r[1].y - sz * n.r[1].x);

	return { x, y, z };
    }

    float wrapAngle (float angle, bool locked) {
	if (locked) {
	    return 0.0f;
	}

	if (angle < 0.0f) {
	    return std::fmod (angle - kPi, kTwoPi) + kPi;
	}

	return std::fmod (angle + kPi, kTwoPi) - kPi;
    }

    glm::vec3 parseVector (const std::string& text) {
	glm::vec3 result (0.0f);
	const char* cursor = text.c_str ();

	for (int i = 0; i < 3 && *cursor != '\0'; i++) {
	    char* end = nullptr;
	    result[i] = std::strtof (cursor, &end);

	    if (end == cursor) {
		break;
	    }

	    cursor = end;
	}

	return result;
    }
} // namespace

PuppetBonePhysics PuppetBonePhysics::parse (const std::string& text) {
    PuppetBonePhysics result;
    const JSON json = JSON::parse (text, nullptr, false);

    if (!json.is_object ()) {
	return result;
    }

    const auto isTrue = [&json] (const char* key) {
	const auto it = json.find (key);
	return it != json.end () && it->is_boolean () && it->get<bool> ();
    };
    const auto number = [&json] (const char* key, float& out) {
	const auto it = json.find (key);
	if (it != json.end () && it->is_number ()) {
	    out = it->get<float> ();
	}
    };
    const auto vector = [&json] (const char* key, glm::vec3& out) {
	const auto it = json.find (key);
	if (it != json.end () && it->is_string ()) {
	    out = parseVector (it->get<std::string> ());
	    return true;
	}
	return false;
    };

    uint32_t flags = 0;
    flags |= isTrue ("ik") ? InverseKinematics : 0;
    flags |= isTrue ("r") ? Rotation : 0;
    flags |= isTrue ("t") ? Translation : 0;
    flags |= isTrue ("se") ? Simulate : 0;
    flags |= isTrue ("re") ? SimulateRigid : 0;

    if ((flags & (Simulate | SimulateRigid)) == 0 || (flags & (Rotation | Translation)) == 0) {
	return result;
    }

    flags |= isTrue ("ge") ? Gravity : 0;
    vector ("gd", result.gravityDirection);
    number ("m", result.mass);
    number ("tf", result.translationFriction);
    number ("rs", result.rotationStiffness);
    number ("ts", result.translationStiffness);
    number ("rf", result.rotationFriction);
    vector ("tp", result.target);
    number ("tm", result.maxTranslation);

    float inertia = 0.0f;
    if (json.contains ("ri") && json["ri"].is_number ()) {
	number ("ri", inertia);
	result.rotationInertia = 1.0f - inertia / 100.0f;
    }
    if (json.contains ("ti") && json["ti"].is_number ()) {
	number ("ti", inertia);
	result.translationInertia = 1.0f - inertia / 100.0f;
    }

    if (isTrue ("la")) {
	flags |= AngleLimits;
	vector ("lamin", result.angleMin);
	vector ("lamax", result.angleMax);
    }

    if (isTrue ("lt")) {
	flags |= TotalAngleLimit;
	number ("ltmax", result.maxAngle);
    }

    // axis locks only count when all three keys are there, an axis set to false is locked
    const auto locks = [&json, &flags] (const char* x, const char* y, const char* z, uint32_t firstFlag) {
	const char* keys[] = { x, y, z };
	for (const char* key : keys) {
	    if (!json.contains (key) || !json[key].is_boolean ()) {
		return;
	    }
	}
	for (int i = 0; i < 3; i++) {
	    if (!json[keys[i]].get<bool> ()) {
		flags |= firstFlag << i;
	    }
	}
    };
    locks ("rax", "ray", "raz", LockRotationX);
    locks ("tax", "tay", "taz", LockTranslationX);

    result.flags = flags;
    return result;
}

glm::mat4 stepPuppetBonePhysics (
    const PuppetBonePhysics& physics, PuppetBonePhysicsState& state, const glm::mat4& world,
    const glm::mat4& previousWorld, float dt, float objectScale
) {
    const uint32_t flags = physics.flags;
    const Rows current { { glm::vec3 (world[0]), glm::vec3 (world[1]), glm::vec3 (world[2]) } };
    const Rows previous { { glm::vec3 (previousWorld[0]), glm::vec3 (previousWorld[1]),
			    glm::vec3 (previousWorld[2]) } };
    const glm::vec3 currentOrigin (world[3]);
    const glm::vec3 previousOrigin (previousWorld[3]);

    // v * inverse, from world space into this bone's space
    const glm::mat3 inverse = glm::inverse (glm::mat3 (world));
    const auto toBone = [&inverse] (const glm::vec3& v) { return inverse * v; };

    // where the target point was last frame, seen from the bone as it is now
    const glm::vec3 previousTarget = toBone (mulRow (physics.target, previous) + previousOrigin - currentOrigin);
    const glm::vec3 previousDirection = glm::normalize (previousTarget);
    glm::vec3 localOffset = toBone (state.offset);

    const glm::vec3 rotatedTarget = mulRow (physics.target, eulerRows (state.angles));
    glm::vec3 predicted = rotatedTarget + localOffset;

    if (flags & PuppetBonePhysics::Gravity) {
	const glm::vec3 gravity = toBone (physics.gravityDirection) * (physics.mass * objectScale);
	const glm::vec3 direction = glm::normalize (rotatedTarget);
	predicted -= gravity - glm::dot (direction, gravity) * direction;

	if (flags & PuppetBonePhysics::Translation) {
	    state.velocity += physics.gravityDirection * (physics.mass * dt * 1000.0f);
	}
    }

    const float distance = std::min (glm::length (predicted - previousTarget), dt * 900.0f);
    const Quat arc = rotationArc (glm::normalize (predicted), previousDirection);

    // rigid "re" bones (0x140201a24): no stiffness, no angle wrapping, axis locks after the angle limits
    const bool rigid = flags & PuppetBonePhysics::SimulateRigid;

    if ((flags & (PuppetBonePhysics::Simulate | PuppetBonePhysics::SimulateRigid))
	&& (flags & PuppetBonePhysics::Rotation)) {
	Quat& velocity = state.angularVelocity;

	velocity = multiply (
	    velocity, slerp (kIdentity, arc, std::min (distance * physics.rotationInertia * kDegToRad, 1.0f))
	);
	if (!rigid) {
	    velocity = multiply (
		velocity,
		slerp (
		    kIdentity, fromEuler (-state.angles), std::min (physics.rotationStiffness * kDegToRad * dt, 1.0f)
		)
	    );
	}

	if (flags & PuppetBonePhysics::TotalAngleLimit) {
	    const float w = velocity.x;
	    velocity = { 1.0f, velocity.y / w, velocity.z / w, velocity.w / w };

	    for (int i = 1; i < 4; i++) {
		float angle = std::atan (velocity[i]) * kRadToDeg * 2.0f;
		angle = std::max (std::min (angle, physics.maxAngle), -physics.maxAngle);
		velocity[i] = std::tan (angle * kDegToRad * 0.5f);
	    }

	    velocity = normalize (velocity);
	}

	// velocity is per 60 Hz frame
	const Quat step = slerp (kIdentity, velocity, std::min (dt / 0.016666668f, 1.0f));
	const Rows angles = eulerRows (state.angles);
	const Rows rotation = quatRows (step);
	const Rows combined { { mulRow (angles.r[0], rotation), mulRow (angles.r[1], rotation),
				mulRow (angles.r[2], rotation) } };
	const glm::vec3 next = eulerFromRows (combined);

	if (rigid) {
	    state.angles = next;
	} else {
	    state.angles = {
		wrapAngle (next.x, flags & PuppetBonePhysics::LockRotationX),
		wrapAngle (next.y, flags & PuppetBonePhysics::LockRotationY),
		wrapAngle (next.z, flags & PuppetBonePhysics::LockRotationZ),
	    };
	}

	if (flags & PuppetBonePhysics::AngleLimits) {
	    const glm::vec3 clamped = glm::min (glm::max (state.angles, physics.angleMin), physics.angleMax);
	    const Quat overshoot = fromEuler (state.angles - clamped);

	    state.angles = clamped;
	    velocity = multiply ({ overshoot.x, -overshoot.y, -overshoot.z, -overshoot.w }, velocity);
	}

	if (rigid) {
	    for (int axis = 0; axis < 3; axis++) {
		if (flags & (PuppetBonePhysics::LockRotationX << axis)) {
		    state.angles[axis] = 0.0f;
		}
	    }
	}

	velocity = slerp (velocity, kIdentity, std::min (dt * physics.rotationFriction, 1.0f));
    }

    if ((flags & (PuppetBonePhysics::Simulate | PuppetBonePhysics::SimulateRigid))
	&& (flags & PuppetBonePhysics::Translation)) {
	const glm::vec3 pulled
	    = state.offset - (state.offset + currentOrigin - previousOrigin) * physics.translationInertia;
	glm::vec3 velocity = rigid ? state.velocity : state.velocity - (dt * physics.translationStiffness) * pulled;
	glm::vec3 offset = pulled + dt * velocity;

	for (int axis = 0; axis < 3; axis++) {
	    if (flags & (PuppetBonePhysics::LockTranslationX << axis)) {
		const glm::vec3& direction = current.r[axis];
		offset -= glm::dot (offset, direction) / glm::dot (direction, direction) * direction;
	    }
	}

	const float limit = objectScale * physics.maxTranslation;
	if (limit > 0.0f && limit * limit < glm::dot (offset, offset)) {
	    offset *= limit / glm::length (offset);
	}

	velocity -= std::min (dt * physics.translationFriction, 1.0f) * velocity;

	state.offset = offset;
	state.velocity = velocity;
	localOffset = toBone (offset);
    }

    // rows of the Euler rotation plus the offset, glm columns hold WE's rows
    const Rows rotation = eulerRows (state.angles);

    return {
	glm::vec4 (rotation.r[0], 0.0f),
	glm::vec4 (rotation.r[1], 0.0f),
	glm::vec4 (rotation.r[2], 0.0f),
	glm::vec4 (localOffset, 1.0f),
    };
}

glm::vec4 puppetSlerp (const glm::vec4& a, const glm::vec4& b, float t) { return slerp (a, b, t); }

glm::vec4 puppetRotationArc (const glm::vec3& from, const glm::vec3& to) { return rotationArc (from, to); }

glm::mat3 puppetQuatRows (const glm::vec4& q) {
    const Rows rows = quatRows (q);
    return { rows.r[0], rows.r[1], rows.r[2] };
}

void applyPuppetBoneImpulse (
    PuppetBonePhysicsState& state, const glm::vec3& directional, const glm::vec3& angularDegrees
) {
    state.velocity += directional;
    state.angularVelocity = multiply (state.angularVelocity, fromEuler (angularDegrees * kDegToRad));
}
} // namespace WallpaperEngine::Render::Objects
