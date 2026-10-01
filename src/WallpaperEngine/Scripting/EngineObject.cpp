#include "EngineObject.h"
#include "ScriptEngine.h"
#include "WallpaperEngine/Logging/Log.h"

#include "WallpaperEngine/Audio/Drivers/AudioDriver.h"
#include "WallpaperEngine/Audio/Drivers/Recorders/PlaybackRecorder.h"
#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Desktop/UserShortcut.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include "WallpaperEngine/Scripting/Adapters/ScriptableObjectAdapter.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <ranges>
#include <vector>

using namespace WallpaperEngine::Scripting;

extern float g_Time;
extern float g_TimeLast;
extern float g_Daytime;

static uint32_t EngineInstanceId = 0;
std::map<uint32_t, EngineObject&> engineInstances;

// read-only properties, writes are ignored instead of aborting the calling script
JSValue engine_set_value (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) { return JS_UNDEFINED; }

// rebuilt on every read so scripts always see the current values
JSValue engine_get_user_properties (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    JSValue result = JS_NewObject (ctx);
    const auto it = engineInstances.find (magic);

    if (it == engineInstances.end ()) {
	return result;
    }

    auto& engine = it->second.getEngine ();

    for (const auto& [name, property] : it->second.getScene ().getUserProperties ()) {
	JS_SetPropertyStr (ctx, result, name.c_str (), engine.userPropertyToJs (*property));
    }

    return result;
}

// the name is the usershortcut user property's; only a shortcut the user assigned is ever opened
JSValue engine_open_user_shortcut (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    const auto it = engineInstances.find (magic);

    if (argc < 1 || !JS_IsString (argv[0]) || it == engineInstances.end ()) {
	return JS_UNDEFINED;
    }

    const char* name = JS_ToCString (ctx, argv[0]);
    const std::string propertyName = name != nullptr ? name : "";
    JS_FreeCString (ctx, name);

    const auto& properties = it->second.getScene ().getUserProperties ();
    const auto property = properties.find (propertyName);

    if (property == properties.end () || !property->second->is<WallpaperEngine::Data::Model::PropertyUserShortcut> ()) {
	sLog.error ("openUserShortcut: no user shortcut property named ", propertyName);
	return JS_UNDEFINED;
    }

    const auto shortcut = WallpaperEngine::Desktop::UserShortcut::parse (property->second->getString ());

    if (!shortcut.has_value ()) {
	return JS_UNDEFINED;
    }

    // a script calling this from update() instead of a click must not start the app every frame
    static std::map<std::string, std::chrono::steady_clock::time_point> lastLaunch;
    const auto now = std::chrono::steady_clock::now ();

    if (const auto last = lastLaunch.find (propertyName);
	last != lastLaunch.end () && now - last->second < std::chrono::seconds (1)) {
	return JS_UNDEFINED;
    }

    lastLaunch[propertyName] = now;
    sLog.out ("Opening user shortcut ", propertyName, ": ", shortcut->target);
    shortcut->launch ();

    return JS_UNDEFINED;
}

// scripts only ever hand the result back to layer properties (layer.font = ...), so the path itself is the handle
JSValue engine_register_asset (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsString (argv[0])) {
	return JS_UNDEFINED;
    }

    return JS_DupValue (ctx, argv[0]);
}

// engine.isRunningInEditor() and friends: fixed answers, this is always a plain desktop wallpaper
JSValue engine_query_flag (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    return JS_NewBool (ctx, magic != 0);
}

// the scene's own coordinate space (project width/height), not the monitor resolution
JSValue engine_get_canvas_size (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    const auto it = engineInstances.find (magic);

    if (it == engineInstances.end ()) {
	return JS_UNDEFINED;
    }

    const auto& camera = it->second.getScene ().getCamera ();
    const DynamicValue size (glm::vec2 (camera.getWidth (), camera.getHeight ()));

    return it->second.getEngine ().getAdapters ().vec2->instantiate (const_cast<DynamicValue&> (size), true);
}

glm::vec2 engine_screen_size (EngineObject& engine) {
    const auto& screen = engine.getScene ().getScreenSize ();

    if (screen.x > 0 && screen.y > 0) {
	return glm::vec2 (screen);
    }

    const auto& camera = engine.getScene ().getCamera ();
    return { camera.getWidth (), camera.getHeight () };
}

