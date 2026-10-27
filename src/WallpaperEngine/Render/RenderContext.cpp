#include <iostream>

#include <GL/glew.h>

#include "CWallpaper.h"
#include "ImageDecoder.h"
#include "RenderContext.h"
#include "SplitLayout.h"
#include "Transition.h"

#include "WallpaperEngine/Debugging/Profiling.h"

#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/VideoPlayback/MPV/GLPlayer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <unordered_map>
#include <vector>

extern float g_RealTime;

namespace {
using Clock = std::chrono::steady_clock;

double millisSince (const Clock::time_point start) {
    return std::chrono::duration<double, std::milli> (Clock::now () - start).count ();
}

// LWE_FRAME_STATS=1 logs per-output frame timings every 2 s, with a glFinish per step (costs throughput)
struct FrameStats {
    Clock::time_point windowStart = Clock::now ();
    Clock::time_point lastFrame {};
    int frames = 0;
    double maxInterval = 0.0;
    std::vector<float> intervals;
    double scene = 0.0, sceneMax = 0.0;
    double video = 0.0, videoMax = 0.0;
    double swap = 0.0, swapMax = 0.0;
};

bool frameStatsEnabled () {
    static const bool enabled = std::getenv ("LWE_FRAME_STATS") != nullptr;
    return enabled;
}

std::unordered_map<std::string, FrameStats> s_frameStats;
}

