#include "PuppetRig.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <strings.h>

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
void stepPuppetClock (PuppetActiveAnimation& clock, float duration, float delta) {
    if ((clock.flags & (PuppetClockPaused | PuppetClockStopped)) != 0
	|| ((clock.flags & PuppetClockSingle) != 0 && clock.time >= duration) || duration <= 0.0f) {
	return;
    }

    if ((clock.flags & PuppetClockBackwards) != 0) {
	delta = -delta;
    }

    const float time = clock.time + delta;
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
	}
	if (clock.time >= duration) {
	    clock.time = std::fmod (clock.time, duration);
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
};

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
	(void)reader.nextUInt32 (); // type, unused
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
			 .physics = PuppetBonePhysics::parse (physics) }
	);
    }

    // the rest of MDLS as 2.8.42 reads it (sub_140261880), only the two counts matter here
    const int version = std::atoi (header + 4);

    if (version < 2 || result.bones.size () != boneCount) {
	return result;
    }

    uint16_t extraCount = 0;
    reader.next (reinterpret_cast<char*> (&extraCount), sizeof (extraCount));

    for (uint16_t i = 0; i < extraCount; i++) {
	(void)reader.nextNullTerminatedString ();
	reader.base ().seekg (sizeof (uint32_t) * 2 + sizeof (float) * 16, std::ios::cur);
    }

    // a flag byte and then a matrix per bone and per extra record. Not the inverse bind matrices: bone 0 holds its bind
    // world and the others their bind locals (makima.body_puppet.mdl), what WE does with them isn't traced
    if (reader.next () != 0) {
	reader.base ().seekg (
	    static_cast<std::streamoff> ((boneCount + extraCount) * sizeof (float) * 16), std::ios::cur
	);
    }

    const uint32_t constraintCount = reader.nextUInt32 ();

    for (uint32_t i = 0; i < constraintCount && reader.base ().good (); i++) {
	reader.base ().seekg (sizeof (uint32_t) * 3, std::ios::cur);
	const uint32_t flags = version >= 4 ? reader.nextUInt32 () : 0;

	if (flags & 2) {
	    reader.base ().seekg (sizeof (uint32_t) + sizeof (float), std::ios::cur);
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
    const auto skip = [&reader] (std::streamoff bytes) { reader.base ().seekg (bytes, std::ios::cur); };
    const auto nextUInt16 = [&reader] () {
	uint16_t value = 0;
	reader.next (reinterpret_cast<char*> (&value), sizeof (value));
	return value;
    };

    const uint16_t groups = nextUInt16 ();
    skip (sizeof (uint32_t) * groups);
    for (uint16_t i = 0; i < groups && reader.base ().good (); i++) {
	skip ((sizeof (uint32_t) + sizeof (float) * 3) * nextUInt16 ());
    }

    const uint16_t chains = nextUInt16 ();
    for (uint16_t i = 0; i < chains && reader.base ().good (); i++) {
	(void)reader.nextUInt32 ();
	skip (sizeof (uint32_t) * reader.nextUInt32 ());
	const uint16_t links = nextUInt16 ();
	for (uint16_t j = 0; j < links && reader.base ().good (); j++) {
	    (void)reader.nextUInt32 ();
	    const uint16_t entries = nextUInt16 ();
	    for (uint16_t k = 0; k < entries && reader.base ().good (); k++) {
		skip (sizeof (uint32_t) * 4);
		skip (sizeof (uint32_t) * nextUInt16 ());
	    }
	}
    }

    if (!reader.base ().good () || static_cast<size_t> (reader.base ().tellg ()) >= nextSectionOffset
	|| reader.next () == 0) {
	reader.base ().clear ();
	return result;
    }

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

    return result;
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

// Parses the optional MDAT section (named attachment points other objects can follow, e.g.
// scene.json's "attachment": "orb" - see docs/rendering/MDL_FILES.md). Keeps the points parsed so far
// and stops at the first implausible entry, the tail of this section isn't fully understood
PuppetAttachmentPointSet
parsePuppetAttachmentPoints (const BinaryReader& reader, size_t mdatOffset, uint32_t boneCount) {
    reader.base ().seekg (static_cast<std::streamoff> (mdatOffset), std::ios::beg);

    char header[9];
    reader.next (header, sizeof (header));

    PuppetAttachmentPointSet result;
    result.mdlaOffset = reader.nextUInt32 ();

    uint16_t pointCount = 0;
    reader.next (reinterpret_cast<char*> (&pointCount), sizeof (pointCount));

    // The WORD trailing every point's matrix is the NEXT point's bone index: point 0's comes right
    // after pointCount and the last point has no trailing WORD, which consumes exactly the section's
    // declared length
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

	// every track is a length-prefixed block of one value (or one 9-float transform) per sample
	const auto skipTrack = [&] (uint32_t sampleBytes) {
	    const uint32_t trackBytes = reader.nextUInt32 ();
	    if (trackBytes != sampleCount * sampleBytes) {
		valid = false;
		return;
	    }
	    reader.base ().seekg (static_cast<std::streamoff> (trackBytes), std::ios::cur);
	};
	const auto skipFlaggedTracks = [&] (uint32_t count, uint32_t sampleBytes) {
	    for (uint32_t i = 0; i < count && valid; i++) {
		(void)reader.nextUInt32 ();
		skipTrack (sampleBytes);
	    }
	};

	clip.boneTracks.resize (boneCount);
	clip.boneAnimated.assign (boneCount, true);

	for (uint32_t boneIndex = 0; boneIndex < boneCount && valid; boneIndex++) {
	    // bit 0 keeps the bone out of this clip, the blend masks it (2.8.42 sub_140261880)
	    clip.boneAnimated[boneIndex] = (reader.nextUInt32 () & 1) == 0;
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
	    skipFlaggedTracks (rig.extraCount, 9 * sizeof (float));
	    skipFlaggedTracks (rig.constraintCount, sizeof (float));
	}

	if (version >= 3 && valid) {
	    skipFlaggedTracks (reader.nextUInt32 (), sizeof (float));

	    if (valid && reader.next () != 0) {
		skipFlaggedTracks (boneCount, sizeof (float));
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
	    reader.base ().seekg (sizeof (uint32_t) * 6, std::ios::cur);
	}

	if (version >= 6 && valid && reader.next () != 0) {
	    skipFlaggedTracks (boneCount, sizeof (float));
	}

	if ((flags & 1) && valid) {
	    reader.base ().seekg (sizeof (uint16_t) + sizeof (uint32_t) * 4, std::ios::cur);
	}

	if (valid) {
	    const uint32_t eventCount = reader.nextUInt32 ();

	    for (uint32_t i = 0; i < eventCount && reader.base ().good (); i++) {
		(void)reader.nextUInt32 (); // frame
		(void)reader.nextNullTerminatedString ();
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
} // namespace

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
    this->boneModel = worldBind;
    this->physicsState.assign (this->bones.size (), {});
    this->hasPhysics
	= std::ranges::any_of (this->bones, [] (const PuppetBone& bone) { return bone.physics.simulated (); });

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

    return std::erase_if (
	       this->layers, [&name] (const PuppetActiveAnimation& layer) { return layer.layer->name == name; }
	   )
	> 0;
}

bool PuppetRig::destroyLayer (size_t serial) {
    return std::erase_if (
	       this->layers, [serial] (const PuppetActiveAnimation& layer) { return layer.serial == serial; }
	   )
	> 0;
}

// sub_1401FDF90, after the pose: a layer that ended runs its ended callbacks, a playSingleAnimation() one is removed
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

    std::erase_if (this->layers, [&ended] (const PuppetActiveAnimation& layer) {
	return layer.autoRemove && std::ranges::find (ended, layer.serial) != ended.end ();
    });
}

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

		    weight = std::clamp ((1.0f - sample.weight) * weight + sample.weight * value, 0.0f, 1.0f);
		}
	    }
	}
    }
}

