#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include "ApplicationState.h"
#include "WallpaperEngine/Audio/AudioInput.h"
#include "WallpaperEngine/Data/JSON.h"

#include "../Render/TextureProvider.h"
#include "WallpaperEngine/Render/WallpaperState.h"
#include "WallpaperEngine/VideoPlayback/VideoSegment.h"

#include "WallpaperEngine/Data/Model/Project.h"

namespace WallpaperEngine::Application {
using namespace WallpaperEngine::Data::Assets;
class ApplicationContext {
public:
    ApplicationContext (int argc, char* argv[]);

    void loadSettingsFromArgv ();

    [[nodiscard]] std::optional<bool> resolveObjectVisibility (int id, const std::string& name) const;

    [[nodiscard]] std::optional<bool> resolveEffectVisibility (int id, const std::string& name) const;

    [[nodiscard]] std::optional<float> resolveAudioSensitivity (int id, const std::string& name) const;

    [[nodiscard]] std::optional<float> resolveSoundVolume (int id, const std::string& name) const;

    enum WINDOW_MODE {
	NORMAL_WINDOW = 0,
	DESKTOP_BACKGROUND = 1,
	EXPLICIT_WINDOW = 2,
    };

    enum WAYLAND_LAYER {
	WAYLAND_LAYER_BACKGROUND = 0,
	WAYLAND_LAYER_BOTTOM = 1,
	WAYLAND_LAYER_TOP = 2,
	WAYLAND_LAYER_OVERLAY = 3,
    };

    enum HRTF_MODE {
	HRTF_AUTO = 0,
	HRTF_ON = 1,
	HRTF_OFF = 2,
    };

    [[nodiscard]] static std::optional<HRTF_MODE> parseHrtfMode (const std::string& value);
    /** 0 to 100 (clamped), nullopt for anything that is not a whole number */
    [[nodiscard]] static std::optional<int> parseAudioInputVolume (const std::string& value);
    /** 0 or more, nullopt for anything that is not a finite number */
    [[nodiscard]] static std::optional<float> parseAudioInputThreshold (const std::string& value);

    struct PlaylistSettings {
	uint32_t delayMinutes = 60;
	std::string mode = "timer";
	std::string order = "sequential";
	bool updateOnPause = false;
	bool videoSequence = false;
	bool beginFirst = false;
	bool playIntro = false;
	bool randomTransition = false;
	int transitionMs = 0;

	[[nodiscard]] bool hasIntro () const { return this->mode == "timer" && this->beginFirst && this->playIntro; }
	[[nodiscard]] bool isDaytime () const { return this->mode == "daytime"; }
	[[nodiscard]] bool isDayOfWeek () const { return this->mode == "dayofweek"; }
	[[nodiscard]] bool isLogon () const { return this->mode == "logon"; }
	[[nodiscard]] bool isScheduled () const { return this->isDaytime () || this->isDayOfWeek (); }
    };

    struct PlaylistDefinition {
	std::string name;
	std::vector<std::filesystem::path> items;
	/**
	 * per item, in step with items: the "preset" it names with what config.json stores under that name
	 */
	struct ItemPreset {
	    std::string file;
	    std::string name;
	    std::optional<WallpaperEngine::Data::JSON::JSON> values;
	};
	std::vector<ItemPreset> presets;
	/**
	 * one per item the playlist lists, usable or not: the schedule is laid out over all of them so an item that is
	 * not installed leaves its time to the next one instead of moving everything after it
	 */
	struct ScheduleSlot {
	    float end = 0.0f;
	    bool usable = false;
	};
	std::vector<ScheduleSlot> slots;
	PlaylistSettings settings;

	/** the only way an item gets in, so items and presets stay the same length */
	void addItem (std::filesystem::path path, ItemPreset preset) {
	    this->items.push_back (std::move (path));
	    this->presets.push_back (std::move (preset));
	}

	[[nodiscard]] std::vector<float> scheduleEnds () const;
	[[nodiscard]] const WallpaperEngine::Data::JSON::JSON* presetValues (std::size_t index) const {
	    return index < this->presets.size () && this->presets[index].values.has_value ()
		? &*this->presets[index].values
		: nullptr;
	}
	/** the item chosen when the playlist was selected, so every reader agrees on what was started with */
	std::optional<std::size_t> startIndex;

