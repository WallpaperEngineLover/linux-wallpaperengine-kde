#include "CSplat.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "WallpaperEngine/Application/ApplicationContext.h"
#include "WallpaperEngine/Application/WallpaperApplication.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Splat/SogLoader.h"

using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Render::Wallpapers;

namespace WallpaperEngine::Render::Wallpapers {
// Orders splats back to front along the view direction with a 16 bit counting sort on a worker thread,
// the result is a list of splat indices to draw as instances.
class SplatSorter {
public:
    SplatSorter (std::vector<glm::vec3> centers, std::vector<uint32_t> ids) :
	m_centers (std::move (centers)), m_ids (std::move (ids)) {
	this->m_worker = std::thread (&SplatSorter::run, this);
    }

    ~SplatSorter () {
	{
	    std::lock_guard lock (this->m_mutex);
	    this->m_quit = true;
	}

	this->m_wake.notify_one ();
	this->m_worker.join ();
    }

    void sortNow (const glm::vec3& position, const glm::vec3& forward, std::vector<uint32_t>& out) const {
	Scratch scratch;
	this->sort (position, forward, scratch, out);
    }

    // Asks the worker for a new order; if it is still busy with an older request, that one is replaced
    void request (const glm::vec3& position, const glm::vec3& forward) {
	{
	    std::lock_guard lock (this->m_mutex);
	    this->m_requestPosition = position;
	    this->m_requestForward = forward;
	    this->m_hasRequest = true;
	}

	this->m_wake.notify_one ();
    }

    bool takeResult (std::vector<uint32_t>& out) {
	std::lock_guard lock (this->m_mutex);

	if (!this->m_hasResult) {
	    return false;
	}

	out = std::move (this->m_result);
	this->m_hasResult = false;

	return true;
    }

private:
    struct Scratch {
	std::vector<float> depths;
	std::vector<uint32_t> keys;
	std::vector<uint32_t> counts;
    };

    void sort (const glm::vec3& position, const glm::vec3& forward, Scratch& scratch, std::vector<uint32_t>& out) const {
	constexpr float nearPlane = 0.05f;
	constexpr uint32_t bucketCount = 65536;
	constexpr uint32_t skipped = 0xFFFFFFFFu;

	const size_t count = this->m_centers.size ();

	scratch.depths.resize (count);
	scratch.keys.resize (count);
	scratch.counts.assign (bucketCount + 1, 0);

	float nearest = std::numeric_limits<float>::max ();
	float farthest = std::numeric_limits<float>::lowest ();

	for (size_t i = 0; i < count; i++) {
	    const float depth = glm::dot (this->m_centers[i] - position, forward);

	    scratch.depths[i] = depth;

	    if (depth > nearPlane) {
		nearest = std::min (nearest, depth);
		farthest = std::max (farthest, depth);
	    }
	}

	out.clear ();

	if (farthest < nearest) {
	    return;
	}

	const float scale = static_cast<float> (bucketCount - 1) / std::max (farthest - nearest, 1e-6f);
	size_t visible = 0;

	for (size_t i = 0; i < count; i++) {
	    if (scratch.depths[i] <= nearPlane) {
		scratch.keys[i] = skipped;
		continue;
	    }

	    // farthest first, so the largest depth lands in bucket 0
	    const uint32_t key = static_cast<uint32_t> ((farthest - scratch.depths[i]) * scale);

	    scratch.keys[i] = std::min (key, bucketCount - 1);
	    scratch.counts[scratch.keys[i] + 1]++;
	    visible++;
	}

	for (uint32_t bucket = 0; bucket < bucketCount; bucket++) {
	    scratch.counts[bucket + 1] += scratch.counts[bucket];
	}

	out.resize (visible);

	for (size_t i = 0; i < count; i++) {
	    if (scratch.keys[i] != skipped) {
		out[scratch.counts[scratch.keys[i]]++] = this->m_ids[i];
	    }
	}
    }

    void run () {
	Scratch scratch;
	std::unique_lock lock (this->m_mutex);

	while (true) {
	    this->m_wake.wait (lock, [this] { return this->m_quit || this->m_hasRequest; });

	    if (this->m_quit) {
		return;
	    }

	    const glm::vec3 position = this->m_requestPosition;
	    const glm::vec3 forward = this->m_requestForward;
	    this->m_hasRequest = false;

	    lock.unlock ();

	    std::vector<uint32_t> result;
	    this->sort (position, forward, scratch, result);

	    lock.lock ();
	    this->m_result = std::move (result);
	    this->m_hasResult = true;
	}
    }

    const std::vector<glm::vec3> m_centers;
    const std::vector<uint32_t> m_ids;

    std::mutex m_mutex;
    std::condition_variable m_wake;
    glm::vec3 m_requestPosition = {};
    glm::vec3 m_requestForward = {};
    bool m_hasRequest = false;
    std::vector<uint32_t> m_result;
    bool m_hasResult = false;
    bool m_quit = false;
    std::thread m_worker;
};
} // namespace WallpaperEngine::Render::Wallpapers

