#include "CImage.h"

#include "CRenderable.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/rotate_vector.hpp>
#undef GLM_ENABLE_EXPERIMENTAL

#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Data/Model/Material.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Data/Model/UserSetting.h"
#include "WallpaperEngine/Data/Parsers/MaterialParser.h"
#include "WallpaperEngine/Data/Utils/BinaryReader.h"
#include "WallpaperEngine/Data/Utils/MemoryStream.h"
#include "WallpaperEngine/Logging/Log.h"

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render::Objects;
using namespace WallpaperEngine::Render::Objects::Effects;
using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Data::Builders;
using namespace WallpaperEngine::Data::Utils;

extern float g_Time;

namespace {
glm::vec2 rotateVec2 (const glm::vec2& value, float angle) {
    const float cosAngle = std::cos (angle);
    const float sinAngle = std::sin (angle);
    return { value.x * cosAngle - value.y * sinAngle, value.x * sinAngle + value.y * cosAngle };
}

bool isMagentaNeonTint (const glm::vec3& color) { return color.r > 0.55f && color.g < 0.25f && color.b > 0.45f; }

std::optional<glm::vec3> findMagentaCompositeTint (const Image& image, const std::vector<int>& skippedEffectIds) {
    for (const auto& effect : image.effects) {
	if (std::find (skippedEffectIds.begin (), skippedEffectIds.end (), static_cast<int> (effect->id))
	    != skippedEffectIds.end ()) {
	    continue;
	}
	if (!effect->visible->value->getBool ()) {
	    continue;
	}

	for (const auto& passOverride : effect->passOverrides) {
	    const auto compositeCombo = passOverride->combos.find ("COMPOSITE");
	    if (compositeCombo == passOverride->combos.end () || compositeCombo->second != 2) {
		continue;
	    }

	    const auto compositeColor = passOverride->constants.find ("compositecolor");
	    if (compositeColor == passOverride->constants.end () || compositeColor->second == nullptr
		|| compositeColor->second->value == nullptr) {
		continue;
	    }

	    const auto tint = compositeColor->second->value->getVec3 ();
	    if (isMagentaNeonTint (tint)) {
		return tint;
	    }
	}
    }

    return std::nullopt;
}

struct PuppetMeshBlock {
    size_t headerOffset = 0;
    uint32_t vertexBytes = 0;
    uint32_t indexBytes = 0;
};

// Finds every byte offset that could plausibly be a MDLV mesh header (DWORD vertexByteLength
// immediately followed by that many bytes of vertex data, then a DWORD indexByteLength followed by
// that many bytes of indices, all landing before the MDLS block). This intentionally doesn't know or
// care about the per-vertex stride - that's resolved afterwards against whatever candidates come back,
// since the stride isn't reliably predictable from the MDLV header version alone (see
// resolvePuppetVertexLayout).
std::vector<PuppetMeshBlock>
findPuppetMeshBlockCandidates (const BinaryReader& reader, size_t markerSize, size_t mdlsOffset, size_t meshHeaderSize) {
    std::vector<PuppetMeshBlock> candidates;

    for (size_t offset = markerSize; offset + meshHeaderSize + sizeof (uint32_t) < mdlsOffset; offset++) {
	reader.base ().seekg (static_cast<std::streamoff> (offset + sizeof (uint32_t)), std::ios::beg);
	const uint32_t candidateVertexBytes = reader.nextUInt32 ();
	const size_t verticesOffset = offset + meshHeaderSize;
	const size_t indexLengthOffset = verticesOffset + candidateVertexBytes;

	if (candidateVertexBytes == 0 || indexLengthOffset + sizeof (uint32_t) > mdlsOffset) {
	    continue;
	}

	reader.base ().seekg (static_cast<std::streamoff> (indexLengthOffset), std::ios::beg);
	const uint32_t candidateIndexBytes = reader.nextUInt32 ();
	const size_t indicesOffset = indexLengthOffset + sizeof (uint32_t);
	if (candidateIndexBytes == 0 || candidateIndexBytes % (sizeof (uint16_t) * 3) != 0
	    || indicesOffset + candidateIndexBytes > mdlsOffset) {
	    continue;
	}

	candidates.push_back (
	    PuppetMeshBlock { .headerOffset = offset, .vertexBytes = candidateVertexBytes, .indexBytes = candidateIndexBytes }
	);
    }

    return candidates;
}

struct PuppetVertexLayout {
    PuppetMeshBlock block;
    size_t vertexStride = 0;
    size_t uvOffset = 0;
};

// Reads raw positions/UVs/indices for a candidate (block, stride) pair. The UV pair has only ever
// been observed as the trailing 8 bytes of the vertex record, whatever bone/weight data precedes it
// (position(12) + ... + uv(8)), so uvOffset = stride - 8 throughout.
struct PuppetMeshData {
    std::vector<GLfloat> positions;
    std::vector<GLfloat> texcoords;
    std::vector<GLushort> indices;
};

std::optional<PuppetMeshData>
readPuppetMeshData (const BinaryReader& reader, const PuppetMeshBlock& block, size_t meshHeaderSize, size_t vertexStride) {
    if (block.vertexBytes % vertexStride != 0) {
	return std::nullopt;
    }

    const size_t uvOffset = vertexStride - sizeof (GLfloat) * 2;
    const size_t vertexCount = block.vertexBytes / vertexStride;
    const size_t verticesOffset = block.headerOffset + meshHeaderSize;
    const size_t indicesOffset = verticesOffset + block.vertexBytes + sizeof (uint32_t);
    const size_t indexCount = block.indexBytes / sizeof (uint16_t);

    PuppetMeshData data;
    data.positions.reserve (vertexCount * 3);
    data.texcoords.reserve (vertexCount * 2);
    data.indices.reserve (indexCount);

    for (size_t index = 0; index < vertexCount; index++) {
	const size_t vertexOffset = verticesOffset + index * vertexStride;
	reader.base ().seekg (static_cast<std::streamoff> (vertexOffset), std::ios::beg);
	const float x = reader.nextFloat ();
	const float y = reader.nextFloat ();
	const float z = reader.nextFloat ();
	reader.base ().seekg (static_cast<std::streamoff> (vertexOffset + uvOffset), std::ios::beg);
	const float u = reader.nextFloat ();
	const float v = reader.nextFloat ();

	data.positions.push_back (x);
	data.positions.push_back (y);
	data.positions.push_back (z);
	data.texcoords.push_back (u);
	data.texcoords.push_back (v);
    }

    reader.base ().seekg (static_cast<std::streamoff> (indicesOffset), std::ios::beg);
    for (size_t index = 0; index < indexCount; index++) {
	uint16_t value = 0;
	reader.next (reinterpret_cast<char*> (&value), sizeof (value));
	if (value >= vertexCount) {
	    return std::nullopt;
	}
	data.indices.push_back (value);
    }

    return data;
}

// Blend indices/weights are always the 32 bytes immediately before the UV pair, regardless of stride
// (see docs/rendering/MDL_FILES.md) - position(12) + [normal(12) + tangent4(16), wide format only] +
// blendindices(16) + blendweight(16) + uv(8).
struct PuppetBlendData {
    std::vector<glm::uvec4> indices;
    std::vector<glm::vec4> weights;
};

std::optional<PuppetBlendData> readPuppetBlendData (
    const BinaryReader& reader, const PuppetMeshBlock& block, size_t meshHeaderSize, size_t vertexStride
) {
    if (vertexStride < 40 || block.vertexBytes % vertexStride != 0) {
	return std::nullopt;
    }

    const size_t blendIndicesOffset = vertexStride - 40;
    const size_t blendWeightsOffset = vertexStride - 24;
    const size_t vertexCount = block.vertexBytes / vertexStride;
    const size_t verticesOffset = block.headerOffset + meshHeaderSize;

    PuppetBlendData data;
    data.indices.reserve (vertexCount);
    data.weights.reserve (vertexCount);

    for (size_t index = 0; index < vertexCount; index++) {
	const size_t vertexOffset = verticesOffset + index * vertexStride;

	reader.base ().seekg (static_cast<std::streamoff> (vertexOffset + blendIndicesOffset), std::ios::beg);
	glm::uvec4 boneIndices;
	boneIndices.x = reader.nextUInt32 ();
	boneIndices.y = reader.nextUInt32 ();
	boneIndices.z = reader.nextUInt32 ();
	boneIndices.w = reader.nextUInt32 ();

	reader.base ().seekg (static_cast<std::streamoff> (vertexOffset + blendWeightsOffset), std::ios::beg);
	glm::vec4 boneWeights;
	boneWeights.x = reader.nextFloat ();
	boneWeights.y = reader.nextFloat ();
	boneWeights.z = reader.nextFloat ();
	boneWeights.w = reader.nextFloat ();

	data.indices.push_back (boneIndices);
	data.weights.push_back (boneWeights);
    }

    return data;
}

// Scores how plausible a candidate vertex layout is: real puppet meshes are triangulated warp grids,
// so triangles formed by adjacent indices should be small relative to the mesh's overall size. A wrong
// stride reinterprets bone/weight bytes as positions, which decorrelates neighbouring vertices and
// produces comparatively huge, inconsistent triangles. Lower is better; nullopt if unscorable (e.g. a
// degenerate single-point mesh).
std::optional<double> scorePuppetMeshCoherence (const PuppetMeshData& data) {
    if (data.indices.size () < 3) {
	return std::nullopt;
    }

    glm::vec3 min (std::numeric_limits<float>::max ());
    glm::vec3 max (std::numeric_limits<float>::lowest ());
    const size_t vertexCount = data.positions.size () / 3;

    for (size_t i = 0; i < vertexCount; i++) {
	const glm::vec3 p (data.positions[i * 3], data.positions[i * 3 + 1], data.positions[i * 3 + 2]);
	min = glm::min (min, p);
	max = glm::max (max, p);
    }

    const double diagonal = glm::length (max - min);
    if (diagonal <= 0.0) {
	return std::nullopt;
    }

    double totalEdgeLength = 0.0;
    size_t edgeCount = 0;

    for (size_t triangle = 0; triangle + 2 < data.indices.size (); triangle += 3) {
	const auto vertexPosition = [&data] (size_t index) {
	    return glm::vec3 (data.positions[index * 3], data.positions[index * 3 + 1], data.positions[index * 3 + 2]);
	};

	const glm::vec3 a = vertexPosition (data.indices[triangle]);
	const glm::vec3 b = vertexPosition (data.indices[triangle + 1]);
	const glm::vec3 c = vertexPosition (data.indices[triangle + 2]);

	totalEdgeLength += glm::length (a - b) + glm::length (b - c) + glm::length (c - a);
	edgeCount += 3;
    }

    if (edgeCount == 0) {
	return std::nullopt;
    }

    return (totalEdgeLength / static_cast<double> (edgeCount)) / diagonal;
}

// The MDLV vertex layout isn't reliably predictable from the header version number alone - the same
// version (e.g. MDLV0023) has been observed with different per-vertex strides depending on how many
// bone influences a given puppet part carries. So instead of a fixed version->stride table, every
// plausible stride is tried against every candidate mesh header found in the file, and whichever
// combination produces the most coherent triangulated mesh wins.
std::optional<PuppetVertexLayout> resolvePuppetVertexLayout (
    const BinaryReader& reader, size_t markerSize, size_t mdlsOffset, size_t meshHeaderSize
) {
    constexpr size_t minVertexStride = 20; // position (12 bytes) + uv (8 bytes), no bone data at all
    constexpr size_t maxVertexStride = 256; // generous upper bound, comfortably covers multi-bone rigs
    constexpr size_t strideStep = 4; // every field observed so far is a 4-byte float/uint

    const auto candidates = findPuppetMeshBlockCandidates (reader, markerSize, mdlsOffset, meshHeaderSize);

    std::optional<PuppetVertexLayout> best;
    double bestScore = std::numeric_limits<double>::max ();

    for (const auto& block : candidates) {
	for (size_t stride = minVertexStride; stride <= maxVertexStride; stride += strideStep) {
	    const auto data = readPuppetMeshData (reader, block, meshHeaderSize, stride);
	    if (!data.has_value ()) {
		continue;
	    }

	    const auto score = scorePuppetMeshCoherence (*data);
	    if (!score.has_value ()) {
		continue;
	    }

	    if (*score >= bestScore) {
		continue;
	    }

	    bestScore = *score;
	    best = PuppetVertexLayout { .block = block, .vertexStride = stride, .uvOffset = stride - sizeof (GLfloat) * 2 };
	}
    }

    return best;
}

struct PuppetBoneSet {
    std::vector<PuppetBone> bones;
    // Points at whatever section comes right after MDLS's second bone array: MDLA directly for
    // puppets with no attachment points, or MDAT (attachment points) otherwise - the caller has to
    // check which one it actually is.
    size_t nextSectionOffset = 0;
};

// Parses the MDLS section's first bone array (local bind-pose transforms + parent hierarchy). The
// second bone array isn't decoded: its per-bone "name" slot turns out to hold physics/jiggle constraint
// parameters (angle limits, stiffness, a target position) rather than anything about mesh skinning, and
// inverse-bind matrices can be derived from the first array alone by walking the parent chain anyway.
PuppetBoneSet parsePuppetBones (const BinaryReader& reader, size_t mdlsOffset) {
    reader.base ().seekg (static_cast<std::streamoff> (mdlsOffset), std::ios::beg);

    char header[9];
    reader.next (header, sizeof (header));

    const uint32_t nextSectionOffset = reader.nextUInt32 ();
    const uint32_t boneCount = reader.nextUInt32 ();

    // A bone count this large can only be a garbage read (wrong mdlsOffset or an unrecognized MDLS
    // layout), not a real rig. Same reasoning as the clip/point-count guards below.
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
	(void) reader.nextNullTerminatedString ();
	(void) reader.nextUInt32 (); // type, unused
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
		    "Puppet bone ", i, " has an implausible matrix byte count (", matrixBytes,
		    "), stopping here (", result.bones.size (), " bone(s) kept)"
		);
		break;
	    }
	    reader.base ().seekg (static_cast<std::streamoff> (matrixBytes), std::ios::cur);
	}

	// trailing per-bone string, jiggle/physics JSON for some rigs
	(void) reader.nextNullTerminatedString ();

	result.bones.push_back (PuppetBone { .parent = parent, .bindLocal = bindLocal });
    }

    return result;
}

