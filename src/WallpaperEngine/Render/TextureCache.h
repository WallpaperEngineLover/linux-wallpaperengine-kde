#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>

#include "TextureProvider.h"
#include "WallpaperEngine/Data/Assets/Types.h"
#include "WallpaperEngine/Data/Model/Types.h"
#include "WallpaperEngine/Data/Utils/BinaryReader.h"
#include "WallpaperEngine/Debugging/ThreadSafety.h"
#include "WallpaperEngine/Render/Helpers/ContextAware.h"
#include "WallpaperEngine/Render/RenderContext.h"

using namespace WallpaperEngine::Render;

namespace WallpaperEngine::Render {
class AlbumTexture;
class CTexture;
namespace Helpers {
    class ContextAware;
}

class RenderContext;

class TextureCache final : Helpers::ContextAware {
public:
    explicit TextureCache (RenderContext& context);
    ~TextureCache () override;

    std::shared_ptr<const TextureProvider> resolve (const std::string& filename, const Data::Model::Project& project);
    [[nodiscard]] std::shared_ptr<const TextureProvider>
    findLoaded (const std::string& filename, const Data::Model::Project& project) const;

    /** The texture name of an icon the engine picked for a user shortcut; a name like it that comes from a wallpaper's
     * own files is not an icon and resolves like any other missing texture */
    [[nodiscard]] std::string iconTextureName (const std::filesystem::path& icon);

    void store (const std::string& name, std::shared_ptr<const TextureProvider> texture);

    void prune ();

    void repaintConditionalTextures ();

    void beginPrefetch (
	const Data::Model::Project& project,
	std::function<void (const std::function<bool (std::vector<std::string>)>& found)> names
    );
    /** Runs a task on the prefetch workers, behind what is queued. endPrefetch drops the ones not started yet and
     * waits for the ones running. The task must not touch the scene being built */
    void queueBackgroundTask (std::function<void ()> task);
    void endPrefetch ();

private:
    struct PrefetchResult {
	Data::Assets::TextureUniquePtr parsed = nullptr;
	std::shared_ptr<CTexture> uploaded = nullptr;
    };

    struct Prefetched {
	std::string name;
	const Data::Model::Project* project = nullptr;
	std::promise<PrefetchResult> result {};
	std::future<PrefetchResult> ready {};
	std::shared_ptr<std::atomic<size_t>> budget = nullptr;
	std::atomic<size_t> reserved = 0;
	std::atomic<bool> claimed = false;
	uint64_t generation = 0;

	void release () { *this->budget -= this->reserved.exchange (0); }
	~Prefetched () { this->release (); }
    };

    bool
    prefetchNames (const Data::Model::Project& project, const std::vector<std::string>& names, uint64_t generation);
    void startPrefetchThreads ();
    void prefetchWorker (const std::stop_token& stop);
    void runPrefetch (const std::shared_ptr<Prefetched>& job);
    bool queueUpload (const std::shared_ptr<Prefetched>& job, Data::Assets::TextureUniquePtr& texture);
    void startUploadThread ();
    void uploadWorker (const std::stop_token& stop);
    void scanPrefetch (
	const Data::Model::Project& project,
	const std::function<void (const std::function<bool (std::vector<std::string>)>& found)>& names,
	uint64_t generation
    );
    PrefetchResult takePrefetched (const Data::Model::Project& project, const std::string& filename);

    struct ConditionalTexture {
	std::weak_ptr<CTexture> texture;
	const Data::Model::Project* project = nullptr;
	std::string filename;
	std::vector<std::function<void ()>> listeners = {};
	bool dirty = false;

	~ConditionalTexture () {
	    for (const auto& deregister : this->listeners) {
		deregister ();
	    }
	}
    };

    void followConditions (
	const std::shared_ptr<CTexture>& texture, const Data::Model::Project& project, const std::string& filename,
	const std::vector<std::string>& properties
    );

    std::vector<std::unique_ptr<ConditionalTexture>> m_conditionalTextures = {};
    std::shared_ptr<const AlbumTexture> m_previousThumbnail = nullptr;
    std::shared_ptr<const AlbumTexture> m_currentThumbnail = nullptr;
    std::set<std::string> m_iconNames = {};
    std::map<std::string, std::shared_ptr<const TextureProvider>> m_sharedTextures = {};
    std::map<std::pair<const Data::Model::Project*, std::string>, std::shared_ptr<const TextureProvider>> m_textureCache
	= {};
    std::function<void ()> m_mediaCallback;

    struct PrefetchTask {
	std::function<void ()> run;
	bool tracked = false;
	bool scan = false;
    };

    Debugging::Mutex m_prefetchMutex;
    std::map<std::pair<const Data::Model::Project*, std::string>, std::shared_ptr<Prefetched>>
	m_prefetched LWE_GUARDED_BY (m_prefetchMutex) = {};
    std::set<std::pair<const Data::Model::Project*, std::string>> m_prefetchRequested LWE_GUARDED_BY (m_prefetchMutex)
	= {};
    std::deque<PrefetchTask> m_prefetchQueue LWE_GUARDED_BY (m_prefetchMutex) = {};
    std::deque<PrefetchTask> m_prefetchJobs LWE_GUARDED_BY (m_prefetchMutex) = {};
    uint64_t m_prefetchGeneration LWE_GUARDED_BY (m_prefetchMutex) = 0;
    unsigned int m_prefetchTracked LWE_GUARDED_BY (m_prefetchMutex) = 0;
    bool m_prefetchScanning LWE_GUARDED_BY (m_prefetchMutex) = false;
    std::condition_variable_any m_prefetchScanned = {};
    std::condition_variable_any m_prefetchWake = {};
    std::shared_ptr<std::atomic<size_t>> m_prefetchBudget = std::make_shared<std::atomic<size_t>> (0);
    std::vector<std::jthread> m_prefetchThreads = {};

    struct UploadTask {
	std::shared_ptr<Prefetched> job;
	Data::Assets::TextureUniquePtr texture;
	uint64_t generation = 0;
    };

    Debugging::Mutex m_uploadMutex;
    std::deque<UploadTask> m_uploadQueue LWE_GUARDED_BY (m_uploadMutex) = {};
    std::condition_variable_any m_uploadWake = {};
    std::atomic<bool> m_uploadUnavailable = false;
    std::atomic<int> m_uploadReady = 0;
    std::vector<std::jthread> m_uploadThreads = {};
};
}