namespace {
// Gaussian splat rasterizer: camera facing quad instances, the screen space ellipse comes from the projected 3D covariance (EWA splatting).
const char* SPLAT_VERTEX_SHADER = R"(#version 330
precision highp float;

uniform usampler2D u_Centers;
uniform sampler2D u_Rotations;
uniform sampler2D u_Scales;
uniform mat4 u_View;
uniform mat4 u_Projection;
uniform vec2 u_Viewport;
uniform vec2 u_Focal;
uniform int u_TextureWidth;

// The clock: a seven segment display in screen space. Splats inside a digit segment are inverted and float up,
// the splats where the digits end up are faded out so they read against the background.
uniform vec4 u_ClockPoints[32];
uniform vec4 u_ClockParams;
uniform vec4 u_ClockBounds;
uniform float u_ClockDistance;

in vec2 a_Corner;
in uint a_Index;

out vec4 v_Color;
out vec2 v_Corner;

float clockField (vec2 screen, vec2 maskOffset) {
    vec2 toCircle = vec2 (max (1.0, u_ClockParams.w), 1.0);
    float radius = u_ClockParams.z;
    float field = 0.0;

    for (int i = 0; i < 32; i++) {
        vec4 point = u_ClockPoints[i];

        if (abs (point.z) <= 0.001) {
            continue;
        }

        vec2 delta = (screen - (point.xy + maskOffset)) * toCircle;
        float halfLength = abs (point.w);
        float dist = length (delta);

        if (halfLength > 0.0001) {
            dist = point.w >= 0.0
                ? length (vec2 (max (abs (delta.x) - halfLength, 0.0), delta.y))
                : length (vec2 (delta.x, max (abs (delta.y) - halfLength, 0.0)));
        }

        float influence = dist <= radius ? point.z : 0.0;

        if (abs (influence) > abs (field)) {
            field = influence;
        }
    }

    return field;
}

void main () {
    ivec2 texel = ivec2 (int (a_Index) % u_TextureWidth, int (a_Index) / u_TextureWidth);
    uvec4 packedCenter = texelFetch (u_Centers, texel, 0);
    vec3 center = uintBitsToFloat (packedCenter.xyz);

    vec4 camera = u_View * vec4 (center, 1.0);
    float depth = -camera.z;
    vec4 clip = u_Projection * camera;

    if (depth < 0.05 || abs (clip.x) > clip.w * 1.3 || abs (clip.y) > clip.w * 1.3) {
        gl_Position = vec4 (0.0, 0.0, 2.0, 1.0);
        v_Color = vec4 (0.0);
        v_Corner = vec2 (0.0);
        return;
    }

    vec4 q = texelFetch (u_Rotations, texel, 0);
    vec3 s = texelFetch (u_Scales, texel, 0).xyz;

    float x = q.x, y = q.y, z = q.z, w = q.w;
    mat3 rotation = mat3 (
        1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y + w * z), 2.0 * (x * z - w * y),
        2.0 * (x * y - w * z), 1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z + w * x),
        2.0 * (x * z + w * y), 2.0 * (y * z - w * x), 1.0 - 2.0 * (x * x + y * y)
    );

    mat3 axes = mat3 (u_View) * mat3 (rotation[0] * s.x, rotation[1] * s.y, rotation[2] * s.z);
    mat3 covariance = axes * transpose (axes);

    float invDepth = 1.0 / depth;
    vec3 jacobianX = vec3 (u_Focal.x * invDepth, 0.0, u_Focal.x * camera.x * invDepth * invDepth);
    vec3 jacobianY = vec3 (0.0, u_Focal.y * invDepth, u_Focal.y * camera.y * invDepth * invDepth);

    vec3 covarianceX = covariance * jacobianX;
    vec3 covarianceY = covariance * jacobianY;

    // the 0.3 is the usual low pass filter that keeps sub-pixel splats from vanishing
    float a = dot (jacobianX, covarianceX) + 0.3;
    float b = dot (jacobianX, covarianceY);
    float c = dot (jacobianY, covarianceY) + 0.3;

    float mid = 0.5 * (a + c);
    float radius = length (vec2 (0.5 * (a - c), b));
    float lambda1 = mid + radius;
    float lambda2 = max (mid - radius, 0.1);

    vec2 direction = abs (b) > 1e-6 ? normalize (vec2 (b, lambda1 - a)) : (a >= c ? vec2 (1.0, 0.0) : vec2 (0.0, 1.0));
    vec2 major = min (sqrt (2.0 * lambda1), 1024.0) * direction;
    vec2 minor = min (sqrt (2.0 * lambda2), 1024.0) * vec2 (direction.y, -direction.x);

    vec2 offset = a_Corner.x * major + a_Corner.y * minor;

    vec2 centerNdc = clip.xy / clip.w;
    vec2 ndc = centerNdc + offset * 2.0 / u_Viewport;

    uint rgba = packedCenter.w;
    v_Color = vec4 (float (rgba & 255u), float ((rgba >> 8) & 255u), float ((rgba >> 16) & 255u), float (rgba >> 24)) / 255.0;

    vec2 screen = vec2 (centerNdc.x * 0.5 + 0.5, 0.5 - centerNdc.y * 0.5);

    if (u_ClockParams.x > 0.0 && screen.x >= u_ClockBounds.x && screen.y >= u_ClockBounds.y
        && screen.x <= u_ClockBounds.z && screen.y <= u_ClockBounds.w) {
        float radius = u_ClockParams.z;
        float lift = u_ClockParams.y;
        float influence = clockField (screen, vec2 (0.0));

        if (influence > 0.001) {
            v_Color.rgb = 1.0 - v_Color.rgb;
        } else {
            float destination = max (0.0, clockField (screen, vec2 (0.0, -radius * (0.58 + 2.35) * lift)));

            v_Color.a *= 1.0 - clamp (u_ClockDistance * destination * 0.72, 0.0, 0.96);
        }

        if (abs (influence) > 0.001) {
            float animation = abs (influence);
            float seed = fract (sin ((float (a_Index) * 29.29 + 7.7) * 12.9898) * 43758.5453);
            float liftPhase = smoothstep (0.0, 0.48, animation);
            float risePhase = smoothstep (0.34, 1.0, animation);
            float verticalSign = influence >= 0.0 ? -1.0 : 1.0;
            vec2 screenOffset = vec2 (
                (seed - 0.5) * radius * 0.10, verticalSign * radius * (0.58 * liftPhase + 2.35 * risePhase)
            ) * lift;

            // screen space is y down, clip space y up
            ndc += vec2 (screenOffset.x * 2.0, -screenOffset.y * 2.0);
        }
    }

    // the engine samples wallpaper textures with row 0 at the top, but a GL render target stores row
    // 0 at the bottom, so the whole frame is drawn upside down here
    gl_Position = vec4 (ndc.x, -ndc.y, clip.z / clip.w, 1.0);
    v_Corner = a_Corner;
}
)";

const char* SPLAT_FRAGMENT_SHADER = R"(#version 330
precision highp float;

in vec4 v_Color;
in vec2 v_Corner;

out vec4 out_FragColor;

void main () {
    float falloff = -dot (v_Corner, v_Corner);

    if (falloff < -4.0) {
        discard;
    }

    float alpha = exp (falloff) * v_Color.a;

    if (alpha < 1.0 / 255.0) {
        discard;
    }

    out_FragColor = vec4 (v_Color.rgb, alpha);
}
)";

