#include "CImage.h"
#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Data/Parsers/ObjectParser.h"

#include "CRenderable.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <strings.h>
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
    for (int i = count - 2; i >= 0; --i) {
	ResolvedTransform local = localTransform (*chain[i]);

	// scene.json's "attachment" follows a named point on the direct parent's puppet rig (see
	// PuppetAttachmentPoint): WE's world matrix is parentWorld * (animated bone world * point local) * childLocal
	// (sub_140148A20), so the point's animated angle rotates the child's offset and adds to its own angle, and the
	// mesh turns around the object origin
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

		    if (!this->m_attachmentDiagnosticLogged.contains (chain[i]->id)) {
			this->m_attachmentDiagnosticLogged.insert (chain[i]->id);
			sLog.out (
			    "Attachment resolve for ", chain[i]->name, " (", chain[i]->id,
			    "): point=", *chain[i]->attachment, " meshPosition=(", meshTransform->position.x, ",",
			    meshTransform->position.y, ") boneAngleDeg=", glm::degrees (meshTransform->angle),
			    " boneScale=(", meshTransform->scale.x, ",", meshTransform->scale.y, ") parentOrigin=(",
			    resolved.origin.x, ",", resolved.origin.y, ") parentScale=", resolved.scale.x,
			    " anchorOrigin=(", anchorOrigin.x, ",", anchorOrigin.y,
			    ") anchorAngleDeg=", glm::degrees (anchorAngle)
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
	resolved = { local.origin, local.scale * resolved.scale, local.angle + anchorAngle };
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
    this->registerProperty ("alignment", *image.alignmentName->value);
    this->registerEffectConstants (image.effects);

    // animation layers carry their own property scripts, WE runs them with the layer as thisObject (the JSON loader
    // sub_1401730D0 binds them to the IAnimationLayer object, sub_14026C980 lists its properties and methods)
    for (size_t layerIndex = 0; layerIndex < image.animationLayers.size (); layerIndex++) {
	const auto& layer = image.animationLayers[layerIndex];
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

    auto scene_width = static_cast<float> (scene.getWidth ());
    auto scene_height = static_cast<float> (scene.getHeight ());

    const auto transform = this->resolveTransform (this->getImage ());
    glm::vec3 origin = transform.origin;
    glm::vec2 size = this->getSize ();
    glm::vec3 scale = transform.scale;

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

    this->m_currentMainFBO = this->m_mainFBO = mainFBO;
    this->m_currentSubFBO = this->m_subFBO = subFBO;

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
    if (this->m_puppetSpacePosition != GL_NONE) {
	glDeleteBuffers (1, &this->m_puppetSpacePosition);
    }
    if (this->m_puppetTexCoord != GL_NONE) {
	glDeleteBuffers (1, &this->m_puppetTexCoord);
    }
    if (this->m_puppetIndices != GL_NONE) {
	glDeleteBuffers (1, &this->m_puppetIndices);
    }
    for (const GLuint buffer :
	 { this->m_puppetClipIndices, this->m_puppetClipComposePosition, this->m_puppetClipComposeTexCoord }) {
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

	// the part ranges and clipping records follow the index buffer (sub_140261880)
	const int version
	    = puppetVersion.size () > strlen ("MDLV") ? std::atoi (puppetVersion.c_str () + strlen ("MDLV")) : 0;
	const size_t indexEnd = layout->block.headerOffset + meshHeaderSize + layout->block.vertexBytes
	    + sizeof (uint32_t) + layout->block.indexBytes;
	this->m_puppetClipping.reset ();
	this->m_puppetParts.clear ();
	this->m_puppetPartOrder.clear ();
	this->m_puppetMeshIndices.clear ();

	// the mesh header's first field holds its flags, 8 animates the order the parts are drawn in
	uint32_t meshFlags = 0;
	std::memcpy (&meshFlags, data.data () + layout->block.headerOffset, sizeof (meshFlags));

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

	    if (clipping.has_value () && clipping->build (mesh->indices)) {
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
	    } else if (clipping.has_value ()) {
		sLog.error (
		    "Puppet ", *this->getImage ().model->puppet, " has clipping masks drawn where their targets are, ",
		    "which aren't supported, drawing it without them"
		);
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
		this->m_rig.addSceneLayers (this->getImage ().animationLayers);
	    } catch (const std::exception& ex) {
		sLog.error (
		    "Could not load puppet skeleton/animation from ", *this->getImage ().model->puppet, ": ",
		    ex.what (), " (falling back to the static bind pose)"
		);
		this->m_rig.clear ();
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
	= !this->m_puppetSkinnedPositions.empty () ? this->m_puppetSkinnedPositions : this->m_puppetRawPositions;

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
	    "Puppet position bake for ", this->getImage ().name, " (", this->getId (),
	    "): bakeScenePosition=", bakeScenePosition, " passes=", this->m_passes.size (),
	    " vertexCount=", positions.size () / 3, " boundsMin=(", boundsMin.x, ",", boundsMin.y, ",", boundsMin.z,
	    ") boundsMax=(", boundsMax.x, ",", boundsMax.y, ",", boundsMax.z, ")"
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

    this->m_rig.updatePose (this->puppetObjectWorld ());
    this->updatePuppetDrawOrder ();
    this->m_rig.finishEndedLayers ([this] (size_t serial) {
	this->getScene ().getScriptEngine ().dispatchAnimationLayerEnded (*this, serial);
    });
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

	if (this->m_puppetClipping->build (this->m_puppetMeshIndices)) {
	    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->m_puppetClipIndices);
	    glBufferData (
		GL_ELEMENT_ARRAY_BUFFER, this->m_puppetClipping->indices.size () * sizeof (GLushort),
		this->m_puppetClipping->indices.data (), GL_DYNAMIC_DRAW
	    );
	}

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

    const size_t vertexCount = this->m_puppetRawPositions.size () / 3;
    this->m_puppetSkinnedPositions.assign (this->m_puppetRawPositions.size (), 0.0f);

    for (size_t v = 0; v < vertexCount; v++) {
	const glm::vec4 bindPos (
	    this->m_puppetRawPositions[v * 3], this->m_puppetRawPositions[v * 3 + 1],
	    this->m_puppetRawPositions[v * 3 + 2], 1.0f
	);

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

	    skinned += weight * glm::vec3 (skinMatrices[boneIndex] * bindPos);
	}

	this->m_puppetSkinnedPositions[v * 3] = skinned.x;
	this->m_puppetSkinnedPositions[v * 3 + 1] = skinned.y;
	this->m_puppetSkinnedPositions[v * 3 + 2] = skinned.z;
    }

    this->updatePuppetPositionBuffer (this->m_size);
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

std::optional<CImage::AttachmentPointTransform>
CImage::getAttachmentPointMeshTransform (const std::string& name) const {
    if (this->m_rig.boneModel.empty ()) {
	return std::nullopt;
    }

    const auto it = std::find_if (
	this->m_rig.attachmentPoints.begin (), this->m_rig.attachmentPoints.end (),
	[&name] (const PuppetAttachmentPoint& point) { return point.name == name; }
    );

    if (it == this->m_rig.attachmentPoints.end ()
	|| static_cast<size_t> (it->boneIndex) >= this->m_rig.boneModel.size ()) {
	return std::nullopt;
    }

    const glm::mat4 animatedWorld = this->m_rig.boneModel[it->boneIndex] * it->localTransform;

    const float angle = std::atan2 (animatedWorld[0][1], animatedWorld[0][0]);

    // scale.y = det(X,Y)/scale.x, projecting the transformed Y-basis onto what an unreflected
    // rotation by `angle` would have produced - comes out negative if the bone's matrix includes a
    // reflection (mirrored bone), instead of folding that into a bogus rotation angle
    const float scaleX = glm::length (glm::vec2 (animatedWorld[0]));
    const glm::vec2 scale = scaleX > 1e-6f
	? glm::vec2 (
	      scaleX, (animatedWorld[0][0] * animatedWorld[1][1] - animatedWorld[0][1] * animatedWorld[1][0]) / scaleX
	  )
	: glm::vec2 (scaleX, glm::length (glm::vec2 (animatedWorld[1])));

    return AttachmentPointTransform { .position = glm::vec3 (animatedWorld[3]), .angle = angle, .scale = scale };
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

    overrides.push_back (
	std::make_unique<ImageEffectPassOverride> (ImageEffectPassOverride {
	    .id = -1,
	    .combos = { { "CLIPPINGUVS", 1 }, { "CLIPPINGTARGET", 1 } },
	    .constants = {},
	    .textures = {},
	})
    );
    this->m_puppetClipTargetPass = new CPass (
	*this, fboProvider, this->m_puppetMeshPass->getPass (), *overrides.back (), std::nullopt, std::nullopt
    );
    this->m_puppetClipTargetPass->setTexture (8, this->getScene ().requireAlphaMaskFrameBuffer (false));
    this->setupPuppetGeometryCallback (this->m_puppetClipTargetPass);

    ComboMap maskCombos;
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
    std::vector<std::shared_ptr<CFBO>> buffers;

    for (const auto& fbo : effect.effect->fbos) {
	const auto created = fboProvider->create (
	    *fbo,
	    this->m_image.model->passthrough ? (this->m_texture->getFlags () | TextureFlags_ClampUVs)
					     : this->m_texture->getFlags (),
	    this->getSize ()
	);

	if (this->followsOutputSize ()) {
	    this->getScene ().followOutputSize (created, fbo->scale);
	}

	buffers.push_back (created);
    }

    this->registerEffectBuffers (effect, std::move (buffers));

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

	    this->m_passes.push_back (
		new CPass (*this, fboProvider, config, std::nullopt, std::nullopt, (*curEffect)->target.value ())
	    );
	} else {
	    for (auto& pass : (*curEffect)->material.value ()->passes) {
		const auto override = curOverride != endOverride
		    ? **curOverride
		    : std::optional<std::reference_wrapper<const ImageEffectPassOverride>> (std::nullopt);
		const auto target = (*curEffect)->target.has_value ()
		    ? *(*curEffect)->target
		    : std::optional<std::reference_wrapper<std::string>> (std::nullopt);

		this->m_passes.push_back (new CPass (*this, fboProvider, *pass, override, (*curEffect)->binds, target));
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

    this->m_readByOtherLayer = std::ranges::any_of (this->getScene ().getScene ().objects, [this] (const auto& object) {
	return object->id != this->getImage ().id
	    && std::ranges::find (object->dependencies, this->getImage ().id) != object->dependencies.end ();
    });

    this->m_hasPassthroughChildren = this->m_image.model->passthrough
	&& std::ranges::any_of (this->getScene ().getScene ().objects,
				[this] (const auto& object) { return object->parent == this->getImage ().id; });

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
    const bool copyForReaders = this->m_readByOtherLayer && this->getImage ().visible->value->getBool ();

    // fog sends every layer through its buffer and a FOG_COMPUTED composite (sub_1401E6F50, sub_1401EBBC0)
    const bool fog = this->getScene ().hasDistanceFog () || this->getScene ().hasHeightFog ();
    this->m_fogPass = nullptr;

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

	// effect masks are laid out over the source texture, so effects run on the flat texture first
	// and the warped mesh is drawn last, sampling their output
	const auto& materialPasses = this->getImage ().model->material->passes;
	const bool hasTrailingPasses = this->m_passes.size () != passCountBeforeTrailingPasses;

	if (this->m_passes.size () > 1 && !hasTrailingPasses && materialPasses.size () == 1
	    && materialPasses.front ()->constants.empty ()) {
	    const auto& base = *materialPasses.front ();
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
		    .constants = {},
		})
	    );

	    this->m_puppetMeshPass = new CPass (
		*this, std::make_shared<FBOProvider> (this), config, std::nullopt, std::nullopt, std::nullopt
	    );
	    this->m_passes.push_back (this->m_puppetMeshPass);
	    this->m_puppetMeshLast = true;
	}
    }

    this->setupPuppetClipping ();

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
	this->m_allPasses[i]->setDepthState (std::nullopt);

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

    // the last pass only goes to the screen if the layer was visible when the passes were set up,
    // layers a script shows later (hidden in scene.json) need that redone
    if (this->effectVisibilityChanged () || this->shouldRenderFinalPass (true) != this->m_passesDrawToScreen) {
	this->rebuildActivePasses ();
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
    glm::vec2 displaySize = size;
    const glm::vec2 declared = this->getImage ().size;
    const auto& model = *this->getImage ().model;

    if (!model.fullscreen && !model.autosize && !model.puppet.has_value () && declared.x > 0.0f && declared.y > 0.0f) {
	displaySize = declared;
    }

    const glm::vec2 scaledSize = displaySize * glm::vec2 (scale);
    this->m_displaySize = displaySize;
    this->m_pos.x = origin.x - (scaledSize.x / 2.0f);
    this->m_pos.w = origin.y + (scaledSize.y / 2.0f);
    this->m_pos.z = origin.x + (scaledSize.x / 2.0f);
    this->m_pos.y = origin.y - (scaledSize.y / 2.0f);

    const uint32_t alignment
	= Data::Parsers::ObjectParser::parseAlignment (this->getImage ().alignmentName->value->getString ());

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

    this->m_scenePivot = { origin.x - sceneWidth / 2.0f, sceneHeight / 2.0f - origin.y, 0.0f };
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

glm::mat4 CImage::ancestorTiltCorrection () const {
    // the quad above is placed with the parent chain folded in 2D (origin, scale, z angle). WE multiplies the full
    // world matrix into the model stack instead (sub_1401E8AA0), so parents' x/y angles tilt their children too:
    // map from the chain without those angles to the full one, in this centered y down space
    const auto& scene = this->getScene ();
    const auto localMatrix = [] (const Object& object, bool tilt) {
	glm::vec3 scale = object.groupScale->value->getVec3 ();
	glm::vec3 angles = object.groupAngles->value->getVec3 ();

	if (object.is<Image> ()) {
	    scale = object.as<Image> ()->scale->value->getVec3 ();
	    angles = object.as<Image> ()->angles->value->getVec3 ();
	} else if (object.is<Particle> ()) {
	    scale = object.as<Particle> ()->scale->value->getVec3 ();
	    angles = object.as<Particle> ()->angles->value->getVec3 ();
	} else if (object.is<Text> ()) {
	    scale = object.as<Text> ()->scale->value->getVec3 ();
	}

	glm::mat4 local = glm::translate (glm::mat4 (1.0f), object.origin->value->getVec3 ());
	local = glm::rotate (local, angles.z, glm::vec3 (0.0f, 0.0f, 1.0f));

	if (tilt) {
	    local = glm::rotate (local, angles.y, glm::vec3 (0.0f, 1.0f, 0.0f));
	    local = glm::rotate (local, angles.x, glm::vec3 (1.0f, 0.0f, 0.0f));
	}

	return std::pair { glm::scale (local, scale), angles.x != 0.0f || angles.y != 0.0f };
    };

    glm::mat4 full (1.0f);
    glm::mat4 flat (1.0f);
    bool tilted = false;
    const Object* current = &this->getImage ();

    for (int depth = 0; current->parent.has_value () && depth < 64; depth++) {
	// attachments follow a puppet bone, which the folded chain resolves on its own
	if (current->attachment.has_value ()) {
	    return glm::mat4 (1.0f);
	}

	const CObject* parent = scene.getObject (current->parent.value ());

	if (parent == nullptr) {
	    break;
	}

	current = &parent->getObject ();
	const auto [withTilt, hasTilt] = localMatrix (*current, true);
	full = withTilt * full;
	flat = localMatrix (*current, false).first * flat;
	tilted |= hasTilt;
    }

    if (!tilted) {
	return glm::mat4 (1.0f);
    }

    const glm::mat4 toScreen = glm::scale (glm::mat4 (1.0f), glm::vec3 (1.0f, -1.0f, 1.0f))
	* glm::translate (glm::mat4 (1.0f), glm::vec3 (-scene.getWidth () / 2.0f, -scene.getHeight () / 2.0f, 0.0f));

    return toScreen * full * glm::inverse (flat) * glm::inverse (toScreen);
}

void CImage::updateScreenSpacePosition () {
    const ResolvedTransform transform = this->updateGeometryBuffers ();

    // angles are already in radians from scene.json. WE's Rz * Ry * Rx seen through the y flip of this space is
    // Rz (-z) * Ry (y) * Rx (-x) (see CParticle.cpp). Only the object's own x/y angles, the parent chain folds z only
    const float angle = transform.angle;
    const glm::vec3 ownAngles = this->getImage ().angles->value->getVec3 ();
    // origin z only shows through a perspective camera, the ortho one keeps -2000..2000 like WE
    glm::mat4 rotModel = glm::translate (glm::mat4 (1.0f), glm::vec3 (0.0f, 0.0f, transform.origin.z));
    if (angle != 0.0f || ownAngles.x != 0.0f || ownAngles.y != 0.0f) {
	rotModel = glm::translate (rotModel, this->m_scenePivot);
	rotModel = glm::rotate (rotModel, -angle, glm::vec3 (0.0f, 0.0f, 1.0f));
	rotModel = glm::rotate (rotModel, ownAngles.y, glm::vec3 (0.0f, 1.0f, 0.0f));
	rotModel = glm::rotate (rotModel, -ownAngles.x, glm::vec3 (1.0f, 0.0f, 0.0f));
	rotModel = glm::translate (rotModel, -this->m_scenePivot);
    }

    rotModel = this->ancestorTiltCorrection () * rotModel;

    glm::mat4 mvp = this->getViewProjection () * rotModel;
    const bool fullscreen = this->getImage ().model->fullscreen;
    const auto& camera = this->getScene ().getCamera ();

    // WE draws fullscreen layers with an identity transform, camera movement and parallax don't reach them
    if (fullscreen) {
	mvp = camera.getFullscreenProjection ();
    } else if (camera.isPerspective () && !this->m_hasPuppetMesh) {
	// 3D scenes: the quad (size in world units) goes through the scene camera with the image's world matrix like
	// any other object (sub_1401E8AA0), "perspective" layers through their own camera. The vertices here are laid
	// out in the 2D scene space, this takes them back to the object's own space first. WE's quad is the declared
	// size (+752, sub_1402066A0) whatever the texture size, like the 2D layout
	const glm::vec2 size = this->m_displaySize;
	const glm::vec2 extent (this->m_pos.z - this->m_pos.x, this->m_pos.w - this->m_pos.y);

	if (extent.x != 0.0f && extent.y != 0.0f) {
	    const glm::vec3 center (
		(this->m_pos.x + this->m_pos.z) / 2.0f, (this->m_pos.y + this->m_pos.w) / 2.0f, 0.0f
	    );
	    // z keeps scale 1 (the quad is flat anyway), a passthrough layer inverts this matrix to draw its children
	    const glm::mat4 toLocal
		= glm::translate (glm::scale (glm::mat4 (1.0f), glm::vec3 (size / extent, 1.0f)), -center);
	    const glm::mat4 viewProjection = this->getImage ().perspective->value->getBool ()
		? camera.getPerspectiveLayerViewProjection ()
		: this->getScene ().getWorldViewProjection ();

	    mvp = viewProjection * this->worldMatrix () * toLocal;
	}
    }

    // CScene::renderFrame() already folds disableparallax into getParallaxDisplacement(). WE's camera parallax only
    // exists in orthographic scenes (scene flags & 0x108, sub_14018AAC0)
    if (this->getScene ().getScene ().camera.parallax.enabled->value->getBool () && !fullscreen
	&& !camera.isPerspective ()) {
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
	    const glm::vec4 visible = this->getScene ().getVisibleCanvasRegion ();
	    x = clampParallaxAxis (x, this->m_pos.x, this->m_pos.z, visible.x, visible.y);
	    y = clampParallaxAxis (y, this->m_pos.y, this->m_pos.w, visible.z, visible.w);
	}

	// WE translates the view (sub_14018AAC0), so its offset isn't turned by the layer's own rotation. The clamp's
	// correction is measured on the unrotated quad and stays inside the rotation
	rotModel = glm::translate (glm::mat4 (1.0f), { offset.x, offset.y, 0.0f }) * rotModel
	    * glm::translate (glm::mat4 (1.0f), { x - offset.x, y - offset.y, 0.0f });
	mvp = this->getViewProjection () * rotModel;
    }

    this->updateLightingTransform (rotModel);

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
    this->m_objectSpaceProjectionInverse = glm::scale (glm::mat4 (1.0f), glm::vec3 (size.x / 2.0f, size.y / 2.0f, 1.0f))
	* this->m_effectTextureProjectionInverse;
}

const Image& CImage::getImage () const { return this->m_image; }

void CImage::markAsDependency () { this->m_isDependency = true; }

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
