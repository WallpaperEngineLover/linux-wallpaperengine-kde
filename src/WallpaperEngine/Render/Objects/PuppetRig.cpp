#include "PuppetRig.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <string_view>
#include <strings.h>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Parsers/ObjectParser.h"
#include "WallpaperEngine/Data/Utils/BinaryReader.h"
#include "WallpaperEngine/Data/Utils/MemoryStream.h"
#include "WallpaperEngine/Logging/Log.h"

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render::Objects;
using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Data::Utils;

extern float g_Time;
extern float g_TimeLast;

namespace {
constexpr uint32_t PuppetClockMirror = 1;
constexpr uint32_t PuppetClockSingle = 2;
constexpr uint32_t PuppetClockFrameSet = 0x2000000;
constexpr uint32_t PuppetClockPaused = 0x20000000;
constexpr uint32_t PuppetClockStopped = 0x40000000;
constexpr uint32_t PuppetClockBackwards = 0x80000000;

// WE's timeline step (2.8.42 sub_1401A9F60, fmodf results taken from the asm): loops wrap, mirrors turn around at
// either end, single clips stop on their end
void stepPuppetClock (
    PuppetActiveAnimation& clock, float duration, float delta, std::vector<std::string>* fired = nullptr
) {
    if ((clock.flags & (PuppetClockPaused | PuppetClockStopped)) != 0
	|| ((clock.flags & PuppetClockSingle) != 0 && clock.time >= duration) || duration <= 0.0f) {
	return;
    }

    if ((clock.flags & PuppetClockBackwards) != 0) {
	delta = -delta;
    }

    const float time = clock.time + delta;
    const auto fire = [&clock, fired] (const auto& passed) {
	if (fired == nullptr) {
	    return;
	}

	for (const auto& event : clock.clip.events) {
	    if (passed (event.time)) {
		fired->push_back (event.payload);
	    }
	}
    };

    // events passed since the last time, including across a loop wrap (0x1401aa0b9)
    if (delta > 0.0f) {
	fire ([&clock, time] (float at) { return at >= clock.time && time > at; });
    } else {
	fire ([&clock, time] (float at) { return at > time && clock.time >= at; });
    }

    clock.time = time;

    if ((clock.flags & PuppetClockSingle) != 0) {
	if (time >= duration) {
	    clock.flags |= PuppetClockStopped;
	    clock.time = duration;
	}
	return;
    }

    if ((clock.flags & PuppetClockMirror) == 0) {
	if (time < 0.0f) {
	    clock.time = std::fmod (time + duration, duration);

	    if (clock.time >= 0.0f) {
		fire ([&clock, duration] (float at) { return at > clock.time && duration >= at; });
	    }
	}
	if (clock.time >= duration) {
	    clock.time = std::fmod (clock.time, duration);

	    if (duration > clock.time) {
		fire ([&clock] (float at) { return at >= 0.0f && clock.time > at; });
	    }
	}
	return;
    }

    if ((clock.flags & PuppetClockBackwards) != 0) {
	if (time <= 0.0f) {
	    clock.time = -std::fmod (time, duration);
	    clock.flags &= ~PuppetClockBackwards;
	}
	return;
    }

    if (time >= duration) {
	clock.flags |= PuppetClockBackwards;
	clock.time = duration - std::fmod (time, duration);
    }
}

// sub_14026C8B0: blend, faded in over the first min(duration / 2, blendtime) seconds and out over the last ones. A
// clip that doesn't stop on its end drops the fade in once it is done
float puppetLayerWeight (PuppetActiveAnimation& layer, float duration) {
    const float blendTime = layer.layer->blendTime;
    const float ramp = std::min (duration * 0.5f, blendTime);
    const bool canFade = std::min (duration, blendTime) > std::numeric_limits<float>::epsilon ();
    float weight = layer.layer->blend->value->getFloat ();

    if (layer.blendIn) {
	const float fade = canFade ? std::min (layer.time / ramp, 1.0f) : 1.0f;
	weight *= fade;

	if ((layer.flags & PuppetClockSingle) == 0 && fade >= 1.0f) {
	    layer.blendIn = false;
	}
    }

    if (layer.blendOut && canFade) {
	weight *= std::min ((duration - layer.time) / ramp, 1.0f);
    }

    return weight;
}

struct PuppetBoneSet {
    std::vector<PuppetBone> bones;
    // Points at whatever section comes right after MDLS's second bone array: MDLA directly for
    // puppets with no attachment points, or MDAT (attachment points) otherwise - the caller has to
    // check which one it actually is.
    size_t nextSectionOffset = 0;
    // MDLS v2+ records after the bones, every MDLA clip carries one track per entry of each
    uint32_t extraCount = 0;
    uint32_t constraintCount = 0;
    std::vector<int> drawOrder;
    std::vector<int> hitOrder;
    std::vector<PuppetExtra> extras;
    PuppetIKRig ik;
};

void parsePuppetCapsules (const BinaryReader& reader, PuppetBoneSet& result, size_t nextSectionOffset);

// Parses the MDLS bones (local bind-pose transforms, parent hierarchy and the per-bone physics JSON) and the
// counts of the records after them that size the MDLA tracks. Inverse-bind matrices are derived from the bind
// pose by walking the parent chain; the file's own copy is skipped.
PuppetBoneSet parsePuppetBones (const BinaryReader& reader, size_t mdlsOffset) {
    reader.base ().seekg (static_cast<std::streamoff> (mdlsOffset), std::ios::beg);

    char header[9];
    reader.next (header, sizeof (header));

    const uint32_t nextSectionOffset = reader.nextUInt32 ();
    const uint32_t boneCount = reader.nextUInt32 ();

    // A bone count this large can only be a garbage read (wrong mdlsOffset or an unrecognized MDLS
    // layout), not a real rig
    constexpr uint32_t maxPlausibleBoneCount = 512;
    if (boneCount > maxPlausibleBoneCount) {
	sLog.error ("Puppet bone count (", boneCount, ") looks implausible, skipping puppet mesh skinning");
	return {};
    }

    PuppetBoneSet result;
    result.nextSectionOffset = nextSectionOffset;
    result.bones.reserve (boneCount);

    for (uint32_t i = 0; i < boneCount; i++) {
	// records start with a null-terminated name, empty for most rigs
	std::string name = reader.nextNullTerminatedString ();
	const uint32_t type = reader.nextUInt32 ();
	const int parent = reader.nextInt ();
	const uint32_t matrixBytes = reader.nextUInt32 ();

	glm::mat4 bindLocal (1.0f);
	if (matrixBytes == sizeof (float) * 16) {
	    float m[16];
	    for (float& value : m) {
		value = reader.nextFloat ();
	    }
	    // the file stores a row-vector-convention, row-major matrix; feeding the 16 values straight
	    // into glm's column-major constructor produces exactly its transpose, which is the
	    // column-vector matrix glm needs to compute M * v
	    bindLocal = glm::mat4 (
		m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]
	    );
	} else {
	    // an implausible byte count here means this bone record wasn't decoded correctly; bail out
	    // rather than seeking by an untrusted amount and reading whatever garbage follows as bones
	    constexpr uint32_t maxPlausibleMatrixBytes = 4096;
	    if (matrixBytes > maxPlausibleMatrixBytes) {
		sLog.error (
		    "Puppet bone ", i, " has an implausible matrix byte count (", matrixBytes, "), stopping here (",
		    result.bones.size (), " bone(s) kept)"
		);
		break;
	    }
	    reader.base ().seekg (static_cast<std::streamoff> (matrixBytes), std::ios::cur);
	}

	// trailing per-bone string, jiggle/physics JSON for some rigs
	const std::string physics = reader.nextNullTerminatedString ();

	result.bones.push_back (
	    PuppetBone { .name = std::move (name),
			 .parent = parent,
			 .bindLocal = bindLocal,
			 .restLocal = bindLocal,
			 .physics = PuppetBonePhysics::parse (physics),
			 .type = type,
			 .ik = PuppetBoneIK::parse (physics) }
	);
    }

    // the rest of MDLS as 2.8.42 reads it (sub_140261880), only the two counts matter here
    const int version = std::atoi (header + 4);

    if (version < 2 || result.bones.size () != boneCount) {
	return result;
    }

    uint16_t extraCount = 0;
    reader.next (reinterpret_cast<char*> (&extraCount), sizeof (extraCount));

