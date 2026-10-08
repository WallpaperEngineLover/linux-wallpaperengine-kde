#include "CImage.h"
#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Data/Parsers/ObjectParser.h"

#include "CMesh.h"
#include "CRenderable.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <strings.h>
#include <tuple>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/rotate_vector.hpp>
#undef GLM_ENABLE_EXPERIMENTAL

#include "WallpaperEngine/Assets/AssetLoadException.h"
#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Data/Model/Material.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Data/Model/UserSetting.h"
#include "WallpaperEngine/Data/Parsers/MaterialParser.h"
#include "WallpaperEngine/Data/Utils/BinaryReader.h"
#include "WallpaperEngine/Data/Utils/MemoryStream.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Scripting/Adapters/ScriptableObjectAdapter.h"
#include "WallpaperEngine/Scripting/ScriptEngine.h"

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render::Objects;
using namespace WallpaperEngine::Render::Objects::Effects;
using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Data::Builders;
using namespace WallpaperEngine::Data::Utils;

extern float g_Time;
extern float g_TimeLast;

namespace {
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
std::vector<PuppetMeshBlock> findPuppetMeshBlockCandidates (
    const BinaryReader& reader, size_t markerSize, size_t mdlsOffset, size_t meshHeaderSize
) {
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
	    PuppetMeshBlock {
		.headerOffset = offset, .vertexBytes = candidateVertexBytes, .indexBytes = candidateIndexBytes }
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

std::optional<PuppetMeshData> readPuppetMeshData (
    const BinaryReader& reader, const PuppetMeshBlock& block, size_t meshHeaderSize, size_t vertexStride
) {
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
std::optional<PuppetVertexLayout>
resolvePuppetVertexLayout (const BinaryReader& reader, size_t markerSize, size_t mdlsOffset, size_t meshHeaderSize) {
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
	    best = PuppetVertexLayout { .block = block,
					.vertexStride = stride,
					.uvOffset = stride - sizeof (GLfloat) * 2 };
	}
    }

    return best;
}

// bounds-checked little endian reads for MDLV/MDMP (sub_140261880)
class PuppetFileReader {
public:
    PuppetFileReader (const std::vector<char>& data, size_t offset) : m_data (data), m_offset (offset) { }

    template <typename T> T next () {
	if (this->m_offset + sizeof (T) > this->m_data.size ()) {
	    throw std::runtime_error ("truncated puppet file");
	}

	T value;
	std::memcpy (&value, this->m_data.data () + this->m_offset, sizeof (T));
	this->m_offset += sizeof (T);
	return value;
    }

    void string () {
	while (this->next<char> () != '\0') { }
    }

    std::string text () {
	std::string result;

	for (char c = this->next<char> (); c != '\0'; c = this->next<char> ()) {
	    result.push_back (c);
	}

	return result;
    }

    /** u32 size + bytes */
    std::pair<size_t, uint32_t> blob () {
	const auto bytes = this->next<uint32_t> ();
	const size_t start = this->m_offset;

	if (start + bytes > this->m_data.size ()) {
	    throw std::runtime_error ("truncated puppet file");
	}

	this->m_offset += bytes;
	return { start, bytes };
    }

    void skip (size_t bytes) { this->m_offset += bytes; }

    [[nodiscard]] size_t offset () const { return this->m_offset; }

private:
    const std::vector<char>& m_data;
    size_t m_offset;
};

struct PuppetMeshHeader {
    uint32_t flags = 0;
    uint32_t format = 0;
    size_t vertices = 0;
    uint32_t vertexBytes = 0;
    uint32_t indexBytes = 0;
};

// first mesh header: tag, default format, materials per mesh, mesh count, then per mesh: material names, flags
// (+u32 with flag 2), bounds (v17+), format (v15+)
PuppetMeshHeader readPuppetMeshHeader (const std::vector<char>& data) {
    PuppetFileReader reader (data, 0);
    reader.string ();

    const int version = std::atoi (data.data () + strlen ("MDLV"));
    PuppetMeshHeader header;
    header.format = reader.next<uint32_t> ();
    const auto materials = reader.next<uint32_t> ();
    (void)reader.next<uint32_t> ();

    for (uint32_t material = 0; material < materials; material++) {
	reader.string ();
    }

    if (version >= 4) {
	header.flags = reader.next<uint32_t> ();

	if (header.flags & 2) {
	    (void)reader.next<uint32_t> ();
	}

	if (version >= 17) {
	    reader.skip (sizeof (float) * 6);
	}

	if (version >= 15) {
	    header.format = reader.next<uint32_t> ();
	}
    }

    header.vertexBytes = reader.next<uint32_t> ();
    header.vertices = reader.offset ();
    reader.skip (header.vertexBytes);
    header.indexBytes = reader.next<uint32_t> ();
    return header;
}

// float4x3 per bone
void packPuppetBones (const std::vector<glm::mat4>& skin, std::vector<GLfloat>& out) {
    out.assign (skin.size () * 12, 0.0f);

    for (size_t bone = 0; bone < skin.size (); bone++) {
	for (int column = 0; column < 4; column++) {
	    for (int row = 0; row < 3; row++) {
		out[bone * 12 + column * 3 + row] = skin[bone][column][row];
	    }
	}
    }
}

// sub_140261880 layout of the first mesh; texcoords not last in the vertex fall back to the heuristic
std::optional<PuppetVertexLayout> readPuppetVertexLayout (const std::vector<char>& data, size_t meshHeaderSize) {
    const auto header = readPuppetMeshHeader (data);
    const uint32_t stride = CMesh::vertexStride (header.format);
    const auto texcoord = CMesh::vertexComponentOffset (header.format, 0x8);

    if (stride == 0 || header.vertexBytes % stride != 0 || !texcoord.has_value ()
	|| *texcoord != stride - sizeof (GLfloat) * 2 || header.vertices + header.vertexBytes > data.size ()) {
	return std::nullopt;
    }

    return PuppetVertexLayout {
	.block = PuppetMeshBlock { .headerOffset = header.vertices - meshHeaderSize,
				   .vertexBytes = header.vertexBytes,
				   .indexBytes = header.indexBytes },
	.vertexStride = stride,
	.uvOffset = *texcoord,
    };
}

// first mesh with flag 2 (sub_1401FBAE0, puppet +912), BLENDROWCOUNT = the u32 after its flags (sub_140209540)
std::optional<PuppetBlendMesh> readPuppetBlendMesh (const std::vector<char>& data) {
    PuppetFileReader reader (data, 0);
    reader.string ();

    const int version = std::atoi (data.data () + strlen ("MDLV"));

    if (version < 4) {
	return std::nullopt;
    }

    const auto defaultFormat = reader.next<uint32_t> ();
    const auto materials = reader.next<uint32_t> ();
    const auto meshes = reader.next<uint32_t> ();

    for (uint32_t mesh = 0; mesh < meshes; mesh++) {
	PuppetBlendMesh result;

	for (uint32_t material = 0; material < materials; material++) {
	    const std::string name = reader.text ();

	    if (material == 0) {
		result.material = name;
	    }
	}

	const auto flags = reader.next<uint32_t> ();

	if (flags & 2) {
	    result.rows = reader.next<uint32_t> ();
	}

	if (version >= 17) {
	    reader.skip (sizeof (float) * 6);
	}

	result.format = version >= 15 ? reader.next<uint32_t> () : defaultFormat;
	std::tie (result.vertices, result.vertexBytes) = reader.blob ();
	std::tie (result.indices, result.indexBytes) = reader.blob ();

	if (flags & 2) {
	    return result;
	}

	if (version >= 21) {
	    if (reader.next<uint8_t> () != 0) {
		(void)reader.next<uint32_t> ();
		(void)reader.blob ();
	    }

	    if (reader.next<uint8_t> () != 0) {
		(void)reader.blob ();
	    }
	}

	if (version >= 23) {
	    const auto records = reader.next<uint32_t> ();

	    for (uint32_t record = 0; record < records; record++) {
		reader.skip (sizeof (uint64_t));
		reader.string ();
		(void)reader.next<uint32_t> ();
		reader.skip (sizeof (uint32_t) * reader.next<uint32_t> ());
		reader.skip (sizeof (uint32_t) * reader.next<uint32_t> ());
	    }
	}
    }

    return std::nullopt;
}

// MDMP per mesh (sub_140261880): u16 targets, scale, vertex count, then per target u64, name and 16 bit blobs:
// positions, normals (0x400), 3 more (0x800), alpha (0x1000), bone rule (0x2000, u32 u32 f32 f32)
std::optional<PuppetMorphTargets>
readPuppetMorphTargets (const std::vector<char>& data, size_t section, uint32_t flags) {
    PuppetFileReader reader (data, section);
    reader.string ();
    (void)reader.next<uint32_t> ();

    const auto targets = reader.next<uint16_t> ();

    if (targets == 0) {
	return std::nullopt;
    }

    PuppetMorphTargets morph;
    morph.scale = reader.next<float> ();
    morph.vertexCount = reader.next<uint32_t> ();

    std::vector<size_t> positions;
    std::vector<size_t> alphas;

    for (uint16_t target = 0; target < targets; target++) {
	(void)reader.next<uint64_t> ();
	morph.names.push_back (reader.text ());

	const auto [position, positionBytes] = reader.blob ();
	morph.vertexCount = std::min (morph.vertexCount, positionBytes / 6);

	if (positionBytes != morph.vertexCount * 6) {
	    throw std::runtime_error ("morph target positions don't match the vertex count");
	}

	positions.push_back (position);

	for (const uint32_t flag : { 0x400u, 0x800u }) {
	    if ((flags & flag) && reader.blob ().second != positionBytes) {
		throw std::runtime_error ("morph target normals don't match the vertex count");
	    }
	}

	if (flags & 0x1000) {
	    const auto [alpha, alphaBytes] = reader.blob ();

	    if (alphaBytes != morph.vertexCount * 2) {
		throw std::runtime_error ("morph target alphas don't match the vertex count");
	    }

	    alphas.push_back (alpha);
	}

	if (flags & 0x2000) {
	    PuppetMorphTargets::BoneRule rule;
	    rule.bone = reader.next<uint32_t> ();
	    rule.axis = (reader.next<uint32_t> () & 2) ? 1.0f : 0.0f;
	    rule.edge0 = reader.next<float> ();
	    rule.edge1 = reader.next<float> ();
	    morph.boneRules.push_back (rule);
	}
    }

    // square of vertexCount * targets + 1 texels, texel 0 is (0, 0, 0, 1)
    const uint32_t texels = morph.vertexCount * targets + 1;
    auto side = static_cast<uint32_t> (std::sqrt (static_cast<float> (texels)));

    if (side * side < texels) {
	side++;
    }

    const auto snorm = [&data] (size_t offset) {
	int16_t value;
	std::memcpy (&value, data.data () + offset, sizeof (value));
	return std::max (static_cast<float> (value) / 32767.0f, -1.0f);
    };

    morph.texels.assign (static_cast<size_t> (side) * side, glm::vec4 (0.0f));
    morph.texels[0] = glm::vec4 (0.0f, 0.0f, 0.0f, 1.0f);
    // without 0x1000 morphed vertices read alpha 0
    morph.alpha = (flags & 0x1000) == 0;
    size_t out = 1;

    for (size_t target = 0; target < alphas.size (); target++) {
	for (uint32_t vertex = 0; vertex < morph.vertexCount; vertex++) {
	    const size_t position = positions[target] + vertex * 6;
	    morph.texels[out] = glm::vec4 (
		snorm (position), snorm (position + 2), snorm (position + 4), snorm (alphas[target] + vertex * 2)
	    );
	    morph.alpha = morph.alpha || morph.texels[out].w != 1.0f;
	    out++;
	}
    }

    return morph;
}

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
    this->registerProperty ("alignment", *image.alignmentName->value);
    this->registerRenderableProperties (image.renderable, *image.colorBlendMode, *image.brightness);
    this->registerEffectConstants (image.effects);

    // animation layers carry their own property scripts, WE runs them with the layer as thisObject (the JSON loader
    // sub_1401730D0 binds them to the IAnimationLayer object, sub_14026C980 lists its properties and methods)
    for (size_t layerIndex = 0; layerIndex < image.animationLayers.size (); layerIndex++) {
	this->registerAnimationLayerProperties (layerIndex, *image.animationLayers[layerIndex]);
    }

    auto scene_width = static_cast<float> (scene.getWidth ());
    auto scene_height = static_cast<float> (scene.getHeight ());

    glm::vec2 size = this->getSize ();

    // a shape's quad is a square of the scene height (shape load sub_14025FAC0, renderer +136)
    if (this->getImage ().shape) {
	size = { scene_height, scene_height };
    }

