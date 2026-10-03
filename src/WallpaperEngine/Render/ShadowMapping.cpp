#include "ShadowMapping.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "WallpaperEngine/Application/ApplicationContext.h"
#include "WallpaperEngine/Application/WallpaperApplication.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/CFBO.h"
#include "WallpaperEngine/Render/Objects/CMesh.h"
#include "WallpaperEngine/Render/RenderContext.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"

using namespace WallpaperEngine::Render;

namespace {
// assets/shaders/shadowcaster.vert/.frag. WE gives the caster material the mesh pass's values of the combos the caster
// shader declares (SKINNING, MORPHING, MORPHING_NORMALS, BONECOUNT) and binds the pass textures by their "material"
// name, so the "morph" texture lands in g_Texture1 (sub_140155FC0). Here one program switches them with u_Skinning,
// u_Morphing and u_MorphingNormals (g_Bones sized for the largest BONECOUNT, 128). With ALPHATOCOVERAGE the GLSL path
// of the fragment shader discards below 0.5
const char* kCasterVertex = R"(#version 330
uniform mat4 u_ModelViewProjection;
uniform bool u_Skinning;
uniform bool u_Morphing;
uniform bool u_MorphingNormals;
uniform mat4x3 g_Bones[128];
uniform sampler2D g_Texture1;
uniform vec4 g_Texture1Resolution;
uniform uint g_MorphOffsets[12];
uniform float g_MorphWeights[12];
in vec3 a_Position;
in vec2 a_TexCoord;
in uvec4 a_BlendIndices;
in vec4 a_BlendWeights;
out vec2 v_TexCoord;

vec3 morphDelta (uint morphMapOffset, vec2 resolutionInv) {
    vec2 offset = 0.5 * resolutionInv;

    if (u_MorphingNormals) {
	uint morphMapIndex = (morphMapOffset * 6u) / 4u;
	float morphMapFlip = float ((morphMapOffset * 6u) % 4u);
	uint morphPixel1x = morphMapIndex % uint (g_Texture1Resolution.x);
	uint morphPixel1y = morphMapIndex / uint (g_Texture1Resolution.y);
	uint morphPixel2x = (morphMapIndex + 1u) % uint (g_Texture1Resolution.x);
	uint morphPixel2y = (morphMapIndex + 1u) / uint (g_Texture1Resolution.y);
	vec4 morphCol1 = textureLod (g_Texture1, vec2 (morphPixel1x, morphPixel1y) * resolutionInv + offset, 0.0);
	vec4 morphCol2 = textureLod (g_Texture1, vec2 (morphPixel2x, morphPixel2y) * resolutionInv + offset, 0.0);
	vec3 posDeltaV1 = morphCol1.xyz;
	vec3 posDeltaV2 = vec3 (morphCol1.zw, morphCol2.x);
	return mix (posDeltaV1, posDeltaV2, step (1.0, morphMapFlip));
    }

    uint morphMapIndex = (morphMapOffset * 3u) / 4u;
    float morphMapFlip = float ((morphMapOffset * 3u) % 4u);
    uint morphPixel1x = morphMapIndex % uint (g_Texture1Resolution.x);
    uint morphPixel1y = morphMapIndex / uint (g_Texture1Resolution.y);
    uint morphPixel2x = (morphMapIndex + 1u) % uint (g_Texture1Resolution.x);
    uint morphPixel2y = (morphMapIndex + 1u) / uint (g_Texture1Resolution.y);
    vec4 morphCol1 = textureLod (g_Texture1, vec2 (morphPixel1x, morphPixel1y) * resolutionInv + offset, 0.0);
    vec4 morphCol2 = textureLod (g_Texture1, vec2 (morphPixel2x, morphPixel2y) * resolutionInv + offset, 0.0);
    vec3 posDeltaV1 = morphCol1.xyz;
    vec3 posDeltaV2 = vec3 (morphCol1.w, morphCol2.xy);
    vec3 posDeltaV3 = vec3 (morphCol1.zw, morphCol2.x);
    vec3 posDeltaV4 = morphCol1.yzw;
    return mix (posDeltaV1, mix (posDeltaV4, mix (posDeltaV3, posDeltaV2, step (2.5, morphMapFlip)),
				 step (1.5, morphMapFlip)), step (0.5, morphMapFlip));
}

void main () {
    vec3 position = a_Position;

    if (u_Morphing) {
	vec2 resolutionInv = 1.0 / g_Texture1Resolution.xy;
	vec3 morphPos = vec3 (0.0);

	for (uint morphTarget = 0u; morphTarget < g_MorphOffsets[0] % 12u; ++morphTarget) {
	    morphPos += morphDelta (uint (gl_VertexID) + g_MorphOffsets[1u + morphTarget], resolutionInv)
		* g_MorphWeights[1u + morphTarget];
	}

	position += morphPos * g_MorphWeights[0];
    }

    if (u_Skinning) {
	position = (g_Bones[a_BlendIndices.x] * a_BlendWeights.x + g_Bones[a_BlendIndices.y] * a_BlendWeights.y
		    + g_Bones[a_BlendIndices.z] * a_BlendWeights.z + g_Bones[a_BlendIndices.w] * a_BlendWeights.w)
	    * vec4 (position, 1.0);
    }

    v_TexCoord = a_TexCoord;
    gl_Position = u_ModelViewProjection * vec4 (position, 1.0);
}
)";

