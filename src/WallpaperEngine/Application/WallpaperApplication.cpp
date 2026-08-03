#include "WallpaperApplication.h"

#include <cstdlib>

#include "Steam/FileSystem/FileSystem.h"
#include "WallpaperEngine/Application/ApplicationState.h"
#include "WallpaperEngine/Assets/AssetLoadException.h"
#include "WallpaperEngine/Audio/Drivers/Detectors/PulseAudioPlayingDetector.h"
#include "WallpaperEngine/FileSystem/Container.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Drivers/VideoFactories.h"
#include "WallpaperEngine/Render/RenderContext.h"
#include "WallpaperEngine/Render/Wallpapers/CVideo.h"

#include "WallpaperEngine/Data/Dumpers/StringPrinter.h"
#include "WallpaperEngine/Data/Parsers/ProjectParser.h"
#include "WallpaperEngine/Data/Utils/AudioSensitivity.h"

#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Debugging/CallStack.h"
#include "WallpaperEngine/FileSystem/Adapters/MediaCover.h"
#include "WallpaperEngine/Media/DBusMediaSource.h"

#include "WallpaperEngine/WebBrowser/CEF/BrowserClient.h"
#include "WallpaperEngine/WebBrowser/CEF/SharedMemoryRenderHandler.h"
#include "WallpaperEngine/WebBrowser/IPC/WebHostSharedMemory.h"
#include "include/cef_browser.h"

#if DEMOMODE
#include "recording.h"
#endif /* DEMOMODE */

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <csignal>
#include <ctime>
#include <fstream>
#include <numeric>
#include <string_view>
#include <unistd.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include <thread>

#define FULLSCREEN_CHECK_WAIT_TIME 250

float g_Time;
float g_TimeLast;
/** Unscaled wall-clock time, unaffected by --speed */
float g_RealTime;
float g_Daytime;

using namespace WallpaperEngine::Assets;
using namespace WallpaperEngine::Application;
using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::FileSystem;

void CustomGLDebugCallback (
    GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam
) {
    if (severity != GL_DEBUG_SEVERITY_HIGH) {
	return;
    }

    sLog.error ("OpenGL error: ", message, ", type: ", type, ", id: ", id);

    std::vector<WallpaperEngine::Debugging::CallStack::CallInfo> callInfo;

    WallpaperEngine::Debugging::CallStack::GetCalls (callInfo);

    for (std::vector<WallpaperEngine::Debugging::CallStack::CallInfo>::size_type i = 0; i < callInfo.size (); ++i) {
	fprintf (
	    stderr, "[%3lu] %15lu: %s in %s\n", callInfo.size () - i, callInfo[i].offset, callInfo[i].function.c_str (),
	    callInfo[i].module.c_str ()
	);
    }
}

bool WallpaperApplication::isCefSubprocess () const {
    for (int i = 1; i < this->m_context.getArgc (); i++) {
	if (std::string_view (this->m_context.getArgv ()[i]).starts_with ("--type=")) {
	    return true;
	}
    }

    return false;
}

WallpaperApplication::WallpaperApplication (ApplicationContext& context) : m_context (context) {
    this->initializeSubsystems ();

    try {
	this->loadBackgrounds ();
    } catch (const std::exception& e) {
	if (!this->isCefSubprocess ()) {
	    throw;
	}

	// CEF re-execs this binary for its own subprocesses with a reconstructed argv that doesn't
	// always resolve to a loadable background. Must still reach setupBrowser() below regardless
	// - that's what calls CefExecuteProcess() to hand off into Chromium's subprocess entrypoint;
	// skipping it here leaves CEF's IPC handshake incomplete, which reads as a crashed subprocess
	// and gets retried, turning one web wallpaper into several stray processes.
	sLog.error ("Skipping background load for CEF subprocess re-exec: ", e.what ());
    }

    this->setupProperties ();
    this->setupAudioSensitivity ();
    this->listObjects ();
    this->listAudioObjects ();
    this->setupBrowser ();
    this->initializePlaylists ();
}

void WallpaperApplication::initializeSubsystems () {
    m_mediaSource = std::make_unique<WallpaperEngine::Media::DBusMediaSource> (std::chrono::milliseconds (2000));
}

AssetLocatorUniquePtr WallpaperApplication::setupAssetLocator (const std::string& bg) const {
    auto container = std::make_unique<Container> ();

    const std::filesystem::path path = bg;

    container->registerAdapterFactory (std::make_unique<MediaCoverFactory> (*this->m_mediaSource));
    container->mount ("$mediaThumbnail", "$mediaThumbnail");
    container->mount (path, "/");

    try {
	container->mount (path / "scene.pkg", "/");
    } catch (std::runtime_error&) { }

    try {
	container->mount (path / "gifscene.pkg", "/");
    } catch (std::runtime_error&) { }

    try {
	container->mount (this->m_context.settings.general.assets, "/");
    } catch (std::runtime_error&) {
	sLog.exception ("Cannot find a valid assets folder, resolved to ", this->m_context.settings.general.assets);
    }

    try {
	container->mount (std::filesystem::current_path (), "/");
    } catch (std::runtime_error&) { }

    auto& vfs = container->getVFS ();

    // Had to get a little creative with the effects to achieve the same bloom effect without any custom code:
    // these virtual files are loaded by an image in the scene that takes the current _rt_FullFrameBuffer and
    // applies the bloom effect to render it out to the screen.
    vfs.add (
	"effects/wpenginelinux/bloomeffect.json",
	{ { "name", "camerabloom_wpengine_linux" },
	  { "group", "wpengine_linux_camera" },
	  { "dependencies", JSON::array () },
	  {
	      "passes",
	      JSON::array (
		  { { { "material", "materials/util/downsample_quarter_bloom.json" },
		      { "target", "_rt_4FrameBuffer" },
		      { "bind", JSON::array ({ { { "name", "_rt_FullFrameBuffer" }, { "index", 0 } } }) } },
		    { { "material", "materials/util/downsample_eighth_blur_v.json" },
		      { "target", "_rt_8FrameBuffer" },
		      { "bind", JSON::array ({ { { "name", "_rt_4FrameBuffer" }, { "index", 0 } } }) } },
		    { { "material", "materials/util/blur_h_bloom.json" },
		      { "target", "_rt_Bloom" },
		      { "bind", JSON::array ({ { { "name", "_rt_8FrameBuffer" }, { "index", 0 } } }) } },
		    { { "material", "materials/util/combine.json" },
		      { "target", "_rt_FullFrameBuffer" },
		      { "bind",
			JSON::array (
			    { { { "name", "_rt_imageLayerComposite_-1_a" }, { "index", 0 } },
			      { { "name", "_rt_Bloom" }, { "index", 1 } } }
			) } } }
	      ),
	  } }
    );

    // Wastes a render pass on an image element that exists only to host the bloom material above
    vfs.add ("models/wpenginelinux.json", { { "material", "materials/wpenginelinux.json" } });

    vfs.add (
	"materials/wpenginelinux.json",
	{ { "passes",
	    JSON::array (
		{ { { "blending", "normal" },
		    { "cullmode", "nocull" },
		    { "depthtest", "disabled" },
		    { "depthwrite", "disabled" },
		    { "shader", "genericimage2" },
		    { "textures", JSON::array ({ "_rt_FullFrameBuffer" }) } } }
	    ) } }
    );

    vfs.add (
	"shaders/commands/copy.frag",
	"uniform sampler2D g_Texture0;\n"
	"in vec2 v_TexCoord;\n"
	"void main () {\n"
	"out_FragColor = texture (g_Texture0, v_TexCoord);\n"
	"}"
    );
    vfs.add (
	"shaders/commands/copy.vert",
	"in vec3 a_Position;\n"
	"in vec2 a_TexCoord;\n"
	"out vec2 v_TexCoord;\n"
	"void main () {\n"
	"gl_Position = vec4 (a_Position, 1.0);\n"
	"v_TexCoord = a_TexCoord;\n"
	"}"
    );

    return std::make_unique<AssetLocator> (std::move (container));
}

void WallpaperApplication::loadBackgrounds () {
    if (this->m_context.settings.render.mode == ApplicationContext::NORMAL_WINDOW
	|| this->m_context.settings.render.mode == ApplicationContext::EXPLICIT_WINDOW) {
	auto path = this->m_context.settings.general.defaultBackground;

	if (this->m_context.settings.general.defaultPlaylist.has_value ()
	    && !this->m_context.settings.general.defaultPlaylist->items.empty ()) {
	    path = this->m_context.settings.general.defaultPlaylist->items.front ();
	}

	this->m_backgrounds["default"] = this->loadBackground (path);
	return;
    }

    for (const auto& [screen, path] : this->m_context.settings.general.screenBackgrounds) {
	// skip span group synthetic keys here, they're handled below
	if (screen.rfind ("span:", 0) == 0) {
	    continue;
	}
	// screens with no path should use the default
	if (path.empty ()) {
	    this->m_backgrounds[screen] = this->loadBackground (this->m_context.settings.general.defaultBackground);
	} else {
	    this->m_backgrounds[screen] = this->loadBackground (path);
	}
    }

    // Load one background per span group
    for (const auto& spanGroup : this->m_context.settings.general.spanGroups) {
	if (spanGroup.screens.empty ()) {
	    continue;
	}

	std::filesystem::path bgPath = spanGroup.background;
	if (bgPath.empty ()) {
	    bgPath = this->m_context.settings.general.defaultBackground;
	}

	// use the first screen's name as the group key for the loaded project
	const std::string groupKey = "span:" + spanGroup.screens.front ();
	this->m_backgrounds[groupKey] = this->loadBackground (bgPath);
    }
}

