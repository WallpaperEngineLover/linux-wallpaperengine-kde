#include "CVideo.h"

#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/VideoPlayback/MPV/GLPlayer.h"

#include <algorithm>
#include <cmath>

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

    const std::filesystem::path videopath
	= this->getVideo ().project.assetLocator->physicalPath (this->getVideo ().filename);

    // starts at a small framebuffer size; resized once mpv starts playback and reports the real resolution
    this->m_player = std::make_unique<GLPlayer> (
	this->getContext (), this->CWallpaper::getWallpaperTexture (), videopath, 64, 64,
	this->CWallpaper::getWallpaperFramebuffer ()
    );
    // --silent disables audio (audio.enabled = false) and must mute video wallpapers too;
    // a volume of 0 mutes the mpv backend (matching what --volume 0 already does).
    const auto& audioSettings = this->getContext ().getApp ().getContext ().settings.audio;
    this->m_player->setVolume (audioSettings.enabled ? audioSettings.volume * 100.0 / 128.0 : 0.0);
    const auto& renderSettings = this->getContext ().getApp ().getContext ().settings.render;
    this->m_player->setSpeed (renderSettings.playbackSpeed);
    this->setSegments (renderSettings.videoSegments);

    if (hdr) {
	this->m_player->setLinearOutput ();
    }
    // needs at least one usage marked for the video to actually start playing
    this->m_player->incrementUsageCount ();
}

CVideo::~CVideo () { this->m_player->decrementUsageCount (); }

void CVideo::renderFrame (const glm::ivec4& viewport) {
    this->updateMuteState ();

    this->m_presentBackBuffer = false;
    this->m_player->render ();

    // mpv reallocates the texture at the video's size later, the CFBO was made at 16x16
    if (const auto fbo = this->find ("_rt_FullFrameBuffer"); fbo != nullptr
	&& (static_cast<int> (fbo->getRealWidth ()) != this->getWidth ()
	    || static_cast<int> (fbo->getRealHeight ()) != this->getHeight ())) {
	fbo->resize (this->getWidth (), this->getHeight ());
	this->m_player->requestRedraw ();
	this->m_player->render ();
    }

    if (!this->m_hdr) {
	this->renderImageAdjustments ({ this->getWidth (), this->getHeight () }, GL_RGBA8);
    }
}

void CVideo::updateMuteState () {
    if (!this->getContext ().getApp ().getContext ().settings.audio.enabled) {
	return;
    }

    const bool desiredMuted
	= this->m_forceMuted || this->getAudioContext ().getDriver ().getAudioDetector ().anythingPlaying ();

    if (this->m_muted == desiredMuted) {
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
    this->updateMuteState ();
}

const Data::Model::Video& CVideo::getVideo () const { return *this->getWallpaperData ().as<Data::Model::Video> (); }

void CVideo::setVolume (double volume) { this->m_player->setVolume (volume); }

void CVideo::setSpeed (double speed) { this->m_player->setSpeed (speed); }

void CVideo::setSegments (const VideoPlayback::VideoSegments& segments) {
    this->m_player->setSegments (normalizeSegments (segments));
}

void CVideo::seek (const double seconds) { this->m_player->seek (seconds); }

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
    if (newState) {
	this->m_player->setPaused ();
    } else {
	this->m_player->clearPaused ();
    }
}

int CVideo::getWidth () const { return this->m_player ? this->m_player->getWidth () : 16; }

int CVideo::getHeight () const { return this->m_player ? this->m_player->getHeight () : 16; }
