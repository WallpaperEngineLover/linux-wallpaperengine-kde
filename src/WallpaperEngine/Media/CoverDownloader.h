#pragma once

#include <atomic>
#include <filesystem>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace WallpaperEngine::Media {
/**
 * Downloads http(s) cover art on worker threads into the cache folder, so every cover user gets a local path.
 * Uses FFmpeg's I/O layer (already linked, brings TLS) instead of libcurl.
 */
class CoverDownloader {
public:
    struct Finished {
	std::string url;
	std::optional<std::filesystem::path> path;
    };

    CoverDownloader ();
    ~CoverDownloader ();

    CoverDownloader (const CoverDownloader&) = delete;
    CoverDownloader& operator= (const CoverDownloader&) = delete;

    /** The cached copy, or nothing while the download (started once) runs. Cancels downloads of other urls */
    std::optional<std::filesystem::path> request (const std::string& url);

    /** Downloads finished since the last call */
    std::vector<Finished> collect ();

    [[nodiscard]] std::filesystem::path cachePath (const std::string& url) const;

private:
    struct Job {
	std::string url;
	std::filesystem::path target;
	std::thread thread;
	std::atomic<bool> finished = false;
	std::atomic<bool> cancelled = false;
	bool succeeded = false;
    };

    static void run (Job& job);
    void prune () const;

    std::filesystem::path m_directory;
    std::list<std::unique_ptr<Job>> m_jobs;
};
} // namespace WallpaperEngine::Media
