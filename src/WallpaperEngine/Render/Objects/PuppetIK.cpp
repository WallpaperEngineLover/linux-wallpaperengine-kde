#include "PuppetIK.h"
#include "PuppetPhysics.h"

#include "WallpaperEngine/Data/JSON.h"
#include "WallpaperEngine/Render/Utils/NoiseUtils.h"

#include <algorithm>
#include <bit>
#include <cmath>

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

using JSON = WallpaperEngine::Data::JSON::JSON;

// wallpaper64.exe 2.8.42 sub_140271910. glm columns hold WE's rows

namespace WallpaperEngine::Render::Objects {
namespace {
    glm::vec3 position (const glm::mat4& m) { return glm::vec3 (m[3]); }

    void setPosition (glm::mat4& m, const glm::vec3& p) { m[3] = glm::vec4 (p, m[3].w); }

    // sub_140214EB0, fast inverse sqrt
    glm::vec4 fastNormalize (const glm::vec4& row) {
	const float squared = row.w * row.w + row.z * row.z + (row.y * row.y + row.x * row.x);
	const float guess = std::bit_cast<float> (0x5F375A86u - (std::bit_cast<uint32_t> (squared) >> 1));
	const float inverse = (1.5f - squared * 0.5f * guess * guess) * guess;

	return row * inverse;
    }

    // shortest arc, w x y z
    glm::vec4 arc (const glm::vec3& from, const glm::vec3& to) {
	const float cosine = glm::dot (from, to);

	if (cosine >= 0.99999988f) {
	    return { 1.0f, 0.0f, 0.0f, 0.0f };
	}

	if (cosine < -0.99999988f) {
	    return puppetRotationArc (from, to);
	}

	const float s = std::sqrt ((cosine + 1.0f) + (cosine + 1.0f));

	return {
	    s * 0.5f,
	    (from.y * to.z - from.z * to.y) * (1.0f / s),
	    (from.z * to.x - from.x * to.z) * (1.0f / s),
	    (from.x * to.y - from.y * to.x) * (1.0f / s),
	};
    }

    // rows of Q * bind
    void rotateRows (glm::mat4& out, const glm::vec4& q, const glm::mat4& bind) {
	const glm::mat3 rows = puppetQuatRows (q);

	for (int j = 0; j < 3; j++) {
	    out[j] = rows[j].x * bind[0] + rows[j].y * bind[1] + rows[j].z * bind[2];
	}
    }

    glm::vec4 quatFromRows (const glm::mat4& m) {
	const glm::quat q = glm::quat_cast (glm::mat3 (glm::vec3 (m[0]), glm::vec3 (m[1]), glm::vec3 (m[2])));
	return { q.w, q.x, q.y, q.z };
    }

    const glm::vec3* childDirection (const PuppetIKRig& rig, int bone, int child) {
	if (bone < 0 || static_cast<size_t> (bone) >= rig.childDirections.size ()) {
	    return nullptr;
	}

	const auto& map = rig.childDirections[bone];
	const auto it = map.find (static_cast<uint32_t> (child));
	return it == map.end () ? nullptr : &it->second;
    }

    float length (const PuppetIKRig& rig, int bone) {
	return bone >= 0 && static_cast<size_t> (bone) < rig.lengths.size () ? rig.lengths[bone] : 0.0f;
    }

    glm::vec3 transformPoint (const glm::mat4& m, const glm::vec3& p) { return glm::vec3 (m * glm::vec4 (p, 1.0f)); }

    const PuppetBoneIK& endSettings (const std::vector<PuppetBoneIK>& ik, uint32_t bone) {
	static const PuppetBoneIK none;
	return bone < ik.size () ? ik[bone] : none;
    }