	[[nodiscard]] std::size_t initialIndex () const;
	/** records the item a "logon" playlist started with, repeating it within a session is harmless */
	void commitStart () const;
	[[nodiscard]] std::size_t startItem () const {
	    return this->startIndex.has_value () ? *this->startIndex : this->initialIndex ();
	}
    };

    struct SpanGroup {
	std::vector<std::string> screens;
	std::filesystem::path background;
	WallpaperEngine::Render::WallpaperState::TextureUVsScaling scaling
	    = WallpaperEngine::Render::WallpaperState::TextureUVsScaling::DefaultUVs;
	TextureFlags clamp = TextureFlags_ClampUVsBorder;
	float zoom = 1.0f;
	glm::vec2 offset = { 0.0f, 0.0f };
	WallpaperEngine::Render::WallpaperState::Alignment alignment = {};
	glm::vec4 cornerColor = { 0.0f, 0.0f, 0.0f, 1.0f };
	WallpaperEngine::Data::Model::ImageAdjustments imageAdjustments = {};
    };

    struct SplitScreen {
	enum Layout {
	    LAYOUT_ASPECT = 0,
	    LAYOUT_EQUAL = 1,
	    LAYOUT_CENTER = 2,
	};

	std::string screen;
	std::vector<std::string> slots;
	Layout layout = LAYOUT_ASPECT;
    };

    [[nodiscard]] const SplitScreen* findSplitScreen (const std::string& screenOrSlot) const;
    [[nodiscard]] bool isScreenRequested (const std::string& name) const;
    [[nodiscard]] std::vector<std::string> describeRequestedScreens () const;

    struct {
	struct {
	    bool onlyListProperties = false;
	    bool onlyListObjects = false;
	    bool onlyListAudioObjects = false;
	    bool onlyListAudioInputs = false;
	    bool onlyListEffects = false;
	    bool onlyListFonts = false;
	    bool dumpStructure = false;
	    bool noShaderCache = false;
	    bool disableParticles = false;
	    bool expandCanvas = false;
	    bool repairAssets = false;
	    bool postProcessingDisabled = false;
	    bool ultraPostProcessing = false;
	    bool displayHDR = false;
	    int volumetricsQuality = 2;
	    int shadowQuality = 2;
	    int msaaSamples = 0;
	    int supersampling = 1;
	    std::vector<std::string> disabledObjects;
	    std::vector<std::string> enabledObjects;
	    std::vector<std::string> disabledEffects;
	    std::vector<std::string> enabledEffects;
	    std::map<std::string, float> audioSensitivity;
	    std::map<std::string, float> soundVolume;
	    std::filesystem::path assets;
	    std::filesystem::path defaultBackground;
	    std::map<std::string, std::filesystem::path> screenBackgrounds;
	    std::map<std::string, std::string> properties;
	    std::map<std::string, WallpaperEngine::Render::WallpaperState::TextureUVsScaling> screenScalings;
	    std::map<std::string, TextureFlags> screenClamps;
	    std::map<std::string, float> screenZooms;
	    std::map<std::string, float> screenRenderScales;
	    std::map<std::string, glm::vec2> screenOffsets;
	    std::map<std::string, WallpaperEngine::Render::WallpaperState::Alignment> screenAlignments;
	    std::map<std::string, glm::vec4> screenCornerColors;
	    std::map<std::string, WallpaperEngine::Data::Model::ImageAdjustments> screenImageAdjustments;
	    std::map<std::string, WallpaperEngine::VideoPlayback::VideoSegments> screenVideoSegments;
	    std::map<std::string, PlaylistDefinition> screenPlaylists;
	    std::optional<PlaylistDefinition> defaultPlaylist;
	    std::vector<SpanGroup> spanGroups;
	    std::vector<SplitScreen> splitScreens;
	    bool webHost = false;
	    std::string webHostShm;
	    std::string webHostFrames;
	    int webHostSocket = -1;
	    uint32_t webHostWidth = 0;
	    uint32_t webHostHeight = 0;
	    std::optional<std::filesystem::path> controlFile;
	} general;

