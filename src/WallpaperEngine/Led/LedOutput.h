#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "LedLayout.h"

namespace WallpaperEngine::Led {
class LedOutput {
public:
    struct Options {
	std::string host = "127.0.0.1";
	uint16_t port = 6742;
	Brightness brightness;
	/** ledextensions64.dll's "reducefps": at most 15 updates a second */
	bool reduceFps = false;
    };

    LedOutput () = default;
    ~LedOutput ();
    LedOutput (const LedOutput&) = delete;
    LedOutput& operator= (const LedOutput&) = delete;

    void start (const Options& options);
    void stop ();
    [[nodiscard]] bool running () const { return this->m_running.load (); }

    void submit (std::span<const uint8_t> rgb);

    [[nodiscard]] size_t deviceCount () const { return this->m_devices.load (); }

private:
    struct Connection;

    void run ();
    bool connectAndServe (const Options& options);

    Options m_options;
    std::thread m_thread;
    std::atomic<bool> m_running = false;
    std::atomic<size_t> m_devices = 0;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::vector<uint8_t> m_frame;
    bool m_fresh = false;
    bool m_protocolReported = false;
};
}
