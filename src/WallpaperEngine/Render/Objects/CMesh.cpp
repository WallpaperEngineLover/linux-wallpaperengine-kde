#include "CMesh.h"

#include <bit>
#include <map>

#include <algorithm>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>

#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Data/Parsers/MaterialParser.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Objects/CRenderable.h"
#include "WallpaperEngine/Render/Objects/Effects/CPass.h"
#include "WallpaperEngine/Scripting/Adapters/ScriptableObjectAdapter.h"
#include "WallpaperEngine/Scripting/ScriptEngine.h"

using namespace WallpaperEngine::Render::Objects;
using namespace WallpaperEngine::Data::Parsers;

extern float g_Time;

namespace {
// vertex attributes in the order they're packed, bit in the mesh's format and size in bytes (sub_1401D9860)
struct VertexComponent {
    uint32_t bit;
    uint32_t size;
};

constexpr VertexComponent VERTEX_COMPONENTS[] = {
    { 0x1, 12 },       { 0x10000, 16 }, { 0x2000000, 12 }, { 0x2, 12 },     { 0x4, 16 },     { 0x800000, 16 },
    { 0x1000000, 16 }, { 0x8, 8 },      { 0x10, 12 },      { 0x20, 16 },    { 0x40, 8 },     { 0x80, 12 },
    { 0x100, 16 },     { 0x200, 8 },    { 0x400, 12 },     { 0x800, 16 },   { 0x1000, 8 },   { 0x2000, 12 },
    { 0x4000, 16 },    { 0x20000, 8 },  { 0x40000, 12 },   { 0x80000, 16 }, { 0x100000, 8 }, { 0x200000, 12 },
    { 0x400000, 16 },  { 0x8000, 16 },
};

constexpr uint32_t FORMAT_POSITION = 0x1;
constexpr uint32_t FORMAT_POSITION4 = 0x10000;
constexpr uint32_t FORMAT_POSITION_C1 = 0x2000000;
constexpr uint32_t FORMAT_COLOR = 0x8000;
constexpr uint32_t FORMAT_NORMAL = 0x2;
constexpr uint32_t FORMAT_TANGENT = 0x4;
constexpr uint32_t FORMAT_TEXCOORD2 = 0x8;
constexpr uint32_t FORMAT_TEXCOORD3 = 0x10;
constexpr uint32_t FORMAT_TEXCOORD4 = 0x20;
constexpr uint32_t FORMAT_BLENDINDICES = 0x800000;
constexpr uint32_t FORMAT_BLENDWEIGHTS = 0x1000000;

constexpr uint32_t MESH_FLAG_WIDE_INDICES = 0x1;
} // namespace

struct CMesh::MdlMesh {
    std::string material;
    /** every material name the file lists for this mesh, "skin" picks one */
    std::vector<std::string> materials;
    /** position in the file, what the morph targets and clips' morph tracks refer to */
    uint32_t index = 0;
    /** MDMP: the deltas' scale (g_MorphWeights[0]) and the targets packed into a texture (sub_1401D7760) */
    float morphScale = 0.0f;
    std::shared_ptr<const TextureProvider> morphTexture = nullptr;
    /** script model data can leave the index buffer out, the vertices are then drawn as a list */
    bool indexed = true;
    uint32_t flags = 0;
    uint32_t format = 0;
    uint32_t stride = 0;
    std::vector<char> vertices;
    std::vector<char> indices;
};

namespace {
using MdlMesh = CMesh::MdlMesh;

class MdlReader {
public:
    explicit MdlReader (const std::vector<char>& data) : m_data (data) { }

    [[nodiscard]] bool has (size_t bytes) const { return this->m_offset + bytes <= this->m_data.size (); }

    uint32_t u32 () {
	uint32_t value = 0;

	if (this->has (sizeof (value))) {
	    std::memcpy (&value, this->m_data.data () + this->m_offset, sizeof (value));
	}

	this->m_offset += sizeof (value);
	return value;
    }

    float f32 () {
	const uint32_t bits = this->u32 ();
	float value;

	std::memcpy (&value, &bits, sizeof (value));
	return value;
    }

    uint8_t u8 () {
	const uint8_t value = this->has (1) ? static_cast<uint8_t> (this->m_data[this->m_offset]) : 0;
	this->m_offset++;
	return value;
    }

    std::string string () {
	std::string value;

	while (this->has (1) && this->m_data[this->m_offset] != '\0') {
	    value += this->m_data[this->m_offset++];
	}

	this->m_offset++;
	return value;
    }

    std::vector<char> bytes (size_t count) {
	if (!this->has (count)) {
	    throw std::runtime_error ("truncated model");
	}

	std::vector<char> value (
	    this->m_data.begin () + this->m_offset, this->m_data.begin () + this->m_offset + count
	);
	this->m_offset += count;
	return value;
    }

    void skip (size_t count) { this->m_offset += count; }

    [[nodiscard]] size_t offset () const { return this->m_offset; }

    void seek (size_t offset) { this->m_offset = offset; }

    uint16_t u16 () {
	uint16_t value = 0;

	if (this->has (sizeof (value))) {
	    std::memcpy (&value, this->m_data.data () + this->m_offset, sizeof (value));
	}

	this->m_offset += sizeof (value);
	return value;
    }

private:
    const std::vector<char>& m_data;
    size_t m_offset = 0;
};

// the morph texture WE creates per mesh (format 19 = DXGI R16G16B16A16_SNORM, sub_1400D2A20), read by the vertex shader
// at texel centers
class MorphTexture final : public TextureProvider {
public:
    MorphTexture (int side, const std::vector<uint16_t>& halfs) : m_side (side), m_resolution (side, side, side, side) {
	GLint previous = 0;
	glGetIntegerv (GL_TEXTURE_BINDING_2D, &previous);
	glGenTextures (1, &this->m_texture);
	glBindTexture (GL_TEXTURE_2D, this->m_texture);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA16_SNORM, side, side, 0, GL_RGBA, GL_SHORT, halfs.data ());
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture (GL_TEXTURE_2D, previous);
    }

