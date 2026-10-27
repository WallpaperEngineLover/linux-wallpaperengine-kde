#pragma once

#include <atomic>
#include <chrono>
#include <random>

#include "WallpaperEngine/Application/ApplicationContext.h"
#include "WallpaperEngine/Application/FramePacer.h"
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
#include "WallpaperEngine/Desktop/SessionMonitor.h"
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

    void runWebHost ();

    [[nodiscard]] const std::map<std::string, ProjectUniquePtr>& getBackgrounds () const;
    [[nodiscard]] ApplicationContext& getContext () const;
    void update (Render::Drivers::Output::OutputViewport* viewport);
    [[nodiscard]] const WallpaperEngine::Render::Drivers::Output::Output& getOutput () const;

    void setDestinationFramebuffer (GLuint framebuffer);

    [[nodiscard]] GLuint getDestinationFramebuffer () const;

private:
    using HotswapTarget = std::optional<std::string>;

    struct ProjectSource {
	AssetLocatorUniquePtr container;
	WallpaperEngine::Data::JSON::JSON json;
	std::optional<WallpaperEngine::Data::JSON::JSON> preset;
	std::filesystem::path directory;
    };

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

    void listObjects () const;
    void listObjectsForProject (const std::string& background, const Project& project) const;

    void listEffects () const;
    void listFonts () const;
    void listFontsForProject (const std::string& background, const Project& project) const;
    void listEffectsForProject (const std::string& background, const Project& project) const;

    void setupAudioSensitivity ();
    void setupAudioSensitivityForProject (const Project& project) const;

    void listAudioObjects () const;
    void listAudioObjectsForProject (const std::string& background, const Project& project) const;

    void setupSoundVolume ();
    void setupSoundVolumeForProject (const Project& project) const;

    void setupBrowser ();
    void setupOutput ();
    void setupAudio ();
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
	bool introPlaying = false;
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

    void applyVolumeHotswap (int volume);
    void applyFpsHotswap (int fps);
    void applyBatteryFpsHotswap (const std::string& value);
    void applyIdlePauseHotswap (const std::string& value);
    void applyLockPauseHotswap (const std::string& value);
    void updateSessionMonitor ();
    void updatePowerMonitor ();
    [[nodiscard]] int frameRateLimit () const;
    [[nodiscard]] uint32_t pauseReasons () const;
    void logPauseReasons (uint32_t reasons);

    void applyXrayHotswap (const std::string& value);

    void applyScalingHotswap (const HotswapTarget& target, const std::string& value);

    void applyZoomHotswap (const HotswapTarget& target, const std::string& value);

    void applyOffsetHotswap (const HotswapTarget& target, const std::string& value);
    void applyAlignmentHotswap (const HotswapTarget& target, const std::string& value);

    void applyParallaxHotswap (const std::string& value);

    void applyCornerColorHotswap (const HotswapTarget& target, const std::string& value);
    void applyImageAdjustmentsHotswap (const HotswapTarget& target, const ImageAdjustments& request);
    /** Without a target: the window, every screen and every span group. update maps old -> new */
    template <typename T, typename Update>
    void updateScreenSetting (
	const HotswapTarget& target, std::map<std::string, T>& screens, T& window,
	T ApplicationContext::SpanGroup::* spanField, Update update
    );
    template <typename Apply> void forEachTargetWallpaper (const HotswapTarget& target, Apply apply) const;

    void applySpeedHotswap (const std::string& value);

    void applyVideoSegmentsHotswap (
	const HotswapTarget& target, const std::optional<std::string>& start, const std::optional<std::string>& end,
	const std::optional<std::string>& segments
    );
    [[nodiscard]] WallpaperEngine::VideoPlayback::VideoSegments
    resolveScreenVideoSegments (const std::string& screen) const;

    void applyVideoSeekHotswap (const HotswapTarget& target, const std::string& value);
    void applyHwdecHotswap (const std::string& value);

    void applyAudioScreenHotswap (const std::string& value);

    void applyAmbientVolumeHotswap (const std::string& value);

    void applySoundVolumeHotswap (const std::map<std::string, std::string>& targets);
    /** false when one of them needs a reload */
    bool applyPropertiesLive (const std::vector<std::string>& names);

    void applyAudioPolicy ();

    [[nodiscard]] std::string resolveScreenBackgroundPath (const std::string& screen) const;
    [[nodiscard]] std::string storageScreen (const std::string& screen) const;
    [[nodiscard]] float resolveScreenZoom (const std::string& screen) const;
    [[nodiscard]] glm::vec2 resolveScreenOffset (const std::string& screen) const;
    [[nodiscard]] WallpaperEngine::Render::WallpaperState::Alignment
    resolveScreenAlignment (const std::string& screen) const;
    [[nodiscard]] glm::vec4 resolveScreenCornerColor (const std::string& screen) const;
    [[nodiscard]] ImageAdjustments
    resolveScreenImageAdjustments (const std::string& screen, const Project& project) const;
    [[nodiscard]] glm::ivec2 resolveScreenRenderSize (const std::string& screen) const;
    [[nodiscard]] const ApplicationContext::SpanGroup* findSpanGroup (const std::string& screen) const;
    [[nodiscard]] ApplicationContext::SpanGroup* findSpanGroup (const std::string& screen);
    void installWallpaper (
	const std::string& screen, const Project& project, const std::filesystem::path& path, bool holdPrevious,
	bool animate = false
    );
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
    bool m_inputSuspended = false;
    enum PauseReason : uint32_t {
	PauseWindows = 1,
	PauseBattery = 2,
	PauseIdle = 4,
	PauseLocked = 8,
	PauseDisplayOff = 16,
    };
    uint32_t m_pauseReasons = 0;
    bool m_showFrameWhilePaused = false;
    std::unique_ptr<WallpaperEngine::Desktop::PowerMonitor> m_powerMonitor = nullptr;
    std::unique_ptr<WallpaperEngine::Desktop::SessionMonitor> m_sessionMonitor = nullptr;
    bool m_screenShotTaken = false;
    mutable std::jthread m_screenshotWriter;

#if DEMOMODE
    struct {
	int width = 0;
	int height = 0;
	std::vector<uint8_t> pixels;
	bool initialized = false;
	int frame = 0;
    } m_demo;
#endif
    uint32_t m_nextFrameScreenshot = 0;
    std::chrono::steady_clock::time_point m_pauseStart {};
    FramePacer m_framePacer;
    std::chrono::steady_clock::time_point m_frameEnd = std::chrono::steady_clock::now ();
    GLuint m_destinationFramebuffer = 0;
};
}