    // sub_14026E340: one length constraint sweep over a rope; "ikse" stretches every bone alike
    void constrainRope (
	const PuppetIKRig& rig, const PuppetIKEntry& entry, const PuppetBoneIK& end, const glm::mat4& world,
	const glm::mat4& inverse, std::vector<PuppetRopeJoint>& joints, bool forward
    ) {
	const auto& bones = entry.bones;
	const glm::vec3 span = joints[entry.endBone].current - joints[bones[0]].current;
	const float distance = std::sqrt ((span.x * span.x + span.y * span.y) + span.z * span.z);
	float scale = 1.0f;

	if (distance > entry.length && (end.flags & PuppetBoneIK::SolveEnd)) {
	    scale = distance / entry.length;
	}

	const bool freeEnd = (end.flags & PuppetBoneIK::FollowParent) != 0;
	const int count = static_cast<int> (bones.size ()) - (freeEnd ? 0 : 1);

	for (int i = 1; i < count; i++) {
	    int child;
	    int parent;

	    if (forward) {
		child = bones[i];
		parent = bones[i - 1];
	    } else {
		const int k = count - i;
		child = bones[freeEnd ? k : k + 1];
		parent = bones[freeEnd ? k - 1 : k];
	    }

	    const float rest = scale * length (rig, child);
	    const glm::vec3 from = transformPoint (inverse, joints[parent].current);
	    const glm::vec3 to = transformPoint (inverse, joints[child].current);
	    const glm::vec3 offset = to - from;
	    const float current = std::sqrt ((offset.y * offset.y + offset.x * offset.x) + offset.z * offset.z);
	    const float miss = rest - current;

	    if (std::fabs (miss) <= 0.001f) {
		continue;
	    }

	    const glm::vec3 direction = current <= 0.0099999998f ? glm::vec3 (0.707f, 0.707f, 0.0f) : offset / current;

	    if (forward && i == 1) {
		joints[child].current = transformPoint (world, to + direction * miss);
	    } else if (!forward && i == 1) {
		joints[parent].current = transformPoint (world, from - direction * miss);
	    } else {
		const float half = miss * 0.5f;
		joints[child].current = transformPoint (world, to + direction * half);
		joints[parent].current = transformPoint (world, from - direction * half);
	    }
	}
    }
    // WE's row major helpers
    struct Euler {
	float a, b, c;
    };

    // sub_1401E23D0
    Euler eulerOf (const glm::mat4& matrix) {
	const float* m = glm::value_ptr (matrix);
	const float a = std::atan2 (m[1], m[0]);
	const float b = std::atan2 (-m[2], std::sqrt (m[6] * m[6] + m[10] * m[10]));
	const float s = std::sin (a);
	const float c = std::cos (a);
	return { a, b, std::atan2 (s * m[8] - c * m[9], c * m[5] - s * m[4]) };
    }

    // sub_140215020, rows only
    glm::mat4 rotationOf (const Euler& e) {
	const float ca = std::cos (e.a), sa = std::sin (e.a);
	const float cb = std::cos (e.b), sb = std::sin (e.b);
	const float cc = std::cos (e.c), sc = std::sin (e.c);
	glm::mat4 out (0.0f);
	float* m = glm::value_ptr (out);
	m[0] = cb * ca;
	m[1] = cb * sa;
	m[2] = -sb;
	m[4] = sb * ca * sc - cc * sa;
	m[5] = sb * sa * sc + cc * ca;
	m[6] = sc * cb;
	m[8] = cc * ca * sb + sc * sa;
	m[9] = cc * sa * sb - sc * ca;
	m[10] = cc * cb;
	m[15] = 1.0f;
	return out;
    }

    // sub_1401E24B0
    float rowLength (const glm::vec4& r) { return std::sqrt ((r.w * r.w + r.z * r.z) + (r.y * r.y + r.x * r.x)); }

    glm::mat4 withPosition (glm::mat4 m, const glm::vec3& p) {
	m[3] = glm::vec4 (p, 1.0f);
	return m;
    }

