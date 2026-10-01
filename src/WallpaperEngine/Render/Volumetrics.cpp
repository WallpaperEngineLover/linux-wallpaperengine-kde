#include "Volumetrics.h"

#include <bit>
#include <cmath>
#include <string>

#include <glm/gtc/matrix_transform.hpp>

#include "WallpaperEngine/Application/ApplicationContext.h"
#include "WallpaperEngine/Application/WallpaperApplication.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/RenderContext.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"

using namespace WallpaperEngine::Render;

namespace {
const char* kVolumeVertex = R"(
uniform mat4 u_ViewProjection;
uniform mat4 u_Volume;
uniform vec3 u_MeshScale;
layout (location = 0) in vec3 a_Position;
out vec4 v_ScreenPos;

void main () {
    gl_Position = u_ViewProjection * u_Volume * vec4 (a_Position * u_MeshScale, 1.0);
#if FULLSCREEN
    gl_Position = vec4 (a_Position.xy, -1.0, 1.0);
#endif
    v_ScreenPos = gl_Position;
}
)";

// the far side of the volume, as normalized depth
const char* kBackFragment = R"(
in vec4 v_ScreenPos;
out vec4 fragColor;

void main () {
    fragColor = vec4 (v_ScreenPos.z / v_ScreenPos.w, 0.0, 0.0, 1.0);
}
)";

// assets/shaders/volumetricsfront.frag: from the near side of the volume to the far one (or the scene's depth in front
// of it), the light's falloff summed over a few samples. With SHADOW each sample is compared against the light's tile
// of _rt_shadowAtlas (g_Texture0), spots through the light's matrix scaled by 0.525 like the cookie, point lights
// through common_pbr_2.h's CalculateProjectedCoordsPoint (copied below)
const char* kFrontFragment = R"(
#define mul(x, y) ((y) * (x))
uniform sampler2D u_Back;
uniform sampler2D u_SceneDepth;
uniform mat4 u_InverseViewProjection;
uniform vec2 u_BufferSize;
uniform vec3 u_LightOrigin;
uniform float u_Radius;
uniform float u_Intensity;
uniform float u_Density;
uniform float u_Exponent;
uniform vec3 u_Color;
uniform vec3 u_SpotForward;
uniform float u_SpotInner;
uniform float u_SpotOuter;
uniform sampler2D u_Cookie;
uniform mat4 u_LightViewProjection;
uniform vec3 u_EyePosition;
uniform vec3 g_FogDistanceColor;
uniform vec4 g_FogDistanceParams;
uniform vec3 g_FogHeightColor;
uniform vec4 g_FogHeightParams;
uniform sampler2DShadow u_ShadowAtlas;
uniform bool u_ShadowValid;
uniform mat4 u_ShadowMatrix;
uniform vec4 u_ShadowTransform;
uniform vec4 u_PointProjection;
in vec4 v_ScreenPos;
out vec4 fragColor;

#if SHADOW && POINTLIGHT
vec4 CalculateProjectedCoordsPoint(vec3 worldPos, vec3 lightOrigin, vec4 projectionInfo, vec4 atlasTransform)
{
	vec3 lightDelta = worldPos - lightOrigin;
	vec3 lightDeltaAbs = abs(lightDelta);
	vec2 viewportScale = vec2(0.5, 0.3333);

#if LIGHTS_SHADOW_MAPPING_QUALITY == 2 || LIGHTS_SHADOW_MAPPING_QUALITY == 1
	vec2 viewportPointCompensation = vec2(0.47, -0.47);
#elif LIGHTS_SHADOW_MAPPING_QUALITY == 3
	vec2 viewportPointCompensation = vec2(0.48, -0.48);
#else
	vec2 viewportPointCompensation = vec2(0.49, -0.49);
#endif

	vec2 viewportOffset;
	vec2 viewportOffsetSteps = atlasTransform.zw * viewportScale;
	mat4 viewMatrix;

	if (lightDeltaAbs.x >= lightDeltaAbs.y && lightDeltaAbs.x >= lightDeltaAbs.z)
	{
		if (lightDelta.x >= 0.0)
		{
			viewMatrix = mat4(
				0, 0, -1, 0,
				0, 1, 0, 0,
				1, 0, 0, 0,
				-lightOrigin.z, -lightOrigin.y, lightOrigin.x, 1
			);
			viewportOffset = vec2(0.0, 0.0);
		}
		else
		{
			viewMatrix = mat4(
				0, 0, 1, 0,
				0, 1, 0, 0,
				-1, 0, 0, 0,
				lightOrigin.z, -lightOrigin.y, -lightOrigin.x, 1
			);
			viewportOffset = vec2(viewportOffsetSteps.x, 0.0);
		}
	}
	else if (lightDeltaAbs.y >= lightDeltaAbs.x && lightDeltaAbs.y >= lightDeltaAbs.z)
	{
		if (lightDelta.y >= 0.0)
		{
			viewMatrix = mat4(
				1, 0, 0, 0,
				0, 0, -1, 0,
				0, 1, 0, 0,
				-lightOrigin.x, -lightOrigin.z, lightOrigin.y, 1
			);
			viewportOffset = vec2(0.0, viewportOffsetSteps.y);
		}
		else
		{
			viewMatrix = mat4(
				1, 0, 0, 0,
				0, 0, 1, 0,
				0, -1, 0, 0,
				-lightOrigin.x, lightOrigin.z, -lightOrigin.y, 1
			);
			viewportOffset = vec2(viewportOffsetSteps.x, viewportOffsetSteps.y);
		}
	}
	else
	{
		if (lightDelta.z >= 0.0)
		{
			viewMatrix = mat4(
				-1, 0, 0, 0,
				0, 1, 0, 0,
				0, 0, -1, 0,
				lightOrigin.x, -lightOrigin.y, lightOrigin.z, 1
			);
			viewportOffset = vec2(0.0, viewportOffsetSteps.y * 2);
		}
		else
		{
			viewMatrix = mat4(
				1, 0, 0, 0,
				0, 1, 0, 0,
				0, 0, 1, 0,
				-lightOrigin.x, -lightOrigin.y, -lightOrigin.z, 1
			);
			viewportOffset = vec2(viewportOffsetSteps.x, viewportOffsetSteps.y * 2);
		}
	}

	mat4 project = mat4(
		1, 0, 0, 0,
		0, 1, 0, 0,
		0, 0, projectionInfo.x, projectionInfo.z,
		0, 0, projectionInfo.y, projectionInfo.w
	);

	vec4 projectedCoords = mul(mul(vec4(worldPos, 1.0), viewMatrix), project);
	projectedCoords.xyz /= projectedCoords.w;

	projectedCoords.xy = projectedCoords.xy	* viewportPointCompensation + vec2(0.5);
	projectedCoords.y = mix(projectedCoords.y, 2.0, step(projectedCoords.w, 0.0));

	projectedCoords.xy *= atlasTransform.zw * viewportScale;
	projectedCoords.xy += atlasTransform.xy + viewportOffset;
	return projectedCoords;
}
#endif

