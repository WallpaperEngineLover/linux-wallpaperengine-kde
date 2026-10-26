#include "CVideo.h"

#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/VideoPlayback/MPV/GLPlayer.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <vector>

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Render::Wallpapers;
using namespace WallpaperEngine::VideoPlayback::MPV;

CVideo::CVideo (
    const Wallpaper& wallpaper, RenderContext& context, AudioContext& audioContext,
    const WallpaperState::TextureUVsScaling& scalingMode, const uint32_t& clampMode
) : CWallpaper (wallpaper, context, audioContext, scalingMode, clampMode) {
    // with --hdr the video keeps its highlights, the output shader turns it into PQ or sRGB per monitor
    const bool hdr = this->getContext ().getDriver ().isHDRAvailable ();
    this->m_hdr = hdr;

    this->setupFramebuffers (false, hdr ? TextureFormat_RGBA16161616f : TextureFormat_ARGB8888);
    this->setLinearInput (hdr);

    this->m_path = this->getVideo ().project.assetLocator->physicalPath (this->getVideo ().filename);
    this->m_segments = normalizeSegments (this->getContext ().getApp ().getContext ().settings.render.videoSegments);

    if (const auto it = s_sources.find (this->m_path); it != s_sources.end () && it->second->m_hdr == hdr
	&& it->second->m_loop && it->second->m_segments == this->m_segments) {
	this->m_source = it->second;
	this->m_source->m_sharers.push_back (this);
	this->m_source->m_player->setRenderOnlyNew (true);
	sLog.out ("Video ", this->m_path.filename ().string (), " shares the decoder of another output");
	return;
    }

    this->createPlayer ();
    s_sources.emplace (this->m_path, this);
}

void CVideo::createPlayer () {
    // starts at a small framebuffer size; resized once mpv starts playback and reports the real resolution
    this->m_player = std::make_unique<GLPlayer> (
	this->getContext (), this->CWallpaper::getWallpaperTexture (), this->m_path, 64, 64,
	this->CWallpaper::getWallpaperFramebuffer ()
    );
    // --silent and a volume of 0 mute mpv too
    const auto& audioSettings = this->getContext ().getApp ().getContext ().settings.audio;
    this->m_player->setVolume (audioSettings.enabled ? audioSettings.volume * 100.0 / 128.0 : 0.0);
    const auto& renderSettings = this->getContext ().getApp ().getContext ().settings.render;
    this->m_player->setSpeed (renderSettings.playbackSpeed);
    this->m_player->setSegments (this->m_segments);

    if (!this->m_loop) {
	this->m_player->setLoop (false);
    }

    if (this->m_hdr) {
	this->m_player->setLinearOutput ();
    }

    this->m_muted = false;
    // needs at least one usage marked for the video to actually start playing
    this->m_player->incrementUsageCount ();
    this->applyPause ();
}

CVideo::~CVideo () {
    if (this->m_source != nullptr) {
	std::erase (this->m_source->m_sharers, this);

	if (this->m_source->m_sharers.empty ()) {
	    this->m_source->m_player->setRenderOnlyNew (false);
	}

	return;
    }

    if (const auto it = s_sources.find (this->m_path); it != s_sources.end () && it->second == this) {
	s_sources.erase (it);
    }

    if (!this->m_sharers.empty ()) {
	// the first sharer that can start a decoder takes the video over while this player still plays, so it starts at
	// the same spot; one that can't is left without video rather than letting the exception out of a destructor
	const std::vector<CVideo*> sharers = std::move (this->m_sharers);
	this->m_sharers.clear ();
	CVideo* heir = nullptr;
	std::size_t next = 0;

	for (CVideo* candidate : sharers) {
	    next++;
	    candidate->m_source = nullptr;

	    try {
		candidate->createPlayer ();
		heir = candidate;
		break;
	    } catch (const std::exception& e) {
		sLog.error (
		    "Video ", this->m_path.filename ().string (), " could not take over the decoder: ", e.what ()
		);
		candidate->m_player.reset ();
	    }
	}

	if (heir != nullptr) {
	    for (CVideo* other : sharers | std::views::drop (next)) {
		other->m_source = heir;
		heir->m_sharers.push_back (other);
	    }

	    if (!heir->m_sharers.empty ()) {
		heir->m_player->setRenderOnlyNew (true);
	    }

	    heir->applyPause ();
	    s_sources[this->m_path] = heir;
	}
    }

    if (this->m_player) {
	this->m_player->decrementUsageCount ();
    }
}