	struct {
	    WINDOW_MODE mode = {};
	    int maximumFPS = 0;
	    float playbackSpeed = 0.0f;
	    WallpaperEngine::VideoPlayback::VideoSegments videoSegments;
	    std::string hwdec;
	    std::string webFrames;
	    std::string frameLimiter;
	    std::string transition;
	    int transitionMs = 0;
	    /** --transition / --transition-duration or their control file keys were given, a playlist's "random" yields
	     */
	    bool transitionChosen = false;
	    bool transitionMsChosen = false;
	    bool freezeAnimations = false;
	    bool pauseOnFullscreen = false;
	    // only fullscreen toplevels that are also activated count (scrollable tiling compositors)
	    bool pauseOnFullscreenOnlyWhenActive = false;
	    bool pauseOnFocus = false;
	    /** wlroots: maximized toplevels pause their outputs too (KDE and GNOME always do, like WE) */
	    bool pauseOnMaximized = false;
	    std::vector<std::string> fullscreenPauseIgnoreAppIds;
	    std::optional<int> batteryFPS;
	    int idlePauseMinutes = 0;
	    bool pauseOnLock = false;
	    bool hdr = false;
	    struct {
		bool baseOnly = false;
		bool noSolidFinal = false;
		bool passLog = false;
		bool brightnessLog = false;
		bool noPuppetAnimation = false;
		std::optional<int> objectFilter;
		std::vector<int> skipObjects;
		std::vector<int> skipEffects;
	    } debug;

	    struct {
		glm::ivec4 geometry = {};
		TextureFlags clamp = {};
		WallpaperEngine::Render::WallpaperState::TextureUVsScaling scalingMode = {};
		float zoom = 0.0f;
		glm::vec2 offset = {};
		WallpaperEngine::Render::WallpaperState::Alignment alignment = {};
		glm::vec4 cornerColor = {};
		WallpaperEngine::Data::Model::ImageAdjustments imageAdjustments = {};
		bool asBackground = false;
		bool regular = false;
	    } window;

	    struct {
		WAYLAND_LAYER layer = {};
		bool layerAuto = false;
	    } wayland;
	} render;

	struct {
	    bool enabled;
	    /** 0-128 */
	    int volume;
	    bool automute;
	    bool audioprocessing;
	    std::optional<std::string> audioScreen;
	    std::optional<int> ambientVolume;
	    HRTF_MODE hrtf;
	    /** WE's audioinputdevice, audioinputvolume and audioinputthreshold */
	    WallpaperEngine::Audio::AudioInput input;
	} audio;

	struct {
	    bool enabled;
	    std::string host;
	    uint16_t port;
	    bool boost;
	    /** WE's brightnessfactor, 1 = as is */
	    float brightness;
	    bool reduceFps;
	} leds;

	struct {
	    bool enabled;
	    bool disableparallax;
	    bool clampParallaxToImageSize;
	    std::filesystem::path inputFile;
	} mouse;