ProjectUniquePtr WallpaperApplication::loadBackground (const std::string& bg) {
    auto container = this->setupAssetLocator (bg);
    auto json = WallpaperEngine::Data::JSON::JSON::parse (container->readString ("project.json"));

    // when a background is loaded, reset the screenshot variables
    // this allows taking screenshots after a background changes
    // useful for playlists
    if (this->m_context.settings.screenshot.take) {
	this->m_nextFrameScreenshot = this->m_context.settings.screenshot.delay;

	if (this->m_videoDriver != nullptr) {
	    this->m_nextFrameScreenshot += this->m_videoDriver->getFrameCounter ();
	}

	this->m_screenShotTaken = false;
    }

    return WallpaperEngine::Data::Parsers::ProjectParser::parse (json, std::move (container));
}

std::vector<std::size_t>
WallpaperApplication::buildPlaylistOrder (const ApplicationContext::PlaylistDefinition& definition) {
    std::vector<std::size_t> order (definition.items.size ());
    std::iota (order.begin (), order.end (), 0);

    if (definition.settings.order == "random") {
	std::shuffle (order.begin (), order.end (), this->m_playlistRng);
    }

    return order;
}

void WallpaperApplication::initializePlaylists () {
    const bool hasDefaultPlaylist = this->m_context.settings.general.defaultPlaylist.has_value ();
    const bool hasScreenPlaylists = !this->m_context.settings.general.screenPlaylists.empty ();

    if (!hasDefaultPlaylist && !hasScreenPlaylists) {
	return;
    }

    const auto now = std::chrono::steady_clock::now ();

    auto registerPlaylist = [this, now] (
				const std::string& key, const ApplicationContext::PlaylistDefinition& playlist,
				std::optional<std::filesystem::path> currentPath
			    ) {
	if (playlist.items.empty ()) {
	    return;
	}

	ActivePlaylist state;

	state.definition = playlist;
	state.order = this->buildPlaylistOrder (playlist);

	if (state.order.empty ()) {
	    return;
	}

	if (currentPath.has_value ()) {
	    state.orderIndex = 0;

	    for (std::size_t i = 0; i < state.order.size (); i++) {
		if (playlist.items[state.order[i]] == currentPath.value ()) {
		    state.orderIndex = i;
		    break;
		}
	    }
	}

	const uint32_t delayMinutes = std::max<uint32_t> (1, state.definition.settings.delayMinutes);
	state.nextSwitch = now + std::chrono::minutes (delayMinutes);
	state.lastUpdate = now;

	this->m_activePlaylists.insert_or_assign (key, std::move (state));
    };

    if (hasDefaultPlaylist
	&& (this->m_context.settings.render.mode == ApplicationContext::NORMAL_WINDOW
	    || this->m_context.settings.render.mode == ApplicationContext::EXPLICIT_WINDOW)) {
	const auto& playlist = this->m_context.settings.general.defaultPlaylist.value ();
	const auto currentPath = playlist.items.empty ()
	    ? std::optional<std::filesystem::path> { this->m_context.settings.general.defaultBackground }
	    : std::optional<std::filesystem::path> { playlist.items.front () };
	registerPlaylist ("default", playlist, currentPath);
    }

    for (const auto& [screen, playlist] : this->m_context.settings.general.screenPlaylists) {
	const auto current = this->m_context.settings.general.screenBackgrounds.find (screen);
	const auto currentPath = current != this->m_context.settings.general.screenBackgrounds.end ()
	    ? std::optional<std::filesystem::path> { current->second }
	    : std::nullopt;
	registerPlaylist (screen, playlist, currentPath);
    }
}

bool WallpaperApplication::makeAnyViewportCurrent () const {
    if (!this->m_renderContext) {
	return false;
    }

    const auto& viewports = this->m_renderContext->getOutput ().getViewports ();

    if (viewports.empty ()) {
	return false;
    }

    viewports.begin ()->second->makeCurrent ();
    return true;
}

bool WallpaperApplication::preflightWallpaper (const std::string& path) {
    try {
	// avoid mutating state, just ensure project.json parses
	auto container = this->setupAssetLocator (path);
	const auto json = WallpaperEngine::Data::JSON::JSON::parse (container->readString ("project.json"));
	if (!json.contains ("type") || !json.contains ("file")) {
	    sLog.error ("Preflight failed for ", path, ": missing required fields");
	    return false;
	}
	return true;
    } catch (const std::exception& e) {
	sLog.error ("Preflight failed for ", path, ": ", e.what ());
	return false;
    }
}

bool WallpaperApplication::selectNextCandidate (ActivePlaylist& playlist, std::size_t& outOrderIndex) {
    if (playlist.order.empty ()) {
	return false;
    }

    std::size_t attempts = 0;
    std::size_t candidateOrderIndex = outOrderIndex;

    while (attempts < playlist.order.size ()) {
	const auto candidateIndex = playlist.order[candidateOrderIndex];

	if (!playlist.failedIndices.contains (candidateIndex)) {
	    outOrderIndex = candidateOrderIndex;
	    return true;
	}

	attempts++;
	candidateOrderIndex = (candidateOrderIndex + 1) % playlist.order.size ();
    }

    return false;
}

void WallpaperApplication::advancePlaylist (
    const std::string& screen, ActivePlaylist& playlist, const std::chrono::steady_clock::time_point& now
) {
    if (playlist.order.empty ()) {
	return;
    }

    playlist.orderIndex = (playlist.orderIndex + 1) % playlist.order.size ();

    if (playlist.orderIndex == 0 && playlist.definition.settings.order == "random") {
	std::shuffle (playlist.order.begin (), playlist.order.end (), this->m_playlistRng);
    }

    std::size_t candidateOrderIndex = playlist.orderIndex;

    if (!this->selectNextCandidate (playlist, candidateOrderIndex)) {
	sLog.error ("All playlist items failed for ", screen, ", keeping current wallpaper");
	const uint32_t delayMinutes = std::max<uint32_t> (1, playlist.definition.settings.delayMinutes);
	playlist.nextSwitch = now + std::chrono::minutes (delayMinutes);
	return;
    }

    const auto candidateIndex = playlist.order[candidateOrderIndex];
    const auto& candidatePath = playlist.definition.items[candidateIndex];

    if (!this->preflightWallpaper (candidatePath.string ())) {
	playlist.failedIndices.insert (candidateIndex);

	if (!this->selectNextCandidate (playlist, candidateOrderIndex)) {
	    sLog.error ("All playlist items failed for ", screen, ", keeping current wallpaper");
	    const uint32_t delayMinutes = std::max<uint32_t> (1, playlist.definition.settings.delayMinutes);
	    playlist.nextSwitch = now + std::chrono::minutes (delayMinutes);
	    return;
	}
    }

    playlist.orderIndex = candidateOrderIndex;
    const auto& nextPath = playlist.definition.items[playlist.order[playlist.orderIndex]];

    bool loaded = false;

    try {
	if (!this->makeAnyViewportCurrent ()) {
	    sLog.error ("Cannot switch playlist on ", screen, ": no active viewport");
	    throw std::runtime_error ("No viewport available");
	}

	auto project = this->loadBackground (nextPath.string ());

	this->setupPropertiesForProject (*project);

	this->m_backgrounds[screen] = std::move (project);

	const auto scalingIt = this->m_context.settings.general.screenScalings.find (screen);
	const auto clampIt = this->m_context.settings.general.screenClamps.find (screen);
	const auto scaling = scalingIt != this->m_context.settings.general.screenScalings.end ()
	    ? scalingIt->second
	    : this->m_context.settings.render.window.scalingMode;
	const auto clamp = clampIt != this->m_context.settings.general.screenClamps.end ()
	    ? clampIt->second
	    : this->m_context.settings.render.window.clamp;

	if (this->m_renderContext) {
	    auto wallpaper = WallpaperEngine::Render::CWallpaper::fromWallpaper (
		*this->m_backgrounds[screen]->wallpaper, *this->m_renderContext, *this->m_audioContext, nextPath,
		scaling, clamp, this->resolveScreenRenderSize (screen)
	    );
	    wallpaper->setZoom (this->resolveScreenZoom (screen));
	    wallpaper->setCornerColor (this->resolveScreenCornerColor (screen));
	    this->m_renderContext->setWallpaper (screen, std::move (wallpaper));
	    this->applyAudioPolicy ();
	}

	this->m_context.settings.general.screenBackgrounds[screen] = nextPath;
	loaded = true;
    } catch (const std::exception& e) {
	sLog.error ("Failed to advance playlist on ", screen, ": ", e.what ());
    }

    if (!loaded) {
	playlist.failedIndices.insert (playlist.order[playlist.orderIndex]);

	// Keep current position; next timer tick will retry advancement
	sLog.error ("Failed to load wallpaper for ", screen, ", will retry on next cycle");
    }

    const uint32_t delayMinutes = std::max<uint32_t> (1, playlist.definition.settings.delayMinutes);
    playlist.nextSwitch = now + std::chrono::minutes (delayMinutes);
}

