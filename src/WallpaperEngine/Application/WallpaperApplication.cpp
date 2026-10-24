#include "WallpaperApplication.h"
#include "ControlFileOwner.h"

#include <cstdlib>

#include "Steam/FileSystem/FileSystem.h"
#include "WallpaperEngine/Application/ApplicationState.h"
#include "WallpaperEngine/Assets/AssetLoadException.h"
#include "WallpaperEngine/Audio/Drivers/Detectors/PulseAudioPlayingDetector.h"
#include "WallpaperEngine/Data/Utils/FiniteFloat.h"
#ifdef ENABLE_WAYLAND
#include "WallpaperEngine/Desktop/HostWayland.h"
#endif
#include "WallpaperEngine/FileSystem/Container.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Logging/ThreadGuard.h"
#include "WallpaperEngine/Render/Drivers/VideoFactories.h"
#include "WallpaperEngine/Render/RenderContext.h"
#include "WallpaperEngine/Render/Shaders/ShaderCache.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include "WallpaperEngine/Render/Wallpapers/CSplat.h"
#include "WallpaperEngine/Render/Wallpapers/CVideo.h"
#include "WallpaperEngine/Render/Wallpapers/CWeb.h"
#include "WallpaperEngine/Scripting/LocalStorage.h"

#include "WallpaperEngine/Data/Dumpers/StringPrinter.h"
#include "WallpaperEngine/Data/Parsers/ProjectParser.h"
#include "WallpaperEngine/Data/Utils/AudioSensitivity.h"

#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Debugging/CallStack.h"
#include "WallpaperEngine/FileSystem/Adapters/MediaCover.h"
#include "WallpaperEngine/Media/DBusMediaSource.h"

#include "WallpaperEngine/WebBrowser/CEF/BrowserClient.h"
#include "WallpaperEngine/WebBrowser/CEF/PageBridge.h"
#include "WallpaperEngine/WebBrowser/CEF/SharedMemoryRenderHandler.h"
#include "WallpaperEngine/WebBrowser/IPC/WebHostSharedMemory.h"
#include "include/cef_browser.h"

#if DEMOMODE
#include "recording.h"
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <csignal>
#include <ctime>
#include <fstream>
#include <numeric>
#include <ranges>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include <thread>
#include <utility>

using WallpaperEngine::Data::Utils::parseFiniteFloat;

#define FULLSCREEN_CHECK_WAIT_TIME 250000

float g_Time;
float g_TimeLast;
float g_RealTime;
float g_Daytime;

using namespace WallpaperEngine::Assets;
using namespace WallpaperEngine::Application;
using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::FileSystem;

void CustomGLDebugCallback (
    GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam
) {
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) {
	return;
    }

    // libmpv rebuffers a GL_STATIC_DRAW buffer every video frame, that hint would drown out everything else
    if (type == GL_DEBUG_TYPE_PERFORMANCE) {
	return;
    }

    sLog.error ("OpenGL error: ", message, ", type: ", type, ", id: ", id, ", severity: ", severity);

    std::vector<WallpaperEngine::Debugging::CallStack::CallInfo> callInfo;

    WallpaperEngine::Debugging::CallStack::GetCalls (callInfo);

    for (std::vector<WallpaperEngine::Debugging::CallStack::CallInfo>::size_type i = 0; i < callInfo.size (); ++i) {
	fprintf (
	    stderr, "[%3lu] %15lu: %s in %s\n", callInfo.size () - i, callInfo[i].offset, callInfo[i].function.c_str (),
	    callInfo[i].module.c_str ()
	);
    }
}

WallpaperApplication::WallpaperApplication (ApplicationContext& context) : m_context (context) {
    Render::Shaders::ShaderCache::get ().setEnabled (!this->m_context.settings.general.noShaderCache);
    this->initializeSubsystems ();

    this->loadBackgrounds ();

    this->setupProperties ();
    this->setupAudioSensitivity ();
    this->setupSoundVolume ();
    this->listObjects ();
    this->listEffects ();
    this->listAudioObjects ();
    this->setupBrowser ();
    this->initializePlaylists ();
}

void WallpaperApplication::initializeSubsystems () {
    m_mediaSource = std::make_unique<WallpaperEngine::Media::DBusMediaSource> (std::chrono::milliseconds (2000));
}

AssetLocatorUniquePtr
WallpaperApplication::setupAssetLocator (const std::string& bg, const std::filesystem::path& overlay) const {
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

    if (!overlay.empty ()) {
	container->mount (overlay, "/");
    }

    try {
	container->mount (this->m_context.settings.general.assets, "/");
    } catch (std::runtime_error&) {
	sLog.exception ("Cannot find a valid assets folder, resolved to ", this->m_context.settings.general.assets);
    }

    try {
	container->mount (std::filesystem::current_path (), "/");
    } catch (std::runtime_error&) { }

    auto& vfs = container->getVFS ();

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
	if (screen.rfind ("span:", 0) == 0) {
	    continue;
	}
	if (path.empty ()) {
	    this->m_backgrounds[screen] = this->loadBackground (this->m_context.settings.general.defaultBackground);
	} else {
	    this->m_backgrounds[screen] = this->loadBackground (path);
	}
    }

    for (const auto& spanGroup : this->m_context.settings.general.spanGroups) {
	if (spanGroup.screens.empty ()) {
	    continue;
	}

	std::filesystem::path bgPath = spanGroup.background;
	if (bgPath.empty ()) {
	    bgPath = this->m_context.settings.general.defaultBackground;
	}

	const std::string groupKey = "span:" + spanGroup.screens.front ();
	this->m_backgrounds[groupKey] = this->loadBackground (bgPath);
    }
}

WallpaperApplication::ProjectSource WallpaperApplication::openProjectSource (const std::string& path) const {
    auto container = this->setupAssetLocator (path);
    auto json = WallpaperEngine::Data::JSON::JSON::parseAsset (container->readString ("project.json"));

    if (json.contains ("file") || !json.contains ("dependency")) {
	return { std::move (container), std::move (json), std::nullopt, path };
    }

    const auto dependency = json.at ("dependency");
    const auto dependencyId = dependency.is_string () ? dependency.get<std::string> () : dependency.dump ();

    if (dependencyId.empty () || dependencyId == "." || dependencyId == ".."
	|| dependencyId.find ('/') != std::string::npos) {
	sLog.exception ("Preset ", path, " has an invalid dependency '", dependencyId, "'");
    }

    std::filesystem::path presetDir = path;

    if (!presetDir.has_filename ()) {
	presetDir = presetDir.parent_path ();
    }

    const auto baseDir = presetDir.parent_path () / dependencyId;

    if (!std::filesystem::exists (baseDir / "project.json")) {
	sLog.exception ("Preset ", path, " requires wallpaper ", dependencyId, " which is not installed");
    }

    auto baseContainer = this->setupAssetLocator (baseDir.string (), presetDir);
    auto baseJson = WallpaperEngine::Data::JSON::JSON::parseAsset (baseContainer->readString ("project.json"));

    return { std::move (baseContainer), std::move (baseJson), json.optional ("preset"), baseDir };
}

namespace {
std::optional<float> presetNumber (const WallpaperEngine::Data::JSON::JSON& preset, const char* key) {
    const auto it = preset.find (key);

    if (it == preset.end ()) {
	return std::nullopt;
    }

    if (it->is_number ()) {
	return it->get<float> ();
    }

    if (it->is_string ()) {
	try {
	    return parseFiniteFloat (it->get<std::string> ());
	} catch (const std::exception&) { }
    }

    return std::nullopt;
}

std::optional<bool> presetBool (const WallpaperEngine::Data::JSON::JSON& preset, const char* key) {
    const auto it = preset.find (key);

    if (it == preset.end ()) {
	return std::nullopt;
    }

    if (it->is_boolean ()) {
	return it->get<bool> ();
    }

    if (it->is_number ()) {
	return it->get<float> () != 0.0f;
    }

    if (it->is_string ()) {
	return it->get<std::string> () == "true" || it->get<std::string> () == "1";
    }

    return std::nullopt;
}

/** Wallpaper Engine's own settings a preset can carry, see wallpaper64.exe sub_140181F30 */
ImageAdjustments parseImageAdjustments (const WallpaperEngine::Data::JSON::JSON& preset) {
    ImageAdjustments result {
	.colorEnabled = presetBool (preset, "wec_e"),
	.brightness = presetNumber (preset, "wec_brs"),
	.contrast = presetNumber (preset, "wec_con"),
	.saturation = presetNumber (preset, "wec_sa"),
	.hue = presetNumber (preset, "wec_hue"),
	.filter = std::nullopt,
	.filterStrength = presetNumber (preset, "wcc_amt"),
	.flipHorizontal = presetBool (preset, "alignmentfliph"),
    };

    if (const auto it = preset.find ("wcc_v"); it != preset.end () && it->is_string ()) {
	result.filter = it->get<std::string> ();
    }

    return result;
}
}

void WallpaperApplication::applyPreset (
    Project& project, const WallpaperEngine::Data::JSON::JSON& preset, const std::filesystem::path& presetDir
) {
    project.imageAdjustments = parseImageAdjustments (preset);

    for (const auto& entry : preset.items ()) {
	const auto property = project.properties.find (entry.key ());

	// wec_* and friends are Wallpaper Engine's own settings (read above), not wallpaper properties
	if (property == project.properties.end () || entry.value ().is_null ()) {
	    continue;
	}

	const auto& value = entry.value ();
	std::string text;

	if (value.is_string ()) {
	    text = value.get<std::string> ();
	} else if (value.is_boolean ()) {
	    text = value.get<bool> () ? "true" : "false";
	} else {
	    text = value.dump ();
	}

	// file and directory properties hold paths relative to the preset folder, pages expect an absolute one
	if (const auto* file = dynamic_cast<const PropertyFile*> (property->second.get ());
	    file != nullptr && !text.empty ()) {
	    std::error_code pathError;
	    const auto folder = std::filesystem::weakly_canonical (presetDir, pathError);
	    const auto candidate = std::filesystem::weakly_canonical (folder / text, pathError);
	    const auto inside = candidate.lexically_relative (folder);

	    // a preset is someone else's content, its files are in its own folder: "/" or "../.." must not open the
	    // user's disk to a web page
	    if (pathError || inside.empty () || *inside.begin () == "..") {
		sLog.error ("Preset value for ", entry.key (), " is outside the preset's folder, ignored: ", text);
		continue;
	    }

	    if (file->isDirectory () ? std::filesystem::is_directory (candidate)
				     : std::filesystem::is_regular_file (candidate)) {
		text = candidate.string ();
	    }
	}

	// a preset is someone else's content, only the user assigns what a shortcut opens
	if (property->second->is<PropertyUserShortcut> ()) {
	    continue;
	}

	if (auto* combo = dynamic_cast<PropertyCombo*> (property->second.get ()); combo != nullptr) {
	    combo->allowValue (text);
	}

	try {
	    property->second->update (text, DynamicValue::UpdateSource::User);
	} catch (const std::exception& e) {
	    sLog.error ("Cannot apply preset value for ", entry.key (), ": ", e.what ());
	}
    }
}

