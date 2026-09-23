#include "PageBridge.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <ranges>

#include "BrowserClient.h"
#include "WallpaperEngine/Logging/Log.h"
#include "include/cef_parser.h"

using namespace WallpaperEngine::WebBrowser::CEF;
using namespace WallpaperEngine::WebBrowser::IPC;
using namespace WallpaperEngine::Data::Model;

namespace {
constexpr std::size_t MAX_COVER_SIZE = 8 * 1024 * 1024;
constexpr auto DIRECTORY_SCAN_INTERVAL = std::chrono::seconds (2);

void runScript (const CefRefPtr<CefFrame>& frame, const std::string& script) {
    frame->ExecuteJavaScript (script, frame->GetURL (), 0);
}

// pages get every string as it is on disk / on the bus, which is not guaranteed to be valid UTF-8
std::string toJson (const nlohmann::json& value) {
    return value.dump (-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::string lowerExtension (const std::filesystem::path& path) {
    std::string extension = path.extension ().string ();

    std::ranges::transform (extension, extension.begin (), [] (unsigned char c) { return std::tolower (c); });

    return extension;
}

// Same split Wallpaper Engine makes for directory properties: images unless the project asks for videos
bool matchesFileType (const std::filesystem::path& path, const std::string& fileType) {
    static const std::array images = { ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webp", ".tga", ".tif", ".tiff",
				       ".avif", ".jfif", ".svg" };
    static const std::array videos = { ".mp4", ".webm", ".mkv", ".avi", ".mov", ".m4v", ".ogv", ".wmv", ".flv" };

    const std::string extension = lowerExtension (path);

    if (fileType == "video") {
	return std::ranges::find (videos, extension) != videos.end ();
    }

    return std::ranges::find (images, extension) != images.end ();
}

const char* imageMimeType (const std::string& data) {
    if (data.starts_with ("\x89PNG")) {
	return "image/png";
    }
    if (data.starts_with ("\xFF\xD8")) {
	return "image/jpeg";
    }
    if (data.starts_with ("GIF8")) {
	return "image/gif";
    }
    if (data.size () > 12 && data.compare (0, 4, "RIFF") == 0 && data.compare (8, 4, "WEBP") == 0) {
	return "image/webp";
    }
    if (data.starts_with ("BM")) {
	return "image/bmp";
    }

    return nullptr;
}

// Media players hand out local files, pages get them inline like they do on Windows. Anything that isn't a known
// image format is not sent at all - the path comes from whatever program registered on the bus.
std::string coverDataUrl (const std::string& path) {
    if (path.empty ()) {
	return "";
    }

    std::error_code error;

    if (!std::filesystem::is_regular_file (path, error) || std::filesystem::file_size (path, error) > MAX_COVER_SIZE) {
	return "";
    }

    std::ifstream file (path, std::ios::binary);
    const std::string data ((std::istreambuf_iterator<char> (file)), std::istreambuf_iterator<char> ());
    const char* mime = imageMimeType (data);

    if (mime == nullptr) {
	return "";
    }

    return std::string ("data:") + mime + ";base64," + CefBase64Encode (data.data (), data.size ()).ToString ();
}

std::string cssColor (const uint8_t (&color)[3]) {
    return "rgb(" + std::to_string (color[0]) + ", " + std::to_string (color[1]) + ", " + std::to_string (color[2])
	+ ")";
}
} // namespace

PageBridge::PageBridge (
    CefRefPtr<CefBrowser> browser, const WebHostSharedMemory& shm, const Properties& properties,
    const BrowserClient& client
) : m_browser (std::move (browser)), m_shm (shm), m_properties (properties), m_client (client) {
    this->m_lastAudioSeq = shm.audioSeq.load (std::memory_order_relaxed);
}

void PageBridge::update () {
    if (!this->m_client.isLoaded ()) {
	return;
    }

    const auto frame = this->m_browser->GetMainFrame ();

    if (!frame) {
	return;
    }

    const auto now = std::chrono::steady_clock::now ();

    if (!this->m_started) {
	this->m_started = true;
	this->m_lastDirectoryScan = now;

	runScript (frame, "window.__lweMedia&&window.__lweMedia({status:{enabled:true}});");
	this->scanDirectories (frame);

	// the page shim holds the user properties back until this, so pages get their files first
	runScript (frame, "window.__lweDirectoriesReady&&window.__lweDirectoriesReady();");
    }

    this->forwardAudio (frame);
    this->forwardMedia (frame);

    if (now - this->m_lastDirectoryScan >= DIRECTORY_SCAN_INTERVAL) {
	this->m_lastDirectoryScan = now;
	this->scanDirectories (frame);
    }
}

// the recorder only produces a mono spectrum, so both channels get the same data
void PageBridge::forwardAudio (CefRefPtr<CefFrame> frame) {
    constexpr std::size_t bands = WebHostSharedMemory::AUDIO_BANDS;

    const uint32_t seq = this->m_shm.audioSeq.load (std::memory_order_acquire);

    if (seq == this->m_lastAudioSeq) {
	return;
    }

    this->m_lastAudioSeq = seq;

    float values[bands];
    bool silent = true;

    for (std::size_t i = 0; i < bands; i++) {
	values[i] = this->m_shm.audioBands[i].load (std::memory_order_relaxed);
	silent = silent && values[i] <= 0.0f;
    }

    // a recorder that isn't running (or a quiet system) would otherwise cost a script call per frame for nothing, but
    // one all-zero frame still has to go out so the visualizer settles back down
    if (silent && this->m_lastAudioSilent) {
	return;
    }

    this->m_lastAudioSilent = silent;

    std::string script = "window.__lweAudio&&window.__lweAudio([";
    char number[24];

    for (int channel = 0; channel < 2; channel++) {
	for (std::size_t i = 0; i < bands; i++) {
	    snprintf (number, sizeof (number), "%.4f,", values[i]);
	    script += number;
	}
    }

    script.back () = ']';
    script += ");";

    runScript (frame, script);
}

void PageBridge::forwardMedia (CefRefPtr<CefFrame> frame) {
    const auto& shm = this->m_shm;
    const uint32_t seq = shm.mediaSeq.load (std::memory_order_acquire);

    // odd means the main process is in the middle of writing it
    if (seq == this->m_lastMediaSeq || seq % 2 != 0) {
	return;
    }

    const int32_t state = shm.mediaState;
    const double position = shm.mediaPosition;
    const double duration = shm.mediaDuration;
    const uint32_t coverVersion = shm.mediaCoverVersion;
    const std::string title (shm.mediaTitle, strnlen (shm.mediaTitle, WebHostSharedMemory::MEDIA_TEXT));
    const std::string artist (shm.mediaArtist, strnlen (shm.mediaArtist, WebHostSharedMemory::MEDIA_TEXT));
    const std::string album (shm.mediaAlbum, strnlen (shm.mediaAlbum, WebHostSharedMemory::MEDIA_TEXT));
    const std::string coverPath (shm.mediaCoverPath, strnlen (shm.mediaCoverPath, WebHostSharedMemory::MEDIA_PATH));
    uint8_t palette[5][3];

    std::copy (&shm.mediaPalette[0][0], &shm.mediaPalette[0][0] + 15, &palette[0][0]);
    std::atomic_thread_fence (std::memory_order_seq_cst);

    // written to while we were reading, try again next time round
    if (shm.mediaSeq.load (std::memory_order_acquire) != seq) {
	return;
    }

    this->m_lastMediaSeq = seq;

    nlohmann::json event = {
	{ "properties",
	  { { "title", title },
	    { "artist", artist },
	    { "subTitle", "" },
	    { "albumTitle", album },
	    { "albumArtist", artist },
	    { "genres", "" },
	    { "contentType", "" } } },
	{ "playback", { { "state", state } } },
	{ "timeline", { { "position", position }, { "duration", duration } } },
    };

    if (coverVersion != this->m_lastCoverVersion) {
	this->m_lastCoverVersion = coverVersion;

	event["thumbnail"] = {
	    { "thumbnail", coverDataUrl (coverPath) },
	    { "primaryColor", cssColor (palette[0]) },
	    { "secondaryColor", cssColor (palette[1]) },
	    { "tertiaryColor", cssColor (palette[2]) },
	    { "textColor", cssColor (palette[3]) },
	    { "highContrastColor", cssColor (palette[4]) },
	};
    }

    sLog.out (
	"--web-host: media state ", state, " (0 stopped, 1 playing, 2 paused), title '", title, "', artist '", artist,
	"', cover ", coverPath.empty () ? "none" : coverPath
    );

    runScript (frame, "window.__lweMedia&&window.__lweMedia(" + toJson (event) + ");");
}

// polled, this process has nothing to receive change notifications on
void PageBridge::scanDirectories (CefRefPtr<CefFrame> frame) {
    for (const auto& [name, property] : this->m_properties) {
	const auto* file = dynamic_cast<const PropertyFile*> (property.get ());

	if (file == nullptr || !file->isDirectory ()) {
	    continue;
	}

	FileTimes current;
	const std::string directory = file->getString ();
	std::error_code error;

	if (!directory.empty () && !std::filesystem::is_directory (directory, error)) {
	    if (this->m_reportedMissing.insert (name).second) {
		sLog.error ("--web-host: directory property ", name, " points to a folder that does not exist: ", directory);
	    }
	} else if (!directory.empty ()) {
	    for (std::filesystem::directory_iterator it (directory, error), end; !error && it != end; it.increment (error)) {
		const auto& entry = *it;
		std::error_code entryError;

		if (entry.path ().filename ().string ().starts_with (".") || !entry.is_regular_file (entryError)
		    || !matchesFileType (entry.path (), file->getFileType ())) {
		    continue;
		}

		current[entry.path ().string ()] = entry.last_write_time (entryError);
	    }
	}

	auto& known = this->m_directories[name];
	std::vector<std::string> changed;
	std::vector<std::string> removed;

	for (const auto& [path, time] : current) {
	    if (const auto previous = known.find (path); previous == known.end () || previous->second != time) {
		changed.push_back (path);
	    }
	}

	for (const auto& path : known | std::views::keys) {
	    if (!current.contains (path)) {
		removed.push_back (path);
	    }
	}

	known = std::move (current);

	if (!changed.empty () || !removed.empty ()) {
	    sLog.out (
		"--web-host: directory property ", name, ": ", changed.size (), " new or changed and ", removed.size (),
		" removed file(s) in ", directory
	    );
	}

	if (!changed.empty ()) {
	    runScript (
		frame, "window.__lweDirectory&&window.__lweDirectory('added'," + toJson (name) + "," + toJson (changed) + ");"
	    );
	}

	if (!removed.empty ()) {
	    runScript (
		frame,
		"window.__lweDirectory&&window.__lweDirectory('removed'," + toJson (name) + "," + toJson (removed) + ");"
	    );
	}
    }
}
