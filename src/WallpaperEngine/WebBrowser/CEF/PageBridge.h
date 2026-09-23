#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/WebBrowser/IPC/WebHostSharedMemory.h"
#include "include/cef_browser.h"

namespace WallpaperEngine::WebBrowser::CEF {
class BrowserClient;

/**
 * Runs in the web host process and feeds the page the audio spectrum, media player state and directory property
 * contents it gets from the main process through shared memory. The page side is the script SubprocessApp injects.
 */
class PageBridge {
public:
    PageBridge (
	CefRefPtr<CefBrowser> browser, const IPC::WebHostSharedMemory& shm,
	const WallpaperEngine::Data::Model::Properties& properties, const BrowserClient& client
    );

    /** Call regularly from the host loop, does nothing until the page finished loading */
    void update ();

private:
    using FileTimes = std::map<std::string, std::filesystem::file_time_type>;

    void forwardAudio (CefRefPtr<CefFrame> frame);
    void forwardMedia (CefRefPtr<CefFrame> frame);
    void scanDirectories (CefRefPtr<CefFrame> frame);

    CefRefPtr<CefBrowser> m_browser;
    const IPC::WebHostSharedMemory& m_shm;
    const WallpaperEngine::Data::Model::Properties& m_properties;
    const BrowserClient& m_client;

    bool m_started = false;
    uint32_t m_lastAudioSeq = 0;
    bool m_lastAudioSilent = true;
    uint32_t m_lastMediaSeq = 0;
    uint32_t m_lastCoverVersion = 0;
    std::chrono::steady_clock::time_point m_lastDirectoryScan;
    std::map<std::string, FileTimes> m_directories;
    std::set<std::string> m_reportedMissing;
};
} // namespace WallpaperEngine::WebBrowser::CEF
