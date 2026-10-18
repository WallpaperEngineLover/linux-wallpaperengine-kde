#include "PuppetMeshFile.h"

#include "CMesh.h"

#include <cstdlib>
#include <limits>
#include <memory>

#include <glm/glm.hpp>

#include "WallpaperEngine/Data/Utils/BinaryReader.h"
#include "WallpaperEngine/Data/Utils/MemoryStream.h"

using namespace WallpaperEngine::Render::Objects;
using namespace WallpaperEngine::Data::Utils;

namespace {
constexpr size_t MARKER_SIZE = 9;
constexpr size_t MESH_HEADER_SIZE = sizeof (uint32_t) * 2;
// the heuristic layout search only runs for a file whose header could not be read, and the first mesh's header sits in
// the first few hundred bytes. Without these limits a damaged file of a few dozen MB took minutes: every offset is
// tried as a mesh and every candidate with every stride
constexpr size_t MAX_HEADER_SCAN = 64 * 1024;
constexpr size_t MAX_LAYOUT_CANDIDATES = 64;
constexpr size_t MAX_LAYOUT_WORK = 16 * 1024 * 1024;

// offsets that could be an MDLV mesh header, stride resolved later
std::vector<PuppetMeshBlock> findPuppetMeshBlockCandidates (
    const BinaryReader& reader, size_t markerSize, size_t mdlsOffset, size_t meshHeaderSize
) {
    std::vector<PuppetMeshBlock> candidates;

    for (size_t offset = markerSize; offset + meshHeaderSize + sizeof (uint32_t) < mdlsOffset
	 && offset < MAX_HEADER_SCAN && candidates.size () < MAX_LAYOUT_CANDIDATES;
	 offset++) {
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

// UVs are always the last 8 bytes of a vertex
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

// blend indices/weights: the 32 bytes before the UVs (docs/rendering/MDL_FILES.md)
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

// lower is better, a wrong stride gives huge inconsistent triangles; nullopt when degenerate
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

// strides vary within one MDLV version, try them all and keep the most coherent mesh
std::optional<PuppetVertexLayout>
resolvePuppetVertexLayout (const BinaryReader& reader, size_t markerSize, size_t mdlsOffset, size_t meshHeaderSize) {
    constexpr size_t minVertexStride = 20; // position + uv
    constexpr size_t maxVertexStride = 256;
    constexpr size_t strideStep = 4;

    const auto candidates = findPuppetMeshBlockCandidates (reader, markerSize, mdlsOffset, meshHeaderSize);

    std::optional<PuppetVertexLayout> best;
    double bestScore = std::numeric_limits<double>::max ();
    size_t work = 0;

    for (const auto& block : candidates) {
	for (size_t stride = minVertexStride; stride <= maxVertexStride; stride += strideStep) {
	    if (block.vertexBytes % stride != 0) {
		continue;
	    }

	    work += block.vertexBytes / stride + block.indexBytes / sizeof (uint16_t);

	    if (work > MAX_LAYOUT_WORK) {
		return best;
	    }

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

struct PuppetMeshHeader {
    uint32_t flags = 0;
    uint32_t format = 0;
    size_t vertices = 0;
    uint32_t vertexBytes = 0;
    uint32_t indexBytes = 0;
};

// tag, default format, materials per mesh, mesh count; per mesh: materials, flags (+u32 with flag 2), bounds (v17+),
// format (v15+)
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

// sub_140261880 layout, falls back to the heuristic when UVs aren't last
std::optional<PuppetVertexLayout> readPuppetVertexLayout (const std::vector<char>& data, size_t meshHeaderSize) {
    const auto header = readPuppetMeshHeader (data);
    const uint32_t stride = CMesh::vertexStride (header.format);
    const auto texcoord = CMesh::vertexComponentOffset (header.format, 0x8);

    if (stride == 0 || header.vertexBytes % stride != 0 || !texcoord.has_value ()
	|| *texcoord != stride - sizeof (GLfloat) * 2
	|| header.vertices + header.vertexBytes + sizeof (uint32_t) + header.indexBytes > data.size ()) {
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
} // namespace

MdlvMeshes WallpaperEngine::Render::Objects::readMdlvMeshes (const std::vector<char>& data) {
    PuppetFileReader reader (data, 0);
    const std::string tag = reader.text ();

    if (!tag.starts_with ("MDLV") || tag.size () <= strlen ("MDLV")) {
	throw std::runtime_error ("not a MDLV model");
    }

    MdlvMeshes result;
    result.version = std::atoi (tag.c_str () + strlen ("MDLV"));

    const auto defaultFormat = reader.next<uint32_t> ();
    const auto materials = reader.next<uint32_t> ();
    const auto meshes = reader.next<uint32_t> ();

    for (uint32_t mesh = 0; mesh < meshes; mesh++) {
	MdlvMeshRecord record;
	record.format = defaultFormat;

	for (uint32_t material = 0; material < materials; material++) {
	    record.materials.push_back (reader.text ());
	}

	if (result.version >= 4) {
	    record.flags = reader.next<uint32_t> ();

	    if (record.flags & 2) {
		(void)reader.next<uint32_t> ();
	    }

	    if (result.version >= 17) {
		glm::vec3 min;
		glm::vec3 max;

		for (int axis = 0; axis < 3; axis++) {
		    min[axis] = reader.next<float> ();
		}

		for (int axis = 0; axis < 3; axis++) {
		    max[axis] = reader.next<float> ();
		}

		record.bounds = { min, max };
	    }

	    if (result.version >= 15) {
		record.format = reader.next<uint32_t> ();
	    }
	}

	record.vertices = reader.blob ();
	record.indices = reader.blob ();

	// v21+: flag + u32 + blob, flag + blob (per bone index ranges); v23+: records (u64, string, u32, two u32 lists)
	if (result.version >= 21) {
	    if (reader.next<uint8_t> () != 0) {
		(void)reader.next<uint32_t> ();
		(void)reader.blob ();
	    }

	    if (reader.next<uint8_t> () != 0) {
		(void)reader.blob ();
	    }
	}

	if (result.version >= 23) {
	    const auto records = reader.next<uint32_t> ();

	    for (uint32_t index = 0; index < records && reader.offset () < data.size (); index++) {
		reader.skip (sizeof (uint64_t));
		reader.string ();
		(void)reader.next<uint32_t> ();
		reader.skip (sizeof (uint32_t) * reader.next<uint32_t> ());
		reader.skip (sizeof (uint32_t) * reader.next<uint32_t> ());
	    }
	}

	result.meshes.push_back (std::move (record));
    }

    result.end = reader.offset ();
    return result;
}

std::optional<size_t>
WallpaperEngine::Render::Objects::findMdlSection (const std::vector<char>& data, size_t first, std::string_view tag) {
    constexpr size_t HEADER = MARKER_SIZE + sizeof (uint32_t);
    size_t offset = first;

    while (offset + HEADER <= data.size () && data[offset] != 0) {
	if (std::string_view (data.data () + offset, tag.size ()) == tag) {
	    return offset;
	}

	uint32_t next = 0;
	std::memcpy (&next, data.data () + offset + MARKER_SIZE, sizeof (next));

	if (next <= offset) {
	    break;
	}

	offset = next;
    }

    return std::nullopt;
}

size_t WallpaperEngine::Render::Objects::findPuppetSkeleton (const std::vector<char>& data) {
    // sections start where the meshes end, so "MDLS" inside mesh data never counts
    try {
	return findMdlSection (data, readMdlvMeshes (data).end, "MDLS").value_or (data.size ());
    } catch (const std::exception&) {
	// unknown mesh layout, search for the tag
    }

    for (size_t offset = MARKER_SIZE; offset + strlen ("MDLS") < data.size (); offset++) {
	if (std::memcmp (data.data () + offset, "MDLS", strlen ("MDLS")) == 0) {
	    return offset;
	}
    }

    return data.size ();
}

std::optional<PuppetMeshFile> WallpaperEngine::Render::Objects::readPuppetMeshFile (const std::vector<char>& data) {
    PuppetMeshFile file;

    file.tag = data.size () >= MARKER_SIZE ? std::string (data.data (), strlen ("MDLV0021")) : "";
    file.version = file.tag.size () > strlen ("MDLV") ? std::atoi (file.tag.c_str () + strlen ("MDLV")) : 0;
    file.skeleton = findPuppetSkeleton (data);

    try {
	file.sections = readMdlvMeshes (data).end;
    } catch (const std::exception&) {
	file.sections = file.skeleton;
    }

    // tag, flags, a second field, mesh count (sub_140261880)
    if (data.size () >= 21) {
	std::memcpy (&file.meshCount, data.data () + 17, sizeof (file.meshCount));
    }

    auto meshBuffer = std::make_unique<char[]> (data.size ());
    std::copy (data.begin (), data.end (), meshBuffer.get ());
    const BinaryReader reader (std::make_shared<MemoryStream> (std::move (meshBuffer), data.size ()));

    std::optional<PuppetVertexLayout> layout;

    try {
	layout = readPuppetVertexLayout (data, MESH_HEADER_SIZE);
    } catch (const std::exception& ex) {
	file.warnings.push_back (std::string ("could not read the first mesh: ") + ex.what ());
    }

    file.declaredLayout = layout.has_value ();

    if (!layout.has_value ()) {
	layout = resolvePuppetVertexLayout (reader, MARKER_SIZE, file.skeleton, MESH_HEADER_SIZE);
    }

    if (!layout.has_value ()) {
	return std::nullopt;
    }

    auto mesh = readPuppetMeshData (reader, layout->block, MESH_HEADER_SIZE, layout->vertexStride);

    if (!mesh.has_value ()) {
	return std::nullopt;
    }

    file.layout = *layout;
    file.positions = std::move (mesh->positions);
    file.texcoords = std::move (mesh->texcoords);
    file.indices = std::move (mesh->indices);
    file.indexEnd = layout->block.headerOffset + MESH_HEADER_SIZE + layout->block.vertexBytes + sizeof (uint32_t)
	+ layout->block.indexBytes;

    try {
	const auto header = readPuppetMeshHeader (data);

	if (header.vertices == layout->block.headerOffset + MESH_HEADER_SIZE) {
	    file.flags = header.flags;
	    file.format = header.format;
	} else {
	    file.warnings.emplace_back ("the first mesh header isn't where its vertices are");
	}
    } catch (const std::exception& ex) {
	file.warnings.push_back (std::string ("could not read the mesh header: ") + ex.what ());
    }

    const size_t vertexCount = layout->block.vertexBytes / layout->vertexStride;
    const size_t verticesOffset = layout->block.headerOffset + MESH_HEADER_SIZE;

    // v21+: flag, u32, one vec3 per vertex (a_PositionC1)
    if (file.version >= 21 && file.indexEnd < data.size () && data[file.indexEnd] != 0) {
	const size_t blob = file.indexEnd + 1 + sizeof (uint32_t);
	uint32_t bytes = 0;

	if (blob + sizeof (bytes) <= data.size ()) {
	    std::memcpy (&bytes, data.data () + blob, sizeof (bytes));
	}

	// WE ignores the size, always three floats per vertex
	if (bytes >= vertexCount * sizeof (float) * 3 && blob + sizeof (bytes) + bytes <= data.size ()) {
	    file.auxPositions.resize (vertexCount * 3);
	    std::memcpy (
		file.auxPositions.data (), data.data () + blob + sizeof (bytes), vertexCount * sizeof (float) * 3
	    );
	} else {
	    file.warnings.emplace_back (
		"the auxiliary vertex block is shorter than the mesh, lighting places the bind positions instead"
	    );
	}
    }

    const auto readComponent = [&] (uint32_t bit, size_t floats, std::vector<float>& out) {
	const auto offset = CMesh::vertexComponentOffset (file.format, bit);

	if (!offset.has_value () || *offset + floats * sizeof (float) > layout->vertexStride) {
	    return;
	}

	out.resize (vertexCount * floats);

	for (size_t vertex = 0; vertex < vertexCount; vertex++) {
	    std::memcpy (
		out.data () + vertex * floats, data.data () + verticesOffset + vertex * layout->vertexStride + *offset,
		floats * sizeof (float)
	    );
	}
    };

    readComponent (0x2, 3, file.normals);
    readComponent (0x4, 4, file.tangents);

    // position.w = morph texel, <= 0 = none
    if (file.format & 0x10000) {
	file.morphIndices.resize (vertexCount);

	for (size_t vertex = 0; vertex < vertexCount; vertex++) {
	    float w;
	    std::memcpy (
		&w, data.data () + verticesOffset + vertex * layout->vertexStride + sizeof (float) * 3, sizeof (w)
	    );

	    if (w > 0.0f) {
		file.morphIndices[vertex] = static_cast<uint32_t> (w);
	    }
	}
    }

    if (const auto blend = readPuppetBlendData (reader, layout->block, MESH_HEADER_SIZE, layout->vertexStride);
	blend.has_value ()) {
	file.blendIndices = blend->indices;
	file.blendWeights = blend->weights;
    }

    return file;
}