    ~MorphTexture () override { glDeleteTextures (1, &this->m_texture); }

    [[nodiscard]] GLuint getTextureID (uint32_t) const override { return this->m_texture; }
    [[nodiscard]] uint32_t getTextureWidth (uint32_t) const override { return this->m_side; }
    [[nodiscard]] uint32_t getTextureHeight (uint32_t) const override { return this->m_side; }
    [[nodiscard]] uint32_t getRealWidth () const override { return this->m_side; }
    [[nodiscard]] uint32_t getRealHeight () const override { return this->m_side; }
    [[nodiscard]] TextureFormat getFormat () const override { return TextureFormat_UNKNOWN; }
    [[nodiscard]] uint32_t getFlags () const override { return TextureFlags_ClampUVs; }
    [[nodiscard]] const std::vector<FrameSharedPtr>& getFrames () const override { return this->m_frames; }
    [[nodiscard]] const glm::vec4* getResolution () const override { return &this->m_resolution; }
    [[nodiscard]] bool isAnimated () const override { return false; }
    bool isReady () const override { return true; }
    void incrementUsageCount () const override { }
    void decrementUsageCount () const override { }
    void update () const override { }

private:
    GLuint m_texture = GL_NONE;
    uint32_t m_side;
    glm::vec4 m_resolution;
    std::vector<FrameSharedPtr> m_frames = {};
};

// bounds is the union of every mesh's box (sub_1402617C0), what the cursor hit test uses; files before version 17 have
// no boxes and leave it empty (min > max)
std::vector<MdlMesh> readMdlMeshes (
    const std::vector<char>& data, glm::vec3& boundsMin, glm::vec3& boundsMax, size_t& end, uint32_t& meshCount,
    std::vector<uint32_t>& meshFlags
) {
    MdlReader reader (data);
    const std::string magic = reader.string ();

    if (!magic.starts_with ("MDLV") || magic.size () < 5) {
	throw std::runtime_error ("not a MDLV model");
    }

    const int version = std::stoi (magic.substr (4));
    const uint32_t defaultFormat = reader.u32 ();
    const uint32_t materialsPerMesh = reader.u32 ();
    meshCount = reader.u32 ();
    std::vector<MdlMesh> meshes;

    boundsMin = glm::vec3 (std::numeric_limits<float>::max ());
    boundsMax = glm::vec3 (std::numeric_limits<float>::lowest ());

    for (uint32_t index = 0; index < meshCount; index++) {
	MdlMesh mesh;
	mesh.index = index;

	for (uint32_t material = 0; material < materialsPerMesh; material++) {
	    mesh.materials.push_back (reader.string ());
	}

	mesh.material = mesh.materials.empty () ? std::string () : mesh.materials.front ();

	mesh.format = defaultFormat;

	if (version >= 4) {
	    mesh.flags = reader.u32 ();

	    if (mesh.flags & 2) {
		reader.u32 ();
	    }

	    if (version >= 17) {
		glm::vec3 min;
		glm::vec3 max;

		for (int axis = 0; axis < 3; axis++) {
		    min[axis] = reader.f32 ();
		}
		for (int axis = 0; axis < 3; axis++) {
		    max[axis] = reader.f32 ();
		}

		boundsMin = glm::min (boundsMin, min);
		boundsMax = glm::max (boundsMax, max);
	    }

	    if (version >= 15) {
		mesh.format = reader.u32 ();
	    }
	}

	for (const auto& component : VERTEX_COMPONENTS) {
	    if (mesh.format & component.bit) {
		mesh.stride += component.size;
	    }
	}

	meshFlags.push_back (mesh.flags);
	mesh.vertices = reader.bytes (reader.u32 ());
	mesh.indices = reader.bytes (reader.u32 ());

	const bool usable = mesh.stride != 0 && mesh.vertices.size () % mesh.stride == 0;

	if (!usable) {
	    sLog.error (
		"Skipping mesh ", index, " with vertex format ", mesh.format, ", its stride doesn't match its data"
	    );
	}

	// sub_140261880 from version 21: a flag byte with a u32 and a sized blob, a flag byte with a sized blob
	// (puppets' per bone index ranges); from 23 a count of records (u64, string, u32 flags, two u32 counted u32
	// lists)
	if (version >= 21) {
	    if (reader.u8 () != 0) {
		reader.u32 ();
		reader.skip (reader.u32 ());
	    }

	    if (reader.u8 () != 0) {
		reader.skip (reader.u32 ());
	    }
	}

	if (version >= 23) {
	    const uint32_t records = reader.u32 ();

	    for (uint32_t record = 0; record < records && reader.has (1); record++) {
		reader.skip (sizeof (uint64_t));
		reader.string ();
		reader.u32 ();
		reader.skip (sizeof (uint32_t) * reader.u32 ());
		reader.skip (sizeof (uint32_t) * reader.u32 ());
	    }
	}

	if (usable) {
	    meshes.push_back (std::move (mesh));
	}
    }

    end = reader.offset ();
    return meshes;
}