float hash12 (vec2 p) {
    vec3 p3 = fract (vec3 (p.xyx) * 43758.5453);
    p3 += vec3 (dot (p3, p3.yzx + 19.19));
    return fract ((p3.x + p3.y) * p3.z);
}

float fogAlpha (float alpha, float viewLength, float height) {
    float fogDistance = 0.0;
    float fogHeight = 0.0;
#if FOG_DIST
    fogDistance = clamp ((viewLength - g_FogDistanceParams.x) / g_FogDistanceParams.y, 0.0, 1.0);
    fogDistance = g_FogDistanceParams.z + g_FogDistanceParams.w * fogDistance * fogDistance;
#endif
#if FOG_HEIGHT
    fogHeight = clamp ((height - g_FogHeightParams.x) / g_FogHeightParams.y, 0.0, 1.0);
    fogHeight = g_FogHeightParams.z + g_FogHeightParams.w * fogHeight * fogHeight;
#endif
    float fogFactor = clamp (max (fogDistance, fogHeight), 0.0, 1.0);
    return alpha * (1.0 - fogFactor * fogFactor);
}

void main () {
    vec3 screenDepth = v_ScreenPos.xyz / v_ScreenPos.w;
    vec2 screenUV = gl_FragCoord.xy / u_BufferSize;
    float backDepth = texture (u_SceneDepth, screenUV).r * 2.0 - 1.0;
    float limitDepth = texture (u_Back, screenUV).r;

    if (backDepth - screenDepth.z < 0.0) {
	discard;
    }

    backDepth = min (backDepth, limitDepth);

    vec4 worldStart = u_InverseViewProjection * vec4 (screenDepth, 1.0);
    vec4 worldEnd = u_InverseViewProjection * vec4 (screenDepth.xy, backDepth, 1.0);
    worldStart.xyz /= worldStart.w;
    worldEnd.xyz /= worldEnd.w;

    const float sampleCount = float (SAMPLES);
    vec3 worldStep = (worldEnd.xyz - worldStart.xyz) / (sampleCount + 1.0);
    float invRadius = 1.0 / u_Radius;
    float maxLightScale = u_Intensity * length (worldEnd.xyz - worldStart.xyz) * invRadius;
#if POINTLIGHT
    maxLightScale *= 0.5;
#endif

#if SHADOW
    // WE's screen UV runs top down, this buffer bottom up
    worldStart.xyz += worldStep * hash12 (screenDepth.xy * 0.5 + 0.5);
#endif

#if COOKIE
    vec3 shadowFactor = vec3 (0.0);
#else
    float shadowFactor = 0.0;
#endif

    for (int s = 0; s < SAMPLES; ++s) {
	worldStart.xyz += worldStep;
	vec3 lightDelta = worldStart.xyz - u_LightOrigin;
	float sampleValue = pow (clamp (1.0 - length (lightDelta) * invRadius, 0.0, 1.0), u_Exponent);
#if SHADOW
	if (u_ShadowValid) {
#if POINTLIGHT
	    vec4 shadowCoords = CalculateProjectedCoordsPoint (worldStart.xyz, u_LightOrigin, u_PointProjection, u_ShadowTransform);
#else
	    vec4 shadowCoords = u_ShadowMatrix * vec4 (worldStart.xyz, 1.0);
	    shadowCoords.xyz /= shadowCoords.w;
	    shadowCoords.xy = (shadowCoords.xy * vec2 (0.525, -0.525) + vec2 (0.5)) * u_ShadowTransform.zw + u_ShadowTransform.xy;
#endif
	    sampleValue *= texture (u_ShadowAtlas, shadowCoords.xyz);
	}
#endif
#if COOKIE
	// the cookie is projected through the light's own camera, a little bigger than its frustum
	vec4 uvs = u_LightViewProjection * vec4 (worldStart.xyz, 1.0);
	uvs.xyz /= uvs.w;
	vec3 cookieColor = texture (u_Cookie, uvs.xy * vec2 (0.525, -0.525) + vec2 (0.5)).rgb;
#elif !POINTLIGHT
	sampleValue *= smoothstep (u_SpotOuter, u_SpotInner, dot (normalize (lightDelta), u_SpotForward));
#endif
#if FOG_DIST || FOG_HEIGHT
	sampleValue *= fogAlpha (sampleValue, length (u_EyePosition - worldStart.xyz), worldStart.y);
#endif
#if COOKIE
	shadowFactor += sampleValue * cookieColor;
#else
	shadowFactor += sampleValue;
#endif
    }

    shadowFactor /= sampleCount;
    fragColor = vec4 (u_Density * maxLightScale * shadowFactor * u_Color * 0.1, 1.0);
}
)";