    try {
	this->detectTexture ();
    } catch (const Assets::AssetLoadException& e) {
	// live WE loads the scene anyway and draws the layer empty
	sLog.error ("Image ", image.id, " has no texture, drawing it empty: ", e.what ());
    }

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
    } else if (
	this->getImage ().model->autosize && !placeholderTexture
	&& std::dynamic_pointer_cast<const CFBO> (this->m_texture) == nullptr
    ) {
	size.x = static_cast<float> (this->m_texture->getRealWidth ());
	size.y = static_cast<float> (this->m_texture->getRealHeight ());
    }

    // fullscreen layers should use the whole projection's size
    if (this->getImage ().model->fullscreen) {
	size = { static_cast<float> (scene.getCanvasWidth ()), static_cast<float> (scene.getCanvasHeight ()) };
    }
    this->m_size = size;

    // taken after the texture/model/fullscreen fallbacks above, unsized layers would otherwise get 0x0 buffers
    glm::vec2 bufferSize = size;

    if (placeholderTexture) {
	bufferSize = glm::min (bufferSize, glm::vec2 (scene.getCanvasWidth (), scene.getCanvasHeight ()));
    }

    this->updateScenePosition (size);

    // register both FBOs into the scene
    std::ostringstream nameA, nameB;

    // WE renders layers with LIGHTING or REFLECTION unlit into _rt_imageLayerAlbedo_<id> when they have offscreen
    // passes and lights them last (sub_1401914B0); here the first pass is lit instead, through PRELIGHTING
    nameA << "_rt_imageLayerComposite_" << this->getImage ().id << "_a";
    nameB << "_rt_imageLayerComposite_" << this->getImage ().id << "_b";

    // sub_1401E8AA0: buffer flags are the object's clampuvs/nointerpolation (else the texture's)
    const auto& renderable = this->getImage ().renderable;
    const bool noInterpolation = renderable.noInterpolation->value->getBool ()
	|| (this->m_texture->getFlags () & TextureFlags_NoInterpolation) != 0;
    const uint32_t compositeFlags = (renderable.clampUVs->value->getBool () ? TextureFlags_ClampUVs : 0)
	| (noInterpolation ? TextureFlags_NoInterpolation : 0);

    // layer buffers are 16 bit float in HDR scene rendering (sub_1401E7170)
    const TextureFormat layerFormat = this->getScene ().isHDR () ? TextureFormat_RGBA16161616f : TextureFormat_ARGB8888;
    const auto mainFBO = scene.create (
	nameA.str (), layerFormat, compositeFlags, 1, { bufferSize.x, bufferSize.y }, { bufferSize.x, bufferSize.y }
    );
    const auto subFBO = scene.create (
	nameB.str (), layerFormat, compositeFlags, 1, { bufferSize.x, bufferSize.y }, { bufferSize.x, bufferSize.y }
    );

    if (this->followsOutputSize ()) {
	scene.followOutputSize (mainFBO, 1);
	scene.followOutputSize (subFBO, 1);
    }

    this->m_currentMainFBO = this->m_mainFBO = this->m_namedMainFBO = mainFBO;
    this->m_currentSubFBO = this->m_subFBO = this->m_namedSubFBO = subFBO;
    this->m_layerBufferConfig = { .size = bufferSize, .format = layerFormat, .flags = compositeFlags };

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
    } else if (
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

    this->uploadSceneTexCoords ({ x, y, width, height });

    this->m_hasPuppetMesh = this->loadPuppetMesh (size);

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

void CImage::renderPassthroughChildren (const std::shared_ptr<const CFBO>& buffer) {
    // the buffer holds the layer's quad (m_pos, copy pass texcoords: v = 1 at m_pos.y), children land in it through
    // model = inverse (layer world), view = identity, ortho (-w/2, w/2, -h/2, h/2, -1000, 1000) (sub_1401ECB20).
    // Fullscreen layers keep the scene's camera
    glm::mat4 transform (1.0f);
    glm::mat4 viewProjection = this->getScene ().getWorldViewProjection ();

    if (!this->getImage ().model->fullscreen) {
	const glm::vec3 center ((this->m_pos.x + this->m_pos.z) / 2.0f, (this->m_pos.y + this->m_pos.w) / 2.0f, 0.0f);
	const glm::vec3 halfExtent (
	    (this->m_pos.z - this->m_pos.x) / 2.0f, (this->m_pos.y - this->m_pos.w) / 2.0f, 1000.0f
	);

	transform = glm::inverse (glm::scale (glm::translate (this->m_modelViewProjectionScreen, center), halfExtent));

	// lights skip the model stack (volumetricsfront.vert only has g_ViewProjectionMatrix), so they see the bare
	// ortho with the scene's y flip: their world position lands in the layer as if the layer sat at the origin
	const glm::vec2 size = this->getSize ();
	viewProjection = glm::scale (glm::mat4 (1.0f), glm::vec3 (1.0f, -1.0f, 1.0f))
	    * glm::ortho (-size.x / 2.0f, size.x / 2.0f, -size.y / 2.0f, size.y / 2.0f, -1000.0f, 1000.0f);
    }

    this->getScene ().renderPassthroughChildren (
	this->getId (),
	{
	    .fbo = buffer,
	    .transform = transform,
	    .alphaMax = !this->m_image.copyBackground->value->getBool (),
	    .viewProjection = viewProjection,
	}
    );
}

bool CImage::usesPooledBuffer () const {
    const int colorBlendMode = this->m_image.colorBlendMode->value->getInt ();

    // puppets only with effects, an animated texture, a blend map or a prelighting material (sub_14020AE00)
    const bool namedPuppet = this->m_hasPuppetMesh
	&& (!this->m_image.effects.empty () || (this->m_texture != nullptr && this->m_texture->isAnimated ())
	    || this->m_blendMap.pass != nullptr || this->m_puppetPrelight.pass != nullptr);

    // 0x800000 / 0x1000000 are distance / height fog (sub_140186440), dependencies and layerimage sources get 0x1010
    return (colorBlendMode == 0 || colorBlendMode == 31) && !this->m_image.renderable.ledSource->value->getBool ()
	&& !namedPuppet && !this->m_readByOtherLayer && !this->m_emitterImageSource
	&& !this->getScene ().hasDistanceFog () && !this->getScene ().hasHeightFog ();
}

int CImage::effectBufferPasses () const {
    int passes = 0;

    for (const auto& effect : this->m_image.effects) {
	const auto visibility = this->getScene ().getContext ().getApp ().getContext ().resolveEffectVisibility (
	    static_cast<int> (effect->id), effect->name
	);

	if (!visibility.value_or (effect->visible->value->getBool ())) {
	    continue;
	}

	passes += this->getImage ().shape ? 0 : 1;

	for (const auto& pass : effect->effect->passes) {
	    passes += pass->compose ? 1 : 0;
	}
    }

    return passes;
}

std::string CImage::layerBufferName (const int index) const {
    // flag 0x10 adds a buffer, a passthrough layer with children gets at least one, at most two
    const bool named = !this->usesPooledBuffer ();
    const int passes = this->effectBufferPasses () + (named ? 1 : 0);
    const int buffers = passes == 0 && this->m_hasPassthroughChildren ? 1 : std::min (passes, 2);

    if (index >= buffers) {
	return "";
    }

    // only the first buffer can be named, the second comes from the pool
    if (named && index == 0) {
	return "_rt_imageLayerComposite_" + std::to_string (this->getImage ().id) + "_a";
    }

    const auto& config = this->m_layerBufferConfig;
    const std::string name = "sb." + std::to_string (std::max (static_cast<int> (config.size.x), 4)) + "."
	+ std::to_string (std::max (static_cast<int> (config.size.y), 4)) + "."
	+ ((config.flags & TextureFlags_NoInterpolation) ? "n" : "b") + "."
	+ ((config.flags & TextureFlags_ClampUVs) ? "c" : "r") + "." + std::to_string (index);
    int matches = 0;

    // every passthrough ancestor whose last offscreen buffer has this name adds one. Without effects WE reads index -1,
    // never a match
    for (auto parent = this->getImage ().parent; parent.has_value ();) {
	const auto* object = this->getScene ().getObject (*parent);

	if (object == nullptr) {
	    break;
	}

	if (const auto* image = object->is<CImage> () ? object->as<CImage> () : nullptr;
	    image != nullptr && image->m_image.model->passthrough) {
	    const int last = image->effectBufferPasses () - (image->usesPooledBuffer () ? 1 : 0);

	    if (last >= 0 && image->layerBufferName (last % 2) == name) {
		matches++;
	    }
	}

	parent = object->getObject ().parent;
    }

    return matches > 0 ? name + std::to_string (matches) : name;
}

void CImage::assignLayerBuffers () {
    if (this->m_namedMainFBO == nullptr) {
	return;
    }

    auto& scene = this->getScene ();
    const auto& config = this->m_layerBufferConfig;
    const auto pooled = [&] (const int index, const std::shared_ptr<const CFBO>& named) -> std::shared_ptr<const CFBO> {
	const std::string name = this->layerBufferName (index);

	// named and _rt_ buffers stay the layer's own
	if (name.empty () || name.starts_with ("_rt_")) {
	    return named;
	}

	if (auto existing = scene.find (name); existing != nullptr) {
	    return existing;
	}

	auto fbo = scene.create (name, config.format, config.flags, 1, config.size, config.size);

	if (this->followsOutputSize ()) {
	    scene.followOutputSize (fbo, 1);
	}

	return fbo;
    };

    this->m_mainFBO = pooled (0, this->m_namedMainFBO);
    this->m_subFBO = pooled (1, this->m_namedSubFBO);
}

bool CImage::hitTest (const glm::vec2& ndc) {
    if (this->getImage ().model->fullscreen) {
	return true;
    }

    this->updateScreenSpacePosition ();

    const glm::vec3 center ((this->m_pos.x + this->m_pos.z) / 2.0f, (this->m_pos.y + this->m_pos.w) / 2.0f, 0.0f);
    const glm::vec2 half (
	std::abs (this->m_pos.z - this->m_pos.x) / 2.0f, std::abs (this->m_pos.w - this->m_pos.y) / 2.0f
    );

    return Wallpapers::CScene::quadContainsPoint (
	glm::translate (this->m_modelViewProjectionScreen, center), half, ndc
    );
}

glm::vec2 CImage::cursorLocalPosition (const glm::vec2& ndc) {
    this->updateScreenSpacePosition ();

    const glm::vec3 center ((this->m_pos.x + this->m_pos.z) / 2.0f, (this->m_pos.y + this->m_pos.w) / 2.0f, 0.0f);
    const auto point
	= Wallpapers::CScene::quadPlanePoint (glm::translate (this->m_modelViewProjectionScreen, center), ndc);
    const glm::vec2 extent (this->m_pos.z - this->m_pos.x, this->m_pos.y - this->m_pos.w);

    if (!point.has_value () || extent.x == 0.0f || extent.y == 0.0f) {
	return glm::vec2 (0.0f);
    }

    // m_pos runs y down from the top edge (.w), x from the texture's left edge (.x)
    const glm::vec2 u (
	(point->x + center.x - this->m_pos.x) / extent.x, (point->y + center.y - this->m_pos.w) / extent.y
    );

    return u * this->m_displaySize;
}

std::optional<std::string> CImage::cursorHitBox (const glm::vec2& ndc) const {
    if (!this->m_hasPuppetMesh) {
	return std::nullopt;
    }

    glm::vec3 origin;
    glm::vec3 direction;
    this->getScene ().cursorLine (ndc, this->getImage ().perspective->value->getBool (), origin, direction);

    return this->m_rig.imageHitBox (origin, direction);
}

CImage::~CImage () {
    this->m_texture->decrementUsageCount ();

    // delete passes first as they depend on the image's data
    for (auto* pass : this->m_allPasses.empty () ? this->m_passes : this->m_allPasses) {
	delete pass;
    }

    this->m_passes.clear ();
    this->m_allPasses.clear ();

    for (auto* pass : this->m_puppetClipMaskPasses) {
	delete pass;
    }

    delete this->m_puppetClipTargetPass;
    delete this->m_puppetClipComposePass;

    glDeleteBuffers (1, &this->m_sceneSpacePosition);
    glDeleteBuffers (1, &this->m_copySpacePosition);
    glDeleteBuffers (1, &this->m_passSpacePosition);
    glDeleteBuffers (1, &this->m_texcoordCopy);
    glDeleteBuffers (1, &this->m_texcoordPass);
    glDeleteBuffers (1, &this->m_texcoordDirect);
    glDeleteBuffers (1, &this->m_texcoordFinal);
    if (this->m_puppetSpacePosition != GL_NONE) {
	glDeleteBuffers (1, &this->m_puppetSpacePosition);
    }
    if (this->m_puppetTexCoord != GL_NONE) {
	glDeleteBuffers (1, &this->m_puppetTexCoord);
    }
    if (this->m_puppetVertexAlphaWeights != GL_NONE) {
	glDeleteBuffers (1, &this->m_puppetVertexAlphaWeights);
    }
    if (this->m_puppetIndices != GL_NONE) {
	glDeleteBuffers (1, &this->m_puppetIndices);
    }
    delete this->m_blendMap.copyPass;
    delete this->m_blendMap.pass;

    for (const GLuint buffer :
	 { this->m_puppetPrelight.bindPositions, this->m_puppetPrelight.morphedPositions,
	   this->m_puppetPrelight.flatPositions, this->m_puppetPrelight.normals, this->m_puppetPrelight.tangents,
	   this->m_puppetPrelight.blendIndices, this->m_puppetPrelight.blendWeights, this->m_puppetClipIndices,
	   this->m_puppetClipComposePosition, this->m_puppetClipComposeTexCoord, this->m_blendMap.vertices,
	   this->m_blendMap.indices, this->m_blendMap.quad }) {
	if (buffer != GL_NONE) {
	    glDeleteBuffers (1, &buffer);
	}
    }
}

bool CImage::loadPuppetMesh (const glm::vec2& size) {
    if (!this->getImage ().model->puppet.has_value ()) {
	return false;
    }

    try {
	const auto file = this->getScene ().readModelFile (*this->getImage ().model->puppet);
	const std::vector<char>& data = *file;

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

	std::optional<PuppetVertexLayout> layout;

	try {
	    layout = readPuppetVertexLayout (data, meshHeaderSize);
	} catch (const std::exception& ex) {
	    sLog.error ("Could not read the first mesh of ", *this->getImage ().model->puppet, ": ", ex.what ());
	}

	if (!layout.has_value ()) {
	    layout = resolvePuppetVertexLayout (reader, markerSize, mdlsOffset, meshHeaderSize);
	}
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
	glBufferData (
	    GL_ARRAY_BUFFER, mesh->texcoords.size () * sizeof (GLfloat), mesh->texcoords.data (), GL_STATIC_DRAW
	);

	glGenBuffers (1, &this->m_puppetIndices);
	glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->m_puppetIndices);
	glBufferData (
	    GL_ELEMENT_ARRAY_BUFFER, mesh->indices.size () * sizeof (GLushort), mesh->indices.data (), GL_STATIC_DRAW
	);

	this->m_puppetIndexCount = static_cast<GLsizei> (mesh->indices.size ());
	this->m_puppetDrawBuffer = this->m_puppetIndices;
	this->m_puppetDrawCount = this->m_puppetIndexCount;

	this->loadPuppetBlendMesh (data);

	// the part ranges and clipping records follow the index buffer (sub_140261880)
	const int version
	    = puppetVersion.size () > strlen ("MDLV") ? std::atoi (puppetVersion.c_str () + strlen ("MDLV")) : 0;
	const size_t indexEnd = layout->block.headerOffset + meshHeaderSize + layout->block.vertexBytes
	    + sizeof (uint32_t) + layout->block.indexBytes;
	this->m_puppetClipping.reset ();
	this->m_puppetParts.clear ();
	this->m_puppetPartOrder.clear ();
	this->m_puppetMeshIndices.clear ();

	// 4 bone alpha, 8 draw order, 0x1000 morph alpha
	this->m_puppetMeshFlags = 0;
	this->m_puppetMeshFormat = 0;

	try {
	    const auto header = readPuppetMeshHeader (data);

	    if (header.vertices == layout->block.headerOffset + meshHeaderSize) {
		this->m_puppetMeshFlags = header.flags;
		this->m_puppetMeshFormat = header.format;
	    } else {
		sLog.error (
		    "The first mesh header of ", *this->getImage ().model->puppet, " isn't where its vertices are"
		);
	    }
	} catch (const std::exception& ex) {
	    sLog.error ("Could not read the mesh header of ", *this->getImage ().model->puppet, ": ", ex.what ());
	}

	const uint32_t meshFlags = this->m_puppetMeshFlags;
	const size_t vertexCount = layout->block.vertexBytes / layout->vertexStride;
	const size_t verticesOffset = layout->block.headerOffset + meshHeaderSize;

	// MDLV 21+: flag, u32 and a block of one vec3 per vertex after the index buffer (a_PositionC1, sub_140261880)
	this->m_puppetPrelight.auxPositions.clear ();

	if (version >= 21 && indexEnd < data.size () && data[indexEnd] != 0) {
	    const size_t blob = indexEnd + 1 + sizeof (uint32_t);
	    uint32_t bytes = 0;

	    if (blob + sizeof (bytes) <= data.size ()) {
		std::memcpy (&bytes, data.data () + blob, sizeof (bytes));
	    }

	    // WE reads three floats per vertex whatever the size says
	    if (bytes >= vertexCount * sizeof (GLfloat) * 3 && blob + sizeof (bytes) + bytes <= data.size ()) {
		this->m_puppetPrelight.auxPositions.resize (vertexCount * 3);
		std::memcpy (
		    this->m_puppetPrelight.auxPositions.data (), data.data () + blob + sizeof (bytes),
		    vertexCount * sizeof (GLfloat) * 3
		);
	    } else {
		sLog.error (
		    "The auxiliary vertex block of ", *this->getImage ().model->puppet,
		    " is shorter than the mesh, lighting places the bind positions instead"
		);
	    }
	}

	const auto readComponent = [&] (uint32_t bit, size_t floats, std::vector<GLfloat>& out) {
	    out.clear ();
	    const auto offset = CMesh::vertexComponentOffset (this->m_puppetMeshFormat, bit);

	    if (!offset.has_value () || *offset + floats * sizeof (GLfloat) > layout->vertexStride) {
		return;
	    }

	    out.resize (vertexCount * floats);

	    for (size_t vertex = 0; vertex < vertexCount; vertex++) {
		std::memcpy (
		    out.data () + vertex * floats,
		    data.data () + verticesOffset + vertex * layout->vertexStride + *offset, floats * sizeof (GLfloat)
		);
	    }
	};

	readComponent (0x2, 3, this->m_puppetPrelight.normalData);
	readComponent (0x4, 4, this->m_puppetPrelight.tangentData);

	// position.w is the morph texel, <= 0 means not morphed
	this->m_puppetMorph.reset ();
	this->m_puppetMorphIndices.clear ();

	if (this->m_puppetMeshFormat & 0x10000) {
	    this->m_puppetMorphIndices.resize (vertexCount);

	    for (size_t vertex = 0; vertex < vertexCount; vertex++) {
		float w;
		std::memcpy (
		    &w, data.data () + verticesOffset + vertex * layout->vertexStride + sizeof (float) * 3, sizeof (w)
		);

		if (w > 0.0f) {
		    this->m_puppetMorphIndices[vertex] = static_cast<uint32_t> (w);
		}
	    }
	}

	if (meshFlags & 8) {
	    try {
		this->m_puppetParts = PuppetClipping::readParts (data, indexEnd, version, mesh->indices.size ());
	    } catch (const std::exception& ex) {
		sLog.error ("Ignoring the part ranges of ", *this->getImage ().model->puppet, ": ", ex.what ());
	    }

	    if (!this->m_puppetParts.empty ()) {
		this->m_puppetMeshIndices = mesh->indices;
		this->m_puppetPartOrder.resize (this->m_puppetParts.size ());

		for (uint32_t index = 0; index < this->m_puppetPartOrder.size (); index++) {
		    this->m_puppetPartOrder[index] = index;
		}
	    }
	}

	try {
	    auto clipping = PuppetClipping::read (data, indexEnd, version, mesh->indices.size ());

	    if (clipping.has_value ()) {
		clipping->build (mesh->indices);
		glGenBuffers (1, &this->m_puppetClipIndices);
		glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->m_puppetClipIndices);
		glBufferData (
		    GL_ELEMENT_ARRAY_BUFFER, clipping->indices.size () * sizeof (GLushort), clipping->indices.data (),
		    GL_STATIC_DRAW
		);
		sLog.out (
		    "Puppet ", *this->getImage ().model->puppet, " clipping: ", clipping->records.size (), " masks, ",
		    clipping->parts.size (), " parts, ", clipping->draws.size (), " draws"
		);
		this->m_puppetClipping = std::move (clipping);
	    }
	} catch (const std::exception& ex) {
	    sLog.error ("Ignoring the clipping masks of ", *this->getImage ().model->puppet, ": ", ex.what ());
	}

	sLog.out (
	    "Loaded puppet mesh ", *this->getImage ().model->puppet, " version=", puppetVersion,
	    " stride=", layout->vertexStride, " vertices=", this->m_puppetRawPositions.size () / 3,
	    " indices=", this->m_puppetIndexCount
	);

	this->m_rig.clear ();
	this->m_puppetBlendIndices.clear ();
	this->m_puppetBlendWeights.clear ();
	this->m_puppetSkinnedPositions.clear ();

	const auto blend = readPuppetBlendData (reader, layout->block, meshHeaderSize, layout->vertexStride);
	if (blend.has_value ()) {
	    this->m_puppetBlendIndices = blend->indices;
	    this->m_puppetBlendWeights = blend->weights;
	}

	if (mdlsOffset < data.size () && blend.has_value ()) {
	    try {
		// MDLV header: tag, flags, a second field and the mesh count (2.8.42 sub_140261880)
		uint32_t meshCount = 0;
		if (data.size () >= 21) {
		    std::memcpy (&meshCount, data.data () + 17, sizeof (meshCount));
		}

		this->m_rig.load (data, mdlsOffset, meshCount, *this->getImage ().model->puppet);
		this->m_rig.drawOrderEnabled = !this->m_puppetParts.empty ();
		this->m_rig.boneAlphaEnabled = (meshFlags & 4) != 0;
		this->m_rig.morphBlendClamped = false;
		this->m_rig.addSceneLayers (this->getImage ().animationLayers);

		if ((this->m_puppetMeshFormat & 0x10000) && this->m_rig.getMorphSection () != 0) {
		    try {
			this->m_puppetMorph = readPuppetMorphTargets (data, this->m_rig.getMorphSection (), meshFlags);
		    } catch (const std::exception& ex) {
			sLog.error (
			    "Ignoring the morph targets of ", *this->getImage ().model->puppet, ": ", ex.what ()
			);
		    }

		    if (this->m_puppetMorph.has_value ()
			&& std::ranges::any_of (this->m_puppetMorph->boneRules, [this] (const auto& rule) {
			       return rule.bone >= this->m_rig.bones.size ();
			   })) {
			sLog.error (
			    "Puppet ", *this->getImage ().model->puppet,
			    " has a morph target bone rule past its skeleton, drawing it without morphs"
			);
			this->m_puppetMorph.reset ();
		    }
		}
	    } catch (const std::exception& ex) {
		sLog.error (
		    "Could not load puppet skeleton/animation from ", *this->getImage ().model->puppet, ": ",
		    ex.what (), " (falling back to the static bind pose)"
		);
		this->m_rig.clear ();
	    }
	}

	// vertex alpha via SKINNING_ALPHA (flag 4) or morph alpha < 1, both need the skinned material
	this->m_puppetVertexAlpha = !this->m_rig.bones.empty ()
	    && ((meshFlags & 4) != 0 || (this->m_puppetMorph.has_value () && this->m_puppetMorph->alpha));

	if (this->m_puppetVertexAlpha) {
	    this->updatePuppetVertexAlpha ({});
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
	= !this->m_puppetSkinnedPositions.empty () ? this->m_puppetSkinnedPositions : this->m_puppetRawPositions;

    if (source.empty ()) {
	return;
    }

    // on the scene the puppet keeps its own y down vertices placed by m_modelViewProjectionScreen, the first effect
    // pass draws into the buffer through m_modelViewProjectionCopy
    const bool drawsOnScene = this->m_passes.size () <= 1 || this->m_puppetMeshLast;
    const glm::vec2 bufferOffset = drawsOnScene ? glm::vec2 (0.0f) : size / 2.0f;

    std::vector<GLfloat> positions;
    positions.reserve (source.size ());
    for (size_t index = 0; index + 2 < source.size (); index += 3) {
	positions.push_back (bufferOffset.x + source[index]);
	positions.push_back (bufferOffset.y - source[index + 1]);
	// raw .mdl Z values aren't used by this engine's orthographic puppet compositing (depth test
	// is disabled for puppets; layering comes from draw order + alpha blending) - and glm::ortho's
	// clip.z = -localZ has no near/far normalization, so a puppet's real mesh depth (tens of units)
	// would get clipped outside [-1,1] and lose most of the mesh. Zero it instead.
	positions.push_back (0.0f);
    }

    // not during the constructor, m_passes isn't resolved yet
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
	    "Puppet position bake for ", this->getImage ().name, " (", this->getId (), "): drawsOnScene=", drawsOnScene,
	    " passes=", this->m_passes.size (), " vertexCount=", positions.size () / 3, " boundsMin=(", boundsMin.x,
	    ",", boundsMin.y, ",", boundsMin.z, ") boundsMax=(", boundsMax.x, ",", boundsMax.y, ",", boundsMax.z, ")"
	);
    }

    if (this->m_puppetSpacePosition == GL_NONE) {
	glGenBuffers (1, &this->m_puppetSpacePosition);
    }
    glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetSpacePosition);
    glBufferData (GL_ARRAY_BUFFER, positions.size () * sizeof (GLfloat), positions.data (), GL_DYNAMIC_DRAW);
}

