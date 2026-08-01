#pragma once

#include <atomic>
#include <chrono>
#include <random>

#include "WallpaperEngine/Application/ApplicationContext.h"
#include "WallpaperEngine/Assets/AssetLocator.h"

#include "WallpaperEngine/Render/CWallpaper.h"
#include "WallpaperEngine/Render/Drivers/Detectors/FullScreenDetector.h"
#include "WallpaperEngine/Render/Drivers/GLFWOpenGLDriver.h"
#include "WallpaperEngine/Render/Drivers/Output/GLFWWindowOutput.h"
#include "WallpaperEngine/Render/RenderContext.h"

#include "WallpaperEngine/Audio/Drivers/SDLAudioDriver.h"

#include "WallpaperEngine/Input/InputContext.h"
#include "WallpaperEngine/WebBrowser/WebBrowserContext.h"

#include "WallpaperEngine/Data/Model/Types.h"
#include "WallpaperEngine/Media/MediaSource.h"

#include <set>

namespace WallpaperEngine::Application {

using namespace WallpaperEngine::Assets;
using namespace WallpaperEngine::Data::Model;
class WallpaperApplication {
public:
    explicit WallpaperApplication (ApplicationContext& context);

    void setup ();
    void render ();
    static void cleanup ();
    void show ();
    void signal (int signal);

    /**
     * Entry point for a disposable CEF host child process (settings.general.webHost) instead of
     * embedding CEF in the main engine process. Never touches Wayland/GL/audio. Runs until the
     * main process signals quit through shared memory (or this process is killed).
     */
    void runWebHost ();

    [[nodiscard]] const std::map<std::string, ProjectUniquePtr>& getBackgrounds () const;
    [[nodiscard]] ApplicationContext& getContext () const;
    void update (Render::Drivers::Output::OutputViewport* viewport);
    [[nodiscard]] const WallpaperEngine::Render::Drivers::Output::Output& getOutput () const;

    /** If not called, the default framebuffer will be used */
    void setDestinationFramebuffer (GLuint framebuffer);

    /** Returns 0 (the default framebuffer) if setDestinationFramebuffer() was never called */
    [[nodiscard]] GLuint getDestinationFramebuffer () const;

private:
    AssetLocatorUniquePtr setupAssetLocator (const std::string& bg) const;
    void initializeSubsystems ();
    void loadBackgrounds ();
    [[nodiscard]] ProjectUniquePtr loadBackground (const std::string& bg);
    void setupProperties ();
    void setupPropertiesForProject (const Project& project);

    /** Prints objects/layers for every loaded background, triggered by --list-objects */
    void listObjects () const;
    void listObjectsForProject (const std::string& background, const Project& project) const;

    void setupBrowser ();
    void setupOutput ();
    void setupAudio ();
    void prepareOutputs ();
    void setupOpenGLDebugging ();
    void takeScreenshot (const std::filesystem::path& filename) const;

    struct ActivePlaylist {
	ApplicationContext::PlaylistDefinition definition;
	std::vector<std::size_t> order;
	std::size_t orderIndex = 0;
	std::chrono::steady_clock::time_point nextSwitch;
	std::chrono::steady_clock::time_point lastUpdate;
	std::set<std::size_t> failedIndices;
    };

    void initializePlaylists ();
    void updatePlaylists ();
    void checkHotswapRequest ();
    void advancePlaylist (
	const std::string& screen, ActivePlaylist& playlist, const std::chrono::steady_clock::time_point& now
    );
    bool selectNextCandidate (ActivePlaylist& playlist, std::size_t& outOrderIndex);
    bool preflightWallpaper (const std::string& path);
    std::vector<std::size_t> buildPlaylistOrder (const ApplicationContext::PlaylistDefinition& definition);
    bool makeAnyViewportCurrent () const;

    /** True if this process was re-exec'd by CEF as a subprocess helper (--type=renderer/gpu-process/...) */
    bool isCefSubprocess () const;

    /**
     * Pushes a volume change live to already-running video players and the SDL audio mixer,
     * without touching the loaded projects. volume is 0-128, matching --volume.
     */
    void applyVolumeHotswap (int volume);