	struct {
	    bool take;
	    /** In frames, not seconds */
	    uint32_t delay;
	    std::filesystem::path path;
	} screenshot;
    } settings = {
        .general = {
            .onlyListProperties = false,
            .onlyListObjects = false,
            .onlyListAudioObjects = false,
            .onlyListAudioInputs = false,
            .onlyListEffects = false,
            .onlyListFonts = false,
            .dumpStructure = false,
            .noShaderCache = false,
            .disabledObjects = {},
            .enabledObjects = {},
            .disabledEffects = {},
            .enabledEffects = {},
            .audioSensitivity = {},
            .soundVolume = {},
            .assets = "",
            .defaultBackground = "",
            .screenBackgrounds = {},
            .properties = {},
            .screenScalings = {},
            .screenClamps = {},
            .screenZooms = {},
            .screenRenderScales = {},
            .screenOffsets = {},
            .screenAlignments = {},
            .screenCornerColors = {},
            .screenImageAdjustments = {},
            .screenVideoSegments = {},
            .screenPlaylists = {},
            .defaultPlaylist = std::nullopt,
            .spanGroups = {},
            .splitScreens = {},
            .webHost = false,
            .webHostShm = "",
            .webHostFrames = "shm",
            .webHostSocket = -1,
            .webHostWidth = 0,
            .webHostHeight = 0,
            .controlFile = std::nullopt,
        },
        .render = {
            .mode = NORMAL_WINDOW,
            .maximumFPS = 60,
            .playbackSpeed = 1.0f,
            .videoSegments = {},
            .hwdec = "auto",
            .webFrames = "auto",
            .frameLimiter = "budget",
            .transition = "none",
            .transitionMs = 600,
            .freezeAnimations = false,
            .pauseOnFullscreen = true,
            .pauseOnFullscreenOnlyWhenActive = false,
            .pauseOnFocus = false,
            .pauseOnMaximized = false,
            .fullscreenPauseIgnoreAppIds = {},
            .batteryFPS = std::nullopt,
            .idlePauseMinutes = 0,
            .pauseOnLock = true,
            .hdr = false,
            .debug = {
                .baseOnly = false,
                .noSolidFinal = false,
	                .passLog = false,
	                .brightnessLog = false,
	                .noPuppetAnimation = false,
	                .objectFilter = std::nullopt,
	                .skipObjects = {},
	                .skipEffects = {},
	            },
            .window = {
                .geometry = {},
                .clamp = TextureFlags_ClampUVsBorder,
                .scalingMode = WallpaperEngine::Render::WallpaperState::TextureUVsScaling::DefaultUVs,
                .zoom = 1.0f,
                .offset = { 0.0f, 0.0f },
                .alignment = {},
                .cornerColor = { 0.0f, 0.0f, 0.0f, 1.0f },
                .imageAdjustments = {},
                .asBackground = false,
                .regular = false,
            },
            .wayland = {
                .layer = WAYLAND_LAYER_BOTTOM,
                .layerAuto = true,
            },
        },
        .audio = {
            .enabled = true,
            .volume = 15,
            .automute = false,
            .audioprocessing = true,
            .audioScreen = std::nullopt,
            .ambientVolume = std::nullopt,
            .hrtf = HRTF_AUTO,
            .input = {},
        },
        .leds = {
            .enabled = false,
            .host = "127.0.0.1",
            .port = 6742,
            .boost = true,
            .brightness = 1.0f,
            .reduceFps = false,
        },
        .mouse = {
            .enabled = true,
            .disableparallax = false,
            .clampParallaxToImageSize = true,
        },
        .screenshot = {
            .take = false,
            .delay = 5,
            .path = "",
        },
    };

    ApplicationState state;

    [[nodiscard]] int getArgc () const;
    [[nodiscard]] char** getArgv () const;

private:
    int m_argc;
    char** m_argv;

    void validateAssets ();
    void validateScreenshot () const;

    static std::filesystem::path translateBackground (const std::string& bgIdOrPath);

    void loadPlaylistsFromConfig ();
    [[nodiscard]] static std::filesystem::path resolvePlaylistItemPath (const std::string& raw);
    [[nodiscard]] static std::filesystem::path configFilePath ();
    [[nodiscard]] static std::optional<WallpaperEngine::Data::JSON::JSON>
    parseConfigJson (const std::filesystem::path& path);
    [[nodiscard]] static PlaylistSettings parsePlaylistSettings (const WallpaperEngine::Data::JSON::JSON& playlistJson);
    void collectPlaylistItems (
	const WallpaperEngine::Data::JSON::JSON& playlistJson, const std::string& name, PlaylistDefinition& definition
    ) const;
    [[nodiscard]] std::optional<PlaylistDefinition> buildPlaylistDefinition (
	const WallpaperEngine::Data::JSON::JSON& playlistJson, const std::string& fallbackName
    ) const;
    void registerPlaylist (PlaylistDefinition&& definition);
    [[nodiscard]] const PlaylistDefinition& getPlaylistFromConfig (const std::string& name);

    std::map<std::string, PlaylistDefinition> m_configPlaylists;
    bool m_loadedConfigPlaylists = false;
};
}