// Resolves each bone's world transform from its parent-relative local transform, by walking up
// the parent chain rather than assuming the array is stored parent-before-child. Nothing in the MDL
// format guarantees that ordering, and it does not hold for every rig seen in practice (small
// sub-meshes like a puppet's eyes/eyebrows in particular) - treating an out-of-order parent as "not
// yet resolved" instead of silently falling back to "no parent" is what makes a bone whose parent
// happens to sit later in the array compose correctly instead of coming out at raw bone-local
// coordinates, detached from the rest of the rig it's supposed to be attached to.
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

std::vector<glm::mat4> composeBoneWorldTransforms (const std::vector<int>& parents, const std::vector<glm::mat4>& locals) {
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
// scene.json's "attachment": "orb" - see docs/rendering/MDL_FILES.md). Stops - keeping whatever
// points parsed cleanly so far - the moment an entry looks implausible, since only two real point
// names have been confirmed against real data and the tail of this section isn't fully understood.
PuppetAttachmentPointSet parsePuppetAttachmentPoints (const BinaryReader& reader, size_t mdatOffset, uint32_t boneCount) {
    reader.base ().seekg (static_cast<std::streamoff> (mdatOffset), std::ios::beg);

    char header[9];
    reader.next (header, sizeof (header));

    PuppetAttachmentPointSet result;
    result.mdlaOffset = reader.nextUInt32 ();

    uint16_t pointCount = 0;
    reader.next (reinterpret_cast<char*> (&pointCount), sizeof (pointCount));

    // What looks like a fixed WORD trailing every point's matrix is actually the NEXT point's bone
    // index, one slot early: point 0's bone index lives right here, straight after pointCount (this
    // field was previously assumed to be padding/unused), and each point's own trailing WORD belongs
    // to the point after it - which is why the last point has no trailing WORD at all. Confirmed on
    // real puppet data: reading a trailing WORD for every point (including the last) overran two bytes
    // past the MDAT section's own declared length, landing exactly on the next section's magic bytes;
    // this shifted reading consumes the section's declared byte length exactly, with nothing left over.
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

// Looks ahead from searchStart for the next byte offset that looks like a valid clip header, to
// resynchronize past the still-undecoded per-clip trailer when a MDLA section holds more than one clip.
std::optional<size_t> findNextPuppetClipHeader (
    const std::vector<char>& data, size_t searchStart, size_t searchLimit, uint32_t expectedBoneCount
) {
    const auto readCString = [&data] (size_t& cursor) -> std::optional<std::string> {
	const size_t start = cursor;
	while (cursor < data.size () && data[cursor] != 0) {
	    const auto byte = static_cast<unsigned char> (data[cursor]);
	    if (byte < 0x20 || byte > 0x7e || cursor - start > 64) {
		return std::nullopt;
	    }
	    cursor++;
	}
	if (cursor >= data.size () || cursor == start) {
	    return std::nullopt;
	}
	std::string value (data.data () + start, cursor - start);
	cursor++;
	return value;
    };

    for (size_t offset = searchStart; offset < searchLimit; offset++) {
	size_t cursor = offset;
	if (!readCString (cursor).has_value () || !readCString (cursor).has_value ()) {
	    continue;
	}
	if (cursor + 16 > data.size ()) {
	    continue;
	}

	float fps;
	uint32_t frameCount;
	uint32_t flag;
	uint32_t boneCount;
	std::memcpy (&fps, data.data () + cursor, sizeof (fps));
	std::memcpy (&frameCount, data.data () + cursor + 4, sizeof (frameCount));
	std::memcpy (&flag, data.data () + cursor + 8, sizeof (flag));
	std::memcpy (&boneCount, data.data () + cursor + 12, sizeof (boneCount));

	if (fps >= 1.0f && fps <= 240.0f && frameCount >= 1 && frameCount <= 100000 && flag == 0
	    && boneCount == expectedBoneCount) {
	    return offset;
	}
    }

    return std::nullopt;
}

// Parses every baked animation clip out of the MDLA section (see docs/rendering/MDL_FILES.md).
std::vector<PuppetAnimationClip> parsePuppetAnimationClips (
    const std::vector<char>& data, const BinaryReader& reader, size_t mdlaOffset, uint32_t expectedBoneCount
) {
    reader.base ().seekg (static_cast<std::streamoff> (mdlaOffset), std::ios::beg);

    char header[9];
    reader.next (header, sizeof (header));

    (void) reader.nextUInt32 (); // total content size, unused
    const uint32_t clipCount = reader.nextUInt32 ();
    (void) reader.nextUInt32 (); // ambiguous animation id when there's more than one clip; matched by name instead
    (void) reader.nextUInt32 (); // unused, always 0 in every sample seen

    // this whole section is only trustworthy insofar as the MDLS "mdlaOffset" field that got us here
    // actually landed on a real MDLA layout for this MDLV sub-format - it's only been confirmed against
    // MDLV0023 samples so far. A clip count this large can only be a garbage read, not a real file.
    constexpr uint32_t maxPlausibleClipCount = 64;
    if (clipCount > maxPlausibleClipCount) {
	sLog.error (
	    "Puppet animation clip count (", clipCount, ") looks implausible, assuming this puppet's MDLA layout wasn't "
	    "recognized and skipping animation entirely"
	);
	return {};
    }

    std::vector<PuppetAnimationClip> clips;
    clips.reserve (clipCount);

    for (uint32_t clipIndex = 0; clipIndex < clipCount; clipIndex++) {
	PuppetAnimationClip clip;
	clip.name = reader.nextNullTerminatedString ();
	clip.mode = reader.nextNullTerminatedString ();
	clip.fps = reader.nextFloat ();
	clip.frameCount = reader.nextUInt32 ();
	(void) reader.nextUInt32 (); // unused, always 0 in every sample seen
	const uint32_t boneCount = reader.nextUInt32 ();

	constexpr uint32_t maxPlausibleFrameCount = 100000;
	if (clip.frameCount > maxPlausibleFrameCount || boneCount != expectedBoneCount) {
	    sLog.error (
		"Puppet animation clip ", clipIndex, " has an implausible frame/bone count (frames=", clip.frameCount,
		", bones=", boneCount, ", expected ", expectedBoneCount,
		"), assuming this puppet's MDLA layout wasn't recognized and stopping here"
	    );
	    break;
	}

	clip.boneTracks.resize (boneCount);

	for (uint32_t boneIndex = 0; boneIndex < boneCount; boneIndex++) {
	    (void) reader.nextUInt32 (); // separator, always 0 in every sample seen
	    const uint32_t trackBytes = reader.nextUInt32 ();
	    const uint32_t sampleCount = clip.frameCount + 1;
	    const uint32_t expectedBytes = sampleCount * 9 * sizeof (float);

	    if (trackBytes != expectedBytes) {
		sLog.error (
		    "Puppet animation track length mismatch in clip ", clip.name, " (expected ", expectedBytes,
		    ", got ", trackBytes, "), skipping"
		);
		reader.base ().seekg (static_cast<std::streamoff> (trackBytes), std::ios::cur);
		continue;
	    }

	    auto& track = clip.boneTracks[boneIndex];
	    track.reserve (sampleCount);

	    for (uint32_t sample = 0; sample < sampleCount; sample++) {
		PuppetKeyframe keyframe;
		keyframe.position = { reader.nextFloat (), reader.nextFloat (), reader.nextFloat () };
		keyframe.rotation = { reader.nextFloat (), reader.nextFloat (), reader.nextFloat () };
		keyframe.scale = { reader.nextFloat (), reader.nextFloat (), reader.nextFloat () };
		track.push_back (keyframe);
	    }
	}

	clips.push_back (std::move (clip));

	if (clipIndex + 1 < clipCount) {
	    const auto pos = static_cast<size_t> (reader.base ().tellg ());
	    const auto next = findNextPuppetClipHeader (data, pos, std::min (pos + 16384, data.size ()), expectedBoneCount);
	    if (!next.has_value ()) {
		sLog.error ("Could not resynchronize puppet animation data after clip ", clips.back ().name);
		break;
	    }
	    reader.base ().seekg (static_cast<std::streamoff> (*next), std::ios::beg);
	}
    }

    return clips;
}
}

CImage::ResolvedTransform CImage::localTransform (const Object& object) {
    glm::vec3 origin = object.origin->value->getVec3 ();
    glm::vec3 scale = glm::vec3 (1.0f);
    float angle = 0.0f;

    if (object.is<Image> ()) {
	const auto* image = object.as<Image> ();
	scale = image->scale->value->getVec3 ();
	angle = image->angles->value->getVec3 ().z;

	// cropoffset is already baked into the object's origin, adding it again shifts the layer
    } else if (object.is<Text> ()) {
	const auto* text = object.as<Text> ();
	scale = text->scale->value->getVec3 ();
    } else {
	scale = object.groupScale->value->getVec3 ();
	angle = object.groupAngles->value->getVec3 ().z;
    }

    return { origin, scale, angle };
}

CImage::ResolvedTransform CImage::resolveTransform (const Object& object) const {
    constexpr int kMaxParentDepth = 32;

    // Walk up the parent chain leaf-first, bounded by kMaxParentDepth to guard
    // against cycles. chain[0] is the requested object; the last entry is the root.
    const Object* chain[kMaxParentDepth + 1];
    int count = 0;
    const Object* current = &object;
    chain[count++] = current;

    while (current->parent.has_value ()) {
	if (count > kMaxParentDepth) {
	    sLog.error ("Parent transform chain is too deep; possible cycle at object id=", current->id);
	    break;
	}
	const auto* parentObject = this->getScene ().getObject (current->parent.value ());
	if (parentObject == nullptr) {
	    break;
	}
	current = &parentObject->getObject ();
	chain[count++] = current;
    }

    // Accumulate top-down: the root's local transform is already its resolved
    // transform, then fold each child onto its already-resolved parent.
    ResolvedTransform resolved = localTransform (*chain[count - 1]);
    float meshPivotAngle = 0.0f;
    for (int i = count - 2; i >= 0; --i) {
	ResolvedTransform local = localTransform (*chain[i]);

	// scene.json's "attachment" follows a named point on the direct parent's puppet rig (see
	// PuppetAttachmentPoint), not the parent's own origin. This mirrors the real engine's attachment
	// resolution (confirmed via disassembly of wallpaper64.exe's sub_140148A20, the function that
	// actually builds an object's world matrix): parentWorldMatrix * boneLocalMatrix, composed with NO
	// Y-axis sign flip anywhere in the chain - the real engine uses one consistent Y convention all the
	// way from JSON through every level of parent/child composition, flipping (if at all) exactly once,
	// at the very end in the camera projection.
	//
	// resolveTransform's own "origin" space already works this same unflipped way for ordinary
	// (non-attachment) children two lines below (`local.origin.y = anchorOrigin.y + offset.y`, no
	// negation) - it's only the FINAL CImage-constructor/updateScenePosition step that ever flips Y, to
	// go from this consistent origin-space into screen/pixel space. The bone's meshPosition, however,
	// comes from getAttachmentPointMeshTransform() already in that same unflipped origin-space
	// convention (see its own doc comment) - so it must be folded in raw, exactly like a normal child's
	// local.origin is, not re-flipped a second time. Confirmed against a real wallpaper with a genuinely
	// large bone rotation (mikasa/3764765600's "eye" attachment, ~-45 degrees): before this fix the
	// attachment landed off the top edge of the screen entirely; with position un-negated it lands
	// correctly on the face.
	//
	// anchorAngle (the bone's rotation, same sign/no-flip as position) rotates the attached child's own
	// local-origin nudge below, via the same offset-rotation every normal child already goes through -
	// that's required for *position* to track the bone correctly: a child's own declared origin is a
	// small offset in the attachment point's local frame, so it has to rotate along with whatever that
	// frame's current orientation is, same as it already scales along with the parent's current scale.
	// It also feeds the child's own final stored angle two lines below - the mathematically consistent
	// choice (attachmentWorldMatrix * childLocalMatrix), and the one actually confirmed working: mikasa's
	// eye (the only attachment point found so far riding a bone with genuine non-zero rotation) is
	// visible with this formula, just not at the correct angle (her declared local angle of ~44.6 degrees
	// and the eye bone's ~-45 degree rotation nearly cancel to ~0 net rotation, rendering as a thin
	// angular sliver instead of a natural lash contour - a real, unsolved cosmetic bug, tracked
	// separately, not this line).
	//
	// Two variants were tried and reverted, both regressions confirmed by the user on real hardware, not
	// just sandbox: (1) flipping only meshTransform->angle's sign within anchorAngle - since anchorAngle
	// also drives the offset-rotation above, this swung the eye's own (~355-unit) local-origin nudge by
	// nearly 90 degrees and pushed the object off the right edge of the screen entirely ("eyes completely
	// disappeared"). (2) splitting a separate finalAngle that dropped the bone's rotation from the final
	// angle entirely, reasoning that position and orientation could use different angles - this looked
	// like a plausible eyelash contour in an isolated sandbox crop, but the eye's own detail marks (a
	// small highlight dot, iris shading, a few lash strokes - confirmed via decode_tex.py on "mikasa
	// eye.tex": barely 0.7% of the canvas is non-transparent) are precisely positioned to overlay a
	// specific closed-eye crease baked into mikasaback's own texture; changing the mesh's rotation swings
	// those small marks to different screen pixels even though the object's own bounding-box center
	// doesn't move, and evidently rotated them off that tiny target entirely - user confirmed "eyes are
	// still invisible" with a real screenshot showing bare skin, no eye at all, where the sandbox crop had
	// suggested something was there. Reverted back to the single-anchorAngle formula below, which is the
	// last state confirmed actually visible (if wrongly rotated) on real hardware - a real fix for the
	// rotation needs to explain why a *different* angle would still hit the same crease, not just look
	// better in isolation.
	// meshPivotAngle: the remaining angle difference pivots around the mesh's own center, not the object origin
	glm::vec3 anchorOrigin = resolved.origin;
	float anchorAngle = resolved.angle;
	glm::vec2 anchorScale = { 1.0f, 1.0f };
	if (chain[i]->attachment.has_value () && chain[i]->parent.has_value ()) {
	    const auto* parentCObject = this->getScene ().getObject (chain[i]->parent.value ());
	    if (const auto* parentImage = dynamic_cast<const CImage*> (parentCObject); parentImage != nullptr) {
		if (const auto meshTransform = parentImage->getAttachmentPointMeshTransform (*chain[i]->attachment);
		    meshTransform.has_value ()) {
		    const glm::vec2 meshOffset = rotateVec2 (
			{ meshTransform->position.x * resolved.scale.x, meshTransform->position.y * resolved.scale.y },
			resolved.angle
		    );
		    anchorOrigin.x = resolved.origin.x + meshOffset.x;
		    anchorOrigin.y = resolved.origin.y + meshOffset.y;
		    anchorAngle = resolved.angle + meshTransform->angle;
		    // the bone's own scale (possibly negative, i.e. a mirrored bone) carries into whatever
		    // rides it, same as position/rotation
		    anchorScale = meshTransform->scale;
		    // the attachment matrix carries a static rotation that only orients the point's own frame,
		    // so it steers the child's offset but not its orientation, and is cancelled around the mesh center
		    meshPivotAngle += -meshTransform->restAngle;

		    if (!this->m_attachmentDiagnosticLogged.contains (chain[i]->id)) {
			this->m_attachmentDiagnosticLogged.insert (chain[i]->id);
			sLog.out (
			    "Attachment resolve for ", chain[i]->name, " (", chain[i]->id, "): point=",
			    *chain[i]->attachment, " meshPosition=(", meshTransform->position.x, ",",
			    meshTransform->position.y, ") boneAngleDeg=", glm::degrees (meshTransform->angle),
			    " boneScale=(", meshTransform->scale.x, ",", meshTransform->scale.y, ") parentOrigin=(",
			    resolved.origin.x, ",", resolved.origin.y, ") parentScale=", resolved.scale.x,
			    " anchorOrigin=(", anchorOrigin.x, ",", anchorOrigin.y, ") anchorAngleDeg=",
			    glm::degrees (anchorAngle), " restAngleDeg=", glm::degrees (meshTransform->restAngle)
			);
		    }
		}
	    }
	}

	const glm::vec2 offset
	    = rotateVec2 ({ local.origin.x * resolved.scale.x, local.origin.y * resolved.scale.y }, anchorAngle);
	local.origin.x = anchorOrigin.x + offset.x;
	local.origin.y = anchorOrigin.y + offset.y;
	local.origin.z = resolved.origin.z + local.origin.z * resolved.scale.z;
	local.scale.x *= anchorScale.x;
	local.scale.y *= anchorScale.y;
	resolved = { local.origin, local.scale * resolved.scale, local.angle + anchorAngle, meshPivotAngle };
    }

    return resolved;
}

CImage::CImage (Wallpapers::CScene& scene, const Image& image) :
    CObject (scene, image), CRenderable (scene, image, *image.model->material), ScriptableObject (scene, image),
    m_sceneSpacePosition (GL_NONE), m_copySpacePosition (GL_NONE), m_passSpacePosition (GL_NONE),
    m_texcoordCopy (GL_NONE), m_texcoordPass (GL_NONE), m_modelViewProjectionScreen (),
    m_modelViewProjectionPass (glm::mat4 (1.0)), m_modelViewProjectionCopy (), m_modelViewProjectionScreenInverse (),
    m_modelViewProjectionPassInverse (glm::inverse (m_modelViewProjectionPass)), m_modelViewProjectionCopyInverse (),
    m_modelMatrix (), m_viewProjectionMatrix (), m_image (image), m_pos (), m_initialized (false) {
    this->registerProperty ("origin", *image.origin->value);
    this->registerProperty ("scale", *image.scale->value);
    this->registerProperty ("angles", *image.angles->value);
    this->registerProperty ("visible", *image.visible->value);
    this->registerProperty ("copybackground", *image.copyBackground->value);
    this->registerProperty ("alpha", *image.alpha->value);
    this->registerProperty ("color", *image.color->value);
    this->registerProperty ("parallaxDepth", *image.parallaxDepth->value);
    this->registerEffectConstants (image.effects);

    auto scene_width = static_cast<float> (scene.getWidth ());
    auto scene_height = static_cast<float> (scene.getHeight ());

    const auto transform = this->resolveTransform (this->getImage ());
    glm::vec3 origin = transform.origin;
    glm::vec2 size = this->getSize ();
    glm::vec3 scale = transform.scale;

    this->detectTexture ();

    const bool placeholderTexture = this->m_texture == nullptr;

    if (this->m_texture == nullptr) {
	if (this->m_image.model->solidlayer && size.x == 0.0f && size.y == 0.0f) {
	    size.x = static_cast<float> (scene.getCanvasWidth ());
	    size.y = static_cast<float> (scene.getCanvasHeight ());
	}
	// TODO: create a dummy texture of correct size, fbo constructors should be enough, but this should be
	// properly handled
	// solid layers are often declared far larger than the scene, nothing samples
	// these buffers past the canvas so only the layout size has to stay as declared
	const glm::vec2 placeholderSize = { std::min (size.x, static_cast<float> (scene.getCanvasWidth ())),
					    std::min (size.y, static_cast<float> (scene.getCanvasHeight ())) };

	this->m_texture = std::make_shared<CFBO> (
	    "", TextureFormat_ARGB8888, TextureFlags_NoFlags, 1, size.x, size.y, placeholderSize.x, placeholderSize.y
	);
    }

    // If the wallpaper doesn't specify a size, fall back to the texture or model dimensions
    if ((size.x == 0.0f || size.y == 0.0f) && this->m_texture != nullptr) {
	size.x = static_cast<float> (this->m_texture->getRealWidth ());
	size.y = static_cast<float> (this->m_texture->getRealHeight ());
    } else if (
	(size.x == 0.0f || size.y == 0.0f) && this->getImage ().model->width.has_value ()
	&& this->getImage ().model->height.has_value ()
    ) {
	size.x = static_cast<float> (this->getImage ().model->width.value ());
	size.y = static_cast<float> (this->getImage ().model->height.value ());
    }

    // autosize takes the size of the loaded texture (a single frame for sprite sheets) over the
    // declared one, or the scene's for project layers. fullscreen still wins over it
    if (this->getImage ().model->autosize && this->getImage ().model->projectlayer) {
	size = { scene_width, scene_height };
    } else if (this->getImage ().model->autosize && !placeholderTexture
	&& std::dynamic_pointer_cast<const CFBO> (this->m_texture) == nullptr) {
	size.x = static_cast<float> (this->m_texture->getRealWidth ());
	size.y = static_cast<float> (this->m_texture->getRealHeight ());
    }

    // fullscreen layers should use the whole projection's size
    if (this->getImage ().model->fullscreen) {
	size = { static_cast<float> (scene.getCanvasWidth ()), static_cast<float> (scene.getCanvasHeight ()) };
	origin = { scene_width / 2, scene_height / 2, 0 };
    }
    this->m_size = size;

    // taken after the texture/model/fullscreen fallbacks above, unsized layers would otherwise get 0x0 buffers
    glm::vec2 bufferSize = size;

    if (placeholderTexture) {
	bufferSize = glm::min (bufferSize, glm::vec2 (scene.getCanvasWidth (), scene.getCanvasHeight ()));
    }

    this->updateScenePosition (origin, size, scale, scene_width, scene_height);

    // register both FBOs into the scene
    std::ostringstream nameA, nameB;

    // WE renders layers with LIGHTING or REFLECTION unlit into _rt_imageLayerAlbedo_<id> when they have offscreen
    // passes and lights them last (sub_1401914B0); here the first pass is lit instead, through PRELIGHTING
    nameA << "_rt_imageLayerComposite_" << this->getImage ().id << "_a";
    nameB << "_rt_imageLayerComposite_" << this->getImage ().id << "_b";

    // scene.json's own "clampuvs" is a per-object override on top of whatever the base texture
    // asset defaults to - without it, effects that distort UVs near the edges (refraction, ripples)
    // can wrap around and sample the opposite edge of the buffer instead of clamping.
    // compose layers always clamp, their effects would otherwise wrap samples from the opposite edge
    const uint32_t compositeFlags = (this->getImage ().clampUVs || this->getImage ().model->passthrough)
	? (this->m_texture->getFlags () | TextureFlags_ClampUVs)
	: this->m_texture->getFlags ();

    // layer buffers are 16 bit float in HDR scene rendering (sub_1401E7170)
    const TextureFormat layerFormat = this->getScene ().isHDR () ? TextureFormat_RGBA16161616f : TextureFormat_ARGB8888;
    this->m_currentMainFBO = this->m_mainFBO = scene.create (
	nameA.str (), layerFormat, compositeFlags, 1, { bufferSize.x, bufferSize.y }, { bufferSize.x, bufferSize.y }
    );
    this->m_currentSubFBO = this->m_subFBO = scene.create (
	nameB.str (), layerFormat, compositeFlags, 1, { bufferSize.x, bufferSize.y }, { bufferSize.x, bufferSize.y }
    );

    GLfloat sceneSpacePosition[] = { this->m_pos.x, this->m_pos.y, 0.0f, this->m_pos.x, this->m_pos.w, 0.0f,
				     this->m_pos.z, this->m_pos.y, 0.0f, this->m_pos.z, this->m_pos.y, 0.0f,
				     this->m_pos.x, this->m_pos.w, 0.0f, this->m_pos.z, this->m_pos.w, 0.0f };

    float width = 1.0f;
    float height = 1.0f;

    if (this->getTexture ()->isAnimated ()) {
	// animated images use different coordinates as they're essentially a texture atlas
	width = static_cast<float> (this->getTexture ()->getRealWidth ())
	    / static_cast<float> (this->getTexture ()->getTextureWidth (0));
	height = static_cast<float> (this->getTexture ()->getRealHeight ())
	    / static_cast<float> (this->getTexture ()->getTextureHeight (0));
    }
    else if (
	this->getTexture () != nullptr
	&& (this->getTexture ()->getTextureWidth (0) != this->getTexture ()->getRealWidth ()
	    || this->getTexture ()->getTextureHeight (0) != this->getTexture ()->getRealHeight ())
    ) {
	// Account for padding in non-power-of-two textures: clamp UVs to the real content
	width = static_cast<float> (this->getTexture ()->getRealWidth ())
	    / static_cast<float> (this->getTexture ()->getTextureWidth (0));
	height = static_cast<float> (this->getTexture ()->getRealHeight ())
	    / static_cast<float> (this->getTexture ()->getTextureHeight (0));
    }

    float x = 0.0f;
    float y = 0.0f;

    if (this->getTexture ()->isAnimated ()) {
	// animations should be copied completely
	x = 0.0f;
	y = 0.0f;
	width = 1.0f;
	height = 1.0f;
    }

    GLfloat realWidth = size.x;
    GLfloat realHeight = size.y;
    GLfloat realX = 0.0;
    GLfloat realY = 0.0;

    if (this->getImage ().model->passthrough) {
	// Passthrough shaders fill the destination FBO from texcoords and sample the scene using positions.
	// Keep the destination quad full-screen in local FBO space, but pass scene-space positions through.
	x = 0.0f;
	y = 0.0f;
	width = 1.0f;
	height = 1.0f;
	realX = this->m_pos.x;
	realY = this->m_pos.w;
	realWidth = this->m_pos.z;
	realHeight = this->m_pos.y;

	if (this->getImage ().model->fullscreen) {
	    realX = -1.0;
	    realY = -1.0;
	    realWidth = 1.0;
	    realHeight = 1.0;
	}
    }

    GLfloat texcoordCopy[] = { x, height, x, y, width, height, width, height, x, y, width, y };

    GLfloat copySpacePosition[] = { realX,     realHeight, 0.0f, realX, realY, 0.0f, realWidth, realHeight, 0.0f,
				    realWidth, realHeight, 0.0f, realX, realY, 0.0f, realWidth, realY,      0.0f };

    GLfloat texcoordPass[] = { 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f };

    GLfloat passSpacePosition[]
	= { -1.0, 1.0, 0.0f, -1.0, -1.0, 0.0f, 1.0, 1.0, 0.0f, 1.0, 1.0, 0.0f, -1.0, -1.0, 0.0f, 1.0, -1.0, 0.0f };

    glGenBuffers (1, &this->m_sceneSpacePosition);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_sceneSpacePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (sceneSpacePosition), sceneSpacePosition, GL_STATIC_DRAW);

    glGenBuffers (1, &this->m_copySpacePosition);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_copySpacePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (copySpacePosition), copySpacePosition, GL_STATIC_DRAW);

    glGenBuffers (1, &this->m_passSpacePosition);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_passSpacePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (passSpacePosition), passSpacePosition, GL_STATIC_DRAW);

    glGenBuffers (1, &this->m_texcoordCopy);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_texcoordCopy);
    glBufferData (GL_ARRAY_BUFFER, sizeof (texcoordCopy), texcoordCopy, GL_STATIC_DRAW);

    glGenBuffers (1, &this->m_texcoordPass);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_texcoordPass);
    glBufferData (GL_ARRAY_BUFFER, sizeof (texcoordPass), texcoordPass, GL_STATIC_DRAW);

    this->m_hasPuppetMesh = this->loadPuppetMesh (size);

    this->m_sceneCenter
	= glm::vec3 ((this->m_pos.x + this->m_pos.z) / 2.0f, (this->m_pos.y + this->m_pos.w) / 2.0f, 0.0f);

    this->m_modelViewProjectionScreen = this->getViewProjection ();
    // must match m_modelViewProjectionScreen - updateScreenSpacePosition() may skip recomputing it
    this->m_modelViewProjectionScreenInverse = glm::inverse (this->m_modelViewProjectionScreen);
    this->updateEffectTextureProjection ();

    if (this->getImage ().model->passthrough) {
	this->m_modelViewProjectionCopy = this->m_modelViewProjectionScreen;
    } else {
	this->m_modelViewProjectionCopy = glm::ortho<float> (0.0, size.x, 0.0, size.y);
    }
    this->m_modelViewProjectionCopyInverse = glm::inverse (this->m_modelViewProjectionCopy);
    this->m_modelMatrix = glm::ortho<float> (0.0, size.x, 0.0, size.y);
    this->m_viewProjectionMatrix = glm::mat4 (1.0);

    // marks the texture as used, which starts video playback if it isn't already
    this->m_texture->incrementUsageCount ();
}