void CVideo::drivePlayer () {
    if (!this->m_player) {
	return;
    }

    this->updateMuteState ();
    // WE draws a video wallpaper as the texture of materials/background.json, shown every frame
    this->m_player->markUsed ();
    this->m_player->render ();

    // mpv reallocates the texture at the video's size later, the CFBO was made at 16x16
    if (const auto fbo = this->find ("_rt_FullFrameBuffer"); fbo != nullptr
	&& (static_cast<int> (fbo->getRealWidth ()) != this->getWidth ()
	    || static_cast<int> (fbo->getRealHeight ()) != this->getHeight ())) {
	fbo->resize (this->getWidth (), this->getHeight ());
	this->m_player->requestRedraw ();
	this->m_player->render ();
    }
}

void CVideo::copyFromSource () {
    CVideo& source = *this->m_source;

    source.drivePlayer ();

    const int width = this->getWidth ();
    const int height = this->getHeight ();

    if (const auto fbo = this->find ("_rt_FullFrameBuffer"); fbo != nullptr
	&& (static_cast<int> (fbo->getRealWidth ()) != width || static_cast<int> (fbo->getRealHeight ()) != height)) {
	fbo->resize (width, height);
    }

    GLboolean mask[4] {};
    const GLboolean scissor = glIsEnabled (GL_SCISSOR_TEST);

    glGetBooleanv (GL_COLOR_WRITEMASK, mask);
    glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable (GL_SCISSOR_TEST);
    glBindFramebuffer (GL_READ_FRAMEBUFFER, source.CWallpaper::getWallpaperFramebuffer ());
    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, this->CWallpaper::getWallpaperFramebuffer ());
    glBlitFramebuffer (0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer (GL_FRAMEBUFFER, this->CWallpaper::getWallpaperFramebuffer ());
    glColorMask (mask[0], mask[1], mask[2], mask[3]);

    if (scissor) {
	glEnable (GL_SCISSOR_TEST);
    }
}

void CVideo::renderFrame (const glm::ivec4& viewport) {
    this->m_presentBackBuffer = false;

    if (this->m_source != nullptr) {
	this->copyFromSource ();
    } else {
	this->drivePlayer ();
    }

    if (!this->m_hdr) {
	this->renderImageAdjustments ({ this->getWidth (), this->getHeight () }, GL_RGBA8);
    }
}

void CVideo::unshare () {
    CVideo& source = *this->m_source;

    this->m_source = nullptr;
    this->createPlayer ();
    std::erase (source.m_sharers, this);

    if (source.m_sharers.empty ()) {
	source.m_player->setRenderOnlyNew (false);
    }

    source.applyPause ();
}

void CVideo::applyPause () {
    if (!this->m_player) {
	return;
    }

    bool all = this->m_wantsPause;

    for (const CVideo* sharer : this->m_sharers) {
	all = all && sharer->m_wantsPause;
    }

    if (all) {
	this->m_player->setPaused ();
    } else {
	this->m_player->clearPaused ();
    }
}

void CVideo::reconcileSharers () {
    const auto sharers = this->m_sharers;

    for (CVideo* sharer : sharers) {
	if (sharer->m_loop != this->m_loop || sharer->m_segments != this->m_segments) {
	    sharer->unshare ();
	}
    }
}

void CVideo::updateMuteState () {
    if (!this->getContext ().getApp ().getContext ().settings.audio.enabled) {
	return;
    }

    bool forced = this->m_forceMuted;

    for (const CVideo* sharer : this->m_sharers) {
	forced = forced && sharer->m_forceMuted;
    }

    const bool desiredMuted = forced || this->getAudioContext ().getDriver ().getAudioDetector ().anythingPlaying ();

    if (!this->m_player || this->m_muted == desiredMuted) {
	return;
    }

    this->m_muted = desiredMuted;

    if (this->m_muted) {
	this->m_player->setMuted ();
    } else {
	this->m_player->clearMuted ();
    }
}

void CVideo::setAudioPolicy (bool muted, std::optional<int> ambientVolume) {
    this->m_forceMuted = muted;
    this->root ().updateMuteState ();
}

const Data::Model::Video& CVideo::getVideo () const { return *this->getWallpaperData ().as<Data::Model::Video> (); }

void CVideo::setVolume (double volume) {
    if (const auto& player = this->root ().m_player) {
	player->setVolume (volume);
    }
}

void CVideo::setSpeed (double speed) {
    if (const auto& player = this->root ().m_player) {
	player->setSpeed (speed);
    }
}

void CVideo::setLoop (const bool loop) {
    this->m_loop = loop;

    if (this->m_source != nullptr) {
	if (this->m_source->m_loop != loop) {
	    this->unshare ();
	}

	return;
    }

    if (this->m_player && this->m_player->isLooping () != loop) {
	this->m_player->setLoop (loop);
    }

    this->reconcileSharers ();
}

