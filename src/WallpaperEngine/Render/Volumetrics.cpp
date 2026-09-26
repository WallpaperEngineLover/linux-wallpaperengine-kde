#include "Volumetrics.h"

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
layout (location = 0) in vec3 a_Position;
out vec4 v_ScreenPos;

void main () {
    gl_Position = u_ViewProjection * u_Volume * vec4 (a_Position, 1.0);
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

// assets/shaders/volumetricsfront.frag for point lights: from the near side of the volume to the far one, the light's
// falloff summed over a few samples. The shadow atlas has nothing in it (no shadow casters), so every sample is lit
const char* kFrontFragment = R"(
uniform sampler2D u_Back;
uniform mat4 u_InverseViewProjection;
uniform vec2 u_BufferSize;
uniform vec3 u_LightOrigin;
uniform float u_Radius;
uniform float u_Intensity;
uniform float u_Density;
uniform float u_Exponent;
uniform vec3 u_Color;
uniform vec3 u_EyePosition;
uniform vec3 g_FogDistanceColor;
uniform vec4 g_FogDistanceParams;
uniform vec3 g_FogHeightColor;
uniform vec4 g_FogHeightParams;
in vec4 v_ScreenPos;
out vec4 fragColor;

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
    float backDepth = texture (u_Back, gl_FragCoord.xy / u_BufferSize).r;

    if (backDepth - screenDepth.z < 0.0) {
	discard;
    }

    // nothing in the scene limits the ray, the scene buffer has no depth there
    backDepth = min (backDepth, 1.0);

    vec4 worldStart = u_InverseViewProjection * vec4 (screenDepth, 1.0);
    vec4 worldEnd = u_InverseViewProjection * vec4 (screenDepth.xy, backDepth, 1.0);
    worldStart.xyz /= worldStart.w;
    worldEnd.xyz /= worldEnd.w;

    const float sampleCount = float (SAMPLES);
    vec3 worldStep = (worldEnd.xyz - worldStart.xyz) / (sampleCount + 1.0);
    float invRadius = 1.0 / u_Radius;
    float maxLightScale = u_Intensity * length (worldEnd.xyz - worldStart.xyz) * invRadius * 0.5;

#if SHADOW
    // WE's screen UV runs top down, this buffer bottom up
    worldStart.xyz += worldStep * hash12 (screenDepth.xy * 0.5 + 0.5);
#endif

    float shadowFactor = 0.0;

    for (int s = 0; s < SAMPLES; ++s) {
	worldStart.xyz += worldStep;
	vec3 lightDelta = worldStart.xyz - u_LightOrigin;
	float sampleValue = pow (clamp (1.0 - length (lightDelta) * invRadius, 0.0, 1.0), u_Exponent);
#if FOG_DIST || FOG_HEIGHT
	sampleValue *= fogAlpha (sampleValue, length (u_EyePosition - worldStart.xyz), worldStart.y);
#endif
	shadowFactor += sampleValue;
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

    for (const GLuint program :
	 { this->m_backProgram, this->m_frontProgram, this->m_frontFullscreenProgram, this->m_frontShadowProgram,
	   this->m_frontShadowFullscreenProgram, this->m_blurProgram, this->m_compositeProgram }) {
	if (program != GL_NONE) {
	    glDeleteProgram (program);
	}
    }

    if (this->m_sphereVertices != GL_NONE) {
	glDeleteBuffers (1, &this->m_sphereVertices);
	glDeleteBuffers (1, &this->m_sphereIndices);
	glDeleteVertexArrays (1, &this->m_vao);
    }
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
		std::cos (longitude) * std::sin (latitude), std::cos (latitude), std::sin (longitude) * std::sin (latitude)
	    );
	}
    }

    std::vector<GLushort> indices;

    for (int ring = 0; ring < 22; ring++) {
	for (int slice = 0; slice < 24; slice++) {
	    const auto a = static_cast<GLushort> (2 + 25 * ring + slice);
	    const auto c = static_cast<GLushort> (a + 25);
	    indices.insert (indices.end (), { a, static_cast<GLushort> (a + 1), c, static_cast<GLushort> (a + 1),
					      static_cast<GLushort> (c + 1), c });
	}
    }

    for (int slice = 0; slice < 24; slice++) {
	indices.insert (indices.end (), { 0, static_cast<GLushort> (3 + slice), static_cast<GLushort> (2 + slice) });
	indices.insert (indices.end (), { 1, static_cast<GLushort> (552 + slice), static_cast<GLushort> (553 + slice) });
    }

    glGenVertexArrays (1, &this->m_vao);
    glBindVertexArray (this->m_vao);
    glGenBuffers (1, &this->m_sphereVertices);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_sphereVertices);
    glBufferData (
	GL_ARRAY_BUFFER, static_cast<GLsizeiptr> (vertices.size () * sizeof (glm::vec3)), vertices.data (), GL_STATIC_DRAW
    );
    glGenBuffers (1, &this->m_sphereIndices);
    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->m_sphereIndices);
    glBufferData (
	GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr> (indices.size () * sizeof (GLushort)), indices.data (),
	GL_STATIC_DRAW
    );
    this->m_sphereIndexCount = static_cast<GLsizei> (indices.size ());
    glEnableVertexAttribArray (0);
    glVertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE, sizeof (glm::vec3), nullptr);

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
    const std::string shadowSamples
	= "#define SHADOW 1\n#define SAMPLES " + std::to_string (shadowed[quality - 1]) + "\n";

    this->m_backProgram = compile ("", kVolumeVertex, kBackFragment);
    this->m_frontProgram = compile (fog + samples, kVolumeVertex, kFrontFragment);
    this->m_frontFullscreenProgram = compile (fog + samples + "#define FULLSCREEN 1\n", kVolumeVertex, kFrontFragment);
    this->m_frontShadowProgram = compile (fog + shadowSamples, kVolumeVertex, kFrontFragment);
    this->m_frontShadowFullscreenProgram
	= compile (fog + shadowSamples + "#define FULLSCREEN 1\n", kVolumeVertex, kFrontFragment);
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
    // shared by the back pass (farthest surface) and the light buffer (nearest one), the sphere's winding doesn't
    // matter that way
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