PuppetActiveAnimation* CImage::findPuppetAnimationLayer (size_t serial) { return this->m_rig.findLayer (serial); }

size_t CImage::getPuppetAnimationLayerCount () const { return this->m_rig.getLayerCount (); }

std::optional<size_t> CImage::getPuppetAnimationLayerAt (int64_t index) const { return this->m_rig.getLayerAt (index); }

std::optional<size_t> CImage::findPuppetAnimationLayerByName (const std::string& name) const {
    return this->m_rig.findLayerByName (name);
}

std::optional<size_t> CImage::createPuppetAnimationLayer (
    const Data::JSON::JSON& animation, const Data::JSON::JSON& config, bool autoRemove
) {
    return this->m_rig.createLayer (animation, config, autoRemove, this->getScene ().getScene ().project);
}

bool CImage::destroyPuppetAnimationLayersByName (const std::string& name) {
    return this->m_rig.destroyLayersByName (name);
}

bool CImage::destroyPuppetAnimationLayer (size_t serial) { return this->m_rig.destroyLayer (serial); }

void CImage::updatePuppetPose () {
    if (!this->m_hasPuppetMesh || this->m_rig.bones.empty ()) {
	return;
    }

    if (this->getScene ().getContext ().getApp ().getContext ().settings.render.debug.noPuppetAnimation) {
	return;
    }

    this->m_rig.ropeEnvironment = this->getScene ().getRopeEnvironment ();
    this->m_rig.updatePose (this->puppetObjectWorld ());
    this->updatePuppetDrawOrder ();
    for (const auto& payload : this->m_rig.takeFiredEvents ()) {
	this->getScene ().getScriptEngine ().dispatchClipEvent (*this, payload);
    }
    this->m_rig.finishEndedLayers ([this] (size_t serial) {
	this->getScene ().getScriptEngine ().dispatchAnimationLayerEnded (*this, serial);
    });
    for (const auto& removed : this->m_rig.takeRemovedLayers ()) {
	this->unregisterAnimationLayerProperties (removed.serial);
    }
}

void CImage::updatePuppetDrawOrder () {
    if (!this->m_rig.drawOrderTouched || this->m_puppetParts.empty ()) {
	return;
    }

    // a part's key is (int) (its order + its bone's MDLS order, made when the parts are, + the bone's animated value)
    const auto& boneOrder = this->m_rig.boneDrawOrder;
    const auto& animated = this->m_rig.drawOrder;
    std::vector<int> keys (this->m_puppetParts.size (), 0);

    for (size_t index = 0; index < this->m_puppetParts.size (); index++) {
	const auto& part = this->m_puppetParts[index];
	const int base = static_cast<int> (part.order) + (part.bone < boneOrder.size () ? boneOrder[part.bone] : 0);
	keys[index] = static_cast<int> (
	    static_cast<float> (base) + (part.bone < animated.size () ? animated[part.bone] : 0.0f)
	);
    }

    const auto byKey = [&keys] (uint32_t a, uint32_t b) { return keys[a] < keys[b]; };

    if (std::ranges::is_sorted (this->m_puppetPartOrder, byKey)) {
	return;
    }

    // MSVC's std::sort (sub_1402154D0) is an insertion sort up to 32 elements, which keeps equal keys in order
    std::ranges::stable_sort (this->m_puppetPartOrder, byKey);

    if (this->m_puppetClipping.has_value ()) {
	this->m_puppetClipping->order = this->m_puppetPartOrder;

	this->m_puppetClipping->build (this->m_puppetMeshIndices);
	glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->m_puppetClipIndices);
	glBufferData (
	    GL_ELEMENT_ARRAY_BUFFER, this->m_puppetClipping->indices.size () * sizeof (GLushort),
	    this->m_puppetClipping->indices.data (), GL_DYNAMIC_DRAW
	);

	return;
    }

    std::vector<uint16_t> indices;
    indices.reserve (this->m_puppetMeshIndices.size ());

    for (const uint32_t index : this->m_puppetPartOrder) {
	const auto& part = this->m_puppetParts[index];
	indices.insert (
	    indices.end (), this->m_puppetMeshIndices.begin () + part.firstIndex,
	    this->m_puppetMeshIndices.begin () + part.firstIndex + part.indexCount
	);
    }

    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->m_puppetIndices);
    glBufferData (GL_ELEMENT_ARRAY_BUFFER, indices.size () * sizeof (GLushort), indices.data (), GL_DYNAMIC_DRAW);
    this->m_puppetIndexCount = static_cast<GLsizei> (indices.size ());
    this->m_puppetDrawCount = this->m_puppetIndexCount;
}

void CImage::updatePuppetSkinning () {
    if (!this->m_rig.poseAnimated || this->m_rig.boneModel.size () != this->m_rig.bones.size ()) {
	return;
    }

    const auto& worldAnimated = this->m_rig.boneModel;
    std::vector<glm::mat4> skinMatrices (this->m_rig.bones.size ());
    for (size_t i = 0; i < this->m_rig.bones.size (); i++) {
	skinMatrices[i] = worldAnimated[i] * this->m_rig.bones[i].inverseBindWorld;
    }

    // sub_14020CFF0: up to 11 active targets, stable sorted by weight descending
    std::vector<std::pair<uint32_t, float>> targets;

    if (this->m_puppetMorph.has_value () && this->m_puppetMorphShader && !this->m_rig.morphWeights.empty ()) {
	const auto& state = this->m_rig.morphWeights.front ();

	for (uint32_t target = 0; target < state.weights.size () && target < 64 && targets.size () < 11; target++) {
	    if (state.active & (uint64_t (1) << target)) {
		targets.emplace_back (target, state.weights[target]);
	    }
	}

	std::ranges::stable_sort (targets, std::ranges::greater {}, &std::pair<uint32_t, float>::second);
    }

    // inverse of the rule bone's model matrix. Animation tracks can name targets past the mesh's own (up to 64),
    // those have no rule and no texels
    const bool boneRules = !targets.empty () && !this->m_puppetMorph->boneRules.empty ();
    std::vector<const PuppetMorphTargets::BoneRule*> rules;
    std::vector<glm::mat4> ruleInverse;

    if (boneRules) {
	const auto& all = this->m_puppetMorph->boneRules;

	for (const auto& [target, weight] : targets) {
	    const auto* rule
		= target < all.size () && all[target].bone < worldAnimated.size () ? &all[target] : nullptr;
	    rules.push_back (rule);
	    ruleInverse.push_back (rule != nullptr ? glm::inverse (worldAnimated[rule->bone]) : glm::mat4 (1.0f));
	}
    }

    const auto skin = [this, &skinMatrices] (size_t v, const glm::vec4& position) {
	glm::vec3 skinned (0.0f);
	const glm::uvec4& indices
	    = v < this->m_puppetBlendIndices.size () ? this->m_puppetBlendIndices[v] : glm::uvec4 (0);
	const glm::vec4& weights
	    = v < this->m_puppetBlendWeights.size () ? this->m_puppetBlendWeights[v] : glm::vec4 (0.0f);

	for (int influence = 0; influence < 4; influence++) {
	    const float weight = weights[influence];
	    if (weight == 0.0f) {
		continue;
	    }

	    const uint32_t boneIndex = indices[influence];
	    if (boneIndex >= skinMatrices.size ()) {
		continue;
	    }

	    skinned += weight * glm::vec3 (skinMatrices[boneIndex] * position);
	}

	return skinned;
    };

    // HLSL: a zero width range gives NaN, saturate makes it 0
    const auto smoothstep = [] (float edge0, float edge1, float x) {
	float t = (x - edge0) / (edge1 - edge0);
	t = std::isnan (t) ? 0.0f : std::clamp (t, 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
    };

    const size_t vertexCount = this->m_puppetRawPositions.size () / 3;
    std::vector<float> morphAlpha;

    if (this->m_puppetVertexAlpha && !targets.empty ()) {
	morphAlpha.assign (vertexCount, 1.0f);
    }

    this->m_puppetSkinnedPositions.assign (this->m_puppetRawPositions.size (), 0.0f);

    auto& prelight = this->m_puppetPrelight;
    const bool prelightMorphs = prelight.pass != nullptr && !targets.empty ();

    if (prelightMorphs) {
	prelight.morphedData = this->m_puppetRawPositions;
    } else {
	prelight.morphedData.clear ();
    }

    for (size_t v = 0; v < vertexCount; v++) {
	glm::vec4 bindPos (
	    this->m_puppetRawPositions[v * 3], this->m_puppetRawPositions[v * 3 + 1],
	    this->m_puppetRawPositions[v * 3 + 2], 1.0f
	);

	if (!targets.empty () && v < this->m_puppetMorphIndices.size () && this->m_puppetMorphIndices[v].has_value ()) {
	    const auto& morph = *this->m_puppetMorph;
	    glm::vec3 delta (0.0f);
	    float alpha = 1.0f;

	    // MORPHING_MODIFIERS: skinned unmorphed position in the rule bone's space scales each target
	    const glm::vec4 preMorph = boneRules ? glm::vec4 (skin (v, bindPos), 1.0f) : glm::vec4 (0.0f);

	    for (size_t i = 0; i < targets.size (); i++) {
		const auto target = targets[i].first;
		float weight = targets[i].second;
		const size_t texel = *this->m_puppetMorphIndices[v] + static_cast<size_t> (morph.vertexCount) * target;
		const glm::vec4 value = texel < morph.texels.size () ? morph.texels[texel] : glm::vec4 (0.0f);

		if (boneRules && rules[i] != nullptr) {
		    const auto& rule = *rules[i];
		    const glm::vec3 local (ruleInverse[i] * preMorph);
		    const float point = smoothstep (rule.edge0, rule.edge1, glm::length (glm::vec2 (local)));
		    const float axis = smoothstep (rule.edge0, rule.edge1, local.x);
		    weight *= glm::mix (point, axis, rule.axis);
		}

		delta += glm::vec3 (value) * weight;
		alpha *= value.w * weight + (1.0f - weight);
	    }

	    bindPos += glm::vec4 (delta * morph.scale, 0.0f);

	    if (prelightMorphs) {
		prelight.morphedData[v * 3] = bindPos.x;
		prelight.morphedData[v * 3 + 1] = bindPos.y;
		prelight.morphedData[v * 3 + 2] = bindPos.z;
	    }

	    if (!morphAlpha.empty ()) {
		morphAlpha[v] = alpha;
	    }
	}

	const glm::vec3 skinned = skin (v, bindPos);

	this->m_puppetSkinnedPositions[v * 3] = skinned.x;
	this->m_puppetSkinnedPositions[v * 3 + 1] = skinned.y;
	this->m_puppetSkinnedPositions[v * 3 + 2] = skinned.z;
    }

    this->updatePuppetPositionBuffer (this->m_size);

    if (this->m_puppetVertexAlpha) {
	this->updatePuppetVertexAlpha (morphAlpha);
    }

    if (prelight.pass != nullptr) {
	packPuppetBones (skinMatrices, prelight.bones);

	if (prelightMorphs) {
	    glBindBuffer (GL_ARRAY_BUFFER, prelight.morphedPositions);
	    glBufferData (
		GL_ARRAY_BUFFER, prelight.morphedData.size () * sizeof (GLfloat), prelight.morphedData.data (),
		GL_DYNAMIC_DRAW
	    );
	}
    }
}

void CImage::updatePuppetVertexAlpha (const std::vector<float>& morphAlpha) {
    // morph alpha * saturate (sum of bone alpha * weight), sub_140206430
    const size_t vertexCount = this->m_puppetRawPositions.size () / 3;
    const auto& boneAlpha = this->m_rig.boneAlpha;
    this->m_puppetVertexAlphaData.assign (vertexCount * 4, 0.0f);

    for (size_t v = 0; v < vertexCount; v++) {
	float alpha = v < morphAlpha.size () ? morphAlpha[v] : 1.0f;

	if (this->m_puppetMeshFlags & 4) {
	    const glm::uvec4& indices
		= v < this->m_puppetBlendIndices.size () ? this->m_puppetBlendIndices[v] : glm::uvec4 (0);
	    const glm::vec4& weights
		= v < this->m_puppetBlendWeights.size () ? this->m_puppetBlendWeights[v] : glm::vec4 (0.0f);
	    float bones = 0.0f;

	    for (int influence = 0; influence < 4; influence++) {
		if (indices[influence] < this->m_rig.bones.size ()) {
		    bones += (indices[influence] < boneAlpha.size () ? boneAlpha[indices[influence]] : 1.0f)
			* weights[influence];
		}
	    }

	    alpha *= std::clamp (bones, 0.0f, 1.0f);
	}

	this->m_puppetVertexAlphaData[v * 4] = alpha;
	this->m_puppetVertexAlphaData[v * 4 + 1] = 1.0f - alpha;
    }

    if (this->m_puppetVertexAlphaWeights == GL_NONE) {
	glGenBuffers (1, &this->m_puppetVertexAlphaWeights);
    }

    glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetVertexAlphaWeights);
    glBufferData (
	GL_ARRAY_BUFFER, this->m_puppetVertexAlphaData.size () * sizeof (GLfloat),
	this->m_puppetVertexAlphaData.data (), GL_DYNAMIC_DRAW
    );
}