// MDMP (sub_140261880): per mesh of the file a u16 target count, and with targets the deltas' scale, the vertex count
// and per target a u64, a name and blobs of 16 bit signed normalized values: positions (3 per vertex), normals with
// mesh flag 0x400, more with 0x800/0x1000, four values with 0x2000. sub_1401D7760 packs every target's deltas one after
// another into a square texture: positions only, or positions and normals interleaved per vertex; with 0x800 it stays
// empty. The shader scales them by g_MorphWeights[0]
void loadMorphTargets (
    const std::vector<char>& data, size_t section, const std::vector<uint32_t>& meshFlags, std::vector<MdlMesh>& meshes
) {
    MdlReader reader (data);
    reader.seek (section);
    reader.string ();
    reader.u32 ();

    for (uint32_t index = 0; index < meshFlags.size () && reader.has (2); index++) {
	const uint16_t targets = reader.u16 ();

	if (targets == 0) {
	    continue;
	}

	const uint32_t flags = meshFlags[index];
	const float scale = reader.f32 ();
	reader.u32 ();

	std::vector<std::vector<char>> positions;
	std::vector<std::vector<char>> normals;

	for (uint16_t target = 0; target < targets; target++) {
	    reader.skip (sizeof (uint64_t));
	    reader.string ();
	    positions.push_back (reader.bytes (reader.u32 ()));

	    if (flags & 0x400) {
		normals.push_back (reader.bytes (reader.u32 ()));
	    }
	    if (flags & 0x800) {
		reader.skip (reader.u32 ());
	    }
	    if (flags & 0x1000) {
		reader.skip (reader.u32 ());
	    }
	    if (flags & 0x2000) {
		reader.skip (sizeof (uint32_t) * 4);
	    }
	}

	const auto mesh = std::ranges::find (meshes, index, &MdlMesh::index);

	if (mesh == meshes.end () || mesh->stride == 0) {
	    continue;
	}

	const uint32_t vertices = static_cast<uint32_t> (mesh->vertices.size () / mesh->stride);
	const uint32_t perTarget = vertices * targets;
	uint32_t units = (flags & 0x400) ? perTarget * 2 : perTarget;

	if (flags & 0x800) {
	    units += perTarget;
	}

	const uint32_t texels = (3 * units) / 4 + ((3 * units) % 4 != 0 ? 1 : 0);
	auto side = static_cast<int> (std::sqrt (static_cast<float> (texels)));

	if (static_cast<uint32_t> (side * side) < texels) {
	    side++;
	}

	std::vector<uint16_t> halfs (static_cast<size_t> (side) * side * 4, 0);
	size_t out = 0;

	for (uint16_t target = 0; target < targets && (flags & 0x800) == 0; target++) {
	    const auto* position = reinterpret_cast<const uint16_t*> (positions[target].data ());
	    const size_t available = positions[target].size () / sizeof (uint16_t);

	    if ((flags & 0x400) == 0) {
		std::copy_n (
		    position, std::min<size_t> (available, 3 * vertices), halfs.begin () + static_cast<long> (out)
		);
		out += 3 * vertices;
		continue;
	    }

	    const auto* normal = reinterpret_cast<const uint16_t*> (normals[target].data ());
	    const size_t normalsAvailable = normals[target].size () / sizeof (uint16_t);

	    for (size_t i = 0; i + 2 < 3 * vertices && i + 2 < available && i + 2 < normalsAvailable; i += 3) {
		std::copy_n (position + i, 3, halfs.begin () + static_cast<long> (out + 2 * i));
		std::copy_n (normal + i, 3, halfs.begin () + static_cast<long> (out + 2 * i + 3));
	    }

	    out += 6 * vertices;
	}

	mesh->morphScale = scale;
	mesh->morphTexture = std::make_shared<MorphTexture> (side, halfs);
    }
}
} // namespace

class CMesh::Part final : public CRenderable {
public:
    Part (CMesh& owner, const Material& material, MdlMesh mesh, const ModelData::Mesh* source = nullptr) :
	CObject (owner.getScene (), owner.getObject ()), CRenderable (owner.getScene (), owner.getObject (), material),
	m_owner (owner), m_mesh (std::move (mesh)), m_source (source),
	m_sourceRevision (source != nullptr ? source->revision : 0) { }

    ~Part () override {
	for (const auto* pass : this->m_passes) {
	    delete pass;
	}

	for (const auto vao : this->m_vaos) {
	    glDeleteVertexArrays (1, &vao);
	}

	if (this->m_casterVAO != GL_NONE) {
	    glDeleteVertexArrays (1, &this->m_casterVAO);
	}

	glDeleteBuffers (1, &this->m_vbo);
	glDeleteBuffers (1, &this->m_ebo);
    }

    void setup () override {
	this->detectTexture ();

	// a model textured by a missing file still has a shape worth drawing
	if (this->m_texture == nullptr) {
	    this->m_texture = this->getContext ().resolveTexture ("util/white", this->getScene ().getScene ().project);
	}

	CRenderable::setup ();

	glGenBuffers (1, &this->m_vbo);
	glBindBuffer (GL_ARRAY_BUFFER, this->m_vbo);
	glBufferData (
	    GL_ARRAY_BUFFER, static_cast<GLsizeiptr> (this->m_mesh.vertices.size ()), this->m_mesh.vertices.data (),
	    this->m_source != nullptr && this->m_source->vertexDynamic ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW
	);
	this->m_vertexCount
	    = this->m_mesh.stride != 0 ? static_cast<GLsizei> (this->m_mesh.vertices.size () / this->m_mesh.stride) : 0;

	GLint previousVAO = 0;
	glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &previousVAO);

	glGenBuffers (1, &this->m_ebo);

	const bool wide = (this->m_mesh.flags & MESH_FLAG_WIDE_INDICES) != 0;
	this->m_indexType = wide ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
	this->m_indexCount
	    = static_cast<GLsizei> (this->m_mesh.indices.size () / (wide ? sizeof (uint32_t) : sizeof (uint16_t)));

	this->m_fboProvider = std::make_shared<FBOProvider> (this);

	// sub_140224C70: SKINNING when the model has bones and the mesh blend indices, BONECOUNT with it
	this->m_skinned = this->m_owner.getBoneCount () > 0 && (this->m_mesh.format & FORMAT_BLENDINDICES) != 0;
	this->m_skinning.combos = {
	    { "SKINNING", this->m_skinned ? 1 : 0 },
	    { "BONECOUNT", this->m_owner.getBoneCount () },
	};

