#include "ScriptEngine.h"

#include "WallpaperEngine/VideoPlayback/MPV/GLPlayer.h"
#include "WallpaperEngine/Media/ThumbnailPalette.h"

#include "Adapters/ScriptableObjectAdapter.h"
#include "Modules/ColorModule.h"
#include "Modules/MathModule.h"
#include "Modules/ScriptModule.h"
#include "ScriptPropertiesObject.h"
#include "ScriptableObject.h"
#include "WallpaperEngine/Audio/AudioContext.h"
#include "WallpaperEngine/Audio/Drivers/Recorders/PlaybackRecorder.h"
#include "WallpaperEngine/Desktop/UserShortcut.h"
#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Data/Utils/ScopeGuard.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/CObject.h"
#include "WallpaperEngine/Render/Objects/CSound.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include "WallpaperEngine/Scripting/Builtins.generated.h"
#include "quickjs.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <future>
#include <optional>
#include <poll.h>
#include <ranges>
#include <signal.h>
#include <spawn.h>
#include <sstream>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace WallpaperEngine::Render::Objects {
class CSound;
}
using namespace WallpaperEngine::Scripting;
using namespace WallpaperEngine::Data::Model;

extern char** environ;
extern float g_Time;
extern float g_TimeLast;

void scriptengine_dump (JSContext* ctx, JSValueConst obj) {
    JSPropertyEnum* props;
    uint32_t len;

    if (JS_GetOwnPropertyNames (ctx, &props, &len, obj, JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) < 0) {
	return;
    }

    for (uint32_t i = 0; i < len; ++i) {
	const char* name = JS_AtomToCString (ctx, props[i].atom);

	JSValue val = JS_GetProperty (ctx, obj, props[i].atom);

	const char* value_str = JS_ToCString (ctx, val);

	printf ("%s = %s\n", name, value_str ? value_str : "<non-string>");

	JS_FreeCString (ctx, value_str);
	JS_FreeValue (ctx, val);
	JS_FreeCString (ctx, name);
    }

    js_free (ctx, props);
}

JSModuleDef* scriptengine_module_loader (JSContext* ctx, const char* module, void* opaque) {
    const auto* scriptEngine = static_cast<ScriptEngine*> (opaque);

    const auto& modules = scriptEngine->getModules ();
    const auto it = modules.find (module);

    if (it == modules.end ()) {
	return nullptr;
    }

    return it->second->getDefinition ();
}

JSValue ScriptEngine::dynamicToJs (DynamicValue& value) const {
    switch (value.getType ()) {
	case DynamicValue::Null:
	    return JS_NULL;
	case DynamicValue::String:
	    return JS_NewString (this->m_context, value.getString ().c_str ());
	case DynamicValue::Float:
	    return JS_NewFloat64 (this->m_context, value.getFloat ());
	case DynamicValue::Int:
	    return JS_NewInt32 (this->m_context, value.getInt ());
	case DynamicValue::Boolean:
	    return JS_NewBool (this->m_context, value.getBool ());
	case DynamicValue::Vec2:
	    return this->m_adapters.vec2->instantiate (value, true);
	case DynamicValue::Vec3:
	    return this->m_adapters.vec3->instantiate (value, true);
	case DynamicValue::Vec4:
	    return this->m_adapters.vec4->instantiate (value, true);
	default:
	    return JS_UNDEFINED;
    }
}

bool ScriptEngine::hasScript (const DynamicValue& value) const {
    return std::ranges::any_of (this->m_scriptModules, [&value] (const auto& entry) {
	return &entry.second.value == &value;
    });
}

JSValue ScriptEngine::userPropertyToJs (Property& property) const {
    // same shape the real engine hands scripts (jsclasses/baseclasses.js), never the command itself
    if (property.is<PropertyUserShortcut> ()) {
	const auto shortcut = Desktop::UserShortcut::parse (property.getString ());
	JSValue result = JS_NewObject (this->m_context);

	JS_SetPropertyStr (this->m_context, result, "isbound", JS_NewBool (this->m_context, shortcut.has_value ()));

	return result;
    }

    if (property.is<PropertyColor> () && property.getType () == DynamicValue::Vec4) {
	DynamicValue rgb (glm::vec3 (property.getVec4 ()));

	return this->m_adapters.vec3->instantiate (rgb, true);
    }

    return this->dynamicToJs (property);
}

static void jsToDynamicValue (JSContext* ctx, JSValue val, DynamicValue& source) {
    if (JS_IsException (val)) {
	return;
    }

    int tag = JS_VALUE_GET_TAG (val);

    // update()'s contract is "return the new value"; falling off the end of a function (or an
    // early "if (cond) return x;" with no else) yields undefined and is meant as "nothing to
    // change this frame", not "reset this property to zero" - DynamicValue::update(source) with
    // no value does the latter (it's meant for genuinely-null JSON properties at parse time, see
    // DynamicValueParser), and calling it here would zero out (e.g. scale -> 0, i.e. invisible)
    // any property whose script doesn't explicitly return on every path.
    if (tag == JS_TAG_UNDEFINED || tag == JS_TAG_UNINITIALIZED || tag == JS_TAG_NULL) {
	return;
    }

    if (tag == JS_TAG_INT) {
	source.update (JS_VALUE_GET_INT (val), DynamicValue::UpdateSource::Script);
	return;
    }

    if (tag == JS_TAG_BOOL) {
	source.update (static_cast<bool> (JS_VALUE_GET_BOOL (val)), DynamicValue::UpdateSource::Script);
	return;
    }

    if (JS_TAG_IS_FLOAT64 (tag)) {
	source.update (static_cast<float> (JS_VALUE_GET_FLOAT64 (val)), DynamicValue::UpdateSource::Script);
	return;
    }

    if (tag == JS_TAG_STRING) {
	const char* str = JS_ToCString (ctx, val);
	source.update (std::string (str == nullptr ? "" : str), DynamicValue::UpdateSource::Script);
	JS_FreeCString (ctx, str);
	return;
    }

    if (tag == JS_TAG_OBJECT) {
	JSValue x = JS_GetPropertyStr (ctx, val, "x");
	JSValue y = JS_GetPropertyStr (ctx, val, "y");
	JSValue z = JS_GetPropertyStr (ctx, val, "z");
	JSValue w = JS_GetPropertyStr (ctx, val, "w");
	ScopeGuard guard ([=] {
	    JS_FreeValue (ctx, x);
	    JS_FreeValue (ctx, y);
	    JS_FreeValue (ctx, z);
	    JS_FreeValue (ctx, w);
	});

	if (!JS_IsNumber (x) || !JS_IsNumber (y)) {
	    sLog.exception ("Vector's x and y components must be numbers");
	}

	double xVal = 0.0f, yVal = 0.0f, zVal = 0.0f, wVal = 0.0f;

	JS_ToFloat64 (ctx, &xVal, x);
	JS_ToFloat64 (ctx, &yVal, y);

	if (!JS_IsNumber (z)) {
	    source.update (glm::vec2 (xVal, yVal), DynamicValue::UpdateSource::Script);
	    return;
	}

	JS_ToFloat64 (ctx, &zVal, z);

	if (!JS_IsNumber (w)) {
	    source.update (glm::vec3 (xVal, yVal, zVal), DynamicValue::UpdateSource::Script);
	    return;
	}

	JS_ToFloat64 (ctx, &wVal, w);
	source.update (glm::vec4 (xVal, yVal, zVal, wVal), DynamicValue::UpdateSource::Script);
    }
}

