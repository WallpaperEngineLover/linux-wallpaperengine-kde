#include "CMesh.h"

#include <cstring>
#include <glm/gtc/matrix_transform.hpp>

#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Data/Parsers/MaterialParser.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Objects/CRenderable.h"
#include "WallpaperEngine/Render/Objects/Effects/CPass.h"

using namespace WallpaperEngine::Render::Objects;
using namespace WallpaperEngine::Data::Parsers;

namespace {
// vertex attributes in the order they're packed, bit in the mesh's format and size in bytes (sub_1401D9860)
struct VertexComponent {
    uint32_t bit;
    uint32_t size;
};

constexpr VertexComponent VERTEX_COMPONENTS[] = {
    { 0x1, 12 },	 { 0x10000, 16 }, { 0x2, 12 },	  { 0x4, 16 },	  { 0x800000, 16 }, { 0x1000000, 16 }, { 0x8, 8 },
    { 0x10, 12 },	 { 0x20, 16 },	  { 0x40, 8 },	  { 0x80, 12 },	  { 0x100, 16 },    { 0x200, 8 },      { 0x400, 12 },
    { 0x800, 16 },	 { 0x1000, 8 },	  { 0x2000, 12 }, { 0x4000, 16 }, { 0x20000, 8 },   { 0x40000, 12 },   { 0x80000, 16 },
    { 0x100000, 8 }, { 0x200000, 12 }, { 0x400000, 16 }, { 0x8000, 16 },
};

constexpr uint32_t FORMAT_POSITION = 0x1;
constexpr uint32_t FORMAT_POSITION4 = 0x10000;
constexpr uint32_t FORMAT_NORMAL = 0x2;
constexpr uint32_t FORMAT_TANGENT = 0x4;
constexpr uint32_t FORMAT_TEXCOORD2 = 0x8;
constexpr uint32_t FORMAT_TEXCOORD3 = 0x10;
constexpr uint32_t FORMAT_TEXCOORD4 = 0x20;
constexpr uint32_t FORMAT_BLENDINDICES = 0x800000;
constexpr uint32_t FORMAT_BLENDWEIGHTS = 0x1000000;

constexpr uint32_t MESH_FLAG_WIDE_INDICES = 0x1;

struct MdlMesh {
    std::string material;
    uint32_t flags = 0;
    uint32_t format = 0;
    uint32_t stride = 0;
    std::vector<char> vertices;
    std::vector<char> indices;
};

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

	std::vector<char> value (this->m_data.begin () + this->m_offset, this->m_data.begin () + this->m_offset + count);
	this->m_offset += count;
	return value;
    }

    void skip (size_t count) { this->m_offset += count; }

private:
    const std::vector<char>& m_data;
    size_t m_offset = 0;
};