	if (this->m_mesh.morphTexture != nullptr) {
	    this->m_skinning.combos["MORPHING"] = 1;
	}

	if (this->m_mesh.flags & 0x400) {
	    this->m_skinning.combos["MORPHING_NORMALS"] = 1;
	}

	for (const auto& materialPass : this->m_material.passes) {
	    std::optional<std::reference_wrapper<const ImageEffectPassOverride>> skinning = std::nullopt;

	    if (this->m_owner.getBoneCount () > 0) {
		skinning = std::cref (this->m_skinning);
	    }

	    auto* pass
		= new Effects::CPass (*this, this->m_fboProvider, *materialPass, skinning, std::nullopt, std::nullopt);

	    pass->setDestination (this->getScene ().getFBO ());
	    pass->setInput (this->getTexture ());

	    if (this->m_mesh.morphTexture != nullptr && this->m_owner.getBoneCount () > 0) {
		pass->setTexture (5, this->m_mesh.morphTexture);
	    }
	    pass->setModelMatrix (&this->m_owner.getModelMatrix ());
	    pass->setViewProjectionMatrix (&this->m_owner.getViewProjectionMatrix ());
	    pass->setModelViewProjectionMatrix (&this->m_owner.getModelViewProjectionMatrix ());
	    pass->setModelViewProjectionMatrixInverse (&this->m_owner.getModelViewProjectionMatrixInverse ());
	    pass->addUniform ("g_NormalModelMatrix", &this->m_owner.getNormalMatrix ());
	    pass->addUniform ("g_EyePosition", &this->m_owner.getEyePosition ());

	    // attribute locations differ between programs, so every pass gets its own vertex array
	    GLuint vao = GL_NONE;
	    glGenVertexArrays (1, &vao);
	    glBindVertexArray (vao);
	    glBindBuffer (GL_ARRAY_BUFFER, this->m_vbo);
	    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->m_ebo);
	    this->bindAttributes (pass->getProgramID ());
	    this->m_vaos.push_back (vao);

	    pass->setGeometryCallback (
		[this, vao] () {
		    glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &this->m_previousVAO);
		    glBindVertexArray (vao);
		    this->uploadBones ();

		    if (this->m_mesh.morphTexture != nullptr && this->m_owner.getBoneCount () > 0) {
			GLint program = 0;
			glGetIntegerv (GL_CURRENT_PROGRAM, &program);
			this->uploadMorphWeights (program);
		    }
		    // WE is D3D: front faces wind clockwise on screen, and the scene buffer is drawn upside down here
		    glFrontFace (GL_CW);
		},
		[this] () { this->draw (); },
		[this] () {
		    glFrontFace (GL_CCW);
		    glBindVertexArray (this->m_previousVAO);
		}
	    );

	    this->m_passes.push_back (pass);
	}

	// the element buffer is part of the vertex array state, fill it once one is bound
	if (!this->m_vaos.empty ()) {
	    glBindVertexArray (this->m_vaos.front ());
	    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->m_ebo);
	    glBufferData (
		GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr> (this->m_mesh.indices.size ()),
		this->m_mesh.indices.data (),
		this->m_source != nullptr && this->m_source->indexDynamic ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW
	    );
	}

	glBindVertexArray (previousVAO);

	// the GPU has them now
	this->m_mesh.vertices = {};
	this->m_mesh.indices = {};
    }

    void render () override {
	// applyData wrote new data into the model's buffers
	if (this->m_source != nullptr && this->m_source->revision != this->m_sourceRevision) {
	    this->m_sourceRevision = this->m_source->revision;
	    this->upload ();
	}

	for (auto* pass : this->m_passes) {
	    pass->render ();
	}
    }

    void renderShadowCaster (const GLuint program, const GLint alphaTest) {
	const BlendingMode blending
	    = this->m_material.passes.empty () ? BlendingMode_Normal : this->m_material.passes.front ()->blending;

	if (blending != BlendingMode_Normal && blending != BlendingMode_AlphaToCoverage) {
	    return;
	}

	if (this->m_casterVAO == GL_NONE) {
	    glGenVertexArrays (1, &this->m_casterVAO);
	    glBindVertexArray (this->m_casterVAO);
	    glBindBuffer (GL_ARRAY_BUFFER, this->m_vbo);
	    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->m_ebo);
	    this->bindAttributes (program);
	}

	const bool alphaToCoverage = blending == BlendingMode_AlphaToCoverage && this->getTexture () != nullptr;

	glUniform1i (alphaTest, alphaToCoverage ? 1 : 0);
	glUniform1i (glGetUniformLocation (program, "u_Skinning"), this->m_skinned ? 1 : 0);
	this->uploadBones ();

	// the caster gets the pass's MORPHING combos and its morph texture too (sub_140155FC0)
	const bool morphing = this->m_mesh.morphTexture != nullptr && this->m_owner.getBoneCount () > 0;

	glUniform1i (glGetUniformLocation (program, "u_Morphing"), morphing ? 1 : 0);
	glUniform1i (glGetUniformLocation (program, "u_MorphingNormals"), (this->m_mesh.flags & 0x400) != 0 ? 1 : 0);

	if (morphing) {
	    glUniform4fv (
		glGetUniformLocation (program, "g_Texture1Resolution"), 1,
		&this->m_mesh.morphTexture->getResolution ()->x
	    );
	    glActiveTexture (GL_TEXTURE1);
	    glBindTexture (GL_TEXTURE_2D, this->m_mesh.morphTexture->getTextureID (0));
	    glActiveTexture (GL_TEXTURE0);
	    this->uploadMorphWeights (static_cast<GLint> (program));
	}

	if (alphaToCoverage) {
	    glActiveTexture (GL_TEXTURE0);
	    glBindTexture (GL_TEXTURE_2D, this->getTexture ()->getTextureID (0));
	}

	glBindVertexArray (this->m_casterVAO);
	this->draw ();
    }

    [[nodiscard]] const float& getBrightness () const override { return this->m_one; }
    [[nodiscard]] const float& getUserAlpha () const override { return this->m_one; }
    [[nodiscard]] const float& getAlpha () const override { return this->m_one; }
    [[nodiscard]] const glm::vec3& getColor () const override { return this->m_white; }
    [[nodiscard]] const glm::vec4& getColor4 () const override { return this->m_white4; }
    [[nodiscard]] const glm::vec3& getCompositeColor () const override { return this->m_white; }