void ScriptEngine::assignJsValue (JSValue val, DynamicValue& target) const {
    jsToDynamicValue (this->m_context, val, target);
}

ScriptEngine::ScriptEngine (Wallpapers::CScene& scene, Media::MediaSource& mediaSource) :
    m_scene (scene), m_mediaSource (mediaSource) {
    this->m_unregisterMediaUpdateCallback
	= mediaSource.addMetadataListener ([this] (const Media::MediaSource::MediaInfo& info) {
	      this->notifyMediaUpdate (info);
	  });

    this->m_unregisterAlbumArtUpdateCallback
	= mediaSource.addAlbumArtListener ([this] (const Media::MediaSource::MediaInfo& info) {
	      // TODO: SEPARATE THESE INTO THEIR OWN UPDATES SO JS ONLY RECEIVES THE MEANINGFUL UPDATES
	      this->notifyMediaUpdate (info);
	  });

    this->m_runtime = JS_NewRuntime ();

    if (!this->m_runtime) {
	sLog.exception ("ScriptEngine: Failed to create JS runtime");
    }

    // debug leaks on termination
    JS_SetDumpFlags (this->m_runtime, JS_DUMP_LEAKS);

    this->m_context = JS_NewContext (this->m_runtime);

    if (!this->m_context) {
	JS_FreeRuntime (this->m_runtime);
	sLog.exception ("ScriptEngine: Failed to create JS context");
    }

    this->m_globalThis = JS_GetGlobalObject (this->m_context);

    this->m_adapters = {
	.vec4 = std::unique_ptr<Adapters::VectorAdapter<4>> (new Adapters::VectorAdapter<4> (*this)),
	.vec3 = std::unique_ptr<Adapters::VectorAdapter<3>> (new Adapters::VectorAdapter<3> (*this)),
	.vec2 = std::unique_ptr<Adapters::VectorAdapter<2>> (new Adapters::VectorAdapter<2> (*this)),
	.object
	= std::unique_ptr<Adapters::ScriptableObjectAdapter> (new Adapters::ScriptableObjectAdapter (*this, "ILayer")),
    };

    this->m_engineObject = std::make_unique<EngineObject> (*this, scene);
    this->m_inputObject = std::make_unique<InputObject> (*this, scene);
    this->m_sceneObject = std::make_unique<SceneObject> (*this, scene);
    this->m_consoleObject = std::make_unique<ConsoleObject> (*this, scene);
    this->m_scriptPropertiesObject = std::make_unique<ScriptPropertiesObject> (*this, scene);

    auto wemath = std::make_unique<Modules::MathModule> (*this);
    auto wecolor = std::make_unique<Modules::ColorModule> (*this);

    this->m_modules.emplace (wemath->getName (), std::move (wemath));
    this->m_modules.emplace (wecolor->getName (), std::move (wecolor));

    JS_SetModuleLoaderFunc (this->m_runtime, nullptr, scriptengine_module_loader, this);
    this->installBuiltins ();
    JS_DefinePropertyValueStr (
	this->m_context, this->m_globalThis, "engine", this->m_engineObject->getInstance (), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_context, this->m_globalThis, "input", this->m_inputObject->getInstance (), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_context, this->m_globalThis, "thisScene", this->m_sceneObject->getInstance (), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_context, this->m_globalThis, "console", this->m_consoleObject->getInstance (), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_context, this->m_globalThis, "shared", JS_NewObject (this->m_context), JS_PROP_ENUMERABLE
    );
}

ScriptEngine::~ScriptEngine () {
    this->m_unregisterMediaUpdateCallback ();
    this->m_unregisterAlbumArtUpdateCallback ();

    for (const auto& module : this->m_scriptModules | std::views::values) {
	JS_FreeValue (this->m_context, module.module);
	JS_FreeValue (this->m_context, module.thisObject);
    }

    for (const auto& entry : this->m_videoEndedCallbacks) {
	JS_FreeValue (this->m_context, entry.callback);
    }

    JS_FreeValue (this->m_context, this->m_globalThis);

    this->m_consoleObject.reset ();
    this->m_engineObject.reset ();
    this->m_inputObject.reset ();
    this->m_sceneObject.reset ();
    this->m_scriptPropertiesObject.reset ();
    this->m_modules.clear ();
    this->m_scriptModules.clear ();

    if (this->m_context) {
	JS_FreeContext (this->m_context);
    }
    if (this->m_runtime) {
	JS_FreeRuntime (this->m_runtime);
    }

    // Freeing the runtime above runs a GC pass that finalizes every still-live vector object,
    // each calling back into its owning VectorAdapter::free() to release the DynamicValue behind
    // it - so the adapters must outlive JS_FreeRuntime(), or those finalizers hit freed memory.
    this->m_adapters.vec4.reset ();
    this->m_adapters.vec3.reset ();
    this->m_adapters.vec2.reset ();
    this->m_adapters.object.reset ();
}

namespace {
// the line the first stack frame inside `context` points at, with a caret under the column
std::string sourceExcerpt (const std::string& stack, const char* context, const std::string& source) {
    const std::string marker = std::string (context) + ":";
    const auto at = stack.find (marker);

    if (at == std::string::npos) {
	return {};
    }

    int line = 0;
    int column = 0;

    if (std::sscanf (stack.c_str () + at + marker.size (), "%d:%d", &line, &column) < 1 || line < 1) {
	return {};
    }

    size_t begin = 0;

    for (int i = 1; i < line; i++) {
	begin = source.find ('\n', begin);

	if (begin == std::string::npos) {
	    return {};
	}

	begin++;
    }

    const auto lineEnd = source.find ('\n', begin);
    std::string text = source.substr (begin, lineEnd == std::string::npos ? std::string::npos : lineEnd - begin);
    std::ranges::replace (text, '\t', ' ');

    std::string result = std::to_string (line) + ": " + text;

    if (column > 0) {
	result += "\n" + std::string (std::to_string (line).size () + 2 + column - 1, ' ') + "^";
    }

    return result;
}
} // namespace