    // name, bone, type (float, 0 target 1 pole), model space matrix
    const auto readMatrix = [&reader] () {
	float m[16];
	for (float& value : m) {
	    value = reader.nextFloat ();
	}
	return glm::mat4 (
	    m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]
	);
    };

    for (uint16_t i = 0; i < extraCount; i++) {
	(void)reader.nextNullTerminatedString ();
	PuppetExtra extra;
	extra.bone = reader.nextUInt32 ();
	extra.type = reader.nextUInt32 ();
	extra.restLocal = readMatrix ();
	result.extras.push_back (extra);
    }

    // a flag byte and then a matrix per bone and per extra record: the rest pose. The vertices and the inverse bind
    // matrices stay in the bone records' space (sub_1401FBAE0), the pose starts from these when they are there
    // (sub_1401FDF90), so a puppet whose parts sit apart in its texture's layout (3227072870) gets put together
    if (reader.next () != 0) {
	for (auto& bone : result.bones) {
	    float m[16];
	    for (float& value : m) {
		value = reader.nextFloat ();
	    }
	    bone.restLocal = glm::mat4 (
		m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]
	    );
	}

	for (auto& extra : result.extras) {
	    extra.restLocal = readMatrix ();
	}
    }

    const uint32_t constraintCount = reader.nextUInt32 ();
    result.ik.extraRules.resize (result.extras.size ());

    // blend rules go on an IK joint's target extra, otherwise on the bone
    for (uint32_t i = 0; i < constraintCount && reader.base ().good (); i++) {
	const uint32_t bone = reader.nextUInt32 ();
	PuppetConstraintRule rule;
	rule.weight = reader.nextUInt32 ();
	rule.target = reader.nextUInt32 ();
	rule.flags = version >= 4 ? reader.nextUInt32 () : 0;

	if (rule.flags & 2) {
	    rule.start = reader.nextFloat ();
	    rule.range = std::max (reader.nextFloat (), 0.00000011920929f);
	}

	if (bone >= result.bones.size ()) {
	    continue;
	}

	if (result.bones[bone].type & 2) {
	    for (size_t extra = 0; extra < result.extras.size (); extra++) {
		if (result.extras[extra].bone == bone && result.extras[extra].type == 0) {
		    result.ik.extraRules[extra].push_back (rule);
		    break;
		}
	    }
	} else {
	    result.bones[bone].rules.push_back (rule);
	}
    }

    if (!reader.base ().good () || static_cast<size_t> (reader.base ().tellg ()) > nextSectionOffset) {
	reader.base ().clear ();
	sLog.error ("Puppet MDLS records after the bones don't fit the section, animation tracks may not line up");
	return result;
    }

    result.extraCount = extraCount;
    result.constraintCount = constraintCount;

    // sub_140261880 goes on with two more blocks before the per bone collision capsules
    const auto nextUInt16 = [&reader] () {
	uint16_t value = 0;
	reader.next (reinterpret_cast<char*> (&value), sizeof (value));
	return value;
    };

    // bone lengths and child rest directions (P+224, P+248)
    auto& ik = result.ik;
    const uint16_t groups = nextUInt16 ();
    ik.lengths.resize (groups);
    for (float& length : ik.lengths) {
	length = reader.nextFloat ();
    }
    ik.childDirections.resize (groups);
    for (uint16_t i = 0; i < groups && reader.base ().good (); i++) {
	const uint16_t children = nextUInt16 ();
	for (uint16_t j = 0; j < children && reader.base ().good (); j++) {
	    const uint32_t child = reader.nextUInt32 ();
	    const float x = reader.nextFloat ();
	    const float y = reader.nextFloat ();
	    const float z = reader.nextFloat ();
	    ik.childDirections[i][child] = glm::vec3 (x, y, z);
	}
    }

    // IK chains (P+272)
    const uint16_t chains = nextUInt16 ();
    for (uint16_t i = 0; i < chains && reader.base ().good (); i++) {
	PuppetIKChain chain;
	chain.root = reader.nextUInt32 ();
	chain.extras.resize (reader.nextUInt32 ());
	for (uint32_t& extra : chain.extras) {
	    extra = reader.nextUInt32 ();
	}
	const uint16_t links = nextUInt16 ();
	for (uint16_t j = 0; j < links && reader.base ().good (); j++) {
	    PuppetIKLink link;
	    link.bone = reader.nextUInt32 ();
	    const uint16_t entries = nextUInt16 ();
	    for (uint16_t k = 0; k < entries && reader.base ().good (); k++) {
		PuppetIKEntry entry;
		entry.endBone = reader.nextUInt32 ();
		entry.flags = reader.nextUInt32 ();
		entry.length = reader.nextFloat ();
		entry.minReach = reader.nextFloat ();
		entry.bones.resize (nextUInt16 ());
		for (int& bone : entry.bones) {
		    bone = reader.nextInt ();
		}
		link.entries.push_back (std::move (entry));
	    }
	    chain.links.push_back (std::move (link));
	}
	ik.chains.push_back (std::move (chain));
    }

    // WE fastfails on bad bone indices, drop the chain instead
    const auto boneIndex
	= [&result] (int bone) { return bone >= 0 && static_cast<size_t> (bone) < result.bones.size (); };
    std::erase_if (ik.chains, [&boneIndex] (const PuppetIKChain& chain) {
	return !boneIndex (static_cast<int> (chain.root))
	    || std::ranges::any_of (chain.links, [&boneIndex] (const PuppetIKLink& link) {
		   return !boneIndex (static_cast<int> (link.bone))
		       || std::ranges::any_of (link.entries, [&boneIndex] (const PuppetIKEntry& entry) {
			      return !boneIndex (static_cast<int> (entry.endBone))
				  || !std::ranges::all_of (entry.bones, boneIndex);
			  });
	       });
    });

    if (reader.base ().good ()) {
	for (size_t i = 0; i < ik.chains.size (); i++) {
	    result.bones[ik.chains[i].root].chain = static_cast<int> (i);
	}

	for (uint32_t i = 0; i < result.extras.size (); i++) {
	    ik.extraBones.push_back (result.extras[i].bone);
	    auto& map = result.extras[i].type == 0 ? ik.targets : ik.poles;
	    if (result.extras[i].type <= 1) {
		map[result.extras[i].bone] = i;
	    }
	}
    } else {
	ik = {};
    }

    if (!reader.base ().good () || static_cast<size_t> (reader.base ().tellg ()) >= nextSectionOffset) {
	reader.base ().clear ();
	return result;
    }

    if (reader.next () != 0) {
	parsePuppetCapsules (reader, result, nextSectionOffset);
    }

    // flag byte + cursor box test order (sub_1401FD690), MDLS v3+ another flag byte + bone draw order
    const auto atEnd = [&reader, nextSectionOffset] () {
	return !reader.base ().good () || static_cast<size_t> (reader.base ().tellg ()) >= nextSectionOffset;
    };

    if (!atEnd () && reader.next () != 0) {
	result.hitOrder.resize (result.bones.size ());

	for (int& bone : result.hitOrder) {
	    bone = static_cast<int> (std::min<uint32_t> (reader.nextUInt32 (), result.bones.size () - 1));
	}
    }

    if (version >= 3 && !atEnd () && reader.next () != 0) {
	result.drawOrder.resize (result.bones.size ());

	for (int& order : result.drawOrder) {
	    order = reader.nextInt ();
	}
    }

    if (!reader.base ().good () || static_cast<size_t> (reader.base ().tellg ()) > nextSectionOffset) {
	reader.base ().clear ();
	result.drawOrder.clear ();
	result.hitOrder.clear ();
    }

    return result;
}

void parsePuppetCapsules (const BinaryReader& reader, PuppetBoneSet& result, size_t nextSectionOffset) {
    // a vec3 of extents and the capsule's frame in bone space (row-vector, row-major like the bind matrices)
    for (auto& bone : result.bones) {
	for (int axis = 0; axis < 3; axis++) {
	    bone.capsuleExtents[axis] = reader.nextFloat ();
	}
	float m[16];
	for (float& value : m) {
	    value = reader.nextFloat ();
	}
	bone.capsule = glm::mat4 (
	    m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]
	);
	bone.hasCapsule = true;
    }

    if (!reader.base ().good () || static_cast<size_t> (reader.base ().tellg ()) > nextSectionOffset) {
	reader.base ().clear ();
	sLog.error ("Puppet MDLS collision capsules don't fit the section, ignoring them");
	for (auto& bone : result.bones) {
	    bone.hasCapsule = false;
	}
    }
}

// Resolves each bone's world transform by walking up the parent chain: the MDL format doesn't
// guarantee parents come before their children, and some rigs (puppet eyes/eyebrows) break that order
void resolveBoneWorldTransform (
    size_t index, const std::vector<int>& parents, const std::vector<glm::mat4>& locals, std::vector<glm::mat4>& world,
    std::vector<bool>& resolved, std::vector<bool>& visiting
) {
    if (resolved[index]) {
	return;
    }

    const int parent = parents[index];
    // a missing parent, an out-of-range index, or a cycle back onto a bone still being resolved are
    // all treated the same way a genuine root bone would be: no parent transform to fold in
    if (parent < 0 || static_cast<size_t> (parent) >= parents.size () || visiting[index]) {
	world[index] = locals[index];
    } else {
	visiting[index] = true;
	resolveBoneWorldTransform (static_cast<size_t> (parent), parents, locals, world, resolved, visiting);
	visiting[index] = false;
	world[index] = world[static_cast<size_t> (parent)] * locals[index];
    }

    resolved[index] = true;
}

std::vector<glm::mat4>
composeBoneWorldTransforms (const std::vector<int>& parents, const std::vector<glm::mat4>& locals) {
    std::vector<glm::mat4> world (locals.size ());
    std::vector<bool> resolved (locals.size (), false);
    std::vector<bool> visiting (locals.size (), false);

    for (size_t i = 0; i < locals.size (); i++) {
	resolveBoneWorldTransform (i, parents, locals, world, resolved, visiting);
    }

    return world;
}

struct PuppetAttachmentPointSet {
    std::vector<PuppetAttachmentPoint> points;
    size_t mdlaOffset = 0;
};

// Parses the optional MDAT section (named attachment points other objects can follow, e.g. scene.json's "attachment":
// "orb"). 2.8.42 reads a u16 count, then per point a u16 bone, the name and a 64 byte matrix (sub_140261880). Keeps
// the points parsed so far and stops at the first implausible entry
PuppetAttachmentPointSet
parsePuppetAttachmentPoints (const BinaryReader& reader, size_t mdatOffset, uint32_t boneCount) {
    reader.base ().seekg (static_cast<std::streamoff> (mdatOffset), std::ios::beg);

    char header[9];
    reader.next (header, sizeof (header));

    PuppetAttachmentPointSet result;
    result.mdlaOffset = reader.nextUInt32 ();

    uint16_t pointCount = 0;
    reader.next (reinterpret_cast<char*> (&pointCount), sizeof (pointCount));

    // every point starts with its bone, read one ahead
    uint16_t nextBoneIndex = 0;
    reader.next (reinterpret_cast<char*> (&nextBoneIndex), sizeof (nextBoneIndex));

    constexpr uint16_t maxPlausiblePointCount = 256;
    if (pointCount > maxPlausiblePointCount) {
	sLog.error ("Puppet attachment point count (", pointCount, ") looks implausible, ignoring attachment points");
	return result;
    }

    for (uint16_t i = 0; i < pointCount; i++) {
	const std::string name = reader.nextNullTerminatedString ();

	float m[16];
	for (float& value : m) {
	    value = reader.nextFloat ();
	}

	const uint16_t boneIndex = nextBoneIndex;
	if (i + 1 < pointCount) {
	    reader.next (reinterpret_cast<char*> (&nextBoneIndex), sizeof (nextBoneIndex));
	}

	if (name.empty () || boneIndex >= boneCount) {
	    sLog.error (
		"Puppet attachment point ", i, " (name=", name, ", bone=", boneIndex,
		") looks implausible, stopping here (", result.points.size (), " point(s) kept)"
	    );
	    break;
	}

	// same row-major-to-column-major transpose trick used for PuppetBone::bindLocal
	const glm::mat4 localTransform (
	    m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]
	);

	result.points.push_back (
	    PuppetAttachmentPoint { .name = name, .boneIndex = boneIndex, .localTransform = localTransform }
	);
    }

    return result;
}