void WallpaperApplication::updatePlaylists () {
    if (this->m_activePlaylists.empty ()) {
	return;
    }

    const auto now = std::chrono::steady_clock::now ();

    for (auto& [screen, playlist] : this->m_activePlaylists) {
	playlist.lastUpdate = now;

	if (playlist.definition.settings.mode != "timer") {
	    continue;
	}

	if (playlist.definition.items.size () <= 1) {
	    continue;
	}

	if (now < playlist.nextSwitch) {
	    continue;
	}

	this->advancePlaylist (screen, playlist, now);
    }
}

namespace {
/** Everything a control-file request can carry; any field left unset means "don't touch this" */
struct HotswapRequest {
    std::optional<std::string> path;
    /** True once a "layers=1" line was seen, meaning disabledObjects/enabledObjects below are a full replacement */
    bool layersProvided = false;
    std::vector<std::string> disabledObjects;
    std::vector<std::string> enabledObjects;
    /** True once at least one "property=name=value" line was seen */
    bool propertiesProvided = false;
    std::map<std::string, std::string> properties;
    std::optional<int> volume;
    /** "on"/"off"/"toggle" (also "1"/"0"/"true"/"false" for on/off) */
    std::optional<std::string> xray;
    /** "stretch"/"fit"/"fill"/"center"/"default" */
    std::optional<std::string> scaling;
    /** floating-point zoom factor layered on top of scaling, e.g. "1.5" */
    std::optional<std::string> zoom;
    /** "on"/"off"/"toggle" (also "1"/"0"/"true"/"false" for on/off) */
    std::optional<std::string> disableParallax;
    /** hex RGB/RGBA color, e.g. "000000" or "#1a1a1aff" */
    std::optional<std::string> cornerColor;
    /** floating-point playback speed multiplier, e.g. "0.5" */
    std::optional<std::string> speed;
    /** screen name to restrict audio to, or "" to clear the restriction, see --audio-screen */
    std::optional<std::string> audioScreen;
    /** 0-128, see --ambient-volume */
    std::optional<std::string> ambientVolume;
    /** True once at least one "audio-sensitivity=id=multiplier" line was seen */
    bool audioSensitivityProvided = false;
    std::map<std::string, std::string> audioSensitivity;
};

std::string trimHotswapToken (const std::string& value) {
    std::size_t begin = 0;
    std::size_t end = value.size ();

    while (begin < end && std::isspace (static_cast<unsigned char> (value[begin]))) {
	begin++;
    }

    while (end > begin && std::isspace (static_cast<unsigned char> (value[end - 1]))) {
	end--;
    }

    return value.substr (begin, end - begin);
}

/**
 * Parses the control file. Supports the original bare-path-on-one-line format for backwards
 * compatibility, plus key=value lines (path/layers/disable-object/enable-object/volume/xray/scaling/zoom/
 * disable-parallax/corner-color/speed/audio-screen/ambient-volume/property) so a single request can carry
 * more than just the background path. "property=name=value" (repeatable) carries --set-property-equivalent
 * overrides.
 */
HotswapRequest parseHotswapRequest (std::istream& file) {
    HotswapRequest request;
    std::string rawLine;

    while (std::getline (file, rawLine)) {
	const std::string line = trimHotswapToken (rawLine);

	if (line.empty ()) {
	    continue;
	}

	const auto separator = line.find ('=');

	if (separator == std::string::npos) {
	    // legacy format: the whole line is the new background path
	    if (!request.path.has_value ()) {
		request.path = line;
	    }

	    continue;
	}

	const std::string key = trimHotswapToken (line.substr (0, separator));
	const std::string value = trimHotswapToken (line.substr (separator + 1));

	if (key == "path") {
	    request.path = value;
	} else if (key == "layers") {
	    request.layersProvided = true;
	} else if (key == "disable-object") {
	    request.layersProvided = true;
	    request.disabledObjects.push_back (value);
	} else if (key == "enable-object") {
	    request.layersProvided = true;
	    request.enabledObjects.push_back (value);
	} else if (key == "volume") {
	    try {
		request.volume = std::stoi (value);
	    } catch (const std::exception&) {
		sLog.error ("Hotswap: ignoring invalid volume value: ", value);
	    }
	} else if (key == "xray") {
	    request.xray = value;
	} else if (key == "scaling") {
	    request.scaling = value;
	} else if (key == "zoom") {
	    request.zoom = value;
	} else if (key == "disable-parallax") {
	    request.disableParallax = value;
	} else if (key == "corner-color") {
	    request.cornerColor = value;
	} else if (key == "speed") {
	    request.speed = value;
	} else if (key == "audio-screen") {
	    request.audioScreen = value;
	} else if (key == "ambient-volume") {
	    request.ambientVolume = value;
	} else if (key == "property") {
	    const auto propSeparator = value.find ('=');

	    if (propSeparator == std::string::npos) {
		sLog.error ("Hotswap: ignoring malformed property line: ", value);
	    } else {
		request.propertiesProvided = true;
		request.properties[value.substr (0, propSeparator)] = value.substr (propSeparator + 1);
	    }
	} else if (key == "audio-sensitivity") {
	    const auto sensSeparator = value.find ('=');

	    if (sensSeparator == std::string::npos) {
		sLog.error ("Hotswap: ignoring malformed audio-sensitivity line: ", value);
	    } else {
		request.audioSensitivityProvided = true;
		request.audioSensitivity[value.substr (0, sensSeparator)] = value.substr (sensSeparator + 1);
	    }
	} else {
	    sLog.error ("Hotswap: ignoring unknown control file key: ", key);
	}
    }

    return request;
}
} // namespace

