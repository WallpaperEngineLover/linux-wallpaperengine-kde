#pragma once

#include "Adapters/VectorAdapter.h"
#include "AnimationSystem.h"
#include "ConsoleObject.h"
#include "EngineObject.h"
#include "ErrorRepeatFilter.h"
#include "InputObject.h"
#include "LocalStorageObject.h"
#include "SceneObject.h"
#include "ScriptWatchdog.h"

#include <chrono>
#include <filesystem>
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

using ScriptLayerHandle = int;
static constexpr ScriptLayerHandle kInvalidLayerHandle = 0;

class ScriptEngine {
public:
    struct LoadedModule {
	DynamicValue& value;
	v8::Global<v8::Object> module;
	// owning layer, tick() rebinds thisLayer to it
	ScriptableObject* object = nullptr;
	// init()/applyUserProperties() wait for the first tick() so every layer of the scene already
	// exists when a script looks its siblings up with thisScene.getLayer()
	bool initialized = false;
	v8::Global<v8::Value> thisObject;
	// owners that aren't the layer: the pass's IMaterial for effect constants, the IAnimationLayer for animation
	// layer properties. Without one thisObject is the layer, like thisLayer
	std::function<v8::Local<v8::Value> (ScriptEngine&)> thisObjectFactory;
	std::string propertyName;
	// -1 until checked, then whether the module exports any cursor* handler
	int cursorHandlers = -1;
	int buttonHandlers = -1;
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

    static ScriptEngine& from (v8::Isolate* isolate) { return *static_cast<ScriptEngine*> (isolate->GetData (0)); }

    v8::Isolate* getIsolate () const { return m_isolate; }
    v8::Local<v8::Context> getContext () const { return m_context.Get (m_isolate); }
    v8::Local<v8::Object> getGlobalThis () const { return this->getContext ()->Global (); }
    LoadedModule* getRunningModule () const { return m_runningModule; }
    bool lastCallStopped () const { return m_runawayReported; }
    ErrorRepeatFilter& errorFilter () { return m_errorFilter; }
    // true while a module's top level code runs, WE calls that the global scope
    bool isEvaluatingModuleBody () const { return m_evaluatingModuleBody; }
    bool isRunningUpdate () const { return m_runningUpdate; }
    v8::Local<v8::Value> dynamicToJs (DynamicValue& value) const;
    /** Same as dynamicToJs() but colour properties come out as Vec3 like they do in real scripts, not Vec4 */
    v8::Local<v8::Value> userPropertyToJs (Property& property) const;
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

    void dropObjectScripts (const ScriptableObject& object);
    void destroyObjectScripts (const ScriptableObject& object);

    [[nodiscard]] uint64_t moduleMark () const { return this->m_nextModuleOrder; }
    /** dropObjectScripts () for whatever queued modules since mark, when the object they belong to was never finished:
     * its constructor threw, so nothing but the modules' pointers to it is left */
    void dropModulesSince (uint64_t mark);

    /** Rebinds an already-running module under key to newValue in place when its script source is identical, so init()
     * does not run twice. Returns false if nothing was rebound */
    bool rebindScript (const std::string& key, DynamicValue& newValue);

    void tick ();

    /** Runs pending init () calls in scene order, before the camera is set like WE */
    void initializePending ();

    // WE text scripts mutate thisLayer in place with optional init/update/destroy, so these keep per-layer state across
    // frames

    ScriptLayerHandle createLayerScript (
	const std::string& scriptSource, std::map<std::string, UserSettingUniquePtr>& initialScriptProps,
	const std::string& initialText
    );

    void tickLayer (ScriptLayerHandle handle, double time, double deltaTime, double fps);

    std::string layerText (ScriptLayerHandle handle);

    void destroyLayer (ScriptLayerHandle handle);

    [[nodiscard]] bool hasCursorHandlers (const ScriptableObject& object);
    /** a script exports cursorDown/cursorUp/cursorClick or has read input.cursorLeftDown */
    [[nodiscard]] bool wantsPointerButtons ();
    void noteButtonStateRead () { this->m_buttonStateRead = true; }
    /**
     * Calls handler (cursorEnter/Leave/Move/Down/Up/Click) on the object's scripts with button (always 0),
     * worldPosition, localPosition and hitBox (sub_18164E4D0)
     */
    void dispatchCursorEvent (
	const char* handler, ScriptableObject& object, const glm::vec2& worldPosition, const glm::vec3& localPosition,
	const std::optional<std::string>& hitBox
    );

    void addAnimationLayerEndedCallback (const ScriptableObject& owner, size_t serial, v8::Local<v8::Value> callback);
    /** Runs the ended callbacks of that animation layer, from the puppet update like WE (sub_1401FDF90) */
    void dispatchAnimationLayerEnded (const ScriptableObject& owner, size_t serial);
    /** Puppet clip event -> animationEvent (sub_140177AD0) with the payload JSON parsed, null if invalid */
    void dispatchClipEvent (const ScriptableObject& owner, const std::string& payload);
    void setThisObjectFactory (const std::string& key, std::function<v8::Local<v8::Value> (ScriptEngine&)> factory);

    AnimationSystem& getAnimations () { return m_animations; }
    EngineObject& getEngineObject () const { return *m_engineObject; }
    ScriptPropertiesObject& getScriptPropertiesObject () const { return *m_scriptPropertiesObject; }
    SceneObject& getSceneObject () const { return *m_sceneObject; }
    LocalStorageObject& getLocalStorageObject () const { return *m_localStorageObject; }
    [[nodiscard]] bool hasScript (const DynamicValue& value) const;
    const JSObjectAdapters& getAdapters () const { return m_adapters; }
    const Render::Wallpapers::CScene& getScene () const { return m_scene; }
    Render::Wallpapers::CScene& getScene () { return m_scene; }
    /** export 2 with the new render size (sub_14017F1B0) */
    void notifyResizeScreen (const glm::vec2& size);
    /** export 4 with the changed properties (sub_1401731D0) */
    void notifyUserPropertiesChanged (const std::vector<std::string>& names);
    std::optional<std::string> readScriptAsset (const std::string& path) const;