    /**
     * Pushes a full-xray toggle live to the renderer, without touching the loaded projects.
     * value is "on"/"off"/"toggle" (also accepts "1"/"0"/"true"/"false" for on/off).
     */
    void applyXrayHotswap (const std::string& value);

    /**
     * Pushes a scaling mode change ("stretch"/"fit"/"fill"/"center"/"default") live to every
     * currently rendered wallpaper, without reloading the loaded projects. Applies to all screens,
     * same as the other hotswap setters below.
     */
    void applyScalingHotswap (const std::string& value);

    /** Pushes a manual zoom factor (e.g. "1.5") live to every currently rendered wallpaper */
    void applyZoomHotswap (const std::string& value);

    /**
     * Pushes a force-disable-parallax toggle live. This is a straight passthrough to
     * settings.mouse.disableparallax, which every parallax-capable object (CImage/CText/CParticle) and
     * CScene's mouse-follow logic already reads directly every frame, so no reload or per-wallpaper
     * plumbing is needed at all. value is "on"/"off"/"toggle" (also "1"/"0"/"true"/"false").
     */
    void applyParallaxHotswap (const std::string& value);

    /**
     * Pushes a corner color change (hex RGB/RGBA, e.g. "000000" or "#1a1a1aff") live to every
     * currently rendered wallpaper. Only visible where clamp mode is border.
     */
    void applyCornerColorHotswap (const std::string& value);

    /**
     * Pushes a playback speed multiplier (e.g. "0.5") live. Straight passthrough to
     * settings.render.playbackSpeed, which render() reads directly every frame to scale the
     * dt fed into g_Time, so no reload or per-wallpaper plumbing is needed at all.
     */
    void applySpeedHotswap (const std::string& value);

    /**
     * Figures out what background a given screen is currently showing, so a layers-only
     * hotswap can reload it without the caller having to resend the path
     */
    [[nodiscard]] std::string resolveScreenBackgroundPath (const std::string& screen) const;
    [[nodiscard]] float resolveScreenZoom (const std::string& screen) const;
    [[nodiscard]] glm::vec4 resolveScreenCornerColor (const std::string& screen) const;
    // The resolution a Web wallpaper on this screen will actually be rendered at - falls back to
    // the combined bounding box of every active screen if this one isn't registered yet.
    [[nodiscard]] glm::ivec2 resolveScreenRenderSize (const std::string& screen) const;

    ApplicationContext& m_context;
    std::map<std::string, ProjectUniquePtr> m_backgrounds {};
    std::map<std::string, ActivePlaylist> m_activePlaylists {};

    std::unique_ptr<WallpaperEngine::Audio::Drivers::Detectors::AudioPlayingDetector> m_audioDetector = nullptr;
    std::unique_ptr<WallpaperEngine::Audio::AudioContext> m_audioContext = nullptr;
    std::unique_ptr<WallpaperEngine::Audio::Drivers::SDLAudioDriver> m_audioDriver = nullptr;
    std::unique_ptr<WallpaperEngine::Audio::Drivers::Recorders::PlaybackRecorder> m_audioRecorder = nullptr;
    std::unique_ptr<WallpaperEngine::Render::RenderContext> m_renderContext = nullptr;
    std::unique_ptr<WallpaperEngine::Render::Drivers::VideoDriver> m_videoDriver = nullptr;
    std::unique_ptr<WallpaperEngine::Render::Drivers::Detectors::FullScreenDetector> m_fullScreenDetector = nullptr;
    std::unique_ptr<WallpaperEngine::WebBrowser::WebBrowserContext> m_browserContext = nullptr;
    std::unique_ptr<WallpaperEngine::Media::MediaSource> m_mediaSource = nullptr;
    std::mt19937 m_playlistRng { std::random_device {}() };
    std::atomic<bool> m_hotswapRequested { false };
    bool m_isPaused = false;
    bool m_screenShotTaken = false;
    uint32_t m_nextFrameScreenshot = 0;
    std::chrono::steady_clock::time_point m_pauseStart {};
    GLuint m_destinationFramebuffer = 0;
};
} // namespace WallpaperEngine::Application