void CImage::updateTextures () const {
    this->getTexture ()->update ();

    for (const auto* pass : this->m_passes) {
	pass->updatePlaybackTextures ();
    }
}

bool CImage::containsScenePoint (const glm::vec2& point) const {
    // m_pos is stored centered on the scene with y pointing down, x/z are left/right and y/w bottom/top
    const float x = point.x - static_cast<float> (this->getScene ().getWidth ()) / 2.0f;
    const float y = static_cast<float> (this->getScene ().getHeight ()) / 2.0f - point.y;

    return x >= std::min (this->m_pos.x, this->m_pos.z) && x <= std::max (this->m_pos.x, this->m_pos.z)
	&& y >= std::min (this->m_pos.y, this->m_pos.w) && y <= std::max (this->m_pos.y, this->m_pos.w);
}

glm::vec2 CImage::getSceneCenter () const {
    return { (this->m_pos.x + this->m_pos.z) / 2.0f + static_cast<float> (this->getScene ().getWidth ()) / 2.0f,
	     static_cast<float> (this->getScene ().getHeight ()) / 2.0f - (this->m_pos.y + this->m_pos.w) / 2.0f };
}

void CImage::refreshScenePosition () {
    const auto sceneWidth = static_cast<float> (this->getScene ().getWidth ());
    const auto sceneHeight = static_cast<float> (this->getScene ().getHeight ());
    const auto transform = this->resolveTransform (this->getImage ());
    glm::vec3 origin = transform.origin;
    const glm::vec2 size = this->resolveGeometrySize (sceneWidth, sceneHeight, origin);

    this->updateScenePosition (origin, size, transform.scale, sceneWidth, sceneHeight);
}