GLuint compileShader (GLenum type, const char* source) {
    const GLuint shader = glCreateShader (type);

    glShaderSource (shader, 1, &source, nullptr);
    glCompileShader (shader);

    GLint status = GL_FALSE;
    glGetShaderiv (shader, GL_COMPILE_STATUS, &status);

    if (status != GL_TRUE) {
	GLint length = 0;
	glGetShaderiv (shader, GL_INFO_LOG_LENGTH, &length);
	std::string log (std::max (length, 1), '\0');
	glGetShaderInfoLog (shader, length, nullptr, log.data ());
	glDeleteShader (shader);
	sLog.exception ("Cannot compile the splat shader: ", log);
    }

    return shader;
}

float quantile (const std::vector<float>& sorted, float fraction) {
    const auto index = static_cast<long> (std::lround ((sorted.size () - 1) * fraction));

    return sorted[std::clamp<long> (index, 0, static_cast<long> (sorted.size ()) - 1)];
}

// Horizontal/vertical extent of the frustum at unit distance, given the wallpaper camera's fov
// (horizontal for landscape viewports, vertical for portrait, matching the viewer)
glm::vec2 frustumTangents (float fovDegrees, int width, int height) {
    const float w = static_cast<float> (std::max (width, 1));
    const float h = static_cast<float> (std::max (height, 1));
    const float aspect = w / h;
    const float tangent = std::tan (std::clamp (fovDegrees, 1.0f, 179.0f) * glm::pi<float> () / 360.0f);

    return w > h ? glm::vec2 (tangent, tangent / aspect) : glm::vec2 (tangent * aspect, tangent);
}
} // namespace

bool CSplat::supports (const Project& project) {
    const auto& properties = project.properties;

    return properties.contains ("sogPreset") && properties.contains ("sogmeta") && properties.contains ("sogdirectory");
}

CSplat::CSplat (
    const Wallpaper& wallpaper, RenderContext& context, AudioContext& audioContext,
    const WallpaperState::TextureUVsScaling& scalingMode, const uint32_t& clampMode
) : CWallpaper (wallpaper, context, audioContext, scalingMode, clampMode) {
    this->setupFramebuffers ();
    this->loadCloud ();
}

CSplat::~CSplat () {
    // stops the worker before the buffers it never touches are freed
    this->m_sorter.reset ();

    glDeleteProgram (this->m_program);
    glDeleteVertexArrays (1, &this->m_vao);
    glDeleteBuffers (1, &this->m_cornerBuffer);
    glDeleteBuffers (1, &this->m_orderBuffer);
    glDeleteTextures (1, &this->m_centerTexture);
    glDeleteTextures (1, &this->m_rotationTexture);
    glDeleteTextures (1, &this->m_scaleTexture);
}

double CSplat::numberProperty (const std::string& name, double fallback) const {
    const auto& properties = this->getWallpaperData ().project.properties;
    const auto property = properties.find (name);

    if (property == properties.end ()) {
	return fallback;
    }

    const std::string text = property->second->toString ();

    if (text == "true") {
	return 1.0;
    }

    if (text == "false") {
	return 0.0;
    }

    try {
	return std::stod (text);
    } catch (const std::exception&) {
	return fallback;
    }
}

std::string CSplat::stringProperty (const std::string& name) const {
    const auto& properties = this->getWallpaperData ().project.properties;
    const auto property = properties.find (name);

    return property == properties.end () ? std::string () : property->second->toString ();
}

void CSplat::loadCloud () {
    const auto& locator = *this->getWallpaperData ().project.assetLocator;

    // same source selection as the wallpaper's own page: a workshop preset carries its own splat
    // data (inline meta + a directory of images), everything else uses one of the bundled scenes
    std::string meta = this->stringProperty ("sogmeta");
    std::string directory = this->stringProperty ("sogdirectory");

    std::replace (directory.begin (), directory.end (), '\\', '/');

    while (!directory.empty () && directory.back () == '/') {
	directory.pop_back ();
    }

    this->m_containFraming = !meta.empty () && !directory.empty ();

    if (meta.empty () || directory.empty ()) {
	const int preset = static_cast<int> (this->numberProperty ("sogPreset", 1));

	if (preset != 1 && preset != 2) {
	    sLog.exception ("This SOG wallpaper uses a custom splat file, which the native renderer cannot load");
	}

	directory = "imgs/" + std::to_string (preset);
	meta = locator.readString (directory + "/meta.json");
    }

    Splat::SplatCloud cloud;

    try {
	cloud = Splat::loadSog (meta, [&] (const std::string& name) { return locator.readString (directory + "/" + name); });
    } catch (const std::exception& e) {
	sLog.exception ("Cannot load the splat cloud from ", directory, ": ", e.what ());
    }

    this->m_cloudCount = cloud.count;
    this->m_textureWidth = cloud.textureWidth;

    // The viewer hangs the splats off an entity rotated 180 degrees around Z (the photo frame has y
    // pointing down), so its camera math all happens in that frame: (x, y, z) -> (-x, -y, z).
    std::vector<glm::vec3> worldCenters;
    std::vector<uint32_t> drawIds;
    worldCenters.reserve (cloud.count);
    drawIds.reserve (cloud.count);

    std::vector<float> horizontalAngles;
    std::vector<float> verticalAngles;
    std::vector<float> depths;

    const uint32_t step = std::max (1u, (cloud.count + 119999) / 120000);
    glm::vec3 boundsMin (std::numeric_limits<float>::max ());
    glm::vec3 boundsMax (std::numeric_limits<float>::lowest ());

    for (uint32_t i = 0; i < cloud.count; i++) {
	const float* centerAndColor = &cloud.centerAndColor[static_cast<size_t> (i) * 4];
	const glm::vec3 world (-centerAndColor[0], -centerAndColor[1], centerAndColor[2]);

	boundsMin = glm::min (boundsMin, world);
	boundsMax = glm::max (boundsMax, world);

	uint32_t packedColor;
	std::memcpy (&packedColor, &centerAndColor[3], sizeof (packedColor));

	// fully transparent splats never contribute a pixel
	if ((packedColor >> 24) != 0) {
	    worldCenters.push_back (world);
	    drawIds.push_back (i);
	}

	if (i % step == 0 && world.z > 1e-4f) {
	    horizontalAngles.push_back (std::atan2 (world.x, world.z));
	    verticalAngles.push_back (std::atan2 (world.y, world.z));
	    depths.push_back (world.z);
	}
    }

    this->m_profile.boundsCenter = (boundsMin + boundsMax) * 0.5f;
    this->m_profile.boundsHalfExtents = glm::max ((boundsMax - boundsMin) * 0.5f, glm::vec3 (0.001f));

    if (depths.size () >= 128) {
	std::sort (horizontalAngles.begin (), horizontalAngles.end ());
	std::sort (verticalAngles.begin (), verticalAngles.end ());
	std::sort (depths.begin (), depths.end ());

	const float horizontalMin = quantile (horizontalAngles, 0.02f);
	const float horizontalMax = quantile (horizontalAngles, 0.98f);
	const float verticalMin = quantile (verticalAngles, 0.02f);
	const float verticalMax = quantile (verticalAngles, 0.98f);

	this->m_profile.direction = glm::normalize (glm::vec3 (
	    std::tan ((horizontalMin + horizontalMax) * 0.5f), std::tan ((verticalMin + verticalMax) * 0.5f), 1.0f
	));
	this->m_profile.angularSize
	    = { std::max (horizontalMax - horizontalMin, 0.01f), std::max (verticalMax - verticalMin, 0.01f) };
	this->m_profile.depthMin = quantile (depths, 0.02f);
	this->m_profile.depthMax = std::max (quantile (depths, 0.98f), this->m_profile.depthMin + 0.01f);
    } else {
	sLog.error ("SOG wallpaper has too few splats in front of the camera to derive its framing, using defaults");
    }

    sLog.out (
	"Loaded ", cloud.count, " splats (", drawIds.size (), " visible), depth ", this->m_profile.depthMin, " - ",
	this->m_profile.depthMax
    );

    this->setupGL (cloud.centerAndColor, cloud.rotation, cloud.scale);
    this->m_sorter = std::make_unique<SplatSorter> (std::move (worldCenters), std::move (drawIds));
}

