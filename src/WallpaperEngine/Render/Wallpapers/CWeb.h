#pragma once

#include <chrono>
#include <glm/ext.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <sys/types.h>
#include <utility>
#include <vector>

#include "WallpaperEngine/Audio/AudioStream.h"
#include "WallpaperEngine/Media/MediaSource.h"
#include "WallpaperEngine/Render/CWallpaper.h"
#include "WallpaperEngine/WebBrowser/IPC/WebHostSharedMemory.h"

#include "WallpaperEngine/Data/Model/Wallpaper.h"

namespace WallpaperEngine::Render::Wallpapers {
// Web wallpapers are rendered by CEF, which this never embeds directly (CEF only supports one
// CefInitialize()/CefShutdown() pair per process, so switching a running process between hosting
// and not hosting a web wallpaper isn't safe). Instead CWeb spawns a disposable child process
// (WallpaperApplication::runWebHost(), a self re-exec with --web-host) that owns CEF for exactly
// one browser, and reads its rendered frames back through shared memory - matching how upstream
// Wallpaper Engine on Windows delegates web wallpapers to a separate webwallpaper.exe process
// rather than hosting CEF in the main process.
class CWeb : public CWallpaper {
public:
    CWeb (
	const Wallpaper& wallpaper, RenderContext& context, AudioContext& audioContext,
	const std::filesystem::path& resolvedBackgroundPath, const WallpaperState::TextureUVsScaling& scalingMode,
	const uint32_t& clampMode, const glm::ivec2& maxRenderSize
    );
    ~CWeb () override;
    [[nodiscard]] int getWidth () const override { return this->m_width; }

    [[nodiscard]] int getHeight () const override { return this->m_height; }

    void setSize (int width, int height);

    /** ambientVolume==0 (or muted) mutes the host's CEF browser; CEF exposes no analog volume control */
    void setAudioPolicy (bool muted, std::optional<int> ambientVolume) override;

protected:
    void renderFrame (const glm::ivec4& viewport) override;
    void updateMouse (const glm::ivec4& viewport);
    /** Mirrors the media player state into the shared segment for the host process to forward to the page */
    void updateMedia ();
    const Web& getWeb () const { return *this->getWallpaperData ().as<Web> (); }

    friend class CWallpaper;

private:
    void spawnHost (const std::filesystem::path& resolvedBackgroundPath);
    // asks the host for the next page frame, at most once per m_frameRequestDivisor renders
    void requestPageFrame ();

    WallpaperEngine::WebBrowser::IPC::WebHostSharedMemory* m_shm = nullptr;
    std::string m_shmName;
    uint32_t m_shmMaxWidth = 0;
    uint32_t m_shmMaxHeight = 0;
    // slot of the shared triple buffer this side owns, the one uploaded to the GL texture
    uint32_t m_frontSlot = 2;
    // size of the texture as currently allocated on the GPU, so renderFrame() can tell whether it
    // needs to reallocate storage (glTexImage2D) or can just update pixels in place (glTexSubImage2D)
    uint32_t m_uploadedTextureWidth = 0;
    uint32_t m_uploadedTextureHeight = 0;
    // Renders come at whatever rate the display and --fps allow, but the page only needs about 60 frames a second:
    // on a 240Hz display every 4th render asks for one, which keeps the cadence even instead of 4x the paint and copy
    // work. Derived from the measured render interval.
    std::chrono::steady_clock::time_point m_lastRender {};
    double m_renderInterval = 0.0;
    uint32_t m_frameRequestDivisor = 1;
    uint32_t m_rendersSinceRequest = 0;
    // LWE_WEB_STATS=1 logs how many renders and page frames per second this wallpaper gets, to tell a slow page from a
    // slow engine when web wallpapers degrade
    bool m_statsEnabled = false;
    std::chrono::steady_clock::time_point m_statsStart {};
    uint32_t m_statsRenders = 0;
    uint32_t m_statsFrames = 0;
    pid_t m_hostPid = -1;
    // registered with the audio recorder so spectra are pushed to the host as soon as they're computed instead of
    // being sampled once per rendered frame
    int m_spectrumListenerId = 0;
    Media::MediaSource::MediaInfo m_lastMedia {};
    bool m_mediaPublished = false;
    uint32_t m_coverVersion = 0;
    bool m_helperFailureLogged = false;

    int m_width = 16;
    int m_height = 17;

    glm::vec2 m_mousePosition = {};
    glm::vec2 m_mousePositionLast = {};
};
}
