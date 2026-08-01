#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <glm/vec4.hpp>

#include "ApplicationState.h"
#include "WallpaperEngine/Data/JSON.h"

#include "../Render/TextureProvider.h"
#include "WallpaperEngine/Render/WallpaperState.h"

#include "WallpaperEngine/Data/Model/Project.h"

namespace WallpaperEngine::Application {
using namespace WallpaperEngine::Data::Assets;
class ApplicationContext {
public:
    ApplicationContext (int argc, char* argv[]);

    void loadSettingsFromArgv ();

    /**
     * Resolves whether an object/layer should be force-shown or force-hidden based on the
     * --disable-object/--enable-object overrides, matching id or name against either.
     *
     * @return true to force visible, false to force hidden, nullopt to leave the scene's own value alone
     */
    [[nodiscard]] std::optional<bool> resolveObjectVisibility (int id, const std::string& name) const;

    enum WINDOW_MODE {
	NORMAL_WINDOW = 0,
	/** Draw to the window server desktop */
	DESKTOP_BACKGROUND = 1,
	EXPLICIT_WINDOW = 2,
    };

    /**
     * Wayland-only: which wlr-layer-shell layer to anchor the wallpaper surface to.
     * Different compositors treat layers differently; e.g. niri's
     * `place-within-backdrop` layer-rule only applies to BACKGROUND surfaces.
     */
    enum WAYLAND_LAYER {
	WAYLAND_LAYER_BACKGROUND = 0,
	WAYLAND_LAYER_BOTTOM = 1,
	WAYLAND_LAYER_TOP = 2,
	WAYLAND_LAYER_OVERLAY = 3,
    };

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
	/** Manual zoom factor layered on top of the scaling mode, see --zoom */
	float zoom = 1.0f;
	/** Color shown outside the wallpaper's bounds when clamp is border, see --corner-color */
	glm::vec4 cornerColor = { 0.0f, 0.0f, 0.0f, 1.0f };
    };

    struct {
	// General settings
	struct {
	    bool onlyListProperties;
	    bool onlyListObjects;
	    bool dumpStructure;
	    bool disableParticles;
	    /** Objects/layers to force-hide, matched by id or name */
	    std::vector<std::string> disabledObjects;
	    /** Objects/layers to force-show, matched by id or name */
	    std::vector<std::string> enabledObjects;
	    std::filesystem::path assets;
	    /** Background to load (provided as the final argument) as fallback for multi-screen setups */
	    std::filesystem::path defaultBackground;
	    std::map<std::string, std::filesystem::path> screenBackgrounds;
	    std::map<std::string, std::string> properties;
	    std::map<std::string, WallpaperEngine::Render::WallpaperState::TextureUVsScaling> screenScalings;
	    std::map<std::string, TextureFlags> screenClamps;
	    /** Manual zoom factor for different screens, layered on top of the scaling mode */
	    std::map<std::string, float> screenZooms;
	    /** Corner color for different screens, shown outside the wallpaper's bounds when clamp is border */
	    std::map<std::string, glm::vec4> screenCornerColors;
	    std::map<std::string, PlaylistDefinition> screenPlaylists;
	    /** Playlist used in window mode */
	    std::optional<PlaylistDefinition> defaultPlaylist;
	    /** Span groups: multiple monitors sharing one stretched wallpaper */
	    std::vector<SpanGroup> spanGroups;
	    /**
	     * Internal, not advertised in --help: marks this process as a disposable CEF host for a
	     * single Web wallpaper instead of embedding CEF in the main engine process. Requires
	     * webHostShm/Width/Height.
	     */
	    bool webHost;
	    std::string webHostShm;
	    uint32_t webHostWidth;
	    uint32_t webHostHeight;
	} general;

	// Render settings
	struct {
	    WINDOW_MODE mode;
	    int maximumFPS;
	    /** Global playback speed multiplier for animations, particles and effects, see --speed */
	    float playbackSpeed;
	    bool pauseOnFullscreen;
	    /**
	     * Wayland-only: if true, only consider fullscreen toplevels that are also activated.
	     * Useful for compositors with "virtual" fullscreen windows (e.g. scrollable tiling).
	     */
	    bool pauseOnFullscreenOnlyWhenActive;
	    /**
	     * Wayland-only: list of app_id substrings to ignore for fullscreen pause.
	     * Example: "firefox" will match "org.mozilla.firefox".
	     */
	    std::vector<std::string> fullscreenPauseIgnoreAppIds;
	    struct {
		bool baseOnly;
		bool noSolidFinal;
		bool passLog;
		std::optional<int> objectFilter;
		std::vector<int> skipObjects;
		std::vector<int> skipEffects;
	    } debug;

	    struct {
		glm::ivec4 geometry;
		TextureFlags clamp;
		WallpaperEngine::Render::WallpaperState::TextureUVsScaling scalingMode;
		/** Manual zoom factor layered on top of scalingMode, see --zoom */
		float zoom;
		/** Corner color shown outside the wallpaper's bounds when clamp is border, see --corner-color */
		glm::vec4 cornerColor;
	    } window;

	    struct {
		WAYLAND_LAYER layer;
	    } wayland;
	} render;

	// Audio settings
	struct {
	    bool enabled;
	    /** 0-128 */
	    int volume;
	    bool automute;
	    bool audioprocessing;
	} audio;

	// Mouse input settings
	struct {
	    bool enabled;
	    bool disableparallax;
	} mouse;

	// Screenshot settings
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
            .dumpStructure = false,
            .disabledObjects = {},
            .enabledObjects = {},
            .assets = "",
            .defaultBackground = "",
            .screenBackgrounds = {},
            .properties = {},
            .screenScalings = {},
            .screenClamps = {},
            .screenZooms = {},
            .screenCornerColors = {},
            .screenPlaylists = {},
            .defaultPlaylist = std::nullopt,
            .spanGroups = {},
            .webHost = false,
            .webHostShm = "",
            .webHostWidth = 0,
            .webHostHeight = 0,
        },
        .render = {
            .mode = NORMAL_WINDOW,
            .maximumFPS = 60,
            .playbackSpeed = 1.0f,
            .pauseOnFullscreen = true,
            .pauseOnFullscreenOnlyWhenActive = false,
            .fullscreenPauseIgnoreAppIds = {},
            .debug = {
                .baseOnly = false,
                .noSolidFinal = false,
	                .passLog = false,
	                .objectFilter = std::nullopt,
	                .skipObjects = {},
	                .skipEffects = {},
	            },
            .window = {
                .geometry = {},
                .clamp = TextureFlags_ClampUVsBorder,
                .scalingMode = WallpaperEngine::Render::WallpaperState::TextureUVsScaling::DefaultUVs,
                .zoom = 1.0f,
                .cornerColor = { 0.0f, 0.0f, 0.0f, 1.0f },
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
        },
        .mouse = {
            .enabled = true,
            .disableparallax = false,
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
} // namespace WallpaperEngine::Application