private:
    void uploadBones () {
	if (!this->m_skinned) {
	    return;
	}

	GLint program = 0;
	glGetIntegerv (GL_CURRENT_PROGRAM, &program);

	auto location = this->m_boneLocations.find (program);

	if (location == this->m_boneLocations.end ()) {
	    location = this->m_boneLocations.emplace (program, glGetUniformLocation (program, "g_Bones")).first;
	}

	if (location->second >= 0) {
	    glUniformMatrix4x3fv (
		location->second, this->m_owner.getBoneCount (), GL_FALSE, this->m_owner.getBones ().data ()
	    );
	}
    }

    // sub_1402222A0: the mesh's active targets in index order (at most 11), sorted by weight from the largest down
    // (stable), g_MorphOffsets = (count, vertex count * target, ...), g_MorphWeights = (the deltas' scale, weights...)
    void uploadMorphWeights (GLint program) const {
	std::vector<std::pair<uint32_t, float>> active;
	const auto& morphs = this->m_owner.getRig ().morphWeights;

	if (this->m_mesh.index < morphs.size ()) {
	    const auto& state = morphs[this->m_mesh.index];

	    for (uint32_t target = 0; target < state.weights.size () && target < 64 && active.size () < 11; target++) {
		if (state.active & (uint64_t (1) << target)) {
		    active.emplace_back (target, state.weights[target]);
		}
	    }
	}

	std::ranges::stable_sort (active, std::ranges::greater {}, &std::pair<uint32_t, float>::second);

	GLuint offsets[12] = {};
	GLfloat weights[12] = {};
	offsets[0] = static_cast<GLuint> (active.size ());
	weights[0] = this->m_mesh.morphScale;

	for (size_t i = 0; i < active.size (); i++) {
	    offsets[1 + i] = this->m_owner.getMorphVertexCount () * active[i].first;
	    weights[1 + i] = active[i].second;
	}

	glUniform1uiv (glGetUniformLocation (program, "g_MorphOffsets"), 12, offsets);
	glUniform1fv (glGetUniformLocation (program, "g_MorphWeights"), 12, weights);
    }

    void draw () const {
	if (this->m_mesh.indexed) {
	    glDrawElements (GL_TRIANGLES, this->m_indexCount, this->m_indexType, nullptr);
	} else {
	    glDrawArrays (GL_TRIANGLES, 0, this->m_vertexCount);
	}
    }

    void upload () const {
	const auto& vertices = this->m_source->vertices;
	const auto& indices = this->m_source->indices;

	glBindBuffer (GL_ARRAY_BUFFER, this->m_vbo);
	glBufferSubData (GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr> (vertices.size ()), vertices.data ());

	if (this->m_mesh.indexed && !this->m_vaos.empty ()) {
	    GLint previousVAO = 0;

	    glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &previousVAO);
	    glBindVertexArray (this->m_vaos.front ());
	    glBufferSubData (GL_ELEMENT_ARRAY_BUFFER, 0, static_cast<GLsizeiptr> (indices.size ()), indices.data ());
	    glBindVertexArray (previousVAO);
	}
    }

    void bindAttributes (const GLuint program) const {
	const auto stride = static_cast<GLsizei> (this->m_mesh.stride);
	uint32_t offset = 0;

	for (const auto& component : VERTEX_COMPONENTS) {
	    if ((this->m_mesh.format & component.bit) == 0) {
		continue;
	    }

	    const char* name = nullptr;
	    GLint elements = static_cast<GLint> (component.size / sizeof (float));

	    switch (component.bit) {
		case FORMAT_POSITION:
		case FORMAT_POSITION4:
		    name = "a_Position";
		    break;
		case FORMAT_POSITION_C1:
		    name = "a_PositionC1";
		    break;
		case FORMAT_COLOR:
		    name = "a_Color";
		    break;
		case FORMAT_NORMAL:
		    name = "a_Normal";
		    break;
		case FORMAT_TANGENT:
		    name = "a_Tangent4";
		    break;
		case FORMAT_BLENDINDICES:
		    name = "a_BlendIndices";
		    break;
		case FORMAT_BLENDWEIGHTS:
		    name = "a_BlendWeights";
		    break;
		case FORMAT_TEXCOORD2:
		case FORMAT_TEXCOORD3:
		case FORMAT_TEXCOORD4:
		    name = "a_TexCoord";
		    break;
		default:
		    break;
	    }

	    const GLint location = name != nullptr ? glGetAttribLocation (program, name) : -1;
	    const auto pointer = reinterpret_cast<const void*> (static_cast<uintptr_t> (offset));
	    offset += component.size;

	    if (location < 0) {
		continue;
	    }

	    glEnableVertexAttribArray (location);

	    if (component.bit == FORMAT_BLENDINDICES) {
		glVertexAttribIPointer (location, elements, GL_UNSIGNED_INT, stride, pointer);
	    } else {
		glVertexAttribPointer (location, elements, GL_FLOAT, GL_FALSE, stride, pointer);
	    }
	}
    }

    CMesh& m_owner;
    MdlMesh m_mesh;
    /** the SKINNING/BONECOUNT combos of models with a skeleton */
    ImageEffectPassOverride m_skinning {};
    bool m_skinned = false;
    std::map<GLint, GLint> m_boneLocations = {};
    /** script model data this part draws, re-uploaded when applyData changes it */
    const ModelData::Mesh* m_source = nullptr;
    uint64_t m_sourceRevision = 0;
    GLsizei m_vertexCount = 0;
    std::shared_ptr<FBOProvider> m_fboProvider = nullptr;
    std::vector<Effects::CPass*> m_passes = {};
    std::vector<GLuint> m_vaos = {};
    GLuint m_casterVAO = GL_NONE;
    GLuint m_vbo = GL_NONE;
    GLuint m_ebo = GL_NONE;
    GLint m_previousVAO = 0;
    GLsizei m_indexCount = 0;
    GLenum m_indexType = GL_UNSIGNED_SHORT;
    float m_one = 1.0f;
    glm::vec3 m_white = glm::vec3 (1.0f);
    glm::vec4 m_white4 = glm::vec4 (1.0f);
};