const char* kQuadVertex = R"(
out vec2 v_TexCoord;

void main () {
    vec2 corner = vec2 ((gl_VertexID << 1) & 2, gl_VertexID & 2);
    v_TexCoord = corner;
    gl_Position = vec4 (corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

// assets/shaders/blur_k3.frag
const char* kBlurFragment = R"(
uniform sampler2D g_Texture0;
uniform vec2 u_Direction;
in vec2 v_TexCoord;
out vec4 fragColor;

void main () {
    vec3 albedo = texture (g_Texture0, v_TexCoord + u_Direction).rgb * 0.25 + texture (g_Texture0, v_TexCoord).rgb * 0.5
	+ texture (g_Texture0, v_TexCoord - u_Direction).rgb * 0.25;
    fragColor = vec4 (albedo, 1.0);
}
)";

const char* kCompositeFragment = R"(
uniform sampler2D g_Texture0;
in vec2 v_TexCoord;
out vec4 fragColor;

void main () {
    fragColor = texture (g_Texture0, v_TexCoord);
}
)";

GLuint compile (const std::string& defines, const char* vertex, const char* fragment) {
    const std::string header = "#version 330\n" + defines;
    const std::string vertexSource = header + vertex;
    const std::string fragmentSource = header + fragment;
    const char* sources[] = { vertexSource.c_str (), fragmentSource.c_str () };
    const GLenum types[] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
    const GLuint program = glCreateProgram ();

    for (int unit = 0; unit < 2; unit++) {
	const GLuint shader = glCreateShader (types[unit]);
	glShaderSource (shader, 1, &sources[unit], nullptr);
	glCompileShader (shader);

	GLint compiled = GL_FALSE;
	glGetShaderiv (shader, GL_COMPILE_STATUS, &compiled);

	if (compiled == GL_FALSE) {
	    char log[2048] = {};
	    glGetShaderInfoLog (shader, sizeof (log) - 1, nullptr, log);
	    sLog.error ("Volumetrics shader failed to compile: ", log);
	}

	glAttachShader (program, shader);
	glDeleteShader (shader);
    }

    glLinkProgram (program);
    return program;
}

// +1 when the triangles wind counterclockwise seen from outside the (convex) mesh, -1 when clockwise
float outwardWinding (
    const std::vector<glm::vec3>& vertices, const std::vector<GLushort>& indices, const glm::vec3& inside
) {
    float sum = 0.0f;

    for (size_t i = 0; i + 2 < indices.size (); i += 3) {
	const glm::vec3& a = vertices[indices[i]];
	const glm::vec3 normal = glm::cross (vertices[indices[i + 1]] - a, vertices[indices[i + 2]] - a);
	sum += glm::dot (normal, a - inside) > 0.0f ? 1.0f : -1.0f;
    }

    return sum >= 0.0f ? 1.0f : -1.0f;
}

void createTarget (auto& target, const glm::ivec2 size, const GLenum format, const GLenum type, const GLenum layout) {
    glGenTextures (1, &target.texture);
    glBindTexture (GL_TEXTURE_2D, target.texture);
    glTexImage2D (GL_TEXTURE_2D, 0, format, size.x, size.y, 0, layout, type, nullptr);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers (1, &target.framebuffer);
    glBindFramebuffer (GL_FRAMEBUFFER, target.framebuffer);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0);
}

void destroyTarget (auto& target) {
    glDeleteFramebuffers (1, &target.framebuffer);
    glDeleteTextures (1, &target.texture);
    target = {};
}
} // namespace

Volumetrics::Volumetrics (Wallpapers::CScene& scene) :
    m_scene (scene), m_quality (scene.getContext ().getApp ().getContext ().settings.general.volumetricsQuality) { }

Volumetrics::~Volumetrics () {
    this->release ();

    for (const GLuint program : { this->m_backProgram, this->m_blurProgram, this->m_compositeProgram }) {
	if (program != GL_NONE) {
	    glDeleteProgram (program);
	}
    }

    for (const auto& type : this->m_frontPrograms) {
	for (const auto& shadow : type) {
	    for (const GLuint program : shadow) {
		if (program != GL_NONE) {
		    glDeleteProgram (program);
		}
	    }
	}
    }

    if (this->m_sceneDepth.texture != GL_NONE) {
	destroyTarget (this->m_sceneDepth);
    }

    for (auto* mesh : { &this->m_sphere, &this->m_cone, &this->m_box, &this->m_fullscreen }) {
	if (mesh->vao != GL_NONE) {
	    glDeleteBuffers (1, &mesh->vertices);
	    glDeleteBuffers (1, &mesh->indices);
	    glDeleteVertexArrays (1, &mesh->vao);
	}
    }
}

void Volumetrics::upload (Mesh& mesh, const std::vector<glm::vec3>& vertices, const std::vector<GLushort>& indices) {
    glGenVertexArrays (1, &mesh.vao);
    glBindVertexArray (mesh.vao);
    glGenBuffers (1, &mesh.vertices);
    glBindBuffer (GL_ARRAY_BUFFER, mesh.vertices);
    glBufferData (
	GL_ARRAY_BUFFER, static_cast<GLsizeiptr> (vertices.size () * sizeof (glm::vec3)), vertices.data (),
	GL_STATIC_DRAW
    );
    glGenBuffers (1, &mesh.indices);
    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, mesh.indices);
    glBufferData (
	GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr> (indices.size () * sizeof (GLushort)), indices.data (),
	GL_STATIC_DRAW
    );
    mesh.indexCount = static_cast<GLsizei> (indices.size ());
    glEnableVertexAttribArray (0);
    glVertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE, sizeof (glm::vec3), nullptr);
}