JSValue engine_get_screen_resolution (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    const auto it = engineInstances.find (magic);

    if (it == engineInstances.end ()) {
	return JS_UNDEFINED;
    }

    const DynamicValue size (engine_screen_size (it->second));

    return it->second.getEngine ().getAdapters ().vec2->instantiate (const_cast<DynamicValue&> (size), true);
}

// magic packs the instance id with the question: bit 0 set asks for landscape instead of portrait
JSValue engine_query_orientation (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    const auto it = engineInstances.find (magic >> 1);

    if (it == engineInstances.end ()) {
	return JS_FALSE;
    }

    const auto size = engine_screen_size (it->second);

    return JS_NewBool (ctx, (magic & 1) ? size.x >= size.y : size.y > size.x);
}

// WE's version of this is the callback behind the stop functions setTimeout/setInterval return;
// called straight off engine it has no timer bound to it and never stops anything
JSValue engine_clear_timeout (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) { return JS_FALSE; }

// layers are never destroyed from scripts here, so any layer handle is still valid
JSValue engine_is_object_valid (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1) {
	return JS_FALSE;
    }

    return JS_NewBool (
	ctx, WallpaperEngine::Scripting::Adapters::ScriptableObjectAdapter::getObject (argv[0]) != nullptr
    );
}

JSValue engine_get_frametime (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return JS_NewFloat64 (ctx, g_Time - g_TimeLast);
}

JSValue engine_get_runtime (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return JS_NewFloat64 (ctx, g_Time);
}

JSValue engine_get_daytime (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return JS_NewFloat64 (ctx, g_Daytime);
}

JSValue engine_stop_interval (
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValueConst* func_data
) {
    if (argc != 1) {
	return JS_ThrowTypeError (ctx, "engine_stop_interval: wrong number of arguments");
    }

    const auto it = engineInstances.find (magic);

    if (it == engineInstances.end ()) {
	return JS_ThrowTypeError (ctx, "engine_stop_interval: engine instance is gone");
    }

    int id = 0;

    JS_ToInt32 (ctx, &id, argv[0]);

    it->second.clearInterval (id);

    return JS_UNDEFINED;
}

JSValue engine_stop_timeout (
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValueConst* func_data
) {
    if (argc != 1) {
	return JS_ThrowTypeError (ctx, "engine_stop_timeout: wrong number of arguments");
    }

    const auto it = engineInstances.find (magic);

    if (it == engineInstances.end ()) {
	return JS_ThrowTypeError (ctx, "engine_stop_timeout: engine instance is gone");
    }

    int id = 0;

    JS_ToInt32 (ctx, &id, argv[0]);

    it->second.clearTimeout (id);

    return JS_UNDEFINED;
}

JSValue engine_set_interval (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    if (argc < 1) {
	return JS_ThrowTypeError (ctx, "engine_set_interval: wrong number of arguments");
    }

    int delay = 0;

    if (argc > 1) {
	JS_ToInt32 (ctx, &delay, argv[1]);
    }

    JSValue function = argv[0];

    if (!JS_IsFunction (ctx, function)) {
	return JS_ThrowTypeError (ctx, "engine_set_interval: expected a function");
    }

    const auto it = engineInstances.find (magic);

    if (it == engineInstances.end ()) {
	return JS_ThrowTypeError (ctx, "engine_set_interval: engine instance is gone");
    }

    int id = it->second.reserveNextIntervalId (function, delay);

    JSValue args[] = { JS_NewInt32 (ctx, id) };

    return JS_NewCFunctionData (ctx, engine_stop_interval, 2, magic, 1, args);
}

// engine.registerAudioBuffers(resolution), scenescript64 2.8.42 sub_181655170: only from a module's top level
// code, no number argument means 16, anything but 16/32/64 throws. Both errors are SyntaxErrors in live WE.
JSValue engine_register_audio_buffers (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    const auto it = engineInstances.find (magic);

    if (it == engineInstances.end ()) {
	return JS_ThrowTypeError (ctx, "registerAudioBuffers: engine instance is gone");
    }

    if (!it->second.getEngine ().isEvaluatingModuleBody ()) {
	return JS_ThrowSyntaxError (ctx, "registerAudioBuffers can only be called from global scope.");
    }

    int resolution = 16;

    if (argc > 0 && JS_IsNumber (argv[0])) {
	JS_ToInt32 (ctx, &resolution, argv[0]);
    }

    if (resolution != 16 && resolution != 32 && resolution != 64) {
	return JS_ThrowSyntaxError (ctx, "Resolution must be either 16, 32 or 64.");
    }

    return it->second.registerAudioBuffers (resolution);
}