CMesh::CMesh (Wallpapers::CScene& scene, const Mesh& mesh) :
    CObject (scene, mesh), ScriptableObject (scene, mesh), m_mesh (mesh) {
    this->registerProperty ("castshadow", *mesh.castShadow->value);
    this->registerProperty ("rootmotion", *mesh.rootMotion->value);

    // animation layer property scripts run with the layer as thisObject, like an image's (sub_1402230C0)
    for (size_t layerIndex = 0; layerIndex < mesh.animationLayers.size (); layerIndex++) {
	const auto& layer = mesh.animationLayers[layerIndex];
	const std::string prefix = "animationlayers[" + std::to_string (layerIndex) + "].";

	for (const auto& [name, setting] :
	     { std::pair { "visible", &layer->visible }, std::pair { "rate", &layer->rate },
	       std::pair { "blend", &layer->blend } }) {
	    if (*setting == nullptr) {
		continue;
	    }

	    this->registerProperty (prefix + name, *(*setting)->value);
	    scene.getScriptEngine ().setThisObjectFactory (
		this->getProperties ().at (prefix + name).key, [this, layerIndex] (Scripting::ScriptEngine& engine) {
		    return Scripting::Adapters::makeAnimationLayerHandle (engine, *this, layerIndex);
		}
	    );
	}
    }
}

CMesh::~CMesh () {
    this->m_parts.clear ();

    if (this->m_modelData != nullptr) {
	this->getScene ().releaseModelData (this->m_modelData->token);
    }
}

void CMesh::setup () {
    // sub_14021AD10: a number is a script's model data, which the layer keeps a reference to
    if (this->m_mesh.modelData.has_value ()) {
	this->m_modelData = this->getScene ().acquireModelData (*this->m_mesh.modelData);

	if (this->m_modelData != nullptr) {
	    this->buildModelDataParts ();
	}

	this->updateMatrices ();
	return;
    }

    // "model" given as an object makes a model layer without a model
    if (this->m_mesh.model.empty ()) {
	this->updateMatrices ();
	return;
    }

    const auto& project = this->getScene ().getScene ().project;
    const auto stream = project.assetLocator->read (this->m_mesh.model);
    const std::vector<char> data { std::istreambuf_iterator<char> (*stream), std::istreambuf_iterator<char> () };

    size_t end = 0;
    uint32_t meshCount = 0;
    std::vector<uint32_t> meshFlags;
    auto meshes = readMdlMeshes (data, this->m_boundsMin, this->m_boundsMax, end, meshCount, meshFlags);

    // version 13+ files name the next section after the last mesh, a skeleton (MDLS) and its clips are loaded the same
    // way as a puppet's (sub_140261880)
    if (end + 4 <= data.size () && std::memcmp (data.data () + end, "MDLS", 4) == 0) {
	try {
	    this->m_rig.load (data, end, meshCount, this->m_mesh.model);
	    this->m_rig.addSceneLayers (this->m_mesh.animationLayers);
	} catch (const std::exception& e) {
	    sLog.error ("Could not load the skeleton of ", this->m_mesh.model, ": ", e.what ());
	    this->m_rig.clear ();
	}
    }

    // sub_140224C70: BONECOUNT is the bone count rounded up to a power of two, at least 16 and at most 128. Morph
    // targets only take effect on models with bones, their offsets all use the vertex count of the last mesh with a
    // morph texture (model +736)
    if (!this->m_rig.bones.empty ()) {
	if (this->m_rig.getMorphSection () != 0) {
	    loadMorphTargets (data, this->m_rig.getMorphSection (), meshFlags, meshes);
	}

	for (const auto& mesh : meshes) {
	    if (mesh.morphTexture != nullptr) {
		this->m_morphVertexCount = static_cast<uint32_t> (mesh.vertices.size () / mesh.stride);
	    }
	}

	const auto bones = static_cast<uint32_t> (std::max<size_t> (this->m_rig.bones.size (), 16));
	this->m_boneCount = static_cast<int> (std::min<uint32_t> (std::bit_ceil (bones), 128));
	this->updateBones ();
    }

    for (auto& mesh : meshes) {
	// "skin" picks one of the mesh's material names, the last one when it's past them
	if (!mesh.materials.empty ()) {
	    mesh.material = mesh.materials[std::min<size_t> (this->m_mesh.skin, mesh.materials.size () - 1)];
	}

	this->addPart (std::move (mesh), nullptr);
    }

    if (this->m_parts.empty ()) {
	throw std::runtime_error ("no drawable meshes in " + this->m_mesh.model);
    }

    this->updateMatrices ();
}