const char* kCasterFragment = R"(#version 330
uniform sampler2D g_Texture0;
uniform int u_AlphaTest;
in vec2 v_TexCoord;

void main () {
    if (u_AlphaTest != 0) {
	float alpha = texture (g_Texture0, v_TexCoord).a;
	alpha = (alpha - 0.5) / max (fwidth (alpha), 0.0001) + 0.5;

	if (alpha < 0.5) {
	    discard;
	}
    }
}
)";

GLuint compile (const GLenum type, const char* source) {
    const GLuint shader = glCreateShader (type);
    glShaderSource (shader, 1, &source, nullptr);
    glCompileShader (shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv (shader, GL_COMPILE_STATUS, &compiled);

    if (compiled == GL_FALSE) {
	char log[2048] = {};
	glGetShaderInfoLog (shader, sizeof (log) - 1, nullptr, log);
	sLog.error ("Shadow caster shader failed to compile: ", log);
    }

    return shader;
}

// WE's matrices put Direct3D's depth 0..1 into z and its viewport maps y = +1 to the first row of the target. The
// same memory layout in GL: y flipped and z moved to -w..w, the window depth is then WE's depth value
glm::mat4 toGL (const glm::mat4& direct3D) {
    glm::mat4 convert (1.0f);
    convert[1][1] = -1.0f;
    convert[2][2] = 2.0f;
    convert[3][2] = -1.0f;
    return convert * direct3D;
}
} // namespace

ShadowMapping::ShadowMapping (Wallpapers::CScene& scene, std::shared_ptr<const CFBO> atlas) :
    m_scene (scene), m_atlas (std::move (atlas)) { }

ShadowMapping::~ShadowMapping () {
    if (this->m_program != GL_NONE) {
	glDeleteProgram (this->m_program);
    }
}

int ShadowMapping::tileSize (const int quality) {
    switch (quality) {
	case 3:
	    return 512;
	case 4:
	    return 1024;
	default:
	    return 256;
    }
}

glm::mat4 ShadowMapping::perspective (const float fovRadians, const float aspect, const float near, const float far) {
    // sub_14009A360 is XMMatrixPerspectiveFovRH with near and far swapped: 1 at the near plane, 0 at the far one
    const float height = 1.0f / std::tan (fovRadians * 0.5f);
    glm::mat4 projection (0.0f);

    projection[0][0] = height / aspect;
    projection[1][1] = height;
    projection[2][2] = near / (far - near);
    projection[2][3] = -1.0f;
    projection[3][2] = far * near / (far - near);

    return projection;
}

void ShadowMapping::setup () {
    const GLuint vertex = compile (GL_VERTEX_SHADER, kCasterVertex);
    const GLuint fragment = compile (GL_FRAGMENT_SHADER, kCasterFragment);

    this->m_program = glCreateProgram ();
    glAttachShader (this->m_program, vertex);
    glAttachShader (this->m_program, fragment);
    glLinkProgram (this->m_program);
    glDeleteShader (vertex);
    glDeleteShader (fragment);

    this->m_modelViewProjection = glGetUniformLocation (this->m_program, "u_ModelViewProjection");
    this->m_alphaTest = glGetUniformLocation (this->m_program, "u_AlphaTest");
    glUseProgram (this->m_program);
    glUniform1i (glGetUniformLocation (this->m_program, "g_Texture0"), 0);
    glUniform1i (glGetUniformLocation (this->m_program, "g_Texture1"), 1);
}

glm::ivec2 ShadowMapping::pack (std::vector<Entry>& entries) {
    // sub_140190C80 0x1401936f8: rows of free space (x from, x to, y top, y bottom), the first one 8192 wide. A tile
    // goes into the first row with room for it, which then also leaves a column below it as a row of its own. Every
    // row it passes without room makes a new full width row below that one
    struct Row {
	uint16_t x0;
	uint16_t x1;
	uint16_t y0;
	uint16_t y1;
    };
    std::vector<Row> rows = { { 0, 0x2000, 0, 0 } };
    int width = 0;
    int height = 0;

    for (auto& entry : entries) {
	const int size = entry.size;

	for (size_t i = 0; i < rows.size (); i++) {
	    Row& row = rows[i];
	    row.y1 = static_cast<uint16_t> (std::max<int> (row.y1, row.y0 + size));

	    const auto rowHeight = static_cast<uint16_t> (row.y1 - row.y0);

	    if (static_cast<uint16_t> (row.x1 - row.x0) >= size && rowHeight >= size) {
		entry.x = row.x0;
		entry.y = row.y0;
		width = std::max (width, size + row.x0);
		height = std::max (height, row.y0 + size);
		row.x0 = static_cast<uint16_t> (row.x0 + size);

		if (rowHeight > 0) {
		    const Row below = { static_cast<uint16_t> (row.x0 - size), row.x0,
					static_cast<uint16_t> (row.y0 + size), row.y1 };
		    rows.push_back (below);
		}
		break;
	    }

	    const uint16_t top = row.y1;
	    rows.push_back ({ 0, 0x2000, top, top });
	}
    }

    return { width, height };
}