void WallpaperEngine::Scripting::logJSException (
    JSContext* ctx, const char* context, const std::optional<std::string>& source
) {
    // scripts that fail every tick would otherwise print the same error every frame
    static std::map<std::string, std::string> lastReported;

    JSValue exc = JS_GetException (ctx);
    ScopeGuard freeException ([=] { JS_FreeValue (ctx, exc); });

    if (JS_IsNull (exc) || JS_IsUndefined (exc) || JS_IsUninitialized (exc)) {
	sLog.error ("ScriptEngine [", context, "]: a native call failed without setting an exception");
	return;
    }

    std::string message;
    std::string stack;

    if (const char* str = JS_ToCString (ctx, exc); str != nullptr) {
	message = str;
	JS_FreeCString (ctx, str);
    }

    JSValue stackValue = JS_GetPropertyStr (ctx, exc, "stack");

    if (!JS_IsUndefined (stackValue)) {
	if (const char* str = JS_ToCString (ctx, stackValue); str != nullptr) {
	    stack = str;
	    JS_FreeCString (ctx, str);
	}
    }

    JS_FreeValue (ctx, stackValue);

    auto& last = lastReported[context];

    if (last == message + stack) {
	return;
    }

    last = message + stack;

    sLog.error ("ScriptEngine [", context, "]: ", message);

    if (!stack.empty ()) {
	sLog.error ("ScriptEngine [", context, "] stack: ", stack);
    }

    if (source.has_value ()) {
	if (const auto excerpt = sourceExcerpt (stack, context, *source); !excerpt.empty ()) {
	    sLog.error ("ScriptEngine [", context, "] source:\n", excerpt);
	}
    }
}

void ScriptEngine::installBuiltins () {
    if (this->m_builtinsInstalled || !this->m_context) {
	return;
    }

    JSValue result = JS_Eval (
	this->m_context, SCENE_SCRIPT_BUILTINS, strlen (SCENE_SCRIPT_BUILTINS), "<scene-script-builtins>",
	JS_EVAL_TYPE_GLOBAL
    );
    if (JS_IsException (result)) {
	logJSException (this->m_context, "installBuiltins");
    }
    JS_FreeValue (this->m_context, result);
    this->m_builtinsInstalled = true;
}

// Layer-script API (Phase 2)

void ScriptEngine::ensureLayerRegistry () {
    if (this->m_layerRegistryReady || !this->m_context) {
	return;
    }
    JSContext* ctx = this->m_context;
    JSValue globalObj = JS_GetGlobalObject (ctx);
    JS_SetPropertyStr (ctx, globalObj, "__textLayers", JS_NewObject (ctx));
    JS_FreeValue (ctx, globalObj);
    this->m_layerRegistryReady = true;
}

ScriptLayerHandle ScriptEngine::createLayerScript (
    const std::string& scriptSource, std::map<std::string, UserSettingUniquePtr>& initialScriptProps,
    const std::string& initialText
) {
    if (!this->m_context) {
	sLog.error ("ScriptEngine: No JS context available");
	return kInvalidLayerHandle;
    }

    this->ensureLayerRegistry ();

    JSContext* ctx = this->m_context;
    JSValue globalObj = JS_GetGlobalObject (ctx);

    // Seed initial scriptProperties and text as temporary globals the IIFE reads.
    JSValue seedProps = JS_NewObject (ctx);

    for (auto& [name, dynVal] : initialScriptProps) {
	JS_SetPropertyStr (ctx, seedProps, name.c_str (), this->dynamicToJs (*dynVal->value));
    }

    JS_SetPropertyStr (ctx, globalObj, "__layerSeedProps", seedProps);
    JS_SetPropertyStr (ctx, globalObj, "__layerSeedText", JS_NewString (ctx, initialText.c_str ()));

    const ScriptLayerHandle id = this->m_nextLayerId++;

    // Same stripping logic as evaluate(): WE scripts come as ES6 modules but
    // QuickJS is easier to drive as plain script evaluation.
    std::string body = scriptSource;
    size_t pos;
    while ((pos = body.find ("'use strict';")) != std::string::npos) {
	body.erase (pos, 13);
    }
    while ((pos = body.find ("\"use strict\";")) != std::string::npos) {
	body.erase (pos, 13);
    }
    while ((pos = body.find ("export ")) != std::string::npos) {
	body.erase (pos, 7);
    }

    // The IIFE gives every layer its own closure so two layers both defining `function update()`
    // or a top-level `var scriptProperties` don't clobber each other. Lifecycle hooks are
    // captured into globalThis.__textLayers[id] so tick/destroy can reach them later.
    // `typeof init === 'function'` is safe even when `init` was never declared.
    std::ostringstream wrapper;
    wrapper
	<< "(function() {\n"
	<< "  var __id = " << id << ";\n"
	<< "  var __props = Object.assign({}, globalThis.__layerSeedProps || {});\n"
	<< "  var thisLayer = { text: String(globalThis.__layerSeedText || '') };\n"
	<< "  var thisScene = {\n"
	<< "    get time()        { var c = globalThis.__sceneCtx; return c ? c.time : 0; },\n"
	<< "    get currentTime() { var c = globalThis.__sceneCtx; return c ? c.time : 0; },\n"
	<< "    get dt()          { var c = globalThis.__sceneCtx; return c ? c.dt   : 0; },\n"
	<< "    get fps()         { var c = globalThis.__sceneCtx; return c ? c.fps  : 60; },\n"
	<< "  };\n"
	// Minimal WE `engine` shim - just enough for built-in text scripts to run without
	// ReferenceError. `frametime` is the per-frame delta in seconds (what InsertFPS reads).
	<< "  var engine = {\n"
	<< "    get frametime() { var c = globalThis.__sceneCtx; return c ? c.dt : 0; },\n"
	<< "    get time()      { var c = globalThis.__sceneCtx; return c ? c.time : 0; },\n"
	<< "  };\n"
	<< "  function createScriptProperties() {\n"
	<< "    var builder = {\n"
	<< "      addSlider:   function(o){ if (!(o.name in __props)) __props[o.name] = o.value; return builder; },\n"
	<< "      addCheckbox: function(o){ if (!(o.name in __props)) __props[o.name] = o.value; return builder; },\n"
	<< "      addCombo:    function(o){ if (!(o.name in __props)) __props[o.name] = o.value; return builder; },\n"
	<< "      addColor:    function(o){ if (!(o.name in __props)) __props[o.name] = o.value; return builder; },\n"
	<< "      addText:     function(o){ if (!(o.name in __props)) __props[o.name] = o.value; return builder; },\n"
	<< "      finish:      function(){ return __props; }\n"
	<< "    };\n"
	<< "    return builder;\n"
	<< "  }\n"
	<< body
	<< "\n"
	// `_tick` wraps the user's `update()` so both WE text conventions work: mutating
	// `thisLayer.text` in place, or returning the new text as a string. Non-string/undefined
	// return leaves `thisLayer.text` as whatever the function assigned itself.
	<< "  globalThis.__textLayers[__id] = {\n"
	<< "    thisLayer: thisLayer,\n"
	<< "    thisScene: thisScene,\n"
	<< "    _init:    (typeof init    === 'function') ? init    : null,\n"
	<< "    _destroy: (typeof destroy === 'function') ? destroy : null,\n"
	<< "    _tick:    (typeof update  === 'function')\n"
	<< "              ? function() {\n"
	<< "                  var r = update(thisLayer.text);\n"
	<< "                  if (typeof r === 'string') thisLayer.text = r;\n"
	<< "                }\n"
	<< "              : null,\n"
	<< "    _scriptProperties: (typeof scriptProperties !== 'undefined') ? scriptProperties : __props\n"
	<< "  };\n"
	<< "})();\n";

    const std::string evalScript = wrapper.str ();
    JSValue result = JS_Eval (ctx, evalScript.c_str (), evalScript.size (), "<layer-script>", JS_EVAL_TYPE_GLOBAL);

    // Unset seeds so they don't leak into the next createLayerScript call.
    JS_SetPropertyStr (ctx, globalObj, "__layerSeedProps", JS_UNDEFINED);
    JS_SetPropertyStr (ctx, globalObj, "__layerSeedText", JS_UNDEFINED);
    JS_FreeValue (ctx, globalObj);

    if (JS_IsException (result)) {
	logJSException (ctx, "createLayerScript");
	JS_FreeValue (ctx, result);
	return kInvalidLayerHandle;
    }
    JS_FreeValue (ctx, result);

    this->m_layerInitialized[id] = false;
    return id;
}