void WallpaperApplication::checkHotswapRequest () {
    if (!this->m_hotswapRequested.exchange (false)) {
	return;
    }

    const char* runtimeDir = getenv ("XDG_RUNTIME_DIR");
    const auto controlFile = std::filesystem::path (runtimeDir != nullptr ? runtimeDir : "/tmp") / "lwe-control";

    std::ifstream file (controlFile);

    if (!file.is_open ()) {
	sLog.error ("Hotswap requested but control file could not be read: ", controlFile.string ());
	return;
    }

    const auto request = parseHotswapRequest (file);

    if (!request.path.has_value () && !request.layersProvided && !request.volume.has_value ()
	&& !request.xray.has_value () && !request.scaling.has_value () && !request.zoom.has_value ()
	&& !request.disableParallax.has_value () && !request.cornerColor.has_value ()
	&& !request.speed.has_value () && !request.audioScreen.has_value () && !request.ambientVolume.has_value ()
	&& !request.propertiesProvided && !request.audioSensitivityProvided) {
	sLog.error ("Hotswap requested but control file was empty");
	return;
    }

    if (request.volume.has_value ()) {
	this->applyVolumeHotswap (*request.volume);
    }

    if (request.xray.has_value ()) {
	this->applyXrayHotswap (*request.xray);
    }

    if (request.scaling.has_value ()) {
	this->applyScalingHotswap (*request.scaling);
    }

    if (request.zoom.has_value ()) {
	this->applyZoomHotswap (*request.zoom);
    }

    if (request.disableParallax.has_value ()) {
	this->applyParallaxHotswap (*request.disableParallax);
    }

    if (request.cornerColor.has_value ()) {
	this->applyCornerColorHotswap (*request.cornerColor);
    }

    if (request.speed.has_value ()) {
	this->applySpeedHotswap (*request.speed);
    }

    if (request.audioScreen.has_value ()) {
	this->applyAudioScreenHotswap (*request.audioScreen);
    }

    if (request.ambientVolume.has_value ()) {
	this->applyAmbientVolumeHotswap (*request.ambientVolume);
    }

    if (request.layersProvided) {
	this->m_context.settings.general.disabledObjects = request.disabledObjects;
	this->m_context.settings.general.enabledObjects = request.enabledObjects;
    }

    if (request.propertiesProvided) {
	for (const auto& [name, value] : request.properties) {
	    this->m_context.settings.general.properties[name] = value;
	}
    }

    if (request.audioSensitivityProvided) {
	for (const auto& [target, value] : request.audioSensitivity) {
	    try {
		this->m_context.settings.general.audioSensitivity[target] = std::stof (value);
	    } catch (const std::exception&) {
		sLog.error ("Hotswap: ignoring invalid audio-sensitivity value: ", value);
	    }
	}
    }

    // volume/xray/speed-only requests are pure live setters with nothing to reload. Properties
    // and audio sensitivity, like layers, are baked into the scene graph at parse time, so they
    // need the same reload.
    if (!request.path.has_value () && !request.layersProvided && !request.propertiesProvided
	&& !request.audioSensitivityProvided) {
	return;
    }

    if (request.path.has_value () && !this->preflightWallpaper (*request.path)) {
	sLog.error ("Hotswap failed, invalid wallpaper at ", *request.path);
	return;
    }

    if (!this->makeAnyViewportCurrent ()) {
	sLog.error ("Hotswap failed, no active viewport");
	return;
    }

    if (request.path.has_value ()) {
	sLog.out ("Hotswapping wallpaper to ", *request.path);
    } else {
	sLog.out ("Hotswapping wallpaper layers");
    }

    for (auto& [screen, background] : this->m_backgrounds) {
	const std::string targetPath = request.path.value_or (this->resolveScreenBackgroundPath (screen));

	try {
	    auto project = this->loadBackground (targetPath);

	    this->setupPropertiesForProject (*project);
	    this->setupAudioSensitivityForProject (*project);

	    background = std::move (project);

	    const auto scalingIt = this->m_context.settings.general.screenScalings.find (screen);
	    const auto clampIt = this->m_context.settings.general.screenClamps.find (screen);
	    const auto scaling = scalingIt != this->m_context.settings.general.screenScalings.end ()
		? scalingIt->second
		: this->m_context.settings.render.window.scalingMode;
	    const auto clamp = clampIt != this->m_context.settings.general.screenClamps.end ()
		? clampIt->second
		: this->m_context.settings.render.window.clamp;

	    if (this->m_renderContext) {
		auto wallpaper = WallpaperEngine::Render::CWallpaper::fromWallpaper (
		    *background->wallpaper, *this->m_renderContext, *this->m_audioContext, targetPath, scaling, clamp,
		    this->resolveScreenRenderSize (screen)
		);
		wallpaper->setZoom (this->resolveScreenZoom (screen));
		wallpaper->setCornerColor (this->resolveScreenCornerColor (screen));
		this->m_renderContext->setWallpaper (screen, std::move (wallpaper));
	    }

	    if (request.path.has_value ()) {
		if (screen.starts_with ("span:")) {
		    for (auto& spanGroup : this->m_context.settings.general.spanGroups) {
			if (!spanGroup.screens.empty () && "span:" + spanGroup.screens.front () == screen) {
			    spanGroup.background = *request.path;
			}
		    }
		} else {
		    this->m_context.settings.general.screenBackgrounds[screen] = *request.path;
		}
	    }
	} catch (const std::exception& e) {
	    sLog.error ("Hotswap failed for screen ", screen, ": ", e.what ());
	}
    }

    this->applyAudioPolicy ();

    if (request.path.has_value ()) {
	this->m_context.settings.general.defaultBackground = *request.path;
    }
}

std::string WallpaperApplication::resolveScreenBackgroundPath (const std::string& screen) const {
    if (screen.starts_with ("span:")) {
	for (const auto& spanGroup : this->m_context.settings.general.spanGroups) {
	    if (!spanGroup.screens.empty () && "span:" + spanGroup.screens.front () == screen) {
		return spanGroup.background.empty () ? this->m_context.settings.general.defaultBackground.string ()
						      : spanGroup.background.string ();
	    }
	}

	return this->m_context.settings.general.defaultBackground.string ();
    }

    const auto it = this->m_context.settings.general.screenBackgrounds.find (screen);

    if (it != this->m_context.settings.general.screenBackgrounds.end () && !it->second.empty ()) {
	return it->second.string ();
    }

    return this->m_context.settings.general.defaultBackground.string ();
}

float WallpaperApplication::resolveScreenZoom (const std::string& screen) const {
    const auto it = this->m_context.settings.general.screenZooms.find (screen);

    return it != this->m_context.settings.general.screenZooms.end () ? it->second
								      : this->m_context.settings.render.window.zoom;
}

glm::vec4 WallpaperApplication::resolveScreenCornerColor (const std::string& screen) const {
    const auto it = this->m_context.settings.general.screenCornerColors.find (screen);

    return it != this->m_context.settings.general.screenCornerColors.end ()
	? it->second
	: this->m_context.settings.render.window.cornerColor;
}

glm::ivec2 WallpaperApplication::resolveScreenRenderSize (const std::string& screen) const {
    const auto& viewports = this->m_renderContext->getOutput ().getViewports ();
    const auto it = viewports.find (screen);

    if (it != viewports.end ()) {
	return { it->second->viewport.z, it->second->viewport.w };
    }

    return { this->m_renderContext->getOutput ().getFullWidth (), this->m_renderContext->getOutput ().getFullHeight () };
}

void WallpaperApplication::applyVolumeHotswap (int volume) {
    volume = std::max (0, std::min (volume, 128));

    this->m_context.settings.audio.volume = volume;
    this->m_context.state.audio.volume = volume;

    if (!this->m_renderContext) {
	return;
    }

    const double scaledVolume = this->m_context.settings.audio.enabled ? volume * 100.0 / 128.0 : 0.0;

    for (const auto& [screen, wallpaper] : this->m_renderContext->getWallpapers ()) {
	if (wallpaper->is<WallpaperEngine::Render::Wallpapers::CVideo> ()) {
	    wallpaper->as<WallpaperEngine::Render::Wallpapers::CVideo> ()->setVolume (scaledVolume);
	}
    }

    sLog.out ("Hotswap: applied volume ", volume, " live");
}

void WallpaperApplication::applyXrayHotswap (const std::string& value) {
    bool newState;

    if (value == "toggle") {
	newState = !this->m_context.state.xray.fullReveal;
    } else if (value == "on" || value == "1" || value == "true") {
	newState = true;
    } else if (value == "off" || value == "0" || value == "false") {
	newState = false;
    } else {
	sLog.error ("Hotswap: ignoring invalid xray value: ", value);
	return;
    }

    this->m_context.state.xray.fullReveal = newState;

    sLog.out ("Hotswap: full xray ", newState ? "enabled" : "disabled", " live");
}

void WallpaperApplication::applyScalingHotswap (const std::string& value) {
    const auto mode = WallpaperEngine::Render::WallpaperState::parseScalingMode (value);

    if (!mode.has_value ()) {
	sLog.error ("Hotswap: ignoring invalid scaling value: ", value);
	return;
    }

    this->m_context.settings.render.window.scalingMode = *mode;

    for (auto& [screen, scaling] : this->m_context.settings.general.screenScalings) {
	scaling = *mode;
    }

    for (auto& spanGroup : this->m_context.settings.general.spanGroups) {
	spanGroup.scaling = *mode;
    }

    if (this->m_renderContext) {
	for (const auto& [screen, wallpaper] : this->m_renderContext->getWallpapers ()) {
	    wallpaper->setScalingMode (*mode);
	}
    }

    sLog.out ("Hotswap: applied scaling mode ", value, " live");
}

void WallpaperApplication::applyZoomHotswap (const std::string& value) {
    float zoom;

    try {
	zoom = std::stof (value);
    } catch (const std::exception&) {
	sLog.error ("Hotswap: ignoring invalid zoom value: ", value);
	return;
    }

    this->m_context.settings.render.window.zoom = zoom;

    for (auto& [screen, screenZoom] : this->m_context.settings.general.screenZooms) {
	screenZoom = zoom;
    }

    for (auto& spanGroup : this->m_context.settings.general.spanGroups) {
	spanGroup.zoom = zoom;
    }

    if (this->m_renderContext) {
	for (const auto& [screen, wallpaper] : this->m_renderContext->getWallpapers ()) {
	    wallpaper->setZoom (zoom);
	}
    }

    sLog.out ("Hotswap: applied zoom ", zoom, " live");
}

void WallpaperApplication::applyParallaxHotswap (const std::string& value) {
    bool newState;

    if (value == "toggle") {
	newState = !this->m_context.settings.mouse.disableparallax;
    } else if (value == "on" || value == "1" || value == "true") {
	newState = true;
    } else if (value == "off" || value == "0" || value == "false") {
	newState = false;
    } else {
	sLog.error ("Hotswap: ignoring invalid disable-parallax value: ", value);
	return;
    }

    this->m_context.settings.mouse.disableparallax = newState;

    sLog.out ("Hotswap: parallax ", newState ? "force-disabled" : "enabled", " live");
}