// Parses every baked animation clip out of the MDLA section (see docs/rendering/MDL_FILES.md), laid out like
// 2.8.42 reads it (sub_140261880). Only the bone tracks are used, everything after them is skipped by its size.
std::vector<PuppetAnimationClip> parsePuppetAnimationClips (
    const BinaryReader& reader, size_t mdlaOffset, uint32_t expectedBoneCount, const PuppetBoneSet& rig,
    uint32_t meshCount
) {
    reader.base ().seekg (static_cast<std::streamoff> (mdlaOffset), std::ios::beg);

    char header[9];
    reader.next (header, sizeof (header));
    const int version = std::atoi (header + 4);

    const uint32_t sectionEnd = reader.nextUInt32 ();
    const uint32_t clipCount = reader.nextUInt32 ();

    constexpr uint32_t maxPlausibleClipCount = 64;
    if (clipCount > maxPlausibleClipCount) {
	sLog.error ("Puppet animation clip count (", clipCount, ") looks implausible, skipping animation entirely");
	return {};
    }

    std::vector<PuppetAnimationClip> clips;
    clips.reserve (clipCount);

    for (uint32_t clipIndex = 0; clipIndex < clipCount; clipIndex++) {
	PuppetAnimationClip clip;
	reader.next (reinterpret_cast<char*> (&clip.id), sizeof (clip.id));
	clip.name = reader.nextNullTerminatedString ();
	clip.mode = reader.nextNullTerminatedString ();
	clip.fps = reader.nextFloat ();
	clip.frameCount = reader.nextUInt32 ();
	const uint32_t flags = reader.nextUInt32 ();
	const uint32_t boneCount = reader.nextUInt32 ();

	constexpr uint32_t maxPlausibleFrameCount = 100000;
	if (clip.frameCount > maxPlausibleFrameCount || boneCount != expectedBoneCount) {
	    sLog.error (
		"Puppet animation clip ", clipIndex, " has an implausible frame/bone count (frames=", clip.frameCount,
		", bones=", boneCount, ", expected ", expectedBoneCount, "), stopping here"
	    );
	    break;
	}

	const uint32_t sampleCount = clip.frameCount + 1;
	bool valid = true;

	clip.boneTracks.resize (boneCount);
	clip.boneAnimated.assign (boneCount, true);
	clip.boneFlags.assign (boneCount, 0);

	for (uint32_t boneIndex = 0; boneIndex < boneCount && valid; boneIndex++) {
	    // bit 0 keeps the bone out of this clip, the blend masks it (2.8.42 sub_140261880)
	    clip.boneFlags[boneIndex] = reader.nextUInt32 ();
	    clip.boneAnimated[boneIndex] = (clip.boneFlags[boneIndex] & 1) == 0;
	    const uint32_t trackBytes = reader.nextUInt32 ();

	    if (trackBytes != sampleCount * 9 * sizeof (float)) {
		valid = false;
		break;
	    }

	    auto& track = clip.boneTracks[boneIndex];
	    track.reserve (sampleCount);

	    for (uint32_t sample = 0; sample < sampleCount; sample++) {
		PuppetKeyframe keyframe;
		keyframe.position = { reader.nextFloat (), reader.nextFloat (), reader.nextFloat () };
		keyframe.rotation = { reader.nextFloat (), reader.nextFloat (), reader.nextFloat () };
		keyframe.scale = { reader.nextFloat (), reader.nextFloat (), reader.nextFloat () };
		// the loader turns the euler angles into a quaternion right away, same order as the matrices
		keyframe.orientation = glm::angleAxis (keyframe.rotation.z, glm::vec3 (0.0f, 0.0f, 1.0f))
		    * glm::angleAxis (keyframe.rotation.y, glm::vec3 (0.0f, 1.0f, 0.0f))
		    * glm::angleAxis (keyframe.rotation.x, glm::vec3 (1.0f, 0.0f, 0.0f));
		track.push_back (keyframe);
	    }
	}

	if (version >= 2) {
	    clip.extraTracks.resize (rig.extraCount);
	    clip.extraAnimated.assign (rig.extraCount, true);

	    for (uint32_t extra = 0; extra < rig.extraCount && valid; extra++) {
		clip.extraAnimated[extra] = (reader.nextUInt32 () & 1) == 0;

		if (reader.nextUInt32 () != sampleCount * 9 * sizeof (float)) {
		    valid = false;
		    break;
		}

		auto& track = clip.extraTracks[extra];
		track.reserve (sampleCount);

		for (uint32_t sample = 0; sample < sampleCount; sample++) {
		    PuppetKeyframe keyframe;
		    keyframe.position = { reader.nextFloat (), reader.nextFloat (), reader.nextFloat () };
		    keyframe.rotation = { reader.nextFloat (), reader.nextFloat (), reader.nextFloat () };
		    keyframe.scale = { reader.nextFloat (), reader.nextFloat (), reader.nextFloat () };
		    keyframe.orientation = glm::angleAxis (keyframe.rotation.z, glm::vec3 (0.0f, 0.0f, 1.0f))
			* glm::angleAxis (keyframe.rotation.y, glm::vec3 (0.0f, 1.0f, 0.0f))
			* glm::angleAxis (keyframe.rotation.x, glm::vec3 (1.0f, 0.0f, 0.0f));
		    track.push_back (keyframe);
		}
	    }

	    clip.constraintTracks.resize (rig.constraintCount);
	    clip.constraintFlags.assign (rig.constraintCount, 0);

	    for (uint32_t constraint = 0; constraint < rig.constraintCount && valid; constraint++) {
		clip.constraintFlags[constraint] = reader.nextUInt32 ();

		if (reader.nextUInt32 () != sampleCount * sizeof (float)) {
		    valid = false;
		    break;
		}

		clip.constraintTracks[constraint].resize (sampleCount);

		for (float& sample : clip.constraintTracks[constraint]) {
		    sample = reader.nextFloat ();
		}
	    }
	}

	if (version >= 3 && valid) {
	    // g_BlendMap tracks (clip +216)
	    const uint32_t blendTracks = reader.nextUInt32 ();
	    clip.blendTracks.resize (blendTracks);

	    for (uint32_t track = 0; track < blendTracks && valid; track++) {
		(void)reader.nextUInt32 ();

		if (reader.nextUInt32 () != sampleCount * sizeof (float)) {
		    valid = false;
		    break;
		}

		clip.blendTracks[track].resize (sampleCount);

		for (float& sample : clip.blendTracks[track]) {
		    sample = reader.nextFloat ();
		}
	    }

	    // bone alpha tracks (clip +240)
	    if (valid && reader.next () != 0) {
		clip.boneAlphaTracks.resize (boneCount);

		for (uint32_t boneIndex = 0; boneIndex < boneCount && valid; boneIndex++) {
		    (void)reader.nextUInt32 ();
		    const uint32_t trackBytes = reader.nextUInt32 ();

		    if (trackBytes != sampleCount * sizeof (float)) {
			valid = false;
			break;
		    }

		    auto& track = clip.boneAlphaTracks[boneIndex];
		    track.resize (sampleCount);

		    for (float& sample : track) {
			sample = reader.nextFloat ();
		    }
		}

		if (!valid) {
		    clip.boneAlphaTracks.clear ();
		}
	    }
	}

	// per mesh morph weight tracks: a u32 whose bit 0 enables them, a u32, a u16 count and per track the target
	// and its samples
	if (version >= 4 && valid && reader.next () != 0) {
	    clip.morphTracks.resize (meshCount);

	    for (uint32_t mesh = 0; mesh < meshCount && valid; mesh++) {
		if ((reader.nextUInt32 () & 1) == 0) {
		    continue;
		}

		auto& meshTracks = clip.morphTracks[mesh];
		meshTracks.enabled = true;
		(void)reader.nextUInt32 ();
		uint16_t count = 0;
		reader.next (reinterpret_cast<char*> (&count), sizeof (count));

		for (uint16_t i = 0; i < count && valid; i++) {
		    PuppetAnimationClip::MorphTrack track;
		    reader.next (reinterpret_cast<char*> (&track.target), sizeof (track.target));
		    const uint32_t trackBytes = reader.nextUInt32 ();

		    if (trackBytes != sampleCount * sizeof (float)) {
			valid = false;
			break;
		    }

		    track.samples.resize (sampleCount);
		    for (float& sample : track.samples) {
			sample = reader.nextFloat ();
		    }

		    meshTracks.tracks.push_back (std::move (track));
		}
	    }
	}

	if (version >= 5 && valid) {
	    for (int axis = 0; axis < 3; axis++) {
		clip.boundsMin[axis] = reader.nextFloat ();
	    }
	    for (int axis = 0; axis < 3; axis++) {
		clip.boundsMax[axis] = reader.nextFloat ();
	    }
	}

	// a flag byte and one float track per bone, the bones' draw order offsets (clip +264)
	if (version >= 6 && valid && reader.next () != 0) {
	    clip.drawOrderTracks.resize (boneCount);

	    for (uint32_t boneIndex = 0; boneIndex < boneCount && valid; boneIndex++) {
		(void)reader.nextUInt32 ();
		const uint32_t trackBytes = reader.nextUInt32 ();

		if (trackBytes != sampleCount * sizeof (float)) {
		    valid = false;
		    break;
		}

		auto& track = clip.drawOrderTracks[boneIndex];
		track.resize (sampleCount);

		for (float& sample : track) {
		    sample = reader.nextFloat ();
		}
	    }

	    if (!valid) {
		clip.drawOrderTracks.clear ();
	    }
	}

	clip.flags = flags;

	if ((flags & 1) && valid) {
	    PuppetAnimationClip::RootMotion record;
	    reader.next (reinterpret_cast<char*> (&record.clip), sizeof (record.clip));
	    record.frameStart = reader.nextUInt32 ();
	    record.frameEnd = reader.nextUInt32 ();
	    record.startOffset = reader.nextUInt32 ();
	    record.bone = static_cast<int> (reader.nextUInt32 ());

	    // the clip it plays has to come before it; WE stops loading the file otherwise
	    if (record.clip >= clips.size ()) {
		sLog.error ("Animation clip ", clip.name, " refers to clip ", record.clip, " which isn't loaded yet");
	    } else {
		// with root motion the loader samples the root bone at the first and the last frame: the ones the
		// record names in the clip it plays, or with 0x400 frame 0 and the end of this clip's own samples
		if ((flags & 0x1F800) != 0) {
		    const bool own = (flags & 0x400) != 0;
		    const PuppetAnimationClip& source = own ? clip : clips[record.clip];
		    const uint32_t first = own ? 0 : record.frameStart;
		    const uint32_t last = own ? clip.frameCount : record.frameEnd;

		    record.frames.first = samplePuppetBoneChain (rig.bones, source, record.bone, first, first, 0.0f);
		    record.frames.inverseFirst = glm::inverse (glm::mat3 (record.frames.first));
		    record.frames.last = samplePuppetBoneChain (rig.bones, source, record.bone, last, last, 0.0f);
		}

		clip.rootMotion = record;
	    }
	}

	if (valid) {
	    const uint32_t eventCount = reader.nextUInt32 ();

	    for (uint32_t i = 0; i < eventCount && reader.base ().good (); i++) {
		PuppetClipEvent event;
		event.time = reader.nextFloat ();
		event.payload = reader.nextNullTerminatedString ();
		clip.events.push_back (std::move (event));
	    }
	}

	if (!valid || !reader.base ().good ()) {
	    reader.base ().clear ();
	    sLog.error ("Puppet animation clip ", clip.name, " doesn't match the MDLA layout, stopping here");
	    if (!valid) {
		break;
	    }
	}

	clips.push_back (std::move (clip));
    }

    if (clips.size () == clipCount && static_cast<size_t> (reader.base ().tellg ()) != sectionEnd) {
	sLog.error (
	    "Puppet MDLA clips end at ", static_cast<size_t> (reader.base ().tellg ()), " but the section ends at ",
	    sectionEnd
	);
    }

    return clips;
}
glm::vec3 lerp (const glm::vec3& a, const glm::vec3& b, float alpha) { return a + (b - a) * alpha; }