void ScriptEngine::tickLayer (ScriptLayerHandle handle, double time, double deltaTime, double fps) {
    if (!this->m_context || handle == kInvalidLayerHandle) {
	return;
    }
    JSContext* ctx = this->m_context;
    JSValue globalObj = JS_GetGlobalObject (ctx);

    JSValue sceneCtx = JS_NewObject (ctx);
    JS_SetPropertyStr (ctx, sceneCtx, "time", JS_NewFloat64 (ctx, time));
    JS_SetPropertyStr (ctx, sceneCtx, "dt", JS_NewFloat64 (ctx, deltaTime));
    JS_SetPropertyStr (ctx, sceneCtx, "fps", JS_NewFloat64 (ctx, fps));
    JS_SetPropertyStr (ctx, globalObj, "__sceneCtx", sceneCtx);

    JSValue layers = JS_GetPropertyStr (ctx, globalObj, "__textLayers");
    JSValue layerObj = JS_GetPropertyUint32 (ctx, layers, static_cast<uint32_t> (handle));
    JS_FreeValue (ctx, layers);

    if (JS_IsUndefined (layerObj) || JS_IsNull (layerObj)) {
	JS_FreeValue (ctx, layerObj);
	JS_FreeValue (ctx, globalObj);
	return;
    }

    auto callHook = [&] (const char* prop, const char* tag) {
	JSValue fn = JS_GetPropertyStr (ctx, layerObj, prop);
	if (JS_IsFunction (ctx, fn)) {
	    JSValue ret = JS_Call (ctx, fn, layerObj, 0, nullptr);
	    if (JS_IsException (ret)) {
		logJSException (ctx, tag);
	    }
	    JS_FreeValue (ctx, ret);
	}
	JS_FreeValue (ctx, fn);
    };

    auto it = this->m_layerInitialized.find (handle);
    if (it != this->m_layerInitialized.end () && !it->second) {
	callHook ("_init", "layer.init");
	it->second = true;
    }
    callHook ("_tick", "layer.update");

    JS_FreeValue (ctx, layerObj);
    JS_FreeValue (ctx, globalObj);
}

std::string ScriptEngine::layerText (ScriptLayerHandle handle) {
    if (!this->m_context || handle == kInvalidLayerHandle) {
	return {};
    }
    JSContext* ctx = this->m_context;
    JSValue globalObj = JS_GetGlobalObject (ctx);
    JSValue layers = JS_GetPropertyStr (ctx, globalObj, "__textLayers");
    JSValue layerObj = JS_GetPropertyUint32 (ctx, layers, static_cast<uint32_t> (handle));

    std::string result;
    if (!JS_IsUndefined (layerObj) && !JS_IsNull (layerObj)) {
	JSValue thisLayer = JS_GetPropertyStr (ctx, layerObj, "thisLayer");
	JSValue textVal = JS_GetPropertyStr (ctx, thisLayer, "text");
	if (!JS_IsUndefined (textVal) && !JS_IsNull (textVal)) {
	    const char* cstr = JS_ToCString (ctx, textVal);
	    if (cstr) {
		result.assign (cstr);
		JS_FreeCString (ctx, cstr);
	    }
	}
	JS_FreeValue (ctx, textVal);
	JS_FreeValue (ctx, thisLayer);
    }

    JS_FreeValue (ctx, layerObj);
    JS_FreeValue (ctx, layers);
    JS_FreeValue (ctx, globalObj);
    return result;
}

void ScriptEngine::destroyLayer (ScriptLayerHandle handle) {
    if (!this->m_context || handle == kInvalidLayerHandle) {
	return;
    }
    JSContext* ctx = this->m_context;
    JSValue globalObj = JS_GetGlobalObject (ctx);
    JSValue layers = JS_GetPropertyStr (ctx, globalObj, "__textLayers");
    JSValue layerObj = JS_GetPropertyUint32 (ctx, layers, static_cast<uint32_t> (handle));

    if (!JS_IsUndefined (layerObj) && !JS_IsNull (layerObj)) {
	JSValue fn = JS_GetPropertyStr (ctx, layerObj, "_destroy");
	if (JS_IsFunction (ctx, fn)) {
	    JSValue ret = JS_Call (ctx, fn, layerObj, 0, nullptr);
	    if (JS_IsException (ret)) {
		logJSException (ctx, "layer.destroy");
	    }
	    JS_FreeValue (ctx, ret);
	}
	JS_FreeValue (ctx, fn);
    }
    JS_FreeValue (ctx, layerObj);
    JS_FreeValue (ctx, layers);
    JS_FreeValue (ctx, globalObj);

    // Remove the entry from globalThis.__textLayers so GC can reclaim its closures.
    const std::string delScript = "delete globalThis.__textLayers[" + std::to_string (handle) + "];";
    JSValue delResult = JS_Eval (ctx, delScript.c_str (), delScript.size (), "<layer-destroy>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException (delResult)) {
	logJSException (ctx, "layer.destroy.delete");
    }
    JS_FreeValue (ctx, delResult);

    this->m_layerInitialized.erase (handle);
}

JSValue ScriptEngine::call (JSValue module, int argc, JSValue argv[], const char* name) {
    JSValue function = JS_GetPropertyStr (this->m_context, module, name);
    ScopeGuard guard ([&] () { JS_FreeValue (this->m_context, function); });

    if (!JS_IsFunction (this->m_context, function)) {
	return JS_UNDEFINED;
    }

    return JS_Call (this->m_context, function, module, argc, argv);
}