void Volumetrics::setup () {
    // the light's volume, a unit sphere (sub_14025C660): poles, then 23 rings of 25 vertices (seam doubled) every 7.5
    // degrees from the top, 24 slices
    std::vector<glm::vec3> vertices = { { 0.0f, 1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f } };

    for (int ring = 1; ring < 24; ring++) {
	const float latitude = static_cast<float> (ring) * 0.1308997f;

	for (int slice = 0; slice <= 24; slice++) {
	    const float longitude = static_cast<float> (slice) * 0.2617994f;
	    vertices.emplace_back (
		std::cos (longitude) * std::sin (latitude), std::cos (latitude),
		std::sin (longitude) * std::sin (latitude)
	    );
	}
    }

    std::vector<GLushort> indices;

    for (int ring = 0; ring < 22; ring++) {
	for (int slice = 0; slice < 24; slice++) {
	    const auto a = static_cast<GLushort> (2 + 25 * ring + slice);
	    const auto c = static_cast<GLushort> (a + 25);
	    indices.insert (
		indices.end (),
		{ a, static_cast<GLushort> (a + 1), c, static_cast<GLushort> (a + 1), static_cast<GLushort> (c + 1), c }
	    );
	}
    }

    for (int slice = 0; slice < 24; slice++) {
	indices.insert (indices.end (), { 0, static_cast<GLushort> (3 + slice), static_cast<GLushort> (2 + slice) });
	indices.insert (
	    indices.end (), { 1, static_cast<GLushort> (552 + slice), static_cast<GLushort> (553 + slice) }
	);
    }

    this->upload (this->m_sphere, vertices, indices);
    this->m_sphere.winding = outwardWinding (vertices, indices, glm::vec3 (0.0f));

    // spot lights without a cookie: a cylinder in the light's clip space (a cone in the world) with 32 sides, depth 0
    // at the near plane and 1 at the far one, the caps fanned out from (0.5, 0.5) (sub_140196CE0)
    vertices = { { 0.5f, 0.5f, 0.0f }, { 0.5f, 0.5f, 1.0f } };
    indices.clear ();

    for (int side = 0; side < 32; side++) {
	const float angle = static_cast<float> (side) * 0.03125f * 6.2831855f;
	const float next = angle + 0.19634955f;
	const auto base = static_cast<GLushort> (vertices.size ());

	vertices.emplace_back (std::sin (angle), -std::cos (angle), 1.0f);
	vertices.emplace_back (std::sin (next), -std::cos (next), 1.0f);
	vertices.emplace_back (std::sin (angle), -std::cos (angle), 0.0f);
	vertices.emplace_back (std::sin (next), -std::cos (next), 0.0f);
	indices.insert (
	    indices.end (),
	    { static_cast<GLushort> (base + 2), base, static_cast<GLushort> (base + 1),
	      static_cast<GLushort> (base + 2), static_cast<GLushort> (base + 1), static_cast<GLushort> (base + 3), 1,
	      static_cast<GLushort> (base + 1), base, 0, static_cast<GLushort> (base + 2),
	      static_cast<GLushort> (base + 3) }
	);
    }

    this->upload (this->m_cone, vertices, indices);
    this->m_cone.winding = outwardWinding (vertices, indices, glm::vec3 (0.0f, 0.0f, 0.5f));
    // cookie spots: the whole frustum as a box in the light's clip space (sub_140196CE0)
    vertices = { { -1.0f, -1.0f, 1.0f }, { 1.0f, -1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f }, { -1.0f, 1.0f, 1.0f },
		 { -1.0f, -1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f }, { 1.0f, 1.0f, 0.0f }, { -1.0f, 1.0f, 0.0f } };
    indices = { 1, 0, 2, 2, 0, 3, 4, 5, 6, 4, 6, 7, 3, 0, 4, 3, 4, 7,
		1, 2, 5, 5, 2, 6, 2, 3, 6, 6, 3, 7, 0, 1, 5, 0, 5, 4 };
    this->upload (this->m_box, vertices, indices);
    this->m_box.winding = outwardWinding (vertices, indices, glm::vec3 (0.0f, 0.0f, 0.5f));
    this->upload (
	this->m_fullscreen, { { -1.0f, 1.0f, 0.0f }, { -1.0f, -3.0f, 0.0f }, { 3.0f, 1.0f, 0.0f } }, { 0, 1, 2 }
    );

    // sample counts per quality, lights casting shadows take more and jitter the start (volumetricsfront.frag)
    const int quality = std::clamp (this->m_quality, 1, 4);
    const int plain[] = { 2, 3, 5, 8 };
    const int shadowed[] = { 12, 24, 32, 64 };
    std::string fog;

    if (this->m_scene.hasDistanceFog ()) {
	fog += "#define FOG_DIST 1\n";
    }
    if (this->m_scene.hasHeightFog ()) {
	fog += "#define FOG_HEIGHT 1\n";
    }

    const std::string samples = "#define SAMPLES " + std::to_string (plain[quality - 1]) + "\n";
    // LIGHTS_SHADOW_MAPPING_QUALITY only goes to point lights (renderer +428), the other shaders don't read it
    const std::string shadowSamples = "#define SHADOW 1\n#define SAMPLES " + std::to_string (shadowed[quality - 1])
	+ "\n#define LIGHTS_SHADOW_MAPPING_QUALITY "
	+ std::to_string (this->m_scene.getContext ().getApp ().getContext ().settings.general.shadowQuality) + "\n";

    this->m_backProgram = compile ("", kVolumeVertex, kBackFragment);

    // cookie lights take the shadowed sample counts too, but don't jitter
    const std::string cookieSamples
	= "#define COOKIE 1\n#define SAMPLES " + std::to_string (shadowed[quality - 1]) + "\n";

    for (int kind = 0; kind < 3; kind++) {
	for (int shadow = 0; shadow < 2; shadow++) {
	    for (int fullscreen = 0; fullscreen < 2; fullscreen++) {
		std::string defines = fog + (shadow ? shadowSamples : kind == 2 ? cookieSamples : samples);

		if (shadow && kind == 2) {
		    defines += "#define COOKIE 1\n";
		}
		if (kind == 0) {
		    defines += "#define POINTLIGHT 1\n";
		}
		if (fullscreen) {
		    defines += "#define FULLSCREEN 1\n";
		}

		this->m_frontPrograms[kind][shadow][fullscreen] = compile (defines, kVolumeVertex, kFrontFragment);
	    }
	}
    }

    this->m_blurProgram = compile ("", kQuadVertex, kBlurFragment);
    this->m_compositeProgram = compile ("", kQuadVertex, kCompositeFragment);
}

