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

namespace WallpaperEngine::Audio::Drivers {
class PulseProcessVolume;
}

namespace WallpaperEngine::Render::Wallpapers {
class WebDmabufImport;
}

namespace WallpaperEngine::Render::Wallpapers {
// CEF allows one CefInitialize()/CefShutdown() per process, so a disposable child (runWebHost(), a --web-host re-exec)
// owns it for one browser and hands frames back through shared memory, like WE's separate webwallpaper.exe
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
    [[nodiscard]] float getAspectRatio () const override { return 0.0f; }

    void setSize (int width, int height);

    /** CEF can only mute: silence mutes the browser, other volumes go to the host's PulseAudio streams */
    void setAudioPolicy (bool muted, std::optional<int> ambientVolume) override;

    [[nodiscard]] bool wantsPointerButtons () const override { return true; }

    void setPropertyOverrides (const std::map<std::string, std::string>& overrides);
    [[nodiscard]] bool hasContent () const override;

protected:
    void renderFrame (const glm::ivec4& viewport) override;
    void updateMouse (const glm::ivec4& viewport);
    void updateMedia ();
    const Web& getWeb () const { return *this->getWallpaperData ().as<Web> (); }

    friend class CWallpaper;

private:
    enum class FrameMode { Shm, Dmabuf };

    [[nodiscard]] FrameMode chooseFrameMode () const;
    void spawnHost (const std::filesystem::path& resolvedBackgroundPath);
    void stopHost (bool graceful);
    void fallBackToShm (const char* reason);
    void renderDmabufFrame ();
    void noteFirstContent ();
    void requestPageFrame ();
    // a slot number or size the host wrote that doesn't fit the segment: nothing is read, the next frame is requested
    void rejectFrame ();

    std::filesystem::path m_hostBackgroundPath;
    FrameMode m_frameMode = FrameMode::Shm;
    // dmabuf mode: our end of the socket the host's buffers arrive on, and what imports them
    int m_dmabufSocket = -1;
    std::unique_ptr<WebDmabufImport> m_dmabufImport;
    uint64_t m_dmabufFrames = 0;
    std::chrono::steady_clock::time_point m_helperReadyAt {};
    float m_audioVolume = 1.0f;
    WallpaperEngine::WebBrowser::IPC::WebHostSharedMemory* m_shm = nullptr;
    std::string m_shmName;
    uint32_t m_shmMaxWidth = 0;
    uint32_t m_shmMaxHeight = 0;
    uint32_t m_frontSlot = 2;
    uint32_t m_uploadedTextureWidth = 0;
    uint32_t m_uploadedTextureHeight = 0;
    // the page only needs about 60 fps: on 240Hz every 4th render asks for a frame, derived from the measured render
    // interval
    std::chrono::steady_clock::time_point m_lastRender {};
    double m_renderInterval = 0.0;
    uint32_t m_frameRequestDivisor = 1;
    uint32_t m_rendersSinceRequest = 0;
    // LWE_WEB_STATS=1 logs renders and page frames per second (slow page or slow engine)
    bool m_statsEnabled = false;
    std::chrono::steady_clock::time_point m_statsStart {};
    uint32_t m_statsRenders = 0;
    uint32_t m_statsFrames = 0;
    pid_t m_hostPid = -1;
    std::unique_ptr<Audio::Drivers::PulseProcessVolume> m_streamVolume;
    // spectra are pushed to the host as soon as they're computed, not sampled per render
    int m_spectrumListenerId = 0;
    // the application can swap recorders on a hotswap, the listener has to come off the one it went on
    Audio::Drivers::Recorders::PlaybackRecorder* m_spectrumRecorder = nullptr;
    Media::MediaSource::MediaInfo m_lastMedia {};
    bool m_mediaPublished = false;
    uint32_t m_coverVersion = 0;
    bool m_helperFailureLogged = false;
    bool m_badFrameLogged = false;
    std::chrono::steady_clock::time_point m_created = std::chrono::steady_clock::now ();
    bool m_pageHasContent = false;

    int m_width = 16;
    int m_height = 17;

    glm::vec2 m_mousePosition = {};
    glm::vec2 m_mousePositionLast = {};
};
}