void CSplat::setupGL (
    const std::vector<float>& centers, const std::vector<float>& rotations, const std::vector<float>& scales
) {
    const GLsizei width = static_cast<GLsizei> (this->m_textureWidth);
    const GLsizei height = static_cast<GLsizei> (centers.size () / 4 / this->m_textureWidth);

    const auto makeTexture = [] (GLuint& texture) {
	glGenTextures (1, &texture);
	glBindTexture (GL_TEXTURE_2D, texture);
	// integer textures cannot be filtered, and the shader only ever texelFetch()es anyway
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };

    // integer texture, a float one may flush denormals and eat some colors
    makeTexture (this->m_centerTexture);
    glTexImage2D (
	GL_TEXTURE_2D, 0, GL_RGBA32UI, width, height, 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, centers.data ()
    );

    makeTexture (this->m_rotationTexture);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, rotations.data ());

    makeTexture (this->m_scaleTexture);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, scales.data ());

    glBindTexture (GL_TEXTURE_2D, 0);

    const GLuint vertexShader = compileShader (GL_VERTEX_SHADER, SPLAT_VERTEX_SHADER);
    const GLuint fragmentShader = compileShader (GL_FRAGMENT_SHADER, SPLAT_FRAGMENT_SHADER);

    this->m_program = glCreateProgram ();
    glAttachShader (this->m_program, vertexShader);
    glAttachShader (this->m_program, fragmentShader);
    glLinkProgram (this->m_program);

    GLint linked = GL_FALSE;
    glGetProgramiv (this->m_program, GL_LINK_STATUS, &linked);

    if (linked != GL_TRUE) {
	GLint length = 0;
	glGetProgramiv (this->m_program, GL_INFO_LOG_LENGTH, &length);
	std::string log (std::max (length, 1), '\0');
	glGetProgramInfoLog (this->m_program, length, nullptr, log.data ());
	sLog.exception ("Cannot link the splat shader: ", log);
    }

    glDetachShader (this->m_program, vertexShader);
    glDetachShader (this->m_program, fragmentShader);
    glDeleteShader (vertexShader);
    glDeleteShader (fragmentShader);

    this->u_Centers = glGetUniformLocation (this->m_program, "u_Centers");
    this->u_Rotations = glGetUniformLocation (this->m_program, "u_Rotations");
    this->u_Scales = glGetUniformLocation (this->m_program, "u_Scales");
    this->u_View = glGetUniformLocation (this->m_program, "u_View");
    this->u_Projection = glGetUniformLocation (this->m_program, "u_Projection");
    this->u_Viewport = glGetUniformLocation (this->m_program, "u_Viewport");
    this->u_Focal = glGetUniformLocation (this->m_program, "u_Focal");
    this->u_TextureWidth = glGetUniformLocation (this->m_program, "u_TextureWidth");
    this->u_ClockPoints = glGetUniformLocation (this->m_program, "u_ClockPoints");
    this->u_ClockParams = glGetUniformLocation (this->m_program, "u_ClockParams");
    this->u_ClockBounds = glGetUniformLocation (this->m_program, "u_ClockBounds");
    this->u_ClockDistance = glGetUniformLocation (this->m_program, "u_ClockDistance");

    const GLint cornerAttribute = glGetAttribLocation (this->m_program, "a_Corner");
    const GLint indexAttribute = glGetAttribLocation (this->m_program, "a_Index");

    constexpr GLfloat corners[] = { -2.0f, -2.0f, 2.0f, -2.0f, -2.0f, 2.0f, 2.0f, 2.0f };

    glGenVertexArrays (1, &this->m_vao);
    glBindVertexArray (this->m_vao);

    glGenBuffers (1, &this->m_cornerBuffer);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_cornerBuffer);
    glBufferData (GL_ARRAY_BUFFER, sizeof (corners), corners, GL_STATIC_DRAW);
    glEnableVertexAttribArray (cornerAttribute);
    glVertexAttribPointer (cornerAttribute, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    glGenBuffers (1, &this->m_orderBuffer);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_orderBuffer);
    glEnableVertexAttribArray (indexAttribute);
    glVertexAttribIPointer (indexAttribute, 1, GL_UNSIGNED_INT, 0, nullptr);
    glVertexAttribDivisor (indexAttribute, 1);

    glBindVertexArray (GL_NONE);
    glBindBuffer (GL_ARRAY_BUFFER, GL_NONE);
}

void CSplat::resizeOutput (int width, int height) {
    this->m_width = width;
    this->m_height = height;
    this->m_hasBase = false;
    this->m_hasDrawn = false;

    glBindTexture (GL_TEXTURE_2D, this->getWallpaperTexture ());
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture (GL_TEXTURE_2D, 0);
}