void Volumetrics::allocate (const glm::ivec2 size) {
    this->release ();
    this->m_size = size;
    // the light buffer is 8 bit unless the scene renders in HDR (format 1 or 15, sub_140196CE0)
    const bool hdr = this->m_scene.isHDR ();
    createTarget (this->m_light, size, hdr ? GL_RGBA16F : GL_RGBA8, hdr ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, GL_RGBA);
    createTarget (this->m_lightB, size, hdr ? GL_RGBA16F : GL_RGBA8, hdr ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, GL_RGBA);
    createTarget (this->m_back, size, GL_R32F, GL_FLOAT, GL_RED);
    glBindTexture (GL_TEXTURE_2D, this->m_back.texture);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    // shared by the back pass and the light buffer
    glGenRenderbuffers (1, &this->m_backDepth);
    glBindRenderbuffer (GL_RENDERBUFFER, this->m_backDepth);
    glRenderbufferStorage (GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, size.x, size.y);
    glBindFramebuffer (GL_FRAMEBUFFER, this->m_back.framebuffer);
    glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, this->m_backDepth);
    glBindFramebuffer (GL_FRAMEBUFFER, this->m_light.framebuffer);
    glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, this->m_backDepth);
}

void Volumetrics::release () {
    if (this->m_light.texture == GL_NONE) {
	return;
    }

    destroyTarget (this->m_light);
    destroyTarget (this->m_lightB);
    destroyTarget (this->m_back);
    glDeleteRenderbuffers (1, &this->m_backDepth);
    this->m_backDepth = GL_NONE;
}

glm::vec3 Volumetrics::approximateNormalize (const glm::vec3& vector) {
    const float lengthSquared = glm::dot (vector, vector);
    const float guess = std::bit_cast<float> (0x5F375A86u - (std::bit_cast<uint32_t> (lengthSquared) >> 1));
    const float inverse = (1.5f - lengthSquared * 0.5f * guess * guess) * guess;

    return vector * inverse;
}

glm::mat4
Volumetrics::spotViewProjection (const Data::Model::Light& light, const glm::mat4& world, const bool orthographic) {
    // sub_14025D420: the light looks down its x axis (view rows z, y, -x of the normalized world axes) through a
    // right handed PerspectiveFov (sub_14009A360) of twice the outer cone, aspect 1, near 0.05 (1 in orthographic
    // scenes), far the radius. Depth runs 0..1 like Direct3D, the cone mesh is built in that space
    const glm::vec3 x = approximateNormalize (glm::vec3 (world[0]));
    const glm::vec3 y = approximateNormalize (glm::vec3 (world[1]));
    const glm::vec3 z = approximateNormalize (glm::vec3 (world[2]));
    const glm::vec3 origin (world[3]);
    glm::mat4 view (1.0f);

    for (int i = 0; i < 3; i++) {
	view[i][0] = z[i];
	view[i][1] = y[i];
	view[i][2] = -x[i];
    }

    view[3][0] = -glm::dot (z, origin);
    view[3][1] = -glm::dot (y, origin);
    view[3][2] = glm::dot (x, origin);

    const float near = orthographic ? 1.0f : 0.05f;
    const float far = std::max (light.radius->value->getFloat (), near + 0.01f);
    const float height = 1.0f / std::tan (light.outerCone->value->getFloat () * 0.017453292f);
    const float range = far / (near - far);
    glm::mat4 projection (0.0f);

    projection[0][0] = height;
    projection[1][1] = height;
    projection[2][2] = range;
    projection[2][3] = -1.0f;
    projection[3][2] = range * near;

    return projection * view;
}