JSValue engine_set_timeout (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    if (argc < 1) {
	return JS_ThrowTypeError (ctx, "engine_set_timeout: wrong number of arguments");
    }

    int delay = 0;

    if (argc > 1) {
	JS_ToInt32 (ctx, &delay, argv[1]);
    }

    JSValue function = argv[0];

    if (!JS_IsFunction (ctx, function)) {
	return JS_ThrowTypeError (ctx, "engine_set_timeout: expected a function");
    }

    const auto it = engineInstances.find (magic);

    if (it == engineInstances.end ()) {
	return JS_ThrowTypeError (ctx, "engine_set_timeout: engine instance is gone");
    }

    int id = it->second.reserveNextTimeoutId (function, delay);

    JSValue args[] = { JS_NewInt32 (ctx, id) };

    return JS_NewCFunctionData (ctx, engine_stop_timeout, 2, magic, 1, args);
}

EngineObject::EngineObject (ScriptEngine& engine, Render::Wallpapers::CScene& scene) :
    m_scene (scene), m_engine (engine), m_instanceId (++EngineInstanceId), m_classId (0) {
    // required so setInterval/setTimeout/registerAudioBuffers's magic-encoded instance id can find
    // their way back to this object from the free-standing JS callback functions above
    engineInstances.emplace (this->m_instanceId, *this);

    this->m_definition = { .class_name = "IEngine" };
    JS_NewClassID (this->m_engine.getRuntime (), &this->m_classId);
    JS_NewClass (this->m_engine.getRuntime (), this->m_classId, &this->m_definition);
    this->m_instance = JS_NewObjectClass (this->m_engine.getContext (), this->m_classId);

    JS_DupValue (this->m_engine.getContext (), this->m_instance);

    JS_SetOpaque (this->m_instance, this);
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "frametime"),
	JS_NewCFunction (this->m_engine.getContext (), engine_get_frametime, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), engine_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "runtime"),
	JS_NewCFunction (this->m_engine.getContext (), engine_get_runtime, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), engine_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "timeOfDay"),
	JS_NewCFunction (this->m_engine.getContext (), engine_get_daytime, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), engine_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "canvasSize"),
	JS_NewCFunctionMagic (
	    this->m_engine.getContext (), engine_get_canvas_size, "get", 0, JS_CFUNC_generic_magic, this->m_instanceId
	),
	JS_NewCFunction (this->m_engine.getContext (), engine_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "userProperties"),
	JS_NewCFunctionMagic (
	    this->m_engine.getContext (), engine_get_user_properties, "get", 0, JS_CFUNC_generic_magic,
	    this->m_instanceId
	),
	JS_NewCFunction (this->m_engine.getContext (), engine_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "AUDIO_RESOLUTION_16",
	JS_NewInt32 (this->m_engine.getContext (), 16), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "AUDIO_RESOLUTION_32",
	JS_NewInt32 (this->m_engine.getContext (), 32), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "AUDIO_RESOLUTION_64",
	JS_NewInt32 (this->m_engine.getContext (), 64), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "setInterval",
	JS_NewCFunctionMagic (
	    this->m_engine.getContext (), engine_set_interval, "setInterval", 2, JS_CFUNC_generic_magic,
	    this->m_instanceId
	),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "setTimeout",
	JS_NewCFunctionMagic (
	    this->m_engine.getContext (), engine_set_timeout, "setTimeout", 2, JS_CFUNC_generic_magic,
	    this->m_instanceId
	),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "registerAsset",
	JS_NewCFunction (this->m_engine.getContext (), engine_register_asset, "registerAsset", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "openUserShortcut",
	JS_NewCFunctionMagic (
	    this->m_engine.getContext (), engine_open_user_shortcut, "openUserShortcut", 1, JS_CFUNC_generic_magic,
	    this->m_instanceId
	),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "registerAudioBuffers",
	JS_NewCFunctionMagic (
	    this->m_engine.getContext (), engine_register_audio_buffers, "registerAudioBuffers", 1,
	    JS_CFUNC_generic_magic, this->m_instanceId
	),
	JS_PROP_ENUMERABLE
    );
    const struct {
	const char* name;
	int answer;
    } flags[] = {
	{ "isRunningInEditor", 0 }, { "isDesktopDevice", 1 }, { "isMobileDevice", 0 },
	{ "isWallpaper", 1 },       { "isScreensaver", 0 },
    };
    for (const auto& flag : flags) {
	JS_DefinePropertyValueStr (
	    this->m_engine.getContext (), this->m_instance, flag.name,
	    JS_NewCFunctionMagic (
		this->m_engine.getContext (), engine_query_flag, flag.name, 0, JS_CFUNC_generic_magic, flag.answer
	    ),
	    JS_PROP_ENUMERABLE
	);
    }
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "screenResolution"),
	JS_NewCFunctionMagic (
	    this->m_engine.getContext (), engine_get_screen_resolution, "get", 0, JS_CFUNC_generic_magic,
	    this->m_instanceId
	),
	JS_NewCFunction (this->m_engine.getContext (), engine_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "isPortrait",
	JS_NewCFunctionMagic (
	    this->m_engine.getContext (), engine_query_orientation, "isPortrait", 0, JS_CFUNC_generic_magic,
	    static_cast<int> (this->m_instanceId << 1)
	),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "isLandscape",
	JS_NewCFunctionMagic (
	    this->m_engine.getContext (), engine_query_orientation, "isLandscape", 0, JS_CFUNC_generic_magic,
	    static_cast<int> ((this->m_instanceId << 1) | 1)
	),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "clearTimeout",
	JS_NewCFunction (this->m_engine.getContext (), engine_clear_timeout, "clearTimeout", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "isObjectValid",
	JS_NewCFunction (this->m_engine.getContext (), engine_is_object_valid, "isObjectValid", 1), JS_PROP_ENUMERABLE
    );
}