namespace WallpaperEngine::Render {
RenderContext::RenderContext (
    Drivers::VideoDriver& driver, WallpaperApplication& app, Media::MediaSource& mediaSource
) :
    m_driver (driver), m_app (app), m_mediaSource (mediaSource),
    m_textureCache (std::make_unique<TextureCache> (*this)) {
    GLint maxTextureSize = 0;
    glGetIntegerv (GL_MAX_TEXTURE_SIZE, &maxTextureSize);
    setMaxImageSide (maxTextureSize);
}

namespace {
    // the test rigs wait for it: the same line on every desktop, unlike the driver specific ones
    void noteFirstFrame (const std::string& output) {
	static bool noted = false;

	if (!noted) {
	    noted = true;
	    sLog.out ("First frame drawn on ", output);
	}
    }
}

void RenderContext::render (Drivers::Output::OutputViewport* viewport) {
    LWE_ZONE_TEXT ("render output", viewport->name);

    if (frameStatsEnabled ()) {
	this->renderWithStats (viewport);
	return;
    }

    viewport->makeCurrent ();
    this->beginProfiledFrame ();
    this->m_textureCache->repaintConditionalTextures ();

#if !NDEBUG
    const std::string str = "Rendering to output " + viewport->name;

    glPushDebugGroup (GL_DEBUG_SOURCE_APPLICATION, 0, -1, str.c_str ());
#endif

    {
	LWE_GPU_ZONE ("draw wallpapers");
	this->drawWallpapers (*viewport);
    }

    noteFirstFrame (viewport->name);

#if !NDEBUG
    glPopDebugGroup ();
#endif

    viewport->swapOutput ();
    LWE_GPU_COLLECT ();
    LWE_FRAME_MARK ();
}

void RenderContext::beginProfiledFrame () {
#ifdef LWE_TRACY
    static bool gpuContextCreated = false;

    if (!gpuContextCreated) {
	TracyGpuContext;
	gpuContextCreated = true;
    }
#endif
}

void RenderContext::renderWithStats (Drivers::Output::OutputViewport* viewport) {
    auto& stats = s_frameStats[viewport->name];
    const auto frameStart = Clock::now ();

    if (stats.lastFrame != Clock::time_point {}) {
	const double interval = std::chrono::duration<double, std::milli> (frameStart - stats.lastFrame).count ();

	stats.maxInterval = std::max (stats.maxInterval, interval);
	stats.intervals.push_back (static_cast<float> (interval));
    }

    stats.lastFrame = frameStart;

    viewport->makeCurrent ();
    this->beginProfiledFrame ();
    this->m_textureCache->repaintConditionalTextures ();
    glFinish ();
    VideoPlayback::MPV::GLPlayer::s_statsMillis = 0.0;

    const auto sceneStart = Clock::now ();

    {
	LWE_GPU_ZONE ("draw wallpapers");
	this->drawWallpapers (*viewport);
    }

    noteFirstFrame (viewport->name);

    glFinish ();

    const double video = VideoPlayback::MPV::GLPlayer::s_statsMillis;
    const double scene = millisSince (sceneStart) - video;
    const auto swapStart = Clock::now ();

    viewport->swapOutput ();
    LWE_GPU_COLLECT ();
    LWE_FRAME_MARK ();

    const double swap = millisSince (swapStart);

    stats.frames++;
    stats.scene += scene;
    stats.sceneMax = std::max (stats.sceneMax, scene);
    stats.video += video;
    stats.videoMax = std::max (stats.videoMax, video);
    stats.swap += swap;
    stats.swapMax = std::max (stats.swapMax, swap);

    const double window = millisSince (stats.windowStart);

    if (window < 2000.0) {
	return;
    }

    const double n = stats.frames;
    double jitter = 0.0, median = 0.0, p99 = 0.0;

    if (stats.intervals.size () > 1) {
	auto& gaps = stats.intervals;
	const double mean = std::accumulate (gaps.begin (), gaps.end (), 0.0) / gaps.size ();

	for (const float gap : gaps) {
	    jitter += (gap - mean) * (gap - mean);
	}

	jitter = std::sqrt (jitter / gaps.size ());

	const auto medianAt = gaps.begin () + gaps.size () / 2;
	std::nth_element (gaps.begin (), medianAt, gaps.end ());
	median = *medianAt;

	const auto p99At = gaps.begin () + (gaps.size () - 1) * 99 / 100;
	std::nth_element (gaps.begin (), p99At, gaps.end ());
	p99 = *p99At;
    }

    sLog.out (
	"FRAME-STATS ", viewport->name, ": ", n * 1000.0 / window, " fps, gap median ", median, " p99 ", p99, " worst ",
	stats.maxInterval, " jitter ", jitter, "ms | scene avg ", stats.scene / n, " max ", stats.sceneMax,
	" | video avg ", stats.video / n, " max ", stats.videoMax, " | swap avg ", stats.swap / n, " max ",
	stats.swapMax, " (ms)"
    );

    stats.intervals.clear ();
    stats = FrameStats { .lastFrame = stats.lastFrame, .intervals = std::move (stats.intervals) };
}

void RenderContext::drawWallpapers (Drivers::Output::OutputViewport& viewport) {
    if (const auto* split = this->m_app.getContext ().findSplitScreen (viewport.name); split != nullptr) {
	this->drawSplitScreen (viewport, *split);
	return;
    }

    const auto ref = this->m_wallpapers.find (viewport.name);

    if (ref == this->m_wallpapers.end ()) {
	return;
    }

    ref->second->setOutputHDR (viewport.isHDR (), viewport.getHDRLuminance ());
    viewport.setWantsPointerButtons (ref->second->wantsPointerButtons ());
    ref->second->render (
	viewport.viewport, this->getOutput ().renderVFlip (), viewport.globalPosition, viewport.logicalSize
    );
    this->drawHeldFrame (viewport.name, viewport.viewport, *ref->second);
}

void RenderContext::drawSplitScreen (
    Drivers::Output::OutputViewport& viewport, const Application::ApplicationContext::SplitScreen& split
) {
    const glm::ivec4 output = viewport.viewport;
    const bool vflip = this->getOutput ().renderVFlip ();
    std::vector<CWallpaper*> wallpapers;
    std::vector<float> aspects;

    for (const auto& slot : split.slots) {
	const auto it = this->m_wallpapers.find (slot);
	auto* wallpaper = it != this->m_wallpapers.end () ? it->second.get () : nullptr;

	wallpapers.push_back (wallpaper);
	aspects.push_back (wallpaper != nullptr ? wallpaper->getAspectRatio () : 0.0f);
    }

    const auto rects = splitSlotRects ({ output.z, output.w }, aspects, split.layout, vflip);

    glBindFramebuffer (GL_FRAMEBUFFER, this->m_app.getDestinationFramebuffer ());
    glEnable (GL_SCISSOR_TEST);
    glScissor (output.x, output.y, output.z, output.w);
    glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
    glClear (GL_COLOR_BUFFER_BIT);
    glDisable (GL_SCISSOR_TEST);

    bool wantsPointerButtons = false;

    for (std::size_t i = 0; i < split.slots.size (); i++) {
	const glm::ivec4 rect { output.x + rects[i].x, output.y + rects[i].y, rects[i].z, rects[i].w };

	this->m_slotRects.insert_or_assign (split.slots[i], rect);

	if (wallpapers[i] == nullptr || rect.z <= 0 || rect.w <= 0) {
	    continue;
	}

	wallpapers[i]->setOutputHDR (viewport.isHDR (), viewport.getHDRLuminance ());
	wallpapers[i]->setPointerRegion (rect);
	wantsPointerButtons = wantsPointerButtons || wallpapers[i]->wantsPointerButtons ();
	wallpapers[i]->render (rect, vflip, viewport.globalPosition, viewport.logicalSize);
	this->drawHeldFrame (split.slots[i], rect, *wallpapers[i]);
    }

    viewport.setWantsPointerButtons (wantsPointerButtons);
}

std::optional<RenderContext::DrawTarget> RenderContext::findDrawTarget (const std::string& display) const {
    const auto& viewports = this->getOutput ().getViewports ();

    if (const auto* split = this->m_app.getContext ().findSplitScreen (display); split != nullptr) {
	const auto viewport = viewports.find (split->screen);
	const auto rect = this->m_slotRects.find (display);

	if (viewport == viewports.end () || rect == this->m_slotRects.end ()) {
	    return std::nullopt;
	}

	return DrawTarget { .viewport = viewport->second, .rect = rect->second };
    }

    const auto viewport = viewports.find (display);

    if (viewport == viewports.end ()) {
	return std::nullopt;
    }

    return DrawTarget { .viewport = viewport->second, .rect = viewport->second->viewport };
}

void RenderContext::setWallpaper (
    const std::string& display, std::shared_ptr<CWallpaper> wallpaper, const bool holdPrevious, const bool animate
) {
    if (holdPrevious) {
	this->holdFrame (display, animate);
    } else {
	this->releaseHeldFrame (display);
    }

    wallpaper->setDestinationFramebuffer (this->m_app.getDestinationFramebuffer ());
    this->m_wallpapers.insert_or_assign (display, wallpaper);
    this->pruneTextures ();
}

void RenderContext::holdFrame (const std::string& display, const bool animate) {
    const auto current = this->m_wallpapers.find (display);
    const auto target = this->findDrawTarget (display);

    if (current == this->m_wallpapers.end () || !target.has_value ()) {
	this->releaseHeldFrame (display);
	return;
    }

    // keep the older frame when switching again before content arrived
    if (const auto older = this->m_heldFrames.find (display);
	older != this->m_heldFrames.end () && !current->second->hasContent ()) {
	older->second.animate = animate;
	return;
    }

    this->releaseHeldFrame (display);

    const glm::ivec4 rect = target->rect;

    if (rect.z <= 0 || rect.w <= 0) {
	return;
    }

    target->viewport->makeCurrent ();

    HeldFrame held { .viewport = rect, .animate = animate };

    if (!allocateHeldFrame (held)) {
	sLog.error ("Cannot keep the previous wallpaper on ", display, " while the new one loads");
	glBindFramebuffer (GL_FRAMEBUFFER, this->m_app.getDestinationFramebuffer ());
	return;
    }

    glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
    glClear (GL_COLOR_BUFFER_BIT);

    current->second->setDestinationFramebuffer (held.framebuffer);
    current->second->present (
	{ 0, 0, rect.z, rect.w }, target->viewport->globalPosition, target->viewport->logicalSize
    );
    current->second->setDestinationFramebuffer (this->m_app.getDestinationFramebuffer ());
    glBindFramebuffer (GL_FRAMEBUFFER, this->m_app.getDestinationFramebuffer ());

    this->m_heldFrames.insert_or_assign (display, held);
}

void RenderContext::releaseHeldFrame (const std::string& display) {
    const auto held = this->m_heldFrames.find (display);

    if (held == this->m_heldFrames.end ()) {
	return;
    }

    glDeleteFramebuffers (1, &held->second.framebuffer);
    glDeleteTextures (1, &held->second.texture);
    this->m_heldFrames.erase (held);
}

namespace {
    void dumpTransitionFrame (GLuint framebuffer, const glm::ivec4& rect, float progress);
    void dumpTransitionReferences (GLuint destination, GLuint held, const glm::ivec4& rect);
}

void RenderContext::drawHeldFrame (const std::string& display, const glm::ivec4& rect, const CWallpaper& wallpaper) {
    auto held = this->m_heldFrames.find (display);

    if (held == this->m_heldFrames.end ()) {
	return;
    }

    float progress = -1.0f;

    if (wallpaper.hasContent ()) {
	const auto& render = this->m_app.getContext ().settings.render;
	const auto kind = parseTransition (render.transition).value_or (TransitionKind::None);

	if (!held->second.animate || kind == TransitionKind::None || !this->ensureTransitionProgram ()) {
	    this->releaseHeldFrame (display);
	    return;
	}

	if (held->second.transitionStart < 0.0) {
	    held->second.transitionStart = g_RealTime;
	}

	const double seconds = std::max (render.transitionMs, TRANSITION_MIN_MS) / 1000.0;
	const double t = (g_RealTime - held->second.transitionStart) / seconds;

	if (t >= 1.0) {
	    this->releaseHeldFrame (display);
	    return;
	}

	progress = static_cast<float> (std::max (t, 0.0));
    }

    // the old frame is stretched over the new slot rect, via a copy since multisampled outputs only take 1:1 blits
    if (held->second.viewport.z != rect.z || held->second.viewport.w != rect.w) {
	HeldFrame resized { .viewport = rect,
			    .animate = held->second.animate,
			    .transitionStart = held->second.transitionStart };

	if (rect.z <= 0 || rect.w <= 0 || !allocateHeldFrame (resized)) {
	    this->releaseHeldFrame (display);
	    glBindFramebuffer (GL_FRAMEBUFFER, this->m_app.getDestinationFramebuffer ());
	    return;
	}

	glBindFramebuffer (GL_READ_FRAMEBUFFER, held->second.framebuffer);
	glBindFramebuffer (GL_DRAW_FRAMEBUFFER, resized.framebuffer);
	glBlitFramebuffer (
	    0, 0, held->second.viewport.z, held->second.viewport.w, 0, 0, rect.z, rect.w, GL_COLOR_BUFFER_BIT, GL_LINEAR
	);
	this->releaseHeldFrame (display);
	held = this->m_heldFrames.insert_or_assign (display, resized).first;
    }

    if (progress >= 0.0f) {
	dumpTransitionReferences (this->m_app.getDestinationFramebuffer (), held->second.framebuffer, rect);
	this->drawTransition (held->second, rect, progress);
	dumpTransitionFrame (this->m_app.getDestinationFramebuffer (), rect, progress);
	return;
    }

    glBindFramebuffer (GL_READ_FRAMEBUFFER, held->second.framebuffer);
    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, this->m_app.getDestinationFramebuffer ());
    glBlitFramebuffer (
	0, 0, rect.z, rect.w, rect.x, rect.y, rect.x + rect.z, rect.y + rect.w, GL_COLOR_BUFFER_BIT, GL_NEAREST
    );
    glBindFramebuffer (GL_FRAMEBUFFER, this->m_app.getDestinationFramebuffer ());
}

