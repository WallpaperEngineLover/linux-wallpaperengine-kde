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

// web host side: feeds the page the spectrum, media state and directory contents from shared memory; the page side is
// SubprocessApp's script
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
    /** changed ones go to applyUserProperties */
    void forwardProperties (CefRefPtr<CefFrame> frame);
    void scanDirectories (CefRefPtr<CefFrame> frame);

    CefRefPtr<CefBrowser> m_browser;
    const IPC::WebHostSharedMemory& m_shm;
    const WallpaperEngine::Data::Model::Properties& m_properties;
    const BrowserClient& m_client;

    bool m_started = false;
    std::chrono::steady_clock::time_point m_nextAudio;
    uint32_t m_lastMediaSeq = 0;
    uint32_t m_lastPropertiesSeq = 0;
    uint32_t m_lastCoverVersion = 0;
    std::chrono::steady_clock::time_point m_lastDirectoryScan;
    std::map<std::string, FileTimes> m_directories;
    std::set<std::string> m_reportedMissing;
};
} // namespace WallpaperEngine::WebBrowser::CEF
