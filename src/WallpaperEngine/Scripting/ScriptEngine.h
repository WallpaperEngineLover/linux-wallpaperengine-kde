#pragma once

#include "Adapters/VectorAdapter.h"
#include "AnimationSystem.h"
#include "ConsoleObject.h"
#include "EngineObject.h"
#include "InputObject.h"
#include "SceneObject.h"

#include <chrono>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Data/Model/Types.h"
#include "WallpaperEngine/Media/MediaSource.h"
#include "WallpaperEngine/Media/ThumbnailPalette.h"

#include <v8-array-buffer.h>
#include <v8-context.h>
#include <v8-exception.h>
#include <v8-isolate.h>
#include <v8-local-handle.h>
#include <v8-persistent-handle.h>
#include <v8-script.h>

namespace WallpaperEngine::Media {
class MediaSource;
}

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}

namespace WallpaperEngine::Render::Objects {
class CImage;
}

namespace WallpaperEngine::VideoPlayback::MPV {
class GLPlayer;
}

namespace WallpaperEngine::Scripting {
/** Logs what tryCatch caught, once per distinct error and context since failing scripts throw every frame */
void logJSException (
    v8::Isolate* isolate, const v8::TryCatch& tryCatch, const char* context,
    const std::optional<std::string>& source = std::nullopt
);

class ScriptPropertiesObject;
class ScriptableObject;
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
	// the module's namespace object, exports are looked up on it
	v8::Global<v8::Object> module;
	// Owning layer, so tick() can rebind `thisLayer` to the right object before each
	// module's update() runs - see ScriptEngine::tick().
	ScriptableObject* object = nullptr;
	// init()/applyUserProperties() wait for the first tick() so every layer of the scene already
	// exists when a script looks its siblings up with thisScene.getLayer()
	bool initialized = false;
	// cached `thisObject` handle, see makeThisObject()
	v8::Global<v8::Value> thisObject;
	// builds thisObject instead of makeThisObject() (animation layer scripts get the IAnimationLayer)
	std::function<v8::Local<v8::Value> (ScriptEngine&)> thisObjectFactory;
	// name of the property the script is attached to ("origin", an effect constant, ...)
	std::string propertyName;
	// -1 until checked, then whether the module exports any cursor* handler
	int cursorHandlers = -1;
	// owning object was destroyed, waiting for the retire pass in tick()
	bool dropped = false;
	// registration order, which is scene order, what update() calls follow
	uint64_t order = 0;
	// its key in m_scriptModules, which stays the same when the entry is rebound
	std::string key;
    };
    struct JSObjectAdapters {
	std::unique_ptr<Adapters::VectorAdapter<4>> vec4;
	std::unique_ptr<Adapters::VectorAdapter<3>> vec3;
	std::unique_ptr<Adapters::VectorAdapter<2>> vec2;
	std::unique_ptr<Adapters::ScriptableObjectAdapter> object;
    };

    /** Enters the engine's isolate and context with a handle scope, for calls into it from outside of JS */
    class Scope {
    public:
	explicit Scope (const ScriptEngine& engine);

    private:
	v8::Isolate::Scope m_isolateScope;
	v8::HandleScope m_handleScope;
	v8::Context::Scope m_contextScope;
    };

    ~ScriptEngine ();
    ScriptEngine (Render::Wallpapers::CScene& scene, Media::MediaSource& mediaSource);
    ScriptEngine (const ScriptEngine&) = delete;
    ScriptEngine& operator= (const ScriptEngine&) = delete;

    /** The engine owning the isolate a native callback runs in */
    static ScriptEngine& from (v8::Isolate* isolate) { return *static_cast<ScriptEngine*> (isolate->GetData (0)); }

    v8::Isolate* getIsolate () const { return m_isolate; }
    v8::Local<v8::Context> getContext () const { return m_context.Get (m_isolate); }
    v8::Local<v8::Object> getGlobalThis () const { return this->getContext ()->Global (); }
    LoadedModule* getRunningModule () const { return m_runningModule; }
    // true while a module's top level code runs, WE calls that the global scope
    bool isEvaluatingModuleBody () const { return m_evaluatingModuleBody; }
    // true while a module's update() runs
    bool isRunningUpdate () const { return m_runningUpdate; }
    v8::Local<v8::Value> dynamicToJs (DynamicValue& value) const;
    /** Same as dynamicToJs() but colour properties come out as Vec3 like they do in real scripts, not Vec4 */
    v8::Local<v8::Value> userPropertyToJs (Property& property) const;
    // Converts a JS value read from `val` into `target` - the inverse of dynamicToJs(), exposed
    // so exotic property setters (e.g. `thisLayer.origin = ...` from another layer's script) can
    // write through to the real property instead of silently discarding the assignment.
    void assignJsValue (v8::Local<v8::Value> val, DynamicValue& target) const;
    // dynamicToJs()/assignJsValue() for a named object property: scripts see "angles" in degrees
    v8::Local<v8::Value> propertyToJs (DynamicValue& value, std::string_view name) const;
    void assignPropertyJsValue (v8::Local<v8::Value> val, DynamicValue& target, std::string_view name) const;

    void queueScript (
	const std::string& key, DynamicValue& currentValue, ScriptableObject& object,
	const std::string& propertyName = {}
    );

    /** Stops a queued script module (by its queueScript() key) and frees it at the start of the next tick(), for when a
     * later registerProperty() supersedes it */
    void retireScript (const std::string& key);

    /** Detaches and retires every module attached to object, for an object destroyed while the script context lives on
     * (setup failed after its properties queued their scripts) */
    void dropObjectScripts (const ScriptableObject& object);

    /** Rebinds an already-running module under key to newValue in place when its script source is identical, so init()
     * does not run twice. Returns false if nothing was rebound */
    bool rebindScript (const std::string& key, DynamicValue& newValue);

    /**
     * Runs a frame tick in the javascript engine. Dispatches any pending events,
     * timeouts, intervals AND calls any update() functions.
     */
    void tick ();

    /** Runs pending init () calls in scene order, before the camera is set like WE */
    void initializePending ();

    // Layer-script API: WE text-object scripts follow a lifecycle that
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
     * the object with an event carrying worldPosition (scene coordinates) and localPosition (where the object was hit,
     * see CScene::dispatchCursorEvents)
     */
    void dispatchCursorEvent (
	const char* handler, ScriptableObject& object, const glm::vec2& worldPosition, const glm::vec3& localPosition
    );

    /** Calls callback (once per playthrough) when player reaches the end of a non-looping video, for
     * IVideoTexture.addEndedCallback() */
    void addVideoEndedCallback (VideoPlayback::MPV::GLPlayer* player, v8::Local<v8::Value> callback);
    /** Calls callback every time the puppet animation layer reaches its end, for IAnimationLayer.addEndedCallback() */
    void addAnimationLayerEndedCallback (const ScriptableObject& owner, size_t serial, v8::Local<v8::Value> callback);
    /** Runs the ended callbacks of that animation layer, from the puppet update like WE (sub_1401FDF90) */
    void dispatchAnimationLayerEnded (const ScriptableObject& owner, size_t serial);
    /** Puppet clip event -> animationEvent (sub_140177AD0) with the payload JSON parsed, null if invalid */
    void dispatchClipEvent (const ScriptableObject& owner, const std::string& payload);
    /** The script queued under key gets thisObject from factory instead of the property handle */
    void setThisObjectFactory (const std::string& key, std::function<v8::Local<v8::Value> (ScriptEngine&)> factory);

    AnimationSystem& getAnimations () { return m_animations; }
    EngineObject& getEngineObject () const { return *m_engineObject; }
    ScriptPropertiesObject& getScriptPropertiesObject () const { return *m_scriptPropertiesObject; }
    SceneObject& getSceneObject () const { return *m_sceneObject; }
    /** Whether a script module is currently running for this property value */
    [[nodiscard]] bool hasScript (const DynamicValue& value) const;
    const JSObjectAdapters& getAdapters () const { return m_adapters; }
    const Render::Wallpapers::CScene& getScene () const { return m_scene; }
    /** Reads a file through the wallpaper's asset locator (project first, then the assets dir), nullopt if it is
     * missing */
    std::optional<std::string> readScriptAsset (const std::string& path) const;

    /** Calls callback with the module under key as the running script (its thisLayer/thisObject) when it is still
     *  there, without one otherwise. Empty when it threw */
    v8::MaybeLocal<v8::Value> callAsModule (const std::string& key, v8::Local<v8::Function> callback);

    /** The module an import names, compiled, instantiated and evaluated once per engine like scenescript64's
     *  resolver (sub_1816469C0) */
    v8::MaybeLocal<v8::Module> importModule (const std::string& specifier);