void WallpaperApplication::applyCornerColorHotswap (const std::string& value) {
    const auto color = WallpaperEngine::Render::CFBO::parseColor (value);

    if (!color.has_value ()) {
	sLog.error ("Hotswap: ignoring invalid corner color: ", value);
	return;
    }

    this->m_context.settings.render.window.cornerColor = *color;

    for (auto& [screen, screenColor] : this->m_context.settings.general.screenCornerColors) {
	screenColor = *color;
    }

    for (auto& spanGroup : this->m_context.settings.general.spanGroups) {
	spanGroup.cornerColor = *color;
    }

    if (this->m_renderContext) {
	for (const auto& [screen, wallpaper] : this->m_renderContext->getWallpapers ()) {
	    wallpaper->setCornerColor (*color);
	}
    }

    sLog.out ("Hotswap: applied corner color ", value, " live");
}

void WallpaperApplication::applySpeedHotswap (const std::string& value) {
    float speed;

    try {
	speed = std::stof (value);
    } catch (const std::exception&) {
	sLog.error ("Hotswap: ignoring invalid speed value: ", value);
	return;
    }

    if (speed <= 0.0f) {
	sLog.error ("Hotswap: ignoring non-positive speed value: ", value);
	return;
    }

    this->m_context.settings.render.playbackSpeed = speed;

    // Particles/effects/scripts read settings.render.playbackSpeed directly every frame, but
    // video wallpapers are driven by mpv's own clock and need the change pushed explicitly.
    if (this->m_renderContext) {
	for (const auto& [screen, wallpaper] : this->m_renderContext->getWallpapers ()) {
	    if (wallpaper->is<WallpaperEngine::Render::Wallpapers::CVideo> ()) {
		wallpaper->as<WallpaperEngine::Render::Wallpapers::CVideo> ()->setSpeed (speed);
	    }
	}
    }

    sLog.out ("Hotswap: applied speed ", speed, " live");
}

void WallpaperApplication::applyAudioScreenHotswap (const std::string& value) {
    this->m_context.settings.audio.audioScreen = value.empty () ? std::nullopt : std::optional<std::string> (value);

    this->applyAudioPolicy ();

    sLog.out ("Hotswap: applied audio screen '", value, "' live");
}

void WallpaperApplication::applyAmbientVolumeHotswap (const std::string& value) {
    int volume;

    try {
	volume = std::stoi (value);
    } catch (const std::exception&) {
	sLog.error ("Hotswap: ignoring invalid ambient volume value: ", value);
	return;
    }

    this->m_context.settings.audio.ambientVolume = std::max (0, std::min (volume, 128));

    this->applyAudioPolicy ();

    sLog.out ("Hotswap: applied ambient volume ", volume, " live");
}

void WallpaperApplication::applyAudioPolicy () {
    if (!this->m_renderContext) {
	return;
    }

    const auto& audioScreen = this->m_context.settings.audio.audioScreen;
    const auto& ambientVolume = this->m_context.settings.audio.ambientVolume;

    std::map<WallpaperEngine::Render::CWallpaper*, bool> wantMuted;

    for (const auto& [screen, wallpaper] : this->m_renderContext->getWallpapers ()) {
	const bool screenMuted = audioScreen.has_value () && screen != *audioScreen;
	auto [it, inserted] = wantMuted.try_emplace (wallpaper.get (), screenMuted);

	if (!inserted) {
	    it->second = it->second && screenMuted;
	}
    }

    for (const auto& [wallpaper, muted] : wantMuted) {
	wallpaper->setAudioPolicy (muted, ambientVolume);
    }
}

void WallpaperApplication::setupPropertiesForProject (const Project& project) {
    for (const auto& [key, cur] : project.properties) {
	auto override = this->m_context.settings.general.properties.find (key);

	if (override != this->m_context.settings.general.properties.end ()) {
	    sLog.out ("Applying override value for ", key);

	    cur->update (override->second, DynamicValue::UpdateSource::User);
	}

	if (this->m_context.settings.general.onlyListProperties) {
	    sLog.out (cur->dump ());
	}
    }
}

void WallpaperApplication::setupProperties () {
    for (const auto& [background, info] : this->m_backgrounds) {
	this->setupPropertiesForProject (*info);
    }
}

void WallpaperApplication::listObjectsForProject (const std::string& background, const Project& project) const {
    if (!project.wallpaper->is<Scene> ()) {
	return;
    }

    const auto scene = project.wallpaper->as<Scene> ();

    sLog.out ("Objects for ", background, ":");

    for (const auto& object : scene->objects) {
	std::string type = "unknown";

	if (object->is<Image> ()) {
	    type = "image";
	} else if (object->is<Particle> ()) {
	    type = "particle";
	} else if (object->is<Text> ()) {
	    type = "text";
	} else if (object->is<Sound> ()) {
	    type = "sound";
	}

	sLog.out ("  ", object->id, " - ", object->name, " (", type, ")");
    }
}

void WallpaperApplication::listObjects () const {
    if (!this->m_context.settings.general.onlyListObjects) {
	return;
    }

    for (const auto& [background, info] : this->m_backgrounds) {
	this->listObjectsForProject (background, *info);
    }
}

namespace {
struct AudioReactiveProperty {
    std::string name;
    DynamicValue* value;
};

void considerAudioReactive (
    std::vector<AudioReactiveProperty>& result, const std::string& name, const UserSettingUniquePtr& setting
) {
    if (!setting || !setting->value) {
	return;
    }

    const auto& source = setting->value->getScriptSource ();

    if (source.has_value () && source->find ("registerAudioBuffers") != std::string::npos) {
	result.push_back ({ name, setting->value.get () });
    }
}

// Mirrors exactly the set of fields ScriptableObject/CImage/CText/CParticle register with the
// script engine (see Scripting/ScriptableObject.cpp, Render/Objects/CImage.cpp, CText.cpp,
// CParticle.cpp) - "origin" is always the base object's own field (never overridden per-type),
// while scale/angles/visible fall back to the generic group* fields only for object types that
// don't provide their own (Sound, Light, plain groups).
std::vector<AudioReactiveProperty> collectAudioReactiveProperties (const Object& object) {
    std::vector<AudioReactiveProperty> result;

    considerAudioReactive (result, "origin", object.origin);

    if (object.is<Image> ()) {
	const auto* image = object.as<Image> ();
	considerAudioReactive (result, "scale", image->scale);
	considerAudioReactive (result, "angles", image->angles);
	considerAudioReactive (result, "visible", image->visible);
	considerAudioReactive (result, "alpha", image->alpha);
	considerAudioReactive (result, "color", image->color);
	considerAudioReactive (result, "parallaxDepth", image->parallaxDepth);
    } else if (object.is<Text> ()) {
	const auto* text = object.as<Text> ();
	considerAudioReactive (result, "scale", text->scale);
	considerAudioReactive (result, "color", text->color);
	considerAudioReactive (result, "alpha", text->alpha);
	considerAudioReactive (result, "visible", text->visible);
	considerAudioReactive (result, "pointSize", text->pointSize);
	considerAudioReactive (result, "text", text->text);
	considerAudioReactive (result, "parallaxDepth", text->parallaxDepth);
    } else if (object.is<Particle> ()) {
	const auto* particle = object.as<Particle> ();
	considerAudioReactive (result, "scale", particle->scale);
	considerAudioReactive (result, "angles", particle->angles);
	considerAudioReactive (result, "visible", particle->visible);
	considerAudioReactive (result, "parallaxDepth", particle->parallaxDepth);
    } else {
	considerAudioReactive (result, "scale", object.groupScale);
	considerAudioReactive (result, "angles", object.groupAngles);
	considerAudioReactive (result, "visible", object.groupVisible);
    }

    return result;
}

float readScriptPropertyFloat (DynamicValue& value, const std::string& key) {
    auto& properties = value.getProperties ();
    const auto it = properties.find (key);

    return it != properties.end () && it->second && it->second->value ? it->second->value->getFloat () : 0.0f;
}
} // namespace

void WallpaperApplication::listAudioObjectsForProject (const std::string& background, const Project& project) const {
    if (!project.wallpaper->is<Scene> ()) {
	return;
    }

    const auto scene = project.wallpaper->as<Scene> ();

    sLog.out ("Audio-reactive objects for ", background, ":");

    for (const auto& object : scene->objects) {
	for (const auto& reactive : collectAudioReactiveProperties (*object)) {
	    sLog.out (
		"  ", object->id, " - ", object->name, " (", reactive.name, "): minvalue=",
		readScriptPropertyFloat (*reactive.value, "minvalue"), " maxvalue=",
		readScriptPropertyFloat (*reactive.value, "maxvalue"), " frequency=",
		readScriptPropertyFloat (*reactive.value, "frequency"), " smoothing=",
		readScriptPropertyFloat (*reactive.value, "smoothing")
	    );
	}
    }
}

void WallpaperApplication::listAudioObjects () const {
    if (!this->m_context.settings.general.onlyListAudioObjects) {
	return;
    }

    for (const auto& [background, info] : this->m_backgrounds) {
	this->listAudioObjectsForProject (background, *info);
    }
}