void ShadowMapping::render (std::vector<Entry>& entries) {
    if (entries.empty ()) {
	return;
    }

    if (this->m_program == GL_NONE) {
	this->setup ();
    }

    // the atlas grows to at least 2x2 and is only reallocated when it's too small on either side, then to exactly
    // the size the tiles need
    const glm::ivec2 needed = glm::max (pack (entries), glm::ivec2 (2));
    auto atlas = std::const_pointer_cast<CFBO> (this->m_atlas);

    if (static_cast<int> (atlas->getRealWidth ()) < needed.x || static_cast<int> (atlas->getRealHeight ()) < needed.y) {
	atlas->resize (needed.x, needed.y);
    }

    const auto atlasWidth = static_cast<float> (atlas->getRealWidth ());
    const auto atlasHeight = static_cast<float> (atlas->getRealHeight ());

    GLint previousFramebuffer = 0;
    GLint previousVAO = 0;
    GLint previousViewport[4] = {};
    glGetIntegerv (GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &previousVAO);
    glGetIntegerv (GL_VIEWPORT, previousViewport);

    // sub_140196530: the first batch clears the atlas to WE's reversed far depth, then the casters draw with depth
    // GREATER and the shadow rasterizer state (device +37 bit 2) of sub_140099050: slope scaled bias -4 (towards
    // the far side) and no culling, the cull mode of the previous state creation is never reset for it (RenderDoc of
    // live WE: NoCull, slope -4, D32)
    glBindFramebuffer (GL_FRAMEBUFFER, atlas->getFramebuffer ());
    glDepthMask (GL_TRUE);
    glClearDepth (0.0);
    glClear (GL_DEPTH_BUFFER_BIT);
    glClearDepth (1.0);
    glColorMask (GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDisable (GL_BLEND);
    glEnable (GL_DEPTH_TEST);
    glDepthFunc (GL_GREATER);
    glDisable (GL_CULL_FACE);
    glEnable (GL_POLYGON_OFFSET_FILL);
    glPolygonOffset (-4.0f, 0.0f);
    glUseProgram (this->m_program);

    for (auto& entry : entries) {
	*entry.transform = glm::vec4 (
	    static_cast<float> (entry.x) / atlasWidth, static_cast<float> (entry.y) / atlasHeight,
	    static_cast<float> (entry.size) / atlasWidth, static_cast<float> (entry.size) / atlasHeight
	);

	if (entry.point) {
	    // six faces two across, three down, each half and a third of the tile (integer sizes like WE). The cube
	    // faces' projection gets [3][2] - 0.00333 (0x140193b4c), pulling the depth away from the light
	    const int faceWidth = entry.size >> 1;
	    const int faceHeight = entry.size / 3;

	    for (int face = 0; face < 6; face++) {
		glm::mat4 biased = entry.viewProjection[face];
		biased[3][2] -= 0.00333f;

		glViewport (
		    entry.x + (face & 1) * faceWidth, entry.y + (face >> 1) * faceHeight, faceWidth, faceHeight
		);
		this->drawCasters (toGL (biased), biased);
	    }
	    continue;
	}

	// every other shadow gets its depth pulled 0.0005 away from the light in clip space (0x14019632a: the
	// viewport matrix's [3][2] - 0.0005), the lit materials compare against the matrix without it
	glm::mat4 biased = entry.viewProjection[0];
	biased[3][2] -= 0.0005f;

	glViewport (entry.x, entry.y, entry.size, entry.size);
	this->drawCasters (toGL (biased), biased);
    }

    glDisable (GL_POLYGON_OFFSET_FILL);
    glPolygonOffset (0.0f, 0.0f);
    glDepthFunc (GL_LESS);
    glDisable (GL_DEPTH_TEST);
    glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindVertexArray (previousVAO);
    glBindFramebuffer (GL_FRAMEBUFFER, previousFramebuffer);
    glViewport (previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
}

void ShadowMapping::drawCasters (const glm::mat4& viewProjection, const glm::mat4& direct3D) const {
    // sub_14018AAC0 mode 2: the scene's render list, objects with flags 0xC00 (models with a normal or
    // alphatocoverage material, sub_140224C70), visible and not drawn by a passthrough layer
    for (auto* object : this->m_scene.getLayers ()) {
	if (!object->is<Objects::CMesh> () || !this->m_scene.isShadowCasterVisible (*object)) {
	    continue;
	}

	object->as<Objects::CMesh> ()->renderShadowCaster (
	    this->m_program, this->m_modelViewProjection, this->m_alphaTest, viewProjection, direct3D
	);
    }
}