CImage::~CImage () {
    this->m_texture->decrementUsageCount ();

    // delete passes first as they depend on the image's data
    for (auto* pass : this->m_allPasses.empty () ? this->m_passes : this->m_allPasses) {
	delete pass;
    }

    this->m_passes.clear ();
    this->m_allPasses.clear ();

    glDeleteBuffers (1, &this->m_sceneSpacePosition);
    glDeleteBuffers (1, &this->m_copySpacePosition);
    glDeleteBuffers (1, &this->m_passSpacePosition);
    glDeleteBuffers (1, &this->m_texcoordCopy);
    glDeleteBuffers (1, &this->m_texcoordPass);
    if (this->m_puppetSpacePosition != GL_NONE) {
	glDeleteBuffers (1, &this->m_puppetSpacePosition);
    }
    if (this->m_puppetTexCoord != GL_NONE) {
	glDeleteBuffers (1, &this->m_puppetTexCoord);
    }
    if (this->m_puppetIndices != GL_NONE) {
	glDeleteBuffers (1, &this->m_puppetIndices);
    }
}

bool CImage::loadPuppetMesh (const glm::vec2& size) {
    if (!this->getImage ().model->puppet.has_value ()) {
	return false;
    }

    try {
	const auto stream = this->getScene ().getScene ().project.assetLocator->read (*this->getImage ().model->puppet);
	std::vector<char> data { std::istreambuf_iterator<char> (*stream), std::istreambuf_iterator<char> () };

	constexpr size_t markerSize = 9;
	constexpr size_t meshHeaderSize = sizeof (uint32_t) * 2;

	const std::string puppetVersion
	    = data.size () >= markerSize ? std::string (data.data (), strlen ("MDLV0021")) : "";

	const size_t mdlsOffset = [&data] () -> size_t {
	    for (size_t offset = markerSize; offset + strlen ("MDLS") < data.size (); offset++) {
		if (std::memcmp (data.data () + offset, "MDLS", strlen ("MDLS")) == 0) {
		    return offset;
		}
	    }
	    return data.size ();
	}();

	auto meshBuffer = std::make_unique<char[]> (data.size ());
	std::copy (data.begin (), data.end (), meshBuffer.get ());
	const BinaryReader reader (std::make_shared<MemoryStream> (std::move (meshBuffer), data.size ()));

	const auto layout = resolvePuppetVertexLayout (reader, markerSize, mdlsOffset, meshHeaderSize);
	if (!layout.has_value ()) {
	    sLog.error ("Could not find a usable MDLV mesh block in ", *this->getImage ().model->puppet);
	    return false;
	}

	const auto mesh = readPuppetMeshData (reader, layout->block, meshHeaderSize, layout->vertexStride);
	if (!mesh.has_value ()) {
	    sLog.error ("Could not find a usable MDLV mesh block in ", *this->getImage ().model->puppet);
	    return false;
	}

	this->m_puppetRawPositions = mesh->positions;
	this->updatePuppetPositionBuffer (size);

	glGenBuffers (1, &this->m_puppetTexCoord);
	glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetTexCoord);
	glBufferData (GL_ARRAY_BUFFER, mesh->texcoords.size () * sizeof (GLfloat), mesh->texcoords.data (), GL_STATIC_DRAW);

	glGenBuffers (1, &this->m_puppetIndices);
	glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->m_puppetIndices);
	glBufferData (
	    GL_ELEMENT_ARRAY_BUFFER, mesh->indices.size () * sizeof (GLushort), mesh->indices.data (), GL_STATIC_DRAW
	);

	this->m_puppetIndexCount = static_cast<GLsizei> (mesh->indices.size ());

	sLog.out (
	    "Loaded puppet mesh ", *this->getImage ().model->puppet, " version=", puppetVersion, " stride=",
	    layout->vertexStride, " vertices=", this->m_puppetRawPositions.size () / 3, " indices=", this->m_puppetIndexCount
	);

	this->m_puppetBones.clear ();
	this->m_puppetActiveAnimations.clear ();
	this->m_puppetBlendIndices.clear ();
	this->m_puppetBlendWeights.clear ();
	this->m_puppetAttachmentPoints.clear ();
	this->m_puppetBoneWorldAnimated.clear ();

	const auto blend = readPuppetBlendData (reader, layout->block, meshHeaderSize, layout->vertexStride);
	if (blend.has_value ()) {
	    this->m_puppetBlendIndices = blend->indices;
	    this->m_puppetBlendWeights = blend->weights;
	}

	if (mdlsOffset < data.size () && blend.has_value ()) {
	    try {
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

		this->m_puppetBones = std::move (boneSet.bones);
		this->m_puppetBoneWorldAnimated = worldBind;

		// the MDLS "next section" field is trusted at face value below, but that's only been
		// confirmed against MDLV0023 puppet-warp samples - other MDLV sub-formats (e.g. rope/particle
		// rigs) may lay out MDLS differently, in which case this field is meaningless. It can point
		// to either an optional MDAT (attachment points) section or straight to MDLA; cross-check
		// which one (if either) it actually is before trusting anything read from that offset.
		constexpr std::array<char, 4> mdlaMagic = { 'M', 'D', 'L', 'A' };
		constexpr std::array<char, 4> mdatMagic = { 'M', 'D', 'A', 'T' };
		const auto magicAt = [&data] (size_t offset, const std::array<char, 4>& magic) {
		    return offset + magic.size () <= data.size ()
			&& std::equal (magic.begin (), magic.end (), data.begin () + static_cast<long> (offset));
		};

		size_t mdlaOffset = boneSet.nextSectionOffset;
		bool mdlaOffsetLooksValid = magicAt (mdlaOffset, mdlaMagic);

		if (!mdlaOffsetLooksValid && magicAt (mdlaOffset, mdatMagic)) {
		    auto attachmentSet
			= parsePuppetAttachmentPoints (reader, mdlaOffset, static_cast<uint32_t> (this->m_puppetBones.size ()));
		    this->m_puppetAttachmentPoints = std::move (attachmentSet.points);
		    mdlaOffset = attachmentSet.mdlaOffset;
		    mdlaOffsetLooksValid = magicAt (mdlaOffset, mdlaMagic);
		}

		std::vector<PuppetAnimationClip> clips;
		if (mdlaOffsetLooksValid) {
		    clips = parsePuppetAnimationClips (
			data, reader, mdlaOffset, static_cast<uint32_t> (this->m_puppetBones.size ())
		    );
		} else {
		    sLog.error (
			"Puppet MDLS data for ", *this->getImage ().model->puppet,
			" doesn't lead to a recognizable MDLA section, skipping animation for this puppet"
		    );
		}

		// puppets can declare several simultaneous "additive" layers (idle sway, blinking, hand
		// movement, ...) - collect every matching one here; updatePuppetSkinning blend-weights
		// them together per bone using each layer's own "blend" setting.
		for (const auto& layer : this->getImage ().animationLayers) {
		    auto match = std::find_if (clips.begin (), clips.end (), [&layer] (const PuppetAnimationClip& clip) {
			return clip.name == layer->name;
		    });
		    if (match == clips.end ()) {
			continue;
		    }

		    this->m_puppetActiveAnimations.push_back (
			PuppetActiveAnimation { .clip = std::move (*match), .layer = layer.get () }
		    );
		}

		if (this->m_puppetActiveAnimations.empty () && !clips.empty () && !this->getImage ().animationLayers.empty ()) {
		    sLog.out (
			"No puppet animation clip name matched an animation layer for ", *this->getImage ().model->puppet,
			", defaulting to the first clip (", clips.front ().name, ")"
		    );
		    this->m_puppetActiveAnimations.push_back (
			PuppetActiveAnimation {
			    .clip = std::move (clips.front ()), .layer = this->getImage ().animationLayers.front ().get () }
		    );
		}

		for (const auto& active : this->m_puppetActiveAnimations) {
		    sLog.out (
			"Playing puppet animation ", active.clip.name, " (", active.clip.mode, ", ", active.clip.fps,
			" fps, ", active.clip.frameCount, " frames) on ", *this->getImage ().model->puppet
		    );
		}

		if (!this->m_puppetAttachmentPoints.empty ()) {
		    std::string names;
		    for (const auto& point : this->m_puppetAttachmentPoints) {
			names += (names.empty () ? "" : ", ") + point.name;
		    }
		    sLog.out (
			"Found ", this->m_puppetAttachmentPoints.size (), " puppet attachment point(s) on ",
			*this->getImage ().model->puppet, ": ", names
		    );
		}
	    } catch (const std::exception& ex) {
		sLog.error (
		    "Could not load puppet skeleton/animation from ", *this->getImage ().model->puppet, ": ", ex.what (),
		    " (falling back to the static bind pose)"
		);
		this->m_puppetBones.clear ();
		this->m_puppetActiveAnimations.clear ();
		this->m_puppetAttachmentPoints.clear ();
		this->m_puppetBoneWorldAnimated.clear ();
	    }
	}

	return true;
    } catch (const std::exception& ex) {
	sLog.error ("Could not load puppet mesh ", *this->getImage ().model->puppet, ": ", ex.what ());
	return false;
    }
}

void CImage::updatePuppetPositionBuffer (const glm::vec2& size) {
    // once an animation clip is driving the mesh, its skinned output replaces the static bind pose
    // as the source of truth - the bind pose (m_puppetRawPositions) is kept around unchanged, since
    // skinning is recomputed from it fresh every frame, not accumulated from the previous frame
    const auto& source
	= !this->m_puppetActiveAnimations.empty () && !this->m_puppetSkinnedPositions.empty () ? this->m_puppetSkinnedPositions : this->m_puppetRawPositions;

    if (source.empty ()) {
	return;
    }

    // A puppet with effects is multi-pass: its geometry pass renders into its own object-sized
    // intermediate FBO (see setupPasses(), the "writesToTarget" branch) using the local-canvas
    // m_modelViewProjectionCopy projection, and later passes composite that FBO's texture onto the
    // scene the normal (non-puppet) way - that first pass still needs plain local canvas coordinates
    // (0..size, matching its texcoords) to line up with that projection. Only a puppet with no
    // effects skips straight from its one and only pass to the shared scene FBO, using
    // m_modelViewProjectionScreen (see setupPasses()) - that path needs vertices already in the same
    // absolute scene-space coordinates uploadGeometryBuffers() bakes into sceneSpacePosition for a
    // normal quad, or every vertex renders shifted by a constant offset equal to wherever this object
    // should have been, reading as the whole mesh floating somewhere else on screen entirely.
    const bool bakeScenePosition = this->m_passes.size () <= 1 || this->m_puppetMeshLast;

    std::vector<GLfloat> positions;
    positions.reserve (source.size ());
    for (size_t index = 0; index + 2 < source.size (); index += 3) {
	const float localX = size.x / 2.0f + source[index];
	const float localY = size.y / 2.0f - source[index + 1];
	if (bakeScenePosition) {
	    // maps the local-canvas coordinate onto this object's scene-space bounding box; m_pos.w is
	    // its bottom edge (m_pos.y is the top, see updateScenePosition()) so localY==0 has to land
	    // there, not on m_pos.y, or the puppet renders vertically flipped
	    positions.push_back (this->m_pos.x + localX * this->m_puppetScale.x);
	    positions.push_back (this->m_pos.w + localY * this->m_puppetScale.y);
	} else {
	    positions.push_back (localX);
	    positions.push_back (localY);
	}
	// raw .mdl Z values aren't used by this engine's orthographic puppet compositing (depth test
	// is disabled for puppets; layering comes from draw order + alpha blending) - and glm::ortho's
	// clip.z = -localZ has no near/far normalization, so a puppet's real mesh depth (tens of units)
	// would get clipped outside [-1,1] and lose most of the mesh. Zero it instead.
	positions.push_back (0.0f);
    }

    // skip the constructor's pre-setup() call, where m_passes/m_pos/m_puppetScale aren't resolved yet
    if (!this->m_puppetPositionDiagnosticLogged && !this->m_passes.empty ()) {
	this->m_puppetPositionDiagnosticLogged = true;
	glm::vec3 boundsMin (std::numeric_limits<float>::max ());
	glm::vec3 boundsMax (std::numeric_limits<float>::lowest ());
	for (size_t i = 0; i + 2 < positions.size (); i += 3) {
	    const glm::vec3 p (positions[i], positions[i + 1], positions[i + 2]);
	    boundsMin = glm::min (boundsMin, p);
	    boundsMax = glm::max (boundsMax, p);
	}
	sLog.out (
	    "Puppet position bake for ", this->getImage ().name, " (", this->getId (), "): bakeScenePosition=",
	    bakeScenePosition, " passes=", this->m_passes.size (), " vertexCount=", positions.size () / 3,
	    " boundsMin=(", boundsMin.x, ",", boundsMin.y, ",", boundsMin.z, ") boundsMax=(", boundsMax.x, ",",
	    boundsMax.y, ",", boundsMax.z, ")"
	);
    }

    if (this->m_puppetSpacePosition == GL_NONE) {
	glGenBuffers (1, &this->m_puppetSpacePosition);
    }
    glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetSpacePosition);
    glBufferData (GL_ARRAY_BUFFER, positions.size () * sizeof (GLfloat), positions.data (), GL_DYNAMIC_DRAW);
}

