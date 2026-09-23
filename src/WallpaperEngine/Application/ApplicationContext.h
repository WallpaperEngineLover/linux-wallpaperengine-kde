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

    /** true to force visible, false to force hidden, nullopt to leave the scene's own value, from --disable-effect/--enable-effect */
    [[nodiscard]] std::optional<bool> resolveEffectVisibility (int id, const std::string& name) const;

    /**
     * Resolves the --audio-sensitivity multiplier for an object/layer, matching id or name, or
     * falling back to a "*" wildcard default if one was given and no more specific match exists.
     *
     * @return the configured multiplier, or nullopt if this object has no override (use the
     *         wallpaper's original minvalue/maxvalue unchanged)
     */
    [[nodiscard]] std::optional<float> resolveAudioSensitivity (int id, const std::string& name) const;

    /**
     * Resolves the --sound-volume override for a Sound object, matching id or name, or falling
     * back to a "*" wildcard default if one was given and no more specific match exists.
     *
     * @return the configured volume (0-1), or nullopt if this object has no override (use the
     *         wallpaper's original volume unchanged)
     */
    [[nodiscard]] std::optional<float> resolveSoundVolume (int id, const std::string& name) const;

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
	/** Re-centers a cropping scaling mode/zoom's visible window, see --offset */
	glm::vec2 offset = { 0.0f, 0.0f };
	/** Color shown outside the wallpaper's bounds when clamp is border, see --corner-color */
	glm::vec4 cornerColor = { 0.0f, 0.0f, 0.0f, 1.0f };
    };

    struct {
	struct {
	    bool onlyListProperties;
	    bool onlyListObjects;
	    bool onlyListAudioObjects;
	    bool onlyListEffects;
	    bool dumpStructure;
	    bool disableParticles;
	    /** Grows a scene's render canvas to fit every image layer that extends past it, see --expand-canvas */
	    bool expandCanvas;
	    /** Objects/layers to force-hide, matched by id or name */
	    std::vector<std::string> disabledObjects;
	    /** Objects/layers to force-show, matched by id or name */
	    std::vector<std::string> enabledObjects;
	    /** Object effects (bloom, blur, glow, etc) to force-hide, matched by effect id or editor name */
	    std::vector<std::string> disabledEffects;
	    /** Object effects to force-show, matched by effect id or editor name */
	    std::vector<std::string> enabledEffects;
	    /** Audio-reactive pulse amplitude multiplier per object, matched by id or name; 0 = locked/no pulse */
	    std::map<std::string, float> audioSensitivity;
	    /** Sound object volume override (0-1), matched by id or name; see --sound-volume */
	    std::map<std::string, float> soundVolume;
	    std::filesystem::path assets;
	    /** Background to load (provided as the final argument) as fallback for multi-screen setups */
	    std::filesystem::path defaultBackground;
	    std::map<std::string, std::filesystem::path> screenBackgrounds;
	    std::map<std::string, std::string> properties;
	    std::map<std::string, WallpaperEngine::Render::WallpaperState::TextureUVsScaling> screenScalings;
	    std::map<std::string, TextureFlags> screenClamps;
	    /** Manual zoom factor for different screens, layered on top of the scaling mode */
	    std::map<std::string, float> screenZooms;
	    /** Re-centers a cropping scaling mode/zoom's visible window for different screens, see --offset */
	    std::map<std::string, glm::vec2> screenOffsets;
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

	struct {
	    WINDOW_MODE mode;
	    int maximumFPS;
	    /** Global playback speed multiplier for animations, particles and effects, see --speed */
	    float playbackSpeed;
	    /** Freezes scene time entirely (scripts, particles, effects and puppet meshes all stop advancing), see --disable-animations */
	    bool freezeAnimations;
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
		/** Logs mean scene brightness every 30 frames */
		bool brightnessLog;
		/** Renders puppets in their static bind pose, ignoring animation clips entirely */
		bool noPuppetAnimation;
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
		/** Re-centers a cropping scaling mode/zoom's visible window, see --offset */
		glm::vec2 offset;
		/** Corner color shown outside the wallpaper's bounds when clamp is border, see --corner-color */
		glm::vec4 cornerColor;
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
	    /** Only this screen (matches --screen-root names) produces audio; nullopt = no restriction */
	    std::optional<std::string> audioScreen;
	    /** 0-128, applied to non-video (scene sound + web) backgrounds instead of volume; nullopt = use volume */
	    std::optional<int> ambientVolume;
	} audio;

	struct {
	    bool enabled;
	    bool disableparallax;
	    /** Clamps parallax displacement so an image never slides past its own edges (no black corners) */
	    bool clampParallaxToImageSize;
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
            .freezeAnimations = false,
            .pauseOnFullscreen = true,
            .pauseOnFullscreenOnlyWhenActive = false,
            .fullscreenPauseIgnoreAppIds = {},
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
            .audioScreen = std::nullopt,
            .ambientVolume = std::nullopt,
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
} // namespace WallpaperEngine::Application
