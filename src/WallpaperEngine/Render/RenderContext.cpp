#include <iostream>

#include <GL/glew.h>

#include "CWallpaper.h"
#include "RenderContext.h"

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

// LWE_FRAME_STATS=1 logs per-output frame timings every 2 seconds. It adds a glFinish after each step so
// GPU work is attributed to the step that queued it, which costs some throughput while enabled
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
    m_textureCache (std::make_unique<TextureCache> (*this)) { }

void RenderContext::render (Drivers::Output::OutputViewport* viewport) {
    if (frameStatsEnabled ()) {
	this->renderWithStats (viewport);
	return;
    }

    viewport->makeCurrent ();

#if !NDEBUG
    const std::string str = "Rendering to output " + viewport->name;

    glPushDebugGroup (GL_DEBUG_SOURCE_APPLICATION, 0, -1, str.c_str ());
#endif /* DEBUG */

    if (const auto ref = this->m_wallpapers.find (viewport->name); ref != this->m_wallpapers.end ()) {
	ref->second->setOutputHDR (viewport->isHDR ());
	ref->second->render (
	    viewport->viewport, this->getOutput ().renderVFlip (), viewport->globalPosition, viewport->logicalSize
	);
    }

#if !NDEBUG
    glPopDebugGroup ();
#endif /* DEBUG */

    viewport->swapOutput ();
}

void RenderContext::renderWithStats (Drivers::Output::OutputViewport* viewport) {
    auto& stats = s_frameStats[viewport->name];
    const auto frameStart = Clock::now ();

    if (stats.lastFrame != Clock::time_point {}) {
	stats.maxInterval = std::max (stats.maxInterval, std::chrono::duration<double, std::milli> (frameStart - stats.lastFrame).count ());
    }

    stats.lastFrame = frameStart;

    viewport->makeCurrent ();
    glFinish ();
    VideoPlayback::MPV::GLPlayer::s_statsMillis = 0.0;

    const auto sceneStart = Clock::now ();

    if (const auto ref = this->m_wallpapers.find (viewport->name); ref != this->m_wallpapers.end ()) {
	ref->second->setOutputHDR (viewport->isHDR ());
	ref->second->render (
	    viewport->viewport, this->getOutput ().renderVFlip (), viewport->globalPosition, viewport->logicalSize
	);
    }

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

void RenderContext::setWallpaper (const std::string& display, std::shared_ptr<CWallpaper> wallpaper) {
    wallpaper->setDestinationFramebuffer (this->m_app.getDestinationFramebuffer ());
    this->m_wallpapers.insert_or_assign (display, wallpaper);
    this->pruneTextures ();
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

std::shared_ptr<const TextureProvider> RenderContext::resolveTexture (
    const std::string& name, const Data::Model::Project& project
) const {
    return this->m_textureCache->resolve (name, project);
}

const std::map<std::string, std::shared_ptr<CWallpaper>>& RenderContext::getWallpapers () const {
    return this->m_wallpapers;
}

Media::MediaSource& RenderContext::getMediaSource () const { return this->m_mediaSource; }

} // namespace WallpaperEngine::Render