    v8::MaybeLocal<v8::Value> callAsModule (const std::string& key, v8::Local<v8::Function> callback);

    /** The module an import names, compiled, instantiated and evaluated once per engine like scenescript64's
     *  resolver (sub_1816469C0) */
    v8::MaybeLocal<v8::Module> importModule (const std::string& specifier);

private:
    /**
     * Gives the JS that runs while it lives a time budget (the outermost one arms the watchdog, a call from a script
     * into another one shares it). A termination can still be pending when the watchdog fired as a call ended, the
     * outermost one cancels it once nothing is left to unwind
     */
    class RunLimit {
    public:
	explicit RunLimit (ScriptEngine& engine);
	~RunLimit ();
	RunLimit (const RunLimit&) = delete;
	RunLimit& operator= (const RunLimit&) = delete;

    private:
	ScriptEngine& m_engine;
    };

    /** After a call into JS came back empty: when V8 was terminating it, say why and switch off its module (nullptr for
     * a call without one, owner then names it in the log) */
    void stopRunaway (const LoadedModule* culprit, const char* what, std::string_view owner = {});
    void restoreHeapLimit ();
    static size_t nearHeapLimit (void* data, size_t currentLimit, size_t initialLimit);

    v8::MaybeLocal<v8::Value>
    call (const LoadedModule& module, const char* name, int argc, v8::Local<v8::Value> argv[]);
    void evaluateGlobalScript (const std::string& source, const char* name);
    v8::Local<v8::Value> textLayer (ScriptLayerHandle handle);

    void installBuiltins ();

    Media::ThumbnailPalette thumbnailPaletteFor (const Media::MediaSource::MediaInfo& media);
    v8::MaybeLocal<v8::Value> callEvent (LoadedModule& module, const char* name, int argc, v8::Local<v8::Value> argv[]);
    enum class MediaChange { Metadata, Thumbnail };
    void
    notifyMediaUpdate (const Media::MediaSource::MediaInfo& media, MediaChange change, LoadedModule* only = nullptr);
    void initializeModule (const std::string& key, LoadedModule& module);
    void bindThisLayer (ScriptableObject& object, LoadedModule* module = nullptr);
    v8::Local<v8::Object> initialUserProperties ();

public:
    /** IAnimation of the animation getAnimation (name) finds on objectId's layer, or any layer without one; empty
     *  when there is none */
    v8::MaybeLocal<v8::Value> findAnimation (const std::string& name, std::optional<int> objectId);
    v8::MaybeLocal<v8::Value> findAnimation (const std::string& name, const std::string& group);

private:
    void dispatchAnimationEvents ();

    void ensureLayerRegistry ();

    std::unique_ptr<v8::ArrayBuffer::Allocator> m_allocator;
    v8::Isolate* m_isolate = nullptr;
    // how long one call into a script may run and how much memory its heap may take, 0 is no limit
    std::chrono::milliseconds m_callBudget;
    size_t m_heapLimit;
    std::unique_ptr<ScriptWatchdog> m_watchdog;
    int m_runDepth = 0;
    // the call in progress, or the last one, was stopped and reported. Reset when the next outermost call starts
    bool m_runawayReported = false;
    ErrorRepeatFilter m_errorFilter;
    // the heap hit its limit during the call in progress, nearHeapLimit() terminated it
    bool m_heapExhausted = false;
    bool m_heapLimitRaised = false;
    v8::Global<v8::Context> m_context;
    std::map<std::string, v8::Global<v8::Module>> m_imports;
    Render::Wallpapers::CScene& m_scene;
    std::unique_ptr<EngineObject> m_engineObject;
    std::unique_ptr<InputObject> m_inputObject;
    std::unique_ptr<SceneObject> m_sceneObject;
    std::unique_ptr<ConsoleObject> m_consoleObject;
    std::unique_ptr<LocalStorageObject> m_localStorageObject;
    std::unique_ptr<ScriptPropertiesObject> m_scriptPropertiesObject;

    void eraseModule (const std::string& key);
    std::map<std::string, LoadedModule> m_scriptModules = {};
    uint64_t m_nextModuleOrder = 0;
    std::vector<std::string> m_retiredScriptKeys = {};

    LoadedModule* m_runningModule = nullptr;
    bool m_evaluatingModuleBody = false;
    bool m_runningUpdate = false;

    struct AnimationLayerEndedCallback {
	const ScriptableObject* owner;
	size_t serial;
	v8::Global<v8::Function> callback;
	const LoadedModule* module;
    };
    std::vector<AnimationLayerEndedCallback> m_animationLayerEndedCallbacks = {};

    ScriptLayerHandle m_nextLayerId = 1;
    bool m_layerRegistryReady = false;
    std::map<ScriptLayerHandle, bool> m_layerInitialized;
    Media::MediaSource& m_mediaSource;
    std::function<void ()> m_unregisterMediaUpdateCallback;
    std::function<void ()> m_unregisterAlbumArtUpdateCallback;
    std::filesystem::path m_palettePath;
    bool m_buttonStateRead = false;
    Media::ThumbnailPalette m_palette;

    JSObjectAdapters m_adapters;
    AnimationSystem m_animations;
};
}