namespace {
glm::vec3 lerp (const glm::vec3& a, const glm::vec3& b, float alpha) { return a + (b - a) * alpha; }
}

void CImage::updatePuppetSkinning () {
    if (this->m_puppetActiveAnimations.empty () || this->m_puppetBones.empty ()) {
	return;
    }

    if (this->getScene ().getContext ().getApp ().getContext ().settings.render.debug.noPuppetAnimation) {
	return;
    }

    // every matching, currently-visible animation layer plays and blends by its own "blend" weight,
    // instead of only the first one. bindLocal from the MDLS array is deliberately not used as a
    // rotation baseline - a clip's own sample is used directly, since some files carry bones whose
    // MDLS bindLocal translation is wildly different from what their animation samples say, and
    // falling back to it visibly detaches whatever that bone drives.
    struct ActiveLayerSample {
	const PuppetAnimationClip* clip;
	uint32_t frame0;
	uint32_t frame1;
	float alpha;
	float blend;
    };

    std::vector<ActiveLayerSample> samples;
    for (const auto& candidate : this->m_puppetActiveAnimations) {
	if (candidate.layer == nullptr || !candidate.layer->visible->value->getBool ()) {
	    continue;
	}

	const auto& clip = candidate.clip;
	const float duration = clip.fps > 0.0f ? static_cast<float> (clip.frameCount) / clip.fps : 0.0f;
	const float rate = candidate.layer->rate->value->getFloat ();

	float frameFloat = 0.0f;
	if (duration > 0.0f) {
	    // "mirror" clips play forward then backward, so the end flows back into the start instead of snapping
	    const bool mirror = std::ranges::equal (clip.mode, std::string_view ("mirror"), [] (char a, char b) {
		return std::tolower (static_cast<unsigned char> (a)) == b;
	    });
	    const float period = mirror ? duration * 2.0f : duration;
	    float elapsed = std::fmod (g_Time * rate, period);
	    if (elapsed < 0.0f) {
		elapsed += period;
	    }
	    if (mirror && elapsed > duration) {
		elapsed = period - elapsed;
	    }
	    frameFloat = elapsed * clip.fps;
	}

	const auto frame0 = std::min (static_cast<uint32_t> (frameFloat), clip.frameCount);
	samples.push_back (ActiveLayerSample {
	    .clip = &clip, .frame0 = frame0, .frame1 = std::min (frame0 + 1, clip.frameCount),
	    .alpha = frameFloat - static_cast<float> (frame0), .blend = candidate.layer->blend->value->getFloat () });
    }

    if (samples.empty ()) {
	return;
    }

    std::vector<int> animatedParents (this->m_puppetBones.size ());
    std::vector<glm::mat4> animatedLocals (this->m_puppetBones.size ());

    for (size_t i = 0; i < this->m_puppetBones.size (); i++) {
	const auto& bone = this->m_puppetBones[i];
	animatedParents[i] = bone.parent;

	const glm::vec3 bindPosition (bone.bindLocal[3]);
	glm::vec3 position = bindPosition;
	bool positionBased = false;
	glm::vec3 rotation (0.0f);
	glm::vec3 scale (1.0f);

	// each layer contributes a blend-weighted delta from the shared baseline (bind position, zero
	// rotation, unit scale) rather than replacing it outright
	for (const auto& sample : samples) {
	    if (i >= sample.clip->boneTracks.size () || sample.clip->boneTracks[i].size () <= sample.frame1) {
		continue;
	    }

	    const auto& track = sample.clip->boneTracks[i];
	    const glm::vec3 trackPosition = lerp (track[sample.frame0].position, track[sample.frame1].position, sample.alpha);
	    const glm::vec3 trackRotation = lerp (track[sample.frame0].rotation, track[sample.frame1].rotation, sample.alpha);
	    const glm::vec3 trackScale = lerp (track[sample.frame0].scale, track[sample.frame1].scale, sample.alpha);

	    // deltas are measured from the clip's own first frame, some rigs carry a static track pose far from bindLocal
	    const glm::vec3 restPosition = track[0].position;
	    if (!positionBased) {
		position = restPosition;
		positionBased = true;
	    }
	    position += sample.blend * (trackPosition - restPosition);
	    rotation += sample.blend * trackRotation;
	    scale += sample.blend * (trackScale - glm::vec3 (1.0f));
	}

	glm::mat4 local = glm::translate (glm::mat4 (1.0f), position);
	local = glm::rotate (local, rotation.z, glm::vec3 (0.0f, 0.0f, 1.0f));
	local = glm::rotate (local, rotation.y, glm::vec3 (0.0f, 1.0f, 0.0f));
	local = glm::rotate (local, rotation.x, glm::vec3 (1.0f, 0.0f, 0.0f));
	local = glm::scale (local, scale);

	animatedLocals[i] = local;
    }

    const std::vector<glm::mat4> worldAnimated = composeBoneWorldTransforms (animatedParents, animatedLocals);

    // attachment points (see getAttachmentPointMeshTransform) need the live bone transforms independently
    // of the skin matrices below, which fold in the inverse bind pose
    this->m_puppetBoneWorldAnimated = worldAnimated;

    std::vector<glm::mat4> skinMatrices (this->m_puppetBones.size ());
    for (size_t i = 0; i < this->m_puppetBones.size (); i++) {
	skinMatrices[i] = worldAnimated[i] * this->m_puppetBones[i].inverseBindWorld;
    }

    const size_t vertexCount = this->m_puppetRawPositions.size () / 3;
    this->m_puppetSkinnedPositions.assign (this->m_puppetRawPositions.size (), 0.0f);

    for (size_t v = 0; v < vertexCount; v++) {
	const glm::vec4 bindPos (
	    this->m_puppetRawPositions[v * 3], this->m_puppetRawPositions[v * 3 + 1], this->m_puppetRawPositions[v * 3 + 2],
	    1.0f
	);

	glm::vec3 skinned (0.0f);
	const glm::uvec4& indices = v < this->m_puppetBlendIndices.size () ? this->m_puppetBlendIndices[v] : glm::uvec4 (0);
	const glm::vec4& weights = v < this->m_puppetBlendWeights.size () ? this->m_puppetBlendWeights[v] : glm::vec4 (0.0f);

	for (int influence = 0; influence < 4; influence++) {
	    const float weight = weights[influence];
	    if (weight == 0.0f) {
		continue;
	    }

	    const uint32_t boneIndex = indices[influence];
	    if (boneIndex >= skinMatrices.size ()) {
		continue;
	    }

	    skinned += weight * glm::vec3 (skinMatrices[boneIndex] * bindPos);
	}

	this->m_puppetSkinnedPositions[v * 3] = skinned.x;
	this->m_puppetSkinnedPositions[v * 3 + 1] = skinned.y;
	this->m_puppetSkinnedPositions[v * 3 + 2] = skinned.z;
    }

    this->updatePuppetPositionBuffer (this->m_size);
}

std::optional<CImage::AttachmentPointTransform> CImage::getAttachmentPointMeshTransform (const std::string& name) const {
    if (this->m_puppetBoneWorldAnimated.empty ()) {
	return std::nullopt;
    }

    const auto it = std::find_if (
	this->m_puppetAttachmentPoints.begin (), this->m_puppetAttachmentPoints.end (),
	[&name] (const PuppetAttachmentPoint& point) { return point.name == name; }
    );

    if (it == this->m_puppetAttachmentPoints.end () || static_cast<size_t> (it->boneIndex) >= this->m_puppetBoneWorldAnimated.size ()) {
	return std::nullopt;
    }

    const glm::mat4 animatedWorld = this->m_puppetBoneWorldAnimated[it->boneIndex] * it->localTransform;

    const float angle = std::atan2 (animatedWorld[0][1], animatedWorld[0][0]);

    // scale.y = det(X,Y)/scale.x, projecting the transformed Y-basis onto what an unreflected
    // rotation by `angle` would have produced - comes out negative if the bone's matrix includes a
    // reflection (mirrored bone), instead of folding that into a bogus rotation angle
    const float scaleX = glm::length (glm::vec2 (animatedWorld[0]));
    const glm::vec2 scale
	= scaleX > 1e-6f ? glm::vec2 (
	      scaleX, (animatedWorld[0][0] * animatedWorld[1][1] - animatedWorld[0][1] * animatedWorld[1][0]) / scaleX
	  )
			 : glm::vec2 (scaleX, glm::length (glm::vec2 (animatedWorld[1])));

    const glm::mat4 bindWorld = glm::inverse (this->m_puppetBones[it->boneIndex].inverseBindWorld) * it->localTransform;
    const float restAngle = std::atan2 (bindWorld[0][1], bindWorld[0][0]);

    return AttachmentPointTransform {
	.position = glm::vec3 (animatedWorld[3]), .angle = angle, .scale = scale, .restAngle = restAngle
    };
}

void CImage::setupPuppetGeometryCallback (Effects::CPass* pass) const {
    pass->setGeometryCallback (
	[this, pass] () {
	    const GLint position = glGetAttribLocation (pass->getProgramID (), "a_Position");
	    const GLint texCoord = glGetAttribLocation (pass->getProgramID (), "a_TexCoord");

	    if (!this->m_puppetDrawDiagnosticLogged) {
		this->m_puppetDrawDiagnosticLogged = true;
		sLog.out (
		    "Puppet draw setup for ", this->getImage ().name, " (", this->getId (), "): programID=",
		    pass->getProgramID (), " a_Position=", position, " a_TexCoord=", texCoord, " indexCount=",
		    this->m_puppetIndexCount, " size=", this->m_size.x, "x", this->m_size.y
		);
	    }

	    if (position >= 0) {
		glEnableVertexAttribArray (position);
		glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetSpacePosition);
		glVertexAttribPointer (position, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
	    }

	    if (texCoord >= 0) {
		glEnableVertexAttribArray (texCoord);
		glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetTexCoord);
		glVertexAttribPointer (texCoord, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
	    }

	    // updatePuppetPositionBuffer flips Y when converting mesh-space positions to screen space,
	    // which mirrors the mesh and reverses triangle winding relative to what the MDL file's index
	    // buffer encodes - but not necessarily uniformly across the whole mesh, since real puppet
	    // meshes aren't guaranteed to be consistently wound to begin with. Puppet content is flat 2D
	    // art with no real backface concept, so rather than chase the "correct" winding, just never
	    // cull it - a material requesting cullmode "normal" would otherwise silently drop whichever
	    // subset of triangles ends up on the wrong side, which looks like patchy missing geometry.
	    glDisable (GL_CULL_FACE);
	},
	[this, pass] () {
	    GLint currentFramebuffer = 0;
	    glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &currentFramebuffer);
	    if (currentFramebuffer != static_cast<GLint> (this->getScene ().getFBO ()->getFramebuffer ())) {
		GLfloat previousClearColor[4] = {};
		glGetFloatv (GL_COLOR_CLEAR_VALUE, previousClearColor);
		glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
		glClear (GL_COLOR_BUFFER_BIT);
		glClearColor (
		    previousClearColor[0], previousClearColor[1], previousClearColor[2], previousClearColor[3]
		);
	    }

	    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->m_puppetIndices);
	    glDrawElements (GL_TRIANGLES, this->m_puppetIndexCount, GL_UNSIGNED_SHORT, nullptr);

	    {
		static int mikasaEyeDumpCounter = 0;
		if (this->getId () == 603 && mikasaEyeDumpCounter++ == 5) {
		    GLint vp[4] = {};
		    glGetIntegerv (GL_VIEWPORT, vp);
		    const int w = vp[2], h = vp[3];
		    if (w > 0 && h > 0 && w < 8192 && h < 8192) {
			std::vector<unsigned char> pixels (static_cast<size_t> (w) * h * 4);
			glReadPixels (0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data ());
			FILE* f = fopen ("/tmp/mikasa_eye_bakepass_dump.raw", "wb");
			if (f) {
			    fwrite (&w, sizeof (int), 1, f);
			    fwrite (&h, sizeof (int), 1, f);
			    fwrite (pixels.data (), 1, pixels.size (), f);
			    fclose (f);
			    sLog.out ("TEMP-DIAG dumped FBO contents for mikasa eye bake pass: ", w, "x", h, " to /tmp/mikasa_eye_bakepass_dump.raw");
			}
		    }
		}
	    }

	    if (!this->m_puppetDrawErrorChecked) {
		this->m_puppetDrawErrorChecked = true;
		GLint boundFBO = 0;
		GLint viewport[4] = {};
		GLint boundTexture = 0;
		glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &boundFBO);
		glGetIntegerv (GL_VIEWPORT, viewport);
		glActiveTexture (GL_TEXTURE0);
		glGetIntegerv (GL_TEXTURE_BINDING_2D, &boundTexture);
		const GLenum err = glGetError ();
		const GLboolean cullEnabled = glIsEnabled (GL_CULL_FACE);
		const GLboolean depthEnabled = glIsEnabled (GL_DEPTH_TEST);
		const GLboolean scissorEnabled = glIsEnabled (GL_SCISSOR_TEST);
		const GLboolean blendEnabled = glIsEnabled (GL_BLEND);
		GLint cullFaceMode = 0, frontFace = 0;
		glGetIntegerv (GL_CULL_FACE_MODE, &cullFaceMode);
		glGetIntegerv (GL_FRONT_FACE, &frontFace);
		GLboolean colorMask[4] = {};
		glGetBooleanv (GL_COLOR_WRITEMASK, colorMask);
		sLog.out (
		    "Puppet draw result for ", this->getImage ().name, " (", this->getId (), "): glError=", err,
		    " boundFBO=", boundFBO, " sceneFBO=", this->getScene ().getFBO ()->getFramebuffer (), " viewport=(",
		    viewport[0], ",", viewport[1], ",", viewport[2], ",", viewport[3], ") boundTexture=", boundTexture,
		    " ownTextureReady=", (this->getTexture () != nullptr && this->getTexture ()->isReady ()), " ownTextureID=",
		    (this->getTexture () != nullptr ? this->getTexture ()->getTextureID (0) : 0), " color4=(",
		    this->getColor4 ().r, ",", this->getColor4 ().g, ",", this->getColor4 ().b, ",", this->getColor4 ().a,
		    ") alpha=", this->getUserAlpha (), " brightness=", this->getBrightness (), " cullEnabled=",
		    (int) cullEnabled, " cullFaceMode=", cullFaceMode, " frontFace=", frontFace, " depthEnabled=",
		    (int) depthEnabled, " scissorEnabled=", (int) scissorEnabled, " blendEnabled=", (int) blendEnabled,
		    " colorMask=(", (int) colorMask[0], ",", (int) colorMask[1], ",", (int) colorMask[2], ",",
		    (int) colorMask[3], ")"
		);
	    }
	},
	[pass] () {
	    const GLint position = glGetAttribLocation (pass->getProgramID (), "a_Position");
	    const GLint texCoord = glGetAttribLocation (pass->getProgramID (), "a_TexCoord");

	    if (position >= 0) {
		glDisableVertexAttribArray (position);
	    }

	    if (texCoord >= 0) {
		glDisableVertexAttribArray (texCoord);
	    }
	}
    );
}