void ScriptEngine::retireScript (const std::string& key) { this->m_retiredScriptKeys.push_back (key); }

bool ScriptEngine::rebindScript (const std::string& key, DynamicValue& newValue) {
    const auto it = this->m_scriptModules.find (key);

    if (it == this->m_scriptModules.end () || it->second.value.getScriptSource () != newValue.getScriptSource ()) {
	return false;
    }

    // LoadedModule::value is a reference member, so the entry is erased and re-emplaced under the same key,
    // m_runningModule may currently point at the erased entry
    // the cached thisObject points at the old value, the replacement builds its own
    JS_FreeValue (this->m_context, it->second.thisObject);
    LoadedModule replacement { .value = newValue,
			       .module = it->second.module,
			       .object = it->second.object,
			       .propertyName = it->second.propertyName };
    this->m_scriptModules.erase (it);
    const auto inserted = this->m_scriptModules.emplace (key, replacement);
    this->m_runningModule = &inserted.first->second;

    return true;
}

void ScriptEngine::queueScript (
    const std::string& key, DynamicValue& currentValue, ScriptableObject& object, const std::string& propertyName
) {
    const auto source = currentValue.getScriptSource ();

    if (!source.has_value ()) {
	return;
    }

    auto it = this->m_scriptModules.find (key);

    if (it != this->m_scriptModules.end ()) {
	return;
    }

    // Compile-only first: JS_Eval(..., JS_EVAL_TYPE_MODULE) alone compiles AND evaluates in one
    // step, but its return value is always a Promise (the module's completion value), never the
    // namespace object. Compiling separately keeps the JS_TAG_MODULE value around so the real
    // namespace can be fetched via JS_GetModuleNamespace afterward.
    JSValue compiledModule = JS_Eval (
	this->m_context, source->c_str (), source->size (), key.c_str (),
	JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY
    );

    if (JS_IsException (compiledModule)) {
	logJSException (this->m_context, key.c_str (), source);
	return;
    }

    auto* moduleDef = static_cast<JSModuleDef*> (JS_VALUE_GET_PTR (compiledModule));

    // Register the entry (with a placeholder module value) and point m_runningModule at it before
    // evaluating below - top-level module code commonly does `export var scriptProperties =
    // createScriptProperties()...finish();`, and scriptpropertiescreator_finish()
    // (ScriptPropertiesObject.cpp) resolves it through getRunningModule(), which must already
    // point here.
    auto inserted = this->m_scriptModules.emplace (
	key,
	LoadedModule {
	    .value = currentValue,
	    .module = JS_UNDEFINED,
	    .object = &object,
	    .propertyName = propertyName,
	}
    );

    if (!inserted.second) {
	JS_FreeValue (this->m_context, compiledModule);
	return;
    }

    this->m_runningModule = &inserted.first->second;

    // module top-level code (class bodies, helpers instantiated on load) can already touch thisLayer
    this->bindThisLayer (object, this->m_runningModule);

    // JS_EvalFunction runs the module body and returns a Promise - for a module with no top-level
    // await this resolves/rejects synchronously, so its state can be checked right away.
    // JS_IsException() on it is always false even if the module threw, since that exception gets
    // caught by the module machinery and stored as the promise's rejection reason instead.
    JSValue evalResult = JS_EvalFunction (this->m_context, compiledModule);

    if (JS_PromiseState (this->m_context, evalResult) == JS_PROMISE_REJECTED) {
	JSValue reason = JS_PromiseResult (this->m_context, evalResult);
	const char* str = JS_ToCString (this->m_context, reason);
	sLog.error ("ScriptEngine [", key, "] module evaluation rejected: ", str != nullptr ? str : "(no message)");
	if (str) {
	    JS_FreeCString (this->m_context, str);
	}
	JS_FreeValue (this->m_context, reason);
	JS_FreeValue (this->m_context, evalResult);
	this->m_runningModule = nullptr;
	this->m_scriptModules.erase (inserted.first);
	return;
    }

    JS_FreeValue (this->m_context, evalResult);

    JSValue module = JS_GetModuleNamespace (this->m_context, moduleDef);

    if (JS_IsException (module)) {
	logJSException (this->m_context, key.c_str ());
	this->m_runningModule = nullptr;
	this->m_scriptModules.erase (inserted.first);
	return;
    }

    inserted.first->second.module = module;

    this->bindThisLayer (object, this->m_runningModule);

    // init() and the first update() run from tick(), once the whole scene has been constructed
}

namespace {
enum AnimationOp {
    AnimPlay,
    AnimPause,
    AnimStop,
    AnimGetFrame,
    AnimSetFrame,
    AnimIsPlaying,
    AnimGetRate,
    AnimSetRate,
    AnimGetFrameCount,
    AnimGetName,
    AnimGetFps,
};

// backs every method/accessor of the IAnimation handle; func_data is [animation system id, clock id]
JSValue animation_access (
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValueConst* func_data
) {
    int systemId = 0;
    int clockId = 0;
    JS_ToInt32 (ctx, &systemId, func_data[0]);
    JS_ToInt32 (ctx, &clockId, func_data[1]);

    auto* system = AnimationSystem::find (systemId);
    auto* clock = system == nullptr ? nullptr : system->clock (clockId);

    if (clock == nullptr) {
	return JS_UNDEFINED;
    }

    double number = 0.0;

    switch (magic) {
	case AnimPlay:
	    clock->play ();
	    return JS_UNDEFINED;
	case AnimPause:
	    clock->pause ();
	    return JS_UNDEFINED;
	case AnimStop:
	    clock->stop ();
	    return JS_UNDEFINED;
	case AnimGetFrame:
	    return JS_NewFloat64 (ctx, clock->getFrame ());
	case AnimSetFrame:
	    if (argc > 0 && JS_ToFloat64 (ctx, &number, argv[0]) == 0) {
		clock->setFrame (static_cast<float> (number));
	    }
	    return JS_UNDEFINED;
	case AnimIsPlaying:
	    return JS_NewBool (ctx, clock->isPlaying ());
	case AnimGetRate:
	    return JS_NewFloat64 (ctx, clock->getRate ());
	case AnimSetRate:
	    if (argc > 0 && JS_ToFloat64 (ctx, &number, argv[0]) == 0) {
		clock->setRate (static_cast<float> (number));
	    }
	    return JS_UNDEFINED;
	case AnimGetFrameCount:
	    return JS_NewFloat64 (ctx, clock->getDefinition ().length);
	case AnimGetName:
	    return JS_NewString (ctx, clock->getDefinition ().name.c_str ());
	case AnimGetFps:
	    return JS_NewFloat64 (ctx, clock->getDefinition ().fps);
	default:
	    return JS_UNDEFINED;
    }
}

JSValue makeAnimationHandle (JSContext* ctx, int systemId, int clockId) {
    JSValue handle = JS_NewObject (ctx);
    JSValue data[] = { JS_NewInt32 (ctx, systemId), JS_NewInt32 (ctx, clockId) };

    const auto method = [&] (const char* name, int op, int length) {
	JS_SetPropertyStr (ctx, handle, name, JS_NewCFunctionData (ctx, animation_access, length, op, 2, data));
    };
    const auto accessor = [&] (const char* name, int getter, int setter) {
	JSAtom atom = JS_NewAtom (ctx, name);
	JSValue get = JS_NewCFunctionData (ctx, animation_access, 0, getter, 2, data);
	JSValue set = setter < 0 ? JS_UNDEFINED : JS_NewCFunctionData (ctx, animation_access, 1, setter, 2, data);
	JS_DefinePropertyGetSet (ctx, handle, atom, get, set, JS_PROP_ENUMERABLE);
	JS_FreeAtom (ctx, atom);
    };

    method ("play", AnimPlay, 0);
    method ("pause", AnimPause, 0);
    method ("stop", AnimStop, 0);
    method ("getFrame", AnimGetFrame, 0);
    method ("setFrame", AnimSetFrame, 1);
    method ("isPlaying", AnimIsPlaying, 0);
    accessor ("rate", AnimGetRate, AnimSetRate);
    accessor ("frameCount", AnimGetFrameCount, -1);
    accessor ("name", AnimGetName, -1);
    accessor ("fps", AnimGetFps, -1);

    JS_FreeValue (ctx, data[0]);
    JS_FreeValue (ctx, data[1]);

    return handle;
}

// thisObject.getAnimation(): func_data is [animation system id, address of the property's DynamicValue]
JSValue this_object_get_animation (
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValueConst* func_data
) {
    int systemId = 0;
    int64_t address = 0;
    JS_ToInt32 (ctx, &systemId, func_data[0]);
    JS_ToInt64 (ctx, &address, func_data[1]);

    auto* system = AnimationSystem::find (systemId);
    auto* value = reinterpret_cast<DynamicValue*> (static_cast<intptr_t> (address));
    auto* clock = system == nullptr ? nullptr : system->clockOf (*value);

    if (clock == nullptr) {
	return JS_NULL;
    }

    return makeAnimationHandle (ctx, systemId, clock->getId ());
}
} // namespace

