#include "CoverDownloader.h"

#include "CoverImage.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Logging/ThreadGuard.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/dict.h>
}

using namespace WallpaperEngine::Media;

namespace {
constexpr auto DOWNLOAD_DEADLINE = std::chrono::seconds (15);
constexpr size_t MAX_CACHED_COVERS = 64;

std::filesystem::path cacheDirectory () {
    if (const char* home = std::getenv ("XDG_CACHE_HOME"); home != nullptr && home[0] == '/') {
	return std::filesystem::path (home) / "linux-wallpaperengine" / "covers";
    }

    if (const char* home = std::getenv ("HOME"); home != nullptr && home[0] != '\0') {
	return std::filesystem::path (home) / ".cache" / "linux-wallpaperengine" / "covers";
    }

    return std::filesystem::temp_directory_path () / "linux-wallpaperengine-covers";
}

// some FFmpeg builds don't find the CA bundle themselves
const char* certificateBundle () {
    for (const char* path :
	 { "/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/cert.pem" }) {
	if (std::filesystem::exists (path)) {
	    return path;
	}
    }

    return nullptr;
}

std::string hashName (const std::string& url) {
    uint64_t hash = 0xcbf29ce484222325ULL;

    for (const unsigned char c : url) {
	hash = (hash ^ c) * 0x100000001b3ULL;
    }

    char name[17];
    std::snprintf (name, sizeof (name), "%016llx", static_cast<unsigned long long> (hash));

    return name;
}

struct Interrupt {
    const std::atomic<bool>& cancelled;
    std::chrono::steady_clock::time_point deadline;
};

int shouldAbort (void* opaque) {
    const auto* interrupt = static_cast<const Interrupt*> (opaque);

    return interrupt->cancelled || std::chrono::steady_clock::now () > interrupt->deadline;
}
}

CoverDownloader::CoverDownloader () : m_directory (cacheDirectory ()) { }

CoverDownloader::~CoverDownloader () {
    for (const auto& job : this->m_jobs) {
	job->cancelled = true;
    }

    for (const auto& job : this->m_jobs) {
	if (job->thread.joinable ()) {
	    job->thread.join ();
	}
    }
}

std::filesystem::path CoverDownloader::cachePath (const std::string& url) const {
    return this->m_directory / (hashName (url) + ".img");
}

std::optional<std::filesystem::path> CoverDownloader::request (const std::string& url) {
    for (const auto& job : this->m_jobs) {
	if (job->url != url) {
	    job->cancelled = true;
	}
    }

    const auto target = this->cachePath (url);
    std::error_code error;

    if (std::filesystem::is_regular_file (target, error)) {
	std::filesystem::last_write_time (target, std::filesystem::file_time_type::clock::now (), error);

	return target;
    }

    const bool running = std::ranges::any_of (this->m_jobs, [&] (const auto& job) {
	return job->url == url && !job->cancelled && !job->finished;
    });

    if (running) {
	return std::nullopt;
    }

    auto job = std::make_unique<Job> ();
    job->url = url;
    job->target = target;
    job->thread = std::thread ([raw = job.get ()] {
	WallpaperEngine::Logging::guardThread ("Cover download", [raw] { run (*raw); });
	raw->finished = true;
    });
    this->m_jobs.push_back (std::move (job));

    return std::nullopt;
}

std::vector<CoverDownloader::Finished> CoverDownloader::collect () {
    std::vector<Finished> done;

    for (auto it = this->m_jobs.begin (); it != this->m_jobs.end ();) {
	auto& job = **it;

	if (!job.finished) {
	    ++it;
	    continue;
	}

	job.thread.join ();

	done.push_back ({ job.url, job.succeeded ? std::optional (job.target) : std::nullopt });
	it = this->m_jobs.erase (it);
    }

    if (std::ranges::any_of (done, [] (const auto& item) { return item.path.has_value (); })) {
	this->prune ();
    }

    return done;
}

void CoverDownloader::run (Job& job) {
    // needed by older FFmpeg builds before the first https connection
    static const int networkReady = avformat_network_init ();
    (void)networkReady;

    const Interrupt interrupt { job.cancelled, std::chrono::steady_clock::now () + DOWNLOAD_DEADLINE };
    const AVIOInterruptCB callback { shouldAbort, const_cast<Interrupt*> (&interrupt) };

    AVDictionary* options = nullptr;
    av_dict_set (&options, "rw_timeout", "8000000", 0);
    av_dict_set (&options, "user_agent", "linux-wallpaperengine", 0);
    // keep redirects off file: and other local protocols
    av_dict_set (&options, "protocol_whitelist", "http,https,tcp,tls,crypto", 0);
    av_dict_set (&options, "tls_verify", "1", 0);

    if (const char* bundle = certificateBundle ()) {
	av_dict_set (&options, "ca_file", bundle, 0);
    }

    AVIOContext* io = nullptr;
    const int opened = avio_open2 (&io, job.url.c_str (), AVIO_FLAG_READ, &callback, &options);
    av_dict_free (&options);

    if (opened < 0) {
	if (!job.cancelled) {
	    sLog.error ("Cannot download the cover art ", job.url, " (FFmpeg error ", opened, ")");
	}

	job.finished = true;
	return;
    }

    std::vector<uint8_t> bytes;
    uint8_t chunk[16 * 1024];

    while (true) {
	const int count = avio_read (io, chunk, sizeof (chunk));

	if (count <= 0) {
	    break;
	}

	bytes.insert (bytes.end (), chunk, chunk + count);

	if (bytes.size () > MAX_COVER_BYTES) {
	    bytes.clear ();
	    break;
	}
    }

    avio_closep (&io);

    if (!job.cancelled && !bytes.empty () && decodeCoverImage (bytes).has_value ()) {
	std::error_code error;
	std::filesystem::create_directories (job.target.parent_path (), error);

	// two jobs for one url can overlap
	static std::atomic<unsigned> counter = 0;
	auto partial = job.target;
	partial += "." + std::to_string (counter++) + ".part";

	{
	    std::ofstream file (partial, std::ios::binary | std::ios::trunc);
	    file.write (reinterpret_cast<const char*> (bytes.data ()), static_cast<std::streamsize> (bytes.size ()));
	}

	std::filesystem::rename (partial, job.target, error);
	job.succeeded = !error;

	if (error) {
	    std::filesystem::remove (partial, error);
	}
    } else if (!job.cancelled) {
	sLog.error ("The cover art ", job.url, " is not a usable image");
    }

    job.finished = true;
}

void CoverDownloader::prune () const {
    std::error_code error;
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;

    for (const auto& entry : std::filesystem::directory_iterator (this->m_directory, error)) {
	if (entry.is_regular_file (error) && entry.path ().extension () == ".img") {
	    files.emplace_back (entry.last_write_time (error), entry.path ());
	}
    }

    if (files.size () <= MAX_CACHED_COVERS) {
	return;
    }

    std::ranges::sort (files);

    for (size_t i = 0; i + MAX_CACHED_COVERS < files.size (); i++) {
	std::filesystem::remove (files[i].second, error);
    }
}