// glm::slerp as WE's sample code has it inlined: the shorter way round, plain lerp once the angle gets tiny
glm::quat slerp (const glm::quat& a, glm::quat b, float alpha) {
    float cosine = glm::dot (a, b);

    if (cosine < 0.0f) {
	b = -b;
	cosine = -cosine;
    }

    if (cosine > 1.0f - std::numeric_limits<float>::epsilon ()) {
	return a * (1.0f - alpha) + b * alpha;
    }

    const float angle = std::acos (cosine);
    return (a * std::sin ((1.0f - alpha) * angle) + b * std::sin (alpha * angle)) / std::sin (angle);
}
} // namespace

glm::mat4 WallpaperEngine::Render::Objects::samplePuppetBoneChain (
    const std::vector<PuppetBone>& bones, const PuppetAnimationClip& clip, int bone, uint32_t frame0, uint32_t frame1,
    float alpha
) {
    // sub_140267F00 (sub_140267580 for one frame): the bone's local matrices up the parent chain, translate * rotate
    // without the scale, the rotation slerped
    glm::mat4 result (1.0f);

    for (size_t depth = 0; bone >= 0 && static_cast<size_t> (bone) < bones.size () && depth < bones.size (); depth++) {
	if (static_cast<size_t> (bone) >= clip.boneTracks.size ()) {
	    break;
	}

	const auto& track = clip.boneTracks[bone];

	if (frame0 >= track.size () || frame1 >= track.size ()) {
	    break;
	}

	const glm::vec3 position = track[frame0].position * (1.0f - alpha) + track[frame1].position * alpha;
	const glm::quat orientation = slerp (track[frame0].orientation, track[frame1].orientation, alpha);

	result = glm::translate (glm::mat4 (1.0f), position) * glm::mat4_cast (orientation) * result;
	bone = bones[bone].parent;
    }

    return result;
}

void PuppetRig::clear () { *this = PuppetRig (); }

void PuppetRig::load (const std::vector<char>& data, size_t mdlsOffset, uint32_t meshCount, const std::string& name) {
    this->clear ();

    auto buffer = std::make_unique<char[]> (data.size ());
    std::copy (data.begin (), data.end (), buffer.get ());
    const BinaryReader reader (std::make_shared<MemoryStream> (std::move (buffer), data.size ()));

    auto boneSet = parsePuppetBones (reader, mdlsOffset);

    std::vector<int> bindParents (boneSet.bones.size ());
    std::vector<glm::mat4> bindLocals (boneSet.bones.size ());
    for (size_t i = 0; i < boneSet.bones.size (); i++) {
	bindParents[i] = boneSet.bones[i].parent;
	bindLocals[i] = boneSet.bones[i].bindLocal;
    }

    const std::vector<glm::mat4> worldBind = composeBoneWorldTransforms (bindParents, bindLocals);
    for (size_t i = 0; i < boneSet.bones.size (); i++) {
	boneSet.bones[i].inverseBindWorld = glm::inverse (worldBind[i]);
    }

    this->bones = std::move (boneSet.bones);
    this->boneDrawOrder = std::move (boneSet.drawOrder);
    this->boneHitOrder = std::move (boneSet.hitOrder);
    this->boneModel = worldBind;
    this->bindModel = worldBind;
    this->extras = boneSet.extras;
    this->ik = std::move (boneSet.ik);
    this->hasIK = !this->ik.chains.empty ();
    this->extraModel.resize (this->extras.size ());
    for (size_t i = 0; i < this->extras.size (); i++) {
	this->extraModel[i] = this->extras[i].restLocal;
    }
    this->physicsState.assign (this->bones.size (), {});
    this->ropeJoints.assign (this->bones.size (), {});
    this->constraintWeights.assign (boneSet.constraintCount, 0.0f);
    this->hasPhysics
	= std::ranges::any_of (this->bones, [] (const PuppetBone& bone) { return bone.physics.simulated (); });
    this->hasRestPose
	= std::ranges::any_of (this->bones, [] (const PuppetBone& bone) { return bone.restLocal != bone.bindLocal; });

    // sub_140261880 walks the sections after MDLS in whatever order they come: every one is "TAGnnnn\0" and the
    // absolute offset of the next one, an empty tag or the end of the file ends it. MDAT holds attachment points, MDMP
    // morph targets (read by the model), MDLA the clips
    size_t offset = boneSet.nextSectionOffset;
    std::set<size_t> visited;

    while (offset + 13 <= data.size () && data[offset] != 0 && visited.insert (offset).second) {
	const std::string tag (data.data () + offset, 4);
	uint32_t next = 0;
	std::memcpy (&next, data.data () + offset + 9, sizeof (next));

	if (tag == "MDAT") {
	    auto attachmentSet
		= parsePuppetAttachmentPoints (reader, offset, static_cast<uint32_t> (this->bones.size ()));
	    this->attachmentPoints = std::move (attachmentSet.points);
	} else if (tag == "MDMP") {
	    this->morphSection = offset;
	} else if (tag == "MDLE" && std::string_view (data.data () + offset, 8) == "MDLE0002") {
	    // skipped byte size, then a row-major matrix per bone
	    const size_t matrices = offset + 17;
	    if (matrices + this->bones.size () * 64 <= data.size ()) {
		this->layerImageBindLocal.resize (this->bones.size ());
		for (size_t bone = 0; bone < this->bones.size (); bone++) {
		    float m[16];
		    std::memcpy (m, data.data () + matrices + bone * 64, sizeof (m));
		    this->layerImageBindLocal[bone] = glm::mat4 (
			m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14],
			m[15]
		    );
		}
	    }
	} else if (tag == "MDLA") {
	    this->clips = parsePuppetAnimationClips (
		reader, offset, static_cast<uint32_t> (this->bones.size ()), boneSet, meshCount
	    );
	}

	if (next <= offset) {
	    sLog.error ("Section ", tag, " of ", name, " doesn't point past itself, the sections after it are skipped");
	    break;
	}

	offset = next;
    }

    if (!this->attachmentPoints.empty ()) {
	std::string names;
	for (const auto& point : this->attachmentPoints) {
	    names += (names.empty () ? "" : ", ") + point.name;
	}
	sLog.out ("Found ", this->attachmentPoints.size (), " attachment point(s) on ", name, ": ", names);
    }
}

void PuppetRig::addSceneLayers (const std::vector<ImageAnimationLayerUniquePtr>& sceneLayers) {
    // a layer plays the clip whose id is its "animation" value, layers without a match are dropped (2.8.42
    // sub_1401FCC20); layer and clip names don't have to agree (3521337568's "j" plays "动画 1")
    for (size_t index = 0; index < sceneLayers.size (); index++) {
	this->addLayer (*sceneLayers[index], nullptr, index, false);
    }

    this->nextLayerSerial = sceneLayers.size ();

    for (const auto& active : this->layers) {
	sLog.out (
	    "Playing animation ", active.clip.name, " (", active.clip.mode, ", ", active.clip.fps, " fps, ",
	    active.clip.frameCount, " frames)"
	);
    }
}