    // sub_14026DCE0, false when parallel
    bool closestPoints (
	const glm::vec3& p1, const glm::vec3& d1, const glm::vec3& p2, const glm::vec3& d2, glm::vec3& onSecond
    ) {
	const float a = (d1.x * d1.x + d1.y * d1.y) + d1.z * d1.z;
	const float b = (d1.x * d2.x + d1.y * d2.y) + d1.z * d2.z;
	const float c = (d2.x * d2.x + d2.y * d2.y) + d2.z * d2.z;
	const float det = a * c - b * b;

	if (det == 0.0f) {
	    return false;
	}

	const glm::vec3 r = p1 - p2;
	const float d = (d1.x * r.x + d1.y * r.y) + d1.z * r.z;
	const float e = (d2.y * r.y + d2.x * r.x) + d2.z * r.z;
	onSecond = p2 + d2 * ((e * a - d * b) / det);
	return true;
    }
} // namespace

glm::mat4 blendPuppetTransform (const glm::mat4& from, const glm::mat4& to, float weight) {
    constexpr float twoPi = 6.2831855f;
    constexpr float pi = 3.1415927f;
    Euler e = eulerOf (from);
    Euler t = eulerOf (to);

    for (float* angle : { &e.a, &e.b, &e.c, &t.a, &t.b, &t.c }) {
	*angle = std::fmod (*angle, twoPi);
    }

    // WE only subtracts pi
    if (t.c - e.c > pi) {
	t.c -= pi;
    }
    if (t.b - e.b > pi) {
	t.b -= pi;
    }
    if (t.a - e.a > pi) {
	t.a -= pi;
    }

    const float keep = 1.0f - weight;
    const glm::mat4 rotation = rotationOf (
	{
	    e.a * keep + t.a * weight,
	    e.b * keep + t.b * weight,
	    e.c * keep + t.c * weight,
	}
    );
    glm::mat4 out (1.0f);

    for (int i = 0; i < 3; i++) {
	out[i] = rotation[i] * (rowLength (from[i]) * keep + rowLength (to[i]) * weight);
    }

    out[3] = glm::vec4 (position (from) * keep + position (to) * weight, 1.0f);
    return out;
}

void applyPuppetChainConstraints (
    const PuppetIKRig& rig, const PuppetIKChain& chain, const std::vector<float>& weights,
    std::vector<glm::mat4>& model, std::vector<glm::mat4>& extras
) {
    const auto validBone = [&model] (int bone) { return bone >= 0 && static_cast<size_t> (bone) < model.size (); };

    for (const uint32_t extra : chain.extras) {
	if (extra >= extras.size () || extra >= rig.extraRules.size () || extra >= rig.extraBones.size ()) {
	    continue;
	}

	for (const auto& rule : rig.extraRules[extra]) {
	    if (!validBone (static_cast<int> (rule.target))) {
		continue;
	    }

	    const float weight = rule.weight < weights.size () ? weights[rule.weight] : 0.0f;
	    const glm::mat4& target = model[rule.target];
	    glm::mat4& controller = extras[extra];

	    if ((rule.flags & 0x10000) == 0 && (rule.flags & 2) == 0) {
		if (rule.flags & 1) {
		    setPosition (controller, position (controller) * (1.0f - weight) + position (target) * weight);
		} else {
		    controller = blendPuppetTransform (controller, target, weight);
		}

		continue;
	    }

	    // once per entry ending at the extra's bone
	    const glm::vec3 axis = glm::vec3 (target[0]) * (1.0f / rowLength (target[0]));
	    const glm::vec3 origin = position (target);

	    for (const auto& link : chain.links) {
		for (const auto& entry : link.entries) {
		    if (entry.endBone != rig.extraBones[extra] || entry.bones.empty ()
			|| std::ranges::any_of (entry.bones, [&validBone] (int bone) { return !validBone (bone); })) {
			continue;
		    }

		    const glm::vec3 offset = position (controller) - origin;
		    const float along = (offset.y * axis.y + offset.x * axis.x) + offset.z * axis.z;

		    if (rule.flags & 0x10000) {
			// "chainalignaxis"
			const glm::vec3 fromRoot = origin - position (model[entry.bones[0]]);
			const float reach
			    = std::sqrt ((fromRoot.x * fromRoot.x + fromRoot.y * fromRoot.y) + fromRoot.z * fromRoot.z);

			if (reach >= entry.length || along < 0.0f) {
			    controller = blendPuppetTransform (controller, withPosition (controller, origin), weight);
			    continue;
			}

			float distance = std::min (entry.length - reach, along);
			controller = blendPuppetTransform (
			    controller, withPosition (target, origin + axis * distance), weight
			);
			float remaining = along;

			for (int i = static_cast<int> (entry.bones.size ()) - 2; i > 0; i--) {
			    const float next = length (rig, entry.bones[i + 1]);
			    remaining -= next;

			    if (remaining < 0.0f) {
				break;
			    }

			    const float past = std::max (remaining - rule.start, 0.0f);
			    const float ramp = past / rule.range >= 1.0f ? 1.0f : past / rule.range;

			    if (ramp <= 0.0f) {
				break;
			    }

			    distance -= next;

			    if (distance < 0.0f) {
				break;
			    }

			    glm::mat4& joint = model[entry.bones[i]];
			    joint = blendPuppetTransform (
				joint, withPosition (target, origin + axis * distance), ramp * weight
			    );
			}
		    } else {
			// "axis"
			const glm::vec3 across = offset - axis * along;
			const glm::vec3 direction = across
			    * (1.0f / std::sqrt ((across.x * across.x + across.y * across.y) + across.z * across.z));
			glm::vec3 onAxis;

			if (closestPoints (position (controller), direction, origin, axis, onAxis)) {
			    controller = blendPuppetTransform (controller, withPosition (controller, onAxis), weight);
			}
		    }
		}
	    }
	}
    }
}

void limitPuppetJointDirection (
    const PuppetBoneIK& settings, const glm::vec3& joint, const glm::vec3& parent, glm::vec3& direction
) {
    const glm::vec3 fromParent = joint - parent;
    const float parentLength
	= std::sqrt ((fromParent.y * fromParent.y + fromParent.x * fromParent.x) + fromParent.z * fromParent.z);
    glm::mat4 frame (1.0f);
    const glm::mat3 rows
	= puppetQuatRows (puppetRotationArc (glm::vec3 (1.0f, 0.0f, 0.0f), fromParent * (1.0f / parentLength)));

    for (int i = 0; i < 3; i++) {
	frame[i] = glm::vec4 (rows[i], 0.0f);
    }

    const float flat = std::sqrt ((direction.x * direction.x + direction.y * direction.y) + 0.0f);
    const glm::vec3 local
	= glm::vec3 (glm::inverse (frame) * glm::vec4 (direction.x / flat, direction.y / flat, 0.0f / flat, 1.0f));
    const float angle = std::atan2 (local.y, local.x);
    float turn = settings.minAngle > angle ? -(angle - settings.minAngle) : 0.0f;

    if (angle > settings.maxAngle) {
	const float back = settings.maxAngle - angle;

	if (turn == 0.0f || std::fabs (turn) > std::fabs (back)) {
	    turn = back;
	}
    }

    if (turn == 0.0f) {
	return;
    }

    const float c = std::cos (turn);
    const float s = std::sin (turn);
    const glm::vec3 turned (c * local.x - s * local.y, s * local.x + c * local.y, local.z);
    const glm::vec3 world = glm::vec3 (frame * glm::vec4 (turned, 1.0f)) * flat;
    direction.x = world.x;
    direction.y = world.y;
}

bool PuppetRopeEnvironment::windAt (const glm::vec3& position, glm::vec3& force) const {
    if (!this->wind) {
	return false;
    }

    // zero width: every simplex corner fails in WE (sub_140198910)
    float noise = 0.0f;

    if (this->width > 0.0f) {
	noise = std::max (0.0f, Utils::simplexNoise2D (this->clock, position.x / this->width * 10.0f));
    }

    force = noise * this->windDirection * this->windStrength;
    return true;
}

void stepPuppetRopeChain (
    const PuppetIKRig& rig, const PuppetIKChain& chain, const std::vector<PuppetBoneIK>& ik,
    std::vector<glm::mat4>& model, const std::vector<glm::mat4>& extras, const glm::mat4& world,
    std::vector<PuppetRopeJoint>& joints, const PuppetRopeEnvironment& environment, float dt
) {
    const auto valid = [&model, &joints] (int bone) {
	return bone >= 0 && static_cast<size_t> (bone) < model.size () && static_cast<size_t> (bone) < joints.size ();
    };
    const glm::mat4 inverse = glm::inverse (world);
    const float dt2 = dt * dt;

    for (auto link = chain.links.rbegin (); link != chain.links.rend (); ++link) {
	for (const auto& entry : link->entries) {
	    if ((entry.flags & 4) == 0 || entry.bones.empty () || !valid (static_cast<int> (link->bone))
		|| !valid (static_cast<int> (entry.endBone))
		|| std::ranges::any_of (entry.bones, [&valid] (int bone) { return !valid (bone); })) {
		continue;
	    }

	    const PuppetBoneIK& end = endSettings (ik, entry.endBone);
	    const glm::vec3 root = position (model[link->bone]);
	    glm::vec3 target = position (model[entry.endBone]);

	    if (entry.flags & 1) {
		if (const auto it = rig.targets.find (entry.endBone);
		    it != rig.targets.end () && it->second < extras.size ()) {
		    target = position (extras[it->second]);
		}
	    }

	    // without "ikse" the rope can't stretch
	    if ((end.flags & PuppetBoneIK::SolveEnd) == 0) {
		const glm::vec3 offset = target - root;
		const float distance = std::sqrt ((offset.x * offset.x + offset.y * offset.y) + offset.z * offset.z);

		if (distance > entry.length) {
		    target = offset / distance * entry.length + root;
		}
	    }

	    joints[link->bone].current = transformPoint (world, root);
	    int count = static_cast<int> (entry.bones.size ());

	    if ((end.flags & PuppetBoneIK::FollowParent) == 0) {
		setPosition (model[entry.endBone], target);
		joints[entry.endBone].current = transformPoint (world, target);
		count--;
	    }

	    if (!joints[link->bone].started) {
		joints[link->bone].started = true;

		for (int i = 1; i < count; i++) {
		    auto& joint = joints[entry.bones[i]];
		    joint.current = transformPoint (world, position (model[entry.bones[i]]));
		    joint.previous = joint.current;
		}
	    }

	    for (int i = 1; i < count; i++) {
		const int bone = entry.bones[i];
		auto& joint = joints[bone];
		const glm::vec3 current = joint.current;
		const glm::vec3 back = joint.previous - current;
		glm::vec3 next = (current + current) - joint.previous;

		// "tf" damping
		if (dt2 > 0.00000011920929f && end.friction > 0.0099999998f
		    && (back.x * back.x + back.y * back.y) + back.z * back.z > 0.000099999997f) {
		    next += back * dt2 * (1.0f / dt2) * end.friction;
		}

		glm::vec3 force (0.0f);

		if (end.flags & PuppetBoneIK::Gravity) {
		    force = end.mass * environment.gravity;
		}

		if (glm::vec3 wind; environment.windAt (next, wind)) {
		    force += wind;
		}

		next += force * dt2;
		joint.previous = current;
		joint.current = next;
		setPosition (model[bone], transformPoint (inverse, next));
	    }
	}
    }
}

PuppetBoneIK PuppetBoneIK::parse (const std::string& text) {
    PuppetBoneIK result;
    const JSON json = JSON::parse (text, nullptr, false);

    if (!json.is_object ()) {
	return result;
    }

    const auto isTrue = [&json] (const char* key) {
	const auto it = json.find (key);
	return it != json.end () && it->is_boolean () && it->get<bool> ();
    };

    // only "ik" bones, except the "ikce" flag
    if (isTrue ("ikce")) {
	result.flags |= Constrained;
    }

    if (!isTrue ("ik")) {
	// no limits for simulated bones
	if ((result.flags & Constrained) && !((isTrue ("se") || isTrue ("re")) && (isTrue ("r") || isTrue ("t")))) {
	    if (const auto it = json.find ("ikrminl"); it != json.end () && it->is_number ()) {
		result.minAngle = it->get<float> ();
	    }

	    if (const auto it = json.find ("ikrmaxl"); it != json.end () && it->is_number ()) {
		result.maxAngle = it->get<float> ();
	    }
	}

	return result;
    }

    result.flags |= Enabled;
    result.flags |= isTrue ("ge") ? Gravity : 0;

    if (const auto it = json.find ("m"); it != json.end () && it->is_number ()) {
	result.mass = it->get<float> ();
    }

    if (const auto it = json.find ("tf"); it != json.end () && it->is_number ()) {
	result.friction = it->get<float> ();
    }

    // non-simulated bones use (tf / 100)^2
    if (!((isTrue ("se") || isTrue ("re")) && (isTrue ("r") || isTrue ("t")))) {
	const float scaled = (result.friction / 100.0f) * (result.friction / 100.0f);
	result.friction = scaled >= 0.0f ? std::min (scaled, 1.0f) : 0.0f;
    }

    if (const auto it = json.find ("ikd"); it != json.end () && it->is_number ()) {
	result.depth = std::max (it->get<int> (), 1);
    }

    result.flags |= isTrue ("ikg") ? Grounded : 0;

    if (isTrue ("ikr")) {
	result.flags |= RotateToTarget;

	if (const auto it = json.find ("ikrd"); it != json.end () && it->is_number ()) {
	    result.rotateDistance = std::max (it->get<float> (), 1.0f);
	}
    }

    result.flags |= isTrue ("ikse") ? SolveEnd : 0;
    result.flags |= isTrue ("ikfe") ? FollowParent : 0;

    return result;
}

void solvePuppetIKChain (
    const PuppetIKRig& rig, const PuppetIKChain& chain, const std::vector<int>& parents,
    const std::vector<PuppetBoneIK>& ik, std::vector<glm::mat4>& model, const std::vector<glm::mat4>& extras,
    const std::vector<glm::mat4>& bindModel, const glm::mat4& world, std::vector<PuppetRopeJoint>& joints
) {
    const auto valid = [&model, &joints] (int bone) {
	return bone >= 0 && static_cast<size_t> (bone) < model.size () && static_cast<size_t> (bone) < joints.size ();
    };

    const auto extraOf = [&extras] (const std::unordered_map<uint32_t, uint32_t>& map, uint32_t bone) {
	const auto it = map.find (bone);
	return it != map.end () && it->second < extras.size () ? &extras[it->second] : nullptr;
    };

    for (const auto& link : chain.links) {
	for (const auto& entry : link.entries) {
	    if (!valid (static_cast<int> (link.bone)) || !valid (static_cast<int> (entry.endBone))
		|| std::ranges::any_of (entry.bones, [&valid] (int bone) { return !valid (bone); })) {
		return;
	    }
	}
    }

    if (!valid (static_cast<int> (chain.root))) {
	return;
    }

    const glm::vec3 root = position (model[chain.root]);
    const glm::mat4 inverse = glm::inverse (world);

    for (int iteration = 0; iteration < 10; iteration++) {
	// forward pass
	for (auto link = chain.links.rbegin (); link != chain.links.rend (); ++link) {
	    glm::vec3 sum (0.0f);
	    float count = 0.0f;

	    for (auto entry = link->entries.rbegin (); entry != link->entries.rend (); ++entry) {
		if (entry->flags & 4) {
		    constrainRope (rig, *entry, endSettings (ik, entry->endBone), world, inverse, joints, false);
		    continue;
		}

		const glm::vec3 start = position (model[link->bone]);
		const glm::mat4* targetExtra = (entry->flags & 1) ? extraOf (rig.targets, entry->endBone) : nullptr;
		glm::vec3 target = targetExtra != nullptr ? position (*targetExtra) : position (model[entry->endBone]);
		const glm::vec3 offset = target - start;
		float distance = std::sqrt (glm::dot (offset, offset));
		const glm::vec3 direction = offset / distance;

		if (entry->minReach > 0.0f && entry->minReach > distance) {
		    target += (entry->minReach - distance) * direction;
		    distance = entry->minReach;
		}

		glm::vec3 pole (0.0f);

		if (const auto* poleExtra = extraOf (rig.poles, entry->endBone); poleExtra != nullptr) {
		    const glm::vec3 toPole = position (*poleExtra) - target;
		    pole = toPole - direction * glm::dot (toPole, direction);
		}

		setPosition (model[entry->endBone], target);
		const int last = static_cast<int> (entry->bones.size ()) - 1;

		if (distance < entry->length || (entry->flags & 2)) {
		    // bend in the plane of the target and the pole
		    const float poleSquared = glm::dot (pole, pole);
		    const glm::vec3 normal (
			direction.y * pole.z - direction.z * pole.y, direction.z * pole.x - direction.x * pole.z,
			direction.x * pole.y - direction.y * pole.x
		    );
		    const float normalLength = std::sqrt (glm::dot (normal, normal));
		    float walked = 0.0f;

		    for (int i = last; i > 0; i--) {
			const int bone = entry->bones[i];
			const int joint = entry->bones[i - 1];
			const glm::vec3 base = position (model[bone]);
			glm::vec3 toJoint = position (model[joint]) - base;

			if (i > 1 && static_cast<size_t> (joint) < ik.size ()
			    && (ik[joint].flags & PuppetBoneIK::Constrained) && valid (parents[joint])) {
			    const float squared
				= (toJoint.x * toJoint.x + toJoint.y * toJoint.y) + toJoint.z * toJoint.z;
			    glm::vec3 forward = -(toJoint * (1.0f / std::sqrt (squared)));
			    limitPuppetJointDirection (
				ik[joint], position (model[joint]), position (model[parents[joint]]), forward
			    );
			    toJoint = -forward;
			}

			if (i == last || entry->length * 0.5f > walked + length (rig, bone)) {
			    if (poleSquared > 0.0099999998f) {
				const glm::vec3 axis = normal * (1.0f / normalLength);
				toJoint -= glm::dot (toJoint, axis) * axis;
			    }

			    const float side = glm::dot (toJoint, pole);

			    if (side < 0.0f) {
				toJoint -= (pole + pole) * side;
			    }
			}

			const float jointLength = std::sqrt (glm::dot (toJoint, toJoint));
			setPosition (model[joint], toJoint * (1.0f / jointLength) * length (rig, bone) + base);
			walked += length (rig, bone);
		    }
		} else {
		    // out of reach
		    for (int i = last; i > 0; i--) {
			const int bone = entry->bones[i];
			setPosition (
			    model[entry->bones[i - 1]], position (model[bone]) - length (rig, bone) * direction
			);
		    }
		}

		count += 1.0f;
		sum += position (model[link->bone]);
	    }

	    if (count > 0.0f) {
		setPosition (model[link->bone], sum / count);
	    }
	}

	// first pass: "chainalignaxis" mirrors joints on the wrong side of the axis plane, 32 steps max
	if (iteration == 0) {
	    for (const uint32_t extra : chain.extras) {
		if (extra >= rig.extraRules.size () || extra >= rig.extraBones.size ()) {
		    continue;
		}

		for (const auto& rule : rig.extraRules[extra]) {
		    if ((rule.flags & 0x10000) == 0 || !valid (static_cast<int> (rule.target))) {
			continue;
		    }

		    const glm::mat4& target = model[rule.target];
		    const float inverse = 1.0f
			/ std::sqrt ((target[0].w * target[0].w + target[0].z * target[0].z)
				     + (target[0].y * target[0].y + target[0].x * target[0].x));
		    const glm::vec3 axis = glm::vec3 (target[0]) * inverse;

		    for (const auto& link : chain.links) {
			for (const auto& entry : link.entries) {
			    const int n = static_cast<int> (entry.bones.size ());

			    if (entry.endBone != rig.extraBones[extra] || n <= 2) {
				continue;
			    }

			    const glm::vec3 fromRoot = position (target) - position (model[entry.bones[0]]);
			    const float full = std::sqrt (
				(fromRoot.x * fromRoot.x + fromRoot.y * fromRoot.y) + fromRoot.z * fromRoot.z
			    );
			    float distance = full;

			    for (int k = 1, budget = 32; budget > 0; budget--) {
				distance -= length (rig, entry.bones[k]);

				if (distance < 0.0f) {
				    break;
				}

				const glm::vec3 previous = position (model[entry.bones[k - 1]]);
				const glm::vec3 span = position (model[entry.bones[k + 1]]) - previous;
				const glm::vec3 side (
				    axis.z * span.y - axis.y * span.z, axis.x * span.z - axis.z * span.x,
				    axis.y * span.x - axis.x * span.y
				);
				const glm::vec3 normal (
				    side.y * span.z - side.z * span.y, side.z * span.x - side.x * span.z,
				    side.x * span.y - side.y * span.x
				);
				const float squared = normal.z * normal.z + (normal.x * normal.x + normal.y * normal.y);

				if (squared >= 0.0099999998f) {
				    const glm::vec3 unit = normal * (1.0f / std::sqrt (squared));
				    const glm::vec3 point = position (model[entry.bones[k]]);
				    const glm::vec3 offset = point - previous;
				    const float depth = (unit.x * offset.x + unit.y * offset.y) + offset.z * unit.z;

				    if (depth > 0.00000011920929f) {
					setPosition (model[entry.bones[k]], point - (unit * depth + unit * depth));
					distance = full;
					k = 1;
				    }
				}

				if (k++ + 2 >= n) {
				    break;
				}
			    }
			}
		    }
		}
	    }
	}

	// backward pass, done within sqrt (0.1)
	bool reached = true;

	for (const auto& link : chain.links) {
	    for (const auto& entry : link.entries) {
		if (entry.flags & 4) {
		    constrainRope (rig, entry, endSettings (ik, entry.endBone), world, inverse, joints, true);
		    continue;
		}

		const bool fixedRoot = link.bone == chain.root && parents[chain.root] != -1;
		setPosition (model[link.bone], fixedRoot ? root : position (model[link.bone]));

		for (size_t i = 0; i + 1 < entry.bones.size (); i++) {
		    const glm::vec3 base = position (model[entry.bones[i]]);
		    const glm::vec3 toNext = position (model[entry.bones[i + 1]]) - base;
		    glm::vec3 direction = toNext * (1.0f / std::sqrt (glm::dot (toNext, toNext)));
		    const int bone = entry.bones[i];

		    // sub_14026DF30
		    if (i > 0 && static_cast<size_t> (bone) < ik.size () && (ik[bone].flags & PuppetBoneIK::Constrained)
			&& valid (parents[bone])) {
			limitPuppetJointDirection (ik[bone], base, position (model[parents[bone]]), direction);
		    }

		    setPosition (model[entry.bones[i + 1]], length (rig, entry.bones[i + 1]) * direction + base);
		}

		if (entry.flags & 1) {
		    if (const auto* target = extraOf (rig.targets, entry.endBone); target != nullptr) {
			const glm::vec3 miss = position (model[entry.endBone]) - position (*target);

			if (glm::dot (miss, miss) > 0.1f) {
			    reached = false;
			}
		    }
		}
	    }
	}

	if (reached) {
	    break;
	}
    }

    // rotate joints from bind pose onto the solved directions
    for (const auto& link : chain.links) {
	const bool shared = link.entries.size () != 1;

	for (const auto& entry : link.entries) {
	    for (size_t i = shared ? 1 : 0; i + 1 < entry.bones.size (); i++) {
		const int bone = entry.bones[i];
		const int child = entry.bones[i + 1];
		const glm::vec3* rest = childDirection (rig, bone, child);

		if (rest == nullptr) {
		    continue;
		}

		const glm::vec3 toChild = position (model[child]) - position (model[bone]);
		const glm::vec3 direction = toChild * (1.0f / std::sqrt (glm::dot (toChild, toChild)));
		rotateRows (model[bone], arc (*rest, direction), bindModel[bone]);
	    }

	    if ((entry.flags & 1) == 0) {
		continue;
	    }

	    const uint32_t end = entry.endBone;
	    const uint32_t endFlags = end < ik.size () ? ik[end].flags : 0;

	    if (endFlags & PuppetBoneIK::FollowParent) {
		if (entry.bones.size () > 2) {
		    const glm::mat4& before = model[entry.bones[entry.bones.size () - 2]];
		    model[end][0] = before[0];
		    model[end][1] = before[1];
		    model[end][2] = before[2];
		}
		continue;
	    }

	    const glm::mat4* target = extraOf (rig.targets, end);

	    if (target == nullptr) {
		continue;
	    }

	    glm::mat4 rows (1.0f);
	    rows[0] = fastNormalize ((*target)[0]);
	    rows[1] = fastNormalize ((*target)[1]);
	    rows[2] = fastNormalize ((*target)[2]);

	    // "ikr": turn short ends towards the target
	    if (endFlags & PuppetBoneIK::RotateToTarget) {
		const glm::vec3 miss = position (*target) - position (model[end]);
		const float squared = glm::dot (miss, miss);

		if (squared > 1.0f) {
		    const float distance = std::sqrt (squared);
		    const float t = std::min ((distance - 1.0f) / ik[end].rotateDistance, 1.0f);
		    const glm::vec4 towards = puppetRotationArc (glm::vec3 (1.0f, 0.0f, 0.0f), miss / distance);
		    const glm::mat3 turned = puppetQuatRows (puppetSlerp (quatFromRows (rows), towards, t));

		    rows[0] = glm::vec4 (turned[0], 0.0f);
		    rows[1] = glm::vec4 (turned[1], 0.0f);
		    rows[2] = glm::vec4 (turned[2], 0.0f);
		}
	    }

	    model[end][0] = rows[0];
	    model[end][1] = rows[1];
	    model[end][2] = rows[2];
	}

	// average the arcs of several entries
	if (!shared || link.entries.empty ()) {
	    continue;
	}

	glm::vec4 average (1.0f, 0.0f, 0.0f, 0.0f);
	const float weight = 1.0f / static_cast<float> (link.entries.size ());

	for (const auto& entry : link.entries) {
	    if (entry.bones.size () < 2) {
		continue;
	    }

	    const glm::vec3* rest = childDirection (rig, static_cast<int> (link.bone), entry.bones[1]);

	    if (rest == nullptr) {
		continue;
	    }

	    const glm::vec3 toChild = position (model[entry.bones[1]]) - position (model[link.bone]);
	    const glm::vec3 direction = toChild * (1.0f / std::sqrt (glm::dot (toChild, toChild)));
	    average = puppetSlerp (average, puppetRotationArc (*rest, direction), weight);
	}

	rotateRows (model[link.bone], average, bindModel[link.bone]);
    }
}
} // namespace WallpaperEngine::Render::Objects