ProjectUniquePtr WallpaperApplication::loadBackground (const std::string& bg) try {
    auto [container, json, preset, directory] = this->openProjectSource (bg);

    if (this->m_context.settings.screenshot.take) {
	this->m_nextFrameScreenshot = this->m_context.settings.screenshot.delay;

	if (this->m_videoDriver != nullptr) {
	    this->m_nextFrameScreenshot += this->m_videoDriver->getFrameCounter ();
	}

	this->m_screenShotTaken = false;
    }

    const std::string sceneFileName = json.contains ("file") && json.at ("file").is_string ()
	? json.at ("file").get<std::string> ()
	: std::string ();
    auto project = WallpaperEngine::Data::Parsers::ProjectParser::parse (json, std::move (container));
    std::error_code error;
    auto folder = std::filesystem::weakly_canonical (std::filesystem::absolute (directory), error);

    if (error || folder.empty ()) {
	folder = std::filesystem::absolute (directory).lexically_normal ();
    }

    if (!folder.has_filename ()) {
	folder = folder.parent_path ();
    }

    // WE opens scene.pkg when there is one
    project->sceneFile = std::filesystem::exists (folder / "scene.pkg") ? folder / "scene.pkg" : folder / sceneFileName;

    if (preset.has_value ()) {
	std::filesystem::path presetDir = bg;

	if (!presetDir.has_filename ()) {
	    presetDir = presetDir.parent_path ();
	}

	applyPreset (*project, *preset, presetDir);
    }

    return project;
} catch (const std::exception& e) {
    throw std::runtime_error ("Cannot load wallpaper '" + bg + "': " + e.what ());
}

std::vector<std::size_t>
WallpaperApplication::buildPlaylistOrder (const ApplicationContext::PlaylistDefinition& definition) {
    std::vector<std::size_t> order (definition.items.size ());
    std::iota (order.begin (), order.end (), 0);

    if (definition.settings.order == "random") {
	std::shuffle (
	    order.begin () + (definition.settings.hasIntro () && order.size () > 1 ? 1 : 0), order.end (),
	    this->m_playlistRng
	);
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
	state.introPlaying = playlist.settings.hasIntro () && playlist.items.size () > 1;

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
	const auto source = this->openProjectSource (path);
	const auto& json = source.json;
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

	const bool skippedIntro = candidateIndex == 0 && playlist.definition.settings.hasIntro ()
	    && playlist.order.size () > 1 && !playlist.introPlaying;

	if (!skippedIntro && !playlist.failedIndices.contains (candidateIndex)) {
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
    playlist.introPlaying = false;

    const bool intro = playlist.definition.settings.hasIntro () && playlist.order.size () > 1;

    if (playlist.orderIndex == 0) {
	playlist.failedIndices.clear ();

	if (playlist.definition.settings.order == "random") {
	    std::shuffle (playlist.order.begin () + (intro ? 1 : 0), playlist.order.end (), this->m_playlistRng);
	}

	if (intro) {
	    playlist.orderIndex = 1;
	}
    }

    std::size_t candidateOrderIndex = playlist.orderIndex;

    if (!this->selectNextCandidate (playlist, candidateOrderIndex)) {
	sLog.error ("All playlist items failed for ", screen, ", keeping current wallpaper");
	const uint32_t delayMinutes = std::max<uint32_t> (1, playlist.definition.settings.delayMinutes);
	playlist.nextSwitch = now + std::chrono::minutes (delayMinutes);
	playlist.failedIndices.clear ();
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
	    playlist.failedIndices.clear ();
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
	this->ensureAudioCapture (*project);

	// the outgoing project must outlive setWallpaper(), the old wallpaper keeps a reference into it
	auto outgoing = std::move (this->m_backgrounds[screen]);
	this->m_backgrounds[screen] = std::move (project);

	try {
	    if (this->m_renderContext) {
		this->installWallpaper (screen, *this->m_backgrounds[screen], nextPath, true);
		this->applyAudioPolicy ();
	    }
	} catch (...) {
	    project = std::move (this->m_backgrounds[screen]);
	    this->m_backgrounds[screen] = std::move (outgoing);

	    if (this->m_renderContext) {
		this->m_renderContext->pruneTextures ();
	    }

	    throw;
	}

	this->m_context.settings.general.screenBackgrounds[screen] = nextPath;
	loaded = true;
    } catch (const std::exception& e) {
	sLog.error ("Failed to advance playlist on ", screen, ": ", e.what ());
    } catch (...) {
	sLog.error ("Failed to advance playlist on ", screen, ": unknown error");
    }

    if (!loaded) {
	playlist.failedIndices.insert (playlist.order[playlist.orderIndex]);

	sLog.error ("Failed to load wallpaper for ", screen, ", skipping it until the playlist comes round again");
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

	// WE only writes a playlist of more than one item into its config (the UI's `1 < items.length`), a single
	// wallpaper is a plain selection that loops, so one item keeps looping here too
	if (playlist.definition.items.size () <= 1) {
	    continue;
	}

	// videosequence (sub_140076BE0, sub_140067720): a video of a timer playlist plays once and the playlist moves
	// on when it ends, the timer never takes it away. The playintro item is treated the same while it is the first
	// one shown
	if ((playlist.definition.settings.videoSequence || playlist.introPlaying) && this->m_renderContext) {
	    const auto& wallpapers = this->m_renderContext->getWallpapers ();
	    const auto current = wallpapers.find (screen);

	    if (current != wallpapers.end () && current->second->is<WallpaperEngine::Render::Wallpapers::CVideo> ()) {
		const auto video = current->second->as<WallpaperEngine::Render::Wallpapers::CVideo> ();

		video->setLoop (false);

		if (video->hasEnded ()) {
		    this->advancePlaylist (screen, playlist, now);
		}

		continue;
	    }
	}

	if (now < playlist.nextSwitch) {
	    continue;
	}

	this->advancePlaylist (screen, playlist, now);
    }
}

namespace {
struct HotswapRequest {
    std::optional<std::string> path;
    std::optional<std::string> screen;
    // a layers=1 line makes disabledObjects/enabledObjects a full replacement
    bool layersProvided = false;
    std::vector<std::string> disabledObjects;
    std::vector<std::string> enabledObjects;
    /** effects=1 seen, the effect lists replace the current ones */
    bool effectsProvided = false;
    std::vector<std::string> disabledEffects;
    std::vector<std::string> enabledEffects;
    std::optional<std::string> disableAnimations;
    bool propertiesProvided = false;
    std::map<std::string, std::string> properties;
    std::optional<int> volume;
    std::optional<int> fps;
    std::optional<std::string> batteryFps;
    std::optional<std::string> idlePause;
    std::optional<std::string> xray;
    std::optional<std::string> scaling;
    std::optional<std::string> zoom;
    std::optional<std::string> offset;
    std::optional<std::string> alignment;
    std::optional<std::string> disableParallax;
    std::optional<std::string> expandCanvas;
    std::optional<std::string> repairAssets;
    std::optional<std::string> cornerColor;
    ImageAdjustments imageAdjustments;
    bool imageAdjustmentsProvided = false;
    std::optional<std::string> speed;
    std::optional<std::string> videoStart;
    std::optional<std::string> videoEnd;
    std::optional<std::string> videoSegments;
    std::optional<std::string> hwdec;
    std::optional<std::string> hrtf;
    std::optional<std::string> videoSeek;
    std::optional<std::string> audioScreen;
    std::optional<std::string> ambientVolume;
    bool audioSensitivityProvided = false;
    std::map<std::string, std::string> audioSensitivity;
    bool soundVolumeProvided = false;
    std::map<std::string, std::string> soundVolume;
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
	    if (!request.path.has_value ()) {
		request.path = line;
	    }

	    continue;
	}

	const std::string key = trimHotswapToken (line.substr (0, separator));
	const std::string value = trimHotswapToken (line.substr (separator + 1));

	if (key == "path") {
	    request.path = value;
	} else if (key == "screen") {
	    request.screen = value.empty () ? std::nullopt : std::optional<std::string> (value);
	} else if (key == "layers") {
	    request.layersProvided = true;
	} else if (key == "disable-object") {
	    request.layersProvided = true;
	    request.disabledObjects.push_back (value);
	} else if (key == "enable-object") {
	    request.layersProvided = true;
	    request.enabledObjects.push_back (value);
	} else if (key == "effects") {
	    request.effectsProvided = true;
	} else if (key == "disable-effect") {
	    request.effectsProvided = true;
	    request.disabledEffects.push_back (value);
	} else if (key == "enable-effect") {
	    request.effectsProvided = true;
	    request.enabledEffects.push_back (value);
	} else if (key == "disable-animations") {
	    request.disableAnimations = value;
	} else if (key == "volume") {
	    try {
		request.volume = std::stoi (value);
	    } catch (const std::exception&) {
		sLog.error ("Hotswap: ignoring invalid volume value: ", value);
	    }
	} else if (key == "fps") {
	    try {
		request.fps = std::stoi (value);
	    } catch (const std::exception&) {
		sLog.error ("Hotswap: ignoring invalid fps value: ", value);
	    }
	} else if (key == "fps-battery") {
	    request.batteryFps = value;
	} else if (key == "idle-pause") {
	    request.idlePause = value;
	} else if (key == "xray") {
	    request.xray = value;
	} else if (key == "scaling") {
	    request.scaling = value;
	} else if (key == "zoom") {
	    request.zoom = value;
	} else if (key == "offset") {
	    request.offset = value;
	} else if (key == "alignment") {
	    request.alignment = value;
	} else if (key == "disable-parallax") {
	    request.disableParallax = value;
	} else if (key == "expand-canvas") {
	    request.expandCanvas = value;
	} else if (key == "repair-assets") {
	    request.repairAssets = value;
	} else if (key == "corner-color") {
	    request.cornerColor = value;
	} else if (key == "image-filter") {
	    request.imageAdjustments.filter = value == "none" ? "" : value;
	    request.imageAdjustmentsProvided = true;
	} else if (key == "color-options" || key == "flip") {
	    const bool enable = value == "on" || value == "1" || value == "true";

	    if (!enable && value != "off" && value != "0" && value != "false") {
		sLog.error ("Hotswap: ignoring invalid ", key, " value: ", value);
	    } else {
		(key == "flip" ? request.imageAdjustments.flipHorizontal : request.imageAdjustments.colorEnabled)
		    = enable;
		request.imageAdjustmentsProvided = true;
	    }
	} else if (
	    key == "image-filter-strength" || key == "brightness" || key == "contrast" || key == "saturation"
	    || key == "hue"
	) {
	    float amount;

	    try {
		amount = std::clamp (parseFiniteFloat (value), 0.0f, 100.0f);
	    } catch (const std::exception&) {
		sLog.error ("Hotswap: ignoring invalid ", key, " value: ", value);
		continue;
	    }

	    auto& adjustments = request.imageAdjustments;

	    if (key == "image-filter-strength") {
		adjustments.filterStrength = amount;
	    } else if (key == "brightness") {
		adjustments.brightness = amount;
	    } else if (key == "contrast") {
		adjustments.contrast = amount;
	    } else if (key == "saturation") {
		adjustments.saturation = amount;
	    } else {
		adjustments.hue = amount;
	    }

	    request.imageAdjustmentsProvided = true;
	} else if (key == "speed") {
	    request.speed = value;
	} else if (key == "video-start") {
	    request.videoStart = value;
	} else if (key == "video-end") {
	    request.videoEnd = value;
	} else if (key == "video-segments") {
	    request.videoSegments = value;
	} else if (key == "video-seek") {
	    request.videoSeek = value;
	} else if (key == "hwdec") {
	    request.hwdec = value;
	} else if (key == "hrtf") {
	    request.hrtf = value;
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
	} else if (key == "sound-volume") {
	    const auto volSeparator = value.find ('=');

	    if (volSeparator == std::string::npos) {
		sLog.error ("Hotswap: ignoring malformed sound-volume line: ", value);
	    } else {
		request.soundVolumeProvided = true;
		request.soundVolume[value.substr (0, volSeparator)] = value.substr (volSeparator + 1);
	    }
	} else {
	    sLog.error ("Hotswap: ignoring unknown control file key: ", key);
	}
    }

    return request;
}
}