void WallpaperApplication::setupAudioSensitivityForProject (const Project& project) const {
    if (!project.wallpaper->is<Scene> ()) {
	return;
    }

    const auto scene = project.wallpaper->as<Scene> ();

    for (const auto& object : scene->objects) {
	const auto sensitivity = this->m_context.resolveAudioSensitivity (object->id, object->name);

	if (!sensitivity.has_value ()) {
	    continue;
	}

	for (const auto& reactive : collectAudioReactiveProperties (*object)) {
	    auto& scriptProps = reactive.value->getProperties ();
	    const auto minIt = scriptProps.find ("minvalue");
	    const auto maxIt = scriptProps.find ("maxvalue");

	    if (minIt == scriptProps.end () || maxIt == scriptProps.end () || !minIt->second->value
		|| !maxIt->second->value) {
		continue;
	    }

	    const auto [newMin, newMax] = WallpaperEngine::Data::Utils::scaleAudioRange (
		minIt->second->value->getFloat (), maxIt->second->value->getFloat (), sensitivity.value ()
	    );

	    minIt->second->value->update (newMin, DynamicValue::UpdateSource::User);
	    maxIt->second->value->update (newMax, DynamicValue::UpdateSource::User);

	    sLog.debug (
		"Applying audio sensitivity ", sensitivity.value (), " to ", object->id, " - ", object->name, " (",
		reactive.name, ")"
	    );
	}
    }
}

void WallpaperApplication::setupAudioSensitivity () {
    for (const auto& [background, info] : this->m_backgrounds) {
	this->setupAudioSensitivityForProject (*info);
    }
}

void WallpaperApplication::setupBrowser () {
    // The main engine process never hosts CEF directly - CEF only supports one
    // CefInitialize()/CefShutdown() pair per process, so a process that might later need to stop
    // and restart hosting a web wallpaper can't safely do it in place. Only two roles reach here:
    // CEF's own subprocess re-execs (--type=zygote/gpu-process/renderer/utility), which must
    // always reach WebBrowserContext even if loadBackgrounds() found nothing to load, and this
    // process's own --web-host role.
    if ((!this->isCefSubprocess () && !this->m_context.settings.general.webHost) || this->m_browserContext) {
	return;
    }

    this->m_browserContext = std::make_unique<WebBrowser::WebBrowserContext> (*this);
}

void WallpaperApplication::runWebHost () {
    using namespace WallpaperEngine::WebBrowser;
    using namespace WallpaperEngine::WebBrowser::IPC;

    const auto backgroundIt = this->m_backgrounds.find ("default");

    if (backgroundIt == this->m_backgrounds.end () || !backgroundIt->second->wallpaper->is<Web> ()) {
	sLog.error ("--web-host requires a single Web wallpaper background, none found");
	return;
    }

    if (!this->m_browserContext) {
	sLog.error ("--web-host: CEF was not initialized (setupBrowser() found no Web project?)");
	return;
    }

    const Project& project = *backgroundIt->second;
    const Web& web = *project.wallpaper->as<Web> ();
    const int width = static_cast<int> (this->m_context.settings.general.webHostWidth);
    const int height = static_cast<int> (this->m_context.settings.general.webHostHeight);

    if (width <= 0 || height <= 0) {
	sLog.error ("--web-host: invalid --web-host-width/--web-host-height");
	return;
    }

    auto* shm = attachSharedMemory (this->m_context.settings.general.webHostShm, width, height);

    if (shm == nullptr) {
	sLog.error ("--web-host: failed to attach shared memory ", this->m_context.settings.general.webHostShm);
	return;
    }

    CefWindowInfo windowInfo;
    windowInfo.SetAsWindowless (0);

    CefBrowserSettings browserSettings;
    browserSettings.windowless_frame_rate = std::max (60, this->m_context.settings.render.maximumFPS);

    const CefRefPtr<CEF::SharedMemoryRenderHandler> renderHandler = new CEF::SharedMemoryRenderHandler (shm);
    const CefRefPtr<CEF::BrowserClient> client = new CEF::BrowserClient (renderHandler, project.properties);

    // the "w" prefix + workshop id host must match what the scheme handler factory expects to resolve
    const std::string htmlURL = std::string (WPENGINE_SCHEME) + "://w" + project.workshopId + "/" + web.filename;

    sLog.out ("--web-host: creating browser for ", htmlURL, " (pid ", static_cast<long> (getpid ()), ")");

    const CefRefPtr<CefBrowser> browser
	= CefBrowserHost::CreateBrowserSync (windowInfo, client, htmlURL, browserSettings, nullptr, nullptr);

    if (!browser) {
	sLog.error ("--web-host: failed to create the CEF browser");
	shm->helperFailed.store (true, std::memory_order_release);
	closeSharedMemory (shm, this->m_context.settings.general.webHostShm, width, height, false);
	return;
    }

    shm->helperReady.store (true, std::memory_order_release);

    Input::MouseClickStatus lastLeft = Input::Released;
    Input::MouseClickStatus lastRight = Input::Released;
    uint32_t lastDesiredWidth = shm->desiredWidth.load (std::memory_order_relaxed);
    uint32_t lastDesiredHeight = shm->desiredHeight.load (std::memory_order_relaxed);
    bool lastAudioMuted = shm->audioMuted.load (std::memory_order_relaxed);
    browser->GetHost ()->SetAudioMuted (lastAudioMuted);

    while (!shm->quitRequested.load (std::memory_order_acquire)) {
	CefDoMessageLoopWork ();

	const uint32_t desiredWidth = shm->desiredWidth.load (std::memory_order_relaxed);
	const uint32_t desiredHeight = shm->desiredHeight.load (std::memory_order_relaxed);

	if (desiredWidth != lastDesiredWidth || desiredHeight != lastDesiredHeight) {
	    lastDesiredWidth = desiredWidth;
	    lastDesiredHeight = desiredHeight;
	    browser->GetHost ()->WasResized ();
	}

	const bool desiredAudioMuted = shm->audioMuted.load (std::memory_order_relaxed);

	if (desiredAudioMuted != lastAudioMuted) {
	    lastAudioMuted = desiredAudioMuted;
	    browser->GetHost ()->SetAudioMuted (lastAudioMuted);
	}

	CefMouseEvent evt;
	evt.x = static_cast<int> (shm->mouseX.load (std::memory_order_relaxed));
	evt.y = static_cast<int> (shm->mouseY.load (std::memory_order_relaxed));
	browser->GetHost ()->SendMouseMoveEvent (evt, false);

	const auto left = static_cast<Input::MouseClickStatus> (shm->leftClick.load (std::memory_order_relaxed));
	const auto right = static_cast<Input::MouseClickStatus> (shm->rightClick.load (std::memory_order_relaxed));

	if (left != lastLeft) {
	    browser->GetHost ()->SendMouseClickEvent (
		evt, CefBrowserHost::MouseButtonType::MBT_LEFT, left == Input::Released, 1
	    );
	    lastLeft = left;
	}

	if (right != lastRight) {
	    browser->GetHost ()->SendMouseClickEvent (
		evt, CefBrowserHost::MouseButtonType::MBT_RIGHT, right == Input::Released, 1
	    );
	    lastRight = right;
	}

	std::this_thread::sleep_for (std::chrono::milliseconds (4));
    }

    browser->GetHost ()->CloseBrowser (true);

    // CloseBrowser() only requests an async close - wait briefly for it to finish before
    // CefShutdown() below tears everything down out from under it
    for (int i = 0; i < 500 && !client->isClosed (); i++) {
	CefDoMessageLoopWork ();
    }

    closeSharedMemory (shm, this->m_context.settings.general.webHostShm, width, height, false);
}