namespace {
    const char* TRANSITION_VERTEX_SHADER = R"(#version 330
void main () {
    vec2 corner = vec2 (float (gl_VertexID & 1), float ((gl_VertexID >> 1) & 1));
    gl_Position = vec4 (corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

    // mode: 0 fade, 1 slide left, 2 slide right, 3 wipe left, 4 wipe right, 5 zoom
    const char* TRANSITION_FRAGMENT_SHADER = R"(#version 330
uniform sampler2D u_Old;
uniform vec4 u_Rect;
uniform float u_Progress;
uniform int u_Mode;
out vec4 outColor;

void main () {
    vec2 uv = (gl_FragCoord.xy - u_Rect.xy) / u_Rect.zw;
    float p = u_Progress * u_Progress * (3.0 - 2.0 * u_Progress);
    float alpha = 1.0;

    if (u_Mode == 0) {
	alpha = 1.0 - p;
    } else if (u_Mode == 1) {
	uv.x += p;
	alpha = uv.x > 1.0 ? 0.0 : 1.0;
    } else if (u_Mode == 2) {
	uv.x -= p;
	alpha = uv.x < 0.0 ? 0.0 : 1.0;
    } else if (u_Mode == 3 || u_Mode == 4) {
	float edge = p * 1.06 - 0.03;
	float x = u_Mode == 3 ? 1.0 - uv.x : uv.x;
	alpha = smoothstep (edge - 0.03, edge + 0.03, x);
    } else {
	uv = (uv - 0.5) / (1.0 + 0.5 * p) + 0.5;
	alpha = 1.0 - p;
    }

    if (alpha <= 0.0) {
	discard;
    }

    outColor = vec4 (texture (u_Old, clamp (uv, 0.0, 1.0)).rgb, alpha);
}
)";

    void writePpm (const std::string& path, const GLuint framebuffer, const glm::ivec4& rect) {
	std::vector<unsigned char> pixels (static_cast<std::size_t> (rect.z) * rect.w * 3);

	glBindFramebuffer (GL_READ_FRAMEBUFFER, framebuffer);
	glPixelStorei (GL_PACK_ALIGNMENT, 1);
	glReadPixels (rect.x, rect.y, rect.z, rect.w, GL_RGB, GL_UNSIGNED_BYTE, pixels.data ());

	if (std::FILE* file = std::fopen (path.c_str (), "wb"); file != nullptr) {
	    std::fprintf (file, "P6\n%d %d\n255\n", rect.z, rect.w);
	    std::fwrite (pixels.data (), 1, pixels.size (), file);
	    std::fclose (file);
	}
    }

    // LWE_TRANSITION_DUMP=<name>.ppm (tools/check_transition.sh, the VM matrix): screenshots never show a transition,
    // they read the wallpaper's own buffer, so the output is written out instead, rows bottom up: the old frame
    // (<name>-old.ppm) and the new wallpaper on its own (<name>-new.ppm) before the first transition frame is drawn,
    // <name>.ppm for the first frame that is at least halfway
    const char* transitionDumpPath () {
	static const char* path = std::getenv ("LWE_TRANSITION_DUMP");
	return path;
    }

    void dumpTransitionReferences (const GLuint destination, const GLuint held, const glm::ivec4& rect) {
	static bool dumped = false;
	const char* path = transitionDumpPath ();

	if (path == nullptr || dumped) {
	    return;
	}

	dumped = true;

	std::string base = path;

	if (base.ends_with (".ppm")) {
	    base.resize (base.size () - 4);
	}

	writePpm (base + "-old.ppm", held, rect);
	writePpm (base + "-new.ppm", destination, rect);
    }

    void dumpTransitionFrame (const GLuint framebuffer, const glm::ivec4& rect, const float progress) {
	static bool dumped = false;
	const char* path = transitionDumpPath ();

	if (path == nullptr || dumped || progress < 0.5f) {
	    return;
	}

	dumped = true;
	writePpm (path, framebuffer, rect);
    }

    GLuint compileTransitionShader (const GLenum type, const char* source) {
	const GLuint shader = glCreateShader (type);

	glShaderSource (shader, 1, &source, nullptr);
	glCompileShader (shader);

	GLint ok = GL_FALSE;
	glGetShaderiv (shader, GL_COMPILE_STATUS, &ok);

	if (ok == GL_TRUE) {
	    return shader;
	}

	char log[512] = {};
	glGetShaderInfoLog (shader, sizeof (log) - 1, nullptr, log);
	sLog.error ("Transition shader: ", log);
	glDeleteShader (shader);
	return GL_NONE;
    }
}

bool RenderContext::ensureTransitionProgram () {
    if (this->m_transitionProgram != GL_NONE) {
	return true;
    }

    if (this->m_transitionProgramFailed) {
	return false;
    }

    this->m_transitionProgramFailed = true;

    const GLuint vertex = compileTransitionShader (GL_VERTEX_SHADER, TRANSITION_VERTEX_SHADER);
    const GLuint fragment = compileTransitionShader (GL_FRAGMENT_SHADER, TRANSITION_FRAGMENT_SHADER);

    if (vertex == GL_NONE || fragment == GL_NONE) {
	glDeleteShader (vertex);
	glDeleteShader (fragment);
	return false;
    }

    const GLuint program = glCreateProgram ();
    glAttachShader (program, vertex);
    glAttachShader (program, fragment);
    glLinkProgram (program);
    glDeleteShader (vertex);
    glDeleteShader (fragment);

    GLint ok = GL_FALSE;
    glGetProgramiv (program, GL_LINK_STATUS, &ok);

    if (ok != GL_TRUE) {
	sLog.error ("Transitions are not available: the shader does not link");
	glDeleteProgram (program);
	return false;
    }

    this->m_transitionProgram = program;
    this->m_transitionProgramFailed = false;
    return true;
}

void RenderContext::drawTransition (const HeldFrame& held, const glm::ivec4& rect, const float progress) {
    const auto kind = parseTransition (this->m_app.getContext ().settings.render.transition);
    int mode = 0;

    switch (kind.value_or (TransitionKind::Fade)) {
	case TransitionKind::SlideLeft:
	    mode = 1;
	    break;
	case TransitionKind::SlideRight:
	    mode = 2;
	    break;
	case TransitionKind::WipeLeft:
	    mode = 3;
	    break;
	case TransitionKind::WipeRight:
	    mode = 4;
	    break;
	case TransitionKind::Zoom:
	    mode = 5;
	    break;
	default:
	    break;
    }

    GLint viewport[4], scissorBox[4], program, texture, activeTexture, blendSrcRGB, blendSrcA, blendDstRGB, blendDstA;
    GLboolean colorMask[4];
    const GLboolean blend = glIsEnabled (GL_BLEND), scissor = glIsEnabled (GL_SCISSOR_TEST),
		    depth = glIsEnabled (GL_DEPTH_TEST), cull = glIsEnabled (GL_CULL_FACE);

    glGetIntegerv (GL_VIEWPORT, viewport);
    glGetIntegerv (GL_SCISSOR_BOX, scissorBox);
    glGetIntegerv (GL_CURRENT_PROGRAM, &program);
    glGetIntegerv (GL_ACTIVE_TEXTURE, &activeTexture);
    glActiveTexture (GL_TEXTURE0);
    glGetIntegerv (GL_TEXTURE_BINDING_2D, &texture);
    glGetIntegerv (GL_BLEND_SRC_RGB, &blendSrcRGB);
    glGetIntegerv (GL_BLEND_SRC_ALPHA, &blendSrcA);
    glGetIntegerv (GL_BLEND_DST_RGB, &blendDstRGB);
    glGetIntegerv (GL_BLEND_DST_ALPHA, &blendDstA);
    glGetBooleanv (GL_COLOR_WRITEMASK, colorMask);

    GLuint vao = GL_NONE;
    glGenVertexArrays (1, &vao);
    glBindVertexArray (vao);

    glBindFramebuffer (GL_FRAMEBUFFER, this->m_app.getDestinationFramebuffer ());
    glViewport (rect.x, rect.y, rect.z, rect.w);
    glEnable (GL_SCISSOR_TEST);
    glScissor (rect.x, rect.y, rect.z, rect.w);
    glDisable (GL_DEPTH_TEST);
    glDisable (GL_CULL_FACE);
    glEnable (GL_BLEND);
    glBlendFuncSeparate (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);

    glUseProgram (this->m_transitionProgram);
    glBindTexture (GL_TEXTURE_2D, held.texture);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glUniform1i (glGetUniformLocation (this->m_transitionProgram, "u_Old"), 0);
    glUniform4f (
	glGetUniformLocation (this->m_transitionProgram, "u_Rect"), static_cast<float> (rect.x),
	static_cast<float> (rect.y), static_cast<float> (rect.z), static_cast<float> (rect.w)
    );
    glUniform1f (glGetUniformLocation (this->m_transitionProgram, "u_Progress"), progress);
    glUniform1i (glGetUniformLocation (this->m_transitionProgram, "u_Mode"), mode);
    glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);