// thisObject.<property name>: lets a listener registered by a property's script write that property
// from outside (`listener.thisObject['Inner Color'] = color`). func_data is [engine address, value address]
JSValue this_object_property (
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValueConst* func_data
) {
    int64_t engineAddress = 0;
    int64_t valueAddress = 0;
    JS_ToInt64 (ctx, &engineAddress, func_data[0]);
    JS_ToInt64 (ctx, &valueAddress, func_data[1]);

    auto* engine = reinterpret_cast<ScriptEngine*> (static_cast<intptr_t> (engineAddress));
    auto* value = reinterpret_cast<DynamicValue*> (static_cast<intptr_t> (valueAddress));

    if (magic == 0) {
	return engine->dynamicToJs (*value);
    }

    if (argc > 0) {
	engine->assignJsValue (argv[0], *value);
    }

    return JS_UNDEFINED;
}

JSValue ScriptEngine::makeThisObject (DynamicValue& value, const std::string& propertyName) {
    JSValue handle = JS_NewObject (this->m_context);
    JSValue data[] = {
	JS_NewInt32 (this->m_context, this->m_animations.getId ()),
	JS_NewInt64 (this->m_context, static_cast<int64_t> (reinterpret_cast<intptr_t> (&value))),
    };

    JS_SetPropertyStr (
	this->m_context, handle, "getAnimation",
	JS_NewCFunctionData (this->m_context, this_object_get_animation, 0, 0, 2, data)
    );
    JS_FreeValue (this->m_context, data[0]);
    JS_FreeValue (this->m_context, data[1]);

    if (!propertyName.empty ()) {
	JSValue propertyData[] = {
	    JS_NewInt64 (this->m_context, static_cast<int64_t> (reinterpret_cast<intptr_t> (this))),
	    JS_NewInt64 (this->m_context, static_cast<int64_t> (reinterpret_cast<intptr_t> (&value))),
	};
	JSAtom atom = JS_NewAtom (this->m_context, propertyName.c_str ());

	JS_DefinePropertyGetSet (
	    this->m_context, handle, atom,
	    JS_NewCFunctionData (this->m_context, this_object_property, 0, 0, 2, propertyData),
	    JS_NewCFunctionData (this->m_context, this_object_property, 1, 1, 2, propertyData), JS_PROP_ENUMERABLE
	);
	JS_FreeAtom (this->m_context, atom);
	JS_FreeValue (this->m_context, propertyData[0]);
	JS_FreeValue (this->m_context, propertyData[1]);
    }

    return handle;
}

void ScriptEngine::bindThisLayer (ScriptableObject& object, LoadedModule* module) {
    JS_SetPropertyStr (this->m_context, this->m_globalThis, "thisLayer", this->m_adapters.object->instantiate (object));

    // thisObject is the property the running script is attached to (what getAnimation() hangs
    // off), not the layer
    if (module == nullptr) {
	JS_SetPropertyStr (
	    this->m_context, this->m_globalThis, "thisObject", this->m_adapters.object->instantiate (object)
	);
	return;
    }

    if (JS_IsUndefined (module->thisObject)) {
	module->thisObject = this->makeThisObject (module->value, module->propertyName);
    }

    JS_SetPropertyStr (
	this->m_context, this->m_globalThis, "thisObject", JS_DupValue (this->m_context, module->thisObject)
    );
}

void ScriptEngine::dispatchAnimationEvents () {
    for (const auto& event : this->m_animations.takeEvents ()) {
	const auto* clock = this->m_animations.clock (event.clock);

	if (clock == nullptr) {
	    continue;
	}

	for (auto& [key, module] : this->m_scriptModules) {
	    if (&module.value != &clock->getRootValue () || !module.initialized) {
		continue;
	    }

	    this->m_runningModule = &module;

	    if (module.object != nullptr) {
		this->bindThisLayer (*module.object, &module);
	    }

	    JSValue eventObject = JS_NewObject (this->m_context);
	    JS_SetPropertyStr (this->m_context, eventObject, "name", JS_NewString (this->m_context, event.name.c_str ()));
	    JS_SetPropertyStr (this->m_context, eventObject, "frame", JS_NewFloat64 (this->m_context, event.frame));

	    JSValue args[] = { eventObject, this->dynamicToJs (module.value) };
	    JSValue result = this->call (module.module, 2, args, "animationEvent");

	    if (JS_IsException (result)) {
		logJSException (this->m_context, key.c_str (), module.value.getScriptSource ());
	    } else {
		// like update(), the handler's return value becomes the property's new value
		jsToDynamicValue (this->m_context, result, module.value);
	    }

	    JS_FreeValue (this->m_context, result);
	    JS_FreeValue (this->m_context, args[0]);
	    JS_FreeValue (this->m_context, args[1]);
	    break;
	}
    }
}