void WallpaperApplication::checkHotswapRequest () {
    if (!this->m_hotswapRequested.exchange (false)) {
	return;
    }

    const char* runtimeDir = getenv ("XDG_RUNTIME_DIR");
    const bool sharedTmp
	= !this->m_context.settings.general.controlFile.has_value () && (runtimeDir == nullptr || *runtimeDir == '\0');
    const auto controlFile = this->m_context.settings.general.controlFile.value_or (
	sharedTmp ? std::filesystem::path ("/tmp/lwe-control-" + std::to_string (geteuid ()))
		  : std::filesystem::path (runtimeDir) / "lwe-control"
    );

    // /tmp is shared, a file another user left there is not a request
    if (sharedTmp && belongsToAnotherUser (controlFile, geteuid ())) {
	sLog.error ("Hotswap requested but the control file belongs to another user: ", controlFile.string ());
	return;
    }

    std::ifstream file (controlFile);

    if (!file.is_open ()) {
	sLog.error ("Hotswap requested but control file could not be read: ", controlFile.string ());
	return;
    }

    const auto request = parseHotswapRequest (file);
    HotswapTarget target;

    if (request.screen.has_value ()) {
	target = this->resolveHotswapScreen (*request.screen);

	if (!target.has_value ()) {
	    sLog.error ("Hotswap: no screen or slot called ", *request.screen);
	    return;
	}
    }

    if (!request.path.has_value () && !request.layersProvided && !request.volume.has_value ()
	&& !request.fps.has_value () && !request.batteryFps.has_value () && !request.idlePause.has_value ()
	&& !request.xray.has_value () && !request.scaling.has_value () && !request.zoom.has_value ()
	&& !request.offset.has_value () && !request.alignment.has_value () && !request.disableParallax.has_value ()
	&& !request.expandCanvas.has_value () && !request.repairAssets.has_value () && !request.cornerColor.has_value ()
	&& !request.imageAdjustmentsProvided && !request.speed.has_value () && !request.videoStart.has_value ()
	&& !request.videoEnd.has_value () && !request.videoSegments.has_value () && !request.videoSeek.has_value ()
	&& !request.audioScreen.has_value () && !request.ambientVolume.has_value () && !request.propertiesProvided
	&& !request.audioSensitivityProvided && !request.soundVolumeProvided && !request.effectsProvided
	&& !request.disableAnimations.has_value () && !request.hwdec.has_value () && !request.hrtf.has_value ()) {
	sLog.error ("Hotswap requested but control file was empty");
	return;
    }

    // before anything changes: a wallpaper that can't load leaves the running one with everything it had
    if (request.path.has_value () && !this->preflightWallpaper (*request.path)) {
	sLog.error ("Hotswap failed, invalid wallpaper at ", *request.path);
	return;
    }

    struct OverrideSnapshot {
	std::vector<std::string> disabledObjects, enabledObjects, disabledEffects, enabledEffects;
	std::map<std::string, std::string> properties;
	std::map<std::string, float> audioSensitivity, soundVolume;
	Data::Model::ImageAdjustments windowImageAdjustments;
	VideoPlayback::VideoSegments videoSegments;
	std::map<std::string, Data::Model::ImageAdjustments> screenImageAdjustments;
	std::map<std::string, VideoPlayback::VideoSegments> screenVideoSegments;
	std::vector<Data::Model::ImageAdjustments> spanImageAdjustments;
    };

    std::optional<OverrideSnapshot> previousOverrides;

    // overrides belong to the wallpaper they were set for (ids and generic property names collide), a new one only gets
    // what this request carries
    if (request.path.has_value ()) {
	auto& general = this->m_context.settings.general;

	previousOverrides = OverrideSnapshot {
	    .disabledObjects = general.disabledObjects,
	    .enabledObjects = general.enabledObjects,
	    .disabledEffects = general.disabledEffects,
	    .enabledEffects = general.enabledEffects,
	    .properties = general.properties,
	    .audioSensitivity = general.audioSensitivity,
	    .soundVolume = general.soundVolume,
	    .windowImageAdjustments = this->m_context.settings.render.window.imageAdjustments,
	    .videoSegments = this->m_context.settings.render.videoSegments,
	    .screenImageAdjustments = general.screenImageAdjustments,
	    .screenVideoSegments = general.screenVideoSegments,
	};

	for (const auto& spanGroup : general.spanGroups) {
	    previousOverrides->spanImageAdjustments.push_back (spanGroup.imageAdjustments);
	}

	general.disabledObjects.clear ();
	general.enabledObjects.clear ();
	general.disabledEffects.clear ();
	general.enabledEffects.clear ();
	general.properties.clear ();
	general.audioSensitivity.clear ();
	general.soundVolume.clear ();

	// per wallpaper like WE; an empty entry keeps the global parts off the new one
	if (target.has_value ()) {
	    if (auto* spanGroup = this->findSpanGroup (*target); spanGroup != nullptr) {
		spanGroup->imageAdjustments = {};
	    } else {
		general.screenImageAdjustments.erase (*target);
	    }

	    general.screenVideoSegments[*target] = {};
	} else {
	    this->m_context.settings.render.window.imageAdjustments = {};
	    this->m_context.settings.render.videoSegments.clear ();
	    general.screenImageAdjustments.clear ();
	    general.screenVideoSegments.clear ();

	    for (auto& spanGroup : general.spanGroups) {
		spanGroup.imageAdjustments = {};
	    }
	}
    }

    if (request.volume.has_value ()) {
	this->applyVolumeHotswap (*request.volume);
    }

    if (request.fps.has_value ()) {
	this->applyFpsHotswap (*request.fps);
    }

    if (request.batteryFps.has_value ()) {
	this->applyBatteryFpsHotswap (*request.batteryFps);
    }

    if (request.idlePause.has_value ()) {
	this->applyIdlePauseHotswap (*request.idlePause);
    }

    if (request.xray.has_value ()) {
	this->applyXrayHotswap (*request.xray);
    }

    if (request.scaling.has_value ()) {
	this->applyScalingHotswap (target, *request.scaling);
    }

    if (request.zoom.has_value ()) {
	this->applyZoomHotswap (target, *request.zoom);
    }

    if (request.offset.has_value ()) {
	this->applyOffsetHotswap (target, *request.offset);
    }

    if (request.alignment.has_value ()) {
	this->applyAlignmentHotswap (target, *request.alignment);
    }

    if (request.disableParallax.has_value ()) {
	this->applyParallaxHotswap (*request.disableParallax);
    }

    if (request.cornerColor.has_value ()) {
	this->applyCornerColorHotswap (target, *request.cornerColor);
    }

    if (request.imageAdjustmentsProvided) {
	this->applyImageAdjustmentsHotswap (target, request.imageAdjustments);
    }

    if (request.speed.has_value ()) {
	this->applySpeedHotswap (*request.speed);
    }

    if (request.videoStart.has_value () || request.videoEnd.has_value () || request.videoSegments.has_value ()) {
	this->applyVideoSegmentsHotswap (target, request.videoStart, request.videoEnd, request.videoSegments);
    }

    if (request.videoSeek.has_value ()) {
	this->applyVideoSeekHotswap (target, *request.videoSeek);
    }

    if (request.hwdec.has_value ()) {
	this->applyHwdecHotswap (*request.hwdec);
    }

    if (request.hrtf.has_value ()) {
	if (const auto mode = ApplicationContext::parseHrtfMode (*request.hrtf); mode.has_value ()) {
	    this->m_context.settings.audio.hrtf = *mode;
	} else {
	    sLog.error ("Hotswap: ignoring invalid hrtf value: ", *request.hrtf);
	}
    }

    if (request.audioScreen.has_value ()) {
	this->applyAudioScreenHotswap (*request.audioScreen);
    }

    if (request.ambientVolume.has_value ()) {
	this->applyAmbientVolumeHotswap (*request.ambientVolume);
    }

    if (request.soundVolumeProvided) {
	this->applySoundVolumeHotswap (request.soundVolume);
    }

    bool sceneSettingsChanged = false;

    if (request.expandCanvas.has_value ()) {
	const auto& value = *request.expandCanvas;
	const bool enable = value == "on" || value == "1" || value == "true";

	if (!enable && value != "off" && value != "0" && value != "false") {
	    sLog.error ("Hotswap: ignoring invalid expand-canvas value: ", value);
	} else {
	    sceneSettingsChanged = this->m_context.settings.general.expandCanvas != enable;
	    this->m_context.settings.general.expandCanvas = enable;
	}
    }

    if (request.repairAssets.has_value ()) {
	const auto& value = *request.repairAssets;
	const bool enable = value == "on" || value == "1" || value == "true";

	if (!enable && value != "off" && value != "0" && value != "false") {
	    sLog.error ("Hotswap: ignoring invalid repair-assets value: ", value);
	} else {
	    sceneSettingsChanged |= this->m_context.settings.general.repairAssets != enable;
	    this->m_context.settings.general.repairAssets = enable;
	}
    }

    if (request.layersProvided) {
	this->m_context.settings.general.disabledObjects = request.disabledObjects;
	this->m_context.settings.general.enabledObjects = request.enabledObjects;
    }

    if (request.effectsProvided) {
	this->m_context.settings.general.disabledEffects = request.disabledEffects;
	this->m_context.settings.general.enabledEffects = request.enabledEffects;
    }

    if (request.disableAnimations.has_value ()) {
	const auto& value = *request.disableAnimations;
	const bool freeze = value == "on" || value == "1" || value == "true";

	if (!freeze && value != "off" && value != "0" && value != "false") {
	    sLog.error ("Hotswap: ignoring invalid disable-animations value: ", value);
	} else {
	    this->m_context.settings.render.freezeAnimations = freeze;
	}
    }

    std::vector<std::string> changedProperties;

    if (request.propertiesProvided) {
	auto& properties = this->m_context.settings.general.properties;

	for (const auto& [name, value] : request.properties) {
	    if (const auto previous = properties.find (name);
		previous == properties.end () || previous->second != value) {
		changedProperties.push_back (name);
	    }

	    properties[name] = value;
	}
    }

    if (request.audioSensitivityProvided) {
	for (const auto& [target, value] : request.audioSensitivity) {
	    try {
		this->m_context.settings.general.audioSensitivity[target] = parseFiniteFloat (value);
	    } catch (const std::exception&) {
		sLog.error ("Hotswap: ignoring invalid audio-sensitivity value: ", value);
	    }
	}
    }

    if (request.propertiesProvided && !request.path.has_value () && !request.layersProvided && !request.effectsProvided
	&& !request.audioSensitivityProvided && !sceneSettingsChanged
	&& this->applyPropertiesLive (changedProperties)) {
	return;
    }

    // volume/xray/speed are live setters; layers, effects, properties, audio sensitivity, expanded canvas and repaired
    // assets are baked in at load
    if (!request.path.has_value () && !request.layersProvided && !request.effectsProvided && !request.propertiesProvided
	&& !request.audioSensitivityProvided && !sceneSettingsChanged) {
	return;
    }

    // the request's own settings are applied by now, a failure from here on puts the old ones back
    const auto restoreOverrides = [&] {
	if (!previousOverrides.has_value ()) {
	    return;
	}

	auto& general = this->m_context.settings.general;

	general.disabledObjects = previousOverrides->disabledObjects;
	general.enabledObjects = previousOverrides->enabledObjects;
	general.disabledEffects = previousOverrides->disabledEffects;
	general.enabledEffects = previousOverrides->enabledEffects;
	general.properties = previousOverrides->properties;
	general.audioSensitivity = previousOverrides->audioSensitivity;
	general.soundVolume = previousOverrides->soundVolume;
	this->m_context.settings.render.window.imageAdjustments = previousOverrides->windowImageAdjustments;
	this->m_context.settings.render.videoSegments = previousOverrides->videoSegments;
	general.screenImageAdjustments = previousOverrides->screenImageAdjustments;
	general.screenVideoSegments = previousOverrides->screenVideoSegments;

	for (size_t index = 0;
	     index < general.spanGroups.size () && index < previousOverrides->spanImageAdjustments.size (); index++) {
	    general.spanGroups[index].imageAdjustments = previousOverrides->spanImageAdjustments[index];
	}
    };

    if (!this->makeAnyViewportCurrent ()) {
	sLog.error ("Hotswap failed, no active viewport");
	restoreOverrides ();
	return;
    }

    if (request.path.has_value ()) {
	sLog.out ("Hotswapping wallpaper to ", *request.path);
    } else {
	sLog.out ("Hotswapping wallpaper layers");
    }

    bool swapped = false;

    // the outgoing project must outlive setWallpaper(), the old wallpaper keeps a reference into it
    for (auto& [screen, background] : this->m_backgrounds) {
	if (!this->hotswapReaches (target, screen)) {
	    continue;
	}

	const std::string targetPath = request.path.value_or (this->resolveScreenBackgroundPath (screen));

	try {
	    auto project = this->loadBackground (targetPath);

	    this->setupPropertiesForProject (*project);
	    this->setupAudioSensitivityForProject (*project);
	    this->setupSoundVolumeForProject (*project);
	    this->ensureAudioCapture (*project);

	    auto outgoing = std::move (background);
	    background = std::move (project);

	    try {
		if (this->m_renderContext) {
		    this->installWallpaper (screen, *background, targetPath, true);
		}
	    } catch (...) {
		project = std::move (background);
		background = std::move (outgoing);

		if (this->m_renderContext) {
		    this->m_renderContext->pruneTextures ();
		}

		throw;
	    }

	    swapped = true;

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
	} catch (...) {
	    sLog.error ("Hotswap failed for screen ", screen, ": unknown error");
	}
    }

    if (!swapped) {
	restoreOverrides ();
    }

    this->applyAudioPolicy ();

    if (swapped && request.path.has_value () && !request.screen.has_value ()) {
	this->m_context.settings.general.defaultBackground = *request.path;
    }
}