ComboMap CImage::puppetVertexAlphaCombos () const {
    if (!this->m_puppetVertexAlpha) {
	return {};
    }

    // sub_140209540; BONECOUNT at least 2 for the alpha trick below
    return {
	{ "SKINNING", 1 },
	{ "BONECOUNT", std::max<int> (2, static_cast<int> (this->m_rig.bones.size ())) },
	{ "SKINNING_ALPHA", 1 },
    };
}

glm::mat4 CImage::worldMatrix () const {
    const glm::mat4 world = this->getScene ().objectWorldMatrix (this->getObject ());
    const uint32_t alignment
	= Data::Parsers::ObjectParser::parseAlignment (this->getImage ().alignmentName->value->getString ());

    if (alignment == ImageAlignment_Center) {
	return world;
    }

    // sub_1402066A0 takes half the object size (+752, truncated to int, 2x2 for fullscreen layers via
    // sub_140209360), puppets included, in the layer's own scaled and rotated space
    const glm::vec2 size = this->getImage ().model->fullscreen ? glm::vec2 (2.0f) : glm::trunc (this->m_displaySize);
    glm::vec3 offset (0.0f);

    if (alignment & ImageAlignment_Left) {
	offset.x = size.x * 0.5f;
    } else if (alignment & ImageAlignment_Right) {
	offset.x = -size.x * 0.5f;
    }

    if (alignment & ImageAlignment_Top) {
	offset.y = -size.y * 0.5f;
    } else if (alignment & ImageAlignment_Bottom) {
	offset.y = size.y * 0.5f;
    }

    return glm::translate (world, offset);
}

glm::mat4 CImage::puppetObjectWorld () const { return this->worldMatrix (); }

CImage::TextureAnimation* CImage::getTextureAnimation () {
    if (!this->m_textureAnimation.has_value ()) {
	if (this->m_texture == nullptr || !this->m_texture->isAnimated () || this->m_texture->getFrames ().empty ()) {
	    return nullptr;
	}

	this->m_textureAnimation = TextureAnimation {};
    }

    return &*this->m_textureAnimation;
}

std::pair<int, float> CImage::sharedTextureFrame () const {
    if (this->m_texture == nullptr || !this->m_texture->isAnimated ()) {
	return { 0, 0.0f };
    }

    // the same walk CPass::resolveTextureAnimationState does on the scene clock
    const auto& frames = this->m_texture->getFrames ();
    double time = fmod (static_cast<double> (g_Time), this->getAnimationTime ());

    for (size_t i = 0; i < frames.size (); i++) {
	if (time - frames[i]->frametime <= 0.0) {
	    return { static_cast<int> (i), static_cast<float> (time) };
	}

	time -= frames[i]->frametime;
    }

    return { 0, 0.0f };
}

int CImage::getTextureFrameCount () const {
    return this->m_texture != nullptr && this->m_texture->isAnimated ()
	? static_cast<int> (this->m_texture->getFrames ().size ())
	: 0;
}

float CImage::getTextureDuration () const {
    return this->m_texture != nullptr && this->m_texture->isAnimated () ? static_cast<float> (this->getAnimationTime ())
									: 0.0f;
}

void CImage::updateTextureAnimation (float frametime) {
    // a scene drawn on several outputs updates its objects once per output
    if (this->m_textureAnimationClock == g_Time) {
	return;
    }

    this->m_textureAnimationClock = g_Time;

    if (!this->m_textureAnimation.has_value () || !this->m_textureAnimation->detached
	|| !this->m_textureAnimation->playing || this->m_texture == nullptr || !this->m_texture->isAnimated ()) {
	return;
    }

    // sub_14015FDD0: forwards wraps to the first frame, backwards to the last, the time stays inside the frame
    auto& animation = *this->m_textureAnimation;
    const auto& frames = this->m_texture->getFrames ();
    const int count = static_cast<int> (frames.size ());
    const float step = frametime * animation.rate;
    const auto frameTime = [&frames, count] (int index) {
	return static_cast<float> (frames[index >= 0 && index < count ? index : 0]->frametime);
    };

    if (count == 0 || step == 0.0f) {
	return;
    }

    animation.time += step;

    if (step > 0.0f) {
	const float current = frameTime (animation.frame);

	if (animation.time >= current) {
	    const float rest = animation.time - current;
	    // compared unsigned, a negative frame wraps to the first one too
	    animation.frame = animation.frame + 1 < 0 || animation.frame + 1 >= count ? 0 : animation.frame + 1;
	    animation.time = std::min (rest, frameTime (animation.frame));
	}
    } else if (animation.time <= 0.0f) {
	animation.frame = animation.frame - 1 >= 0 ? animation.frame - 1 : count - 1;
	animation.time = std::max (animation.time + frameTime (animation.frame), 0.0f);
    }
}

std::optional<int> CImage::getTextureFrameOverride () const {
    // a negative frame leaves the texture on its shared clock
    if (!this->m_textureAnimation.has_value () || !this->m_textureAnimation->detached
	|| this->m_textureAnimation->frame < 0) {
	return std::nullopt;
    }

    return this->m_textureAnimation->frame;
}

bool CImage::hasPuppetPose () const { return this->m_rig.hasPose (); }

int CImage::getBlendShapeIndex (const std::string& name) const {
    if (!this->m_puppetMorph.has_value () || name.empty ()) {
	return -1;
    }

    const auto& names = this->m_puppetMorph->names;
    const auto it = std::ranges::find (names, name);

    return it == names.end () ? -1 : static_cast<int> (it - names.begin ());
}

float CImage::getBlendShapeWeight (int target) const {
    if (target < 0 || this->m_rig.morphWeights.empty ()) {
	return 0.0f;
    }

    const auto& weights = this->m_rig.morphWeights.front ().weights;

    return static_cast<size_t> (target) < weights.size () ? weights[target] : 0.0f;
}

void CImage::setBlendShapeWeight (int target, float weight) {
    if (target < 0 || !this->m_puppetMorph.has_value ()
	|| static_cast<size_t> (target) >= this->m_puppetMorph->names.size ()) {
	return;
    }

    if (this->m_rig.morphWeights.empty ()) {
	this->m_rig.morphWeights.resize (1);
    }

    auto& state = this->m_rig.morphWeights.front ();

    if (state.weights.size () <= static_cast<size_t> (target)) {
	state.weights.resize (this->m_puppetMorph->names.size (), 0.0f);
    }

    state.weights[target] = weight;

    // WE sign-extends the 32 bit (1 << target)
    const auto bit = static_cast<uint64_t> (static_cast<int64_t> (static_cast<int32_t> (1u << (target & 31))));

    if (std::abs (weight) >= 1.1920929e-7f) {
	state.active |= bit;
    } else {
	state.active &= ~bit;
    }
}

int CImage::findPuppetBone (const std::string& name) const { return this->m_rig.findBone (name); }

const glm::mat4& CImage::getPuppetBoneTransform (int bone) const { return this->m_rig.getBoneTransform (bone); }

void CImage::setPuppetBoneTransform (int bone, const glm::mat4& transform) {
    this->m_rig.setBoneTransform (bone, transform, this->puppetObjectWorld ());
}

const glm::mat4& CImage::getPuppetLocalBoneTransform (int bone) const {
    return this->m_rig.getLocalBoneTransform (bone);
}

void CImage::setPuppetLocalBoneTransform (int bone, const glm::mat4& transform) {
    this->m_rig.setLocalBoneTransform (bone, transform, this->puppetObjectWorld ());
}

void CImage::applyPuppetBonePhysicsImpulse (int bone, const glm::vec3& directional, const glm::vec3& angularDegrees) {
    this->m_rig.applyBonePhysicsImpulse (bone, directional, angularDegrees);
}

void CImage::resetPuppetBonePhysics (int bone) { this->m_rig.resetBonePhysics (bone); }

std::optional<glm::mat4> CImage::getAttachmentMatrix (const std::string& name) const {
    return this->m_rig.attachmentMatrix (this->m_rig.findAttachment (name));
}

void CImage::setupPuppetGeometryCallback (Effects::CPass* pass) const {
    pass->setGeometryCallback (
	[this, pass] () {
	    const GLint position = glGetAttribLocation (pass->getProgramID (), "a_Position");
	    const GLint texCoord = glGetAttribLocation (pass->getProgramID (), "a_TexCoord");

	    if (!this->m_puppetDrawDiagnosticLogged) {
		this->m_puppetDrawDiagnosticLogged = true;
		sLog.out (
		    "Puppet draw setup for ", this->getImage ().name, " (", this->getId (),
		    "): programID=", pass->getProgramID (), " a_Position=", position, " a_TexCoord=", texCoord,
		    " indexCount=", this->m_puppetIndexCount, " size=", this->m_size.x, "x", this->m_size.y
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

	    // skinning is on the CPU; two identity bones with alpha 1 and 0, weighted (alpha, 1 - alpha), carry the
	    // vertex alpha through SKINNING_ALPHA
	    if (this->m_puppetVertexAlpha && this->m_puppetVertexAlphaWeights != GL_NONE) {
		const GLuint program = pass->getProgramID ();
		const GLint blendWeights = glGetAttribLocation (program, "a_BlendWeights");
		const GLint blendIndices = glGetAttribLocation (program, "a_BlendIndices");
		const GLint normal = glGetAttribLocation (program, "a_Normal");
		const GLint tangent = glGetAttribLocation (program, "a_Tangent4");

		if (blendWeights >= 0) {
		    glEnableVertexAttribArray (blendWeights);
		    glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetVertexAlphaWeights);
		    glVertexAttribPointer (blendWeights, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
		}
		if (blendIndices >= 0) {
		    glDisableVertexAttribArray (blendIndices);
		    glVertexAttribI4ui (blendIndices, 0, 1, 0, 0);
		}
		if (normal >= 0) {
		    glDisableVertexAttribArray (normal);
		    glVertexAttrib3f (normal, 0.0f, 0.0f, 1.0f);
		}
		if (tangent >= 0) {
		    glDisableVertexAttribArray (tangent);
		    glVertexAttrib4f (tangent, 1.0f, 0.0f, 0.0f, 1.0f);
		}

		constexpr GLfloat bones[24]
		    = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f,
			1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f };
		constexpr GLfloat bonesAlpha[2] = { 1.0f, 0.0f };

		if (const GLint location = glGetUniformLocation (program, "g_Bones"); location >= 0) {
		    glUniformMatrix4x3fv (location, 2, GL_FALSE, bones);
		}
		if (const GLint location = glGetUniformLocation (program, "g_BonesAlpha"); location >= 0) {
		    glUniform1fv (location, 2, bonesAlpha);
		}
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
	    // a mesh pass into the layer's own buffer starts it fresh, one meant for the scene may be going into a
	    // passthrough layer's buffer right now, which holds its other children
	    if (pass == this->m_puppetMeshPass && !this->m_puppetDrawKeep
		&& pass->getDestination () != this->getScene ().getFBO ()) {
		GLfloat previousClearColor[4] = {};
		glGetFloatv (GL_COLOR_CLEAR_VALUE, previousClearColor);
		glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
		glClear (GL_COLOR_BUFFER_BIT);
		glClearColor (
		    previousClearColor[0], previousClearColor[1], previousClearColor[2], previousClearColor[3]
		);
	    }

	    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->m_puppetDrawBuffer);
	    glDrawElements (
		GL_TRIANGLES, this->m_puppetDrawCount, GL_UNSIGNED_SHORT,
		reinterpret_cast<const void*> (static_cast<uintptr_t> (this->m_puppetDrawOffset) * sizeof (GLushort))
	    );

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
		    " ownTextureReady=", (this->getTexture () != nullptr && this->getTexture ()->isReady ()),
		    " ownTextureID=", (this->getTexture () != nullptr ? this->getTexture ()->getTextureID (0) : 0),
		    " color4=(", this->getColor4 ().r, ",", this->getColor4 ().g, ",", this->getColor4 ().b, ",",
		    this->getColor4 ().a, ") alpha=", this->getUserAlpha (), " brightness=", this->getBrightness (),
		    " cullEnabled=", (int)cullEnabled, " cullFaceMode=", cullFaceMode, " frontFace=", frontFace,
		    " depthEnabled=", (int)depthEnabled, " scissorEnabled=", (int)scissorEnabled,
		    " blendEnabled=", (int)blendEnabled, " colorMask=(", (int)colorMask[0], ",", (int)colorMask[1], ",",
		    (int)colorMask[2], ",", (int)colorMask[3], ")"
		);
	    }
	},
	[pass] () {
	    for (const char* name : { "a_Position", "a_TexCoord", "a_BlendWeights" }) {
		if (const GLint location = glGetAttribLocation (pass->getProgramID (), name); location >= 0) {
		    glDisableVertexAttribArray (location);
		}
	    }
	}
    );
}

bool CImage::litDrawsDirect () const {
    return this->hasLitMaterial () && !this->getImage ().shape && this->m_passes.size () == 1
	&& this->m_passes.front () == this->m_allPasses.front () && this->m_passesDrawToScreen
	&& this->m_blendMap.pass == nullptr && this->m_puppetPrelight.pass == nullptr
	&& this->m_puppetClipTargetPass == nullptr;
}

void CImage::setupDirectLitPass () {
    if (!this->litDrawsDirect ()) {
	return;
    }

    auto* base = this->m_allPasses.front ();
    const bool meshPass = base == this->m_puppetMeshPass;
    ComboMap combos;

    if (meshPass && this->m_materials.puppetVertexAlpha != nullptr) {
	combos = this->m_materials.puppetVertexAlpha->combos;
    }

    combos.insert_or_assign ("PRELIGHTING", 0);
    this->m_materials.directLit = std::make_unique<ImageEffectPassOverride> (ImageEffectPassOverride {
	.id = -1,
	.combos = std::move (combos),
	.constants = {},
	.textures = {},
    });

    auto* pass = new CPass (
	*this, std::make_shared<FBOProvider> (this), base->getPass (), *this->m_materials.directLit, std::nullopt,
	std::nullopt
    );
    pass->addUniform ("g_NormalModelMatrix", &this->m_lightingNormal);

    delete base;
    this->m_allPasses.front () = pass;
    this->m_directLitPass = pass;

    if (meshPass) {
	this->m_puppetMeshPass = pass;
    }

    this->rebuildActivePasses ();
}