std::vector<glm::mat4> PuppetRig::skinMatrices () const {
    std::vector<glm::mat4> skin (this->bones.size ());

    for (size_t i = 0; i < this->bones.size (); i++) {
	skin[i]
	    = (i < this->boneModel.size () ? this->boneModel[i] : glm::mat4 (1.0f)) * this->bones[i].inverseBindWorld;
    }

    return skin;
}

bool PuppetRig::clipBounds (glm::vec3& min, glm::vec3& max) const {
    glm::vec3 lower (std::numeric_limits<float>::max ());
    glm::vec3 upper (-std::numeric_limits<float>::max ());
    bool found = false;

    for (const auto& layer : this->layers) {
	if (layer.layer == nullptr || !layer.layer->visible->value->getBool ()) {
	    continue;
	}

	found = found || layer.clip.boundsMax.x > layer.clip.boundsMin.x;
	lower = glm::min (lower, layer.clip.boundsMin);
	upper = glm::max (upper, layer.clip.boundsMax);
    }

    if (found) {
	min = lower;
	max = upper;
    }

    return found;
}

PuppetActiveAnimation* PuppetRig::findLayer (size_t serial) {
    const auto it = std::ranges::find (this->layers, serial, &PuppetActiveAnimation::serial);

    return it == this->layers.end () ? nullptr : &*it;
}

// sub_1401FCC20: the layer plays the clip whose id is its "animation", without one it isn't created. autosort puts it
// after the last non-additive layer, "index" at that position (never past the last one), otherwise it goes last
bool PuppetRig::addLayer (
    const ImageAnimationLayer& layer, ImageAnimationLayerUniquePtr owned, size_t serial, bool autoRemove
) {
    if (!layer.animation) {
	return false;
    }

    const auto id = static_cast<uint64_t> (layer.animation->value->getInt ());
    const auto match = std::ranges::find (this->clips, id, &PuppetAnimationClip::id);

    if (match == this->clips.end ()) {
	return false;
    }

    // sub_1401A8C10: "mirror" sets flag 1, "single" flag 2, anything else loops; starts at 0, playing
    uint32_t flags = 0;
    if (strcasecmp (match->mode.c_str (), "mirror") == 0) {
	flags |= PuppetClockMirror;
    } else if (strcasecmp (match->mode.c_str (), "single") == 0) {
	flags |= PuppetClockSingle;
    }

    PuppetActiveAnimation entry {
	.clip = *match,
	.layer = &layer,
	.ownedLayer = std::move (owned),
	.serial = serial,
	.blendIn = layer.blendIn,
	// only a clip that stops on its end fades out
	.blendOut = layer.blendOut && (flags & PuppetClockSingle) != 0,
	.autoRemove = autoRemove,
	.flags = flags,
    };

    auto& list = this->layers;
    auto position = list.end ();

    if (layer.autosort) {
	position = std::find_if (list.rbegin (), list.rend (), [] (const PuppetActiveAnimation& other) {
		       return !other.layer->additive;
		   }).base ();
    } else if (layer.index.has_value () && !list.empty ()) {
	const auto index = std::clamp<int64_t> (*layer.index, 0, static_cast<int64_t> (list.size ()) - 1);
	position = list.begin () + index;
    }

    list.insert (position, std::move (entry));
    return true;
}

size_t PuppetRig::getLayerCount () const { return this->layers.size (); }

std::optional<size_t> PuppetRig::getLayerAt (int64_t index) const {
    if (index < 0 || index >= static_cast<int64_t> (this->layers.size ())) {
	return std::nullopt;
    }

    return this->layers[static_cast<size_t> (index)].serial;
}

std::optional<size_t> PuppetRig::findLayerByName (const std::string& name) const {
    // sub_14020E910 keeps the last layer with that name
    std::optional<size_t> result = std::nullopt;

    if (name.empty ()) {
	return result;
    }

    for (const auto& layer : this->layers) {
	if (layer.layer->name == name) {
	    result = layer.serial;
	}
    }

    return result;
}

// sub_14020EA30: a config without blendin/blendout fades both ways. A clip name looks the clip up by name and puts its
// id into the config, an object is the layer JSON itself with the config's keys written over it
std::optional<size_t> PuppetRig::createLayer (
    const Data::JSON::JSON& animation, const Data::JSON::JSON& config, bool autoRemove, const Project& project
) {
    if (this->clips.empty ()) {
	return std::nullopt;
    }

    auto settings = config.is_object () ? config : Data::JSON::JSON::object ();

    if (!settings.contains ("blendin")) {
	settings["blendin"] = true;
    }
    if (!settings.contains ("blendout")) {
	settings["blendout"] = true;
    }

    Data::JSON::JSON layerJson;

    if (animation.is_string ()) {
	const auto name = animation.get<std::string> ();
	const auto clip = std::ranges::find (this->clips, name, &PuppetAnimationClip::name);

	if (name.empty () || clip == this->clips.end () || clip->id == 0) {
	    return std::nullopt;
	}

	layerJson = std::move (settings);
	layerJson["animation"] = clip->id;
    } else if (animation.is_object ()) {
	layerJson = animation;

	for (const auto& [key, value] : settings.items ()) {
	    layerJson[key] = value;
	}
    } else {
	return std::nullopt;
    }

    if (!layerJson.contains ("animation") || !layerJson["animation"].is_number ()) {
	return std::nullopt;
    }

    const size_t serial = this->nextLayerSerial++;

    // sub_1401A38F0 hands out a fresh id when the config has none
    if (!layerJson.contains ("id") || !layerJson["id"].is_number ()) {
	layerJson["id"] = static_cast<int> (serial);
    }

    ImageAnimationLayerUniquePtr owned;

    try {
	owned = ObjectParser::parseAnimationLayer (layerJson, project);
    } catch (const std::exception& ex) {
	sLog.error ("createAnimationLayer: invalid layer config: ", ex.what ());
	return std::nullopt;
    }

    const auto& layer = *owned;

    if (!this->addLayer (layer, std::move (owned), serial, autoRemove)) {
	return std::nullopt;
    }

    return serial;
}

bool PuppetRig::destroyLayersByName (const std::string& name) {
    if (name.empty ()) {
	return false;
    }

    return this->removeLayers ([&name] (const PuppetActiveAnimation& layer) { return layer.layer->name == name; }) > 0;
}

bool PuppetRig::destroyLayer (size_t serial) {
    return this->removeLayers ([serial] (const PuppetActiveAnimation& layer) { return layer.serial == serial; }) > 0;
}

// sub_1401FDF90, after the pose: a layer that ended runs its ended callbacks, a playSingleAnimation() one is removed
std::vector<std::string> PuppetRig::takeFiredEvents () { return std::exchange (this->firedEvents, {}); }

void PuppetRig::finishEndedLayers (const std::function<void (size_t)>& dispatch) {
    std::vector<size_t> ended;

    for (const auto& layer : this->layers) {
	if (layer.ended) {
	    ended.push_back (layer.serial);
	}
    }

    // callbacks may create or destroy layers, so layers are looked up again by serial
    for (const size_t serial : ended) {
	dispatch (serial);
    }

    this->removeLayers ([&ended] (const PuppetActiveAnimation& layer) {
	return layer.autoRemove && std::ranges::find (ended, layer.serial) != ended.end ();
    });
}

size_t PuppetRig::removeLayers (const std::function<bool (const PuppetActiveAnimation&)>& predicate) {
    const auto removed = std::ranges::stable_partition (this->layers, std::not_fn (predicate));
    const auto count = static_cast<size_t> (std::ranges::distance (removed));

    for (auto& layer : removed) {
	if (layer.ownedLayer != nullptr) {
	    this->removedLayers.push_back (std::move (layer));
	}
    }

    this->layers.erase (removed.begin (), removed.end ());
    return count;
}

std::vector<glm::mat4> PuppetRig::layerImageBindModel () const {
    const bool own = this->layerImageBindLocal.size () == this->bones.size ();
    std::vector<glm::mat4> result (this->bones.size (), glm::mat4 (1.0f));

    // a parent listed later is still unset, WE reads it uninitialised
    for (size_t i = 0; i < this->bones.size (); i++) {
	const glm::mat4& local = own ? this->layerImageBindLocal[i] : this->bones[i].bindLocal;
	const int parent = this->bones[i].parent;
	result[i] = parent >= 0 && static_cast<size_t> (parent) < result.size () ? result[parent] * local : local;
    }

    return result;
}

std::vector<PuppetActiveAnimation> PuppetRig::takeRemovedLayers () { return std::exchange (this->removedLayers, {}); }

void PuppetRig::updateMorphWeights (const std::vector<PuppetLayerSample>& samples) {
    // sub_14021C480: every mesh's weights start at zero each frame, then the layers apply theirs in order. A layer at
    // full weight sets them (a weight of about 0 switches the target off), additive ones add theirs kept between
    // the two values, the others blend towards theirs and clamp to 0..1. Layers at zero weight are skipped
    for (auto& mesh : this->morphWeights) {
	mesh.active = 0;
	std::ranges::fill (mesh.weights, 0.0f);
    }

    constexpr float epsilon = 1.1920929e-7f;

    for (const auto& sample : samples) {
	if (sample.weight == 0.0f) {
	    continue;
	}

	const bool direct = sample.weight == 1.0f && !sample.additive;
	const auto& meshes = sample.clip->morphTracks;

	if (this->morphWeights.size () < meshes.size ()) {
	    this->morphWeights.resize (meshes.size ());
	}

	for (size_t mesh = 0; mesh < meshes.size (); mesh++) {
	    if (!meshes[mesh].enabled) {
		continue;
	    }

	    auto& state = this->morphWeights[mesh];

	    for (const auto& track : meshes[mesh].tracks) {
		if (track.target >= 64 || track.samples.size () <= sample.frame1) {
		    continue;
		}

		if (state.weights.size () <= track.target) {
		    state.weights.resize (track.target + 1, 0.0f);
		}

		const float value = (1.0f - sample.alpha) * track.samples[sample.frame0]
		    + sample.alpha * track.samples[sample.frame1];
		const uint64_t bit = uint64_t (1) << track.target;
		float& weight = state.weights[track.target];

		if (direct) {
		    if (std::abs (value) < epsilon) {
			state.active &= ~bit;
		    } else {
			state.active |= bit;
			weight = value;
		    }
		} else if (sample.additive) {
		    if (std::abs (value) >= epsilon) {
			const float added = value * sample.weight;
			state.active |= bit;
			weight = std::clamp (weight + added, std::min (weight, added), std::max (weight, added));
		    }
		} else {
		    if (std::abs (value) >= epsilon) {
			state.active |= bit;
		    }

		    weight = (1.0f - sample.weight) * weight + sample.weight * value;

		    if (this->morphBlendClamped) {
			weight = std::clamp (weight, 0.0f, 1.0f);
		    }
		}
	    }
	}
    }
}