void CMesh::buildModelDataParts () {
    this->m_parts.clear ();
    this->m_materials.clear ();
    this->m_modelStructure = this->m_modelData->structure;
    this->m_boundsMin = this->m_modelData->boundsMin;
    this->m_boundsMax = this->m_modelData->boundsMax;

    for (const auto& source : this->m_modelData->meshes) {
	if (source.stride == 0 || source.vertices.empty ()) {
	    continue;
	}

	MdlMesh mesh;

	mesh.material = source.material;
	mesh.indexed = source.indexed;
	mesh.flags = source.wideIndices ? MESH_FLAG_WIDE_INDICES : 0;
	mesh.format = source.format;
	mesh.stride = source.stride;
	mesh.vertices = source.vertices;
	mesh.indices = source.indices;

	this->addPart (std::move (mesh), &source);
    }
}

void CMesh::addPart (MdlMesh mesh, const ModelData::Mesh* source) {
    const auto& project = this->getScene ().getScene ().project;
    const std::string name = source != nullptr ? "script model data" : this->m_mesh.model;
    auto material = MaterialParser::load (project, mesh.material);

    if (material == nullptr || material->passes.empty ()) {
	sLog.error ("Skipping a mesh of ", name, " without a usable material ", mesh.material);
	return;
    }

    // a model that doesn't say otherwise is solid, the church in 3777414605 relies on it
    for (const auto& pass : material->passes) {
	if (pass->depthtest == DepthtestMode_Unknown) {
	    pass->depthtest = DepthtestMode_Enabled;
	}

	if (pass->depthwrite == DepthwriteMode_Unknown) {
	    pass->depthwrite = DepthwriteMode_Enabled;
	}

	if (pass->cullmode == CullingMode_Unknown) {
	    pass->cullmode = CullingMode_Normal;
	}
    }

    auto part = std::make_unique<Part> (*this, *material, std::move (mesh), source);
    this->m_materials.push_back (std::move (material));

    try {
	part->setup ();
    } catch (const std::exception& e) {
	sLog.error ("Cannot set up a mesh of ", name, ": ", e.what ());
	return;
    }

    this->m_parts.push_back (std::move (part));
}

void CMesh::updateAnimation () {
    if (this->m_rig.bones.empty ()) {
	return;
    }

    // sub_14021C480 leaves hidden models alone, their layer clocks don't run either
    if (!this->m_mesh.groupVisible->value->getBool () || this->getScene ().isHiddenByAncestor (*this)) {
	this->m_rig.clockTime = g_Time;
	return;
    }

    this->m_rig.updatePose (this->getScene ().objectWorldMatrix (this->m_mesh), this);
    this->m_rig.finishEndedLayers ([this] (size_t serial) {
	this->getScene ().getScriptEngine ().dispatchAnimationLayerEnded (*this, serial);
    });
    this->updateBones ();
}

bool CMesh::rootMotionEnabled () const { return this->m_mesh.rootMotion->value->getBool (); }

glm::mat4 CMesh::rootMotionWorld () const { return this->getScene ().objectWorldMatrix (this->m_mesh); }

void CMesh::rootMotionMove (const glm::vec3& offset) {
    const glm::vec3 origin = this->m_mesh.origin->value->getVec3 () + offset;
    this->m_mesh.origin->value->update (origin, DynamicValue::UpdateSource::Script);
}

glm::vec3 CMesh::rootMotionAngles () const { return this->m_mesh.groupAngles->value->getVec3 (); }

void CMesh::rootMotionTurn (const glm::vec3& angles) {
    this->m_mesh.groupAngles->value->update (angles, DynamicValue::UpdateSource::Script);
}

void CMesh::updateBones () {
    // g_Bones is a float4x3 array (its own constant buffer in WE, sub_1402222A0): the first three columns of every skin
    // matrix, model space bone times inverse bind
    const auto skin = this->m_rig.skinMatrices ();
    const size_t count = std::min<size_t> (skin.size (), static_cast<size_t> (this->m_boneCount));

    this->m_bones.assign (static_cast<size_t> (this->m_boneCount) * 12, 0.0f);

    for (size_t bone = 0; bone < count; bone++) {
	for (int column = 0; column < 4; column++) {
	    for (int row = 0; row < 3; row++) {
		this->m_bones[bone * 12 + column * 3 + row] = skin[bone][column][row];
	    }
	}
    }
}

void CMesh::render () {
    if (!this->m_mesh.groupVisible->value->getBool ()) {
	return;
    }

    // replaceData changed the shapes (the model data's listeners, sub_1401D6400)
    if (this->m_modelData != nullptr && this->m_modelData->structure != this->m_modelStructure) {
	this->buildModelDataParts ();
    }

    this->updateMatrices ();

    for (const auto& part : this->m_parts) {
	part->render ();
    }
}

void CMesh::updateMatrices () {
    const auto& scene = this->getScene ();
    const auto& camera = scene.getCamera ();
    const glm::mat4 model = scene.objectWorldMatrix (this->m_mesh);

    this->m_modelMatrix = model;
    // WE scales each axis of the model matrix to unit length with a fast inverse square root (uniform setter
    // sub_1400D8300 case 0xE), so non-uniform scale doesn't stretch the normals and tangents (3734636606's floor)
    const auto inverseLength = [] (const float squared) {
	const float estimate = std::bit_cast<float> (0x5F375A86u - (std::bit_cast<uint32_t> (squared) >> 1));
	return (1.5f - squared * 0.5f * estimate * estimate) * estimate;
    };

    for (int axis = 0; axis < 3; axis++) {
	const glm::vec3 column (model[axis]);
	this->m_normalMatrix[axis] = column * inverseLength (glm::dot (column, column));
    }
    this->m_viewProjection = scene.getWorldViewProjection (this->m_mesh.perspective->value->getBool ());

    // orthographic scenes with camera parallax translate the view of every object, models too (sub_14018AAC0),
    // by the same offset images get; it's in the y down space, the view here is WE's y up world
    if (!camera.isPerspective () && scene.getScene ().camera.parallax.enabled->value->getBool ()) {
	const glm::vec2 offset = scene.getParallaxOffset (this->m_mesh);
	this->m_viewProjection = glm::translate (this->m_viewProjection, glm::vec3 (offset.x, -offset.y, 0.0f));
    }

    this->m_modelViewProjection = this->m_viewProjection * model;
    this->m_modelViewProjectionInverse = glm::inverse (this->m_modelViewProjection);

    if (camera.isPerspective ()) {
	this->m_eyePosition = camera.getEye ();
    } else {
	// the renderer eye like every pass gets it, in 2D scenes 2000 units out over the scene center plus the camera
	// (end of sub_1401891A0)
	this->m_eyePosition = scene.getFog ().eyeWorld;
    }
}