std::vector<CSplat::ClockPoint> CSplat::buildClockPoints (
    const std::string& text, float aspect, float size, float originXPercent, float originYPercent
) const {
    // seven segment layout in a digit's own units: segments a-g as (x1, y1, x2, y2)
    static constexpr float SEGMENT_LINES[7][4] = {
	{ 0.18f, 0.00f, 0.82f, 0.00f }, { 1.00f, 0.14f, 1.00f, 0.68f }, { 1.00f, 0.92f, 1.00f, 1.46f },
	{ 0.18f, 1.60f, 0.82f, 1.60f }, { 0.00f, 0.92f, 0.00f, 1.46f }, { 0.00f, 0.14f, 0.00f, 0.68f },
	{ 0.18f, 0.80f, 0.82f, 0.80f },
    };
    static constexpr const char* DIGIT_SEGMENTS[10]
	= { "abcdef", "bc", "abged", "abgcd", "fgbc", "afgcd", "afgecd", "abc", "abcdefg", "abcdfg" };
    constexpr float DIGIT_HEIGHT = 1.6f;
    constexpr float DIGIT_ADVANCE = 1.32f;
    constexpr float COLON_WIDTH = 0.38f;
    constexpr float CLOCK_WIDTH = 0.36f;

    struct Raw {
	float x;
	float y;
	bool horizontal;
	float logicalHalfLength;
    };

    std::vector<Raw> raw;
    float cursor = 0.0f;

    for (const char character : text) {
	if (character == ':') {
	    raw.push_back ({ cursor + COLON_WIDTH * 0.5f, 0.54f, false, 0.0f });
	    raw.push_back ({ cursor + COLON_WIDTH * 0.5f, 1.06f, false, 0.0f });
	    cursor += COLON_WIDTH + 0.30f;
	    continue;
	}

	if (character < '0' || character > '9') {
	    continue;
	}

	for (const char* segment = DIGIT_SEGMENTS[character - '0']; *segment != '\0'; segment++) {
	    const float* line = SEGMENT_LINES[*segment - 'a'];
	    const float dx = std::abs (line[2] - line[0]);
	    const float dy = std::abs (line[3] - line[1]);

	    raw.push_back (
		{ cursor + (line[0] + line[2]) * 0.5f, (line[1] + line[3]) * 0.5f, dx >= dy, std::max (dx, dy) * 0.5f }
	    );
	}

	cursor += DIGIT_ADVANCE;
    }

    const float totalWidth = std::max (cursor - 0.16f, 1.0f);
    const float width = CLOCK_WIDTH * size;
    const float height = width * (DIGIT_HEIGHT / totalWidth) * aspect;
    const float originX = std::clamp (originXPercent / 100.0f, 0.02f, 0.98f);
    const float originY = std::clamp (originYPercent / 100.0f, 0.02f, 0.98f);

    std::vector<ClockPoint> points;

    for (size_t i = 0; i < raw.size () && i < CLOCK_POINT_CAPACITY; i++) {
	const Raw& point = raw[i];
	ClockPoint result;

	result.x = std::clamp (originX + (point.x / totalWidth - 0.5f) * width, 0.0f, 1.0f);
	result.y = std::clamp (originY + (point.y / DIGIT_HEIGHT - 0.5f) * height, 0.0f, 1.0f);

	if (point.logicalHalfLength > 0.0f) {
	    result.halfLength = point.horizontal ? point.logicalHalfLength / totalWidth * width * aspect
						 : -point.logicalHalfLength / DIGIT_HEIGHT * height;
	}

	result.strength = 1.0f;
	points.push_back (result);
    }

    return points;
}