    glBindTexture (GL_TEXTURE_2D, texture);
    glActiveTexture (activeTexture);
    glUseProgram (program);
    glBindVertexArray (GL_NONE);
    glDeleteVertexArrays (1, &vao);
    glColorMask (colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
    glBlendFuncSeparate (blendSrcRGB, blendDstRGB, blendSrcA, blendDstA);
    blend ? glEnable (GL_BLEND) : glDisable (GL_BLEND);
    scissor ? glEnable (GL_SCISSOR_TEST) : glDisable (GL_SCISSOR_TEST);
    depth ? glEnable (GL_DEPTH_TEST) : glDisable (GL_DEPTH_TEST);
    cull ? glEnable (GL_CULL_FACE) : glDisable (GL_CULL_FACE);
    glScissor (scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]);
    glViewport (viewport[0], viewport[1], viewport[2], viewport[3]);
}

bool RenderContext::allocateHeldFrame (HeldFrame& held) {
    glGenTextures (1, &held.texture);
    glBindTexture (GL_TEXTURE_2D, held.texture);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA16F, held.viewport.z, held.viewport.w, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    glBindTexture (GL_TEXTURE_2D, 0);
    glGenFramebuffers (1, &held.framebuffer);
    glBindFramebuffer (GL_FRAMEBUFFER, held.framebuffer);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, held.texture, 0);

    if (glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
	return true;
    }

    glDeleteFramebuffers (1, &held.framebuffer);
    glDeleteTextures (1, &held.texture);
    return false;
}

