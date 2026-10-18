#include <iostream>

#include <GL/glew.h>

#include "CWallpaper.h"
#include "ImageDecoder.h"
#include "RenderContext.h"
#include "SplitLayout.h"

#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/VideoPlayback/MPV/GLPlayer.h"

#include <chrono>
#include <cstdlib>
#include <unordered_map>

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

void RenderContext::render (Drivers::Output::OutputViewport* viewport) {
    if (frameStatsEnabled ()) {
	this->renderWithStats (viewport);
	return;
    }

    viewport->makeCurrent ();
    this->m_textureCache->repaintConditionalTextures ();

#if !NDEBUG
    const std::string str = "Rendering to output " + viewport->name;

    glPushDebugGroup (GL_DEBUG_SOURCE_APPLICATION, 0, -1, str.c_str ());
#endif /* DEBUG */

    this->drawWallpapers (*viewport);

#if !NDEBUG
    glPopDebugGroup ();
#endif /* DEBUG */

    viewport->swapOutput ();
}

void RenderContext::renderWithStats (Drivers::Output::OutputViewport* viewport) {
    auto& stats = s_frameStats[viewport->name];
    const auto frameStart = Clock::now ();

    if (stats.lastFrame != Clock::time_point {}) {
	stats.maxInterval = std::max (
	    stats.maxInterval, std::chrono::duration<double, std::milli> (frameStart - stats.lastFrame).count ()
	);
    }

    stats.lastFrame = frameStart;

    viewport->makeCurrent ();
    this->m_textureCache->repaintConditionalTextures ();
    glFinish ();
    VideoPlayback::MPV::GLPlayer::s_statsMillis = 0.0;

    const auto sceneStart = Clock::now ();

    this->drawWallpapers (*viewport);

    glFinish ();

    const double video = VideoPlayback::MPV::GLPlayer::s_statsMillis;
    const double scene = millisSince (sceneStart) - video;
    const auto swapStart = Clock::now ();

    viewport->swapOutput ();

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

    sLog.out (
	"FRAME-STATS ", viewport->name, ": ", n * 1000.0 / window, " fps, worst gap ", stats.maxInterval,
	"ms | scene avg ", stats.scene / n, " max ", stats.sceneMax, " | video avg ", stats.video / n, " max ",
	stats.videoMax, " | swap avg ", stats.swap / n, " max ", stats.swapMax, " (ms)"
    );

    stats = FrameStats { .lastFrame = stats.lastFrame };
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

    // bars around centered slots and failed slots
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
    const std::string& display, std::shared_ptr<CWallpaper> wallpaper, const bool holdPrevious
) {
    if (holdPrevious) {
	this->holdFrame (display);
    } else {
	this->releaseHeldFrame (display);
    }

    wallpaper->setDestinationFramebuffer (this->m_app.getDestinationFramebuffer ());
    this->m_wallpapers.insert_or_assign (display, wallpaper);
    this->pruneTextures ();
}

void RenderContext::holdFrame (const std::string& display) {
    const auto current = this->m_wallpapers.find (display);
    const auto target = this->findDrawTarget (display);

    if (current == this->m_wallpapers.end () || !target.has_value ()) {
	this->releaseHeldFrame (display);
	return;
    }

    // keep the older frame when switching again before content arrived
    if (this->m_heldFrames.contains (display) && !current->second->hasContent ()) {
	return;
    }

    this->releaseHeldFrame (display);

    const glm::ivec4 rect = target->rect;

    if (rect.z <= 0 || rect.w <= 0) {
	return;
    }

    target->viewport->makeCurrent ();

    HeldFrame held { .viewport = rect };

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

void RenderContext::drawHeldFrame (const std::string& display, const glm::ivec4& rect, const CWallpaper& wallpaper) {
    auto held = this->m_heldFrames.find (display);

    if (held == this->m_heldFrames.end ()) {
	return;
    }

    if (wallpaper.hasContent ()) {
	this->releaseHeldFrame (display);
	return;
    }

    // the old frame is stretched over the new slot rect, via a copy since multisampled outputs only take 1:1 blits
    if (held->second.viewport.z != rect.z || held->second.viewport.w != rect.w) {
	HeldFrame resized { .viewport = rect };

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

    glBindFramebuffer (GL_READ_FRAMEBUFFER, held->second.framebuffer);
    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, this->m_app.getDestinationFramebuffer ());
    glBlitFramebuffer (
	0, 0, rect.z, rect.w, rect.x, rect.y, rect.x + rect.z, rect.y + rect.w, GL_COLOR_BUFFER_BIT, GL_NEAREST
    );
    glBindFramebuffer (GL_FRAMEBUFFER, this->m_app.getDestinationFramebuffer ());
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

} // namespace WallpaperEngine::Render
