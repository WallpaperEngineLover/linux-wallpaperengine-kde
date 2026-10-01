#include "ModelData.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

using namespace WallpaperEngine::Render::ModelData;

namespace {
// format bit and size of every vertex component, the tables at 0x140484A20 / 0x1404849B0 (sub_1400EA5B0)
constexpr std::pair<uint32_t, uint32_t> COMPONENTS[] = {
    { 0x1, 12 },       { 0x10000, 16 }, { 0x2000000, 12 }, { 0x2, 12 },     { 0x4, 16 },     { 0x800000, 16 },
    { 0x1000000, 16 }, { 0x8, 8 },      { 0x10, 12 },      { 0x20, 16 },    { 0x40, 8 },     { 0x80, 12 },
    { 0x100, 16 },     { 0x200, 8 },    { 0x400, 12 },     { 0x800, 16 },   { 0x1000, 8 },   { 0x2000, 12 },
    { 0x4000, 16 },    { 0x20000, 8 },  { 0x40000, 12 },   { 0x80000, 16 }, { 0x100000, 8 }, { 0x200000, 12 },
    { 0x400000, 16 },  { 0x8000, 16 },
};

uint32_t vertexStride (uint32_t format) {
    uint32_t stride = 0;

    for (const auto& [bit, size] : COMPONENTS) {
	if (format & bit) {
	    stride += size;
	}
    }

    return stride;
}

template <typename T> std::vector<char> bytesOf (const std::vector<T>& values) {
    std::vector<char> result (values.size () * sizeof (T));

    if (!values.empty ()) {
	std::memcpy (result.data (), values.data (), result.size ());
    }

    return result;
}

/** sub_1401C2F10: the mesh and its buffers from a shape, false (error set) when the data doesn't fit the format */
bool buildMesh (Mesh& mesh, const ShapeConfig& shape, Error& error) {
    mesh.format = shape.format;
    mesh.stride = vertexStride (shape.format);

    const auto bytes = static_cast<uint32_t> (shape.vertices.size () * sizeof (float));

    // WE divides by the stride unchecked, a format without components would crash it
    if (mesh.stride == 0 || bytes % mesh.stride != 0) {
	error = Error::InconsistentVertexBufferSize;
	return false;
    }

    mesh.vertexDynamic = shape.vertexDynamic;
    mesh.indexDynamic = shape.indexDynamic;
    mesh.vertexFloats = static_cast<uint32_t> (shape.vertices.size ());
    mesh.indexCount = shape.indexCount ();
    mesh.indexed = shape.hasIndices ();
    mesh.wideIndices = shape.hasIndices32;
    mesh.vertices = bytesOf (shape.vertices);
    mesh.indices = shape.hasIndices32 ? bytesOf (shape.indices32) : bytesOf (shape.indices16);
    mesh.revision++;
    return true;
}

// WE takes the bounds with fminf/fmaxf per component
glm::vec3 lower (const glm::vec3& a, const glm::vec3& b) {
    return { std::fmin (a.x, b.x), std::fmin (a.y, b.y), std::fmin (a.z, b.z) };
}

glm::vec3 upper (const glm::vec3& a, const glm::vec3& b) {
    return { std::fmax (a.x, b.x), std::fmax (a.y, b.y), std::fmax (a.z, b.z) };
}

/** Copies new data over a dynamic buffer, WE copies as many bytes as the buffer was created with */
void writeInPlace (std::vector<char>& target, const std::vector<char>& source) {
    std::memcpy (target.data (), source.data (), std::min (target.size (), source.size ()));
}
} // namespace

const char* WallpaperEngine::Render::ModelData::errorMessage (Error error) {
    // scenescript64 sub_1816361F0: index 0 is an empty string
    switch (error) {
	case Error::InvalidMaterial:
	    return "Invalid material";
	case Error::ShaderExpectingMoreVertexData:
	    return "Shader expecting more vertex data.";
	case Error::InconsistentVertexBufferSize:
	    return "Inconsistent vertex buffer size";
	case Error::InvalidToken:
	    return "Invalid model data token";
	case Error::VertexBufferSizeCannotIncrease:
	    return "Vertex buffer size cannot increase";
	case Error::IndexBufferSizeCannotIncrease:
	    return "Index buffer size cannot increase";
	case Error::NotCreatedAsDynamic:
	    return "Model data not created as dynamic";
	case Error::BufferLockFailed:
	    return "Buffer lock failed";
	case Error::IncorrectIndexBufferType:
	    return "Incorrect index buffer type";
	case Error::CannotAddShapes:
	    return "Cannot add shapes in IModelData.update";
	case Error::CannotDeleteShapes:
	    return "Cannot delete shape or buffers in IModelData.update";
	case Error::CannotChangeMaterial:
	    return "Material cannot be changed in IModelData.update";
	case Error::CannotChangeVertexFormat:
	    return "Vertex format cannot be changed in IModelData.update";
	case Error::None:
	    break;
    }

    return "";
}