std::vector<MdlMesh> readMdlMeshes (const std::vector<char>& data) {
    MdlReader reader (data);
    const std::string magic = reader.string ();

    if (!magic.starts_with ("MDLV") || magic.size () < 5) {
	throw std::runtime_error ("not a MDLV model");
    }

    const int version = std::stoi (magic.substr (4));
    const uint32_t defaultFormat = reader.u32 ();
    const uint32_t materialsPerMesh = reader.u32 ();
    const uint32_t meshCount = reader.u32 ();
    std::vector<MdlMesh> meshes;

    for (uint32_t index = 0; index < meshCount; index++) {
	MdlMesh mesh;

	for (uint32_t material = 0; material < materialsPerMesh; material++) {
	    const std::string name = reader.string ();

	    if (material == 0) {
		mesh.material = name;
	    }
	}

	mesh.format = defaultFormat;

	if (version >= 4) {
	    mesh.flags = reader.u32 ();

	    if (mesh.flags & 2) {
		reader.u32 ();
	    }

	    // bounding box
	    if (version >= 17) {
		reader.skip (sizeof (float) * 6);
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

	mesh.vertices = reader.bytes (reader.u32 ());
	mesh.indices = reader.bytes (reader.u32 ());

	const bool usable = mesh.stride != 0 && mesh.vertices.size () % mesh.stride == 0;

	if (!usable) {
	    sLog.error ("Skipping mesh ", index, " with vertex format ", mesh.format, ", its stride doesn't match its data");
	}

	// version 23 files (the 2.4 engine only reads up to 19) close every mesh with a small block: two flag bytes, an
	// optional sized blob and another sized blob. Static meshes write six zero bytes, puppets fill it
	if (version >= 23) {
	    const uint8_t first = reader.u8 ();
	    const uint8_t second = reader.u8 ();

	    if (first != 0) {
		if (usable) {
		    meshes.push_back (std::move (mesh));
		}

		if (index + 1 < meshCount) {
		    sLog.error ("Unknown mesh trailer in model, only the first ", index + 1, " meshes are read");
		}

		break;
	    }

	    if (second != 0) {
		reader.skip (reader.u32 ());
	    }

	    reader.skip (reader.u32 ());
	}

	if (usable) {
	    meshes.push_back (std::move (mesh));
	}
    }

    return meshes;
}
} // namespace

class CMesh::Part final : public CRenderable {
public:
    Part (CMesh& owner, const Material& material, MdlMesh mesh) :
	CObject (owner.getScene (), owner.getObject ()),
	CRenderable (owner.getScene (), owner.getObject (), material), m_owner (owner), m_mesh (std::move (mesh)) { }

    ~Part () override {
	for (const auto* pass : this->m_passes) {
	    delete pass;
	}

	for (const auto vao : this->m_vaos) {
	    glDeleteVertexArrays (1, &vao);
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
	    GL_STATIC_DRAW
	);

	GLint previousVAO = 0;
	glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &previousVAO);

	glGenBuffers (1, &this->m_ebo);

	const bool wide = (this->m_mesh.flags & MESH_FLAG_WIDE_INDICES) != 0;
	this->m_indexType = wide ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
	this->m_indexCount
	    = static_cast<GLsizei> (this->m_mesh.indices.size () / (wide ? sizeof (uint32_t) : sizeof (uint16_t)));

	this->m_fboProvider = std::make_shared<FBOProvider> (this);

	for (const auto& materialPass : this->m_material.passes) {
	    auto* pass = new Effects::CPass (*this, this->m_fboProvider, *materialPass, std::nullopt, std::nullopt, std::nullopt);

	    pass->setDestination (this->getScene ().getFBO ());
	    pass->setInput (this->getTexture ());
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
		    // WE is D3D: front faces wind clockwise on screen, and the scene buffer is drawn upside down here
		    glFrontFace (GL_CW);
		},
		[this] () { glDrawElements (GL_TRIANGLES, this->m_indexCount, this->m_indexType, nullptr); },
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
		this->m_mesh.indices.data (), GL_STATIC_DRAW
	    );
	}

	glBindVertexArray (previousVAO);

	// the GPU has them now
	this->m_mesh.vertices = {};
	this->m_mesh.indices = {};
    }

    void render () override {
	for (auto* pass : this->m_passes) {
	    pass->render ();
	}
    }

    [[nodiscard]] const float& getBrightness () const override { return this->m_one; }
    [[nodiscard]] const float& getUserAlpha () const override { return this->m_one; }
    [[nodiscard]] const float& getAlpha () const override { return this->m_one; }
    [[nodiscard]] const glm::vec3& getColor () const override { return this->m_white; }
    [[nodiscard]] const glm::vec4& getColor4 () const override { return this->m_white4; }
    [[nodiscard]] const glm::vec3& getCompositeColor () const override { return this->m_white; }

private:
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
		case FORMAT_POSITION4: name = "a_Position"; break;
		case FORMAT_NORMAL: name = "a_Normal"; break;
		case FORMAT_TANGENT: name = "a_Tangent4"; break;
		case FORMAT_BLENDINDICES: name = "a_BlendIndices"; break;
		case FORMAT_BLENDWEIGHTS: name = "a_BlendWeights"; break;
		case FORMAT_TEXCOORD2:
		case FORMAT_TEXCOORD3:
		case FORMAT_TEXCOORD4: name = "a_TexCoord"; break;
		default: break;
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
    std::shared_ptr<FBOProvider> m_fboProvider = nullptr;
    std::vector<Effects::CPass*> m_passes = {};
    std::vector<GLuint> m_vaos = {};
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
    CObject (scene, mesh), ScriptableObject (scene, mesh), m_mesh (mesh) { }