EngineObject::~EngineObject () {
    for (const auto& [id, timeout] : this->m_timeouts) {
	JS_FreeValue (this->m_engine.getContext (), timeout.callback);
    }
    for (const auto& [id, interval] : this->m_intervals) {
	JS_FreeValue (this->m_engine.getContext (), interval.callback);
    }

    engineInstances.erase (this->m_instanceId);
    this->m_intervals.clear ();
    this->m_timeouts.clear ();

    for (const auto& buffer : this->m_audioBuffers) {
	JS_FreeValue (this->m_engine.getContext (), buffer);
    }

    JS_FreeValue (this->m_engine.getContext (), this->m_instance);
}

float* EngineObject::audioBufferData (int index) {
    auto* ctx = this->m_engine.getContext ();
    const size_t size = (16u << (index / 3)) * sizeof (float);
    size_t length = 0;

    if (!JS_IsUndefined (this->m_audioBuffers[index])) {
	if (auto* data = JS_GetArrayBuffer (ctx, &length, this->m_audioBuffers[index]); data != nullptr) {
	    return reinterpret_cast<float*> (data);
	}

	// a script detached it with transfer(), WE's memory would still be there so start a new one
	JS_FreeValue (ctx, JS_GetException (ctx));
	JS_FreeValue (ctx, this->m_audioBuffers[index]);
    }

    const std::vector<uint8_t> zeros (size);
    this->m_audioBuffers[index] = JS_NewArrayBufferCopy (ctx, zeros.data (), zeros.size ());

    return reinterpret_cast<float*> (JS_GetArrayBuffer (ctx, &length, this->m_audioBuffers[index]));
}

// WE keeps nine buffers per script context and wraps them in new Float32Arrays on every call, so all
// registrations of a resolution share their memory. Ours also share the ArrayBuffer object: one over
// memory we own would be reallocated by ArrayBuffer.prototype.transfer()
JSValue EngineObject::registerAudioBuffers (int resolution) {
    auto* ctx = this->m_engine.getContext ();
    const int first = 3 * (resolution >> 5);

    for (int index = 0; index < 9; index++) {
	this->audioBufferData (index);
    }

    JSValue result = JS_NewObject (ctx);
    static constexpr const char* sections[] = { "left", "right", "average" };

    for (int section = 0; section < 3; section++) {
	JSValue args[] = { this->m_audioBuffers[first + section], JS_NewInt32 (ctx, 0), JS_NewInt32 (ctx, resolution) };
	JSValue array = JS_NewTypedArray (ctx, 3, args, JS_TYPED_ARRAY_FLOAT32);

	if (JS_IsException (array)) {
	    JS_FreeValue (ctx, result);
	    return array;
	}

	JS_SetPropertyStr (ctx, result, sections[section], array);
    }

    return result;
}

