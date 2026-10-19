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

    struct PlaylistSettings {
	uint32_t delayMinutes = 60;
	std::string mode = "timer";
	std::string order = "sequential";
	bool updateOnPause = false;
	bool videoSequence = false;
    };

    struct PlaylistDefinition {
	std::string name;
	std::vector<std::filesystem::path> items;
	PlaylistSettings settings;
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
	    bool onlyListProperties;
	    bool onlyListObjects;
	    bool onlyListAudioObjects;
	    bool onlyListEffects;
	    bool dumpStructure;
	    bool noShaderCache;
	    bool disableParticles;
	    bool expandCanvas;
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
	    std::map<std::string, glm::vec2> screenOffsets;
	    std::map<std::string, WallpaperEngine::Render::WallpaperState::Alignment> screenAlignments;
	    std::map<std::string, glm::vec4> screenCornerColors;
	    std::map<std::string, WallpaperEngine::Data::Model::ImageAdjustments> screenImageAdjustments;
	    std::map<std::string, WallpaperEngine::VideoPlayback::VideoSegments> screenVideoSegments;
	    std::map<std::string, PlaylistDefinition> screenPlaylists;
	    std::optional<PlaylistDefinition> defaultPlaylist;
	    std::vector<SpanGroup> spanGroups;
	    std::vector<SplitScreen> splitScreens;
	    bool webHost;
	    std::string webHostShm;
	    uint32_t webHostWidth;
	    uint32_t webHostHeight;
	    std::optional<std::filesystem::path> controlFile;
	} general;

	struct {
	    WINDOW_MODE mode;
	    int maximumFPS;
	    float playbackSpeed;
	    WallpaperEngine::VideoPlayback::VideoSegments videoSegments;
	    std::string hwdec;
	    bool freezeAnimations;
	    bool pauseOnFullscreen;
	    // only fullscreen toplevels that are also activated count (scrollable tiling compositors)
	    bool pauseOnFullscreenOnlyWhenActive;
	    bool pauseOnFocus;
	    /** wlroots: maximized toplevels pause their outputs too (KDE and GNOME always do, like WE) */
	    bool pauseOnMaximized;
	    std::vector<std::string> fullscreenPauseIgnoreAppIds;
	    std::optional<int> batteryFPS;
	    int idlePauseMinutes;
	    bool hdr;
	    struct {
		bool baseOnly;
		bool noSolidFinal;
		bool passLog;
		bool brightnessLog;
		bool noPuppetAnimation;
		std::optional<int> objectFilter;
		std::vector<int> skipObjects;
		std::vector<int> skipEffects;
	    } debug;

	    struct {
		glm::ivec4 geometry;
		TextureFlags clamp;
		WallpaperEngine::Render::WallpaperState::TextureUVsScaling scalingMode;
		float zoom;
		glm::vec2 offset;
		WallpaperEngine::Render::WallpaperState::Alignment alignment;
		glm::vec4 cornerColor;
		WallpaperEngine::Data::Model::ImageAdjustments imageAdjustments;
		bool asBackground;
		bool regular;
	    } window;

	    struct {
		WAYLAND_LAYER layer;
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
	} audio;

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
            .onlyListEffects = false,
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
            .freezeAnimations = false,
            .pauseOnFullscreen = true,
            .pauseOnFullscreenOnlyWhenActive = false,
            .pauseOnFocus = false,
            .pauseOnMaximized = false,
            .fullscreenPauseIgnoreAppIds = {},
            .batteryFPS = std::nullopt,
            .idlePauseMinutes = 0,
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
    std::filesystem::path resolvePlaylistItemPath (const std::string& raw) const;
    std::filesystem::path configFilePath () const;
    std::optional<WallpaperEngine::Data::JSON::JSON> parseConfigJson (const std::filesystem::path& path) const;
    PlaylistSettings parsePlaylistSettings (const WallpaperEngine::Data::JSON::JSON& playlistJson) const;
    std::vector<std::filesystem::path>
    collectPlaylistItems (const WallpaperEngine::Data::JSON::JSON& playlistJson, const std::string& name) const;
    std::optional<PlaylistDefinition> buildPlaylistDefinition (
	const WallpaperEngine::Data::JSON::JSON& playlistJson, const std::string& fallbackName
    ) const;
    void registerPlaylist (PlaylistDefinition&& definition);
    [[nodiscard]] const PlaylistDefinition& getPlaylistFromConfig (const std::string& name);

    std::map<std::string, PlaylistDefinition> m_configPlaylists;
    bool m_loadedConfigPlaylists = false;
};
}