void Volumetrics::renderLight (const Data::Model::Light& light, const glm::mat4& world) {
    if (this->m_quality <= 0) {
	return;
    }

    if (this->m_sphereVertices == GL_NONE) {
	this->setup ();
    }

    // light buffer at an eighth of the output resolution, a quarter from high quality up
    const glm::ivec2 size = glm::max (this->m_scene.getOutputResolution () / (this->m_quality >= 3 ? 4 : 8), glm::ivec2 (1));

    if (size != this->m_size) {
	this->allocate (size);
    }

    GLint previousFramebuffer = 0;
    GLint previousVao = 0;
    GLint previousViewport[4] = {};
    glGetIntegerv (GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &previousVao);
    glGetIntegerv (GL_VIEWPORT, previousViewport);

    const float radius = light.radius->value->getFloat ();
    const glm::mat4 volume = world * glm::scale (glm::mat4 (1.0f), glm::vec3 (radius));
    const glm::mat4 viewProjection = this->m_scene.getWorldViewProjection ();
    const glm::vec3 origin (world[3]);
    const auto& fog = this->m_scene.getFog ();
    const auto& camera = this->m_scene.getCamera ();
    const glm::vec3 forward = camera.isOrthogonal ()
	? glm::vec3 (0.0f, 0.0f, -1.0f)
	: -glm::vec3 (camera.getView ()[0][2], camera.getView ()[1][2], camera.getView ()[2][2]);
    const glm::vec3 probe = fog.eyeWorld + forward * 0.2f - origin;
    const bool inside = radius * radius > glm::dot (probe, probe);
    const bool shadow = light.castShadow
	&& this->m_scene.getContext ().getApp ().getContext ().settings.general.shadowQuality > 0;

    glBindVertexArray (this->m_vao);
    glViewport (0, 0, size.x, size.y);
    glDisable (GL_CULL_FACE);
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
    glUniformMatrix4fv (glGetUniformLocation (this->m_backProgram, "u_ViewProjection"), 1, GL_FALSE, &viewProjection[0][0]);
    glUniformMatrix4fv (glGetUniformLocation (this->m_backProgram, "u_Volume"), 1, GL_FALSE, &volume[0][0]);
    glDrawElements (GL_TRIANGLES, this->m_sphereIndexCount, GL_UNSIGNED_SHORT, nullptr);

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
    glEnable (GL_BLEND);
    glBlendEquation (GL_FUNC_ADD);
    glBlendFunc (GL_ONE, GL_ONE);

    const GLuint program = shadow ? (inside ? this->m_frontShadowFullscreenProgram : this->m_frontShadowProgram)
				  : (inside ? this->m_frontFullscreenProgram : this->m_frontProgram);
    const glm::mat4 inverse = glm::inverse (viewProjection);
    const glm::vec3 color = light.color->value->getVec3 ();

    glUseProgram (program);
    glActiveTexture (GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, this->m_back.texture);
    glUniform1i (glGetUniformLocation (program, "u_Back"), 0);
    glUniformMatrix4fv (glGetUniformLocation (program, "u_ViewProjection"), 1, GL_FALSE, &viewProjection[0][0]);
    glUniformMatrix4fv (glGetUniformLocation (program, "u_Volume"), 1, GL_FALSE, &volume[0][0]);
    glUniformMatrix4fv (glGetUniformLocation (program, "u_InverseViewProjection"), 1, GL_FALSE, &inverse[0][0]);
    glUniform2f (glGetUniformLocation (program, "u_BufferSize"), static_cast<float> (size.x), static_cast<float> (size.y));
    glUniform3fv (glGetUniformLocation (program, "u_LightOrigin"), 1, &origin[0]);
    // g_RenderVar1.x is 99% of the radius, the falloff ends just inside the mesh
    glUniform1f (glGetUniformLocation (program, "u_Radius"), radius * 0.99f);
    glUniform1f (glGetUniformLocation (program, "u_Intensity"), light.intensity->value->getFloat ());
    glUniform1f (glGetUniformLocation (program, "u_Density"), light.density->value->getFloat ());
    glUniform1f (glGetUniformLocation (program, "u_Exponent"), light.volumetricsExponent->value->getFloat ());
    glUniform3fv (glGetUniformLocation (program, "u_Color"), 1, &color[0]);
    glUniform3fv (glGetUniformLocation (program, "u_EyePosition"), 1, &fog.eyeWorld[0]);
    glUniform3fv (glGetUniformLocation (program, "g_FogDistanceColor"), 1, &fog.distanceColor[0]);
    glUniform4fv (glGetUniformLocation (program, "g_FogDistanceParams"), 1, &fog.distanceParams[0]);
    glUniform3fv (glGetUniformLocation (program, "g_FogHeightColor"), 1, &fog.heightColor[0]);
    glUniform4fv (glGetUniformLocation (program, "g_FogHeightParams"), 1, &fog.heightParamsWorld[0]);
    glDrawElements (GL_TRIANGLES, this->m_sphereIndexCount, GL_UNSIGNED_SHORT, nullptr);

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

    glBindVertexArray (this->m_vao);
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
	glUniform2f (glGetUniformLocation (this->m_blurProgram, "u_Direction"), 1.0f / static_cast<float> (this->m_size.x), 0.0f);
	glDrawArrays (GL_TRIANGLES, 0, 3);

	glBindFramebuffer (GL_FRAMEBUFFER, this->m_light.framebuffer);
	glBindTexture (GL_TEXTURE_2D, this->m_lightB.texture);
	glUniform2f (glGetUniformLocation (this->m_blurProgram, "u_Direction"), 0.0f, 1.0f / static_cast<float> (this->m_size.y));
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