std::optional<std::string> WallpaperApplication::resolveHotswapScreen (const std::string& name) const {
    if (this->m_backgrounds.contains (name)) {
	return name;
    }

    for (const auto& spanGroup : this->m_context.settings.general.spanGroups) {
	if (std::ranges::find (spanGroup.screens, name) != spanGroup.screens.end ()) {
	    return "span:" + spanGroup.screens.front ();
	}
    }

    return std::nullopt;
}

bool WallpaperApplication::hotswapReaches (const HotswapTarget& target, const std::string& screen) const {
    if (!target.has_value () || *target == screen) {
	return true;
    }

    const auto* spanGroup = this->findSpanGroup (*target);

    return spanGroup != nullptr && std::ranges::find (spanGroup->screens, screen) != spanGroup->screens.end ();
}

template <typename T, typename Update>
void WallpaperApplication::updateScreenSetting (
    const HotswapTarget& target, std::map<std::string, T>& screens, T& window,
    T ApplicationContext::SpanGroup::* spanField, Update update
) {
    if (target.has_value ()) {
	if (auto* spanGroup = this->findSpanGroup (*target); spanGroup != nullptr) {
	    spanGroup->*spanField = update (spanGroup->*spanField);
	} else {
	    auto& value = screens[*target];
	    value = update (value);
	}

	return;
    }

    window = update (window);

    for (auto& value : screens | std::views::values) {
	value = update (value);
    }

    for (auto& spanGroup : this->m_context.settings.general.spanGroups) {
	spanGroup.*spanField = update (spanGroup.*spanField);
    }
}

template <typename Apply>
void WallpaperApplication::forEachTargetWallpaper (const HotswapTarget& target, Apply apply) const {
    if (!this->m_renderContext) {
	return;
    }

    std::set<WallpaperEngine::Render::CWallpaper*> done;

    for (const auto& [screen, wallpaper] : this->m_renderContext->getWallpapers ()) {
	if (this->hotswapReaches (target, screen) && done.insert (wallpaper.get ()).second) {
	    apply (screen, *wallpaper);
	}
    }
}

