#include "EngineObject.h"
#include "ScriptEngine.h"
#include "WallpaperEngine/Logging/Log.h"

#include "WallpaperEngine/Audio/Drivers/AudioDriver.h"
#include "WallpaperEngine/Audio/Drivers/Recorders/PlaybackRecorder.h"
#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include "WallpaperEngine/Scripting/Adapters/ScriptableObjectAdapter.h"

#include <ranges>

using namespace WallpaperEngine::Scripting;

extern float g_Time;
extern float g_TimeLast;
extern float g_Daytime;

static uint32_t EngineInstanceId = 0;
std::map<uint32_t, EngineObject&> engineInstances;

// read-only properties, writes are ignored instead of aborting the calling script
JSValue engine_set_value (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) { return JS_UNDEFINED; }

// rebuilt on every read so scripts always see the current values
JSValue engine_get_user_properties (
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic
) {
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

JSValue engine_open_user_shortcut (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
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

JSValue engine_get_screen_resolution (
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic
) {
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
JSValue engine_clear_timeout (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return JS_FALSE;
}

// layers are never destroyed from scripts here, so any layer handle is still valid
JSValue engine_is_object_valid (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1) {
	return JS_FALSE;
    }

    return JS_NewBool (ctx, WallpaperEngine::Scripting::Adapters::ScriptableObjectAdapter::getObject (argv[0]) != nullptr);
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

// Backs the "average"/"left"/"right" getters on the object returned by registerAudioBuffers().
// The recorder only ever produces one (mono) spectrum - see PulseAudioPlaybackRecorder - so all
// three read the same data, matching how CPass already binds it to both the Left and Right
// g_AudioSpectrum shader uniforms.
JSValue audio_buffer_get_values (
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValueConst* func_data
) {
    int engineInstanceId = 0;
    int resolution = 32;

    JS_ToInt32 (ctx, &engineInstanceId, func_data[0]);
    JS_ToInt32 (ctx, &resolution, func_data[1]);

    JSValue result = JS_NewArray (ctx);

    const auto it = engineInstances.find (engineInstanceId);

    if (it == engineInstances.end ()) {
	sLog.error ("registerAudioBuffers: no EngineObject found for instance ", engineInstanceId, " - returning zeros");
	return result;
    }

    const auto& recorder = it->second.getScene ().getAudioContext ().getDriver ().getRecorder ();
    const float* data = recorder.audio32;

    if (resolution == 16) {
	data = recorder.audio16;
    } else if (resolution == 64) {
	data = recorder.audio64;
    }

    // audio16/32/64 are written from the recorder's own capture thread (see
    // PulseAudioPlaybackRecorder), so reading them here (the script thread) needs the same lock.
    recorder.lock ();

    static int diagnosticCounter = 0;
    if (++diagnosticCounter >= 500) {
	diagnosticCounter = 0;
	sLog.debug ("registerAudioBuffers: average[0..3] = ", data[0], ", ", data[1], ", ", data[2], ", ", data[3]);
    }

    for (int i = 0; i < resolution; i++) {
	JS_SetPropertyUint32 (ctx, result, i, JS_NewFloat64 (ctx, data[i]));
    }

    recorder.unlock ();

    return result;
}

// engine.registerAudioBuffers(resolution): resolution must be 16, 32 or 64 (falls back to 32
// otherwise), matching the AUDIO_RESOLUTION_* constants below. Returns an object whose
// average/left/right properties are re-read from the live FFT spectrum every access, so scripts
// that poll them from an update() callback see current values each frame.
JSValue engine_register_audio_buffers (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    int resolution = 32;

    if (argc > 0) {
	JS_ToInt32 (ctx, &resolution, argv[0]);
    }

    if (resolution != 16 && resolution != 32 && resolution != 64) {
	resolution = 32;
    }

    JSValue result = JS_NewObject (ctx);
    static constexpr const char* properties[] = { "average", "left", "right" };

    for (const char* property : properties) {
	JSValue closureData[] = { JS_NewInt32 (ctx, magic), JS_NewInt32 (ctx, resolution) };

	JS_DefinePropertyGetSet (
	    ctx, result, JS_NewAtom (ctx, property),
	    JS_NewCFunctionData (ctx, audio_buffer_get_values, 0, 0, 2, closureData),
	    JS_NewCFunction (ctx, engine_set_value, "set", 1), JS_PROP_ENUMERABLE
	);
    }

    return result;
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
	JS_NewCFunction (this->m_engine.getContext (), engine_open_user_shortcut, "openUserShortcut", 0),
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
	{ "isWallpaper", 1 },	    { "isScreensaver", 0 },
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
	JS_NewCFunction (this->m_engine.getContext (), engine_is_object_valid, "isObjectValid", 1),
	JS_PROP_ENUMERABLE
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

    JS_FreeValue (this->m_engine.getContext (), this->m_instance);
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
