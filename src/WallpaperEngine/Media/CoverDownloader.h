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

    std::optional<std::filesystem::path> request (const std::string& url);

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
}