void CImage::addEffectPasses (const ImageEffect& effect) {
    const auto fboProvider = std::make_shared<FBOProvider> (this);

    for (const auto& fbo : effect.effect->fbos) {
	fboProvider->create (
	    *fbo,
	    this->m_image.model->passthrough ? (this->m_texture->getFlags () | TextureFlags_ClampUVs)
					      : this->m_texture->getFlags (),
	    this->getSize ()
	);
    }

    auto curEffect = effect.effect->passes.begin ();
    auto endEffect = effect.effect->passes.end ();
    auto curOverride = effect.passOverrides.begin ();
    auto endOverride = effect.passOverrides.end ();

    for (; curEffect != endEffect; ++curEffect) {
	if (!(*curEffect)->material.has_value ()) {
	    if (!(*curEffect)->command.has_value ()) {
		sLog.error ("Pass without material and command not supported");
		continue;
	    }

	    if (!(*curEffect)->source.has_value ()) {
		sLog.error ("Pass without material and source not supported");
		continue;
	    }

	    if (!(*curEffect)->target.has_value ()) {
		sLog.error ("Pass without material and target not supported");
		continue;
	    }

	    if ((*curEffect)->command != Command_Copy) {
		sLog.error ("Only copy command is supported for pass without material");
		continue;
	    }

	    auto virtualPass
		= std::make_unique<MaterialPass> (MaterialPass { .blending = BlendingMode_Normal,
								 .cullmode = CullingMode_Disable,
								 .depthtest = DepthtestMode_Disabled,
								 .depthwrite = DepthwriteMode_Disabled,
								 .shader = "commands/copy",
								 .textures = { { 0, *(*curEffect)->source } },
								 .combos = {},
								 .constants = {} });

	    const auto& config = *this->m_virtualPassess.emplace_back (std::move (virtualPass));

	    this->m_passes.push_back (new CPass (
		*this, fboProvider, config, std::nullopt, std::nullopt, (*curEffect)->target.value ()
	    ));
	} else {
	    for (auto& pass : (*curEffect)->material.value ()->passes) {
		const auto override = curOverride != endOverride
		    ? **curOverride
		    : std::optional<std::reference_wrapper<const ImageEffectPassOverride>> (std::nullopt);
		const auto target = (*curEffect)->target.has_value ()
		    ? *(*curEffect)->target
		    : std::optional<std::reference_wrapper<std::string>> (std::nullopt);

		this->m_passes.push_back (
		    new CPass (*this, fboProvider, *pass, override, (*curEffect)->binds, target)
		);
	    }

	    if (curOverride != endOverride) {
		++curOverride;
	    }
	}
    }
}

void CImage::setup () {
    if (this->m_initialized) {
	return;
    }

    // passthrough without effects has nothing to draw, WE doesn't composite these either
    // (sub_140175830 only takes the offscreen path with effects or a non-normal blend mode)
    if (this->m_image.model->passthrough && this->m_image.effects.empty ()) {
	return;
    }

    const auto& debug = this->getScene ().getContext ().getApp ().getContext ().settings.render.debug;

    for (const auto& cur : this->getImage ().model->material->passes) {
	this->m_passes.push_back (
	    new CPass (*this, std::make_shared<FBOProvider> (this), *cur, std::nullopt, std::nullopt, std::nullopt)
	);
    }

    std::vector<const DynamicValue*> passVisibility (this->m_passes.size (), nullptr);
    std::vector<bool> passFromEffect (this->m_passes.size (), false);

    if (!debug.baseOnly && !this->getImage ().effects.empty ()) {
	for (const auto& cur : this->m_image.effects) {
	    if (std::find (debug.skipEffects.begin (), debug.skipEffects.end (), static_cast<int> (cur->id))
		!= debug.skipEffects.end ()) {
		continue;
	    }

	    const auto effectVisibility = this->getScene ().getContext ().getApp ().getContext ().resolveEffectVisibility (
		static_cast<int> (cur->id), cur->name
	    );

	    // an explicit --disable-effect/--enable-effect override wins over the scene's own visibility
	    if (effectVisibility.has_value () && !*effectVisibility) {
		continue;
	    }

	    // scripts can toggle hidden effects at runtime; puppets can't, their mesh pass layout
	    // depends on the pass count
	    const bool followsVisibility = !effectVisibility.has_value () && !this->m_hasPuppetMesh;

	    if (!followsVisibility && !effectVisibility.has_value () && !cur->visible->value->getBool ()) {
		continue;
	    }

	    const DynamicValue* visibleValue = followsVisibility ? cur->visible->value.get () : nullptr;
	    const size_t firstEffectPass = this->m_passes.size ();

	    try {
		this->addEffectPasses (*cur);
	    } catch (const std::exception& e) {
		if (visibleValue == nullptr || visibleValue->getBool ()) {
		    throw;
		}

		for (size_t i = firstEffectPass; i < this->m_passes.size (); i++) {
		    delete this->m_passes[i];
		}

		this->m_passes.resize (firstEffectPass);
		sLog.error (
		    "Dropping hidden effect ", cur->id, " (", cur->name, ") on ", this->getImage ().name, ": ",
		    e.what ()
		);
		continue;
	    }

	    passVisibility.resize (this->m_passes.size (), visibleValue);
	    passFromEffect.resize (this->m_passes.size (), true);
	}
    }

    const size_t passCountBeforeTrailingPasses = this->m_passes.size ();

    if (!debug.baseOnly) {
	const auto magentaCompositeTint = findMagentaCompositeTint (this->m_image, debug.skipEffects);
	if (magentaCompositeTint.has_value ()) {
	    auto tintOverride = std::make_unique<ImageEffectPassOverride> (ImageEffectPassOverride {
		.id = -1,
		.combos = {
		    { "BLENDMODE", 30 },
		},
		.constants = {},
		.textures = {},
	    });
	    tintOverride->constants.emplace ("color", UserSettingBuilder::fromValue (magentaCompositeTint.value ()));
	    tintOverride->constants.emplace ("alpha", UserSettingBuilder::fromValue (1.0f));

	    this->m_materials.compatibilityMaterials.emplace_back (
		MaterialParser::load (this->getScene ().getScene ().project, "materials/effects/tint.json")
	    );
	    this->m_materials.compatibilityOverrides.emplace_back (std::move (tintOverride));

	    this->m_passes.push_back (new CPass (
		*this, std::make_shared<FBOProvider> (this),
		**this->m_materials.compatibilityMaterials.back ()->passes.begin (),
		*this->m_materials.compatibilityOverrides.back (), std::nullopt, std::nullopt
	    ));
	}
    }

    const int colorBlendMode = this->m_image.colorBlendMode->value->getInt ();
    const bool readByOtherLayer = std::ranges::any_of (this->getScene ().getScene ().objects, [this] (const auto& object) {
	return object->id != this->getImage ().id
	    && std::ranges::find (object->dependencies, this->getImage ().id) != object->dependencies.end ();
    });
    // WE keeps the result of a layer another one reads in _a and only copies it to the screen from there,
    // drawing the last effect pass straight to the screen would leave _a one pass behind (or empty)
    const bool copyForReaders = readByOtherLayer && this->getImage ().visible->value->getBool ();

    // fog sends every layer through its buffer and a FOG_COMPUTED composite (sub_1401E6F50, sub_1401EBBC0)
    const bool fog = this->getScene ().hasDistanceFog () || this->getScene ().hasHeightFog ();
    this->m_fogPass = nullptr;

    if (!debug.baseOnly && (colorBlendMode > 0 || copyForReaders || fog)) {
	// scenes from version 3 on composite with genericimage4, the one with fog (sub_1401EBBC0)
	const auto& project = this->getScene ().getScene ().project;
	this->m_materials.colorBlending.material = MaterialParser::load (
	    project,
	    project.sceneVersion >= 3 ? "materials/util/effectpassthrough_4.json" : "materials/util/effectpassthrough.json"
	);
	ComboMap combos;

	if (colorBlendMode > 0) {
	    combos.emplace ("BLENDMODE", colorBlendMode);
	}
	if (fog) {
	    combos.emplace ("FOG_COMPUTED", 1);
	}

	this->m_materials.colorBlending.override = std::make_unique<ImageEffectPassOverride> (ImageEffectPassOverride {
	    .id = -1,
	    .combos = combos,
	    .constants = {},
	    .textures = {},
	});

	this->m_passes.push_back (new CPass (
	    *this, std::make_shared<FBOProvider> (this), **this->m_materials.colorBlending.material->passes.begin (),
	    *this->m_materials.colorBlending.override, std::nullopt, std::nullopt
	));

	if (fog) {
	    this->m_fogPass = this->m_passes.back ();
	}
    }

    if (this->m_hasPuppetMesh && !this->m_passes.empty ()) {
	this->m_puppetMeshPass = this->m_passes.front ();
	this->m_puppetMeshLast = this->m_passes.size () == 1;

	// effect masks are laid out over the source texture, so effects run on the flat texture first
	// and the warped mesh is drawn last, sampling their output
	const auto& materialPasses = this->getImage ().model->material->passes;
	const bool hasTrailingPasses = this->m_passes.size () != passCountBeforeTrailingPasses;

	if (this->m_passes.size () > 1 && !hasTrailingPasses && materialPasses.size () == 1
	    && materialPasses.front ()->constants.empty ()) {
	    const auto& base = *materialPasses.front ();
	    const auto& config = *this->m_virtualPassess.emplace_back (std::make_unique<MaterialPass> (MaterialPass {
		.blending = base.blending,
		.cullmode = base.cullmode,
		.depthtest = base.depthtest,
		.depthwrite = base.depthwrite,
		.shader = base.shader,
		.textures = {},
		.usertextures = {},
		.combos = base.combos,
		.constants = {},
	    }));

	    this->m_puppetMeshPass
		= new CPass (*this, std::make_shared<FBOProvider> (this), config, std::nullopt, std::nullopt, std::nullopt);
	    this->m_passes.push_back (this->m_puppetMeshPass);
	    this->m_puppetMeshLast = true;
	}
    }

    passVisibility.resize (this->m_passes.size (), nullptr);
    passFromEffect.resize (this->m_passes.size (), false);

    for (size_t i = 0; i < this->m_passes.size (); i++) {
	this->m_allPassStates.push_back (
	    { passVisibility[i], this->m_passes[i]->getBlendingMode (), passFromEffect[i] }
	);
    }

    this->m_allPasses = this->m_passes;

    CRenderable::setup ();

    this->rebuildActivePasses ();
    this->m_initialized = true;
}

bool CImage::effectVisibilityChanged () const {
    for (size_t i = 0; i < this->m_allPassStates.size (); i++) {
	const auto* visible = this->m_allPassStates[i].visible;

	if (visible != nullptr && visible->getBool () != this->m_activePassMask[i]) {
	    return true;
	}
    }

    return false;
}

void CImage::rebuildActivePasses () {
    this->m_passes.clear ();
    this->m_activePassMask.assign (this->m_allPasses.size (), false);
    this->m_hasActiveEffectPass = false;

    for (size_t i = 0; i < this->m_allPasses.size (); i++) {
	const auto& state = this->m_allPassStates[i];

	this->m_allPasses[i]->setBlendingMode (state.blending);

	if (state.visible == nullptr || state.visible->getBool ()) {
	    this->m_activePassMask[i] = true;
	    this->m_hasActiveEffectPass |= state.fromEffect;
	    this->m_passes.push_back (this->m_allPasses[i]);
	}
    }

    // if there's more than one pass the blendmode has to be moved from the beginning to the end
    if (this->m_passes.size () > 1) {
	const auto first = this->m_passes.begin ();
	const auto last = this->m_passes.rbegin ();

	(*last)->setBlendingMode ((*first)->getBlendingMode ());
	(*first)->setBlendingMode (BlendingMode_Normal);
    }

    // setupPasses() ping-pongs these, every rebuild has to start from the same pair
    this->m_currentMainFBO = this->m_mainFBO;
    this->m_currentSubFBO = this->m_subFBO;

    this->setupPasses ();
}