void CMesh::renderShadowCaster (
    const GLuint program, const GLint modelViewProjection, const GLint alphaTest, const glm::mat4& viewProjection,
    const glm::mat4& cullViewProjection
) {
    this->updateMatrices ();

    // perspective scenes (renderer +284 & 1) skip a shadow viewport whose frustum (sub_1401849E0: x, y and z against
    // w, planes normalized) has the model's bounding sphere fully outside one plane. The sphere spans the world
    // corners of the .mdl bounds (sub_1402222A0)
    if (this->getScene ().getCamera ().isPerspective () && this->m_boundsMax.x > this->m_boundsMin.x) {
	const glm::vec3 low (this->m_modelMatrix * glm::vec4 (this->m_boundsMin, 1.0f));
	const glm::vec3 high (this->m_modelMatrix * glm::vec4 (this->m_boundsMax, 1.0f));
	const glm::vec3 half = (high - low) * 0.5f;
	const glm::vec3 center = low + half;
	const float radius = glm::length (half);
	const glm::mat4& m = cullViewProjection;
	const glm::vec4 rows[4] = {
	    { m[0][0], m[1][0], m[2][0], m[3][0] },
	    { m[0][1], m[1][1], m[2][1], m[3][1] },
	    { m[0][2], m[1][2], m[2][2], m[3][2] },
	    { m[0][3], m[1][3], m[2][3], m[3][3] },
	};

	for (const glm::vec4& plane : { rows[3] + rows[0], rows[3] - rows[0], rows[3] + rows[1], rows[3] - rows[1],
					rows[3] + rows[2], rows[3] - rows[2] }) {
	    const glm::vec4 normalized = plane / glm::length (glm::vec3 (plane));

	    if (-radius > glm::dot (glm::vec3 (normalized), center) + normalized.w) {
		return;
	    }
	}
    }

    const glm::mat4 matrix = viewProjection * this->m_modelMatrix;
    glUniformMatrix4fv (modelViewProjection, 1, GL_FALSE, &matrix[0][0]);

    for (const auto& part : this->m_parts) {
	part->renderShadowCaster (program, alphaTest);
    }
}

bool CMesh::hitTest (const glm::vec2& ndc) const { return this->boxEntry (ndc).has_value (); }

glm::vec3 CMesh::cursorLocalPosition (const glm::vec2& ndc) const {
    const auto entry = this->boxEntry (ndc);

    return entry.has_value () ? entry.value () - (this->m_boundsMax - this->m_boundsMin) * 0.5f : glm::vec3 (0.0f);
}

std::optional<glm::vec3> CMesh::boxEntry (const glm::vec2& ndc) const {
    if (this->m_boundsMax.x <= this->m_boundsMin.x) {
	return std::nullopt;
    }

    const auto& scene = this->getScene ();
    const glm::mat4 toModel = glm::inverse (
	scene.getWorldViewProjection (this->m_mesh.perspective->value->getBool ())
	* scene.objectWorldMatrix (this->m_mesh)
    );
    const glm::vec4 nearPoint = toModel * glm::vec4 (ndc, -1.0f, 1.0f);
    const glm::vec4 farPoint = toModel * glm::vec4 (ndc, 1.0f, 1.0f);
    const glm::vec3 origin = glm::vec3 (nearPoint) / nearPoint.w;
    const glm::vec3 direction = glm::vec3 (farPoint) / farPoint.w - origin;
    const glm::vec3 extent = this->m_boundsMax - this->m_boundsMin;
    float enter = std::numeric_limits<float>::lowest ();
    float leave = std::numeric_limits<float>::max ();

    // slabs, no t >= 0 check: WE tests the whole line
    for (int axis = 0; axis < 3; axis++) {
	const float a = (0.0f - origin[axis]) / direction[axis];
	const float b = (extent[axis] - origin[axis]) / direction[axis];

	enter = std::max (enter, std::min (a, b));
	leave = std::min (leave, std::max (a, b));
    }

    if (leave < enter) {
	return std::nullopt;
    }

    return origin + direction * enter;
}

std::optional<glm::mat4> CMesh::getAttachmentMatrix (const std::string& name) const {
    return this->m_rig.attachmentMatrix (this->m_rig.findAttachment (name));
}

const Mesh& CMesh::getMesh () const { return this->m_mesh; }

const glm::mat4& CMesh::getModelMatrix () const { return this->m_modelMatrix; }

const glm::mat3& CMesh::getNormalMatrix () const { return this->m_normalMatrix; }

const glm::mat4& CMesh::getViewProjectionMatrix () const { return this->m_viewProjection; }

const glm::mat4& CMesh::getModelViewProjectionMatrix () const { return this->m_modelViewProjection; }

const glm::mat4& CMesh::getModelViewProjectionMatrixInverse () const { return this->m_modelViewProjectionInverse; }

const glm::vec3& CMesh::getEyePosition () const { return this->m_eyePosition; }