void ScriptEngine::initializeModule (const std::string& key, LoadedModule& module) {
    module.initialized = true;

    // init() receives the property's static/base value exactly once, before update() starts being
    // called every tick - scripts commonly stash it (e.g. audio-reactive scale scripts scaling a
    // captured base value) and would otherwise read undefined forever.
    JSValue initArgs[] = { this->dynamicToJs (module.value) };
    const DynamicValue valueBeforeInit (module.value);
    JSValue initResult = this->call (module.module, 1, initArgs, "init");

    if (JS_IsException (initResult)) {
	logJSException (this->m_context, key.c_str (), module.value.getScriptSource ());
    } else if (
	valueBeforeInit.getType () == module.value.getType () && valueBeforeInit.getVec4 () == module.value.getVec4 ()
	&& valueBeforeInit.getString () == module.value.getString ()
    ) {
	// init() may hand back a different starting value, unless something already moved the property meanwhile
	jsToDynamicValue (this->m_context, initResult, module.value);
    }

    JS_FreeValue (this->m_context, initResult);
    JS_FreeValue (this->m_context, initArgs[0]);

    // the real engine hands every user property to applyUserProperties() once at load (and only the
    // changed ones afterward) - scripts like music selectors rely on that first call to start up
    JSValue userProps = JS_NewObject (this->m_context);
    for (const auto& [name, property] : this->m_engineObject->getScene ().getUserProperties ()) {
	JS_SetPropertyStr (this->m_context, userProps, name.c_str (), this->userPropertyToJs (*property));
    }
    JSValue applyArgs[] = { userProps };
    JSValue applyResult = this->call (module.module, 1, applyArgs, "applyUserProperties");

    if (JS_IsException (applyResult)) {
	logJSException (this->m_context, key.c_str (), module.value.getScriptSource ());
    }

    JS_FreeValue (this->m_context, applyResult);
    JS_FreeValue (this->m_context, userProps);

    if (this->m_mediaSource.getMediaInfo ().available) {
	this->notifyMediaUpdate (this->m_mediaSource.getMediaInfo (), &module);
    }
}

namespace {
constexpr const char* CURSOR_HANDLERS[] = { "cursorEnter", "cursorLeave", "cursorMove",
					    "cursorDown",  "cursorUp",	  "cursorClick" };
}

bool ScriptEngine::hasCursorHandlers (const ScriptableObject& object) {
    for (auto& module : this->m_scriptModules | std::views::values) {
	if (module.object != &object || !module.initialized) {
	    continue;
	}

	if (module.cursorHandlers < 0) {
	    module.cursorHandlers = 0;

	    for (const char* handler : CURSOR_HANDLERS) {
		JSValue function = JS_GetPropertyStr (this->m_context, module.module, handler);

		if (JS_IsFunction (this->m_context, function)) {
		    module.cursorHandlers = 1;
		}

		JS_FreeValue (this->m_context, function);
	    }
	}

	if (module.cursorHandlers > 0) {
	    return true;
	}
    }

    return false;
}

void ScriptEngine::dispatchCursorEvent (
    const char* handler, ScriptableObject& object, const glm::vec2& worldPosition, const glm::vec2& localPosition
) {
    const auto makePosition = [this] (const glm::vec2& value) {
	JSValue result = JS_NewObject (this->m_context);

	JS_SetPropertyStr (this->m_context, result, "x", JS_NewFloat64 (this->m_context, value.x));
	JS_SetPropertyStr (this->m_context, result, "y", JS_NewFloat64 (this->m_context, value.y));
	JS_SetPropertyStr (this->m_context, result, "z", JS_NewFloat64 (this->m_context, 0.0));

	return result;
    };

    for (auto& [key, module] : this->m_scriptModules) {
	if (module.object != &object || !module.initialized || module.cursorHandlers <= 0) {
	    continue;
	}

	this->m_runningModule = &module;
	this->bindThisLayer (*module.object, &module);

	JSValue event = JS_NewObject (this->m_context);
	JS_SetPropertyStr (this->m_context, event, "worldPosition", makePosition (worldPosition));
	JS_SetPropertyStr (this->m_context, event, "localPosition", makePosition (localPosition));

	JSValue args[] = { event };
	JSValue result = this->call (module.module, 1, args, handler);

	if (JS_IsException (result)) {
	    logJSException (this->m_context, key.c_str (), module.value.getScriptSource ());
	}

	JS_FreeValue (this->m_context, result);
	JS_FreeValue (this->m_context, event);
    }

    this->m_runningModule = nullptr;
}

void ScriptEngine::addVideoEndedCallback (VideoPlayback::MPV::GLPlayer* player, JSValueConst callback) {
    if (player == nullptr || !JS_IsFunction (this->m_context, callback)) {
	return;
    }

    this->m_videoEndedCallbacks.push_back ({ player, JS_DupValue (this->m_context, callback) });
}

