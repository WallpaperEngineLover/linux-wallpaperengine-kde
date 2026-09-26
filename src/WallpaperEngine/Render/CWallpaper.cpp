#include <cmath>
#include <tuple>
#include <vector>

#include <stb_image.h>

#include "CWallpaper.h"
#include "WallpaperEngine/Assets/AssetLoadException.h"
#include "WallpaperEngine/Data/Parsers/TextureParser.h"
#include "WallpaperEngine/Data/Utils/BinaryReader.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include "WallpaperEngine/Render/Wallpapers/CSplat.h"
#include "WallpaperEngine/Render/Wallpapers/CVideo.h"
#include "WallpaperEngine/Render/Wallpapers/CWeb.h"

#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"

using namespace WallpaperEngine::Render;
using WallpaperEngine::Data::Utils::BinaryReader;

CWallpaper::CWallpaper (
    const Wallpaper& wallpaperData, RenderContext& context, AudioContext& audioContext,
    const WallpaperState::TextureUVsScaling& scalingMode, const uint32_t& clampMode
) :
    ContextAware (context), FBOProvider (nullptr), m_wallpaperData (wallpaperData), m_audioContext (audioContext),
    m_state (scalingMode, clampMode) {
    glGenVertexArrays (1, &this->m_vaoBuffer);
    glBindVertexArray (this->m_vaoBuffer);

    this->setupShaders ();

    constexpr GLfloat texCoords[] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f };

    // inverted positions so the final texture is rendered properly
    constexpr GLfloat position[] = { -1.0f, 1.0f,  0.0f, 1.0,  1.0f, 0.0f, -1.0f, -1.0f, 0.0f,
				     -1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f,  -1.0f, 0.0f };

    // GL_DYNAMIC_DRAW: render() rewrites this buffer's contents (via glBufferSubData) whenever
    // the wallpaper's UVs change. The attrib pointer/enable state below is captured by the VAO
    // once here rather than redone every render() call - only the buffer's contents change later.
    glGenBuffers (1, &this->m_texCoordBuffer);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_texCoordBuffer);
    glBufferData (GL_ARRAY_BUFFER, sizeof (texCoords), texCoords, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray (this->a_TexCoord);
    glVertexAttribPointer (this->a_TexCoord, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    // Static for the object's lifetime, so its attrib setup also only needs to happen once here.
    glGenBuffers (1, &this->m_positionBuffer);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_positionBuffer);
    glBufferData (GL_ARRAY_BUFFER, sizeof (position), position, GL_STATIC_DRAW);
    glEnableVertexAttribArray (this->a_Position);
    glVertexAttribPointer (this->a_Position, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
}

CWallpaper::~CWallpaper () {
    // programs here only ever have 2 shaders attached (vertex + fragment)
    GLuint attachedShaders[2];
    GLsizei attachedCount = 0;

    glGetAttachedShaders (this->m_shader, 2, &attachedCount, attachedShaders);

    for (auto i = 0; i < attachedCount; i++) {
	glDeleteShader (attachedShaders[i]);
    }

    glDeleteProgram (this->m_shader);

    if (this->m_lutTexture != GL_NONE) {
	glDeleteTextures (1, &this->m_lutTexture);
    }

    glDeleteBuffers (1, &this->m_texCoordBuffer);
    glDeleteBuffers (1, &this->m_positionBuffer);
    glDeleteVertexArrays (1, &this->m_vaoBuffer);
}

const AssetLocator& CWallpaper::getAssetLocator () const { return *this->m_wallpaperData.project.assetLocator; }

const Wallpaper& CWallpaper::getWallpaperData () const { return this->m_wallpaperData; }

GLuint CWallpaper::getWallpaperFramebuffer () const { return this->m_sceneFBO->getFramebuffer (); }

GLuint CWallpaper::getWallpaperTexture () const { return this->m_sceneFBO->getTextureID (0); }

void CWallpaper::setupShaders () {
    const GLuint vertexShaderID = glCreateShader (GL_VERTEX_SHADER);

    const char* sourcePointer = "#version 330\n"
				"precision highp float;\n"
				"in vec3 a_Position;\n"
				"in vec2 a_TexCoord;\n"
				"out vec2 v_TexCoord;\n"
				"void main () {\n"
				"gl_Position = vec4 (a_Position, 1.0);\n"
				"v_TexCoord = a_TexCoord;\n"
				"}";

    glShaderSource (vertexShaderID, 1, &sourcePointer, nullptr);
    glCompileShader (vertexShaderID);

    GLint result = GL_FALSE;
    int infoLogLength = 0;

    glGetShaderiv (vertexShaderID, GL_COMPILE_STATUS, &result);
    glGetShaderiv (vertexShaderID, GL_INFO_LOG_LENGTH, &infoLogLength);

    if (infoLogLength > 0) {
	const auto logBuffer = new char[infoLogLength + 1];
	memset (logBuffer, 0, infoLogLength + 1);
	glGetShaderInfoLog (vertexShaderID, infoLogLength, nullptr, logBuffer);
	const std::string message = logBuffer;
	delete[] logBuffer;
	sLog.exception (message);
    }

    const GLuint fragmentShaderID = glCreateShader (GL_FRAGMENT_SHADER);

    // image filter and color options are assets/shaders/ccsimple.frag
    sourcePointer = "#version 330\n"
		    "precision highp float;\n"
		    "uniform sampler2D g_Texture0;\n"
		    "uniform sampler3D g_Texture1;\n"
		    "uniform vec4 g_Params;\n"
		    "uniform float g_LutParams;\n"
		    "uniform bool u_ColorEnabled;\n"
		    "uniform bool u_LutEnabled;\n"
		    "uniform bool u_InputLinear;\n"
		    "uniform bool u_OutputPQ;\n"
		    "in vec2 v_TexCoord;\n"
		    "out vec4 out_FragColor;\n"
		    // BT.2408 reference white, what the PQ image description of HDR surfaces anchors SDR content to
		    "const float referenceWhite = 203.0;\n"
		    "const mat3 bt709to2020 = mat3 (0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880, 0.0433, 0.0114, 0.8956);\n"
		    "const mat3 bt2020to709 = mat3 (1.6605, -0.1246, -0.0182, -0.5876, 1.1329, -0.1006, -0.0728, -0.0083, "
		    "1.1187);\n"
		    "vec3 srgbToLinear (vec3 c) {\n"
		    "return mix (c / 12.92, pow ((c + 0.055) / 1.055, vec3 (2.4)), step (0.04045, c));\n"
		    "}\n"
		    "vec3 linearToSrgb (vec3 c) {\n"
		    "return mix (c * 12.92, 1.055 * pow (c, vec3 (1.0 / 2.4)) - 0.055, step (0.0031308, c));\n"
		    "}\n"
		    // mpv decodes SDR video with BT.1886, a pure 2.4 power, going back the same way keeps SDR videos
		    // exactly as they look without --hdr
		    "vec3 videoToLinear (vec3 c) { return pow (c, vec3 (2.4)); }\n"
		    "vec3 linearToVideo (vec3 c) { return pow (c, vec3 (1.0 / 2.4)); }\n"
		    "vec3 pqEncode (vec3 nits) {\n"
		    "vec3 y = pow (clamp (nits / 10000.0, 0.0, 1.0), vec3 (0.1593017578125));\n"
		    "return pow ((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), vec3 (78.84375));\n"
		    "}\n"
		    // highlights of HDR video on an SDR output, a soft knee instead of a hard clip
		    "vec3 softClip (vec3 x) {\n"
		    "return mix (x, 0.8 + 0.2 * (1.0 - exp (-(x - 0.8) / 0.2)), step (0.8, x));\n"
		    "}\n"
		    "vec3 hsv2rgb (vec3 c) {\n"
		    "vec4 K = vec4 (1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);\n"
		    "vec3 p = abs (fract (c.xxx + K.xyz) * 6.0 - K.www);\n"
		    "return c.z * mix (K.xxx, clamp (p - K.xxx, 0.0, 1.0), c.y);\n"
		    "}\n"
		    "vec3 rgb2hsv (vec3 RGB) {\n"
		    "vec4 P = (RGB.g < RGB.b) ? vec4 (RGB.bg, -1.0, 2.0 / 3.0) : vec4 (RGB.gb, 0.0, -1.0 / 3.0);\n"
		    "vec4 Q = (RGB.r < P.x) ? vec4 (P.xyw, RGB.r) : vec4 (RGB.r, P.yzx);\n"
		    "float C = Q.x - min (Q.w, Q.y);\n"
		    "float H = abs ((Q.w - Q.y) / (6.0 * C + 1e-10) + Q.z);\n"
		    "return vec3 (H, C / (Q.x + 1e-10), Q.x);\n"
		    "}\n"
		    "void main () {\n"
		    "vec4 albedo = texture (g_Texture0, v_TexCoord);\n"
		    "bool filtered = u_ColorEnabled || u_LutEnabled;\n"
		    "vec3 overbright = vec3 (0.0);\n"
		    // the filters work on SDR-encoded values like in Wallpaper Engine, what's past reference white is kept aside
		    "if (u_InputLinear && filtered) {\n"
		    "vec3 light = max (bt2020to709 * albedo.rgb, 0.0);\n"
		    "overbright = max (light - 1.0, 0.0);\n"
		    "albedo.rgb = linearToVideo (min (light, 1.0));\n"
		    "}\n"
		    "if (u_ColorEnabled) {\n"
		    "albedo.rgb = mix (vec3 (0.5), albedo.rgb, g_Params.y);\n"
		    "vec3 hsv = rgb2hsv (albedo.rgb);\n"
		    "hsv.z *= g_Params.x;\n"
		    "hsv.y *= g_Params.z;\n"
		    "hsv.x += g_Params.w;\n"
		    "albedo.rgb = hsv2rgb (hsv);\n"
		    "}\n"
		    "if (u_LutEnabled) {\n"
		    "albedo.rgb = mix (albedo.rgb, texture (g_Texture1, albedo.rgb).rgb, g_LutParams);\n"
		    "}\n"
		    "if (u_InputLinear || u_OutputPQ) {\n"
		    "vec3 encoded = clamp (albedo.rgb, 0.0, 1.0);\n"
		    "vec3 linear2020 = !u_InputLinear ? bt709to2020 * srgbToLinear (encoded)\n"
		    "    : filtered ? bt709to2020 * (videoToLinear (encoded) + overbright) : albedo.rgb;\n"
		    "albedo.rgb = u_OutputPQ ? pqEncode (max (linear2020, 0.0) * referenceWhite)\n"
		    "    : linearToVideo (min (softClip (max (bt2020to709 * linear2020, 0.0)), 1.0));\n"
		    "}\n"
		    "out_FragColor = albedo;\n"
		    "}";

    glShaderSource (fragmentShaderID, 1, &sourcePointer, nullptr);
    glCompileShader (fragmentShaderID);

    result = GL_FALSE;
    infoLogLength = 0;

    glGetShaderiv (fragmentShaderID, GL_COMPILE_STATUS, &result);
    glGetShaderiv (fragmentShaderID, GL_INFO_LOG_LENGTH, &infoLogLength);

    if (infoLogLength > 0) {
	const auto logBuffer = new char[infoLogLength + 1];
	memset (logBuffer, 0, infoLogLength + 1);
	glGetShaderInfoLog (fragmentShaderID, infoLogLength, nullptr, logBuffer);
	const std::string message = logBuffer;
	delete[] logBuffer;
	sLog.exception (message);
    }

    this->m_shader = glCreateProgram ();
    glAttachShader (this->m_shader, vertexShaderID);
    glAttachShader (this->m_shader, fragmentShaderID);
    glLinkProgram (this->m_shader);
    result = GL_FALSE;
    infoLogLength = 0;

    glGetProgramiv (this->m_shader, GL_LINK_STATUS, &result);
    glGetProgramiv (this->m_shader, GL_INFO_LOG_LENGTH, &infoLogLength);

    if (infoLogLength > 0) {
	const auto logBuffer = new char[infoLogLength + 1];
	memset (logBuffer, 0, infoLogLength + 1);
	glGetProgramInfoLog (this->m_shader, infoLogLength, nullptr, logBuffer);
	const std::string message = logBuffer;
	delete[] logBuffer;
	sLog.exception (message);
    }

    // shaders can be detached and deleted once linked into the program
    glDetachShader (this->m_shader, vertexShaderID);
    glDetachShader (this->m_shader, fragmentShaderID);

    glDeleteShader (vertexShaderID);
    glDeleteShader (fragmentShaderID);

    this->g_Texture0 = glGetUniformLocation (this->m_shader, "g_Texture0");
    this->g_Texture1 = glGetUniformLocation (this->m_shader, "g_Texture1");
    this->g_Params = glGetUniformLocation (this->m_shader, "g_Params");
    this->g_LutParams = glGetUniformLocation (this->m_shader, "g_LutParams");
    this->u_ColorEnabled = glGetUniformLocation (this->m_shader, "u_ColorEnabled");
    this->u_LutEnabled = glGetUniformLocation (this->m_shader, "u_LutEnabled");
    this->u_InputLinear = glGetUniformLocation (this->m_shader, "u_InputLinear");
    this->u_OutputPQ = glGetUniformLocation (this->m_shader, "u_OutputPQ");
    this->a_Position = glGetAttribLocation (this->m_shader, "a_Position");
    this->a_TexCoord = glGetAttribLocation (this->m_shader, "a_TexCoord");
}

void CWallpaper::setDestinationFramebuffer (GLuint framebuffer) { this->m_destFramebuffer = framebuffer; }

void CWallpaper::setSpanInfo (const SpanInfo& spanInfo) { this->m_spanInfo = spanInfo; }

const CWallpaper::SpanInfo* CWallpaper::getSpanInfo () const {
    return this->m_spanInfo.has_value () ? &this->m_spanInfo.value () : nullptr;
}

void CWallpaper::updateUVs (const glm::ivec4& viewport, const bool vflip) {
    if (this->m_state.hasChanged (viewport, vflip, this->getCanvasWidth (), this->getCanvasHeight ())) {
	this->m_state.updateState (viewport, vflip, this->getCanvasWidth (), this->getCanvasHeight ());
    }
}

void CWallpaper::render (
    const glm::ivec4& viewport, const bool vflip, const glm::ivec2& globalPosition, const glm::ivec2& logicalSize
) {
    const uint32_t currentFrame = this->getContext ().getDriver ().getFrameCounter ();
    const bool needsSceneRender = (currentFrame != this->m_lastRenderedFrame);
    const glm::ivec4 sceneViewport = this->m_spanInfo.has_value ()
	? glm::ivec4 { 0, 0, this->m_spanInfo->totalBounds.z, this->m_spanInfo->totalBounds.w }
	: viewport;

    this->m_screenSize = { sceneViewport.z, sceneViewport.w };

#if !NDEBUG
    glPushDebugGroup (GL_DEBUG_SOURCE_APPLICATION, 0, -1, "Rendering scene");
#endif /* !NDEBUG */
    if (needsSceneRender) {
	this->renderFrame (sceneViewport);
	this->m_lastRenderedFrame = currentFrame;
    }
#if !NDEBUG
    glPopDebugGroup ();
    glPushDebugGroup (GL_DEBUG_SOURCE_APPLICATION, 0, -1, "Rendering scene to output");
#endif /* !NDEBUG */

    float ustart, uend, vstart, vend;

    if (this->m_spanInfo.has_value ()) {
	// span mode: scale the wallpaper to the bounding box using the normal scaling rules
	// (fill/fit/stretch/default), then slice per monitor
	const auto& span = this->m_spanInfo.value ();
	const float spanW = static_cast<float> (span.totalBounds.z);
	const float spanH = static_cast<float> (span.totalBounds.w);
	const float spanX = static_cast<float> (span.totalBounds.x);
	const float spanY = static_cast<float> (span.totalBounds.y);

	this->updateUVs (span.totalBounds, vflip);
	auto [baseUstart, baseUend, baseVstart, baseVend] = this->m_state.getTextureUVs ();

	// this viewport's relative position within the bounding box [0..1]; logicalSize is in the
	// same coordinate space as globalPosition and totalBounds
	float relLeft = (static_cast<float> (globalPosition.x) - spanX) / spanW;
	float relRight = (static_cast<float> (globalPosition.x + logicalSize.x) - spanX) / spanW;

	// a flipped span mirrors the whole group, the leftmost screen shows the right end of the image
	if (this->m_flipHorizontal) {
	    std::tie (relLeft, relRight) = std::make_pair (1.0f - relLeft, 1.0f - relRight);
	}
	const float relTop = (static_cast<float> (globalPosition.y) - spanY) / spanH;
	const float relBottom = (static_cast<float> (globalPosition.y + logicalSize.y) - spanY) / spanH;

	// interpolate within the base UVs to get this viewport's slice
	const float baseURange = baseUend - baseUstart;
	const float baseVRange = baseVend - baseVstart;

	ustart = baseUstart + relLeft * baseURange;
	uend = baseUstart + relRight * baseURange;
	vstart = baseVstart + relTop * baseVRange;
	vend = baseVstart + relBottom * baseVRange;

	if (this->m_lastRenderedFrame < 5) {
	    sLog.debug (
		"SPAN DEBUG: viewport=", viewport.z, "x", viewport.w, " globalPos=(", globalPosition.x, ",",
		globalPosition.y, ")", " span=(", span.totalBounds.x, ",", span.totalBounds.y, ",", span.totalBounds.z,
		",", span.totalBounds.w, ")", " rel=[", relLeft, ",", relRight, "]x[", relTop, ",", relBottom, "]",
		" baseUV=[", baseUstart, ",", baseUend, "]x[", baseVstart, ",", baseVend, "]", " finalUV=[", ustart,
		",", uend, "]x[", vstart, ",", vend, "]"
	    );
	}
    } else {
	updateUVs (viewport, vflip);
	auto uvs = this->m_state.getTextureUVs ();
	ustart = uvs.ustart;
	uend = uvs.uend;

	if (this->m_flipHorizontal) {
	    std::swap (ustart, uend);
	}
	vstart = uvs.vstart;
	vend = uvs.vend;
    }

    const GLfloat texCoords[] = {
	ustart, vstart, uend, vstart, ustart, vend, ustart, vend, uend, vstart, uend, vend,
    };

    glViewport (viewport.x, viewport.y, viewport.z, viewport.w);

    glBindFramebuffer (GL_FRAMEBUFFER, this->m_destFramebuffer);

    glBindVertexArray (this->m_vaoBuffer);

    if (ustart != this->m_uploadedUstart || uend != this->m_uploadedUend || vstart != this->m_uploadedVstart
	|| vend != this->m_uploadedVend) {
	glBindBuffer (GL_ARRAY_BUFFER, this->m_texCoordBuffer);
	glBufferSubData (GL_ARRAY_BUFFER, 0, sizeof (texCoords), texCoords);
	this->m_uploadedUstart = ustart;
	this->m_uploadedUend = uend;
	this->m_uploadedVstart = vstart;
	this->m_uploadedVend = vend;
    }

    this->drawOutputQuad ();

    if (this->getContext ().getApp ().getContext ().settings.render.debug.brightnessLog) {
	static uint32_t brightnessFrameCounter = 0;
	if ((brightnessFrameCounter++ % 30) == 0) {
	    std::vector<unsigned char> px (viewport.z * viewport.w * 4);
	    glReadPixels (
		viewport.x, viewport.y, viewport.z, viewport.w, GL_RGBA, GL_UNSIGNED_BYTE, px.data ()
	    );
	    uint64_t sum = 0;
	    for (size_t i = 0; i < px.size (); i += 4) {
		sum += px[i] + px[i + 1] + px[i + 2];
	    }
	    const double mean = static_cast<double> (sum) / (px.size () / 4 * 3);
	    sLog.out ("[BRIGHTNESS] frame=", brightnessFrameCounter, " mean=", mean);
	}
    }

#if !NDEBUG
    glPopDebugGroup ();
#endif /* !NDEBUG */
}

void CWallpaper::drawOutputQuad () {
    glDisable (GL_BLEND);
    glDisable (GL_DEPTH_TEST);
    glDisable (GL_CULL_FACE);
    glUseProgram (this->m_shader);
    glActiveTexture (GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, this->getWallpaperTexture ());
    glUniform1i (this->g_Texture0, 0);
    // a sampler2D and a sampler3D on the same unit is an invalid draw even when the LUT is never read
    glUniform1i (this->g_Texture1, 1);

    const bool lutEnabled = this->m_lutTexture != GL_NONE && this->m_lutStrength > 0.0f;

    glUniform1i (this->u_ColorEnabled, this->m_colorEnabled);
    glUniform1i (this->u_LutEnabled, lutEnabled);
    glUniform1i (this->u_InputLinear, this->m_linearInput);
    glUniform1i (this->u_OutputPQ, this->m_outputHDR);
    glUniform4fv (this->g_Params, 1, &this->m_colorParams.x);
    glUniform1f (this->g_LutParams, this->m_lutStrength);

    if (lutEnabled) {
	glActiveTexture (GL_TEXTURE1);
	glBindTexture (GL_TEXTURE_3D, this->m_lutTexture);
	glActiveTexture (GL_TEXTURE0);
    }

    glDrawArrays (GL_TRIANGLES, 0, 6);
}

void CWallpaper::setPause (bool newState) { }

void CWallpaper::setAudioPolicy (bool muted, std::optional<int> ambientVolume) { }

void CWallpaper::setupFramebuffers (const bool depth, const TextureFormat format) {
    const uint32_t width = this->getCanvasWidth ();
    const uint32_t height = this->getCanvasHeight ();
    const uint32_t clamp = this->m_state.getClampingMode ();

    const auto sceneFBO = this->create (
	"_rt_FullFrameBuffer", format, clamp, 1.0, { width, height }, { width, height },
	this->m_cornerColor
    );

    if (depth) {
	sceneFBO->attachDepthBuffer ();
    }

    this->m_sceneFBO = sceneFBO;

    this->alias ("_rt_MipMappedFrameBuffer", "_rt_FullFrameBuffer");
}

AudioContext& CWallpaper::getAudioContext () const { return this->m_audioContext; }

const WallpaperState& CWallpaper::getState () const { return this->m_state; }

void CWallpaper::setScalingMode (WallpaperState::TextureUVsScaling mode) { this->m_state.setTextureUVsStrategy (mode); }

void CWallpaper::setZoom (float zoom) { this->m_state.setZoom (zoom); }

void CWallpaper::setOffset (float offsetX, float offsetY) { this->m_state.setOffset (offsetX, offsetY); }

void CWallpaper::setCornerColor (const glm::vec4& color) {
    this->m_cornerColor = color;

    if (this->m_sceneFBO != nullptr) {
	this->m_sceneFBO->setBorderColor (color);
    }
}

void CWallpaper::setImageAdjustments (const ImageAdjustments& adjustments) {
    // wallpaper64.exe sub_140181F30: sliders are 0-100 around 50, contrast and saturation go through a square
    // root and brightness through a square before reaching ccsimple
    const float brightness = adjustments.brightness.value_or (50.0f) / 50.0f;
    const float contrast = adjustments.contrast.value_or (50.0f) / 50.0f;
    const float saturation = adjustments.saturation.value_or (50.0f) / 50.0f;
    const float hue = adjustments.hue.value_or (50.0f) / 100.0f - 0.5f;

    this->m_colorEnabled = adjustments.colorEnabled.value_or (false)
	&& (contrast != 1.0f || brightness != 1.0f || saturation != 1.0f || hue != 0.0f);
    this->m_colorParams = {
	std::pow (brightness, 2.0f), std::pow (contrast, 0.5f), std::pow (saturation, 0.5f), hue
    };
    this->m_lutStrength = adjustments.filterStrength.value_or (100.0f) / 100.0f;
    this->m_flipHorizontal = adjustments.flipHorizontal.value_or (false);

    // only scenes and videos have these in Wallpaper Engine
    if (this->is<Wallpapers::CWeb> ()) {
	this->m_colorEnabled = false;
	this->m_flipHorizontal = false;
	this->loadLut ("");
	return;
    }

    this->loadLut (adjustments.filter.value_or (""));
}

bool CWallpaper::hasImageAdjustments () const {
    return this->m_colorEnabled || this->m_flipHorizontal
	|| (this->m_lutTexture != GL_NONE && this->m_lutStrength > 0.0f);
}

GLuint CWallpaper::renderAdjustedFramebuffer () {
    // a linear (HDR video) framebuffer needs encoding to sRGB even without adjustments
    if (!this->hasImageAdjustments () && !this->m_linearInput) {
	return this->getWallpaperFramebuffer ();
    }

    const auto width = static_cast<uint32_t> (this->getCanvasWidth ());
    const auto height = static_cast<uint32_t> (this->getCanvasHeight ());

    if (this->m_adjustedFBO == nullptr || this->m_adjustedFBO->getRealWidth () != width
	|| this->m_adjustedFBO->getRealHeight () != height) {
	this->m_adjustedFBO = std::make_unique<CFBO> (
	    "_rt_ImageAdjustments", TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0f, width, height, width, height
	);
    }

    // the output quad puts v=0 at the top, the framebuffer keeps row 0 at the bottom
    const float ustart = this->m_flipHorizontal ? 1.0f : 0.0f;
    const float uend = 1.0f - ustart;
    const GLfloat texCoords[] = { ustart, 1.0f, uend, 1.0f, ustart, 0.0f, ustart, 0.0f, uend, 1.0f, uend, 0.0f };

    // render() uploads its own UVs again next frame
    this->m_uploadedUstart = std::numeric_limits<float>::quiet_NaN ();

    const GLuint previousDestination = this->m_destFramebuffer;
    const bool outputHDR = this->m_outputHDR;
    this->m_destFramebuffer = this->m_adjustedFBO->getFramebuffer ();
    // screenshots are always SDR
    this->m_outputHDR = false;

    glViewport (0, 0, static_cast<GLsizei> (width), static_cast<GLsizei> (height));
    glBindFramebuffer (GL_FRAMEBUFFER, this->m_destFramebuffer);
    glBindVertexArray (this->m_vaoBuffer);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_texCoordBuffer);
    glBufferSubData (GL_ARRAY_BUFFER, 0, sizeof (texCoords), texCoords);
    this->drawOutputQuad ();

    this->m_destFramebuffer = previousDestination;
    this->m_outputHDR = outputHDR;

    return this->m_adjustedFBO->getFramebuffer ();
}