private:
    /** Calls the module's export `name` if it is a function, undefined otherwise. Empty when it threw */
    v8::MaybeLocal<v8::Value>
    call (const LoadedModule& module, const char* name, int argc, v8::Local<v8::Value> argv[]);
    void evaluateGlobalScript (const std::string& source, const char* name);
    /** A layer script's entry in globalThis.__textLayers, undefined when there is none */
    v8::Local<v8::Value> textLayer (ScriptLayerHandle handle);

    void installBuiltins ();

    Media::ThumbnailPalette thumbnailPaletteFor (const Media::MediaSource::MediaInfo& media);
    void notifyMediaUpdate (const Media::MediaSource::MediaInfo& media, LoadedModule* only = nullptr);
    void initializeModule (const std::string& key, LoadedModule& module);
    void bindThisLayer (ScriptableObject& object, LoadedModule* module = nullptr);
    v8::Local<v8::Value> makeThisObject (DynamicValue& value, const std::string& propertyName);

public:
    /** IAnimation of the animation getAnimation (name) finds on objectId's layer, or any layer without one; empty
     *  when there is none */
    v8::MaybeLocal<v8::Value> findAnimation (const std::string& name, std::optional<int> objectId);
    /** Same, within one animation group */
    v8::MaybeLocal<v8::Value> findAnimation (const std::string& name, const std::string& group);