void RenderContext::pruneTextures () const { this->m_textureCache->prune (); }

void RenderContext::setPause (const bool newState) const {
    for (const auto& wallpaper : this->m_wallpapers | std::views::values) {
	wallpaper->setPause (newState);
    }

    VideoPlayback::MPV::GLPlayer::setGlobalPause (newState);
}

Input::InputContext& RenderContext::getInputContext () const { return this->m_driver.getInputContext (); }

const WallpaperApplication& RenderContext::getApp () const { return this->m_app; }

const Drivers::VideoDriver& RenderContext::getDriver () const { return this->m_driver; }

const Drivers::Output::Output& RenderContext::getOutput () const { return this->m_driver.getOutput (); }

std::shared_ptr<const TextureProvider>
RenderContext::resolveTexture (const std::string& name, const Data::Model::Project& project) const {
    return this->m_textureCache->resolve (name, project);
}

std::string RenderContext::iconTextureName (const std::filesystem::path& icon) const {
    return this->m_textureCache->iconTextureName (icon);
}

std::shared_ptr<const TextureProvider>
RenderContext::findLoadedTexture (const std::string& name, const Data::Model::Project& project) const {
    return this->m_textureCache->findLoaded (name, project);
}

const std::map<std::string, std::shared_ptr<CWallpaper>>& RenderContext::getWallpapers () const {
    return this->m_wallpapers;
}

Media::MediaSource& RenderContext::getMediaSource () const { return this->m_mediaSource; }

std::optional<glm::ivec4> RenderContext::getSlotRect (const std::string& slot) const {
    const auto rect = this->m_slotRects.find (slot);

    return rect != this->m_slotRects.end () ? std::optional (rect->second) : std::nullopt;
}

}
