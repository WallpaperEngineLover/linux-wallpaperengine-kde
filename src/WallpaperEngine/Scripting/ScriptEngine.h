#pragma once

#include "Adapters/VectorAdapter.h"
#include "AnimationSystem.h"
#include "ConsoleObject.h"
#include "EngineObject.h"
#include "InputObject.h"
#include "Modules/ScriptModule.h"
#include "SceneObject.h"

#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Data/Model/Types.h"
#include "WallpaperEngine/Media/MediaSource.h"
#include "WallpaperEngine/Media/ThumbnailPalette.h"

namespace WallpaperEngine::Media {
class MediaSource;
}
extern "C" {
#include "quickjs.h"
}

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}

namespace WallpaperEngine::VideoPlayback::MPV {
class GLPlayer;
}

namespace WallpaperEngine::Scripting {
void logJSException (JSContext* ctx, const char* context, const std::optional<std::string>& source = std::nullopt);

class ScriptPropertiesObject;
namespace Adapters {
    class ScriptableObjectAdapter;
}
using namespace WallpaperEngine::Data::Model;

// Opaque handle returned by createLayerScript. 0 means invalid / not created.
using ScriptLayerHandle = int;
static constexpr ScriptLayerHandle kInvalidLayerHandle = 0;

class ScriptEngine {
public:
    struct LoadedModule {
	DynamicValue& value;
	JSValue module;
	// Owning layer, so tick() can rebind `thisLayer` to the right object before each
	// module's update() runs - see ScriptEngine::tick().
	ScriptableObject* object = nullptr;
	// init()/applyUserProperties() wait for the first tick() so every layer of the scene already
	// exists when a script looks its siblings up with thisScene.getLayer()
	bool initialized = false;
	// cached `thisObject` handle, see makeThisObject()
	JSValue thisObject = JS_UNDEFINED;
	// name of the property the script is attached to ("origin", an effect constant, ...)
	std::string propertyName;
	// -1 until checked, then whether the module exports any cursor* handler
	int cursorHandlers = -1;
    };
    struct JSObjectAdapters {
	std::unique_ptr<Adapters::VectorAdapter<4>> vec4;
	std::unique_ptr<Adapters::VectorAdapter<3>> vec3;
	std::unique_ptr<Adapters::VectorAdapter<2>> vec2;
	std::unique_ptr<Adapters::ScriptableObjectAdapter> object;
    };

    ~ScriptEngine ();
    ScriptEngine (Render::Wallpapers::CScene& scene, Media::MediaSource& mediaSource);
    ScriptEngine (const ScriptEngine&) = delete;
    ScriptEngine& operator= (const ScriptEngine&) = delete;

    JSRuntime* getRuntime () const { return m_runtime; }
    JSContext* getContext () const { return m_context; }
    JSValue getGlobalThis () const { return m_globalThis; }
    LoadedModule* getRunningModule () const { return m_runningModule; }
    JSValue dynamicToJs (DynamicValue& value) const;
    /** Same as dynamicToJs() but colour properties come out as Vec3 like they do in real scripts, not Vec4 */
    JSValue userPropertyToJs (Property& property) const;
    // Converts a JS value read from `val` into `target` - the inverse of dynamicToJs(), exposed
    // so exotic property setters (e.g. `thisLayer.origin = ...` from another layer's script) can
    // write through to the real property instead of silently discarding the assignment.
    void assignJsValue (JSValue val, DynamicValue& target) const;

    /**
     * Evaluate a WallpaperEngine script's update() function.
     *
     * @param key The full JS script text (ES6 module with export function update(value))
     * @param currentValue The current value to pass to update()
     * @return The modified value from update(), or a copy of currentValue on error
     */
    void queueScript (
	const std::string& key, DynamicValue& currentValue, ScriptableObject& object, const std::string& propertyName = {}
    );

    /** Stops a queued script module (by its queueScript() key) and frees it at the start of the next tick(), for when a later registerProperty() supersedes it */
    void retireScript (const std::string& key);

    /** Rebinds an already-running module under key to newValue in place when its script source is identical, so init() does not run twice. Returns false if nothing was rebound */
    bool rebindScript (const std::string& key, DynamicValue& newValue);

    /**
     * Runs a frame tick in the javascript engine. Dispatches any pending events,
     * timeouts, intervals AND calls any update() functions.
     */
    void tick ();

    // Layer-script API (Phase 2 - dynamic text): WE text-object scripts follow a lifecycle that
    // doesn't fit the simple `update(value) -> value` contract above. They typically look like:
    //
    //   export var scriptProperties = createScriptProperties()…finish();
    //   export function init()   { /* subscribe to events, cache data    */ }
    //   export function update() { thisLayer.text = computeCurrentText(); }
    //
    // The script mutates `thisLayer` in place rather than returning a value, and lifecycle
    // functions are optional, so the API below keeps per-layer state alive across frames:
    // `init()` runs once, `update()` re-runs every tick.