void WallpaperApplication::takeScreenshot (const std::filesystem::path& filename) const {
    const int width = this->m_renderContext->getOutput ().getFullWidth ();
    const int height = this->m_renderContext->getOutput ().getFullHeight ();
    const bool vflip = this->m_renderContext->getOutput ().renderVFlip ();
    const auto& wallpapers = this->m_renderContext->getWallpapers ();

    struct ViewportCapture {
	uint8_t* buffer;
	int readWidth;
	int readHeight;
	int vpWidth;
	int vpHeight;
	int xoffset;
	float ustart, uend, vstart, vend;
    };

    std::vector<ViewportCapture> captures;
    int currentXOffset = 0;

    for (const auto& [screen, viewport] : this->m_renderContext->getOutput ().getViewports ()) {
	viewport->makeCurrent ();

	const auto wallpaperIt = wallpapers.find (screen);
	if (wallpaperIt == wallpapers.end ()) {
	    sLog.error ("Cannot find wallpaper for screen ", screen);
	    continue;
	}

	const auto& wallpaper = wallpaperIt->second;
	const int vpWidth = viewport->viewport.z - viewport->viewport.x;
	const int vpHeight = viewport->viewport.w - viewport->viewport.y;

	// bind the wallpaper's FBO to read from it directly
	// this is more reliable than the default framebuffer on some drivers (NVIDIA/Wayland)
	glBindFramebuffer (GL_FRAMEBUFFER, wallpaper->getWallpaperFramebuffer ());

	// ensure rendering is complete before reading
	glFinish ();

	const int readWidth = wallpaper->getWidth ();
	const int readHeight = wallpaper->getHeight ();
	const auto bufferSize = readWidth * readHeight * 3;
	auto* buffer = new uint8_t[bufferSize];

	glPixelStorei (GL_PACK_ALIGNMENT, 1);
	if (GLEW_VERSION_4_5) {
	    glReadnPixels (0, 0, readWidth, readHeight, GL_RGB, GL_UNSIGNED_BYTE, bufferSize, buffer);
	} else {
	    glReadPixels (0, 0, readWidth, readHeight, GL_RGB, GL_UNSIGNED_BYTE, buffer);
	}

	glBindFramebuffer (GL_FRAMEBUFFER, 0);

	if (const GLenum error = glGetError (); error != GL_NO_ERROR) {
	    sLog.error ("Cannot obtain pixel data for screen ", screen, ". OpenGL error: ", error);
	    delete[] buffer;
	    continue;
	}

	// Get the UV coordinates which define the visible portion based on scaling mode
	const auto [ustart, uend, vstart, vend] = wallpaper->getState ().getTextureUVs ();

	captures.push_back (
	    { buffer, readWidth, readHeight, vpWidth, vpHeight, currentXOffset, ustart, uend, vstart, vend }
	);

	if (viewport->single) {
	    currentXOffset += vpWidth;
	}
    }

    const auto extension = filename.extension ();
    const std::string extStr = extension.string ();

    // Offload pixel processing and saving to a background thread to avoid hitches
    std::thread ([captures, width, height, vflip, extStr, filename] () {
	auto* bitmap = new uint8_t[width * height * 3] { 0 };

	for (const auto& capture : captures) {
	    // copy pixels to bitmap, sampling from the UV-defined region
	    for (int y = 0; y < capture.vpHeight; y++) {
		for (int x = 0; x < capture.vpWidth; x++) {
		    // interpolate within the UV range to get source coordinates
		    const float u
			= capture.ustart + (static_cast<float> (x) / capture.vpWidth) * (capture.uend - capture.ustart);
		    const float v = capture.vstart
			+ (static_cast<float> (y) / capture.vpHeight) * (capture.vend - capture.vstart);

		    // convert UV to pixel coordinates in the source buffer
		    const int srcX = std::clamp (static_cast<int> (u * capture.readWidth), 0, capture.readWidth - 1);
		    const int srcY = std::clamp (static_cast<int> (v * capture.readHeight), 0, capture.readHeight - 1);
		    const int srcIdx = (srcY * capture.readWidth + srcX) * 3;

		    const int xfinal = x + capture.xoffset;
		    // FBO content is not flipped like default framebuffer, so invert vflip logic
		    const int yfinal = vflip ? y : (capture.vpHeight - y - 1);

		    if (yfinal >= 0 && yfinal < height && xfinal >= 0 && xfinal < width) {
			bitmap[yfinal * width * 3 + xfinal * 3] = capture.buffer[srcIdx];
			bitmap[yfinal * width * 3 + xfinal * 3 + 1] = capture.buffer[srcIdx + 1];
			bitmap[yfinal * width * 3 + xfinal * 3 + 2] = capture.buffer[srcIdx + 2];
		    }
		}
	    }
	    delete[] capture.buffer;
	}

	if (extStr == ".bmp") {
	    stbi_write_bmp (filename.c_str (), width, height, 3, bitmap);
	} else if (extStr == ".png") {
	    stbi_write_png (filename.c_str (), width, height, 3, bitmap, width * 3);
	} else if (extStr == ".jpg" || extStr == ".jpeg") {
	    stbi_write_jpg (filename.c_str (), width, height, 3, bitmap, 100);
	}

	delete[] bitmap;
    }).detach ();
}

void WallpaperApplication::setupOutput () {
    const char* XDG_SESSION_TYPE = getenv ("XDG_SESSION_TYPE");

    if (!XDG_SESSION_TYPE) {
	sLog.exception (
	    "Cannot read environment variable XDG_SESSION_TYPE, window server detection failed. Please ensure proper "
	    "values are set"
	);
    }

    sLog.debug ("Checking for window servers: ");

    for (const auto& windowServer : sVideoFactories.getRegisteredDrivers ()) {
	sLog.debug ("\t", windowServer);
    }

    this->m_videoDriver = sVideoFactories.createVideoDriver (
	this->m_context.settings.render.mode, XDG_SESSION_TYPE, this->m_context, *this
    );
    this->m_fullScreenDetector
	= sVideoFactories.createFullscreenDetector (XDG_SESSION_TYPE, this->m_context, *this->m_videoDriver);
}

void WallpaperApplication::setupAudio () {
    // ensure audioprocessing is required by any background, and we have it enabled
    const bool audioProcessingRequired = std::ranges::any_of (
	this->m_backgrounds, [] (const std::pair<const std::string, ProjectUniquePtr>& pair) -> bool {
	    return pair.second->supportsAudioProcessing;
	}
    );

    if (audioProcessingRequired && this->m_context.settings.audio.audioprocessing) {
	this->m_audioRecorder
	    = std::make_unique<WallpaperEngine::Audio::Drivers::Recorders::PulseAudioPlaybackRecorder> ();
    } else {
	this->m_audioRecorder = std::make_unique<WallpaperEngine::Audio::Drivers::Recorders::PlaybackRecorder> ();
    }

    if (this->m_context.settings.audio.automute) {
	m_audioDetector = std::make_unique<WallpaperEngine::Audio::Drivers::Detectors::PulseAudioPlayingDetector> (
	    this->m_context, *this->m_fullScreenDetector
	);
    } else {
	m_audioDetector = std::make_unique<WallpaperEngine::Audio::Drivers::Detectors::AudioPlayingDetector> (
	    this->m_context, *this->m_fullScreenDetector
	);
    }

    m_audioDriver = std::make_unique<WallpaperEngine::Audio::Drivers::SDLAudioDriver> (
	this->m_context, *this->m_audioDetector, *this->m_audioRecorder
    );
    m_audioContext = std::make_unique<WallpaperEngine::Audio::AudioContext> (*m_audioDriver);
}

void WallpaperApplication::prepareOutputs () {
    m_renderContext
	= std::make_unique<WallpaperEngine::Render::RenderContext> (*m_videoDriver, *this, *this->m_mediaSource);

    // set all the specific wallpapers required (skip span group synthetic keys)
    for (const auto& [background, info] : this->m_backgrounds) {
	if (background.rfind ("span:", 0) == 0) {
	    continue;
	}
	const auto scalingIt = this->m_context.settings.general.screenScalings.find (background);
	const auto clampIt = this->m_context.settings.general.screenClamps.find (background);
	const auto scaling = scalingIt != this->m_context.settings.general.screenScalings.end ()
	    ? scalingIt->second
	    : this->m_context.settings.render.window.scalingMode;
	const auto clamp = clampIt != this->m_context.settings.general.screenClamps.end ()
	    ? clampIt->second
	    : this->m_context.settings.render.window.clamp;

	auto wallpaper = WallpaperEngine::Render::CWallpaper::fromWallpaper (
	    *info->wallpaper, *m_renderContext, *m_audioContext, this->resolveScreenBackgroundPath (background),
	    scaling, clamp, this->resolveScreenRenderSize (background)
	);
	wallpaper->setZoom (this->resolveScreenZoom (background));
	wallpaper->setCornerColor (this->resolveScreenCornerColor (background));
	m_renderContext->setWallpaper (background, std::move (wallpaper));
    }

    // Set up span groups: one shared wallpaper per group, registered for each viewport
    for (const auto& spanGroup : this->m_context.settings.general.spanGroups) {
	if (spanGroup.screens.empty ()) {
	    continue;
	}

	const std::string groupKey = "span:" + spanGroup.screens.front ();
	const auto bgIt = this->m_backgrounds.find (groupKey);
	if (bgIt == this->m_backgrounds.end ()) {
	    continue;
	}

	// Compute the bounding box of all viewports in this span group
	const auto& viewports = m_renderContext->getOutput ().getViewports ();
	int minX = INT_MAX, minY = INT_MAX, maxX = INT_MIN, maxY = INT_MIN;
	bool anyFound = false;

	for (const auto& screenName : spanGroup.screens) {
	    const auto vpIt = viewports.find (screenName);
	    if (vpIt == viewports.end ()) {
		sLog.error ("Span group screen not found: ", screenName);
		continue;
	    }
	    anyFound = true;
	    const auto& vp = vpIt->second;
	    const int x = vp->globalPosition.x;
	    const int y = vp->globalPosition.y;
	    const int w = vp->logicalSize.x;
	    const int h = vp->logicalSize.y;
	    sLog.debug (
		"SPAN DEBUG prepareOutputs: screen '", screenName, "' globalPos=(", x, ",", y, ") logicalSize=", w, "x",
		h
	    );
	    minX = std::min (minX, x);
	    minY = std::min (minY, y);
	    maxX = std::max (maxX, x + w);
	    maxY = std::max (maxY, y + h);
	}

	if (!anyFound) {
	    sLog.error ("No viewports found for span group, skipping");
	    continue;
	}

	sLog.debug (
	    "SPAN DEBUG prepareOutputs: bounding box=(", minX, ",", minY, ",", maxX - minX, ",", maxY - minY, ")"
	);

	WallpaperEngine::Render::CWallpaper::SpanInfo spanInfo;
	spanInfo.totalBounds = { minX, minY, maxX - minX, maxY - minY };

	// Create one shared wallpaper with the span group's scaling mode
	auto sharedWallpaper = WallpaperEngine::Render::CWallpaper::fromWallpaper (
	    *bgIt->second->wallpaper, *m_renderContext, *m_audioContext, this->resolveScreenBackgroundPath (groupKey),
	    spanGroup.scaling, spanGroup.clamp, glm::ivec2 { maxX - minX, maxY - minY }
	);
	sharedWallpaper->setZoom (spanGroup.zoom);
	sharedWallpaper->setCornerColor (spanGroup.cornerColor);

	// Convert to shared_ptr so it can be registered for multiple viewports
	std::shared_ptr<WallpaperEngine::Render::CWallpaper> shared (std::move (sharedWallpaper));
	shared->setSpanInfo (spanInfo);

	// Register the same wallpaper for each screen in the span group
	for (const auto& screenName : spanGroup.screens) {
	    m_renderContext->setWallpaper (screenName, shared);
	}
    }

    this->applyAudioPolicy ();
}