void CWallpaper::loadLut (const std::string& name) {
    if (name == this->m_lutName) {
	return;
    }

    this->m_lutName = name;

    if (this->m_lutTexture != GL_NONE) {
	glDeleteTextures (1, &this->m_lutTexture);
	this->m_lutTexture = GL_NONE;
    }

    if (name.empty ()) {
	return;
    }

    try {
	const auto stream = this->getAssetLocator ().texture ("lut/" + name);
	const auto texture = WallpaperEngine::Data::Parsers::TextureParser::parse (BinaryReader (stream));
	const auto& mipmap = texture->images.at (0).at (0);

	if (!(texture->flags & TextureFlags_Volume)) {
	    sLog.error ("Image filter ", name, " is not a volume texture");
	    return;
	}

	stbi_uc* decoded = nullptr;
	const void* pixels = mipmap->uncompressedData.get ();
	const size_t expected = static_cast<size_t> (mipmap->width) * mipmap->height * mipmap->depth * 4;

	// the z slices are stacked vertically in the image, the order glTexImage3D wants
	if (texture->freeImageFormat != FIF_UNKNOWN) {
	    int width, height, channels;

	    decoded = stbi_load_from_memory (
		reinterpret_cast<const stbi_uc*> (mipmap->uncompressedData.get ()), mipmap->uncompressedSize, &width,
		&height, &channels, 4
	    );

	    if (decoded == nullptr || static_cast<size_t> (width) * height * 4 != expected) {
		stbi_image_free (decoded);
		sLog.error ("Image filter ", name, " has unexpected image data");
		return;
	    }

	    pixels = decoded;
	} else if (texture->format != TextureFormat_ARGB8888 || static_cast<size_t> (mipmap->uncompressedSize) < expected) {
	    sLog.error ("Image filter ", name, " has an unsupported format");
	    return;
	}

	glGenTextures (1, &this->m_lutTexture);
	glBindTexture (GL_TEXTURE_3D, this->m_lutTexture);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
	glTexImage3D (
	    GL_TEXTURE_3D, 0, GL_RGBA8, mipmap->width, mipmap->height, mipmap->depth, 0, GL_RGBA, GL_UNSIGNED_BYTE,
	    pixels
	);
	glBindTexture (GL_TEXTURE_3D, 0);

	stbi_image_free (decoded);
    } catch (const std::exception& e) {
	sLog.error ("Cannot load image filter ", name, ": ", e.what ());
    }
}