    /**
     * Create a persistent "layer script" from a WE text-object script.
     *
     * @param scriptSource The full JS script text.
     * @param initialScriptProps Initial values for scriptProperties entries.
     *        Ownership stays with the caller; we only snapshot current values.
     * @param initialText Initial value of `thisLayer.text` (usually the
     *        static placeholder carried in the JSON).
     * @return A positive handle, or kInvalidLayerHandle if evaluation failed.
     */
    ScriptLayerHandle createLayerScript (
	const std::string& scriptSource, std::map<std::string, UserSettingUniquePtr>& initialScriptProps,
	const std::string& initialText
    );

    /**
     * Advance a layer by one frame.
     *
     * On the first call, invokes `init()` (if defined) before `update()`.
     * Updates a `thisScene` context visible to the script (time, fps).
     * Silently no-ops if the handle is invalid.
     */
    void tickLayer (ScriptLayerHandle handle, double time, double deltaTime, double fps);

    /**
     * Read the current value of `thisLayer.text` for the given layer.
     * Returns an empty string if the handle is invalid.
     */
    std::string layerText (ScriptLayerHandle handle);

    /**
     * Tear down a layer: invokes `destroy()` (if defined) and frees state.
     */
    void destroyLayer (ScriptLayerHandle handle);

    /** Whether any running script on the object exports a cursor handler (cursorEnter, cursorClick, ...) */
    [[nodiscard]] bool hasCursorHandlers (const ScriptableObject& object);
    /**
     * Calls `handler` (cursorEnter/cursorLeave/cursorMove/cursorDown/cursorUp/cursorClick) on every script running on
     * the object with an event carrying worldPosition (scene coordinates) and localPosition (offset from the layer's center)
     */
    void dispatchCursorEvent (
	const char* handler, ScriptableObject& object, const glm::vec2& worldPosition, const glm::vec2& localPosition
    );

    /** Calls callback (once per playthrough) when player reaches the end of a non-looping video, for IVideoTexture.addEndedCallback() */
    void addVideoEndedCallback (VideoPlayback::MPV::GLPlayer* player, JSValueConst callback);

    AnimationSystem& getAnimations () { return m_animations; }
    /** Whether a script module is currently running for this property value */
    [[nodiscard]] bool hasScript (const DynamicValue& value) const;
    const JSObjectAdapters& getAdapters () const { return m_adapters; }
    const Render::Wallpapers::CScene& getScene () const { return m_scene; }
    const std::map<std::string, std::unique_ptr<Modules::ScriptModule>>& getModules () const { return m_modules; }

private:
    JSValue call (JSValue module, int argc, JSValueConst argv[], const char* name);

    void installBuiltins ();

    Media::ThumbnailPalette thumbnailPaletteFor (const Media::MediaSource::MediaInfo& media);
    void notifyMediaUpdate (const Media::MediaSource::MediaInfo& media, LoadedModule* only = nullptr);
    void initializeModule (const std::string& key, LoadedModule& module);
    void bindThisLayer (ScriptableObject& object, LoadedModule* module = nullptr);
    JSValue makeThisObject (DynamicValue& value, const std::string& propertyName);
    void dispatchAnimationEvents ();

    // Installs globalThis.__layers and related helpers. Called lazily.
    void ensureLayerRegistry ();

    JSRuntime* m_runtime = nullptr;
    JSContext* m_context = nullptr;
    JSValue m_globalThis;
    Render::Wallpapers::CScene& m_scene;
    std::unique_ptr<EngineObject> m_engineObject;
    std::unique_ptr<InputObject> m_inputObject;
    std::unique_ptr<SceneObject> m_sceneObject;
    std::unique_ptr<ConsoleObject> m_consoleObject;
    std::unique_ptr<ScriptPropertiesObject> m_scriptPropertiesObject;

    std::map<std::string, std::unique_ptr<Modules::ScriptModule>> m_modules = {};
    std::map<std::string, LoadedModule> m_scriptModules = {};
    std::vector<std::string> m_retiredScriptKeys = {};

    LoadedModule* m_runningModule = nullptr;

    struct VideoEndedCallback {
	VideoPlayback::MPV::GLPlayer* player;
	JSValue callback;
	// set once the callback ran for the current end, cleared when the video is seeked back
	bool notified = false;
    };
    std::vector<VideoEndedCallback> m_videoEndedCallbacks = {};

    ScriptLayerHandle m_nextLayerId = 1;
    bool m_layerRegistryReady = false;
    std::map<ScriptLayerHandle, bool> m_layerInitialized;
    bool m_builtinsInstalled = false;
    Media::MediaSource& m_mediaSource;
    std::function<void ()> m_unregisterMediaUpdateCallback;
    std::function<void ()> m_unregisterAlbumArtUpdateCallback;
    std::string m_paletteUrl;
    Media::ThumbnailPalette m_palette;

    JSObjectAdapters m_adapters;
    AnimationSystem m_animations;
};
} // namespace WallpaperEngine::Scripting