void PuppetRig::updatePose (const glm::mat4& objectWorld) {
    if (this->bones.empty ()) {
	return;
    }

    // WE's layer blend (2.8.42 sub_1401FDF90): each bone starts from its rest pose (the MDLS bind matrix as
    // position, rotation and scale) and every visible layer in order blends towards its clip's sample
    // (sub_1401F9020) or, when additive, adds the sample's difference from the rest pose (sub_1401F9820).
    // Bones whose track is flagged off in a clip keep what they have.
    // every layer runs its own clock, stepped by dt * rate while the layer is visible (sub_1401FDF90)
    const float dt = this->clockTime < 0.0f ? 0.0f : std::max (g_Time - this->clockTime, 0.0f);
    this->clockTime = g_Time;

    std::vector<PuppetLayerSample> samples;
    for (auto& candidate : this->layers) {
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
	stepPuppetClock (candidate, duration, dt * candidate.layer->rate->value->getFloat ());

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
	samples.push_back (
	    PuppetLayerSample { .clip = &clip,
				.frame0 = static_cast<uint32_t> (frame0),
				.frame1 = std::min (static_cast<uint32_t> (frame0 + 1), clip.frameCount),
				.alpha = std::fmod (time, frameTime) / frameTime,
				.weight = puppetLayerWeight (candidate, duration),
				.additive = candidate.layer->additive }
	);
    }

    this->poseAnimated = !samples.empty () || this->hasPhysics || this->poseScripted;

    // q and -q are the same rotation, blends take the one on the same side
    const auto nlerp = [] (const glm::quat& a, glm::quat b, float t) {
	if (glm::dot (a, b) < 0.0f) {
	    b = -b;
	}
	return glm::normalize (a * (1.0f - t) + b * t);
    };

    std::vector<int> animatedParents (this->bones.size ());
    std::vector<glm::mat4> animatedLocals (this->bones.size ());

    for (size_t i = 0; i < this->bones.size (); i++) {
	const auto& bone = this->bones[i];
	animatedParents[i] = bone.parent;

	// no animation layer playing, physics runs on the bind pose like WE
	if (samples.empty ()) {
	    animatedLocals[i] = bone.bindLocal;
	    continue;
	}

	const glm::vec3 restPosition (bone.bindLocal[3]);
	const glm::vec3 restScale (
	    glm::length (glm::vec3 (bone.bindLocal[0])), glm::length (glm::vec3 (bone.bindLocal[1])),
	    glm::length (glm::vec3 (bone.bindLocal[2]))
	);
	const glm::quat restOrientation = glm::normalize (
	    glm::quat_cast (
		glm::mat3 (
		    glm::vec3 (bone.bindLocal[0]) / restScale.x, glm::vec3 (bone.bindLocal[1]) / restScale.y,
		    glm::vec3 (bone.bindLocal[2]) / restScale.z
		)
	    )
	);

	glm::vec3 position = restPosition;
	glm::vec3 scale = restScale;
	glm::quat orientation = restOrientation;

	for (const auto& sample : samples) {
	    const auto& clip = *sample.clip;
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
		position += (samplePosition - restPosition) * weight;
		scale += (sampleScale - restScale) * weight;
		const glm::quat delta = glm::conjugate (restOrientation) * sampleOrientation;
		orientation = orientation * nlerp (glm::quat (1.0f, 0.0f, 0.0f, 0.0f), delta, weight);
	    } else {
		position = position * (1.0f - weight) + samplePosition * weight;
		scale = scale * (1.0f - weight) + sampleScale * weight;
		orientation = nlerp (orientation, sampleOrientation, weight);
	    }
	}

	animatedLocals[i] = glm::translate (glm::mat4 (1.0f), position) * glm::mat4_cast (orientation)
	    * glm::scale (glm::mat4 (1.0f), scale);
    }

    this->composePose (animatedParents, animatedLocals, objectWorld);
    this->updateMorphWeights (samples);
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

    std::vector<glm::mat4> model (count);
    std::vector<glm::mat4> scene (count);
    std::vector<uint8_t> resolved (count, 0);

    // parents first, a simulated parent moves its children
    const auto resolve = [&] (const auto& self, size_t index) -> void {
	if (resolved[index] != 0) {
	    return;
	}

	resolved[index] = 1;
	const int parent = parents[index];

	if (parent >= 0 && static_cast<size_t> (parent) < count && resolved[parent] != 1) {
	    self (self, static_cast<size_t> (parent));
	    model[index] = model[parent] * locals[index];
	} else {
	    model[index] = locals[index];
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

	resolved[index] = 2;
    };

    for (size_t i = 0; i < count; i++) {
	resolve (resolve, i);
    }

    this->boneLocal = locals;
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

const glm::mat4& PuppetRig::getBoneTransform (int bone) const { return this->boneScene[bone]; }

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
    // sub_140210E10
    if (bone >= 0 && static_cast<size_t> (bone) < this->physicsState.size ()) {
	this->physicsState[bone] = {};
    }
}