void Volumetrics::copySceneDepth (const GLint framebuffer) {
    // _rt_volumetricsBack is a depth only target the size of the output, filled from the bound target's depth before
    // every light (render target vtable slot 8 -> CopyResource). A target of another size (a layer buffer) can't be
    // copied, the texture keeps what it had
    const auto fbo = this->m_scene.getFBO ();
    const glm::ivec2 size (fbo->getRealWidth (), fbo->getRealHeight ());

    if (size != this->m_sceneDepthSize) {
	if (this->m_sceneDepth.texture != GL_NONE) {
	    destroyTarget (this->m_sceneDepth);
	}

	this->m_sceneDepthSize = size;
	glGenTextures (1, &this->m_sceneDepth.texture);
	glBindTexture (GL_TEXTURE_2D, this->m_sceneDepth.texture);
	glTexImage2D (
	    GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, size.x, size.y, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr
	);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glGenFramebuffers (1, &this->m_sceneDepth.framebuffer);
	glBindFramebuffer (GL_FRAMEBUFFER, this->m_sceneDepth.framebuffer);
	glFramebufferTexture2D (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, this->m_sceneDepth.texture, 0);
	glDrawBuffer (GL_NONE);
	glReadBuffer (GL_NONE);
	// passes leave their own depthwrite behind, and glClear honors it
	glDepthMask (GL_TRUE);
	glClearDepth (1.0);
	glClear (GL_DEPTH_BUFFER_BIT);
	glBindFramebuffer (GL_FRAMEBUFFER, framebuffer);
    }

    GLint type = GL_NONE;
    GLint renderbuffer = GL_NONE;
    glGetFramebufferAttachmentParameteriv (
	GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type
    );

    if (framebuffer == 0 || type != GL_RENDERBUFFER) {
	return;
    }

    glGetFramebufferAttachmentParameteriv (
	GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &renderbuffer
    );

    GLint previousRenderbuffer = 0;
    GLint width = 0;
    GLint height = 0;
    glGetIntegerv (GL_RENDERBUFFER_BINDING, &previousRenderbuffer);
    glBindRenderbuffer (GL_RENDERBUFFER, renderbuffer);
    glGetRenderbufferParameteriv (GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &width);
    glGetRenderbufferParameteriv (GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &height);
    glBindRenderbuffer (GL_RENDERBUFFER, previousRenderbuffer);

    if (glm::ivec2 (width, height) != size) {
	return;
    }

    glBindFramebuffer (GL_READ_FRAMEBUFFER, framebuffer);
    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, this->m_sceneDepth.framebuffer);
    glBlitFramebuffer (0, 0, size.x, size.y, 0, 0, size.x, size.y, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer (GL_FRAMEBUFFER, framebuffer);
}