void ScriptEngine::tick () {
    this->m_engineObject->tick ();

    // index based, a callback is free to register more callbacks (and reallocate the vector)
    for (size_t index = 0; index < this->m_videoEndedCallbacks.size (); index++) {
	auto& entry = this->m_videoEndedCallbacks[index];

	if (!entry.player->hasEnded ()) {
	    entry.notified = false;
	    continue;
	}

	if (entry.notified) {
	    continue;
	}

	entry.notified = true;

	JSValue callback = JS_DupValue (this->m_context, entry.callback);
	JSValue result = JS_Call (this->m_context, callback, JS_UNDEFINED, 0, nullptr);

	if (JS_IsException (result)) {
	    logJSException (this->m_context, "video ended callback");
	}

	JS_FreeValue (this->m_context, result);
	JS_FreeValue (this->m_context, callback);
    }

    // safe to erase retired modules now, nothing is mid-iteration and no queueScript() is on the stack
    for (const auto& key : this->m_retiredScriptKeys) {
	const auto it = this->m_scriptModules.find (key);
	if (it == this->m_scriptModules.end ()) {
	    continue;
	}
	JS_FreeValue (this->m_context, it->second.module);
	JS_FreeValue (this->m_context, it->second.thisObject);
	this->m_scriptModules.erase (it);
    }
    this->m_retiredScriptKeys.clear ();

    // long stalls (window dragged, suspend) shouldn't skip whole animations
    this->m_animations.tick (std::clamp (g_Time - g_TimeLast, 0.0f, 0.25f));

    // run any pending notifications

    for (auto& [key, module] : this->m_scriptModules) {
	this->m_runningModule = &module;

	// `thisLayer` is a single global binding shared by every module and only set once, at
	// registration in queueScript() - without rebinding it here, every module but the last
	// registered would see whatever object queueScript() last ran for, not its own layer.
	if (module.object != nullptr) {
	    this->bindThisLayer (*module.object, &module);
	}

	if (!module.initialized) {
	    this->initializeModule (key, module);
	}

	JSValue args[] = { this->dynamicToJs (module.value) };
	JSValue result = this->call (module.module, 1, args, "update");
	ScopeGuard guard ([result, args, this] () {
	    JS_FreeValue (this->m_context, result);
	    JS_FreeValue (this->m_context, args[0]);
	});

	if (JS_IsException (result)) {
	    logJSException (this->m_context, key.c_str (), module.value.getScriptSource ());
	    continue;
	}

	if (key.starts_with ("scale_")) {
	    static int scaleDiagnosticCounter = 0;
	    if (++scaleDiagnosticCounter >= 300) {
		scaleDiagnosticCounter = 0;
		sLog.debug (
		    "scale script '", key, "': update() returned tag=", JS_VALUE_GET_TAG (result),
		    ", current vec3 = (", module.value.getVec3 ().x, ", ", module.value.getVec3 ().y, ", ",
		    module.value.getVec3 ().z, ")"
		);
	    }

	    // Edge-triggered marker for when a pulse lands visually, timestamped so it can be
	    // correlated against when the sound was played.
	    static std::map<std::string, bool> wasPulsing;
	    const bool pulsingNow = std::abs (module.value.getVec3 ().x - 1.0f) > 0.03f;
	    if (pulsingNow && !wasPulsing[key]) {
		const auto now = std::chrono::system_clock::now ();
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds> (now.time_since_epoch ()) % 1000;
		const std::time_t t = std::chrono::system_clock::to_time_t (now);
		std::tm tmBuf {};
		localtime_r (&t, &tmBuf);
		char buf[16];
		std::strftime (buf, sizeof (buf), "%H:%M:%S", &tmBuf);
		sLog.debug (
		    "[", buf, ".", (ms.count () < 100 ? "0" : ""), (ms.count () < 10 ? "0" : ""), ms.count (),
		    "] scale script '", key, "': PULSE, vec3.x=", module.value.getVec3 ().x
		);
	    }
	    wasPulsing[key] = pulsingNow;
	}

	jsToDynamicValue (this->m_context, result, module.value);
    }

    this->dispatchAnimationEvents ();
}

Media::ThumbnailPalette ScriptEngine::thumbnailPaletteFor (const Media::MediaSource::MediaInfo& media) {
    if (!media.url.has_value ()) {
	return {};
    }

    // every module gets the event, decode the cover once per file
    if (this->m_paletteUrl == *media.url) {
	return this->m_palette;
    }

    this->m_paletteUrl = *media.url;
    this->m_palette = {};

    std::string path = *media.url;

    if (!path.starts_with ("file://")) {
	return this->m_palette;
    }

    this->m_palette = Media::loadThumbnailPalette (path.substr (7));

    return this->m_palette;
}

void ScriptEngine::notifyMediaUpdate (const Media::MediaSource::MediaInfo& media, LoadedModule* only) {
    JSContext* ctx = this->m_context;

    const auto palette = this->thumbnailPaletteFor (media);

    DynamicValue primaryColorValue (palette.primary);
    DynamicValue secondaryColorValue (palette.secondary);
    DynamicValue tertiaryColorValue (palette.tertiary);
    DynamicValue textColorValue (palette.text);
    DynamicValue highContrastColorValue (palette.highContrast);

    JSValue primaryColor = this->m_adapters.vec3->instantiate (primaryColorValue, true);
    JSValue secondaryColor = this->m_adapters.vec3->instantiate (secondaryColorValue, true);
    JSValue tertiaryColor = this->m_adapters.vec3->instantiate (tertiaryColorValue, true);
    JSValue textColor = this->m_adapters.vec3->instantiate (textColorValue, true);
    JSValue highContrastColor = this->m_adapters.vec3->instantiate (highContrastColorValue, true);

    JSValue propertiesEvent = JS_NewObject (ctx);

    JS_SetPropertyStr (ctx, propertiesEvent, "title", JS_NewString (ctx, media.title.c_str ()));
    JS_SetPropertyStr (ctx, propertiesEvent, "artist", JS_NewString (ctx, media.artist.c_str ()));
    JS_SetPropertyStr (ctx, propertiesEvent, "albumTitle", JS_NewString (ctx, media.album.c_str ()));

    JSValue playbackEvent = JS_NewObject (ctx);

    JS_SetPropertyStr (ctx, playbackEvent, "state", JS_NewInt32 (ctx, media.playbackState));

    JSValue mediaTimelineEvent = JS_NewObject (ctx);

    JS_SetPropertyStr (ctx, mediaTimelineEvent, "position", JS_NewFloat64 (ctx, media.position));
    JS_SetPropertyStr (ctx, mediaTimelineEvent, "duration", JS_NewFloat64 (ctx, media.duration));

    JSValue mediaThumbnailEvent = JS_NewObject (ctx);

    JS_SetPropertyStr (ctx, mediaThumbnailEvent, "hasThumbnail", JS_NewBool (ctx, media.url.has_value ()));
    JS_SetPropertyStr (ctx, mediaThumbnailEvent, "primaryColor", primaryColor);
    JS_SetPropertyStr (ctx, mediaThumbnailEvent, "secondaryColor", secondaryColor);
    JS_SetPropertyStr (ctx, mediaThumbnailEvent, "tertiaryColor", tertiaryColor);
    JS_SetPropertyStr (ctx, mediaThumbnailEvent, "textColor", textColor);
    JS_SetPropertyStr (ctx, mediaThumbnailEvent, "highContrastColor", highContrastColor);

    JSValue propertiesArgs[] = { propertiesEvent };
    JSValue playbackArgs[] = { playbackEvent };
    JSValue mediaTimelineArgs[] = { mediaTimelineEvent };
    JSValue mediaThumbnailArgs[] = { mediaThumbnailEvent };

    for (auto& module : this->m_scriptModules | std::views::values) {
	// modules that haven't run init() yet get the current media state replayed once they do
	if (!module.initialized || (only != nullptr && only != &module)) {
	    continue;
	}

	JSValue result1 = this->call (module.module, 1, propertiesArgs, "mediaPropertiesChanged");
	JSValue result2 = this->call (module.module, 1, playbackArgs, "mediaPlaybackChanged");
	JSValue result3 = this->call (module.module, 1, mediaTimelineArgs, "mediaTimelineChanged");
	JSValue result4 = this->call (module.module, 1, mediaThumbnailArgs, "mediaThumbnailChanged");

	JS_FreeValue (ctx, result1);
	JS_FreeValue (ctx, result2);
	JS_FreeValue (ctx, result3);
	JS_FreeValue (ctx, result4);
    }

    // free all created objects as we don't keep a ref to them anymore
    JS_FreeValue (ctx, propertiesEvent);
    JS_FreeValue (ctx, playbackEvent);
    JS_FreeValue (ctx, mediaTimelineEvent);
    JS_FreeValue (ctx, mediaThumbnailEvent);
}