std::shared_ptr<const CFBO> CWallpaper::findFBO (const std::string& name) const {
    const auto fbo = this->find (name);

    if (fbo == nullptr) {
	sLog.exception ("Cannot find FBO ", name);
    }

    return fbo;
}

std::shared_ptr<const CFBO> CWallpaper::getFBO () const { return this->m_sceneFBO; }

std::unique_ptr<CWallpaper> CWallpaper::fromWallpaper (
    const Wallpaper& wallpaper, RenderContext& context, AudioContext& audioContext,
    const std::filesystem::path& resolvedBackgroundPath, const WallpaperState::TextureUVsScaling& scalingMode,
    const uint32_t& clampMode, const glm::ivec2& maxRenderSize
) {
    if (wallpaper.is<Scene> ()) {
	return std::make_unique<WallpaperEngine::Render::Wallpapers::CScene> (
	    wallpaper, context, audioContext, scalingMode, clampMode
	);
    }

    if (wallpaper.is<Video> ()) {
	return std::make_unique<WallpaperEngine::Render::Wallpapers::CVideo> (
	    wallpaper, context, audioContext, scalingMode, clampMode
	);
    }

    // SOG depth wallpapers draw the splat data natively, the CEF viewer renders badly (see CSplat)
    if (wallpaper.is<Web> () && WallpaperEngine::Render::Wallpapers::CSplat::supports (wallpaper.project)) {
	try {
	    return std::make_unique<WallpaperEngine::Render::Wallpapers::CSplat> (
		wallpaper, context, audioContext, scalingMode, clampMode
	    );
	} catch (const std::exception& e) {
	    sLog.error ("Native splat renderer failed, falling back to the web page: ", e.what ());
	}
    }

    if (wallpaper.is<Web> ()) {
	return std::make_unique<WallpaperEngine::Render::Wallpapers::CWeb> (
	    wallpaper, context, audioContext, resolvedBackgroundPath, scalingMode, clampMode, maxRenderSize
	);
    }

    sLog.exception ("Unsupported wallpaper type");
}
