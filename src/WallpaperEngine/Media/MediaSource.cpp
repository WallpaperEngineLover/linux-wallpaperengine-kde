#include "MediaSource.h"

#include "CoverDownloader.h"
#include "CoverImage.h"

#include <ranges>

using namespace WallpaperEngine::Media;

MediaSource::MediaSource (std::chrono::milliseconds updateInterval) :
    m_aliveFlag (std::make_unique<bool> (true)), m_mediaInfo (
						     {
							 .playbackState = PlaybackState::Stopped,
							 .title = "",
							 .artist = "",
							 .url = std::nullopt,
							 .duration = 0.0f,
							 .position = 0.0f,
							 .available = false,
						     }
						 ),
    m_updateInterval (updateInterval) { }

MediaSource::~MediaSource () {
    if (this->m_aliveFlag) {
	*m_aliveFlag = false;
    }

    m_albumArtListeners.clear ();
    m_metadataListeners.clear ();
}

void MediaSource::update () {
    if (this->m_covers) {
	for (const auto& finished : this->m_covers->collect ()) {
	    // a download of the previous track can land after the player moved on
	    if (!finished.path.has_value () || this->m_mediaInfo.url != finished.url) {
		continue;
	    }

	    this->m_mediaInfo.cover = finished.path;
	    this->notifyAlbumArtListeners ();
	}
    }

    auto now = std::chrono::steady_clock::now ();

    if (now <= m_nextUpdate) {
	return;
    }

    m_nextUpdate = now + m_updateInterval;

    this->performUpdate ();
}

std::function<void ()> MediaSource::addMetadataListener (std::function<void (const MediaInfo&)> listener) {
    int listenerId = ++m_metadataListenerId;

    m_metadataListeners.emplace (listenerId, listener);

    auto alive = m_aliveFlag;

    return [this, listenerId, alive] () {
	if (!alive || !*alive) {
	    return;
	}

	m_metadataListeners.erase (listenerId);
    };
}

std::function<void ()> MediaSource::addAlbumArtListener (std::function<void (const MediaInfo&)> listener) {
    int listenerId = ++m_albumArtListenerId;

    m_albumArtListeners.emplace (listenerId, listener);

    auto alive = m_aliveFlag;

    return [this, listenerId, alive] () {
	if (!alive || !*alive) {
	    return;
	}

	m_albumArtListeners.erase (listenerId);
    };
}

void MediaSource::fireMetadataListeners () {
    for (auto& listener : m_metadataListeners | std::views::values) {
	listener (m_mediaInfo);
    }
}

void MediaSource::fireAlbumArtListeners () {
    this->resolveCover ();
    this->notifyAlbumArtListeners ();
}

void MediaSource::notifyAlbumArtListeners () {
    for (auto& listener : m_albumArtListeners | std::views::values) {
	listener (m_mediaInfo);
    }
}

void MediaSource::resolveCover () {
    this->m_mediaInfo.cover.reset ();

    if (!this->m_mediaInfo.url.has_value ()) {
	return;
    }

    const auto& url = *this->m_mediaInfo.url;

    if (url.starts_with ("http://") || url.starts_with ("https://")) {
	if (!this->m_covers) {
	    this->m_covers = std::make_unique<CoverDownloader> ();
	}

	this->m_mediaInfo.cover = this->m_covers->request (url);
	return;
    }

    if (const auto path = localCoverPath (url); path.has_value ()) {
	std::error_code error;

	if (std::filesystem::is_regular_file (*path, error)) {
	    this->m_mediaInfo.cover = *path;
	}
    }
}