void CImage::setupPasses () {
    // like WE, start on whichever buffer makes the last offscreen pass land in _a, which is what other layers read
    auto offscreenPasses = std::ranges::count_if (this->m_passes, [] (const Effects::CPass* pass) {
	return !pass->getTarget ().has_value ();
    });

    this->m_passesDrawToScreen = this->shouldRenderFinalPass (true);

    if (!this->m_passes.empty () && !this->m_passes.back ()->getTarget ().has_value () && this->m_passesDrawToScreen) {
	offscreenPasses--;
    }

    if (offscreenPasses % 2 == 0) {
	std::swap (this->m_currentMainFBO, this->m_currentSubFBO);
    }

    std::shared_ptr<const CFBO> drawTo = this->m_currentMainFBO;
    std::shared_ptr<const TextureProvider> asInput = this->getTexture ();
    GLuint texcoord = this->getTexCoordCopy ();

    auto cur = this->m_passes.begin ();
    auto end = this->m_passes.end ();
    bool first = true;
    bool inTargetEffectSequence = false;
    std::shared_ptr<const TextureProvider> effectInput = nullptr;

    for (; cur != end; ++cur) {
	Effects::CPass* pass = *cur;
	std::shared_ptr<const CFBO> prevDrawTo = drawTo;
	bool writesToTarget = false;
	const bool isFirstPass = first;
	const bool isMeshPass = this->m_hasPuppetMesh && pass == this->m_puppetMeshPass;
	GLuint spacePosition = isMeshPass ? this->m_puppetSpacePosition
	    : isFirstPass                 ? this->getCopySpacePosition ()
					  : this->getPassSpacePosition ();
	const glm::mat4* projection
	    = (isFirstPass) ? &this->m_modelViewProjectionCopy : &this->m_modelViewProjectionPass;
	const glm::mat4* inverseProjection
	    = (isFirstPass) ? &this->m_modelViewProjectionCopyInverse : &this->m_modelViewProjectionPassInverse;
	first = false;

	if (isMeshPass) {
	    pass->setBlendingMode (BlendingMode_Translucent);
	    this->setupPuppetGeometryCallback (pass);
	}

	pass->setModelMatrix (&this->m_modelMatrix);
	pass->setViewProjectionMatrix (&this->m_viewProjectionMatrix);
	pass->setEffectTextureProjectionMatrix (&this->m_effectTextureProjection, &this->m_effectTextureProjectionInverse);

	writesToTarget = this->configurePassTarget (pass, drawTo, asInput, effectInput, inTargetEffectSequence);
	// TODO: PROPERLY CHECK IF THIS IS ALL THAT'S NEEDED
	if (!writesToTarget && this->shouldRenderFinalPass (std::next (cur) == end)) {
	    drawTo = this->getScene ().getFBO ();

	    // A puppet with no effects has its geometry pass be both the first AND the last pass, drawn
	    // straight into the shared scene FBO below - same as any other object's final pass, so it
	    // needs the same screen-space projection. updatePuppetPositionBuffer() bakes this object's
	    // resolved scene position/scale directly into m_puppetSpacePosition (mirroring what
	    // uploadGeometryBuffers does for a normal quad's sceneSpacePosition), so m_modelViewProjectionScreen
	    // is the correct projection for those vertices now, not the local-canvas m_modelViewProjectionCopy
	    // this pass otherwise uses when rendering to an intermediate, object-sized target. The
	    // spacePosition reassignment below is a no-op for puppets either way - the puppet geometry
	    // callback always binds m_puppetSpacePosition itself, ignoring whatever spacePosition holds.
	    spacePosition = this->getSceneSpacePosition ();
	    projection = &this->m_modelViewProjectionScreen;
	    // WE's final pass inverse lands in the layer's local space (origin at its center, unscaled
	    // pixels); older shaders like the bundled xray.vert unproject the pointer through it
	    inverseProjection = &this->m_objectSpaceProjectionInverse;
	}

	pass->setLightingTransform (
	    projection == &this->m_modelViewProjectionCopy ? &this->m_lightingCopyModel : &this->m_lightingSceneModel,
	    &this->m_lightingNormal, &this->m_lightingViewProjection
	);

	// the fog composite measures in WE's world, the lighting model already maps the layer there
	if (pass == this->m_fogPass && projection == &this->m_modelViewProjectionScreen) {
	    pass->setModelMatrix (&this->m_lightingSceneModel);
	    pass->setFogWorld (true);
	}

	pass->setDestination (drawTo);
	pass->setInput (asInput);
	pass->setPreviousInput (inTargetEffectSequence ? effectInput : nullptr);
	pass->setPosition (spacePosition);
	pass->setTexCoord (texcoord);
	pass->setModelViewProjectionMatrix (projection);
	pass->setModelViewProjectionMatrixInverse (inverseProjection);

	texcoord = this->getTexCoordPass ();

	if (writesToTarget) {
	    asInput = drawTo;
	    drawTo = prevDrawTo;
	} else {
	    drawTo = prevDrawTo;
	    this->pinpongFramebuffer (&drawTo, &asInput);
	    inTargetEffectSequence = false;
	    effectInput = nullptr;
	}
    }
}

bool CImage::shouldRenderFinalPass (bool isLastPass) const {
    const auto& appContext = this->getScene ().getContext ().getApp ().getContext ();
    const auto visibility = appContext.resolveObjectVisibility (this->getId (), this->getObject ().name);
    const bool visible = visibility.value_or (this->getImage ().visible->value->getBool ());

    if (!isLastPass || !visible) {
	return false;
    }

    const auto& debug = this->getScene ().getContext ().getApp ().getContext ().settings.render.debug;
    return !(debug.noSolidFinal && this->getImage ().model->solidlayer);
}

bool CImage::configurePassTarget (
    Effects::CPass* pass, std::shared_ptr<const CFBO>& drawTo, const std::shared_ptr<const TextureProvider>& asInput,
    std::shared_ptr<const TextureProvider>& effectInput, bool& inTargetEffectSequence
) {
    if (!pass->getTarget ().has_value ()) {
	return false;
    }

    const std::string target = pass->getTarget ().value ();
    std::shared_ptr<const CFBO> resolved = pass->getFBOProvider ()->find (target);
    if (resolved == nullptr) {
	resolved = this->getScene ().findFBO (target);
    }
    if (resolved == nullptr) {
	sLog.error (
	    "Pass target FBO '", target, "' could not be resolved for object ", pass->getRenderable ().getId (),
	    " shader=", pass->getPass ().shader
	);
	return false;
    }

    if (!inTargetEffectSequence) {
	effectInput = asInput;
	inTargetEffectSequence = true;
    }
    drawTo = resolved;
    return true;
}

void CImage::pinpongFramebuffer (std::shared_ptr<const CFBO>* drawTo, std::shared_ptr<const TextureProvider>* asInput) {
    std::shared_ptr<const CFBO> currentMainFBO = this->m_currentMainFBO;
    std::shared_ptr<const CFBO> currentSubFBO = this->m_currentSubFBO;

    if (drawTo != nullptr) {
	*drawTo = currentSubFBO;
    }
    if (asInput != nullptr) {
	*asInput = currentMainFBO;
    }

    this->m_currentMainFBO = currentSubFBO;
    this->m_currentSubFBO = currentMainFBO;
}

void CImage::render () {
    if (!this->m_initialized) {
	return;
    }

    const auto& appContext = this->getScene ().getContext ().getApp ().getContext ();
    const auto visibility = appContext.resolveObjectVisibility (this->getId (), this->getObject ().name);
    // a hidden layer another object reads through _rt_imageLayerComposite_<id> (xray's "bloody" twins) still
    // has to fill that FBO every frame, shouldRenderFinalPass() keeps it off the screen
    if (!visibility.value_or (this->getImage ().visible->value->getBool ())
	&& (visibility.has_value () || !this->m_isDependency)) {
	return;
    }

    // the last pass only goes to the screen if the layer was visible when the passes were set up,
    // layers a script shows later (hidden in scene.json) need that redone
    if (this->effectVisibilityChanged () || this->shouldRenderFinalPass (true) != this->m_passesDrawToScreen) {
	this->rebuildActivePasses ();
    }

    if (this->m_image.model->passthrough && !this->m_hasActiveEffectPass) {
	return;
    }

    glColorMask (true, true, true, true);

    this->updateScreenSpacePosition ();

    if (this->m_hasPuppetMesh) {
	this->updatePuppetSkinning ();
    }

#if !NDEBUG
    std::string str = "Image ";

    if (this->getScene ().getScene ().camera.bloom.enabled->value->getBool () && this->getId () == -1) {
	str += "bloom";
    } else {
	str += this->getImage ().name + " (" + std::to_string (this->getId ()) + ", "
	    + this->getImage ().model->material->filename + ")";
    }

    glPushDebugGroup (GL_DEBUG_SOURCE_APPLICATION, 0, -1, str.c_str ());
#endif /* DEBUG */

    // WE (sub_140207B50): a passthrough layer without copybackground clears its buffer to 0,0,0,0 instead of
    // drawing the scene behind it, so its effects start from a transparent image
    const Effects::CPass* skippedBasePass = this->m_image.model->passthrough
	    && !this->m_image.copyBackground->value->getBool () && !this->m_allPasses.empty ()
	? this->m_allPasses.front ()
	: nullptr;

    auto cur = this->m_passes.begin ();
    const auto end = this->m_passes.end ();

    for (; cur != end; ++cur) {
	if (std::next (cur) == end) {
	    glColorMask (true, true, true, false);
	}

	if (*cur == skippedBasePass && std::next (cur) != end) {
	    (*cur)->clearDestination ();
	    continue;
	}

	(*cur)->render ();

    }

    // restore alpha writes - CParticle::render() never resets glColorMask, so leaving this
    // disabled here leaks into the next frame's clear if bloom renders last
    glColorMask (true, true, true, true);

#if !NDEBUG
    glPopDebugGroup ();
#endif /* DEBUG */
}

const float& CImage::getBrightness () const { return this->m_image.brightness->value->getFloat (); }

const float& CImage::getUserAlpha () const { return this->getAlpha (); }

const float& CImage::getAlpha () const {
    // some scenes store out-of-range alpha (e.g. 222) - it feeds mix() in blend modes, so it must stay in 0..1
    m_alphaCache = glm::clamp (this->m_image.alpha->value->getFloat (), 0.0f, 1.0f);
    return m_alphaCache;
}

const glm::vec3& CImage::getColor () const {
    // the object's brightness only scales its color in HDR scene rendering (sub_140207740)
    m_colorCache = this->m_image.color->value->getVec3 ()
	* (this->getScene ().isHDR () ? this->m_image.brightness->value->getFloat () : 1.0f);
    return m_colorCache;
}

const glm::vec4& CImage::getColor4 () const {
    // "version" 2 materials take color and alpha together through g_Color4
    m_color4Cache = glm::vec4 (this->getColor (), this->getAlpha ());
    return m_color4Cache;
}

const glm::vec3& CImage::getCompositeColor () const { return this->m_image.color->value->getVec3 (); }

glm::vec2 CImage::resolveGeometrySize (float sceneWidth, float sceneHeight, glm::vec3& origin) const {
    glm::vec2 size = this->getSize ();

    if ((size.x == 0.0f || size.y == 0.0f) && this->m_texture != nullptr) {
	size.x = static_cast<float> (this->m_texture->getRealWidth ());
	size.y = static_cast<float> (this->m_texture->getRealHeight ());
    } else if (
	(size.x == 0.0f || size.y == 0.0f) && this->getImage ().model->width.has_value ()
	&& this->getImage ().model->height.has_value ()
    ) {
	size.x = static_cast<float> (this->getImage ().model->width.value ());
	size.y = static_cast<float> (this->getImage ().model->height.value ());
    }

    if (this->getImage ().model->fullscreen) {
	size = { static_cast<float> (this->getScene ().getCanvasWidth ()),
		 static_cast<float> (this->getScene ().getCanvasHeight ()) };
	origin = { sceneWidth / 2.0f, sceneHeight / 2.0f, 0.0f };
    }

    return size;
}

void CImage::updateScenePosition (
    const glm::vec3& origin, const glm::vec2& size, const glm::vec3& scale, float sceneWidth, float sceneHeight
) {
    const glm::vec2 scaledSize = size * glm::vec2 (scale);
    this->m_pos.x = origin.x - (scaledSize.x / 2.0f);
    this->m_pos.w = origin.y + (scaledSize.y / 2.0f);
    this->m_pos.z = origin.x + (scaledSize.x / 2.0f);
    this->m_pos.y = origin.y - (scaledSize.y / 2.0f);

    const uint32_t alignment = this->getImage ().alignment;

    if (alignment & ImageAlignment_Top) {
	this->m_pos.y -= scaledSize.y / 2.0f;
	this->m_pos.w -= scaledSize.y / 2.0f;
    } else if (alignment & ImageAlignment_Bottom) {
	this->m_pos.y += scaledSize.y / 2.0f;
	this->m_pos.w += scaledSize.y / 2.0f;
    }

    if (alignment & ImageAlignment_Left) {
	this->m_pos.x += scaledSize.x / 2.0f;
	this->m_pos.z += scaledSize.x / 2.0f;
    } else if (alignment & ImageAlignment_Right) {
	this->m_pos.x -= scaledSize.x / 2.0f;
	this->m_pos.z -= scaledSize.x / 2.0f;
    }

    this->m_pos.x -= sceneWidth / 2.0f;
    this->m_pos.y = sceneHeight / 2.0f - this->m_pos.y;
    this->m_pos.z -= sceneWidth / 2.0f;
    this->m_pos.w = sceneHeight / 2.0f - this->m_pos.w;
}