std::string WallpaperApplication::storageScreen (const std::string& screen) const {
    // WE: nothing in a window, "Monitor<index>" on the desktop (sub_14012E710); output names survive reordering
    const auto mode = this->m_context.settings.render.mode;
    return mode == ApplicationContext::NORMAL_WINDOW || mode == ApplicationContext::EXPLICIT_WINDOW ? "" : screen;
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

glm::vec2 WallpaperApplication::resolveScreenOffset (const std::string& screen) const {
    const auto it = this->m_context.settings.general.screenOffsets.find (screen);

    return it != this->m_context.settings.general.screenOffsets.end () ? it->second
								       : this->m_context.settings.render.window.offset;
}

WallpaperEngine::Render::WallpaperState::Alignment
WallpaperApplication::resolveScreenAlignment (const std::string& screen) const {
    const auto it = this->m_context.settings.general.screenAlignments.find (screen);

    return it != this->m_context.settings.general.screenAlignments.end ()
	? it->second
	: this->m_context.settings.render.window.alignment;
}

glm::vec4 WallpaperApplication::resolveScreenCornerColor (const std::string& screen) const {
    const auto it = this->m_context.settings.general.screenCornerColors.find (screen);

    return it != this->m_context.settings.general.screenCornerColors.end ()
	? it->second
	: this->m_context.settings.render.window.cornerColor;
}

ImageAdjustments
WallpaperApplication::resolveScreenImageAdjustments (const std::string& screen, const Project& project) const {
    const auto fallback = this->m_context.settings.render.window.imageAdjustments.over (project.imageAdjustments);

    for (const auto& spanGroup : this->m_context.settings.general.spanGroups) {
	if (spanGroup.screens.empty ()) {
	    continue;
	}

	if ("span:" + spanGroup.screens.front () == screen
	    || std::find (spanGroup.screens.begin (), spanGroup.screens.end (), screen) != spanGroup.screens.end ()) {
	    return spanGroup.imageAdjustments.over (fallback);
	}
    }

    const auto it = this->m_context.settings.general.screenImageAdjustments.find (screen);

    return it != this->m_context.settings.general.screenImageAdjustments.end () ? it->second.over (fallback) : fallback;
}

glm::ivec2 WallpaperApplication::resolveScreenRenderSize (const std::string& screen) const {
    const auto& viewports = this->m_renderContext->getOutput ().getViewports ();
    const auto* split = this->m_context.findSplitScreen (screen);
    const auto it = viewports.find (split != nullptr ? split->screen : screen);

    if (it != viewports.end ()) {
	return { it->second->viewport.z, it->second->viewport.w };
    }

    return { this->m_renderContext->getOutput ().getFullWidth (),
	     this->m_renderContext->getOutput ().getFullHeight () };
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

    this->applyAudioPolicy ();

    sLog.out ("Hotswap: applied volume ", volume, " live");
}

void WallpaperApplication::applyFpsHotswap (int fps) {
    fps = std::max (1, fps);

    this->m_context.settings.render.maximumFPS = fps;

    sLog.out ("Hotswap: applied fps limit ", fps, " live");
}

void WallpaperApplication::applyBatteryFpsHotswap (const std::string& value) {
    auto& batteryFPS = this->m_context.settings.render.batteryFPS;

    if (value == "off" || value == "none" || value.empty ()) {
	batteryFPS = std::nullopt;
    } else if (value == "pause") {
	batteryFPS = 0;
    } else {
	try {
	    batteryFPS = std::max (0, std::stoi (value));
	} catch (const std::exception&) {
	    sLog.error ("Hotswap: ignoring invalid fps-battery value: ", value);
	    return;
	}
    }

    this->updatePowerMonitor ();

    sLog.out ("Hotswap: applied fps-battery ", batteryFPS.has_value () ? std::to_string (*batteryFPS) : "off", " live");
}

void WallpaperApplication::applyIdlePauseHotswap (const std::string& value) {
    int minutes = 0;

    if (value != "off" && value != "none" && !value.empty ()) {
	try {
	    minutes = std::max (0, std::stoi (value));
	} catch (const std::exception&) {
	    sLog.error ("Hotswap: ignoring invalid idle-pause value: ", value);
	    return;
	}
    }

    this->m_context.settings.render.idlePauseMinutes = minutes;
    this->updatePowerMonitor ();

    sLog.out ("Hotswap: applied idle-pause ", minutes, " live");
}

void WallpaperApplication::updatePowerMonitor () {
    const auto& render = this->m_context.settings.render;

    if (!render.batteryFPS.has_value () && render.idlePauseMinutes <= 0) {
	this->m_powerMonitor.reset ();
	return;
    }

    if (!this->m_powerMonitor) {
	this->m_powerMonitor = std::make_unique<WallpaperEngine::Desktop::PowerMonitor> ();
	sLog.out ("Power: ", this->m_powerMonitor->onBattery () ? "running on battery" : "on external power");
    }

    this->m_powerMonitor->setIdleTimeout (render.idlePauseMinutes);
}

int WallpaperApplication::frameRateLimit () const {
    const auto& render = this->m_context.settings.render;

    if (render.batteryFPS.value_or (0) > 0 && this->m_powerMonitor && this->m_powerMonitor->onBattery ()) {
	return std::min (*render.batteryFPS, render.maximumFPS);
    }

    return render.maximumFPS;
}

uint32_t WallpaperApplication::pauseReasons () const {
    const auto& render = this->m_context.settings.render;
    uint32_t reasons = 0;

    if (this->m_fullScreenDetector->anythingFullscreen ()) {
	reasons |= PauseWindows;
    }

    if (this->m_powerMonitor) {
	if (render.batteryFPS == 0 && this->m_powerMonitor->onBattery ()) {
	    reasons |= PauseBattery;
	}

	if (render.idlePauseMinutes > 0 && this->m_powerMonitor->idle ()) {
	    reasons |= PauseIdle;
	}
    }

    return reasons;
}

void WallpaperApplication::logPauseReasons (uint32_t reasons) {
    if (reasons == this->m_pauseReasons) {
	return;
    }

    this->m_pauseReasons = reasons;

    if (reasons == 0) {
	sLog.out ("Resumed");
	return;
    }

    std::string names;

    for (const auto& [bit, name] : { std::pair { PauseWindows, "windows" }, std::pair { PauseBattery, "battery" },
				     std::pair { PauseIdle, "idle" } }) {
	if (reasons & bit) {
	    names += names.empty () ? name : std::string (", ") + name;
	}
    }

    sLog.out ("Paused: ", names);
}

void WallpaperApplication::applyXrayHotswap (const std::string& value) {
    auto& mode = this->m_context.state.xray.mode;

    // on/off/toggle predate "disabled" and only switch between full and normal
    if (value == "toggle") {
	mode = mode == XrayMode::Full ? XrayMode::Normal : XrayMode::Full;
    } else if (value == "full" || value == "on" || value == "1" || value == "true") {
	mode = XrayMode::Full;
    } else if (value == "normal" || value == "off" || value == "0" || value == "false") {
	mode = XrayMode::Normal;
    } else if (value == "disabled") {
	mode = XrayMode::Disabled;
    } else {
	sLog.error ("Hotswap: ignoring invalid xray value: ", value);
	return;
    }

    const char* name = mode == XrayMode::Full ? "full" : mode == XrayMode::Disabled ? "disabled" : "normal";
    sLog.out ("Hotswap: xray ", name, " live");
}

void WallpaperApplication::applyScalingHotswap (const HotswapTarget& target, const std::string& value) {
    const auto mode = WallpaperEngine::Render::WallpaperState::parseScalingMode (value);

    if (!mode.has_value ()) {
	sLog.error ("Hotswap: ignoring invalid scaling value: ", value);
	return;
    }

    this->updateScreenSetting (
	target, this->m_context.settings.general.screenScalings, this->m_context.settings.render.window.scalingMode,
	&ApplicationContext::SpanGroup::scaling, [&] (const auto&) { return *mode; }
    );
    this->forEachTargetWallpaper (target, [&] (const std::string&, WallpaperEngine::Render::CWallpaper& wallpaper) {
	wallpaper.setScalingMode (*mode);
    });

    sLog.out ("Hotswap: applied scaling mode ", value, " live");
}

void WallpaperApplication::applyZoomHotswap (const HotswapTarget& target, const std::string& value) {
    float zoom;

    try {
	zoom = parseFiniteFloat (value);
    } catch (const std::exception&) {
	sLog.error ("Hotswap: ignoring invalid zoom value: ", value);
	return;
    }

    this->updateScreenSetting (
	target, this->m_context.settings.general.screenZooms, this->m_context.settings.render.window.zoom,
	&ApplicationContext::SpanGroup::zoom, [&] (const auto&) { return zoom; }
    );
    this->forEachTargetWallpaper (target, [&] (const std::string&, WallpaperEngine::Render::CWallpaper& wallpaper) {
	wallpaper.setZoom (zoom);
    });

    sLog.out ("Hotswap: applied zoom ", zoom, " live");
}

void WallpaperApplication::applyOffsetHotswap (const HotswapTarget& target, const std::string& value) {
    const auto comma = value.find (',');

    if (comma == std::string::npos) {
	sLog.error ("Hotswap: ignoring malformed offset value (expected X,Y): ", value);
	return;
    }

    glm::vec2 offset;

    try {
	offset.x = parseFiniteFloat (value.substr (0, comma));
	offset.y = parseFiniteFloat (value.substr (comma + 1));
    } catch (const std::exception&) {
	sLog.error ("Hotswap: ignoring invalid offset value: ", value);
	return;
    }

    this->updateScreenSetting (
	target, this->m_context.settings.general.screenOffsets, this->m_context.settings.render.window.offset,
	&ApplicationContext::SpanGroup::offset, [&] (const auto&) { return offset; }
    );
    this->forEachTargetWallpaper (target, [&] (const std::string&, WallpaperEngine::Render::CWallpaper& wallpaper) {
	wallpaper.setOffset (offset.x, offset.y);
    });

    sLog.out ("Hotswap: applied offset ", offset.x, ",", offset.y, " live");
}

void WallpaperApplication::applyAlignmentHotswap (const HotswapTarget& target, const std::string& value) {
    std::array<float, 4> values {};
    std::stringstream stream (value);
    std::string part;
    size_t count = 0;

    try {
	while (count < values.size () && std::getline (stream, part, ',')) {
	    values[count++] = parseFiniteFloat (part);
	}
    } catch (const std::exception&) {
	count = 0;
    }

    if (count != values.size ()) {
	sLog.error ("Hotswap: ignoring malformed alignment value (expected position,x,y,zoom): ", value);
	return;
    }

    const WallpaperEngine::Render::WallpaperState::Alignment alignment {
	.position = std::clamp (values[0], 0.0f, 100.0f) / 100.0f,
	.x = std::clamp (values[1], 0.0f, 100.0f) / 100.0f,
	.y = std::clamp (values[2], 0.0f, 100.0f) / 100.0f,
	.zoom = std::clamp (values[3], 0.0f, 200.0f) / 100.0f,
    };

    this->updateScreenSetting (
	target, this->m_context.settings.general.screenAlignments, this->m_context.settings.render.window.alignment,
	&ApplicationContext::SpanGroup::alignment, [&] (const auto&) { return alignment; }
    );
    this->forEachTargetWallpaper (target, [&] (const std::string&, WallpaperEngine::Render::CWallpaper& wallpaper) {
	wallpaper.setAlignment (alignment);
    });

    sLog.out ("Hotswap: applied alignment ", value, " live");
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

void WallpaperApplication::applyCornerColorHotswap (const HotswapTarget& target, const std::string& value) {
    const auto color = WallpaperEngine::Render::CFBO::parseColor (value);

    if (!color.has_value ()) {
	sLog.error ("Hotswap: ignoring invalid corner color: ", value);
	return;
    }

    this->updateScreenSetting (
	target, this->m_context.settings.general.screenCornerColors, this->m_context.settings.render.window.cornerColor,
	&ApplicationContext::SpanGroup::cornerColor, [&] (const auto&) { return *color; }
    );
    this->forEachTargetWallpaper (target, [&] (const std::string&, WallpaperEngine::Render::CWallpaper& wallpaper) {
	wallpaper.setCornerColor (*color);
    });

    sLog.out ("Hotswap: applied corner color ", value, " live");
}

void WallpaperApplication::applyImageAdjustmentsHotswap (const HotswapTarget& target, const ImageAdjustments& request) {
    this->updateScreenSetting (
	target, this->m_context.settings.general.screenImageAdjustments,
	this->m_context.settings.render.window.imageAdjustments, &ApplicationContext::SpanGroup::imageAdjustments,
	[&] (const ImageAdjustments& adjustments) { return request.over (adjustments); }
    );
    this->forEachTargetWallpaper (
	target, [&] (const std::string& screen, WallpaperEngine::Render::CWallpaper& wallpaper) {
	    wallpaper.setImageAdjustments (
		this->resolveScreenImageAdjustments (screen, wallpaper.getWallpaperData ().project)
	    );
	}
    );

    sLog.out ("Hotswap: applied image filter and color options live");
}

void WallpaperApplication::applySpeedHotswap (const std::string& value) {
    float speed;

    try {
	speed = parseFiniteFloat (value);
    } catch (const std::exception&) {
	sLog.error ("Hotswap: ignoring invalid speed value: ", value);
	return;
    }

    if (speed <= 0.0f) {
	sLog.error ("Hotswap: ignoring non-positive speed value: ", value);
	return;
    }

    this->m_context.settings.render.playbackSpeed = speed;

    // video wallpapers run on mpv's clock and need the speed pushed, everything else reads it every frame
    if (this->m_renderContext) {
	for (const auto& [screen, wallpaper] : this->m_renderContext->getWallpapers ()) {
	    if (wallpaper->is<WallpaperEngine::Render::Wallpapers::CVideo> ()) {
		wallpaper->as<WallpaperEngine::Render::Wallpapers::CVideo> ()->setSpeed (speed);
	    }
	}
    }

    sLog.out ("Hotswap: applied speed ", speed, " live");
}

void WallpaperApplication::applyVideoSegmentsHotswap (
    const HotswapTarget& target, const std::optional<std::string>& start, const std::optional<std::string>& end,
    const std::optional<std::string>& segments
) {
    using WallpaperEngine::Render::Wallpapers::CVideo;
    auto& general = this->m_context.settings.general;
    WallpaperEngine::VideoPlayback::VideoSegments current = target.has_value ()
	? this->resolveScreenVideoSegments (*target)
	: this->m_context.settings.render.videoSegments;
    const auto clears = [] (const std::string& value) { return value.empty () || value == "none"; };

    if (segments.has_value ()) {
	if (clears (*segments)) {
	    current.clear ();
	} else if (const auto parsed = CVideo::parseSegments (*segments)) {
	    current = *parsed;
	} else {
	    sLog.error ("Hotswap: ignoring invalid video-segments value: ", *segments);
	}
    }

    if (start.has_value () || end.has_value ()) {
	WallpaperEngine::VideoPlayback::VideoSegment segment;

	if (!current.empty ()) {
	    segment = { current.front ().start, current.back ().end };
	}

	if (start.has_value ()) {
	    if (clears (*start)) {
		segment.start = 0.0;
	    } else if (const auto seconds = CVideo::parseTime (*start)) {
		segment.start = *seconds;
	    } else {
		sLog.error ("Hotswap: ignoring invalid video-start value: ", *start);
	    }
	}

	if (end.has_value ()) {
	    if (clears (*end)) {
		segment.end.reset ();
	    } else if (const auto seconds = CVideo::parseTime (*end)) {
		segment.end = seconds;
	    } else {
		sLog.error ("Hotswap: ignoring invalid video-end value: ", *end);
	    }
	}

	current = { segment };
    }

    current = CVideo::normalizeSegments (current);

    if (target.has_value ()) {
	general.screenVideoSegments[*target] = current;
    } else {
	this->m_context.settings.render.videoSegments = current;
	general.screenVideoSegments.clear ();
    }

    this->forEachTargetWallpaper (target, [&] (const std::string&, WallpaperEngine::Render::CWallpaper& wallpaper) {
	if (wallpaper.is<CVideo> ()) {
	    wallpaper.as<CVideo> ()->setSegments (current);
	}
    });

    std::string description = current.empty () ? "whole video" : "";

    for (const auto& segment : current) {
	description += (description.empty () ? "" : ", ") + std::to_string (segment.start) + " - "
	    + (segment.end.has_value () ? std::to_string (*segment.end) : "end");
    }

    sLog.out ("Hotswap: applied video parts ", description, " live");
}

WallpaperEngine::VideoPlayback::VideoSegments
WallpaperApplication::resolveScreenVideoSegments (const std::string& screen) const {
    const auto& screens = this->m_context.settings.general.screenVideoSegments;
    const auto it = screens.find (screen);

    return it != screens.end () ? it->second : this->m_context.settings.render.videoSegments;
}

void WallpaperApplication::applyVideoSeekHotswap (const HotswapTarget& target, const std::string& value) {
    const auto seconds = WallpaperEngine::Render::Wallpapers::CVideo::parseTime (value);

    if (!seconds.has_value ()) {
	sLog.error ("Hotswap: ignoring invalid video-seek value: ", value);
	return;
    }

    this->forEachTargetWallpaper (target, [&] (const std::string&, WallpaperEngine::Render::CWallpaper& wallpaper) {
	if (wallpaper.is<WallpaperEngine::Render::Wallpapers::CVideo> ()) {
	    wallpaper.as<WallpaperEngine::Render::Wallpapers::CVideo> ()->seek (*seconds);
	}
    });
}

void WallpaperApplication::applyHwdecHotswap (const std::string& value) {
    if (!WallpaperEngine::VideoPlayback::MPV::GLPlayer::isValidHwdec (value)) {
	sLog.error ("Hotswap: ignoring invalid hwdec value: ", value);
	return;
    }

    if (this->m_context.settings.render.hwdec == value) {
	return;
    }

    this->m_context.settings.render.hwdec = value;
    WallpaperEngine::VideoPlayback::MPV::GLPlayer::setHwdecForAll (value);

    sLog.out ("Hotswap: applied hwdec ", value, " live");
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
	bool screenMuted = audioScreen.has_value () && screen != *audioScreen;

	if (const auto* split = this->m_context.findSplitScreen (screen); split != nullptr) {
	    const bool named
		= audioScreen.has_value () && std::ranges::find (split->slots, *audioScreen) != split->slots.end ();
	    const std::string& audible = named ? *audioScreen : split->slots.front ();

	    screenMuted = screen != audible || (audioScreen.has_value () && !named && *audioScreen != split->screen);
	}
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

void WallpaperApplication::listEffectsForProject (const std::string& background, const Project& project) const {
    if (!project.wallpaper->is<Scene> ()) {
	return;
    }

    const auto scene = project.wallpaper->as<Scene> ();

    sLog.out ("Effects for ", background, ":");

    for (const auto& object : scene->objects) {
	if (!object->is<Image> ()) {
	    continue;
	}

	const auto* image = object->as<Image> ();

	for (const auto& effect : image->effects) {
	    sLog.out (
		"  ", effect->id, " - ", effect->name, " (object=", object->id, " objectName=\"", object->name,
		"\" group=\"", effect->effect->group, "\")"
	    );
	}
    }
}

void WallpaperApplication::listEffects () const {
    if (!this->m_context.settings.general.onlyListEffects) {
	return;
    }

    for (const auto& [background, info] : this->m_backgrounds) {
	this->listEffectsForProject (background, *info);
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
}

void WallpaperApplication::listAudioObjectsForProject (const std::string& background, const Project& project) const {
    if (!project.wallpaper->is<Scene> ()) {
	return;
    }

    const auto scene = project.wallpaper->as<Scene> ();

    sLog.out ("Audio-reactive objects for ", background, ":");

    for (const auto& object : scene->objects) {
	for (const auto& reactive : collectAudioReactiveProperties (*object)) {
	    sLog.out (
		"  ", object->id, " - ", object->name, " (", reactive.name,
		"): minvalue=", readScriptPropertyFloat (*reactive.value, "minvalue"),
		" maxvalue=", readScriptPropertyFloat (*reactive.value, "maxvalue"),
		" frequency=", readScriptPropertyFloat (*reactive.value, "frequency"),
		" smoothing=", readScriptPropertyFloat (*reactive.value, "smoothing")
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

void WallpaperApplication::setupSoundVolumeForProject (const Project& project) const {
    if (!project.wallpaper->is<Scene> ()) {
	return;
    }

    const auto scene = project.wallpaper->as<Scene> ();

    for (const auto& object : scene->objects) {
	if (!object->is<Sound> ()) {
	    continue;
	}

	const auto* sound = object->as<Sound> ();
	const auto volume = this->m_context.resolveSoundVolume (object->id, object->name);

	if (!volume.has_value () || !sound->volume || !sound->volume->value) {
	    continue;
	}

	sound->volume->value->update (std::clamp (volume.value (), 0.0f, 1.0f), DynamicValue::UpdateSource::User);

	sLog.debug ("Applying sound volume ", volume.value (), " to ", object->id, " - ", object->name);
    }
}

void WallpaperApplication::setupSoundVolume () {
    for (const auto& [background, info] : this->m_backgrounds) {
	this->setupSoundVolumeForProject (*info);
    }
}

void WallpaperApplication::applySoundVolumeHotswap (const std::map<std::string, std::string>& targets) {
    for (const auto& [target, value] : targets) {
	try {
	    this->m_context.settings.general.soundVolume[target] = parseFiniteFloat (value);
	} catch (const std::exception&) {
	    sLog.error ("Hotswap: ignoring invalid sound-volume value: ", value);
	}
    }

    for (const auto& [background, info] : this->m_backgrounds) {
	this->setupSoundVolumeForProject (*info);
    }

    sLog.out ("Hotswap: applied sound volume live");
}

bool WallpaperApplication::applyPropertiesLive (const std::vector<std::string>& names) {
    using WallpaperEngine::Render::Wallpapers::CScene;
    using WallpaperEngine::Render::Wallpapers::CWeb;

    const auto wallpaperOf = [this] (const std::string& screen) -> WallpaperEngine::Render::CWallpaper* {
	if (!this->m_renderContext) {
	    return nullptr;
	}

	const auto& wallpapers = this->m_renderContext->getWallpapers ();
	const auto it = wallpapers.find (screen);
	return it != wallpapers.end () ? it->second.get () : nullptr;
    };

    for (const auto& name : names) {
	for (const auto& [screen, info] : this->m_backgrounds) {
	    const auto property = info->properties.find (name);

	    if (property == info->properties.end ()) {
		continue;
	    }

	    static const bool force = std::getenv ("LWE_FORCE_LIVE_PROPERTIES") != nullptr;
	    const auto* wallpaper = wallpaperOf (screen);

	    const bool scene = info->wallpaper->is<Scene> () && wallpaper != nullptr && wallpaper->is<CScene> ();
	    const bool web = info->wallpaper->is<Web> () && wallpaper != nullptr
		&& (wallpaper->is<CWeb> () || wallpaper->is<WallpaperEngine::Render::Wallpapers::CSplat> ());

	    if (!(scene || web) || (!force && !property->second->appliesLive ())) {
		return false;
	    }
	}
    }

    for (const auto& name : names) {
	const auto& value = this->m_context.settings.general.properties.at (name);

	for (const auto& [background, info] : this->m_backgrounds) {
	    if (const auto property = info->properties.find (name); property != info->properties.end ()) {
		try {
		    property->second->update (value, DynamicValue::UpdateSource::User);
		} catch (const std::exception& e) {
		    sLog.error ("Hotswap: ignoring invalid value for property ", name, ": ", e.what ());
		}
	    }
	}

	sLog.out ("Hotswap: applied property ", name, " live");
    }

    if (this->m_renderContext && !names.empty ()) {
	for (const auto& wallpaper : this->m_renderContext->getWallpapers () | std::views::values) {
	    if (wallpaper->is<CScene> ()) {
		wallpaper->as<CScene> ()->getScriptEngine ().notifyUserPropertiesChanged (names);
	    } else if (wallpaper->is<CWeb> ()) {
		wallpaper->as<CWeb> ()->setPropertyOverrides (this->m_context.settings.general.properties);
	    }
	}
    }

    return true;
}

void WallpaperApplication::setupBrowser () {
    // CEF allows one CefInitialize/CefShutdown per process, so only the --web-host process hosts it
    if (!this->m_context.settings.general.webHost || this->m_browserContext) {
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
    // frames come when the main process asks, so they line up with what it presents
    windowInfo.external_begin_frame_enabled = true;
    // dmabuf frames: the browser paints into GPU buffers (OnAcceleratedPaint) the main process imports, instead of
    // reading them back (OnPaint)
    const bool dmabufFrames = this->m_context.settings.general.webHostFrames == "dmabuf"
	&& this->m_context.settings.general.webHostSocket >= 0;
    windowInfo.shared_texture_enabled = dmabufFrames;

    CefBrowserSettings browserSettings;
    // only a ceiling while external begin frames are on, the main process decides when a frame is produced
    browserSettings.windowless_frame_rate = std::max (60, this->m_context.settings.render.maximumFPS);

    const CefRefPtr<CEF::SharedMemoryRenderHandler> renderHandler
	= new CEF::SharedMemoryRenderHandler (shm, dmabufFrames ? this->m_context.settings.general.webHostSocket : -1);
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

    // a windowless browser is treated as backgrounded by default, which freezes the page's own timers and rAF
    browser->GetHost ()->WasHidden (false);

    shm->helperReady.store (true, std::memory_order_release);

    Input::MouseClickStatus lastLeft = Input::Released;
    Input::MouseClickStatus lastRight = Input::Released;
    uint32_t lastFrameRequest = shm->frameRequestSeq.load (std::memory_order_acquire);
    auto lastBeginFrame = std::chrono::steady_clock::now ();
    int lastMouseX = -1;
    int lastMouseY = -1;
    uint32_t lastDesiredWidth = shm->desiredWidth.load (std::memory_order_relaxed);
    uint32_t lastDesiredHeight = shm->desiredHeight.load (std::memory_order_relaxed);
    bool lastAudioMuted = shm->audioMuted.load (std::memory_order_relaxed);
    bool resizeRequested = false;
    browser->GetHost ()->SetAudioMuted (lastAudioMuted);
    CEF::PageBridge bridge (browser, *shm, project.properties, *client);

    while (!shm->quitRequested.load (std::memory_order_acquire)) {
	// the heartbeat keeps the page alive while the main process isn't rendering (surface covered, no frame
	// callbacks)
	const auto now = std::chrono::steady_clock::now ();
	const uint32_t frameRequest = shm->frameRequestSeq.load (std::memory_order_acquire);

	if (frameRequest != lastFrameRequest || now - lastBeginFrame > std::chrono::milliseconds (100)) {
	    lastFrameRequest = frameRequest;
	    lastBeginFrame = now;
	    browser->GetHost ()->SendExternalBeginFrame ();
	}

	CefDoMessageLoopWork ();

	const uint32_t desiredWidth = shm->desiredWidth.load (std::memory_order_relaxed);
	const uint32_t desiredHeight = shm->desiredHeight.load (std::memory_order_relaxed);

	// also compare with what CEF was last told, else a resize during CreateBrowserSync is lost (16x17 page)
	const bool viewDiffers = std::clamp (desiredWidth, 1u, shm->maxWidth) != renderHandler->viewWidth ()
	    || std::clamp (desiredHeight, 1u, shm->maxHeight) != renderHandler->viewHeight ();

	if (!viewDiffers) {
	    resizeRequested = false;
	}

	if (desiredWidth != lastDesiredWidth || desiredHeight != lastDesiredHeight
	    || (viewDiffers && !resizeRequested)) {
	    resizeRequested = viewDiffers;
	    lastDesiredWidth = desiredWidth;
	    lastDesiredHeight = desiredHeight;
	    browser->GetHost ()->WasResized ();
	}

	const bool desiredAudioMuted = shm->audioMuted.load (std::memory_order_relaxed);

	if (desiredAudioMuted != lastAudioMuted) {
	    lastAudioMuted = desiredAudioMuted;
	    browser->GetHost ()->SetAudioMuted (lastAudioMuted);
	}

	bridge.update ();

	CefMouseEvent evt;
	evt.x = static_cast<int> (shm->mouseX.load (std::memory_order_relaxed));
	evt.y = static_cast<int> (shm->mouseY.load (std::memory_order_relaxed));

	if (evt.x != lastMouseX || evt.y != lastMouseY) {
	    lastMouseX = evt.x;
	    lastMouseY = evt.y;
	    browser->GetHost ()->SendMouseMoveEvent (evt, false);
	}

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

	shm->waitForFrameRequest (lastFrameRequest, 1);
    }

    browser->GetHost ()->CloseBrowser (true);

    // CloseBrowser() is async, give it time to finish before CefShutdown()
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
	int yoffset;
	float ustart, uend, vstart, vend;
    };

    std::vector<ViewportCapture> captures;
    int currentXOffset = 0;

    for (const auto& [screen, viewport] : this->m_renderContext->getOutput ().getViewports ()) {
	viewport->makeCurrent ();

	const int screenWidth = viewport->viewport.z - viewport->viewport.x;
	std::vector<std::pair<std::string, glm::ivec4>> parts;

	if (const auto* split = this->m_context.findSplitScreen (screen); split != nullptr) {
	    const auto& output = viewport->viewport;

	    for (const auto& slot : split->slots) {
		const auto rect = this->m_renderContext->getSlotRect (slot);

		if (!rect.has_value ()) {
		    continue;
		}

		const int top = vflip ? output.y + output.w - (rect->y + rect->w) : rect->y - output.y;
		parts.emplace_back (slot, glm::ivec4 (rect->x - output.x, top, rect->z, rect->w));
	    }
	} else {
	    parts.emplace_back (screen, glm::ivec4 (0, 0, screenWidth, viewport->viewport.w - viewport->viewport.y));
	}

	for (const auto& [part, area] : parts) {
	    const auto wallpaperIt = wallpapers.find (part);
	    if (wallpaperIt == wallpapers.end ()) {
		sLog.error ("Cannot find wallpaper for screen ", part);
		continue;
	    }

	    const auto& wallpaper = wallpaperIt->second;
	    const int vpWidth = area.z;
	    const int vpHeight = area.w;

	    // the default framebuffer is unreliable on some drivers (NVIDIA/Wayland)
	    glBindFramebuffer (GL_FRAMEBUFFER, wallpaper->renderAdjustedFramebuffer ());

	    glFinish ();

	    while (glGetError () != GL_NO_ERROR) { }

	    const int readWidth = wallpaper->getFramebufferWidth ();
	    const int readHeight = wallpaper->getFramebufferHeight ();
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

	    const glm::vec4 uvs = wallpaper->getOutputUVs ();

	    captures.push_back (
		{ buffer, readWidth, readHeight, vpWidth, vpHeight, currentXOffset + area.x, area.y, uvs.x, uvs.y,
		  uvs.z, uvs.w }
	    );
	}

	if (viewport->single) {
	    currentXOffset += screenWidth;
	}
    }

    const auto extension = filename.extension ();
    const std::string extStr = extension.string ();

    const auto encode = [captures, width, height, vflip, extStr, filename] () {
	auto* bitmap = new uint8_t[width * height * 3] { 0 };

	for (const auto& capture : captures) {
	    const float spanX = std::abs (capture.uend - capture.ustart) * capture.readWidth / capture.vpWidth;
	    const float spanY = std::abs (capture.vend - capture.vstart) * capture.readHeight / capture.vpHeight;
	    const int samplesX = std::max (1, static_cast<int> (std::lround (spanX)));
	    const int samplesY = std::max (1, static_cast<int> (std::lround (spanY)));
	    // sample from the output pixel's center, its left edge truncated 250/1920*1920 to 249
	    const auto firstSample = [] (const int pixel, const int size, const float start, const float end,
					 const int readSize, const int samples) {
		const double center = start + (pixel + 0.5) / size * (end - start);
		return static_cast<int> (std::floor (center * readSize - samples * 0.5 + 0.5));
	    };

	    for (int y = 0; y < capture.vpHeight; y++) {
		for (int x = 0; x < capture.vpWidth; x++) {
		    const int srcX
			= firstSample (x, capture.vpWidth, capture.ustart, capture.uend, capture.readWidth, samplesX);
		    const int srcY
			= firstSample (y, capture.vpHeight, capture.vstart, capture.vend, capture.readHeight, samplesY);
		    int sum[3] = { 0, 0, 0 };

		    for (int sy = 0; sy < samplesY; sy++) {
			const int row = std::clamp (srcY + sy, 0, capture.readHeight - 1);

			for (int sx = 0; sx < samplesX; sx++) {
			    const int column = std::clamp (srcX + sx, 0, capture.readWidth - 1);
			    const int srcIdx = (row * capture.readWidth + column) * 3;

			    sum[0] += capture.buffer[srcIdx];
			    sum[1] += capture.buffer[srcIdx + 1];
			    sum[2] += capture.buffer[srcIdx + 2];
			}
		    }

		    const int count = samplesX * samplesY;
		    const int xfinal = x + capture.xoffset;
		    // FBOs aren't flipped like the default framebuffer
		    const int yfinal = capture.yoffset + (vflip ? y : (capture.vpHeight - y - 1));

		    if (yfinal >= 0 && yfinal < height && xfinal >= 0 && xfinal < width) {
			for (int channel = 0; channel < 3; channel++) {
			    bitmap[yfinal * width * 3 + xfinal * 3 + channel]
				= static_cast<uint8_t> ((sum[channel] + count / 2) / count);
			}
		    }
		}
	    }
	    delete[] capture.buffer;
	}

	const auto write = [&] (const std::filesystem::path& target) {
	    if (extStr == ".bmp") {
		return stbi_write_bmp (target.c_str (), width, height, 3, bitmap) != 0;
	    }

	    if (extStr == ".png") {
		return stbi_write_png (target.c_str (), width, height, 3, bitmap, width * 3) != 0;
	    }

	    if (extStr == ".jpg" || extStr == ".jpeg") {
		return stbi_write_jpg (target.c_str (), width, height, 3, bitmap, 100) != 0;
	    }

	    return false;
	};

	// whoever waits for the file never sees half of it
	const std::filesystem::path partial = filename.string () + ".partial";
	std::error_code error;

	if (!write (partial) || (std::filesystem::rename (partial, filename, error), error)) {
	    std::filesystem::remove (partial, error);
	    write (filename);
	}

	delete[] bitmap;
    };

    this->m_screenshotWriter
	= std::jthread ([encode] { WallpaperEngine::Logging::guardThread ("Screenshot writer", encode); });
}

void WallpaperApplication::setupOutput () {
    const char* XDG_SESSION_TYPE = getenv ("XDG_SESSION_TYPE");

    if (!XDG_SESSION_TYPE) {
	sLog.exception (
	    "Cannot read environment variable XDG_SESSION_TYPE, window server detection failed. Please ensure proper "
	    "values are set"
	);
    }

    std::string sessionType = XDG_SESSION_TYPE;

#ifdef ENABLE_WAYLAND
    const auto drivers = sVideoFactories.getRegisteredDrivers ();

    if (sessionType == "wayland" && this->m_context.settings.render.mode == ApplicationContext::DESKTOP_BACKGROUND
	&& getenv ("DISPLAY") != nullptr && std::ranges::find (drivers, "x11") != drivers.end ()
	&& WallpaperEngine::Desktop::compositorLacksLayerShell ()) {
	sLog.out ("The compositor has no layer shell, drawing through XWayland into a background window instead");
	setenv ("LWE_X11_BACKGROUND_WINDOW", "1", 0);
	setenv ("LWE_XWAYLAND_FALLBACK", "1", 1);
	unsetenv ("WAYLAND_DISPLAY");
	setenv ("XDG_SESSION_TYPE", "x11", 1);
	sessionType = "x11";
    }
#endif

    sLog.debug ("Checking for window servers: ");

    for (const auto& windowServer : sVideoFactories.getRegisteredDrivers ()) {
	sLog.debug ("\t", windowServer);
    }

    this->m_videoDriver
	= sVideoFactories.createVideoDriver (this->m_context.settings.render.mode, sessionType, this->m_context, *this);
    this->m_fullScreenDetector
	= sVideoFactories.createFullscreenDetector (sessionType, this->m_context, *this->m_videoDriver);
}

void WallpaperApplication::setupAudio () {
    const bool audioProcessingRequired = std::ranges::any_of (
	this->m_backgrounds, [] (const std::pair<const std::string, ProjectUniquePtr>& pair) -> bool {
	    return pair.second->supportsAudioProcessing;
	}
    );

    if (audioProcessingRequired && this->m_context.settings.audio.audioprocessing) {
	this->m_audioRecorder
	    = std::make_unique<WallpaperEngine::Audio::Drivers::Recorders::PulseAudioPlaybackRecorder> ();
	this->m_audioCapturing = true;
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

void WallpaperApplication::ensureAudioCapture (const Project& project) {
    if (this->m_audioCapturing || !project.supportsAudioProcessing || !this->m_context.settings.audio.audioprocessing
	|| !this->m_audioDriver) {
	return;
    }

    sLog.out ("Starting audio capture for ", project.title);

    this->m_previousAudioRecorder = std::move (this->m_audioRecorder);
    this->m_audioRecorder = std::make_unique<WallpaperEngine::Audio::Drivers::Recorders::PulseAudioPlaybackRecorder> ();
    this->m_audioDriver->setRecorder (*this->m_audioRecorder);
    this->m_audioCapturing = true;
}

ApplicationContext::SpanGroup* WallpaperApplication::findSpanGroup (const std::string& screen) {
    return const_cast<ApplicationContext::SpanGroup*> (std::as_const (*this).findSpanGroup (screen));
}

const ApplicationContext::SpanGroup* WallpaperApplication::findSpanGroup (const std::string& screen) const {
    if (!screen.starts_with ("span:")) {
	return nullptr;
    }

    for (const auto& spanGroup : this->m_context.settings.general.spanGroups) {
	if (!spanGroup.screens.empty () && "span:" + spanGroup.screens.front () == screen) {
	    return &spanGroup;
	}
    }

    return nullptr;
}

void WallpaperApplication::installWallpaper (
    const std::string& screen, const Project& project, const std::filesystem::path& path, const bool holdPrevious
) try {
    const auto& general = this->m_context.settings.general;
    const auto& window = this->m_context.settings.render.window;
    const auto* spanGroup = this->findSpanGroup (screen);

    if (spanGroup == nullptr) {
	const auto scalingIt = general.screenScalings.find (screen);
	const auto clampIt = general.screenClamps.find (screen);

	auto wallpaper = WallpaperEngine::Render::CWallpaper::fromWallpaper (
	    *project.wallpaper, *this->m_renderContext, *this->m_audioContext, path,
	    scalingIt != general.screenScalings.end () ? scalingIt->second : window.scalingMode,
	    clampIt != general.screenClamps.end () ? clampIt->second : window.clamp,
	    this->resolveScreenRenderSize (screen)
	);
	wallpaper->setStorageScreen (this->storageScreen (screen));
	wallpaper->setZoom (this->resolveScreenZoom (screen));
	const auto offset = this->resolveScreenOffset (screen);
	wallpaper->setOffset (offset.x, offset.y);
	wallpaper->setAlignment (this->resolveScreenAlignment (screen));
	wallpaper->setCornerColor (this->resolveScreenCornerColor (screen));
	wallpaper->setImageAdjustments (this->resolveScreenImageAdjustments (screen, project));

	if (wallpaper->is<WallpaperEngine::Render::Wallpapers::CVideo> ()) {
	    wallpaper->as<WallpaperEngine::Render::Wallpapers::CVideo> ()->setSegments (
		this->resolveScreenVideoSegments (screen)
	    );
	}

	this->m_renderContext->setWallpaper (screen, std::move (wallpaper), holdPrevious);
	return;
    }

    const auto& viewports = this->m_renderContext->getOutput ().getViewports ();
    glm::ivec2 low (INT_MAX);
    glm::ivec2 high (INT_MIN);

    for (const auto& name : spanGroup->screens) {
	const auto it = viewports.find (name);

	if (it == viewports.end ()) {
	    sLog.error ("Span group screen not found: ", name);
	    continue;
	}

	low = glm::min (low, it->second->globalPosition);
	high = glm::max (high, it->second->globalPosition + it->second->logicalSize);
    }

    if (high.x <= low.x || high.y <= low.y) {
	throw std::runtime_error ("none of the span group's screens is there");
    }

    std::shared_ptr<WallpaperEngine::Render::CWallpaper> wallpaper
	= WallpaperEngine::Render::CWallpaper::fromWallpaper (
	    *project.wallpaper, *this->m_renderContext, *this->m_audioContext, path, spanGroup->scaling,
	    spanGroup->clamp, high - low
	);
    wallpaper->setStorageScreen (this->storageScreen (screen));
    wallpaper->setZoom (spanGroup->zoom);
    wallpaper->setOffset (spanGroup->offset.x, spanGroup->offset.y);
    wallpaper->setAlignment (spanGroup->alignment);
    wallpaper->setCornerColor (spanGroup->cornerColor);
    wallpaper->setImageAdjustments (this->resolveScreenImageAdjustments (screen, project));
    wallpaper->setSpanInfo ({ .screens = spanGroup->screens, .totalBounds = { low, high - low } });

    if (wallpaper->is<WallpaperEngine::Render::Wallpapers::CVideo> ()) {
	wallpaper->as<WallpaperEngine::Render::Wallpapers::CVideo> ()->setSegments (
	    this->resolveScreenVideoSegments (screen)
	);
    }

    for (const auto& name : spanGroup->screens) {
	this->m_renderContext->setWallpaper (name, wallpaper, holdPrevious);
    }
} catch (const std::exception& e) {
    throw std::runtime_error ("Cannot set up wallpaper '" + path.string () + "' on " + screen + ": " + e.what ());
}

void WallpaperApplication::prepareOutputs () {
    m_renderContext
	= std::make_unique<WallpaperEngine::Render::RenderContext> (*m_videoDriver, *this, *this->m_mediaSource);

    for (const auto& [background, info] : this->m_backgrounds) {
	if (this->findSpanGroup (background) == nullptr) {
	    this->installWallpaper (background, *info, this->resolveScreenBackgroundPath (background), false);
	    continue;
	}

	try {
	    this->installWallpaper (background, *info, this->resolveScreenBackgroundPath (background), false);
	} catch (const std::runtime_error& e) {
	    sLog.error ("Skipping span group ", background, ": ", e.what ());
	}
    }

    this->applyAudioPolicy ();
}

void WallpaperApplication::setupOpenGLDebugging () {
    glDebugMessageCallback (CustomGLDebugCallback, nullptr);
    glEnable (GL_DEBUG_OUTPUT_SYNCHRONOUS);
}

void WallpaperApplication::setup () {
    this->setupOutput ();

    // we_manager's LD_PRELOAD (system libEGL) has done its job here, but CEF's subprocesses would inherit it and WebGL
    // silently stops painting only affects processes started after this point
    unsetenv ("LD_PRELOAD");

    this->setupAudio ();
    this->prepareOutputs ();
    this->setupOpenGLDebugging ();
    this->updatePowerMonitor ();

    if (this->m_context.settings.general.dumpStructure) {
	auto prettyPrinter = Data::Dumpers::StringPrinter ();

	for (const auto& [background, info] : this->m_renderContext->getWallpapers ()) {
	    prettyPrinter.printWallpaper (info->getWallpaperData ());
	}

	std::cout << prettyPrinter.str () << std::endl;
    }

#if DEMOMODE
    if (this->m_renderContext->getWallpapers ().size () > 1) {
	sLog.exception ("Demo mode only supports one background");
    }

    this->m_demo.width = this->m_renderContext->getWallpapers ().begin ()->second->getFramebufferWidth ();
    this->m_demo.height = this->m_renderContext->getWallpapers ().begin ()->second->getFramebufferHeight ();
    this->m_demo.pixels.assign (static_cast<size_t> (this->m_demo.width) * this->m_demo.height * 3, 0);
    this->m_demo.initialized = false;
    this->m_demo.frame = 0;
#endif
}

void WallpaperApplication::render () {
    static float rawTimeLast = 0.0f;

    if (this->m_isPaused) {
	usleep (FULLSCREEN_CHECK_WAIT_TIME);
	m_videoDriver->dispatchWhilePaused ();

	// automute keeps following other programs' sound, or the music stays on until the wallpaper resumes
	if (this->m_audioDriver != nullptr) {
	    this->m_audioDriver->getAudioDetector ().update ();
	}

	const uint32_t reasons = this->pauseReasons ();
	this->logPauseReasons (reasons);

	// fullscreen pauses delay a hotswap, battery/idle pauses take it at once (it may lift them) and show one frame
	const bool takeHotswap = this->m_hotswapRequested && (reasons & PauseWindows) == 0;

	if (reasons != 0 && !takeHotswap && this->m_context.state.general.keepRunning) {
	    return;
	}

	this->m_showFrameWhilePaused = takeHotswap;
	m_renderContext->setPause (false);

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
	const time_t seconds = time (nullptr);
	struct tm timeinfo {};
	localtime_r (&seconds, &timeinfo);
	g_Daytime = static_cast<float> ((timeinfo.tm_hour * 60) + timeinfo.tm_min) / (24.0f * 60.0f);

	// LWE_FIXED_TIMESTEP=<seconds>: same step every frame so frame N always shows the same moment (regression
	// renders)
	static const float fixedTimestep = [] {
	    const char* value = std::getenv ("LWE_FIXED_TIMESTEP");
	    return value != nullptr ? std::max (0.0f, std::strtof (value, nullptr)) : 0.0f;
	}();

	const float rawTimeNow = m_videoDriver->getRenderTime ();
	const float rawDelta = fixedTimestep > 0.0f ? fixedTimestep : rawTimeNow - rawTimeLast;
	rawTimeLast = rawTimeNow;

	g_TimeLast = g_Time;
	if (!this->m_context.settings.render.freezeAnimations) {
	    // WE clamps a frame's step to [1e-4, 0.25] (2.8.42 sub_140110630), a stall doesn't make scripts catch up at
	    // once
	    g_Time += std::clamp (
		std::min (rawDelta, 0.25f) * this->m_context.settings.render.playbackSpeed, 0.0001f, 0.25f
	    );
	}
	g_RealTime = fixedTimestep > 0.0f ? g_RealTime + fixedTimestep : rawTimeNow;
	m_audioDriver->update (rawDelta * this->m_context.settings.render.playbackSpeed);
	m_mediaSource->update ();
	m_videoDriver->getInputContext ().update ();

	// LWE_HOTSWAP_AT_FRAME=<n>: read the control file before frame n as if SIGUSR1 came in (regression tests)
	static const int64_t hotswapFrame = [] {
	    const char* value = std::getenv ("LWE_HOTSWAP_AT_FRAME");
	    return value != nullptr ? std::strtoll (value, nullptr, 10) : -1;
	}();

	if (hotswapFrame >= 0 && m_videoDriver->getFrameCounter () == static_cast<uint64_t> (hotswapFrame)) {
	    this->m_hotswapRequested = true;
	    this->checkHotswapRequest ();
	}

	m_videoDriver->dispatchEventQueue ();

	// spritesheetrefreshsync: sleep until the first image's next sprite frame (sub_140110630)
	if (fixedTimestep <= 0.0f && !this->m_context.settings.render.freezeAnimations) {
	    std::optional<float> syncDelay;

	    for (const auto& [screen, wallpaper] : this->m_renderContext->getWallpapers ()) {
		if (!wallpaper->is<WallpaperEngine::Render::Wallpapers::CScene> ()) {
		    continue;
		}

		if (const auto delay
		    = wallpaper->as<WallpaperEngine::Render::Wallpapers::CScene> ()->spriteSheetSyncDelay ();
		    delay.has_value ()) {
		    syncDelay = std::min (syncDelay.value_or (*delay), *delay);
		}
	    }

	    if (syncDelay.has_value ()) {
		float wait = *syncDelay;
		const float speed = this->m_context.settings.render.playbackSpeed;

		if (speed > 0.001f) {
		    wait /= speed;
		}

		std::this_thread::sleep_for (
		    std::chrono::milliseconds (static_cast<int> (std::min (wait, 0.25f) * 1000.0f))
		);
	    }
	}

	const auto frameDone = std::chrono::steady_clock::now ();
	const auto pause = this->m_framePacer.beginSleep (
	    frameDone - this->m_frameEnd, this->frameRateLimit (),
	    this->m_context.settings.render.frameLimiter == "fixed"
	);

	if (pause > std::chrono::nanoseconds::zero ()) {
	    std::this_thread::sleep_for (pause);
	}

	this->m_frameEnd = std::chrono::steady_clock::now ();
	this->m_framePacer.endSleep (this->m_frameEnd - frameDone);

	static const bool paceStats = std::getenv ("LWE_FRAME_STATS") != nullptr;
	static auto paceWindowStart = this->m_frameEnd;

	if (paceStats && this->m_frameEnd - paceWindowStart >= std::chrono::seconds (2)) {
	    const auto stats = this->m_framePacer.takeStats ();
	    const auto millis = [] (std::chrono::nanoseconds time) {
		return std::chrono::duration<double, std::milli> (time).count ();
	    };
	    const double n = std::max (1, stats.frames);

	    sLog.out (
		"FRAME-PACE: limit ", this->frameRateLimit (), " fps, loop ",
		n * 1000.0 / millis (this->m_frameEnd - paceWindowStart), " fps | work avg ", millis (stats.work) / n,
		" max ", millis (stats.workMax), " | sleep asked avg ", millis (stats.asked) / n, " got avg ",
		millis (stats.slept) / n, " (worst overshoot ", millis (stats.overshootMax), ") | no sleep ",
		stats.late, " of ", stats.frames, " frames (ms)"
	    );
	    paceWindowStart = this->m_frameEnd;
	}

	if (m_videoDriver->closeRequested ()) {
	    sLog.out ("Stop requested by driver");
	    this->m_context.state.general.keepRunning = false;
	}

#if DEMOMODE
	// wait a full render cycle before starting, giving video/web decoders time to set up
	if (m_videoDriver->getFrameCounter () > (uint32_t)this->m_context.settings.render.maximumFPS) {
	    if (!this->m_demo.initialized) {
		this->m_demo.width = this->m_renderContext->getWallpapers ().begin ()->second->getFramebufferWidth ();
		this->m_demo.height = this->m_renderContext->getWallpapers ().begin ()->second->getFramebufferHeight ();
		this->m_demo.pixels.assign (static_cast<size_t> (this->m_demo.width) * this->m_demo.height * 3, 0);
		init_encoder ("output.webm", this->m_demo.width, this->m_demo.height);
		this->m_demo.initialized = true;
	    }

	    glBindFramebuffer (
		GL_FRAMEBUFFER, this->m_renderContext->getWallpapers ().begin ()->second->getWallpaperFramebuffer ()
	    );

	    glPixelStorei (GL_PACK_ALIGNMENT, 1);
	    glReadPixels (
		0, 0, this->m_demo.width, this->m_demo.height, GL_RGB, GL_UNSIGNED_BYTE, this->m_demo.pixels.data ()
	    );
	    write_video_frame (this->m_demo.pixels.data ());
	    this->m_demo.frame++;

	    if (this->m_demo.frame >= FRAME_COUNT) {
		this->m_context.state.general.keepRunning = false;
	    }
	}
#endif
	const uint32_t reasons = this->pauseReasons ();

	if (reasons != 0 && !this->m_showFrameWhilePaused && this->m_context.state.general.keepRunning) {
	    this->logPauseReasons (reasons);
	    this->m_isPaused = true;
	    this->m_pauseStart = std::chrono::steady_clock::now ();

	    m_renderContext->setPause (true);
	    return;
	}

	if (reasons == 0) {
	    this->logPauseReasons (0);
	}

	this->m_showFrameWhilePaused = false;
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
#endif
}

void WallpaperApplication::show () {
    setup ();
    while (this->m_context.state.general.keepRunning) {
	render ();
    }
    WallpaperEngine::Scripting::LocalStorage::flushAll ();
    cleanup ();
}

void WallpaperApplication::update (Render::Drivers::Output::OutputViewport* viewport) {
    m_renderContext->render (viewport);
}

void WallpaperApplication::signal (int signal) {
    if (signal == SIGUSR1) {
	this->m_hotswapRequested = true;
	return;
    }

    // no logger here, it allocates and a signal mid-malloc would deadlock
    static constexpr char message[] = "Stop requested by signal\n";
    std::ignore = write (STDOUT_FILENO, message, sizeof (message) - 1);
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