bool CSplat::updateClock (float aspect) {
    constexpr float TRANSITION_SECONDS = 0.82f;
    constexpr float BASE_RADIUS = 0.011f;

    const bool enabled = this->numberProperty ("clockEnabled", 1.0) != 0.0;
    const float size = std::clamp (static_cast<float> (this->numberProperty ("clockSize", 1.0)), 0.55f, 1.8f);
    const float x = std::clamp (static_cast<float> (this->numberProperty ("clockX", 50.0)), 8.0f, 92.0f);
    const float y = std::clamp (static_cast<float> (this->numberProperty ("clockY", 18.0)), 8.0f, 92.0f);
    const float lift = std::clamp (static_cast<float> (this->numberProperty ("clockLift", 0.92)), 0.0f, 2.0f);
    const float distance = std::clamp (static_cast<float> (this->numberProperty ("clockDistance", 0.92)), 0.0f, 2.0f);
    const float radius = std::clamp (BASE_RADIUS * size, 0.008f, 0.055f);

    const auto now = std::chrono::steady_clock::now ();
    const std::array<float, 5> layout = { size, x, y, aspect, enabled ? 1.0f : 0.0f };

    const std::time_t wallClock = std::time (nullptr);
    std::tm local {};
    localtime_r (&wallClock, &local);
    char text[8];
    std::snprintf (text, sizeof (text), "%02d:%02d", local.tm_hour, local.tm_min);

    if (layout != this->m_clockLayout) {
	// first frame, or the clock was moved/resized: rebuild it from nothing so it rises in again
	this->m_clockLayout = layout;
	this->m_clockText = text;
	this->m_clockPrevious.clear ();
	this->m_clockCurrent = this->buildClockPoints (this->m_clockText, aspect, size, x, y);
	this->m_clockTransitionStart = now;
    } else if (this->m_clockText != text || this->m_clockCurrent.empty ()) {
	this->m_clockPrevious = this->m_clockCurrent;
	this->m_clockText = text;
	this->m_clockCurrent = this->buildClockPoints (this->m_clockText, aspect, size, x, y);
	this->m_clockTransitionStart = now;
    }

    const float progress = std::clamp (std::chrono::duration<float> (now - this->m_clockTransitionStart).count () / TRANSITION_SECONDS, 0.0f, 1.0f);
    const float rise = 1.0f - std::pow (1.0f - progress, 3.0f);
    const float sink = std::pow (1.0f - progress, 3.0f);

    // points that stay across a minute change keep full strength; new ones rise, old ones sink away
    const auto keyOf = [] (const ClockPoint& point) {
	return std::make_pair (std::lround (point.x * 10000.0f), std::lround (point.y * 10000.0f));
    };
    std::vector<std::pair<long, long>> currentKeys;
    std::vector<std::pair<long, long>> previousKeys;

    for (const auto& point : this->m_clockCurrent) {
	currentKeys.push_back (keyOf (point));
    }

    for (const auto& point : this->m_clockPrevious) {
	previousKeys.push_back (keyOf (point));
    }

    const auto contains = [] (const std::vector<std::pair<long, long>>& keys, const std::pair<long, long>& key) {
	return std::find (keys.begin (), keys.end (), key) != keys.end ();
    };

    std::vector<ClockPoint> packed;

    for (const auto& point : this->m_clockCurrent) {
	ClockPoint entry = point;
	entry.strength = contains (previousKeys, keyOf (point)) ? 1.0f : rise;
	packed.push_back (entry);
    }

    if (progress < 1.0f) {
	for (const auto& point : this->m_clockPrevious) {
	    if (!contains (currentKeys, keyOf (point))) {
		ClockPoint entry = point;
		entry.strength = -sink;
		packed.push_back (entry);
	    }
	}
    }

    std::stable_sort (packed.begin (), packed.end (), [] (const ClockPoint& a, const ClockPoint& b) {
	return std::abs (a.strength) > std::abs (b.strength);
    });

    std::array<glm::vec4, CLOCK_POINT_CAPACITY> points = {};
    bool active = false;
    glm::vec4 bounds (1.0f, 1.0f, 0.0f, 0.0f);
    // where the digits end up floating to: the splats behind that spot get faded out
    const float finalRise = radius * (0.58f + 2.35f) * lift;

    for (size_t i = 0; i < packed.size () && i < CLOCK_POINT_CAPACITY; i++) {
	const ClockPoint& point = packed[i];
	const float strength = enabled ? std::clamp (point.strength, -1.0f, 1.0f) : 0.0f;

	points[i] = { point.x, point.y, strength, point.halfLength };

	if (std::abs (strength) <= 0.001f) {
	    continue;
	}

	active = true;

	// reach of this capsule on screen (x is stretched by the aspect ratio when measuring distance)
	const float halfLength = std::abs (point.halfLength);
	const float reachX = ((point.halfLength > 0.0f ? halfLength : 0.0f) + radius) / std::max (1.0f, aspect);
	const float reachY = (point.halfLength < 0.0f ? halfLength : 0.0f) + radius;

	bounds.x = std::min (bounds.x, point.x - reachX);
	bounds.y = std::min (bounds.y, point.y - reachY - finalRise);
	bounds.z = std::max (bounds.z, point.x + reachX);
	bounds.w = std::max (bounds.w, point.y + reachY);
    }

    const glm::vec4 params (active ? 1.0f : 0.0f, lift, radius, std::max (1.0f, aspect));
    const bool changed = points != this->m_clockPoints || params != this->m_clockParams || distance != this->m_clockDistance;

    this->m_clockPoints = points;
    this->m_clockParams = params;
    this->m_clockBounds = bounds;
    this->m_clockDistance = distance;

    return changed;
}

CSplat::BaseCamera CSplat::buildBaseCamera (int width, int height, float focusDepth) const {
    const SceneProfile& profile = this->m_profile;
    BaseCamera base;

    // the viewer treats a focus depth of 1 or less as "unset" and falls back to 30%
    const float focusPercent = std::clamp (focusDepth <= 1.0f ? 30.0f : focusDepth, 0.0f, 100.0f) / 100.0f;
    const float focusOnAxis = profile.depthMin + (profile.depthMax - profile.depthMin) * focusPercent;
    const float focusDistance = std::max (0.01f, focusOnAxis / std::max (profile.direction.z, 1e-4f));

    base.position = glm::vec3 (0.0f);
    base.target = profile.direction * focusDistance;
    base.distance = focusDistance;
    base.front = glm::normalize (base.position - base.target);
    base.right = glm::normalize (glm::cross (glm::vec3 (0.0f, 1.0f, 0.0f), profile.direction));
    base.up = glm::normalize (glm::cross (profile.direction, base.right));

    const float inverseFocus = 1.0f / std::max (focusOnAxis, 1e-4f);
    base.parallaxDepthFactor = std::max (
	std::abs (1.0f / std::max (profile.depthMin, 1e-4f) - inverseFocus),
	std::abs (1.0f / std::max (profile.depthMax, 1e-4f) - inverseFocus)
    );

    const float cameraSide = base.front.z < 0.0f ? -1.0f : 1.0f;
    base.frontZ = profile.boundsCenter.z + cameraSide * profile.boundsHalfExtents.z;

    // Fit the photo's angular size to the viewport. The base page crops to fill (the smaller of the
    // two fits); presets show the whole photo (the larger). Both keep the viewer's 4% breathing room.
    const float aspect = static_cast<float> (width) / static_cast<float> (std::max (height, 1));
    const float horizontal = profile.angularSize.x;
    const float vertical = profile.angularSize.y;
    const float horizontalFit = width > height ? horizontal : 2.0f * std::atan (std::tan (horizontal * 0.5f) / aspect);
    const float verticalFit = width > height ? 2.0f * std::atan (std::tan (vertical * 0.5f) * aspect) : vertical;
    const float fov = this->m_containFraming ? std::max (horizontalFit, verticalFit) : std::min (horizontalFit, verticalFit);

    base.fov = std::clamp (glm::degrees (fov) * 1.04f, 20.0f, 120.0f);
    base.viewport = { width, height };
    base.focusDepth = focusDepth;

    return base;
}

bool CSplat::viewFitsFrontFace (const glm::vec3& position, const glm::vec3& target, const BaseCamera& base) const {
    const glm::vec3 toTarget = target - position;

    if (glm::length (toTarget) <= 1e-6f) {
	return false;
    }

    const glm::vec3 forward = glm::normalize (toTarget);
    const glm::vec3 sideways (forward.z, 0.0f, -forward.x);

    if (glm::length (sideways) <= 1e-6f) {
	return false;
    }

    const glm::vec3 right = glm::normalize (sideways);
    const glm::vec3 up = glm::normalize (glm::cross (forward, right));
    const glm::vec2 tangents = frustumTangents (base.fov, base.viewport.x, base.viewport.y);
    const glm::vec3& center = this->m_profile.boundsCenter;
    const glm::vec3& half = this->m_profile.boundsHalfExtents;
    const float epsilon = std::max (half.x, half.y) * 0.0001f;

    for (const float xSign : { -1.0f, 1.0f }) {
	for (const float ySign : { -1.0f, 1.0f }) {
	    const glm::vec3 ray = forward + right * tangents.x * xSign + up * tangents.y * ySign;

	    if (std::abs (ray.z) <= 1e-6f) {
		return false;
	    }

	    const float distance = (base.frontZ - position.z) / ray.z;

	    if (!std::isfinite (distance) || distance <= 0.0f) {
		return false;
	    }

	    const glm::vec3 hit = position + ray * distance;

	    if (hit.x < center.x - half.x - epsilon || hit.x > center.x + half.x + epsilon
		|| hit.y < center.y - half.y - epsilon || hit.y > center.y + half.y + epsilon) {
		return false;
	    }
	}
    }

    return true;
}