// sub_140209540: lit layers drawn again later get a PRELIGHTING/SKINNING/MORPHING draw into the layer buffer, placed by
// a_PositionC1 (sub_140207B50); morphs on the CPU
void CImage::setupPuppetPrelight () {
    this->m_puppetPrelight.pass = nullptr;

    if (!this->m_hasPuppetMesh || this->m_passes.empty () || this->m_puppetMeshPass == this->m_passes.front ()
	|| !this->hasLitMaterial () || this->m_rig.bones.empty () || this->m_puppetBlendIndices.empty ()
	|| this->getImage ().model->passthrough) {
	return;
    }

    this->m_materials.puppetPrelight = std::make_unique<ImageEffectPassOverride> (ImageEffectPassOverride {
	.id = -1,
	.combos = {
	    { "SKINNING", 1 },
	    { "BONECOUNT", static_cast<int> (this->m_rig.bones.size ()) },
	    { "PRELIGHTINGDUALVERTEX", 1 },
	},
	.constants = {},
	.textures = {},
    });

    auto* pass = new CPass (
	*this, std::make_shared<FBOProvider> (this), this->m_passes.front ()->getPass (),
	*this->m_materials.puppetPrelight, std::nullopt, std::nullopt
    );
    delete this->m_passes.front ();
    this->m_passes.front () = pass;
    this->m_puppetPrelight.pass = pass;

    packPuppetBones (
	std::vector<glm::mat4> (this->m_rig.bones.size (), glm::mat4 (1.0f)), this->m_puppetPrelight.bones
    );
    this->uploadPuppetPrelightBuffers ();

    pass->setGeometryCallback (
	[this, pass] () {
	    const auto& prelight = this->m_puppetPrelight;
	    const GLuint program = pass->getProgramID ();
	    const GLint flat = glGetAttribLocation (program, "a_PositionC1");
	    const auto bind = [program] (const char* name, GLuint buffer, GLint size) {
		const GLint location = glGetAttribLocation (program, name);

		if (location >= 0) {
		    glEnableVertexAttribArray (location);
		    glBindBuffer (GL_ARRAY_BUFFER, buffer);
		    glVertexAttribPointer (location, size, GL_FLOAT, GL_FALSE, 0, nullptr);
		}
	    };

	    bind (
		"a_Position",
		flat >= 0 && !prelight.morphedData.empty () ? prelight.morphedPositions : prelight.bindPositions, 3
	    );
	    bind ("a_PositionC1", prelight.flatPositions, 3);
	    bind ("a_TexCoord", this->m_puppetTexCoord, 2);
	    bind ("a_BlendWeights", prelight.blendWeights, 4);

	    if (const GLint location = glGetAttribLocation (program, "a_BlendIndices"); location >= 0) {
		glEnableVertexAttribArray (location);
		glBindBuffer (GL_ARRAY_BUFFER, prelight.blendIndices);
		glVertexAttribIPointer (location, 4, GL_UNSIGNED_INT, 0, nullptr);
	    }

	    if (const GLint location = glGetAttribLocation (program, "a_Normal"); location >= 0) {
		if (prelight.normals != GL_NONE) {
		    bind ("a_Normal", prelight.normals, 3);
		} else {
		    glDisableVertexAttribArray (location);
		    glVertexAttrib3f (location, 0.0f, 0.0f, 1.0f);
		}
	    }

	    if (const GLint location = glGetAttribLocation (program, "a_Tangent4"); location >= 0) {
		if (prelight.tangents != GL_NONE) {
		    bind ("a_Tangent4", prelight.tangents, 4);
		} else {
		    glDisableVertexAttribArray (location);
		    glVertexAttrib4f (location, 1.0f, 0.0f, 0.0f, 1.0f);
		}
	    }

	    if (const GLint location = glGetUniformLocation (program, "g_Bones"); location >= 0) {
		glUniformMatrix4x3fv (
		    location, static_cast<GLsizei> (prelight.bones.size () / 12), GL_FALSE, prelight.bones.data ()
		);
	    }

	    glDisable (GL_CULL_FACE);
	},
	[this] () {
	    GLfloat previousClearColor[4] = {};
	    glGetFloatv (GL_COLOR_CLEAR_VALUE, previousClearColor);
	    glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
	    glClear (GL_COLOR_BUFFER_BIT);
	    glClearColor (previousClearColor[0], previousClearColor[1], previousClearColor[2], previousClearColor[3]);

	    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->m_puppetIndices);
	    glDrawElements (GL_TRIANGLES, this->m_puppetIndexCount, GL_UNSIGNED_SHORT, nullptr);
	},
	[pass] () {
	    for (const char* name : { "a_Position", "a_PositionC1", "a_TexCoord", "a_BlendWeights", "a_BlendIndices",
				      "a_Normal", "a_Tangent4" }) {
		if (const GLint location = glGetAttribLocation (pass->getProgramID (), name); location >= 0) {
		    glDisableVertexAttribArray (location);
		}
	    }
	}
    );
}

void CImage::uploadPuppetPrelightBuffers () {
    auto& prelight = this->m_puppetPrelight;
    const size_t vertexCount = this->m_puppetRawPositions.size () / 3;
    const auto upload = [] (GLuint& buffer, const void* data, size_t bytes, GLenum usage) {
	if (buffer == GL_NONE) {
	    glGenBuffers (1, &buffer);
	}

	glBindBuffer (GL_ARRAY_BUFFER, buffer);
	glBufferData (GL_ARRAY_BUFFER, static_cast<GLsizeiptr> (bytes), data, usage);
    };
    const auto& raw = this->m_puppetRawPositions;
    const auto& flat = prelight.auxPositions.size () == raw.size () ? prelight.auxPositions : raw;

    upload (prelight.bindPositions, raw.data (), raw.size () * sizeof (GLfloat), GL_STATIC_DRAW);
    upload (prelight.morphedPositions, raw.data (), raw.size () * sizeof (GLfloat), GL_DYNAMIC_DRAW);
    upload (prelight.flatPositions, flat.data (), flat.size () * sizeof (GLfloat), GL_STATIC_DRAW);

    if (prelight.normalData.size () == vertexCount * 3) {
	upload (
	    prelight.normals, prelight.normalData.data (), prelight.normalData.size () * sizeof (GLfloat),
	    GL_STATIC_DRAW
	);
    }

    if (prelight.tangentData.size () == vertexCount * 4) {
	upload (
	    prelight.tangents, prelight.tangentData.data (), prelight.tangentData.size () * sizeof (GLfloat),
	    GL_STATIC_DRAW
	);
    }

    std::vector<glm::uvec4> indices (vertexCount, glm::uvec4 (0));
    std::vector<glm::vec4> weights (vertexCount, glm::vec4 (0.0f));
    std::copy_n (
	this->m_puppetBlendIndices.begin (), std::min (vertexCount, this->m_puppetBlendIndices.size ()),
	indices.begin ()
    );
    std::copy_n (
	this->m_puppetBlendWeights.begin (), std::min (vertexCount, this->m_puppetBlendWeights.size ()),
	weights.begin ()
    );

    upload (prelight.blendIndices, indices.data (), indices.size () * sizeof (glm::uvec4), GL_STATIC_DRAW);
    upload (prelight.blendWeights, weights.data (), weights.size () * sizeof (glm::vec4), GL_STATIC_DRAW);
}

// sub_140209540 / sub_140206AE0: a puppet with clipping records gets clippingmaskimage4 for its masks and its own
// material again with CLIPPINGUVS and CLIPPINGTARGET for what they clip
void CImage::setupPuppetClipping () {
    if (!this->m_puppetClipping.has_value () || this->m_puppetMeshPass == nullptr) {
	return;
    }

    const auto& project = this->getScene ().getScene ().project;
    const auto& records = this->m_puppetClipping->records;
    auto& overrides = this->m_materials.clippingOverrides;
    const auto fboProvider = std::make_shared<FBOProvider> (this);

    // the mesh pass's combos, sub_140209540 adds the clipping ones
    overrides.push_back (
	std::make_unique<ImageEffectPassOverride> (ImageEffectPassOverride {
	    .id = -1,
	    .combos = this->m_materials.puppetVertexAlpha != nullptr ? this->m_materials.puppetVertexAlpha->combos
								     : this->puppetVertexAlphaCombos (),
	    .constants = {},
	    .textures = {},
	})
    );
    overrides.back ()->combos.insert_or_assign ("CLIPPINGUVS", 1);
    overrides.back ()->combos.insert_or_assign ("CLIPPINGTARGET", 1);
    this->m_puppetClipTargetPass = new CPass (
	*this, fboProvider, this->m_puppetMeshPass->getPass (), *overrides.back (), std::nullopt, std::nullopt
    );
    this->m_puppetClipTargetPass->setTexture (8, this->getScene ().requireAlphaMaskFrameBuffer (false));
    this->setupPuppetGeometryCallback (this->m_puppetClipTargetPass);

    ComboMap maskCombos = this->puppetVertexAlphaCombos ();
    const auto& imagePasses = this->getImage ().model->material->passes;

    if (!imagePasses.empty () && imagePasses.front ()->blending == BlendingMode_AlphaToCoverage) {
	maskCombos.emplace ("ALPHATOCOVERAGE", 1);
    }

    this->m_materials.clippingMask = MaterialParser::load (project, "materials/util/clippingmaskimage4.json");

    for (const auto& record : records) {
	overrides.push_back (
	    std::make_unique<ImageEffectPassOverride> (ImageEffectPassOverride {
		.id = -1,
		.combos = maskCombos,
		.constants = {},
		.textures = { { 1, record.mask } },
	    })
	);

	auto* pass = new CPass (
	    *this, fboProvider, **this->m_materials.clippingMask->passes.begin (), *overrides.back (), std::nullopt,
	    std::nullopt
	);
	pass->addUniform ("g_RenderVar0", &this->m_puppetClipRenderVar0);
	pass->setClearColor (&this->m_puppetClipClearColor);
	this->setupPuppetGeometryCallback (pass);
	this->m_puppetClipMaskPasses.push_back (pass);
    }

    if (std::ranges::none_of (records, [] (const PuppetClipping::Record& record) { return record.parent != -1; })) {
	return;
    }

    // nested masks: the intermediate goes through flattexture onto _rt_FullAlphaMask with identity matrices
    constexpr GLfloat positions[] = { -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, -1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f };
    constexpr GLfloat texcoords[] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };

    glGenBuffers (1, &this->m_puppetClipComposePosition);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetClipComposePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (positions), positions, GL_STATIC_DRAW);
    glGenBuffers (1, &this->m_puppetClipComposeTexCoord);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetClipComposeTexCoord);
    glBufferData (GL_ARRAY_BUFFER, sizeof (texcoords), texcoords, GL_STATIC_DRAW);

    this->m_materials.clippingCompose = MaterialParser::load (project, "materials/util/flattexture.json");
    auto* compose = new CPass (
	*this, fboProvider, **this->m_materials.clippingCompose->passes.begin (), std::nullopt, std::nullopt,
	std::nullopt
    );
    compose->setDestination (this->getScene ().requireAlphaMaskFrameBuffer (false));
    compose->setInput (this->getScene ().requireAlphaMaskFrameBuffer (true));
    compose->setKeepDestination (true);
    compose->setModelViewProjectionMatrix (&this->m_puppetClipIdentity);
    compose->setModelViewProjectionMatrixInverse (&this->m_puppetClipIdentity);
    compose->setModelMatrix (&this->m_puppetClipIdentity);
    compose->setViewProjectionMatrix (&this->m_puppetClipIdentity);
    static const glm::mat3 identityNormal (1.0f);
    compose->setLightingTransform (&this->m_puppetClipIdentity, &identityNormal, &this->m_puppetClipIdentity);
    compose->setEffectTextureProjectionMatrix (&this->m_puppetClipIdentity, &this->m_puppetClipIdentity);
    // sub_14020D6A0 sets the renderer alpha to 1 for it
    compose->addUniform ("g_Alpha", &this->m_puppetClipComposeAlpha);
    compose->setGeometryCallback (
	[this, compose] () {
	    const GLint position = glGetAttribLocation (compose->getProgramID (), "a_Position");
	    const GLint texCoord = glGetAttribLocation (compose->getProgramID (), "a_TexCoord");

	    if (position >= 0) {
		glEnableVertexAttribArray (position);
		glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetClipComposePosition);
		glVertexAttribPointer (position, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
	    }

	    if (texCoord >= 0) {
		glEnableVertexAttribArray (texCoord);
		glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetClipComposeTexCoord);
		glVertexAttribPointer (texCoord, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
	    }
	},
	[] () {
	    // translucent with renderer flag 0x100 (sub_140099F60): the source color becomes DEST_COLOR, alpha ONE/ONE,
	    // rgb written only, so the intermediate multiplies into the mask
	    GLboolean colorMask[4] = {};
	    glGetBooleanv (GL_COLOR_WRITEMASK, colorMask);
	    glEnable (GL_BLEND);
	    glBlendFuncSeparate (GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
	    glColorMask (true, true, true, false);
	    glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
	    glColorMask (colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
	},
	[compose] () {
	    for (const char* name : { "a_Position", "a_TexCoord" }) {
		const GLint location = glGetAttribLocation (compose->getProgramID (), name);

		if (location >= 0) {
		    glDisableVertexAttribArray (location);
		}
	    }
	}
    );
    this->m_puppetClipComposePass = compose;
}

void CImage::loadPuppetBlendMesh (const std::vector<char>& data) {
    std::optional<PuppetBlendMesh> mesh;

    try {
	mesh = readPuppetBlendMesh (data);
    } catch (const std::exception& ex) {
	sLog.error ("Could not read the blend map mesh of ", *this->getImage ().model->puppet, ": ", ex.what ());
	return;
    }

    if (!mesh.has_value ()) {
	return;
    }

    const uint32_t stride = CMesh::vertexStride (mesh->format);

    if (stride == 0 || mesh->vertexBytes % stride != 0 || mesh->material.empty ()
	|| !CMesh::vertexComponentOffset (mesh->format, 0x1).has_value ()) {
	sLog.error (
	    "Ignoring the blend map mesh of ", *this->getImage ().model->puppet, ": vertex format ", mesh->format,
	    " doesn't match its data"
	);
	return;
    }

    glGenBuffers (1, &this->m_blendMap.vertices);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_blendMap.vertices);
    glBufferData (GL_ARRAY_BUFFER, mesh->vertexBytes, data.data () + mesh->vertices, GL_STATIC_DRAW);
    glGenBuffers (1, &this->m_blendMap.indices);
    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->m_blendMap.indices);
    glBufferData (GL_ELEMENT_ARRAY_BUFFER, mesh->indexBytes, data.data () + mesh->indices, GL_STATIC_DRAW);

    this->m_blendMap.indexCount = static_cast<GLsizei> (mesh->indexBytes / sizeof (GLushort));
    this->m_blendMap.format = mesh->format;
    this->m_blendMap.stride = stride;
    // g_BlendMap rows from puppet +848
    this->m_blendMap.rows = std::min<uint32_t> (mesh->rows, this->m_rig.blendMap.size () / 4);
    this->m_blendMap.material = mesh->material;
}

// pass flags 0x10 LIGHTING / 0x8 REFLECTION (sub_140209540)
bool CImage::hasLitMaterial () const {
    const auto& materialPasses = this->getImage ().model->material->passes;

    if (materialPasses.empty ()) {
	return false;
    }

    const auto& combos = materialPasses.front ()->combos;
    const auto enabled = [&combos] (const char* name) {
	const auto combo = combos.find (name);
	return combo != combos.end () && combo->second != 0;
    };

    return enabled ("LIGHTING") || enabled ("REFLECTION");
}

