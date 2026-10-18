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
#include "WallpaperEngine/Desktop/PowerMonitor.h"
#include "WallpaperEngine/Media/MediaSource.h"

#include <set>
#include <thread>

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

    // entry point of a disposable --web-host CEF process, no Wayland/GL/audio; runs until the main process signals quit
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
    /** screen= background key, nullopt for every screen */
    using HotswapTarget = std::optional<std::string>;

    /** A project.json ready to be parsed. For presets, json is the base wallpaper's and preset holds the overrides */
    struct ProjectSource {
	AssetLocatorUniquePtr container;
	WallpaperEngine::Data::JSON::JSON json;
	std::optional<WallpaperEngine::Data::JSON::JSON> preset;
	/** Wallpaper folder, the base wallpaper's for a preset */
	std::filesystem::path directory;
    };

    /** overlay is an extra folder mounted after bg, used for preset-owned files (e.g. splat data) */
    AssetLocatorUniquePtr setupAssetLocator (const std::string& bg, const std::filesystem::path& overlay = {}) const;
    ProjectSource openProjectSource (const std::string& path) const;
    static void applyPreset (
	Project& project, const WallpaperEngine::Data::JSON::JSON& preset, const std::filesystem::path& presetDir
    );
    void initializeSubsystems ();
    void loadBackgrounds ();
    [[nodiscard]] ProjectUniquePtr loadBackground (const std::string& bg);
    void setupProperties ();
    void setupPropertiesForProject (const Project& project);

    // --list-objects
    void listObjects () const;
    void listObjectsForProject (const std::string& background, const Project& project) const;

    // --list-effects
    void listEffects () const;
    void listEffectsForProject (const std::string& background, const Project& project) const;

    // --audio-sensitivity
    void setupAudioSensitivity ();
    void setupAudioSensitivityForProject (const Project& project) const;

    // --list-audio-objects
    void listAudioObjects () const;
    void listAudioObjectsForProject (const std::string& background, const Project& project) const;

    // --sound-volume
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

    // pushes a volume (0-128, like --volume) live to video players and the SDL mixer
    void applyVolumeHotswap (int volume);
    void applyFpsHotswap (int fps);
    void applyBatteryFpsHotswap (const std::string& value);
    void applyIdlePauseHotswap (const std::string& value);
    void updatePowerMonitor ();
    [[nodiscard]] int frameRateLimit () const;
    [[nodiscard]] uint32_t pauseReasons () const;
    void logPauseReasons (uint32_t reasons);

    /** "full"/"normal"/"disabled", or the older "on"/"off"/"toggle" */
    void applyXrayHotswap (const std::string& value);

    // "stretch"/"fit"/"fill"/"center"/"default"
    void applyScalingHotswap (const HotswapTarget& target, const std::string& value);

    // e.g. "1.5"
    void applyZoomHotswap (const HotswapTarget& target, const std::string& value);

    // e.g. "0.5,-1"
    void applyOffsetHotswap (const HotswapTarget& target, const std::string& value);
    // "position,x,y,zoom" in WE's 0-100/0-200 units
    void applyAlignmentHotswap (const HotswapTarget& target, const std::string& value);

    // passthrough to settings.mouse.disableparallax, read every frame; "on"/"off"/"toggle" (or "1"/"0"/"true"/"false")
    void applyParallaxHotswap (const std::string& value);

    // hex RGB/RGBA, e.g. "000000" or "#1a1a1aff"; only visible where clamp is border
    void applyCornerColorHotswap (const HotswapTarget& target, const std::string& value);
    void applyImageAdjustmentsHotswap (const HotswapTarget& target, const ImageAdjustments& request);
    /** Without a target: the window, every screen and every span group. update maps old -> new */
    template <typename T, typename Update>
    void updateScreenSetting (
	const HotswapTarget& target, std::map<std::string, T>& screens, T& window,
	T ApplicationContext::SpanGroup::* spanField, Update update
    );
    /** once per wallpaper, a span's is shared by its screens */
    template <typename Apply> void forEachTargetWallpaper (const HotswapTarget& target, Apply apply) const;

    // e.g. "0.5", passthrough to settings.render.playbackSpeed which render() reads every frame
    void applySpeedHotswap (const std::string& value);

    /** Video wallpapers restart from the first part, video-start/video-end make it one part */
    void applyVideoSegmentsHotswap (
	const HotswapTarget& target, const std::optional<std::string>& start, const std::optional<std::string>& end,
	const std::optional<std::string>& segments
    );
    [[nodiscard]] WallpaperEngine::VideoPlayback::VideoSegments
    resolveScreenVideoSegments (const std::string& screen) const;

    /** Jumps video wallpapers to a position (seconds, m:ss or h:mm:ss), e.g. a few seconds before the loop end */
    void applyVideoSeekHotswap (const HotswapTarget& target, const std::string& value);
    void applyHwdecHotswap (const std::string& value);

    // empty clears the restriction, then re-applies the audio policy
    void applyAudioScreenHotswap (const std::string& value);

    // 0-128, then re-applies the audio policy
    void applyAmbientVolumeHotswap (const std::string& value);

    // id-or-name -> 0-1 volume, live since Sound objects re-read their volume every frame
    void applySoundVolumeHotswap (const std::map<std::string, std::string>& targets);
    /** false when one of them needs a reload */
    bool applyPropertiesLive (const std::vector<std::string>& names);

    // setAudioPolicy() from audioScreen/ambientVolume; a span group's shared wallpaper is muted only if none of its
    // screens match
    void applyAudioPolicy ();

    // the background a screen shows, so a layers-only hotswap can reload it without a path
    [[nodiscard]] std::string resolveScreenBackgroundPath (const std::string& screen) const;
    /** Screen suffix for script localStorage's screen store */
    [[nodiscard]] std::string storageScreen (const std::string& screen) const;
    [[nodiscard]] float resolveScreenZoom (const std::string& screen) const;
    [[nodiscard]] glm::vec2 resolveScreenOffset (const std::string& screen) const;
    [[nodiscard]] WallpaperEngine::Render::WallpaperState::Alignment
    resolveScreenAlignment (const std::string& screen) const;
    [[nodiscard]] glm::vec4 resolveScreenCornerColor (const std::string& screen) const;
    /** Command line and control file settings for this screen (or span group) over what the preset carries */
    [[nodiscard]] ImageAdjustments
    resolveScreenImageAdjustments (const std::string& screen, const Project& project) const;
    // a Web wallpaper's render size on this screen, the bounding box of all screens if it isn't registered yet
    [[nodiscard]] glm::ivec2 resolveScreenRenderSize (const std::string& screen) const;
    /** The span group loaded as this "span:<first screen>" key, null for a plain screen */
    [[nodiscard]] const ApplicationContext::SpanGroup* findSpanGroup (const std::string& screen) const;
    [[nodiscard]] ApplicationContext::SpanGroup* findSpanGroup (const std::string& screen);
    // builds the wallpaper for a screen or span group and puts it on its outputs; throws on failure, the outputs keep
    // what they had
    void installWallpaper (
	const std::string& screen, const Project& project, const std::filesystem::path& path, bool holdPrevious
    );
    /** Screen, slot or span key as is, a span member as its span key; nullopt when unknown */
    [[nodiscard]] std::optional<std::string> resolveHotswapScreen (const std::string& name) const;
    [[nodiscard]] bool hotswapReaches (const HotswapTarget& target, const std::string& screen) const;

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
    enum PauseReason : uint32_t {
	PauseWindows = 1,
	PauseBattery = 2,
	PauseIdle = 4,
    };
    uint32_t m_pauseReasons = 0;
    // a hotswap during a battery/idle pause draws one frame
    bool m_showFrameWhilePaused = false;
    std::unique_ptr<WallpaperEngine::Desktop::PowerMonitor> m_powerMonitor = nullptr;
    bool m_screenShotTaken = false;
    // encodes and writes the last screenshot, joined before the next one and when the application goes
    mutable std::jthread m_screenshotWriter;

#if DEMOMODE
    /** What the demo recording keeps between frames (setup () sizes it, render () feeds the encoder) */
    struct {
	int width = 0;
	int height = 0;
	std::vector<uint8_t> pixels;
	bool initialized = false;
	int frame = 0;
    } m_demo;
#endif /* DEMOMODE */
    uint32_t m_nextFrameScreenshot = 0;
    std::chrono::steady_clock::time_point m_pauseStart {};
    GLuint m_destinationFramebuffer = 0;
};
} // namespace WallpaperEngine::Application