// The viewer's camera: sits at the photo's origin and moves on a sphere around the focus point,
// as far as the mouse asks without the view leaving the front face of the splat cloud.
CSplat::Pose CSplat::posePlacement (
    const BaseCamera& base, glm::vec2 tilt, glm::vec2 orbit, float parallaxStrength
) const {
    const float strength = std::clamp (parallaxStrength, 0.0f, 0.16f);
    float parallaxMove = base.distance * strength * 2.4f;

    if (std::isfinite (base.parallaxDepthFactor) && base.parallaxDepthFactor > 1e-6f) {
	const glm::vec2 tangents = frustumTangents (base.fov, base.viewport.x, base.viewport.y);
	const float screenTangent = std::max (0.001f, std::min (tangents.x, tangents.y));
	const float move = strength * 2.0f * screenTangent / base.parallaxDepthFactor;

	if (std::isfinite (move) && move > 0.0f) {
	    parallaxMove = std::min (move, base.distance * 0.8f);
	}
    }

    const float orbitMove = std::min (std::max (base.distance, 1e-4f) * 0.18f, base.distance * 0.8f);

    const glm::vec3 mouseOffset = base.right * (tilt.x * parallaxMove) + base.up * (-tilt.y * parallaxMove);
    const glm::vec3 orbitOffset = base.right * (orbit.x * orbitMove) + base.up * (-orbit.y * orbitMove);

    const auto fits = [&] (const glm::vec3& offset) -> std::optional<glm::vec3> {
	const float tangentDistance = glm::length (offset);

	if (tangentDistance >= base.distance) {
	    return std::nullopt;
	}

	const float frontDistance = std::sqrt (std::max (base.distance * base.distance - tangentDistance * tangentDistance, 0.0f));
	const glm::vec3 position = base.target + base.front * frontDistance + offset;

	if (!this->viewFitsFrontFace (position, base.target, base)) {
	    return std::nullopt;
	}

	return position;
    };

    // shrink an offset toward zero until the view fits, 14 halvings like the viewer
    const auto shrink = [&] (const glm::vec3& fixed, const glm::vec3& variable, glm::vec3 position) {
	float low = 0.0f;
	float high = 1.0f;

	for (int iteration = 0; iteration < 14; iteration++) {
	    const float middle = (low + high) * 0.5f;

	    if (const auto candidate = fits (fixed + variable * middle)) {
		low = middle;
		position = *candidate;
	    } else {
		high = middle;
	    }
	}

	return position;
    };

    Pose pose;
    pose.target = base.target;

    if (const auto full = fits (orbitOffset + mouseOffset)) {
	pose.position = *full;
    } else if (const auto orbitOnly = fits (orbitOffset)) {
	pose.position = shrink (orbitOffset, mouseOffset, *orbitOnly);
    } else {
	pose.position = base.position;

	if (this->viewFitsFrontFace (pose.position, base.target, base)) {
	    pose.position = shrink (glm::vec3 (0.0f), orbitOffset, pose.position);
	}
    }

    return pose;
}