void WallpaperApplication::setupOpenGLDebugging () {
#if !NDEBUG
    glDebugMessageCallback (CustomGLDebugCallback, nullptr);
    glEnable (GL_DEBUG_OUTPUT_SYNCHRONOUS);
#endif
}

void WallpaperApplication::setup () {
    this->setupOutput ();

    // we_manager launches us with LD_PRELOAD forcing the system's libEGL, because CEF's own
    // bundled libEGL sits next to us on LD_LIBRARY_PATH and would otherwise shadow it and break
    // our own Wayland/EGL output. That preload has already done its job for our own process by
    // this point (setupOutput() above just created our GL context with it) - but it stays in the
    // environment we inherit, and CEF's own subprocesses (spawned lazily, e.g. the GPU process
    // the moment a web wallpaper's WebGL content first needs one) would inherit it too, forcing
    // Mesa's system EGL onto CEF's ANGLE stack instead of the bundled EGL it actually expects.
    // That silently breaks WebGL - no crash, no error, just a canvas that never paints anything.
    // Unsetting it here only affects processes we fork+exec after this point; it doesn't undo
    // anything already loaded into our own process by the dynamic linker at our own startup.
    unsetenv ("LD_PRELOAD");

    this->setupAudio ();
    this->prepareOutputs ();
    this->setupOpenGLDebugging ();

    if (this->m_context.settings.general.dumpStructure) {
	auto prettyPrinter = Data::Dumpers::StringPrinter ();

	for (const auto& [background, info] : this->m_renderContext->getWallpapers ()) {
	    prettyPrinter.printWallpaper (info->getWallpaperData ());
	}

	std::cout << prettyPrinter.str () << std::endl;
    }

#if DEMOMODE
    // ensure only one background is running so everything can be properly caught
    if (this->m_renderContext->getWallpapers ().size () > 1) {
	sLog.exception ("Demo mode only supports one background");
    }

    int width = this->m_renderContext->getWallpapers ().begin ()->second->getWidth ();
    int height = this->m_renderContext->getWallpapers ().begin ()->second->getHeight ();
    std::vector<uint8_t> pixels (width * height * 3);
    bool initialized = false;
    int frame = 0;
#endif /* DEMOMODE */
}

void WallpaperApplication::render () {
    static time_t seconds;
    static struct tm* timeinfo;
    static float rawTimeLast = 0.0f;

    if (this->m_isPaused) {
	usleep (FULLSCREEN_CHECK_WAIT_TIME);
	if (this->m_fullScreenDetector->anythingFullscreen () && this->m_context.state.general.keepRunning) {
	    return;
	}
	m_renderContext->setPause (false);

	// account for paused duration in playlist timers
	const auto pausedNow = std::chrono::steady_clock::now ();
	const auto pausedDuration = pausedNow - this->m_pauseStart;

	for (auto& [_, playlist] : this->m_activePlaylists) {
	    if (!playlist.definition.settings.updateOnPause) {
		playlist.nextSwitch += pausedDuration;
		playlist.lastUpdate += pausedDuration;
	    }
	}

	this->m_isPaused = false;
    } else {
	time (&seconds);
	timeinfo = localtime (&seconds);
	g_Daytime = static_cast<float> ((timeinfo->tm_hour * 60) + timeinfo->tm_min) / (24.0f * 60.0f);

	const float rawTimeNow = m_videoDriver->getRenderTime ();
	const float rawDelta = rawTimeNow - rawTimeLast;
	rawTimeLast = rawTimeNow;

	g_TimeLast = g_Time;
	g_Time += rawDelta * this->m_context.settings.render.playbackSpeed;
	g_RealTime = rawTimeNow;
	m_audioDriver->update ();
	m_mediaSource->update ();
	m_videoDriver->getInputContext ().update ();
	m_videoDriver->dispatchEventQueue ();

	if (m_videoDriver->closeRequested ()) {
	    sLog.out ("Stop requested by driver");
	    this->m_context.state.general.keepRunning = false;
	}

#if DEMOMODE
	// wait for a full render cycle before actually starting
	// this gives some extra time for video and web decoders to set themselves up
	// because of size changes
	if (m_videoDriver->getFrameCounter () > (uint32_t)this->m_context.settings.render.maximumFPS) {
	    if (!initialized) {
		width = this->m_renderContext->getWallpapers ().begin ()->second->getWidth ();
		height = this->m_renderContext->getWallpapers ().begin ()->second->getHeight ();
		pixels.reserve (width * height * 3);
		init_encoder ("output.webm", width, height);
		initialized = true;
	    }

	    glBindFramebuffer (
		GL_FRAMEBUFFER, this->m_renderContext->getWallpapers ().begin ()->second->getWallpaperFramebuffer ()
	    );

	    glPixelStorei (GL_PACK_ALIGNMENT, 1);
	    glReadPixels (0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data ());
	    write_video_frame (pixels.data ());
	    frame++;

	    if (frame >= FRAME_COUNT) {
		this->m_context.state.general.keepRunning = false;
	    }
	}
#endif /* DEMOMODE */
	if (this->m_fullScreenDetector->anythingFullscreen () && this->m_context.state.general.keepRunning) {
	    this->m_isPaused = true;
	    this->m_pauseStart = std::chrono::steady_clock::now ();

	    m_renderContext->setPause (true);
	    return;
	}
    }

    this->checkHotswapRequest ();
    this->updatePlaylists ();

    if (!this->m_context.settings.screenshot.take || this->m_screenShotTaken == true) {
	return;
    }

    if (this->m_videoDriver->getFrameCounter () < this->m_nextFrameScreenshot) {
	return;
    }

    this->takeScreenshot (this->m_context.settings.screenshot.path);
    this->m_screenShotTaken = true;
}

void WallpaperApplication::cleanup () {
    sLog.out ("Stopping");

#if DEMOMODE
    close_encoder ();
#endif /* DEMOMODE */

    SDL_Quit ();
}

void WallpaperApplication::show () {
    setup ();
    while (this->m_context.state.general.keepRunning) {
	render ();
    }
    cleanup ();
}

void WallpaperApplication::update (Render::Drivers::Output::OutputViewport* viewport) {
    m_renderContext->render (viewport);
}

void WallpaperApplication::signal (int signal) {
    if (signal == SIGUSR1) {
	// keep the handler itself trivial, the actual reload happens on the render thread
	this->m_hotswapRequested = true;
	return;
    }

    sLog.out ("Stop requested by signal ", signal);
    this->m_context.state.general.keepRunning = false;
}

const std::map<std::string, ProjectUniquePtr>& WallpaperApplication::getBackgrounds () const {
    return this->m_backgrounds;
}

ApplicationContext& WallpaperApplication::getContext () const { return this->m_context; }

const WallpaperEngine::Render::Drivers::Output::Output& WallpaperApplication::getOutput () const {
    return this->m_renderContext->getOutput ();
}

void WallpaperApplication::setDestinationFramebuffer (GLuint framebuffer) {
    this->m_destinationFramebuffer = framebuffer;
    for (const auto& [screen, wallpaper] : this->m_renderContext->getWallpapers ()) {
	wallpaper->setDestinationFramebuffer (framebuffer);
    };
}

GLuint WallpaperApplication::getDestinationFramebuffer () const { return this->m_destinationFramebuffer; }