void CImage::uploadGeometryBuffers (const glm::vec2& size) {
    GLfloat sceneSpacePosition[] = { this->m_pos.x, this->m_pos.y, 0.0f, this->m_pos.x, this->m_pos.w, 0.0f,
				     this->m_pos.z, this->m_pos.y, 0.0f, this->m_pos.z, this->m_pos.y, 0.0f,
				     this->m_pos.x, this->m_pos.w, 0.0f, this->m_pos.z, this->m_pos.w, 0.0f };

    float width = 1.0f;
    float height = 1.0f;
    if (this->getTexture () != nullptr && !this->getTexture ()->isAnimated ()
	&& (this->getTexture ()->getTextureWidth (0) != this->getTexture ()->getRealWidth ()
	    || this->getTexture ()->getTextureHeight (0) != this->getTexture ()->getRealHeight ())) {
	width = static_cast<float> (this->getTexture ()->getRealWidth ())
	    / static_cast<float> (this->getTexture ()->getTextureWidth (0));
	height = static_cast<float> (this->getTexture ()->getRealHeight ())
	    / static_cast<float> (this->getTexture ()->getTextureHeight (0));
    }

    float x = 0.0f;
    float y = 0.0f;
    GLfloat realWidth = size.x;
    GLfloat realHeight = size.y;
    GLfloat realX = 0.0f;
    GLfloat realY = 0.0f;

    if (this->getImage ().model->passthrough) {
	width = 1.0f;
	height = 1.0f;
	realX = this->m_pos.x;
	realY = this->m_pos.w;
	realWidth = this->m_pos.z;
	realHeight = this->m_pos.y;

	if (this->getImage ().model->fullscreen) {
	    realX = -1.0f;
	    realY = -1.0f;
	    realWidth = 1.0f;
	    realHeight = 1.0f;
	}
    }

    GLfloat texcoordCopy[] = { x, height, x, y, width, height, width, height, x, y, width, y };
    GLfloat copySpacePosition[] = { realX,     realHeight, 0.0f, realX, realY, 0.0f, realWidth, realHeight, 0.0f,
				    realWidth, realHeight, 0.0f, realX, realY, 0.0f, realWidth, realY,      0.0f };

    glBindBuffer (GL_ARRAY_BUFFER, this->m_sceneSpacePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (sceneSpacePosition), sceneSpacePosition, GL_DYNAMIC_DRAW);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_copySpacePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (copySpacePosition), copySpacePosition, GL_DYNAMIC_DRAW);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_texcoordCopy);
    glBufferData (GL_ARRAY_BUFFER, sizeof (texcoordCopy), texcoordCopy, GL_DYNAMIC_DRAW);

    this->m_sceneCenter
	= glm::vec3 ((this->m_pos.x + this->m_pos.z) / 2.0f, (this->m_pos.y + this->m_pos.w) / 2.0f, 0.0f);
    this->m_modelViewProjectionCopy = this->getImage ().model->passthrough
	? this->m_modelViewProjectionScreen
	: glm::ortho<float> (0.0, size.x, 0.0, size.y);
    this->m_modelViewProjectionCopyInverse = glm::inverse (this->m_modelViewProjectionCopy);
    this->m_modelMatrix = glm::ortho<float> (0.0, size.x, 0.0, size.y);
}

CImage::ResolvedTransform CImage::updateGeometryBuffers () {
    auto sceneWidth = static_cast<float> (this->getScene ().getWidth ());
    auto sceneHeight = static_cast<float> (this->getScene ().getHeight ());
    const auto transform = this->resolveTransform (this->getImage ());
    glm::vec3 origin = transform.origin;
    const glm::vec3 scale = transform.scale;
    const glm::vec2 size = this->resolveGeometrySize (sceneWidth, sceneHeight, origin);
    this->m_size = size;
    this->m_puppetScale = scale;

    // must run before the puppet position buffer rebake below - it needs this frame's m_pos, not the
    // previous one, to place puppet vertices at this object's actual scene position instead of its
    // position from before whatever moved it (parallax, a script, an attachment point it follows, ...)
    this->updateScenePosition (origin, size, scale, sceneWidth, sceneHeight);

    if (this->m_pos != this->m_lastUploadedPos || size != this->m_lastUploadedGeometrySize) {
	this->uploadGeometryBuffers (size);
	// puppet vertices bake m_pos/scale in directly (see updatePuppetPositionBuffer), so they need
	// the same "position or size changed" rebake trigger as the quad buffers above - a puppet whose
	// animation is disabled (or one with no MDLA data at all, i.e. always static) would otherwise
	// never get repositioned after its very first, load-time bake
	if (this->m_hasPuppetMesh) {
	    this->updatePuppetPositionBuffer (size);
	}
	this->m_lastUploadedPos = this->m_pos;
	this->m_lastUploadedGeometrySize = size;
    }

    return transform;
}

namespace {
// keeps an edge pair (e.g. m_pos.x/.z) from sliding past the viewport once `offset` is added to both,
// so the image never uncovers ground it doesn't have pixels for; an image too small to cover the viewport
// on this axis has no ground to uncover, it is an object sitting on the scene and moves freely
float clampParallaxAxis (float offset, float edgeA, float edgeB, float sceneExtent) {
    const float low = std::min (edgeA, edgeB);
    const float high = std::max (edgeA, edgeB);
    const float half = sceneExtent / 2.0f;
    const float maxOffset = -half - low;
    const float minOffset = half - high;

    if (minOffset > maxOffset)
	return offset;

    return std::clamp (offset, minOffset, maxOffset);
}
} // namespace

void CImage::updateScreenSpacePosition () {
    const ResolvedTransform transform = this->updateGeometryBuffers ();

    // angles are already in radians from scene.json. WE's Rz * Ry * Rx seen through the y flip of this space is
    // Rz (-z) * Ry (y) * Rx (-x) (see CParticle.cpp). Only the object's own x/y angles, the parent chain folds z only
    const float angle = transform.angle;
    const glm::vec3 ownAngles = this->getImage ().angles->value->getVec3 ();
    // origin z only shows through a perspective camera, the ortho one keeps -2000..2000 like WE
    glm::mat4 rotModel = glm::translate (glm::mat4 (1.0f), glm::vec3 (0.0f, 0.0f, transform.origin.z));
    if (angle != 0.0f || ownAngles.x != 0.0f || ownAngles.y != 0.0f) {
	rotModel = glm::translate (rotModel, this->m_sceneCenter);
	rotModel = glm::rotate (rotModel, -angle, glm::vec3 (0.0f, 0.0f, 1.0f));
	rotModel = glm::rotate (rotModel, ownAngles.y, glm::vec3 (0.0f, 1.0f, 0.0f));
	rotModel = glm::rotate (rotModel, -ownAngles.x, glm::vec3 (1.0f, 0.0f, 0.0f));
	rotModel = glm::translate (rotModel, -this->m_sceneCenter);
    }

    if (transform.meshPivotAngle != 0.0f && this->m_hasPuppetMesh) {
	const auto& source = !this->m_puppetSkinnedPositions.empty () ? this->m_puppetSkinnedPositions : this->m_puppetRawPositions;
	glm::vec2 boundsMin (std::numeric_limits<float>::max ());
	glm::vec2 boundsMax (std::numeric_limits<float>::lowest ());
	for (size_t i = 0; i + 2 < source.size (); i += 3) {
	    boundsMin = glm::min (boundsMin, glm::vec2 (source[i], source[i + 1]));
	    boundsMax = glm::max (boundsMax, glm::vec2 (source[i], source[i + 1]));
	}

	if (boundsMin.x <= boundsMax.x) {
	    const glm::vec2 meshCenter = (boundsMin + boundsMax) / 2.0f;
	    const glm::vec4 pivot (
		this->m_pos.x + (this->m_size.x / 2.0f + meshCenter.x) * this->m_puppetScale.x,
		this->m_pos.w + (this->m_size.y / 2.0f - meshCenter.y) * this->m_puppetScale.y, 0.0f, 1.0f
	    );
	    const glm::vec3 rotatedPivot = glm::vec3 (rotModel * pivot);
	    glm::mat4 pivotRot = glm::translate (glm::mat4 (1.0f), rotatedPivot);
	    pivotRot = glm::rotate (pivotRot, -transform.meshPivotAngle, glm::vec3 (0.0f, 0.0f, 1.0f));
	    pivotRot = glm::translate (pivotRot, -rotatedPivot);
	    rotModel = pivotRot * rotModel;
	}
    }

    glm::mat4 mvp = this->getViewProjection () * rotModel;

    // CScene::renderFrame() already folds disableparallax into getParallaxDisplacement()
    if (this->getScene ().getScene ().camera.parallax.enabled->value->getBool ()) {
	const glm::vec2 offset = this->getScene ().getParallaxOffset (this->getImage ());
	float x = offset.x;
	float y = offset.y;

	// a texture that isn't UV-clamped tiles/repeats instead of showing black past its edges (GL_REPEAT,
	// see CTexture.cpp), so sliding it further is harmless and exempt from the clamp; scene.json's own
	// "clampuvs" overrides the base texture's flag the same way it does for the composite FBOs above
	const bool textureTiles = !this->getImage ().clampUVs && this->getTexture () != nullptr
	    && (this->getTexture ()->getFlags () & TextureFlags_ClampUVs) == 0;

	if (this->getScene ().getContext ().getApp ().getContext ().settings.mouse.clampParallaxToImageSize
	    && !textureTiles) {
	    const float sceneWidth = static_cast<float> (this->getScene ().getCanvasWidth ());
	    const float sceneHeight = static_cast<float> (this->getScene ().getCanvasHeight ());
	    x = clampParallaxAxis (x, this->m_pos.x, this->m_pos.z, sceneWidth);
	    y = clampParallaxAxis (y, this->m_pos.y, this->m_pos.w, sceneHeight);
	}

	mvp = glm::translate (mvp, { x, y, 0.0f });
	rotModel = glm::translate (rotModel, { x, y, 0.0f });
    }

    this->updateLightingTransform (rotModel);

    // only the inverse is expensive; skip it when mvp didn't actually change
    if (mvp != this->m_modelViewProjectionScreen) {
	this->m_modelViewProjectionScreenInverse = glm::inverse (mvp);
    }
    this->m_modelViewProjectionScreen = mvp;
    this->updateEffectTextureProjection ();
    if (this->getImage ().model->passthrough) {
	this->m_modelViewProjectionCopy = this->m_modelViewProjectionScreen;
	this->m_modelViewProjectionCopyInverse = this->m_modelViewProjectionScreenInverse;
    }
}

glm::mat4 CImage::getViewProjection () const {
    const auto& camera = this->getScene ().getCamera ();

    // "perspective" layers get their own camera in 2D scenes (sub_1401E8AA0 -> sub_1401E5B60)
    if (camera.isOrthogonal () && this->getImage ().perspective->value->getBool ()) {
	return camera.getPerspectiveLayerViewProjection ();
    }

    return camera.getProjection () * camera.getLookAt ();
}

void CImage::updateLightingTransform (const glm::mat4& sceneTransform) {
    const auto width = static_cast<float> (this->getScene ().getWidth ());
    const auto height = static_cast<float> (this->getScene ().getHeight ());
    const glm::mat4 toWorld = glm::scale (
	glm::translate (glm::mat4 (1.0f), glm::vec3 (width / 2.0f, height / 2.0f, 0.0f)), glm::vec3 (1.0f, -1.0f, 1.0f)
    );
    const glm::mat3 flip = glm::mat3 (glm::scale (glm::mat4 (1.0f), glm::vec3 (1.0f, -1.0f, 1.0f)));

    this->m_lightingSceneModel = toWorld * sceneTransform;
    this->m_lightingNormal = flip * glm::mat3 (sceneTransform) * flip;
    this->m_lightingViewProjection = this->getViewProjection () * glm::inverse (toWorld);

    // the first pass draws into the layer's own buffer, (0, 0) there is the image's top left corner in the scene
    if (this->getImage ().model->passthrough) {
	this->m_lightingCopyModel = this->m_lightingSceneModel;
	return;
    }

    const glm::vec2 size = glm::max (this->getSize (), glm::vec2 (1.0f));
    const glm::mat4 copyToScene = glm::scale (
	glm::translate (glm::mat4 (1.0f), glm::vec3 (this->m_pos.x, this->m_pos.w, 0.0f)),
	glm::vec3 ((this->m_pos.z - this->m_pos.x) / size.x, (this->m_pos.y - this->m_pos.w) / size.y, 1.0f)
    );

    this->m_lightingCopyModel = this->m_lightingSceneModel * copyToScene;
}

void CImage::updateEffectTextureProjection () {
    // the final quad puts texcoord (0, 0) at (m_pos.x, m_pos.w), which is the layer's local (-1, +1) corner;
    // the scene FBO is y-flipped against the screen the pointer position is measured on, hence the flip
    const glm::vec3 center ((this->m_pos.x + this->m_pos.z) / 2.0f, (this->m_pos.y + this->m_pos.w) / 2.0f, 0.0f);
    const glm::vec3 halfSize ((this->m_pos.z - this->m_pos.x) / 2.0f, (this->m_pos.w - this->m_pos.y) / 2.0f, 1.0f);
    const glm::mat4 projection = glm::scale (glm::mat4 (1.0f), glm::vec3 (1.0f, -1.0f, 1.0f))
	* this->m_modelViewProjectionScreen * glm::scale (glm::translate (glm::mat4 (1.0f), center), halfSize);

    if (projection == this->m_effectTextureProjection) {
	return;
    }

    this->m_effectTextureProjection = projection;
    // a zero-sized layer has no inverse, keep the last usable one instead of feeding NaNs to the shader
    if (halfSize.x != 0.0f && halfSize.y != 0.0f) {
	this->m_effectTextureProjectionInverse = glm::inverse (projection);
    }

    const glm::vec2 size = this->getSize ();
    this->m_objectSpaceProjectionInverse
	= glm::scale (glm::mat4 (1.0f), glm::vec3 (size.x / 2.0f, size.y / 2.0f, 1.0f))
	* this->m_effectTextureProjectionInverse;
}

const Image& CImage::getImage () const { return this->m_image; }

void CImage::markAsDependency () { this->m_isDependency = true; }

glm::vec2 CImage::getSize () const {
    if (this->m_texture == nullptr) {
	return this->getImage ().size;
    }

    // compose layers sample the whole scene, but effect masks map over the layer's own size
    if (this->getImage ().model->passthrough && this->getImage ().size.x > 0.0f
	&& this->getImage ().size.y > 0.0f) {
	return this->getImage ().size;
    }

    // solid layers use a stock white texture, the real footprint is declared by the scene
    if (this->getImage ().model->solidlayer && this->getImage ().size.x > 0.0f && this->getImage ().size.y > 0.0f) {
	return this->getImage ().size;
    }

    return { this->m_texture->getRealWidth (), this->m_texture->getRealHeight () };
}

GLuint CImage::getSceneSpacePosition () const { return this->m_sceneSpacePosition; }

GLuint CImage::getCopySpacePosition () const { return this->m_copySpacePosition; }

GLuint CImage::getPassSpacePosition () const { return this->m_passSpacePosition; }

GLuint CImage::getTexCoordCopy () const { return this->m_texcoordCopy; }

GLuint CImage::getTexCoordPass () const { return this->m_texcoordPass; }