void PuppetRig::updatePose (const glm::mat4& objectWorld, PuppetRootMotionHost* host) {
    if (this->bones.empty ()) {
	return;
    }

    // WE's layer blend (2.8.42 sub_1401FDF90, models sub_14021C480): each bone starts from its rest pose (the MDLS
    // rest matrix as position, rotation and scale) and every visible layer in order steps its clock and blends
    // towards its clip's sample (sub_1401F9020) or, when additive, adds the sample's difference from the rest pose
    // (sub_1401F9820). Bones whose track is flagged off in a clip keep what they have.
    // every layer runs its own clock, stepped by dt * rate while the layer is visible (sub_1401FDF90)
    const float dt = this->clockTime < 0.0f ? 0.0f : std::max (g_Time - this->clockTime, 0.0f);
    this->clockTime = g_Time;

    const size_t count = this->bones.size ();
    std::vector<glm::vec3> positions (count);
    std::vector<glm::vec3> scales (count);
    std::vector<glm::quat> orientations (count);
    std::vector<glm::quat> restOrientations (count);

    for (size_t i = 0; i < count; i++) {
	const glm::mat4& rest = this->bones[i].restLocal;
	positions[i] = glm::vec3 (rest[3]);
	scales[i] = glm::vec3 (
	    glm::length (glm::vec3 (rest[0])), glm::length (glm::vec3 (rest[1])), glm::length (glm::vec3 (rest[2]))
	);
	restOrientations[i] = glm::normalize (
	    glm::quat_cast (
		glm::mat3 (
		    glm::vec3 (rest[0]) / scales[i].x, glm::vec3 (rest[1]) / scales[i].y,
		    glm::vec3 (rest[2]) / scales[i].z
		)
	    )
	);
	orientations[i] = restOrientations[i];
    }

    // extras (P+576/P+584)
    const size_t extraCount = this->extras.size ();
    std::vector<glm::vec3> extraPositions (extraCount);
    std::vector<glm::vec3> extraScales (extraCount);
    std::vector<glm::quat> extraOrientations (extraCount);
    std::vector<glm::quat> extraRestOrientations (extraCount);

    for (size_t i = 0; i < extraCount; i++) {
	const glm::mat4& rest = this->extras[i].restLocal;
	extraPositions[i] = glm::vec3 (rest[3]);
	extraScales[i] = glm::vec3 (
	    glm::length (glm::vec3 (rest[0])), glm::length (glm::vec3 (rest[1])), glm::length (glm::vec3 (rest[2]))
	);
	extraRestOrientations[i] = glm::normalize (
	    glm::quat_cast (
		glm::mat3 (
		    glm::vec3 (rest[0]) / extraScales[i].x, glm::vec3 (rest[1]) / extraScales[i].y,
		    glm::vec3 (rest[2]) / extraScales[i].z
		)
	    )
	);
	extraOrientations[i] = extraRestOrientations[i];
    }

    // q and -q are the same rotation, blends take the one on the same side
    const auto nlerp = [] (const glm::quat& a, glm::quat b, float t) {
	if (glm::dot (a, b) < 0.0f) {
	    b = -b;
	}
	return glm::normalize (a * (1.0f - t) + b * t);
    };

    const auto blend = [&] (const PuppetLayerSample& sample) {
	const auto& clip = *sample.clip;

	for (size_t i = 0; i < count; i++) {
	    if (i >= clip.boneTracks.size () || clip.boneTracks[i].size () <= sample.frame1
		|| (i < clip.boneAnimated.size () && !clip.boneAnimated[i])) {
		continue;
	    }

	    const auto& from = clip.boneTracks[i][sample.frame0];
	    const auto& to = clip.boneTracks[i][sample.frame1];
	    const glm::vec3 samplePosition = lerp (from.position, to.position, sample.alpha);
	    const glm::vec3 sampleScale = lerp (from.scale, to.scale, sample.alpha);
	    const glm::quat sampleOrientation = nlerp (from.orientation, to.orientation, sample.alpha);
	    const float weight = sample.weight;

	    if (sample.additive) {
		const glm::vec3 restPosition (this->bones[i].restLocal[3]);
		const glm::vec3 restScale (
		    glm::length (glm::vec3 (this->bones[i].restLocal[0])),
		    glm::length (glm::vec3 (this->bones[i].restLocal[1])),
		    glm::length (glm::vec3 (this->bones[i].restLocal[2]))
		);
		positions[i] += (samplePosition - restPosition) * weight;
		scales[i] += (sampleScale - restScale) * weight;
		const glm::quat delta = glm::conjugate (restOrientations[i]) * sampleOrientation;
		orientations[i] = orientations[i] * nlerp (glm::quat (1.0f, 0.0f, 0.0f, 0.0f), delta, weight);
	    } else {
		positions[i] = positions[i] * (1.0f - weight) + samplePosition * weight;
		scales[i] = scales[i] * (1.0f - weight) + sampleScale * weight;
		orientations[i] = nlerp (orientations[i], sampleOrientation, weight);
	    }
	}

	for (size_t i = 0; i < extraCount; i++) {
	    if (i >= clip.extraTracks.size () || clip.extraTracks[i].size () <= sample.frame1
		|| (i < clip.extraAnimated.size () && !clip.extraAnimated[i])) {
		continue;
	    }

	    const auto& from = clip.extraTracks[i][sample.frame0];
	    const auto& to = clip.extraTracks[i][sample.frame1];
	    const glm::vec3 samplePosition = lerp (from.position, to.position, sample.alpha);
	    const glm::vec3 sampleScale = lerp (from.scale, to.scale, sample.alpha);
	    const glm::quat sampleOrientation = nlerp (from.orientation, to.orientation, sample.alpha);
	    const float weight = sample.weight;

	    if (sample.additive) {
		const glm::mat4& rest = this->extras[i].restLocal;
		extraPositions[i] += (samplePosition - glm::vec3 (rest[3])) * weight;
		extraScales[i] += (sampleScale
				   - glm::vec3 (
				       glm::length (glm::vec3 (rest[0])), glm::length (glm::vec3 (rest[1])),
				       glm::length (glm::vec3 (rest[2]))
				   ))
		    * weight;
		const glm::quat delta = glm::conjugate (extraRestOrientations[i]) * sampleOrientation;
		extraOrientations[i] = extraOrientations[i] * nlerp (glm::quat (1.0f, 0.0f, 0.0f, 0.0f), delta, weight);
	    } else {
		extraPositions[i] = extraPositions[i] * (1.0f - weight) + samplePosition * weight;
		extraScales[i] = extraScales[i] * (1.0f - weight) + sampleScale * weight;
		extraOrientations[i] = nlerp (extraOrientations[i], sampleOrientation, weight);
	    }
	}
    };

    std::vector<PuppetLayerSample> samples;

    for (size_t index = 0; index < this->layers.size (); index++) {
	auto& candidate = this->layers[index];
	candidate.ended = false;

	if (candidate.layer == nullptr || !candidate.layer->visible->value->getBool ()) {
	    continue;
	}

	const auto& clip = candidate.clip;
	if (clip.fps <= 0.0f || clip.frameCount == 0) {
	    continue;
	}

	const float frameTime = 1.0f / clip.fps;
	const float duration = static_cast<float> (clip.frameCount) * frameTime;

	const uint32_t previousFlags = candidate.flags;
	const float previousTime = candidate.time;
	stepPuppetClock (candidate, duration, dt * candidate.layer->rate->value->getFloat (), &this->firedEvents);

	// a layer ends when a single clip stops, a mirror turns around or a loop wraps, unless it was paused, stopped
	// or moved by setFrame() since the last step
	if ((previousFlags & (PuppetClockFrameSet | PuppetClockPaused | PuppetClockStopped)) == 0) {
	    if ((candidate.flags & PuppetClockSingle) != 0) {
		candidate.ended = (candidate.flags & PuppetClockStopped) != 0;
	    } else if ((candidate.flags & PuppetClockMirror) != 0) {
		candidate.ended = (previousFlags & PuppetClockBackwards) != (candidate.flags & PuppetClockBackwards);
	    } else {
		candidate.ended = previousTime > candidate.time;
	    }
	}
	candidate.flags &= ~PuppetClockFrameSet;

	const float time = candidate.time;

	// sub_140170580
	const int lastFrame = static_cast<int> (clip.frameCount) - 1;
	const int frame0 = std::clamp (static_cast<int> (time / frameTime), 0, lastFrame);
	PuppetLayerSample sample { .clip = &clip,
				   .frame0 = static_cast<uint32_t> (frame0),
				   .frame1 = std::min (static_cast<uint32_t> (frame0 + 1), clip.frameCount),
				   .alpha = std::fmod (time, frameTime) / frameTime,
				   .weight = puppetLayerWeight (candidate, duration),
				   .additive = candidate.layer->additive };

	// models only (sub_14021C480): a clip with flags & 1 and without 0x400 plays the frames of the clip its record
	// names, from frameStart on
	if (host != nullptr && (clip.flags & 0x401) == 1 && clip.rootMotion.has_value ()
	    && clip.rootMotion->clip < this->clips.size ()) {
	    sample.clip = &this->clips[clip.rootMotion->clip];
	    sample.frame0 += clip.rootMotion->frameStart;
	    sample.frame1 += clip.rootMotion->frameStart;
	}

	samples.push_back (sample);

	if (sample.weight == 0.0f) {
	    candidate.rootMotion.started = false;
	    continue;
	}

	blend (sample);

	if (host == nullptr || !host->rootMotionEnabled () || (clip.flags & 0x1F800) == 0
	    || !clip.rootMotion.has_value ()) {
	    candidate.rootMotion.started = false;
	    continue;
	}

	// what the visible normal layers above this one leave of it
	float rest = 1.0f;

	for (size_t above = index + 1; above < this->layers.size (); above++) {
	    auto& layer = this->layers[above];

	    if (layer.layer == nullptr || !layer.layer->visible->value->getBool () || layer.layer->additive) {
		continue;
	    }

	    const float aboveDuration
		= layer.clip.fps > 0.0f ? static_cast<float> (layer.clip.frameCount) * (1.0f / layer.clip.fps) : 0.0f;
	    rest *= 1.0f - puppetLayerWeight (layer, aboveDuration);
	}

	if (rest > 0.0001f) {
	    this->applyRootMotion (candidate, sample, positions, orientations, rest, *host);
	}

	candidate.rootMotion.started = true;
	candidate.rootMotion.time = candidate.time;
    }

    this->poseAnimated
	= !samples.empty () || this->hasPhysics || this->poseScripted || this->hasRestPose || this->hasIK;

    // no scale (~0x1402012a0)
    for (size_t i = 0; i < extraCount; i++) {
	this->extraModel[i]
	    = glm::translate (glm::mat4 (1.0f), extraPositions[i]) * glm::mat4_cast (extraOrientations[i]);
    }

    std::vector<int> animatedParents (count);
    std::vector<glm::mat4> animatedLocals (count);

    for (size_t i = 0; i < count; i++) {
	animatedParents[i] = this->bones[i].parent;

	// no animation layer playing, physics runs on the rest pose like WE
	if (samples.empty ()) {
	    animatedLocals[i] = this->bones[i].restLocal;
	    continue;
	}

	animatedLocals[i] = glm::translate (glm::mat4 (1.0f), positions[i]) * glm::mat4_cast (orientations[i])
	    * glm::scale (glm::mat4 (1.0f), scales[i]);
    }

    // root motion may have moved the object, WE takes the world matrix again for the bones
    this->composePose (animatedParents, animatedLocals, host != nullptr ? host->rootMotionWorld () : objectWorld);
    this->updateMorphWeights (samples);
    this->updateDrawOrder (samples);
    this->updateBoneAlpha (samples);
    this->updateBlendMap (samples);
    this->updateConstraintWeights (samples);
}