void Volumetrics::renderLight (
    const Data::Model::Light& light, const glm::mat4& world, const glm::mat4& viewProjection, const Shadow* shadow
) {
    if (this->m_quality <= 0) {
	return;
    }

    if (this->m_sphere.vao == GL_NONE) {
	this->setup ();
    }

    // light buffer at an eighth of the output resolution, a quarter from high quality up
    const glm::ivec2 size
	= glm::max (this->m_scene.getOutputResolution () / (this->m_quality >= 3 ? 4 : 8), glm::ivec2 (1));

    if (size != this->m_size) {
	this->allocate (size);
    }

    GLint previousFramebuffer = 0;
    GLint previousVao = 0;
    GLint previousViewport[4] = {};
    glGetIntegerv (GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &previousVao);
    glGetIntegerv (GL_VIEWPORT, previousViewport);

    this->copySceneDepth (previousFramebuffer);

    const bool spot = light.type == Data::Model::LightType::Spot;
    const bool cookie = spot && light.useCookie;
    const float radius = light.radius->value->getFloat ();
    const glm::vec3 origin (world[3]);
    const auto& fog = this->m_scene.getFog ();
    const auto& camera = this->m_scene.getCamera ();
    const glm::vec3 forward = camera.isOrthogonal ()
	? glm::vec3 (0.0f, 0.0f, -1.0f)
	: -glm::vec3 (camera.getView ()[0][2], camera.getView ()[1][2], camera.getView ()[2][2]);
    const glm::vec3 probe = fog.eyeWorld + forward * 0.2f - origin;
    const bool shadowed
	= light.castShadow && this->m_scene.getContext ().getApp ().getContext ().settings.general.shadowQuality > 0;
    glm::mat4 volume;
    bool inside;

    const glm::mat4 lightViewProjection
	= spot ? spotViewProjection (light, world, camera.isOrthogonal ()) : glm::mat4 (1.0f);

    if (cookie) {
	// the eye is inside the frustum's planes (sub_1401849E0: x+w, w-x, y+w, w-y, z+w, w-z all >= 0) of the light's
	// +824 matrix, which has reversed depth (z' = w - z here). That gives 0 <= z <= 2w: the near plane counts, the
	// far one practically never does (2w lies past the camera's infinite distance once far > 2 near)
	volume = glm::inverse (lightViewProjection);

	const glm::vec4 clip = lightViewProjection * glm::vec4 (fog.eyeWorld + forward * 0.1f, 1.0f);

	inside
	    = std::abs (clip.x) <= clip.w && std::abs (clip.y) <= clip.w && clip.z >= 0.0f && clip.z <= 2.0f * clip.w;
    } else if (spot) {
	// the cone is drawn from the light's clip space, whose inverse also gives the cone's radius at the far plane.
	// The eye is inside when it's in front of the light, no further than the radius and within the cone there
	volume = glm::inverse (lightViewProjection);

	const glm::vec3 direction = glm::normalize (glm::vec3 (world[0]));
	const float along = glm::dot (direction, probe);
	const float across = glm::length (probe - direction * along);
	const glm::vec4 center = volume * glm::vec4 (0.0f, 0.0f, 1.0f, 1.0f);
	const glm::vec4 edge = volume * glm::vec4 (0.0f, 1.0f, 1.0f, 1.0f);
	const float farRadius = glm::length (glm::vec3 (edge) / edge.w - glm::vec3 (center) / center.w);

	inside = along > 0.0f && radius >= along && along / radius * farRadius >= across;
    } else {
	volume = world * glm::scale (glm::mat4 (1.0f), glm::vec3 (radius));
	inside = radius * radius > glm::dot (probe, probe);
    }

    const Mesh& mesh = cookie ? this->m_box : spot ? this->m_cone : this->m_sphere;
    // volumetrics_back and volumetrics_front cull (cullmode normal): the back pass only draws the far side of the
    // volume and the front pass only the near one, so where the scene's depth range clips the far side off the ray
    // runs to the far plane. The near side faces the viewer when the mesh's outward winding, flipped by a mirroring
    // transform, comes out clockwise on screen
    const bool nearIsFront = mesh.winding * glm::determinant (viewProjection * volume) < 0.0f;

    glBindVertexArray (mesh.vao);
    glViewport (0, 0, size.x, size.y);
    glEnable (GL_CULL_FACE);
    glFrontFace (GL_CCW);
    glCullFace (nearIsFront ? GL_FRONT : GL_BACK);
    glDisable (GL_BLEND);
    glEnable (GL_DEPTH_TEST);
    glDepthMask (GL_TRUE);
    glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    // far side of the volume
    glBindFramebuffer (GL_FRAMEBUFFER, this->m_back.framebuffer);
    glClearColor (1.0f, 0.0f, 0.0f, 0.0f);
    glClearDepth (0.0);
    glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDepthFunc (GL_GREATER);
    glUseProgram (this->m_backProgram);
    glUniformMatrix4fv (
	glGetUniformLocation (this->m_backProgram, "u_ViewProjection"), 1, GL_FALSE, &viewProjection[0][0]
    );
    glUniformMatrix4fv (glGetUniformLocation (this->m_backProgram, "u_Volume"), 1, GL_FALSE, &volume[0][0]);
    glUniform3f (glGetUniformLocation (this->m_backProgram, "u_MeshScale"), 1.0f, 1.0f, 1.0f);
    glDrawElements (GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_SHORT, nullptr);

    // near side, added into the light buffer. Cleared by the first light since the last composite
    glBindFramebuffer (GL_FRAMEBUFFER, this->m_light.framebuffer);
    glClearDepth (1.0);

    if (!this->m_pending) {
	glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
	glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	this->m_pending = true;
    } else {
	glClear (GL_DEPTH_BUFFER_BIT);
    }

    glDepthFunc (GL_LESS);
    glCullFace (nearIsFront ? GL_BACK : GL_FRONT);
    glEnable (GL_BLEND);
    glBlendEquation (GL_FUNC_ADD);
    glBlendFunc (GL_ONE, GL_ONE);

    const GLuint program = this->m_frontPrograms[cookie ? 2 : spot ? 1 : 0][shadowed][inside];
    const glm::mat4 inverse = glm::inverse (viewProjection);
    const glm::vec3 color = light.color->value->getVec3 ();
    const glm::vec3 spotForward (world[0]);

    glUseProgram (program);
    glActiveTexture (GL_TEXTURE1);
    glBindTexture (GL_TEXTURE_2D, this->m_sceneDepth.texture);
    glActiveTexture (GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, this->m_back.texture);
    glUniform1i (glGetUniformLocation (program, "u_Back"), 0);
    glUniform1i (glGetUniformLocation (program, "u_SceneDepth"), 1);
    glUniformMatrix4fv (
	glGetUniformLocation (program, "u_LightViewProjection"), 1, GL_FALSE, &lightViewProjection[0][0]
    );

    if (cookie) {
	// the light's own cookie texture (light +816, sub_14025D080)
	const auto texture = this->m_scene.getContext ().resolveTexture (
	    light.cookie.empty () ? "cookie/flashlight1" : light.cookie, this->m_scene.getScene ().project
	);

	glActiveTexture (GL_TEXTURE2);
	glBindTexture (GL_TEXTURE_2D, texture->getTextureID (0));
	glActiveTexture (GL_TEXTURE0);
	glUniform1i (glGetUniformLocation (program, "u_Cookie"), 2);
    }
    // g_RenderVar0 = the tile's atlas transform, g_RenderVar3 = the point projection, g_AltModelMatrix = the spot's
    // matrix, the atlas on g_Texture0 with WE's comparison sampler (CFBO::setupDepthOnly)
    const bool shadowValid = shadowed && shadow != nullptr && shadow->atlasTransform != nullptr;

    glUniform1i (glGetUniformLocation (program, "u_ShadowValid"), shadowValid ? 1 : 0);

    if (shadowValid) {
	glActiveTexture (GL_TEXTURE3);
	glBindTexture (GL_TEXTURE_2D, this->m_scene.getShadowAtlas ()->getTextureID (0));
	glActiveTexture (GL_TEXTURE0);
	glUniform1i (glGetUniformLocation (program, "u_ShadowAtlas"), 3);
	glUniformMatrix4fv (glGetUniformLocation (program, "u_ShadowMatrix"), 1, GL_FALSE, &shadow->matrix[0][0]);
	glUniform4fv (glGetUniformLocation (program, "u_ShadowTransform"), 1, &(*shadow->atlasTransform)[0]);
	glUniform4fv (glGetUniformLocation (program, "u_PointProjection"), 1, &shadow->pointProjection[0]);
    }

    glUniformMatrix4fv (glGetUniformLocation (program, "u_ViewProjection"), 1, GL_FALSE, &viewProjection[0][0]);
    glUniformMatrix4fv (glGetUniformLocation (program, "u_Volume"), 1, GL_FALSE, &volume[0][0]);
    glUniformMatrix4fv (glGetUniformLocation (program, "u_InverseViewProjection"), 1, GL_FALSE, &inverse[0][0]);
    // volumetricsfront.vert pulls the cone's sides in a little, the sphere stays as it is
    glUniform3f (glGetUniformLocation (program, "u_MeshScale"), spot ? 0.99f : 1.0f, spot ? 0.99f : 1.0f, 1.0f);
    glUniform2f (
	glGetUniformLocation (program, "u_BufferSize"), static_cast<float> (size.x), static_cast<float> (size.y)
    );
    glUniform3fv (glGetUniformLocation (program, "u_LightOrigin"), 1, &origin[0]);
    // g_RenderVar1.x is 99% of the radius, the falloff ends just inside the mesh
    glUniform1f (glGetUniformLocation (program, "u_Radius"), radius * 0.99f);
    glUniform1f (glGetUniformLocation (program, "u_Intensity"), light.intensity->value->getFloat ());
    glUniform1f (glGetUniformLocation (program, "u_Density"), light.density->value->getFloat ());
    glUniform1f (glGetUniformLocation (program, "u_Exponent"), light.volumetricsExponent->value->getFloat ());
    glUniform3fv (glGetUniformLocation (program, "u_Color"), 1, &color[0]);
    // g_RenderVar1.yz are the cosines of the cone angles, g_RenderVar3 the world matrix's x row as it is
    glUniform1f (
	glGetUniformLocation (program, "u_SpotInner"), std::cos (light.innerCone->value->getFloat () * 0.017453292f)
    );
    glUniform1f (
	glGetUniformLocation (program, "u_SpotOuter"), std::cos (light.outerCone->value->getFloat () * 0.017453292f)
    );
    glUniform3fv (glGetUniformLocation (program, "u_SpotForward"), 1, &spotForward[0]);
    glUniform3fv (glGetUniformLocation (program, "u_EyePosition"), 1, &fog.eyeWorld[0]);
    glUniform3fv (glGetUniformLocation (program, "g_FogDistanceColor"), 1, &fog.distanceColor[0]);
    glUniform4fv (glGetUniformLocation (program, "g_FogDistanceParams"), 1, &fog.distanceParams[0]);
    glUniform3fv (glGetUniformLocation (program, "g_FogHeightColor"), 1, &fog.heightColor[0]);
    glUniform4fv (glGetUniformLocation (program, "g_FogHeightParams"), 1, &fog.heightParamsWorld[0]);

    // from inside the volume a triangle covering the screen (-1,1 / -1,-3 / 3,1) goes through volumetrics_fullscreen
    if (inside) {
	glDisable (GL_CULL_FACE);
	glBindVertexArray (this->m_fullscreen.vao);
	glDrawElements (GL_TRIANGLES, this->m_fullscreen.indexCount, GL_UNSIGNED_SHORT, nullptr);
    } else {
	glDrawElements (GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_SHORT, nullptr);
    }

    // passes only switch culling on and off, they expect the back faces to go
    glCullFace (GL_BACK);
    glFrontFace (GL_CCW);
    glDisable (GL_CULL_FACE);
    glDisable (GL_DEPTH_TEST);
    glDepthFunc (GL_LESS);
    glDisable (GL_BLEND);
    glBindFramebuffer (GL_FRAMEBUFFER, previousFramebuffer);
    glBindVertexArray (previousVao);
    glViewport (previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
}

void Volumetrics::composite () {
    if (!this->m_pending) {
	return;
    }

    this->m_pending = false;

    GLint previousFramebuffer = 0;
    GLint previousVao = 0;
    GLint previousViewport[4] = {};
    glGetIntegerv (GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &previousVao);
    glGetIntegerv (GL_VIEWPORT, previousViewport);

    glBindVertexArray (this->m_fullscreen.vao);
    glDisable (GL_DEPTH_TEST);
    glDisable (GL_CULL_FACE);
    glDisable (GL_BLEND);
    glActiveTexture (GL_TEXTURE0);

    // volumetrics_blur_h/_v below high quality, buffer A -> B -> A
    if (this->m_quality < 3) {
	glViewport (0, 0, this->m_size.x, this->m_size.y);
	glUseProgram (this->m_blurProgram);
	glUniform1i (glGetUniformLocation (this->m_blurProgram, "g_Texture0"), 0);

	glBindFramebuffer (GL_FRAMEBUFFER, this->m_lightB.framebuffer);
	glBindTexture (GL_TEXTURE_2D, this->m_light.texture);
	glUniform2f (
	    glGetUniformLocation (this->m_blurProgram, "u_Direction"), 1.0f / static_cast<float> (this->m_size.x), 0.0f
	);
	glDrawArrays (GL_TRIANGLES, 0, 3);

	glBindFramebuffer (GL_FRAMEBUFFER, this->m_light.framebuffer);
	glBindTexture (GL_TEXTURE_2D, this->m_lightB.texture);
	glUniform2f (
	    glGetUniformLocation (this->m_blurProgram, "u_Direction"), 0.0f, 1.0f / static_cast<float> (this->m_size.y)
	);
	glDrawArrays (GL_TRIANGLES, 0, 3);
    }

    // volumetrics_combine: added onto the scene
    glBindFramebuffer (GL_FRAMEBUFFER, previousFramebuffer);
    glViewport (previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
    glEnable (GL_BLEND);
    glBlendEquation (GL_FUNC_ADD);
    glBlendFuncSeparate (GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
    glUseProgram (this->m_compositeProgram);
    glUniform1i (glGetUniformLocation (this->m_compositeProgram, "g_Texture0"), 0);
    glBindTexture (GL_TEXTURE_2D, this->m_light.texture);
    glDrawArrays (GL_TRIANGLES, 0, 3);

    glDisable (GL_BLEND);
    glBindVertexArray (previousVao);
}