CMesh::~CMesh () = default;

void CMesh::setup () {
    const auto& project = this->getScene ().getScene ().project;
    const auto stream = project.assetLocator->read (this->m_mesh.model);
    const std::vector<char> data { std::istreambuf_iterator<char> (*stream), std::istreambuf_iterator<char> () };

    for (auto& mesh : readMdlMeshes (data)) {
	auto material = MaterialParser::load (project, mesh.material);

	if (material == nullptr || material->passes.empty ()) {
	    sLog.error ("Skipping a mesh of ", this->m_mesh.model, " without a usable material ", mesh.material);
	    continue;
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

	auto part = std::make_unique<Part> (*this, *material, std::move (mesh));
	this->m_materials.push_back (std::move (material));

	try {
	    part->setup ();
	} catch (const std::exception& e) {
	    sLog.error ("Cannot set up a mesh of ", this->m_mesh.model, ": ", e.what ());
	    continue;
	}

	this->m_parts.push_back (std::move (part));
    }

    if (this->m_parts.empty ()) {
	throw std::runtime_error ("no drawable meshes in " + this->m_mesh.model);
    }

    this->updateMatrices ();
}

void CMesh::render () {
    if (!this->m_mesh.groupVisible->value->getBool ()) {
	return;
    }

    this->updateMatrices ();

    for (const auto& part : this->m_parts) {
	part->render ();
    }
}

glm::mat4 CMesh::localTransform (const Object& object) const {
    const glm::vec3 angles = object.groupAngles->value->getVec3 ();

    // T * Rz * Ry * Rx * S, WE's row vector version of it is sub_140148A20 + sub_14016B7C0
    glm::mat4 transform = glm::translate (glm::mat4 (1.0f), object.origin->value->getVec3 ());
    transform = glm::rotate (transform, angles.z, glm::vec3 (0.0f, 0.0f, 1.0f));
    transform = glm::rotate (transform, angles.y, glm::vec3 (0.0f, 1.0f, 0.0f));
    transform = glm::rotate (transform, angles.x, glm::vec3 (1.0f, 0.0f, 0.0f));
    return glm::scale (transform, object.groupScale->value->getVec3 ());
}

void CMesh::updateMatrices () {
    glm::mat4 model = this->localTransform (this->m_mesh);
    const Object* current = &this->m_mesh;

    for (int depth = 0; current->parent.has_value () && depth < 64; depth++) {
	const auto* parent = this->getScene ().getObject (current->parent.value ());

	if (parent == nullptr) {
	    break;
	}

	current = &parent->getObject ();
	model = this->localTransform (*current) * model;
    }

    const auto& camera = this->getScene ().getCamera ();

    this->m_modelMatrix = model;
    this->m_normalMatrix = glm::mat3 (model);
    this->m_viewProjection = camera.getPerspective () * camera.getView ();
    this->m_modelViewProjection = this->m_viewProjection * model;
    this->m_modelViewProjectionInverse = glm::inverse (this->m_modelViewProjection);
    this->m_eyePosition = camera.getEye ();
}

const Mesh& CMesh::getMesh () const { return this->m_mesh; }

const glm::mat4& CMesh::getModelMatrix () const { return this->m_modelMatrix; }

const glm::mat3& CMesh::getNormalMatrix () const { return this->m_normalMatrix; }

const glm::mat4& CMesh::getViewProjectionMatrix () const { return this->m_viewProjection; }

const glm::mat4& CMesh::getModelViewProjectionMatrix () const { return this->m_modelViewProjection; }

const glm::mat4& CMesh::getModelViewProjectionMatrixInverse () const { return this->m_modelViewProjectionInverse; }

const glm::vec3& CMesh::getEyePosition () const { return this->m_eyePosition; }