void PuppetRig::updateConstraintWeights (const std::vector<PuppetLayerSample>& samples) {
    // puppet +592 persists across frames, zeroed only by a first layer that isn't a full weight set
    // (0x1401fee2e..0x1401fee87)
    for (size_t layer = 0; layer < samples.size (); layer++) {
	const auto& sample = samples[layer];
	const auto& tracks = sample.clip->constraintTracks;
	const bool direct = sample.weight == 1.0f && !sample.additive;

	if (layer == 0 && !direct) {
	    std::ranges::fill (this->constraintWeights, 0.0f);
	}

	for (size_t track = 0; track < tracks.size () && track < this->constraintWeights.size (); track++) {
	    if ((sample.clip->constraintFlags[track] & 1) != 0 || sample.frame1 >= tracks[track].size ()) {
		continue;
	    }

	    const float value
		= (1.0f - sample.alpha) * tracks[track][sample.frame0] + sample.alpha * tracks[track][sample.frame1];
	    float& current = this->constraintWeights[track];

	    if (direct) {
		current = value;
	    } else if (!sample.additive) {
		current = (1.0f - sample.weight) * current + sample.weight * value;
	    } else {
		current += value * sample.weight;
	    }
	}
    }
}

void PuppetRig::updateBlendMap (const std::vector<PuppetLayerSample>& samples) {
    // instance +848 persists across frames: set, lerp or add per layer (0x1401ff041..0x1401ffa18)
    for (const auto& sample : samples) {
	const auto& tracks = sample.clip->blendTracks;
	const bool direct = sample.weight == 1.0f && !sample.additive;

	for (size_t track = 0; track < tracks.size () && track < this->blendMap.size (); track++) {
	    if (sample.frame1 >= tracks[track].size ()) {
		continue;
	    }

	    const float value
		= (1.0f - sample.alpha) * tracks[track][sample.frame0] + sample.alpha * tracks[track][sample.frame1];
	    float& current = this->blendMap[track];

	    if (direct) {
		current = value;
	    } else if (!sample.additive) {
		current = (1.0f - sample.weight) * current + sample.weight * value;
	    } else {
		current += value * sample.weight;
	    }
	}
    }
}

void PuppetRig::updateBoneAlpha (const std::vector<PuppetLayerSample>& samples) {
    if (!this->boneAlphaEnabled) {
	return;
    }

    // starts at 1 every frame; additive layers move by (value - 1) * weight, bones with (flags & 3) == 1 are skipped
    this->boneAlpha.assign (this->bones.size (), 1.0f);

    for (const auto& sample : samples) {
	const auto& tracks = sample.clip->boneAlphaTracks;
	const bool direct = sample.weight == 1.0f && !sample.additive;

	for (size_t bone = 0; bone < tracks.size () && bone < this->boneAlpha.size (); bone++) {
	    if (bone < sample.clip->boneFlags.size () && (sample.clip->boneFlags[bone] & 3) == 1) {
		continue;
	    }

	    if (sample.frame1 >= tracks[bone].size ()) {
		continue;
	    }

	    const float value
		= (1.0f - sample.alpha) * tracks[bone][sample.frame0] + sample.alpha * tracks[bone][sample.frame1];
	    float& alpha = this->boneAlpha[bone];

	    if (direct) {
		alpha = value;
	    } else if (!sample.additive) {
		alpha = (1.0f - sample.weight) * alpha + sample.weight * value;
	    } else {
		alpha = std::clamp (
		    alpha + (value - 1.0f) * sample.weight, std::min (alpha, value), std::max (alpha, value)
		);
	    }
	}
    }
}

void PuppetRig::updateDrawOrder (const std::vector<PuppetLayerSample>& samples) {
    this->drawOrderTouched = false;

    if (!this->drawOrderEnabled) {
	return;
    }

    // every frame starts from the MDLS orders; a layer at full weight sets its clip's value for the frame it is on
    // (no interpolation), a blended one lerps towards it by its weight, an additive one moves by (value - MDLS
    // order) * weight without passing the value
    this->drawOrder.assign (this->bones.size (), 0.0f);

    for (size_t bone = 0; bone < this->bones.size () && bone < this->boneDrawOrder.size (); bone++) {
	this->drawOrder[bone] = static_cast<float> (this->boneDrawOrder[bone]);
    }

    for (const auto& sample : samples) {
	const auto& tracks = sample.clip->drawOrderTracks;
	const bool direct = sample.weight == 1.0f && !sample.additive;

	for (size_t bone = 0; bone < tracks.size () && bone < this->drawOrder.size (); bone++) {
	    if (bone < sample.clip->boneFlags.size () && (sample.clip->boneFlags[bone] & 3) == 1) {
		continue;
	    }

	    if (sample.frame0 >= tracks[bone].size ()) {
		continue;
	    }

	    const float value = tracks[bone][sample.frame0];
	    float& order = this->drawOrder[bone];

	    if (direct) {
		order = value;
	    } else if (!sample.additive) {
		order = (1.0f - sample.weight) * order + sample.weight * value;
	    } else {
		const float rest
		    = bone < this->boneDrawOrder.size () ? static_cast<float> (this->boneDrawOrder[bone]) : 0.0f;
		order = std::clamp (
		    order + (value - rest) * sample.weight, std::min (order, value), std::max (order, value)
		);
	    }

	    this->drawOrderTouched = true;
	}
    }
}

void PuppetRig::applyRootMotion (
    PuppetActiveAnimation& layer, const PuppetLayerSample& sample, std::vector<glm::vec3>& positions,
    std::vector<glm::quat>& orientations, const float rest, PuppetRootMotionHost& host
) {
    const auto& clip = layer.clip;
    const auto& record = *clip.rootMotion;

    if (record.bone < 0 || static_cast<size_t> (record.bone) >= this->bones.size ()) {
	return;
    }

    // the root bone from the clip that is playing, and when the layer starts what it counts from: the record's start
    // offset (frame 0 with 0x400), the first frame without one
    const auto& source = *sample.clip;
    const glm::mat4 current
	= samplePuppetBoneChain (this->bones, source, record.bone, sample.frame0, sample.frame1, sample.alpha);
    glm::mat4 start = record.frames.first;

    if (!layer.rootMotion.started && record.startOffset != 0) {
	const uint32_t frame = (clip.flags & 0x400) != 0 ? 0 : record.startOffset + record.frameStart;
	start = samplePuppetBoneChain (this->bones, source, record.bone, frame, frame, 0.0f);
    }

    const auto step = stepPuppetRootMotion (
	clip.flags, record.frames, current, start, layer.time, sample.weight, rest, glm::mat3 (host.rootMotionWorld ()),
	layer.rootMotion, positions[record.bone], orientations[record.bone]
    );

    if (step.moves) {
	host.rootMotionMove (step.offset);
    }

    if (step.turns) {
	host.rootMotionTurn (turnPuppetRootMotion (host.rootMotionAngles (), step.yaw));
    }
}