void CSplat::renderFrame (const glm::ivec4& viewport) {
    if (viewport.z <= 0 || viewport.w <= 0) {
	return;
    }

    if (viewport.z != this->m_width || viewport.w != this->m_height) {
	this->resizeOutput (viewport.z, viewport.w);
    }

    const bool depthEnabled = this->numberProperty ("depthEnabled", 1.0) != 0.0;
    const float focusDepth = static_cast<float> (this->numberProperty ("focusDepth", 30.0));

    if (!this->m_hasBase || this->m_base.viewport != glm::ivec2 (this->m_width, this->m_height)
	|| this->m_base.focusDepth != focusDepth) {
	this->m_base = this->buildBaseCamera (this->m_width, this->m_height, focusDepth);
	this->m_hasBase = true;
    }

    const auto now = std::chrono::steady_clock::now ();
    const float elapsed = this->m_hasLastFrame ? std::chrono::duration<float> (now - this->m_lastFrame).count () : 0.0f;
    const float delta = std::clamp (elapsed, 0.0f, 0.05f);

    this->m_lastFrame = now;
    this->m_hasLastFrame = true;

    // mouse -> target tilt, -1..1 with y up. Skipped for --disable-parallax, and until the wallpaper's
    // depth effect is switched on.
    glm::vec2 targetTilt (0.0f);
    const auto& settings = this->getContext ().getApp ().getContext ().settings;

    if (depthEnabled && !settings.mouse.disableparallax) {
	const glm::dvec2 mouse = this->getContext ().getInputContext ().getMouseInput ().position ();
	const float sensitivity = static_cast<float> (this->numberProperty ("sensorSensitivity", 4.0)) / 4.0f;
	const float normalizedX = std::clamp (static_cast<float> ((mouse.x - viewport.x) / viewport.z), 0.0f, 1.0f);
	const float normalizedY = std::clamp (static_cast<float> ((mouse.y - viewport.y) / viewport.w), 0.0f, 1.0f);

	targetTilt = glm::clamp (glm::vec2 (normalizedX * 2.0f - 1.0f, normalizedY * 2.0f - 1.0f) * sensitivity, -1.0f, 1.0f);
    }

    // the viewer eases toward the target by 14% per 60Hz frame
    const float inertia = 1.0f - std::pow (1.0f - 0.14f, delta * 60.0f);

    this->m_tilt += (targetTilt - this->m_tilt) * inertia;

    for (int axis = 0; axis < 2; axis++) {
	if (std::abs (targetTilt[axis] - this->m_tilt[axis]) < 0.0005f) {
	    this->m_tilt[axis] = targetTilt[axis];
	}
    }

    glm::vec2 tilt = this->m_tilt;
    const float tiltLength = glm::length (tilt);

    if (tiltLength > 0.55f) {
	tilt *= 0.55f / tiltLength;
    }

    glm::vec2 orbit (0.0f);

    if (depthEnabled && this->numberProperty ("orbitEnabled", 0.0) != 0.0) {
	const float speed = static_cast<float> (this->numberProperty ("orbitSpeed", 0.12));
	const float amount = static_cast<float> (this->numberProperty ("orbitAmount", 0.34));

	this->m_orbitPhase = std::fmod (this->m_orbitPhase + delta * speed * glm::two_pi<float> (), glm::two_pi<float> ());
	orbit = { std::cos (this->m_orbitPhase) * amount, std::sin (this->m_orbitPhase) * amount * 0.47f };
    }

    const float parallaxStrength = static_cast<float> (this->numberProperty ("parallaxStrength", 0.08));
    const Pose pose = this->posePlacement (this->m_base, tilt, orbit, parallaxStrength);

    const glm::vec3 forward = glm::normalize (pose.target - pose.position);

    // The splat data stays in the photo's frame, so the world flip the viewer applies to its entity
    // goes into the view matrix instead
    const glm::mat4 flip = glm::scale (glm::mat4 (1.0f), glm::vec3 (-1.0f, -1.0f, 1.0f));
    const glm::mat4 view = glm::lookAt (pose.position, pose.target, glm::vec3 (0.0f, 1.0f, 0.0f)) * flip;

    const glm::vec2 tangents = frustumTangents (this->m_base.fov, this->m_width, this->m_height);
    constexpr float nearPlane = 0.05f;
    constexpr float farPlane = 200.0f;
    glm::mat4 projection (0.0f);
    projection[0][0] = 1.0f / tangents.x;
    projection[1][1] = 1.0f / tangents.y;
    projection[2][2] = -(farPlane + nearPlane) / (farPlane - nearPlane);
    projection[2][3] = -1.0f;
    projection[3][2] = -2.0f * farPlane * nearPlane / (farPlane - nearPlane);

    const glm::vec2 focal (
	static_cast<float> (this->m_width) * 0.5f / tangents.x, static_cast<float> (this->m_height) * 0.5f / tangents.y
    );

    // re-sort only once the camera has moved enough to change the ordering visibly
    const bool moved = !this->m_hasOrder
	|| glm::distance (pose.position, this->m_sortedPosition) > this->m_base.distance * 0.0002f
	|| glm::dot (forward, this->m_sortedForward) < 0.99999f;

    std::vector<uint32_t> order;
    bool haveNewOrder = false;

    if (!this->m_hasOrder) {
	// nothing to draw yet: this one has to be synchronous
	this->m_sorter->sortNow (pose.position, forward, order);
	this->m_hasOrder = true;
	haveNewOrder = true;
    } else if (this->m_sorter->takeResult (order)) {
	haveNewOrder = true;
    }

    if (moved) {
	this->m_sorter->request (pose.position, forward);
	this->m_sortedPosition = pose.position;
	this->m_sortedForward = forward;
    }

    if (haveNewOrder) {
	glBindBuffer (GL_ARRAY_BUFFER, this->m_orderBuffer);
	glBufferData (
	    GL_ARRAY_BUFFER, static_cast<GLsizeiptr> (order.size () * sizeof (uint32_t)), order.data (), GL_STREAM_DRAW
	);
	glBindBuffer (GL_ARRAY_BUFFER, GL_NONE);
	this->m_drawCount = static_cast<uint32_t> (order.size ());
    }

    const bool clockChanged = this->updateClock (static_cast<float> (this->m_width) / static_cast<float> (this->m_height));

    // a still camera and clock produce the exact same frame, which is still sitting in the output texture
    if (this->m_hasDrawn && !haveNewOrder && !clockChanged && view == this->m_drawnView
	&& projection == this->m_drawnProjection) {
	return;
    }

    this->m_drawnView = view;
    this->m_drawnProjection = projection;
    this->m_hasDrawn = true;

    glBindFramebuffer (GL_FRAMEBUFFER, this->getWallpaperFramebuffer ());
    glViewport (0, 0, this->m_width, this->m_height);
    glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
    glClear (GL_COLOR_BUFFER_BIT);

    if (this->m_drawCount == 0) {
	return;
    }

    glDisable (GL_DEPTH_TEST);
    glDisable (GL_CULL_FACE);
    glEnable (GL_BLEND);
    // splats arrive back to front; keep the cleared alpha so the finished frame stays opaque
    glBlendFuncSeparate (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);

    glUseProgram (this->m_program);

    glActiveTexture (GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, this->m_centerTexture);
    glActiveTexture (GL_TEXTURE1);
    glBindTexture (GL_TEXTURE_2D, this->m_rotationTexture);
    glActiveTexture (GL_TEXTURE2);
    glBindTexture (GL_TEXTURE_2D, this->m_scaleTexture);

    glUniform1i (this->u_Centers, 0);
    glUniform1i (this->u_Rotations, 1);
    glUniform1i (this->u_Scales, 2);
    glUniformMatrix4fv (this->u_View, 1, GL_FALSE, glm::value_ptr (view));
    glUniformMatrix4fv (this->u_Projection, 1, GL_FALSE, glm::value_ptr (projection));
    glUniform2f (this->u_Viewport, static_cast<float> (this->m_width), static_cast<float> (this->m_height));
    glUniform2f (this->u_Focal, focal.x, focal.y);
    glUniform1i (this->u_TextureWidth, static_cast<GLint> (this->m_textureWidth));
    glUniform4fv (this->u_ClockPoints, static_cast<GLsizei> (CLOCK_POINT_CAPACITY), glm::value_ptr (this->m_clockPoints[0]));
    glUniform4fv (this->u_ClockParams, 1, glm::value_ptr (this->m_clockParams));
    glUniform4fv (this->u_ClockBounds, 1, glm::value_ptr (this->m_clockBounds));
    glUniform1f (this->u_ClockDistance, this->m_clockDistance);

    glBindVertexArray (this->m_vao);
    glDrawArraysInstanced (GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei> (this->m_drawCount));
    glBindVertexArray (GL_NONE);

    glDisable (GL_BLEND);
    glActiveTexture (GL_TEXTURE0);
}
