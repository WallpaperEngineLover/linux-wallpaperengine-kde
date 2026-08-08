#include <vector>

#include "CWallpaper.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include "WallpaperEngine/Render/Wallpapers/CVideo.h"
#include "WallpaperEngine/Render/Wallpapers/CWeb.h"

#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"

using namespace WallpaperEngine::Render;

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

    sourcePointer = "#version 330\n"
		    "precision highp float;\n"
		    "uniform sampler2D g_Texture0;\n"
		    "in vec2 v_TexCoord;\n"
		    "out vec4 out_FragColor;\n"
		    "void main () {\n"
		    "out_FragColor = texture (g_Texture0, v_TexCoord);\n"
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
    this->a_Position = glGetAttribLocation (this->m_shader, "a_Position");
    this->a_TexCoord = glGetAttribLocation (this->m_shader, "a_TexCoord");
}

void CWallpaper::setDestinationFramebuffer (GLuint framebuffer) { this->m_destFramebuffer = framebuffer; }

void CWallpaper::setSpanInfo (const SpanInfo& spanInfo) { this->m_spanInfo = spanInfo; }

const CWallpaper::SpanInfo* CWallpaper::getSpanInfo () const {
    return this->m_spanInfo.has_value () ? &this->m_spanInfo.value () : nullptr;
}

void CWallpaper::updateUVs (const glm::ivec4& viewport, const bool vflip) {
    if (this->m_state.hasChanged (viewport, vflip, this->getWidth (), this->getHeight ())) {
	this->m_state.updateState (viewport, vflip, this->getWidth (), this->getHeight ());
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
	const float relLeft = (static_cast<float> (globalPosition.x) - spanX) / spanW;
	const float relRight = (static_cast<float> (globalPosition.x + logicalSize.x) - spanX) / spanW;
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
	vstart = uvs.vstart;
	vend = uvs.vend;
    }

    const GLfloat texCoords[] = {
	ustart, vstart, uend, vstart, ustart, vend, ustart, vend, uend, vstart, uend, vend,
    };

    glViewport (viewport.x, viewport.y, viewport.z, viewport.w);

    glBindFramebuffer (GL_FRAMEBUFFER, this->m_destFramebuffer);

    glBindVertexArray (this->m_vaoBuffer);

    glDisable (GL_BLEND);
    glDisable (GL_DEPTH_TEST);
    glDisable (GL_CULL_FACE);
    glUseProgram (this->m_shader);
    glActiveTexture (GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, this->getWallpaperTexture ());
    glUniform1i (this->g_Texture0, 0);

    if (ustart != this->m_uploadedUstart || uend != this->m_uploadedUend || vstart != this->m_uploadedVstart
	|| vend != this->m_uploadedVend) {
	glBindBuffer (GL_ARRAY_BUFFER, this->m_texCoordBuffer);
	glBufferSubData (GL_ARRAY_BUFFER, 0, sizeof (texCoords), texCoords);
	this->m_uploadedUstart = ustart;
	this->m_uploadedUend = uend;
	this->m_uploadedVstart = vstart;
	this->m_uploadedVend = vend;
    }

    glDrawArrays (GL_TRIANGLES, 0, 6);

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

void CWallpaper::setPause (bool newState) { }

void CWallpaper::setAudioPolicy (bool muted, std::optional<int> ambientVolume) { }

void CWallpaper::setupFramebuffers () {
    const uint32_t width = this->getWidth ();
    const uint32_t height = this->getHeight ();
    const uint32_t clamp = this->m_state.getClampingMode ();

    this->m_sceneFBO = this->create (
	"_rt_FullFrameBuffer", TextureFormat_ARGB8888, clamp, 1.0, { width, height }, { width, height },
	this->m_cornerColor
    );

    this->alias ("_rt_MipMappedFrameBuffer", "_rt_FullFrameBuffer");
}

AudioContext& CWallpaper::getAudioContext () const { return this->m_audioContext; }

const WallpaperState& CWallpaper::getState () const { return this->m_state; }

void CWallpaper::setScalingMode (WallpaperState::TextureUVsScaling mode) { this->m_state.setTextureUVsStrategy (mode); }

void CWallpaper::setZoom (float zoom) { this->m_state.setZoom (zoom); }

void CWallpaper::setCornerColor (const glm::vec4& color) {
    this->m_cornerColor = color;

    if (this->m_sceneFBO != nullptr) {
	this->m_sceneFBO->setBorderColor (color);
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

    if (wallpaper.is<Web> ()) {
	return std::make_unique<WallpaperEngine::Render::Wallpapers::CWeb> (
	    wallpaper, context, audioContext, resolvedBackgroundPath, scalingMode, clampMode, maxRenderSize
	);
    }

    sLog.exception ("Unsupported wallpaper type");
}