bool CVideo::hasEnded () const {
    const auto& player = this->root ().m_player;
    return player && player->hasEnded ();
}

void CVideo::setSegments (const VideoPlayback::VideoSegments& segments) {
    this->m_segments = normalizeSegments (segments);

    if (this->m_source != nullptr) {
	if (this->m_source->m_segments != this->m_segments) {
	    this->unshare ();
	}

	return;
    }

    if (this->m_player) {
	this->m_player->setSegments (this->m_segments);
    }

    this->reconcileSharers ();
}

void CVideo::seek (const double seconds) {
    if (const auto& player = this->root ().m_player) {
	player->seek (seconds);
    }
}

std::optional<double> CVideo::parseTime (const std::string& value) {
    double seconds = 0.0;
    std::size_t begin = 0;

    for (int field = 0; field < 3; field++) {
	const auto colon = value.find (':', begin);
	const std::string part = value.substr (begin, colon == std::string::npos ? colon : colon - begin);
	double number;
	std::size_t used = 0;

	try {
	    number = std::stod (part, &used);
	} catch (const std::exception&) {
	    return std::nullopt;
	}

	if (used != part.size () || !std::isfinite (number) || number < 0.0) {
	    return std::nullopt;
	}

	seconds = seconds * 60.0 + number;

	if (colon == std::string::npos) {
	    return seconds;
	}

	begin = colon + 1;
    }

    return std::nullopt;
}

std::optional<VideoPlayback::VideoSegments> CVideo::parseSegments (const std::string& value) {
    VideoPlayback::VideoSegments segments;
    std::size_t begin = 0;

    const auto trim = [] (std::string text) {
	const auto first = text.find_first_not_of (" \t");
	const auto last = text.find_last_not_of (" \t");
	return first == std::string::npos ? std::string () : text.substr (first, last - first + 1);
    };

    while (begin <= value.size ()) {
	const auto comma = value.find (',', begin);
	const std::string part = trim (value.substr (begin, comma == std::string::npos ? comma : comma - begin));
	begin = comma == std::string::npos ? value.size () + 1 : comma + 1;

	if (part.empty ()) {
	    continue;
	}

	const auto dash = part.find ('-');

	if (dash == std::string::npos) {
	    return std::nullopt;
	}

	const std::string startText = trim (part.substr (0, dash));
	const std::string endText = trim (part.substr (dash + 1));
	VideoPlayback::VideoSegment segment;

	if (!startText.empty ()) {
	    const auto start = parseTime (startText);

	    if (!start.has_value ()) {
		return std::nullopt;
	    }

	    segment.start = *start;
	}

	if (!endText.empty ()) {
	    segment.end = parseTime (endText);

	    if (!segment.end.has_value ()) {
		return std::nullopt;
	    }
	}

	segments.push_back (segment);
    }

    return segments;
}

VideoPlayback::VideoSegments CVideo::normalizeSegments (VideoPlayback::VideoSegments segments) {
    std::erase_if (segments, [] (const VideoPlayback::VideoSegment& segment) {
	if (!segment.end.has_value () || *segment.end > segment.start) {
	    return false;
	}

	sLog.error ("Video part ", segment.start, "s - ", *segment.end, "s ends before it starts, skipping it");
	return true;
    });

    std::sort (segments.begin (), segments.end (), [] (const auto& a, const auto& b) { return a.start < b.start; });

    VideoPlayback::VideoSegments merged;

    for (const auto& segment : segments) {
	if (merged.empty () || (merged.back ().end.has_value () && segment.start > *merged.back ().end)) {
	    merged.push_back (segment);
	    continue;
	}

	auto& last = merged.back ();

	if (!segment.end.has_value ()) {
	    last.end.reset ();
	} else if (last.end.has_value ()) {
	    last.end = std::max (*last.end, *segment.end);
	}
    }

    if (merged.size () == 1 && merged.front ().start <= 0.0 && !merged.front ().end.has_value ()) {
	merged.clear ();
    }

    return merged;
}

void CVideo::setPause (bool newState) {
    this->m_wantsPause = newState;
    this->root ().applyPause ();
}

int CVideo::getWidth () const {
    if (this->m_player) {
	return this->m_player->getWidth ();
    }

    return this->m_source != nullptr ? this->m_source->getWidth () : 16;
}

int CVideo::getHeight () const {
    if (this->m_player) {
	return this->m_player->getHeight ();
    }

    return this->m_source != nullptr ? this->m_source->getHeight () : 16;
}

std::unordered_map<std::filesystem::path, CVideo*> CVideo::s_sources;