// sub_140209540: lit materials draw texture + flag 2 mesh into _rt_imageLayerAlbedo_<id>, else an offscreen layer's
// base draw (sub_140207B50) puts the mesh over the texture. Never for layers drawn straight to the scene
void CImage::setupPuppetBlendMap (bool offscreen) {
    if (this->m_blendMap.indexCount == 0 || this->m_passes.empty () || this->m_texture == nullptr) {
	return;
    }

    if (!this->hasLitMaterial () && !offscreen) {
	return;
    }

    const auto& project = this->getScene ().getScene ().project;
    const auto fboProvider = std::make_shared<FBOProvider> (this);
    const auto& texture = this->m_texture;

    try {
	this->m_materials.blendMap = MaterialParser::load (project, this->m_blendMap.material);
    } catch (const std::exception& ex) {
	sLog.error ("Cannot load the blend map material ", this->m_blendMap.material, ": ", ex.what ());
	return;
    }

    if (this->m_materials.blendMap->passes.empty ()) {
	return;
    }

    this->m_blendMap.albedo = std::make_shared<CFBO> (
	"_rt_imageLayerAlbedo_" + std::to_string (this->getId ()), TextureFormat_ARGB8888, texture->getFlags (), 1.0f,
	texture->getRealWidth (), texture->getRealHeight (), texture->getTextureWidth (0), texture->getTextureHeight (0)
    );

    const glm::vec2 albedoSize (texture->getTextureWidth (0), texture->getTextureHeight (0));
    // mesh is in texture pixels, y up
    this->m_blendMap.projection = glm::ortho (0.0f, albedoSize.x, albedoSize.y, 0.0f, -1.0f, 1.0f);

    constexpr GLfloat quad[] = { -1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, -1.0f, 0.0f, 1.0f, 0.0f,
				 -1.0f, 1.0f,  0.0f, 0.0f, 1.0f, 1.0f, 1.0f,  0.0f, 1.0f, 1.0f };

    glGenBuffers (1, &this->m_blendMap.quad);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_blendMap.quad);
    glBufferData (GL_ARRAY_BUFFER, sizeof (quad), quad, GL_STATIC_DRAW);

    const auto setMatrices = [this] (Effects::CPass* pass, const glm::mat4* projection) {
	static const glm::mat3 identityNormal (1.0f);
	pass->setModelViewProjectionMatrix (projection);
	pass->setModelViewProjectionMatrixInverse (&this->m_puppetClipIdentity);
	pass->setModelMatrix (&this->m_puppetClipIdentity);
	pass->setViewProjectionMatrix (&this->m_puppetClipIdentity);
	pass->setLightingTransform (&this->m_puppetClipIdentity, &identityNormal, &this->m_puppetClipIdentity);
	pass->setEffectTextureProjectionMatrix (&this->m_puppetClipIdentity, &this->m_puppetClipIdentity);
    };

    // fullscreenlayer.json copy, normal blending
    ComboMap copyCombos;

    if (texture->isAnimated ()) {
	copyCombos.emplace ("SPRITESHEET", 1);
    }

    const auto& copyConfig = *this->m_virtualPassess.emplace_back (
	std::make_unique<MaterialPass> (MaterialPass {
	    .blending = BlendingMode_Normal,
	    .cullmode = CullingMode_Disable,
	    .depthtest = DepthtestMode_Disabled,
	    .depthwrite = DepthwriteMode_Disabled,
	    .shader = "passthrough",
	    .textures = {},
	    .usertextures = {},
	    .combos = copyCombos,
	    .constants = {},
	})
    );

    auto* copy = new CPass (*this, fboProvider, copyConfig, std::nullopt, std::nullopt, std::nullopt);
    copy->setInput (texture);
    copy->setDestination (this->m_blendMap.albedo);
    setMatrices (copy, &this->m_puppetClipIdentity);
    copy->setGeometryCallback (
	[this, copy] () {
	    const GLint position = glGetAttribLocation (copy->getProgramID (), "a_Position");
	    const GLint texCoord = glGetAttribLocation (copy->getProgramID (), "a_TexCoord");

	    glBindBuffer (GL_ARRAY_BUFFER, this->m_blendMap.quad);

	    if (position >= 0) {
		glEnableVertexAttribArray (position);
		glVertexAttribPointer (position, 3, GL_FLOAT, GL_FALSE, sizeof (GLfloat) * 5, nullptr);
	    }

	    if (texCoord >= 0) {
		glEnableVertexAttribArray (texCoord);
		glVertexAttribPointer (
		    texCoord, 2, GL_FLOAT, GL_FALSE, sizeof (GLfloat) * 5,
		    reinterpret_cast<const void*> (sizeof (GLfloat) * 3)
		);
	    }
	},
	[] () { glDrawArrays (GL_TRIANGLE_STRIP, 0, 4); },
	[copy] () {
	    for (const char* name : { "a_Position", "a_TexCoord" }) {
		if (const GLint location = glGetAttribLocation (copy->getProgramID (), name); location >= 0) {
		    glDisableVertexAttribArray (location);
		}
	    }
	}
    );
    this->m_blendMap.copyPass = copy;

    this->m_materials.blendMapOverride = std::make_unique<ImageEffectPassOverride> (ImageEffectPassOverride {
	.id = -1,
	.combos = { { "BLENDROWCOUNT", static_cast<int> (this->m_blendMap.rows) } },
	.constants = {},
	.textures = {},
    });

    auto* pass = new CPass (
	*this, fboProvider, **this->m_materials.blendMap->passes.begin (), *this->m_materials.blendMapOverride,
	std::nullopt, std::nullopt
    );
    pass->setInput (texture);
    pass->setDestination (this->m_blendMap.albedo);
    pass->setKeepDestination (true);
    setMatrices (pass, &this->m_blendMap.projection);
    pass->setGeometryCallback (
	[this, pass] () {
	    const GLuint program = pass->getProgramID ();
	    const auto stride = static_cast<GLsizei> (this->m_blendMap.stride);
	    const auto attribute = [&] (const char* name, uint32_t bit, GLint size, bool integer) {
		const GLint location = glGetAttribLocation (program, name);
		const auto offset = CMesh::vertexComponentOffset (this->m_blendMap.format, bit);

		if (location < 0) {
		    return;
		}

		if (!offset.has_value ()) {
		    glDisableVertexAttribArray (location);
		    return;
		}

		const auto* pointer = reinterpret_cast<const void*> (static_cast<uintptr_t> (*offset));
		glEnableVertexAttribArray (location);

		if (integer) {
		    glVertexAttribIPointer (location, size, GL_UNSIGNED_INT, stride, pointer);
		} else {
		    glVertexAttribPointer (location, size, GL_FLOAT, GL_FALSE, stride, pointer);
		}
	    };

	    glBindBuffer (GL_ARRAY_BUFFER, this->m_blendMap.vertices);
	    attribute ("a_Position", 0x1, 3, false);
	    attribute ("a_BlendIndices", 0x800000, 4, true);
	    attribute ("a_TexCoordVec4", 0x20, 4, false);

	    if (const GLint location = glGetUniformLocation (program, "g_BlendMap");
		location >= 0 && this->m_blendMap.rows > 0) {
		glUniform4fv (location, static_cast<GLsizei> (this->m_blendMap.rows), this->m_rig.blendMap.data ());
	    }
	},
	[this] () {
	    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->m_blendMap.indices);
	    glDrawElements (GL_TRIANGLES, this->m_blendMap.indexCount, GL_UNSIGNED_SHORT, nullptr);
	},
	[pass] () {
	    for (const char* name : { "a_Position", "a_BlendIndices", "a_TexCoordVec4" }) {
		if (const GLint location = glGetAttribLocation (pass->getProgramID (), name); location >= 0) {
		    glDisableVertexAttribArray (location);
		}
	    }
	}
    );
    this->m_blendMap.pass = pass;

    this->m_passes.front ()->setTexture (0, this->m_blendMap.albedo);

    if (this->m_puppetClipTargetPass != nullptr && this->m_puppetMeshPass == this->m_passes.front ()) {
	this->m_puppetClipTargetPass->setTexture (0, this->m_blendMap.albedo);
    }
}

void CImage::selectPuppetDraw (const int draw) {
    if (draw < 0 || !this->m_puppetClipping.has_value ()
	|| static_cast<size_t> (draw) >= this->m_puppetClipping->draws.size ()) {
	this->m_puppetDrawBuffer = this->m_puppetIndices;
	this->m_puppetDrawOffset = 0;
	this->m_puppetDrawCount = draw < 0 ? this->m_puppetIndexCount : 0;
	return;
    }

    const auto& range = this->m_puppetClipping->draws[draw];
    this->m_puppetDrawBuffer = this->m_puppetClipIndices;
    this->m_puppetDrawOffset = static_cast<GLsizei> (range.offset);
    this->m_puppetDrawCount = static_cast<GLsizei> (range.count);
}

void CImage::renderPuppetClipMask (
    const Effects::CPass& meshPass, const int record, const int draw, const bool clear, const bool intermediate
) {
    auto& scene = this->getScene ();
    auto* pass = this->m_puppetClipMaskPasses[record];
    const bool inverted = this->m_puppetClipping->records[record].flags & PuppetClipping::Inverted;

    pass->copyBindings (meshPass);
    pass->setDestination (scene.requireAlphaMaskFrameBuffer (intermediate));
    // a mesh going onto the scene may be drawn into a passthrough layer, the mask has to be laid out the same way
    pass->setFollowLayerTarget (meshPass.getDestination () == scene.getFBO ());
    pass->setKeepDestination (!clear && !intermediate);
    this->m_puppetClipClearColor = glm::vec4 (inverted ? 1.0f : 0.0f);
    this->m_puppetClipRenderVar0.x = inverted ? 1.0f : 0.0f;

    this->selectPuppetDraw (draw);
    pass->render ();

    if (intermediate && this->m_puppetClipComposePass != nullptr) {
	this->m_puppetClipComposePass->render ();
    }
}

void CImage::renderPuppetClipped (Effects::CPass* meshPass) {
    const auto& commands = this->m_puppetClipping->commands;
    const auto& records = this->m_puppetClipping->records;
    auto* target = this->m_puppetClipTargetPass;
    int draw = 0;
    bool drawn = false;

    target->copyBindings (*meshPass);

    const auto drawMesh = [&] (Effects::CPass* pass, const int index) {
	this->selectPuppetDraw (index);
	this->m_puppetDrawKeep = drawn;
	pass->setKeepDestination (drawn);
	pass->render ();
	drawn = true;
    };

    const auto drawTargets = [&] (const int record, const int index) {
	target->setBlendingMode (
	    records[record].flags & PuppetClipping::Additive ? BlendingMode_Additive : BlendingMode_Translucent
	);
	drawMesh (target, index);
    };

    for (size_t i = 0; i < commands.size (); i++) {
	switch (commands[i]) {
	    case PuppetClipping::PlainDraw:
		drawMesh (meshPass, draw++);
		break;

	    case PuppetClipping::Mask:
		{
		    const int record = commands[++i];

		    if (static_cast<size_t> (record) < records.size ()) {
			this->renderPuppetClipMask (*meshPass, record, draw, true, false);
			drawTargets (record, draw + 1);
		    }

		    draw += 2;
		    break;
		}

	    case PuppetClipping::NestedMask:
		{
		    const int chain = commands[++i];

		    for (int link = 0; link < chain; link++) {
			const int parent = commands[++i];
			const int parentDraw = commands[++i];
			this->renderPuppetClipMask (*meshPass, parent, parentDraw, link == 0, link != 0);
		    }

		    const int record = commands[++i];
		    this->renderPuppetClipMask (*meshPass, record, draw, false, true);
		    drawTargets (record, draw + 1);
		    draw += 2;
		    break;
		}

	    default:
		break;
	}
    }

    meshPass->setKeepDestination (false);
    this->m_puppetDrawKeep = false;
    this->selectPuppetDraw (-1);
}

bool CImage::followsOutputSize () const {
    // a fullscreen layer takes its texture's size (sub_1401912C0), which for the usual one is _rt_FullFrameBuffer
    return this->getImage ().model->fullscreen && this->m_texture == this->getScene ().getFBO ();
}

void CImage::addEffectPasses (const ImageEffect& effect) {
    const auto fboProvider = std::make_shared<FBOProvider> (this);
    EffectBuffers buffers;

    for (const auto& fbo : effect.effect->fbos) {
	if (!fbo->conditions.holds (effect.combos)) {
	    continue;
	}

	const auto created = fboProvider->create (*fbo, this->getSize (), this->getScene ().isHDR ());

	if (this->followsOutputSize ()) {
	    this->getScene ().followOutputSize (created, *fbo);
	}

	buffers.emplace_back (fbo.get (), created);
    }

    this->registerEffectBuffers (effect, std::move (buffers));

    for (size_t passIndex = 0; passIndex < effect.effect->passes.size (); passIndex++) {
	auto& effectPass = *effect.effect->passes[passIndex];

	// a failed pass is left out, overrides still go by the effect's pass index
	if (!effectPass.conditions.holds (effect.combos)) {
	    continue;
	}

	if (!effectPass.material.has_value ()) {
	    if (!effectPass.command.has_value ()) {
		sLog.error ("Pass without material and command not supported");
		continue;
	    }

	    if (!effectPass.source.has_value ()) {
		sLog.error ("Pass without material and source not supported");
		continue;
	    }

	    if (!effectPass.target.has_value ()) {
		sLog.error ("Pass without material and target not supported");
		continue;
	    }

	    if (effectPass.command != Command_Copy) {
		sLog.error ("Only copy command is supported for pass without material");
		continue;
	    }

	    auto virtualPass = std::make_unique<MaterialPass> (MaterialPass { .blending = BlendingMode_Normal,
									      .cullmode = CullingMode_Disable,
									      .depthtest = DepthtestMode_Disabled,
									      .depthwrite = DepthwriteMode_Disabled,
									      .shader = "commands/copy",
									      .textures = { { 0, *effectPass.source } },
									      .combos = {},
									      .constants = {} });

	    const auto& config = *this->m_virtualPassess.emplace_back (std::move (virtualPass));

	    this->m_passes.push_back (
		new CPass (*this, fboProvider, config, std::nullopt, std::nullopt, effectPass.target.value ())
	    );
	    continue;
	}

	const auto override = passIndex < effect.passOverrides.size ()
	    ? std::optional<std::reference_wrapper<const ImageEffectPassOverride>> (*effect.passOverrides[passIndex])
	    : std::nullopt;
	const auto target = effectPass.target.has_value ()
	    ? std::optional<std::reference_wrapper<std::string>> (*effectPass.target)
	    : std::nullopt;

	for (auto& pass : effectPass.material.value ()->passes) {
	    this->m_passes.push_back (new CPass (*this, fboProvider, *pass, override, effectPass.binds, target));
	    this->registerEffectMaterial (effect, passIndex, this->m_passes.back ());
	}
    }
}