void PuppetRig::composePose (
    const std::vector<int>& parents, const std::vector<glm::mat4>& locals, const glm::mat4& objectWorld
) {
    const size_t count = parents.size ();
    const float dt = std::max (g_Time - g_TimeLast, 0.0f);

    // sub_1401FDF90 swaps the current and previous scene matrices first, so whatever a script wrote into the current
    // ones last frame is what the physics compares against
    const bool hasPrevious = this->boneScene.size () == count;
    if (hasPrevious) {
	this->physicsPreviousScene = this->boneScene;
    }

    // WE runs the physics on the bones' scene transforms (object world * bone)
    const glm::mat4& object = objectWorld;
    const float objectScale = (glm::length (glm::vec3 (object[0])) + glm::length (glm::vec3 (object[1]))
			       + glm::length (glm::vec3 (object[2])))
	/ 3.0f;

    // model matrices (P+712) persist across frames like WE, the solver starts from last frame
    std::vector<glm::mat4> model = this->boneModel.size () == count ? this->boneModel : this->bindModel;
    model.resize (count, glm::mat4 (1.0f));
    std::vector<glm::mat4> scene (count);
    std::vector<glm::mat4> local = locals;
    std::vector<uint8_t> resolved (count, 0);
    std::vector<int> ikParents (count);
    std::vector<PuppetBoneIK> ikSettings (count);

    for (size_t i = 0; i < count; i++) {
	ikParents[i] = this->bones[i].parent;
	ikSettings[i] = this->bones[i].ik;
    }

    // parents first, a simulated parent moves its children
    const auto resolve = [&] (const auto& self, size_t index) -> void {
	if (resolved[index] != 0) {
	    return;
	}

	resolved[index] = 1;
	const int parent = parents[index];
	const bool parented = parent >= 0 && static_cast<size_t> (parent) < count;

	if (parented && resolved[parent] != 1) {
	    self (self, static_cast<size_t> (parent));
	}

	// posed by the solver (~0x140201300)
	if ((this->bones[index].type & 2) && this->bones[index].chain < 0) {
	    scene[index] = object * model[index];
	    local[index] = parented ? glm::inverse (model[parent]) * model[index] : model[index];
	    resolved[index] = 2;
	    return;
	}

	model[index] = parented ? model[parent] * locals[index] : locals[index];

	// the bone's blend rules, before physics
	for (const auto& rule : this->bones[index].rules) {
	    if (rule.target < count) {
		const float weight
		    = rule.weight < this->constraintWeights.size () ? this->constraintWeights[rule.weight] : 0.0f;
		model[index] = blendPuppetTransform (model[index], model[rule.target], weight);
	    }
	}

	scene[index] = object * model[index];
	const auto& physics = this->bones[index].physics;

	// the first frame has nothing to compare against and only records where the bones are
	if (physics.simulated () && hasPrevious) {
	    model[index] = model[index]
		* stepPuppetBonePhysics (
			       physics, this->physicsState[index], scene[index], this->physicsPreviousScene[index], dt,
			       objectScale
		);
	    scene[index] = object * model[index];
	}

	// solve the chain this bone starts, its scene matrix keeps the unsolved pose
	if (const int chain = this->bones[index].chain;
	    chain >= 0 && static_cast<size_t> (chain) < this->ik.chains.size ()) {
	    const auto& solved = this->ik.chains[chain];
	    const bool rope = std::ranges::any_of (solved.links, [] (const PuppetIKLink& link) {
		return std::ranges::any_of (link.entries, [] (const PuppetIKEntry& entry) {
		    return (entry.flags & 4) != 0;
		});
	    });

	    // sub_14026F4C0
	    applyPuppetChainConstraints (this->ik, solved, this->constraintWeights, model, this->extraModel);

	    if (rope) {
		stepPuppetRopeChain (
		    this->ik, solved, ikSettings, model, this->extraModel, object, this->ropeJoints,
		    this->ropeEnvironment, dt
		);
	    }

	    solvePuppetIKChain (
		this->ik, solved, ikParents, ikSettings, model, this->extraModel, this->bindModel, object,
		this->ropeJoints
	    );
	}

	resolved[index] = 2;
    };

    for (size_t i = 0; i < count; i++) {
	resolve (resolve, i);
    }

    this->boneLocal = std::move (local);
    this->boneModel = std::move (model);
    this->boneScene = std::move (scene);
}

bool PuppetRig::hasPose () const { return !this->bones.empty () && this->boneScene.size () == this->bones.size (); }

int PuppetRig::findBone (const std::string& name) const {
    for (size_t i = 0; i < this->bones.size (); i++) {
	if (this->bones[i].name == name) {
	    return static_cast<int> (i);
	}
    }

    return -1;
}

int PuppetRig::findAttachment (const std::string& name) const {
    const auto point = std::ranges::find (this->attachmentPoints, name, &PuppetAttachmentPoint::name);

    return point == this->attachmentPoints.end () ? -1 : static_cast<int> (point - this->attachmentPoints.begin ());
}

std::optional<glm::mat4> PuppetRig::attachmentMatrix (int index) const {
    if (index < 0 || static_cast<size_t> (index) >= this->attachmentPoints.size ()) {
	return std::nullopt;
    }

    const auto& point = this->attachmentPoints[index];

    if (point.boneIndex < 0 || static_cast<size_t> (point.boneIndex) >= this->boneModel.size ()) {
	return std::nullopt;
    }

    return this->boneModel[point.boneIndex] * point.localTransform;
}

const glm::mat4& PuppetRig::getBoneTransform (int bone) const { return this->boneScene[bone]; }

namespace {
// sub_1401853C0, no t >= 0 check
bool lineHitsBox (const glm::vec3& origin, const glm::vec3& direction, const glm::vec3& extents, float& enter) {
    float leave = std::numeric_limits<float>::max ();
    enter = std::numeric_limits<float>::lowest ();

    for (int axis = 0; axis < 3; axis++) {
	const float a = (extents[axis] - origin[axis]) / direction[axis];
	const float b = (-extents[axis] - origin[axis]) / direction[axis];

	enter = std::max (enter, std::min (a, b));
	leave = std::min (leave, std::max (a, b));
    }

    return leave >= enter;
}
} // namespace

std::string PuppetRig::hitBoxName (int bone) const {
    return this->bones[bone].name.empty () ? std::to_string (bone) : this->bones[bone].name;
}

std::optional<std::string> PuppetRig::imageHitBox (const glm::vec3& origin, const glm::vec3& direction) const {
    if (!this->hasPose () || this->boneHitOrder.empty () || !this->bones.front ().hasCapsule) {
	return std::nullopt;
    }

    for (auto it = this->boneHitOrder.rbegin (); it != this->boneHitOrder.rend (); ++it) {
	const int bone = *it;
	const glm::mat4 toBox = glm::inverse (this->boneScene[bone] * this->bones[bone].capsule);
	const glm::vec3 localOrigin = toBox * glm::vec4 (origin, 1.0f);
	const glm::vec3 localDirection = glm::normalize (glm::vec3 (toBox * glm::vec4 (direction, 0.0f)));
	float enter;

	if (lineHitsBox (localOrigin, localDirection, this->bones[bone].capsuleExtents, enter)) {
	    return this->hitBoxName (bone);
	}
    }

    return std::nullopt;
}

std::optional<std::string> PuppetRig::modelHitBox (
    const glm::vec3& origin, const glm::vec3& direction, const glm::mat4& objectWorld, glm::vec3& local
) const {
    if (this->bones.empty () || !this->bones.front ().hasCapsule || this->boneModel.size () != this->bones.size ()) {
	return std::nullopt;
    }

    std::optional<std::string> name;
    float nearest = std::numeric_limits<float>::max ();

    for (size_t bone = 0; bone < this->bones.size (); bone++) {
	const glm::mat4 toBox = glm::inverse (objectWorld * this->boneModel[bone] * this->bones[bone].capsule);
	const glm::vec3 localOrigin = toBox * glm::vec4 (origin, 1.0f);
	const glm::vec3 localDirection = glm::normalize (glm::vec3 (toBox * glm::vec4 (direction, 0.0f)));
	float enter;

	if (lineHitsBox (localOrigin, localDirection, this->bones[bone].capsuleExtents, enter) && enter < nearest) {
	    nearest = enter;
	    local = localOrigin + localDirection * enter;
	    name = this->hitBoxName (static_cast<int> (bone));
	}
    }

    return name;
}

void PuppetRig::setBoneTransform (int bone, const glm::mat4& transform, const glm::mat4& objectWorld) {
    // sub_14020F350: only this bone, its children keep their matrices until the next update
    this->boneScene[bone] = transform;
    this->boneModel[bone] = glm::inverse (objectWorld) * transform;
    this->poseScripted = true;
    this->poseAnimated = true;
}

const glm::mat4& PuppetRig::getLocalBoneTransform (int bone) const { return this->boneLocal[bone]; }

void PuppetRig::setLocalBoneTransform (int bone, const glm::mat4& transform, const glm::mat4& objectWorld) {
    this->boneLocal[bone] = transform;

    // sub_14020DB40: the bone and every later bone whose parent was touched, in index order
    const glm::mat4& object = objectWorld;
    std::set<int> touched;

    const int count = static_cast<int> (this->bones.size ());

    for (int index = bone; index < count; index++) {
	const int parent = this->bones[index].parent;

	if (index != bone && !touched.contains (parent)) {
	    continue;
	}

	touched.insert (index);
	this->boneModel[index]
	    = parent < 0 || parent >= count ? this->boneLocal[index] : this->boneModel[parent] * this->boneLocal[index];
	this->boneScene[index] = object * this->boneModel[index];
    }

    this->poseScripted = true;
    this->poseAnimated = true;
}

void PuppetRig::applyBonePhysicsImpulse (int bone, const glm::vec3& directional, const glm::vec3& angularDegrees) {
    if (bone >= 0 && static_cast<size_t> (bone) < this->physicsState.size ()) {
	applyPuppetBoneImpulse (this->physicsState[bone], directional, angularDegrees);
    }
}

void PuppetRig::resetBonePhysics (int bone) {
    // sub_140210E10, rope joints included
    if (bone >= 0 && static_cast<size_t> (bone) < this->physicsState.size ()) {
	this->physicsState[bone] = {};
    }

    if (bone >= 0 && static_cast<size_t> (bone) < this->ropeJoints.size ()) {
	this->ropeJoints[bone] = {};
    }
}