uint32_t Store::create (const Config& config, const MaterialCheck& check, Error& error) {
    constexpr float MAX = std::numeric_limits<float>::max ();
    auto model = std::make_shared<Model> ();
    glm::vec3 boundsMin (MAX);
    glm::vec3 boundsMax (-MAX);

    error = Error::None;

    for (const auto& shape : config.shapes) {
	Mesh& mesh = model->meshes.emplace_back ();

	// a shape that doesn't fit still gets its material checked, the last error wins
	buildMesh (mesh, shape, error);

	if (const Error materialError = check (shape.material, mesh.format); materialError != Error::None) {
	    error = materialError;
	}

	mesh.material = shape.material;

	glm::vec3 meshMin (MAX);
	glm::vec3 meshMax (-MAX);

	if (config.boundsMin.has_value () && config.boundsMax.has_value ()) {
	    meshMin = *config.boundsMin;
	    meshMax = *config.boundsMax;
	} else {
	    // every three floats of the buffer count as a position, whatever the vertex format is
	    const auto& floats = shape.vertices;

	    for (size_t i = 0; i + 2 < floats.size (); i += 3) {
		meshMin = lower (meshMin, glm::vec3 (floats[i], floats[i + 1], floats[i + 2]));
		meshMax = upper (meshMax, glm::vec3 (floats[i], floats[i + 1], floats[i + 2]));
	    }
	}

	boundsMin = lower (boundsMin, meshMin);
	boundsMax = upper (boundsMax, meshMax);
    }

    if (error != Error::None) {
	return 0;
    }

    model->boundsMin = boundsMin;
    model->boundsMax = boundsMax;
    model->token = ++this->m_counter;
    this->m_models.emplace (model->token, model);

    return model->token;
}

void Store::apply (uint32_t token, const Config& config, bool replace, const MaterialCheck& check, Error& error) {
    error = Error::None;

    const auto found = this->m_models.find (token);

    if (found == this->m_models.end ()) {
	error = Error::InvalidToken;
	return;
    }

    Model& model = *found->second;
    auto& meshes = model.meshes;
    bool changed = false;
    bool completed = true;

    const auto notify = [&model] () { model.structure++; };

    if (!replace && config.shapes.size () > meshes.size ()) {
	error = Error::CannotAddShapes;
	return;
    }

    for (size_t index = 0; index < config.shapes.size (); index++) {
	const ShapeConfig& shape = config.shapes[index];

	if (shape.deleteShape || shape.deleteIndices) {
	    if (!replace) {
		error = Error::CannotDeleteShapes;
		completed = false;
		break;
	    }

	    // null shapes go after the loop
	    if (shape.deleteShape) {
		continue;
	    }
	}

	if (replace && index >= meshes.size ()) {
	    meshes.emplace_back ();
	}

	Mesh& mesh = meshes[index];

	if (!shape.material.empty () && shape.material != mesh.material) {
	    if (!replace) {
		error = Error::CannotChangeMaterial;
		completed = false;
		break;
	    }

	    changed = true;
	    mesh.material = shape.material;
	}

	const bool formatChanged = shape.format != 0 && mesh.format != shape.format;

	if (formatChanged && !replace) {
	    error = Error::CannotChangeVertexFormat;
	    completed = false;
	    break;
	}

	const bool vertices = shape.hasVertices;
	const bool indices = shape.hasIndices ();
	const bool vertexGrows = vertices && shape.vertices.size () > mesh.vertexFloats;
	const bool indexGrows = indices && shape.indexCount () > mesh.indexCount;
	bool rebuildVertices = vertexGrows || formatChanged;
	bool rebuildIndices = formatChanged || indexGrows;

	if (vertices && !mesh.vertexDynamic) {
	    rebuildVertices = true;
	}

	if (indices && !mesh.indexDynamic) {
	    rebuildIndices = true;
	}

	if (shape.deleteIndices && mesh.indexed) {
	    rebuildIndices = true;
	}

	if (rebuildVertices || rebuildIndices) {
	    // applyData checks this before the growth, so its own messages for growing buffers never come up
	    if (!replace) {
		error = Error::NotCreatedAsDynamic;
		completed = false;
		break;
	    }

	    changed = true;
	    buildMesh (mesh, shape, error);

	    if (!shape.material.empty ()) {
		if (const Error materialError = check (shape.material, mesh.format); materialError != Error::None) {
		    error = materialError;
		    notify ();
		    return;
		}
	    }

	    continue;
	}

	if (vertices) {
	    writeInPlace (mesh.vertices, bytesOf (shape.vertices));
	    mesh.revision++;
	}

	if (shape.hasIndices32 && mesh.wideIndices) {
	    writeInPlace (mesh.indices, bytesOf (shape.indices32));
	    mesh.revision++;
	} else if (shape.hasIndices16 && !mesh.wideIndices) {
	    writeInPlace (mesh.indices, bytesOf (shape.indices16));
	    mesh.revision++;
	} else if (indices) {
	    error = Error::IncorrectIndexBufferType;
	    completed = false;
	    break;
	}
    }

    if (completed) {
	for (size_t index = 0, mesh = 0; index < config.shapes.size (); index++, mesh++) {
	    if (config.shapes[index].deleteShape && mesh < meshes.size ()) {
		meshes.erase (meshes.begin () + static_cast<std::ptrdiff_t> (mesh));
		mesh--;
		changed = true;
	    }
	}
    }

    if (changed) {
	notify ();
    }
}

void Store::release (uint32_t token) {
    const auto found = this->m_models.find (token);

    if (found != this->m_models.end () && found->second->references-- == 1) {
	this->m_models.erase (found);
    }
}

std::shared_ptr<Model> Store::acquire (int token) {
    const auto found = this->m_models.find (static_cast<uint32_t> (token));

    if (found == this->m_models.end ()) {
	return nullptr;
    }

    found->second->references++;
    return found->second;
}