void CImage::setup () {
    if (this->m_initialized) {
	return;
    }

    this->m_readByOtherLayer = std::ranges::any_of (this->getScene ().getScene ().objects, [this] (const auto& object) {
	return object->id != this->getImage ().id
	    && std::ranges::find (object->dependencies, this->getImage ().id) != object->dependencies.end ();
    });
    this->m_emitterImageSource
	= std::ranges::any_of (this->getScene ().getScene ().objects, [this] (const auto& object) {
	      return std::ranges::any_of (object->componentDependencies, [this] (const auto& record) {
		  return record.type == "emitterimage" && record.id == this->getImage ().id;
	      });
	  });

    // setParent may add children later
    this->m_hasPassthroughChildren = this->m_image.model->passthrough
	&& (this->getScene ().scriptsMayReparent ()
	    || std::ranges::any_of (this->getScene ().getScene ().objects, [this] (const auto& object) {
		   return object->parent == this->getImage ().id;
	       }));

    // passthrough without effects has nothing to draw unless another layer reads its _rt_imageLayerComposite or it
    // has children to draw into its buffer
    if (this->m_image.model->passthrough && this->m_image.effects.empty () && !this->m_readByOtherLayer
	&& !this->m_hasPassthroughChildren) {
	return;
    }

    const auto& debug = this->getScene ().getContext ().getApp ().getContext ().settings.render.debug;

    // a shape has no image of its own, only its effect is drawn (sub_14025FAF0)
    if (!this->getImage ().shape) {
	for (const auto& cur : this->getImage ().model->material->passes) {
	    this->m_passes.push_back (
		new CPass (*this, std::make_shared<FBOProvider> (this), *cur, std::nullopt, std::nullopt, std::nullopt)
	    );
	}
    }

    std::vector<const DynamicValue*> passVisibility (this->m_passes.size (), nullptr);
    std::vector<bool> passFromEffect (this->m_passes.size (), false);

    if (!debug.baseOnly && !this->getImage ().effects.empty ()) {
	for (const auto& cur : this->m_image.effects) {
	    if (std::find (debug.skipEffects.begin (), debug.skipEffects.end (), static_cast<int> (cur->id))
		!= debug.skipEffects.end ()) {
		continue;
	    }

	    const auto effectVisibility
		= this->getScene ().getContext ().getApp ().getContext ().resolveEffectVisibility (
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

    if (!debug.baseOnly && !this->getImage ().shape) {
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
    // WE keeps the result of a layer another one reads in _a and only copies it to the screen from there,
    // drawing the last effect pass straight to the screen would leave _a one pass behind (or empty)
    const bool copyForReaders = this->copiesForReaders ();

    // fog sends every layer through its buffer and a FOG_COMPUTED composite (sub_1401E6F50, sub_1401EBBC0)
    const bool fog = this->getScene ().hasDistanceFog () || this->getScene ().hasHeightFog ();
    this->m_fogPass = nullptr;
    this->m_materials.colorBlendingPass = nullptr;

    // children go into the layer's buffer, which then needs a composite onto the scene even without effects
    // (sub_1401E8AA0 with children, sub_140208670)
    if (!debug.baseOnly && !this->getImage ().shape
	&& (colorBlendMode > 0 || copyForReaders || fog || this->m_hasPassthroughChildren)) {
	// scenes from version 3 on composite with genericimage4, the one with fog (sub_1401EBBC0)
	const auto& project = this->getScene ().getScene ().project;
	this->m_materials.colorBlending.material = MaterialParser::load (
	    project,
	    project.sceneVersion >= 3 ? "materials/util/effectpassthrough_4.json"
				      : "materials/util/effectpassthrough.json"
	);
	ComboMap combos;

	if (colorBlendMode > 0) {
	    combos.emplace ("BLENDMODE", colorBlendMode);
	}
	if (fog) {
	    combos.emplace ("FOG_COMPUTED", 1);
	}
	// draws the warped mesh over the flat texture in the buffer (sub_140208670)
	if (this->m_hasPuppetMesh) {
	    combos.merge (this->puppetVertexAlphaCombos ());
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
	this->m_materials.colorBlendingPass = this->m_passes.back ();
	// the layer buffer already holds color and alpha, WE draws this composite with a white renderer color
	// (sub_1401E8AA0 sets renderer +288..+300 to 1 before it)
	this->m_passes.back ()->setNeutralColor (true);

	if (fog) {
	    this->m_fogPass = this->m_passes.back ();
	}
    }

    if (this->m_hasPuppetMesh && !this->m_passes.empty ()) {
	this->m_puppetMeshPass = this->m_passes.front ();
	this->m_puppetMeshLast = this->m_passes.size () == 1;
	this->m_materials.puppetVertexAlpha = nullptr;

	if (this->m_puppetVertexAlpha) {
	    this->m_materials.puppetVertexAlpha = std::make_unique<ImageEffectPassOverride> (ImageEffectPassOverride {
		.id = -1,
		.combos = this->puppetVertexAlphaCombos (),
		.constants = {},
		.textures = {},
	    });
	}

	// effect masks are laid out over the source texture, so effects run on the flat texture first
	// and the warped mesh is drawn last, sampling their output
	const auto& materialPasses = this->getImage ().model->material->passes;
	const bool hasTrailingPasses = this->m_passes.size () != passCountBeforeTrailingPasses;
	const bool lit = this->hasLitMaterial ();

	if (hasTrailingPasses && this->m_passes.back () == this->m_materials.colorBlendingPass) {
	    this->m_puppetMeshPass = this->m_passes.back ();
	    this->m_puppetMeshLast = true;
	} else if (
	    this->m_passes.size () > 1 && !hasTrailingPasses && materialPasses.size () == 1
	    && (lit || materialPasses.front ()->constants.empty ())
	) {
	    // lit layer with effects (sub_140209540): first pass lights the flat texture, the mesh is drawn by the same
	    // material with LIGHTING/REFLECTION off and is the only pass with vertex alpha
	    const auto& base = *materialPasses.front ();
	    ShaderConstantMap constants;

	    for (const auto& [name, setting] : base.constants) {
		auto value = std::make_unique<DynamicValue> ();
		value->connect (setting->value.get ());
		value->setAnimation (setting->value->getAnimation ());
		constants.emplace (
		    name,
		    std::make_unique<UserSetting> (UserSetting {
			.value = std::move (value),
			.property = setting->property,
			.condition = setting->condition,
		    })
		);
	    }

	    const auto& config = *this->m_virtualPassess.emplace_back (
		std::make_unique<MaterialPass> (MaterialPass {
		    .blending = base.blending,
		    .cullmode = base.cullmode,
		    .depthtest = base.depthtest,
		    .depthwrite = base.depthwrite,
		    .shader = base.shader,
		    .textures = {},
		    .usertextures = {},
		    .combos = base.combos,
		    .constants = std::move (constants),
		})
	    );

	    if (lit) {
		auto combos = this->puppetVertexAlphaCombos ();
		combos.insert_or_assign ("LIGHTING", 0);
		combos.insert_or_assign ("REFLECTION", 0);
		this->m_materials.puppetVertexAlpha
		    = std::make_unique<ImageEffectPassOverride> (ImageEffectPassOverride {
			.id = -1,
			.combos = combos,
			.constants = {},
			.textures = {},
		    });
	    }

	    this->m_puppetMeshPass = this->m_materials.puppetVertexAlpha != nullptr
		? new CPass (
		      *this, std::make_shared<FBOProvider> (this), config, *this->m_materials.puppetVertexAlpha,
		      std::nullopt, std::nullopt
		  )
		: new CPass (
		      *this, std::make_shared<FBOProvider> (this), config, std::nullopt, std::nullopt, std::nullopt
		  );
	    this->m_passes.push_back (this->m_puppetMeshPass);
	    this->m_puppetMeshLast = true;
	} else if (this->m_materials.puppetVertexAlpha != nullptr) {
	    // first pass draws the mesh, rebuilt with the combos
	    auto* pass = new CPass (
		*this, std::make_shared<FBOProvider> (this), this->m_passes.front ()->getPass (),
		*this->m_materials.puppetVertexAlpha, std::nullopt, std::nullopt
	    );
	    delete this->m_passes.front ();
	    this->m_passes.front () = pass;
	    this->m_puppetMeshPass = pass;
	}
    }

    // WE sets MORPHING on any shader (sub_140209540), only shaders with MORPHING code use it
    this->m_puppetMorphShader = false;

    if (const auto& material = this->getImage ().model->material; material != nullptr && !material->passes.empty ()) {
	const auto pass = std::ranges::find_if (this->m_passes, [&material] (const CPass* candidate) {
	    return candidate->getPass ().shader == material->passes.front ()->shader
		&& candidate->getShader () != nullptr;
	});

	this->m_puppetMorphShader
	    = pass != this->m_passes.end () && (*pass)->getShader ()->getVertex ().refersTo ("MORPHING");
    }
    this->setupPuppetPrelight ();
    this->setupPuppetClipping ();
    // offscreen when a visible effect adds passes (sub_1401E7170)
    this->setupPuppetBlendMap (std::ranges::find (passFromEffect, true) != passFromEffect.end ());

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
    this->setupDirectLitPass ();
    this->m_setupInputs = this->currentSetupInputs ();
    this->m_initialized = true;
}

CImage::SetupInputs CImage::currentSetupInputs () const {
    SetupInputs inputs {
	.colorBlendMode = this->m_image.colorBlendMode->value->getInt (),
	.copyForReaders = this->copiesForReaders (),
    };

    if (this->m_hasPuppetMesh) {
	const auto& context = this->getScene ().getContext ().getApp ().getContext ();

	for (const auto& effect : this->m_image.effects) {
	    if (!context.resolveEffectVisibility (static_cast<int> (effect->id), effect->name).has_value ()) {
		inputs.puppetEffects.push_back (effect->visible->value->getBool ());
	    }
	}
    }

    return inputs;
}

void CImage::releasePasses () {
    this->releaseEffectMaterials ();

    for (auto* pass : this->m_allPasses.empty () ? this->m_passes : this->m_allPasses) {
	delete pass;
    }

    for (auto* pass : this->m_puppetClipMaskPasses) {
	delete pass;
    }

    delete this->m_puppetClipTargetPass;
    delete this->m_puppetClipComposePass;
    delete this->m_blendMap.copyPass;
    delete this->m_blendMap.pass;

    for (GLuint* buffer :
	 { &this->m_puppetClipComposePosition, &this->m_puppetClipComposeTexCoord, &this->m_blendMap.quad }) {
	if (*buffer != GL_NONE) {
	    glDeleteBuffers (1, buffer);
	    *buffer = GL_NONE;
	}
    }

    this->m_passes.clear ();
    this->m_allPasses.clear ();
    this->m_allPassStates.clear ();
    this->m_activePassMask.clear ();
    this->m_virtualPassess.clear ();
    this->m_puppetClipMaskPasses.clear ();
    this->m_puppetClipTargetPass = nullptr;
    this->m_puppetClipComposePass = nullptr;
    this->m_blendMap.copyPass = nullptr;
    this->m_blendMap.pass = nullptr;
    this->m_blendMap.albedo = nullptr;
    this->m_puppetPrelight.pass = nullptr;
    this->m_directLitPass = nullptr;
    this->m_materials.directLit = nullptr;
    this->m_puppetMeshPass = nullptr;
    this->m_puppetMeshLast = false;
    this->m_fogPass = nullptr;
    this->m_hasActiveEffectPass = false;
    this->m_materials.colorBlendingPass = nullptr;
    this->m_materials.compatibilityMaterials.clear ();
    this->m_materials.compatibilityOverrides.clear ();
    this->m_materials.clippingOverrides.clear ();
    this->m_initialized = false;
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
	this->m_allPasses[i]->setDepthState (std::nullopt);

	if (state.visible == nullptr || state.visible->getBool ()) {
	    this->m_activePassMask[i] = true;
	    this->m_hasActiveEffectPass |= state.fromEffect;
	    this->m_passes.push_back (this->m_allPasses[i]);
	}
    }

    // WE skips the children-only composite while an effect runs, the last effect pass draws instead (sub_1401EBF60)
    if (this->m_hasActiveEffectPass && this->m_hasPassthroughChildren && !this->m_passes.empty ()
	&& this->m_passes.back () == this->m_materials.colorBlendingPass
	&& this->m_image.colorBlendMode->value->getInt () == 0 && !this->copiesForReaders ()
	&& !this->getScene ().hasDistanceFog () && !this->getScene ().hasHeightFog ()) {
	this->m_activePassMask
	    [std::ranges::find (this->m_allPasses, this->m_passes.back ()) - this->m_allPasses.begin ()] = false;
	this->m_passes.pop_back ();
    }

    // a hidden layerimage source keeps its buffer but skips the composite (sub_1401D3AE0)
    if (this->m_emitterImageSource && !this->shouldRenderFinalPass (true) && this->m_passes.size () > 1
	&& this->m_passes.back () == this->m_materials.colorBlendingPass) {
	const auto index = std::ranges::find (this->m_allPasses, this->m_passes.back ()) - this->m_allPasses.begin ();
	this->m_activePassMask[index] = false;
	this->m_passes.pop_back ();
    }

    // if there's more than one pass the blendmode has to be moved from the beginning to the end
    if (this->m_passes.size () > 1) {
	const auto first = this->m_passes.begin ();
	const auto last = this->m_passes.rbegin ();

	(*last)->setBlendingMode ((*first)->getBlendingMode ());
	(*first)->setBlendingMode (BlendingMode_Normal);

	// the pass that draws an image's effects onto the scene also takes the image material's depth test and write
	// (sub_1401EBF60 through image slot 33 sub_140209160, material bytes +498/+499): a 3D layer behind a model
	// stays behind it (3453730450's moon_BBB)
	const auto& materialPasses = this->getImage ().model->material->passes;

	const auto lastState = std::ranges::find (this->m_allPasses, *last) - this->m_allPasses.begin ();

	if (!this->getImage ().shape && !materialPasses.empty () && this->m_allPassStates[lastState].fromEffect) {
	    const auto& base = *materialPasses.front ();
	    (*last)->setDepthState (std::make_pair (base.depthtest, base.depthwrite));
	}
    }

    // a shape's last pass goes onto the scene additively (its blend override sub_140260790 writes 2)
    if (this->getImage ().shape && !this->m_passes.empty ()) {
	this->m_passes.back ()->setBlendingMode (BlendingMode_Additive);
    }

    // passthrough with children and no other composite: slot 30 (sub_140208670) draws it with effectpassthrough or
    // fullscreenlayer.json, colour tints, alpha does nothing (live WE)
    if (this->m_materials.colorBlendingPass != nullptr && !this->m_passes.empty ()
	&& this->m_passes.back () == this->m_materials.colorBlendingPass) {
	const int colorBlendMode = this->m_image.colorBlendMode->value->getInt ();
	const bool childrenComposite = this->m_hasPassthroughChildren && !this->m_hasActiveEffectPass
	    && (colorBlendMode == 0 || colorBlendMode == 31) && !this->copiesForReaders ()
	    && !this->getScene ().hasDistanceFog () && !this->getScene ().hasHeightFog ();
	auto* composite = this->m_passes.back ();

	composite->setNeutralColor (!childrenComposite);

	if (childrenComposite) {
	    composite->setBlendingMode (
		colorBlendMode == 31 ? BlendingMode_Additive
		    : !this->m_image.copyBackground->value->getBool () || this->getImage ().model->fullscreen
		    ? BlendingMode_Translucent
		    : BlendingMode_Normal
	    );
	}
    }

    // setupPasses() ping-pongs these, every rebuild has to start from the same pair
    this->assignLayerBuffers ();
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
	pass->setLayerModelMatrix (&this->m_layerModelMatrix);
	pass->setEffectTextureProjectionMatrix (
	    &this->m_effectTextureProjection, &this->m_effectTextureProjectionInverse
	);

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
	    texcoord = isFirstPass ? this->m_texcoordDirect : this->m_texcoordFinal;
	    projection = &this->m_modelViewProjectionScreen;
	    // WE's final pass inverse lands in the layer's local space (origin at its center, unscaled
	    // pixels); older shaders like the bundled xray.vert unproject the pointer through it
	    inverseProjection = &this->m_objectSpaceProjectionInverse;
	}

	pass->setLightingTransform (
	    projection == &this->m_modelViewProjectionCopy ? &this->m_lightingCopyModel : &this->m_lightingSceneModel,
	    &this->m_lightingNormal, &this->m_lightingViewProjection
	);

	if (pass == this->m_puppetPrelight.pass) {
	    pass->setLightingTransform (
		&this->m_puppetPrelight.model, &this->m_puppetPrelight.normal, &this->m_lightingViewProjection
	    );
	}

	if (pass == this->m_directLitPass) {
	    pass->setModelMatrix (&this->m_lightingSceneModel);
	    pass->setViewProjectionMatrix (&this->m_lightingViewProjection);
	}

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
	pass->setModelViewProjectionMatrix (
	    pass == this->m_puppetPrelight.pass ? &this->m_puppetPrelight.projection : projection
	);
	pass->setModelViewProjectionMatrixInverse (inverseProjection);
	// what WE's intermediate passes see as g_EffectModelViewProjectionMatrix: their geometry where the layer
	// is on screen (sub_1401EBF60), the final pass keeps its own MVP
	pass->setEffectModelViewProjectionMatrix (
	    projection == &this->m_modelViewProjectionCopy       ? &this->m_effectModelViewProjectionCopy
		: projection == &this->m_modelViewProjectionPass ? &this->m_effectModelViewProjectionPass
								 : nullptr
	);

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

    if (this->currentSetupInputs () != this->m_setupInputs) {
	this->releasePasses ();
	this->setup ();

	if (!this->m_initialized) {
	    return;
	}
    }

    // the last pass only goes to the screen if the layer was visible when the passes were set up,
    // layers a script shows later (hidden in scene.json) need that redone
    if (this->effectVisibilityChanged () || this->shouldRenderFinalPass (true) != this->m_passesDrawToScreen) {
	this->rebuildActivePasses ();
    }

    // the lit pass may move into or out of the layer buffer
    if ((this->m_directLitPass != nullptr) != this->litDrawsDirect ()) {
	this->releasePasses ();
	this->setup ();

	if (!this->m_initialized) {
	    return;
	}
    }

    if (this->m_image.model->passthrough && !this->m_hasActiveEffectPass && !this->m_readByOtherLayer
	&& !this->m_hasPassthroughChildren) {
	return;
    }

    glColorMask (true, true, true, true);

    this->updateScreenSpacePosition ();

    if (this->m_hasPuppetMesh) {
	this->updatePuppetSkinning ();
    }

    if (this->m_blendMap.pass != nullptr) {
	this->m_blendMap.copyPass->render ();
	this->m_blendMap.pass->render ();
    }

#if !NDEBUG
    const std::string str = "Image " + this->getImage ().name + " (" + std::to_string (this->getId ()) + ", "
	+ this->getImage ().model->material->filename + ")";

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
	// the scene keeps its own alpha, but a hidden dependency's last pass fills the _a buffer other layers
	// read and needs its alpha (2938612768's audio cover drawn through the background's blend effects)
	if (std::next (cur) == end && this->m_passesDrawToScreen) {
	    glColorMask (true, true, true, false);
	}

	if (*cur == skippedBasePass && std::next (cur) != end) {
	    (*cur)->clearDestination ();
	} else if (*cur == this->m_puppetMeshPass && this->m_puppetClipTargetPass != nullptr) {
	    this->renderPuppetClipped (*cur);
	} else {
	    (*cur)->render ();
	}

	if (this->m_hasPassthroughChildren && *cur == this->m_allPasses.front () && std::next (cur) != end) {
	    this->renderPassthroughChildren ((*cur)->getDestination ());
	    // the children's final passes leave the scene's rgb only mask behind
	    glColorMask (true, true, true, true);
	}
    }

    // CParticle::render () never resets glColorMask, a disabled mask would leak into the next clear
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
    // a solid layer showing an image the user picked draws it untinted, its color only tints the plain
    // white fill (checked against WE with 3765586324's local file background, black and white color alike)
    const glm::vec3 color
	= this->showsUserTextureOnSolidLayer () ? glm::vec3 (1.0f) : this->m_image.color->value->getVec3 ();
    // the object's brightness only scales its color in HDR scene rendering (sub_140207740)
    m_colorCache = color * (this->getScene ().isHDR () ? this->m_image.brightness->value->getFloat () : 1.0f);
    return m_colorCache;
}

bool CImage::showsUserTextureOnSolidLayer () const {
    if (!this->m_image.model->solidlayer || this->m_image.model->material->passes.empty ()) {
	return false;
    }

    const auto& properties = this->getScene ().getScene ().project.properties;
    for (const auto& [index, propertyName] : (*this->m_image.model->material->passes.begin ())->usertextures) {
	const auto it = properties.find (propertyName);
	if (it != properties.end ()) {
	    it->second->pin ();
	}
	if (it != properties.end () && !it->second->is<Data::Model::PropertyUserShortcut> ()
	    && !it->second->getString ().empty ()) {
	    return true;
	}
    }

    return false;
}

const glm::vec4& CImage::getColor4 () const {
    // "version" 2 materials take color and alpha together through g_Color4
    m_color4Cache = glm::vec4 (this->getColor (), this->getAlpha ());
    return m_color4Cache;
}

const glm::vec3& CImage::getCompositeColor () const { return this->m_image.color->value->getVec3 (); }

glm::vec2 CImage::resolveGeometrySize () const {
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
    }

    return size;
}

void CImage::updateScenePosition (const glm::vec2& size) {
    glm::vec2 displaySize = size;
    const glm::vec2 declared = this->getImage ().size;
    const auto& model = *this->getImage ().model;

    if (!model.fullscreen && !model.autosize && !model.puppet.has_value () && declared.x > 0.0f && declared.y > 0.0f) {
	displaySize = declared;
    }

    this->m_displaySize = displaySize;

    // WE's quad (sub_1401EDE30) is centered on the object, size truncated to int; m_pos is y down
    const glm::vec2 half = (model.fullscreen ? displaySize : glm::trunc (displaySize)) / 2.0f;
    this->m_pos = { -half.x, half.y, half.x, -half.y };
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
    this->uploadSceneTexCoords ({ x, y, width, height });

    this->m_modelViewProjectionCopy = this->getImage ().model->passthrough
	? this->m_modelViewProjectionScreen
	: glm::ortho<float> (0.0, size.x, 0.0, size.y);
    this->m_modelViewProjectionCopyInverse = glm::inverse (this->m_modelViewProjectionCopy);
    this->m_modelMatrix = glm::ortho<float> (0.0, size.x, 0.0, size.y);
    // WE's buffer ortho has near -1000 / far 1000 (sub_14009A630), so the mesh keeps its z
    const glm::mat4 placement = glm::translate (glm::mat4 (1.0f), glm::vec3 (size / 2.0f, 0.0f));
    this->m_puppetPrelight.projection = this->getImage ().model->passthrough
	? this->m_modelViewProjectionCopy * glm::scale (placement, glm::vec3 (1.0f, -1.0f, 0.0f))
	: glm::ortho<float> (0.0, size.x, 0.0, size.y, -1000.0f, 1000.0f)
	    * glm::scale (placement, glm::vec3 (1.0f, -1.0f, 1.0f));
}

void CImage::updateGeometryBuffers () {
    const glm::vec2 size = this->resolveGeometrySize ();
    this->m_size = size;
    this->updateScenePosition (size);

    if (this->m_pos != this->m_lastUploadedPos || size != this->m_lastUploadedGeometrySize) {
	this->uploadGeometryBuffers (size);
	if (this->m_hasPuppetMesh) {
	    this->updatePuppetPositionBuffer (size);
	}
	this->m_lastUploadedPos = this->m_pos;
	this->m_lastUploadedGeometrySize = size;
    }
}

namespace {
// keeps an edge pair (e.g. m_pos.x/.z) from sliding into the on-screen part of the canvas once `offset` is added
// to both, so the image never uncovers ground it doesn't have pixels for; an image too small to cover it on this
// axis has no ground to uncover, it is an object sitting on the scene and moves freely. Canvas cropped away by the
// cover scaling is free to scroll in, WE has no clamp at all (sub_14018AAC0 adds the raw offset to the view)
float clampParallaxAxis (float offset, float edgeA, float edgeB, float visibleLow, float visibleHigh) {
    const float low = std::min (edgeA, edgeB);
    const float high = std::max (edgeA, edgeB);
    const float maxOffset = visibleLow - low;
    const float minOffset = visibleHigh - high;

    if (minOffset > maxOffset) {
	return offset;
    }

    // a layer that doesn't cover the screen at rest stays where the scene puts it (3621923790's hair), the clamp
    // only keeps the parallax from uncovering more
    return std::clamp (offset, std::min (minOffset, 0.0f), std::max (maxOffset, 0.0f));
}
} // namespace

void CImage::updateScreenSpacePosition () {
    this->updateGeometryBuffers ();

    // WE multiplies the world matrix into the model stack (sub_1401E8AA0), seen here from y down scene space
    const auto& scene = this->getScene ();
    const glm::mat4 flipY = glm::scale (glm::mat4 (1.0f), glm::vec3 (1.0f, -1.0f, 1.0f));
    const glm::mat4 toScene = flipY
	* glm::translate (glm::mat4 (1.0f), glm::vec3 (-scene.getWidth () / 2.0f, -scene.getHeight () / 2.0f, 0.0f));
    const glm::mat4 objectModel = this->worldMatrix () * flipY;
    glm::mat4 rotModel = toScene * objectModel;

    glm::mat4 mvp = this->getViewProjection () * rotModel;
    std::optional<std::pair<glm::mat4, glm::mat4>> objectTransform;
    const bool fullscreen = this->getImage ().model->fullscreen;
    const auto& camera = scene.getCamera ();

    // WE draws fullscreen layers with an identity transform, camera movement and parallax don't reach them
    if (fullscreen) {
	rotModel = glm::mat4 (1.0f);
	mvp = camera.getFullscreenProjection ();
    } else if (camera.isPerspective ()) {
	const glm::mat4 viewProjection = this->getImage ().perspective->value->getBool ()
	    ? camera.getPerspectiveLayerViewProjection ()
	    : scene.getWorldViewProjection ();

	mvp = viewProjection * objectModel;
	objectTransform.emplace (objectModel, viewProjection);
    }

    // CScene::renderFrame() already folds in disableparallax. Ortho scenes only (sub_14018AAC0), outside the rotation
    if (scene.getScene ().camera.parallax.enabled->value->getBool () && !fullscreen && !camera.isPerspective ()) {
	glm::vec2 offset = scene.getParallaxOffset (this->getImage ());

	// a texture that isn't UV-clamped tiles/repeats instead of showing black past its edges (GL_REPEAT,
	// see CTexture.cpp), so sliding it further is harmless and exempt from the clamp; scene.json's own
	// "clampuvs" overrides the base texture's flag the same way it does for the composite FBOs above
	const bool textureTiles = !this->getImage ().clampUVs && this->getTexture () != nullptr
	    && (this->getTexture ()->getFlags () & TextureFlags_ClampUVs) == 0;

	if (scene.getContext ().getApp ().getContext ().settings.mouse.clampParallaxToImageSize && !textureTiles) {
	    glm::vec2 low (std::numeric_limits<float>::max ());
	    glm::vec2 high (std::numeric_limits<float>::lowest ());

	    for (const glm::vec2 corner :
		 { glm::vec2 (this->m_pos.x, this->m_pos.y), glm::vec2 (this->m_pos.x, this->m_pos.w),
		   glm::vec2 (this->m_pos.z, this->m_pos.y), glm::vec2 (this->m_pos.z, this->m_pos.w) }) {
		const glm::vec2 point (rotModel * glm::vec4 (corner, 0.0f, 1.0f));
		low = glm::min (low, point);
		high = glm::max (high, point);
	    }

	    const glm::vec4 visible = scene.getVisibleCanvasRegion ();
	    offset.x = clampParallaxAxis (offset.x, low.x, high.x, visible.x, visible.y);
	    offset.y = clampParallaxAxis (offset.y, low.y, high.y, visible.z, visible.w);
	}

	rotModel = glm::translate (glm::mat4 (1.0f), { offset.x, offset.y, 0.0f }) * rotModel;
	mvp = this->getViewProjection () * rotModel;
    }

    this->updateLightingTransform (rotModel, objectTransform, mvp);

    // only the inverse is expensive; skip it when mvp didn't actually change
    if (mvp != this->m_modelViewProjectionScreen) {
	this->m_modelViewProjectionScreenInverse = glm::inverse (mvp);
    }
    this->m_modelViewProjectionScreen = mvp;
    this->m_layerModelMatrix = this->getScene ().objectWorldMatrix (this->getImage ());
    // buffer space (0..size, y = size at m_pos.y like the copy quad) onto the layer's scene quad, then the screen
    const glm::vec2 bufferSize = this->m_size;
    if (bufferSize.x > 0.0f && bufferSize.y > 0.0f) {
	glm::mat4 bufferToScene (1.0f);
	bufferToScene[0][0] = (this->m_pos.z - this->m_pos.x) / bufferSize.x;
	bufferToScene[1][1] = (this->m_pos.y - this->m_pos.w) / bufferSize.y;
	bufferToScene[3][0] = this->m_pos.x;
	bufferToScene[3][1] = this->m_pos.w;
	this->m_effectModelViewProjectionCopy = mvp * bufferToScene;
	// later passes draw WE's unit quad (-1..1), sub_1401EBF60 scales it by half the layer size first
	glm::mat4 unitToBuffer (1.0f);
	unitToBuffer[0][0] = bufferSize.x * 0.5f;
	unitToBuffer[1][1] = bufferSize.y * 0.5f;
	unitToBuffer[3][0] = bufferSize.x * 0.5f;
	unitToBuffer[3][1] = bufferSize.y * 0.5f;
	this->m_effectModelViewProjectionPass = this->m_effectModelViewProjectionCopy * unitToBuffer;
    }
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

void CImage::updateLightingTransform (
    const glm::mat4& sceneTransform, const std::optional<std::pair<glm::mat4, glm::mat4>>& objectTransform,
    const glm::mat4& screen
) {
    const auto width = static_cast<float> (this->getScene ().getWidth ());
    const auto height = static_cast<float> (this->getScene ().getHeight ());
    const glm::mat4 toWorld = glm::scale (
	glm::translate (glm::mat4 (1.0f), glm::vec3 (width / 2.0f, height / 2.0f, 0.0f)), glm::vec3 (1.0f, -1.0f, 1.0f)
    );
    const glm::mat3 flip = glm::mat3 (glm::scale (glm::mat4 (1.0f), glm::vec3 (1.0f, -1.0f, 1.0f)));

    // WE's normal matrix: model rows at unit length (sub_1400D8300)
    const auto inverseLength = [] (const float squared) {
	const float estimate = std::bit_cast<float> (0x5F375A86u - (std::bit_cast<uint32_t> (squared) >> 1));
	return (1.5f - squared * 0.5f * estimate * estimate) * estimate;
    };
    const glm::mat3 normal
	= objectTransform.has_value () ? glm::mat3 (this->worldMatrix ()) : flip * glm::mat3 (sceneTransform) * flip;

    if (objectTransform.has_value ()) {
	this->m_lightingSceneModel = objectTransform->first;
	this->m_lightingViewProjection = objectTransform->second;
    } else {
	this->m_lightingSceneModel = toWorld * sceneTransform;
	this->m_lightingViewProjection = this->getViewProjection () * glm::inverse (toWorld);
    }

    if (this->getImage ().model->fullscreen) {
	this->m_lightingViewProjection = screen * glm::inverse (this->m_lightingSceneModel);
    }

    for (int axis = 0; axis < 3; axis++) {
	this->m_lightingNormal[axis] = normal[axis] * inverseLength (glm::dot (normal[axis], normal[axis]));
    }

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

    // prelighting vertices are the puppet's own, moved by half the texture into the buffer (sub_140207B50)
    const glm::mat4 modelToCopy
	= glm::scale (glm::translate (glm::mat4 (1.0f), glm::vec3 (size / 2.0f, 0.0f)), glm::vec3 (1.0f, -1.0f, 1.0f));

    this->m_puppetPrelight.model = this->m_lightingCopyModel * modelToCopy;

    for (int axis = 0; axis < 3; axis++) {
	const glm::vec3 column (this->m_puppetPrelight.model[axis]);
	this->m_puppetPrelight.normal[axis] = column * inverseLength (glm::dot (column, column));
    }
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
    this->m_objectSpaceProjectionInverse = glm::scale (glm::mat4 (1.0f), glm::vec3 (size.x / 2.0f, size.y / 2.0f, 1.0f))
	* this->m_effectTextureProjectionInverse;
}

const Image& CImage::getImage () const { return this->m_image; }

void CImage::markAsDependency () { this->m_isDependency = true; }

bool CImage::copiesForReaders () const {
    return this->m_readByOtherLayer && (this->getImage ().visible->value->getBool () || this->m_emitterImageSource);
}

glm::vec2 CImage::getSize () const {
    if (this->m_texture == nullptr) {
	return this->getImage ().size;
    }

    // compose layers sample the whole scene, but effect masks map over the layer's own size
    if (this->getImage ().model->passthrough && this->getImage ().size.x > 0.0f && this->getImage ().size.y > 0.0f) {
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

// sub_1402066A0: texcoords 0.15 texel in from every edge, on the plain draw unless
// fullscreen/nopadding/passthrough/solidlayer and the last effect pass unless fullscreen/passthrough/solidlayer. Texel
// = the texture's storage size, else the object's size
void CImage::uploadSceneTexCoords (const glm::vec4& copy) {
    const auto& model = *this->getImage ().model;
    const auto texture = this->getTexture ();
    const glm::vec2 texel = texture != nullptr ? glm::vec2 (texture->getTextureWidth (0), texture->getTextureHeight (0))
					       : glm::trunc (this->getSize ());
    const glm::vec2 inset = 0.15000001f / glm::max (texel, glm::vec2 (1.0f));
    const bool excluded = model.fullscreen || model.passthrough || model.solidlayer;

    const auto upload = [&inset] (GLuint& buffer, glm::vec4 rect, const bool apply) {
	if (apply) {
	    rect += glm::vec4 (inset, -inset);
	}

	const GLfloat texcoords[]
	    = { rect.x, rect.w, rect.x, rect.y, rect.z, rect.w, rect.z, rect.w, rect.x, rect.y, rect.z, rect.y };

	if (buffer == GL_NONE) {
	    glGenBuffers (1, &buffer);
	}

	glBindBuffer (GL_ARRAY_BUFFER, buffer);
	glBufferData (GL_ARRAY_BUFFER, sizeof (texcoords), texcoords, GL_DYNAMIC_DRAW);
    };

    upload (this->m_texcoordDirect, copy, !excluded && !model.nopadding);
    upload (this->m_texcoordFinal, { 0.0f, 0.0f, 1.0f, 1.0f }, !excluded);
}