uint32_t EngineObject::reserveNextTimeoutId (JSValue function, uint64_t duration) {
    const auto id = ++this->m_nextTimeoutId;

    this->m_timeouts[id] = Timeout { .callback = JS_DupValue (this->m_engine.getContext (), function),
				     .duration = std::chrono::milliseconds (duration),
				     .next = std::chrono::steady_clock::now () + std::chrono::milliseconds (duration) };

    return id;
}

uint32_t EngineObject::reserveNextIntervalId (JSValue function, uint64_t duration) {
    const auto id = ++this->m_nextIntervalId;

    this->m_intervals[id]
	= Timeout { .callback = JS_DupValue (this->m_engine.getContext (), function),
		    .duration = std::chrono::milliseconds (duration),
		    .next = std::chrono::steady_clock::now () + std::chrono::milliseconds (duration) };

    return id;
}

void EngineObject::clearInterval (uint32_t id) {
    const auto it = this->m_intervals.find (id);

    if (it == this->m_intervals.end ()) {
	return;
    }

    JS_FreeValue (this->getEngine ().getContext (), it->second.callback);

    this->m_intervals.erase (id);
}

void EngineObject::clearTimeout (uint32_t id) {
    const auto it = this->m_timeouts.find (id);

    if (it == this->m_timeouts.end ()) {
	return;
    }

    JS_FreeValue (this->getEngine ().getContext (), it->second.callback);

    this->m_timeouts.erase (id);
}

static void callTimerCallback (JSContext* ctx, JSValueConst callback, const char* kind) {
    JSValue result = JS_Call (ctx, callback, JS_NULL, 0, nullptr);

    if (JS_IsException (result)) {
	logJSException (ctx, kind);
    }

    JS_FreeValue (ctx, result);
}

void EngineObject::tick () {
    const auto now = std::chrono::steady_clock::now ();
    auto* ctx = this->m_engine.getContext ();

    // scenescript64 sub_18164F800 copies [left | right | average] into the registered buffers before any
    // callback runs, so what scripts write there lasts until the next frame
    if (!JS_IsUndefined (this->m_audioBuffers[0])) {
	const auto& recorder = this->m_scene.getAudioContext ().getDriver ().getRecorder ();
	const float* sources[] = { recorder.audio16, recorder.audio32, recorder.audio64 };

	for (int index = 0; index < 9; index++) {
	    const int bands = 16 << (index / 3);

	    std::copy_n (sources[index / 3] + (index % 3) * bands, bands, this->audioBufferData (index));
	}
    }

    // only ids are collected up front, callbacks may clearInterval()/setTimeout() from inside themselves
    std::vector<uint32_t> dueIntervals;

    for (const auto& [id, interval] : this->m_intervals) {
	if (interval.next <= now) {
	    dueIntervals.push_back (id);
	}
    }

    for (const auto id : dueIntervals) {
	const auto it = this->m_intervals.find (id);

	if (it == this->m_intervals.end ()) {
	    continue;
	}

	it->second.next = now + it->second.duration;

	JSValue callback = JS_DupValue (ctx, it->second.callback);
	callTimerCallback (ctx, callback, "setInterval");
	JS_FreeValue (ctx, callback);
    }

    std::vector<uint32_t> dueTimeouts;

    for (const auto& [id, timeout] : this->m_timeouts) {
	if (timeout.next <= now) {
	    dueTimeouts.push_back (id);
	}
    }

    for (const auto id : dueTimeouts) {
	const auto it = this->m_timeouts.find (id);

	if (it == this->m_timeouts.end ()) {
	    continue;
	}

	JSValue callback = it->second.callback;
	this->m_timeouts.erase (it);

	callTimerCallback (ctx, callback, "setTimeout");
	JS_FreeValue (ctx, callback);
    }
}
