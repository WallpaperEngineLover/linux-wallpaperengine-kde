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

#include "WallpaperEngine/Data/JSON.h"
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
    /** A project.json ready to be parsed. For presets, json is the base wallpaper's and preset holds the overrides */
    struct ProjectSource {
	AssetLocatorUniquePtr container;
	WallpaperEngine::Data::JSON::JSON json;
	std::optional<WallpaperEngine::Data::JSON::JSON> preset;
    };

    /** overlay is an extra folder mounted after bg, used for preset-owned files (e.g. splat data) */
    AssetLocatorUniquePtr setupAssetLocator (const std::string& bg, const std::filesystem::path& overlay = {}) const;
    ProjectSource openProjectSource (const std::string& path) const;
    static void applyPreset (
	const Project& project, const WallpaperEngine::Data::JSON::JSON& preset, const std::filesystem::path& presetDir
    );
    void initializeSubsystems ();
    void loadBackgrounds ();
    [[nodiscard]] ProjectUniquePtr loadBackground (const std::string& bg);
    void setupProperties ();
    void setupPropertiesForProject (const Project& project);

    /** Prints objects/layers for every loaded background, triggered by --list-objects */
    void listObjects () const;
    void listObjectsForProject (const std::string& background, const Project& project) const;

    /** Prints per-object effects (bloom, blur, glow, etc) for every loaded background, triggered by --list-effects */
    void listEffects () const;
    void listEffectsForProject (const std::string& background, const Project& project) const;

    /** Applies --audio-sensitivity overrides for every loaded background */
    void setupAudioSensitivity ();
    void setupAudioSensitivityForProject (const Project& project) const;

    /** Prints audio-reactive objects/properties for every loaded background, triggered by --list-audio-objects */
    void listAudioObjects () const;
    void listAudioObjectsForProject (const std::string& background, const Project& project) const;

    /** Applies --sound-volume overrides for every loaded background */
    void setupSoundVolume ();
    void setupSoundVolumeForProject (const Project& project) const;

    void setupBrowser ();
    void setupOutput ();
    void setupAudio ();
    /** Starts audio capture for a wallpaper loaded after startup, setupAudio() only saw the first ones */
    void ensureAudioCapture (const Project& project);
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
    void applyFpsHotswap (int fps);

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

    /** Pushes an offset re-center (e.g. "0.5,-1") live to every currently rendered wallpaper */
    void applyOffsetHotswap (const std::string& value);

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
     * Pushes a new --audio-screen restriction live (empty value clears it back to "no
     * restriction"), then re-applies audio policy to every currently rendered wallpaper.
     */
    void applyAudioScreenHotswap (const std::string& value);

    /** Pushes a new --ambient-volume (0-128) live, then re-applies audio policy to every currently rendered wallpaper */
    void applyAmbientVolumeHotswap (const std::string& value);

    /**
     * Pushes --sound-volume overrides (id-or-name -> 0-1 volume) live to every Sound object in the
     * currently loaded projects, without reloading - unlike --set-property/--audio-sensitivity,
     * Sound objects already re-read their own volume every frame (see CSound::render()), so this
     * is a pure live setter.
     */
    void applySoundVolumeHotswap (const std::map<std::string, std::string>& targets);

    /**
     * Recomputes and pushes CWallpaper::setAudioPolicy() to every currently rendered wallpaper
     * based on settings.audio.audioScreen/ambientVolume. Span-group screens share a single
     * CWallpaper instance across several screen names, so this groups by instance first and
     * only mutes one if none of its screens match audioScreen.
     */
    void applyAudioPolicy ();

    /**
     * Figures out what background a given screen is currently showing, so a layers-only
     * hotswap can reload it without the caller having to resend the path
     */
    [[nodiscard]] std::string resolveScreenBackgroundPath (const std::string& screen) const;
    [[nodiscard]] float resolveScreenZoom (const std::string& screen) const;
    [[nodiscard]] glm::vec2 resolveScreenOffset (const std::string& screen) const;
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
    // the recorder replaced by ensureAudioCapture(), the outgoing wallpaper's passes still point into it
    std::unique_ptr<WallpaperEngine::Audio::Drivers::Recorders::PlaybackRecorder> m_previousAudioRecorder = nullptr;
    bool m_audioCapturing = false;
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