private:
    void dispatchAnimationEvents ();

    // Installs globalThis.__layers and related helpers. Called lazily.
    void ensureLayerRegistry ();

    std::unique_ptr<v8::ArrayBuffer::Allocator> m_allocator;
    v8::Isolate* m_isolate = nullptr;
    v8::Global<v8::Context> m_context;
    // imports by lowercased name
    std::map<std::string, v8::Global<v8::Module>> m_imports;
    Render::Wallpapers::CScene& m_scene;
    std::unique_ptr<EngineObject> m_engineObject;
    std::unique_ptr<InputObject> m_inputObject;
    std::unique_ptr<SceneObject> m_sceneObject;
    std::unique_ptr<ConsoleObject> m_consoleObject;
    std::unique_ptr<ScriptPropertiesObject> m_scriptPropertiesObject;

    std::map<std::string, LoadedModule> m_scriptModules = {};
    uint64_t m_nextModuleOrder = 0;
    std::vector<std::string> m_retiredScriptKeys = {};

    LoadedModule* m_runningModule = nullptr;
    bool m_evaluatingModuleBody = false;
    bool m_runningUpdate = false;

    struct VideoEndedCallback {
	VideoPlayback::MPV::GLPlayer* player;
	v8::Global<v8::Function> callback;
	// set once the callback ran for the current end, cleared when the video is seeked back
	bool notified = false;
    };
    std::vector<VideoEndedCallback> m_videoEndedCallbacks = {};

    struct AnimationLayerEndedCallback {
	const ScriptableObject* owner;
	size_t serial;
	v8::Global<v8::Function> callback;
	// the module that registered it, callbacks run with its thisLayer/thisObject
	const LoadedModule* module;
    };
    std::vector<AnimationLayerEndedCallback> m_animationLayerEndedCallbacks = {};

    ScriptLayerHandle m_nextLayerId = 1;
    bool m_layerRegistryReady = false;
    std::map<ScriptLayerHandle, bool> m_layerInitialized;
    Media::MediaSource& m_mediaSource;
    std::function<void ()> m_unregisterMediaUpdateCallback;
    std::function<void ()> m_unregisterAlbumArtUpdateCallback;
    std::string m_paletteUrl;
    Media::ThumbnailPalette m_palette;

    JSObjectAdapters m_adapters;
    AnimationSystem m_animations;
};
} // namespace WallpaperEngine::Scripting
