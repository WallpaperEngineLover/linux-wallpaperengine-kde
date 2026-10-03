#include "EngineObject.h"
#include "JS.h"
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
#include <numeric>
#include <vector>

using namespace WallpaperEngine::Scripting;

extern float g_Time;
extern float g_TimeLast;
extern float g_Daytime;

namespace {
EngineObject& engineOf (const v8::FunctionCallbackInfo<v8::Value>& info) {
    return ScriptEngine::from (info.GetIsolate ()).getEngineObject ();
}

// read-only properties, writes are ignored instead of aborting the calling script
void engine_set_value (const v8::FunctionCallbackInfo<v8::Value>&) { }

// rebuilt on every read so scripts always see the current values
void engine_get_user_properties (const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto& engine = ScriptEngine::from (info.GetIsolate ());
    const auto context = engine.getContext ();
    const v8::Local<v8::Object> result = v8::Object::New (info.GetIsolate ());

    for (const auto& [name, property] : engine.getScene ().getUserProperties ()) {
	JS::set (context, result, name, engine.userPropertyToJs (*property));
    }

    info.GetReturnValue ().Set (result);
}

// the name is the usershortcut user property's; only a shortcut the user assigned is ever opened
void engine_open_user_shortcut (const v8::FunctionCallbackInfo<v8::Value>& info) {
    if (info.Length () < 1 || !info[0]->IsString ()) {
	return;
    }

    const std::string propertyName = JS::toString (info.GetIsolate (), info[0]);
    const auto& properties = ScriptEngine::from (info.GetIsolate ()).getScene ().getUserProperties ();
    const auto property = properties.find (propertyName);

    if (property == properties.end () || !property->second->is<WallpaperEngine::Data::Model::PropertyUserShortcut> ()) {
	sLog.error ("openUserShortcut: no user shortcut property named ", propertyName);
	return;
    }

    const auto shortcut = WallpaperEngine::Desktop::UserShortcut::parse (property->second->getString ());

    if (!shortcut.has_value ()) {
	return;
    }

    // a script calling this from update() instead of a click must not start the app every frame
    static std::map<std::string, std::chrono::steady_clock::time_point> lastLaunch;
    const auto now = std::chrono::steady_clock::now ();

    if (const auto last = lastLaunch.find (propertyName);
	last != lastLaunch.end () && now - last->second < std::chrono::seconds (1)) {
	return;
    }

    lastLaunch[propertyName] = now;
    sLog.out ("Opening user shortcut ", propertyName, ": ", shortcut->target);
    shortcut->launch ();
}

// scripts only ever hand the result back to layer properties (layer.font = ...), so the path itself is the handle
void engine_register_asset (const v8::FunctionCallbackInfo<v8::Value>& info) {
    if (info.Length () < 1 || !info[0]->IsString ()) {
	return;
    }

    info.GetReturnValue ().Set (info[0]);
}

// engine.isRunningInEditor() and friends: fixed answers, this is always a plain desktop wallpaper
void engine_query_flag (const v8::FunctionCallbackInfo<v8::Value>& info, int answer) {
    info.GetReturnValue ().Set (answer != 0);
}

// the scene's own coordinate space (project width/height), not the monitor resolution
void engine_get_canvas_size (const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto& engine = ScriptEngine::from (info.GetIsolate ());
    const auto& camera = engine.getScene ().getCamera ();

    info.GetReturnValue ().Set (
	engine.getAdapters ().vec2->create (glm::vec2 (camera.getWidth (), camera.getHeight ()))
    );
}

glm::vec2 engine_screen_size (const ScriptEngine& engine) {
    const auto& screen = engine.getScene ().getScreenSize ();

    if (screen.x > 0 && screen.y > 0) {
	return glm::vec2 (screen);
    }

    const auto& camera = engine.getScene ().getCamera ();
    return { camera.getWidth (), camera.getHeight () };
}

void engine_get_screen_resolution (const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto& engine = ScriptEngine::from (info.GetIsolate ());

    info.GetReturnValue ().Set (engine.getAdapters ().vec2->create (engine_screen_size (engine)));
}

// magic 1 asks for landscape instead of portrait
void engine_query_orientation (const v8::FunctionCallbackInfo<v8::Value>& info, int landscape) {
    const auto size = engine_screen_size (ScriptEngine::from (info.GetIsolate ()));

    info.GetReturnValue ().Set (landscape != 0 ? size.x >= size.y : size.y > size.x);
}

// layers are never destroyed from scripts here, so any layer handle is still valid
void engine_is_object_valid (const v8::FunctionCallbackInfo<v8::Value>& info) {
    info.GetReturnValue ().Set (
	info.Length () > 0
	&& ScriptEngine::from (info.GetIsolate ()).getAdapters ().object->getObject (info[0]) != nullptr
    );
}

void engine_get_frametime (const v8::FunctionCallbackInfo<v8::Value>& info) {
    info.GetReturnValue ().Set (static_cast<double> (g_Time - g_TimeLast));
}

void engine_get_runtime (const v8::FunctionCallbackInfo<v8::Value>& info) {
    info.GetReturnValue ().Set (static_cast<double> (g_Time));
}

void engine_get_daytime (const v8::FunctionCallbackInfo<v8::Value>& info) {
    info.GetReturnValue ().Set (static_cast<double> (g_Daytime));
}

// the stop function setTimeout/setInterval return, also engine.clearTimeout without a timer bound to it
// (scenescript64 sub_181656780)
void engine_clear_timeout (const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto& engine = ScriptEngine::from (info.GetIsolate ());

    if (engine.isEvaluatingModuleBody ()) {
	JS::throwSyntaxError (info.GetIsolate (), "timeout cannot be cleared from global scope.");
	return;
    }

    info.GetReturnValue ().Set (
	info.Data ()->IsArray () && engine.getEngineObject ().stopTimer (info.Data ().As<v8::Array> ())
    );
}

// engine.setTimeout (callback, delay) / setInterval (callback, delay), sub_181655E10 / sub_1816562D0: delays are
// milliseconds, a timeout's is optional, an interval needs one above 0
void engine_add_timer (const v8::FunctionCallbackInfo<v8::Value>& info, int interval) {
    auto& engine = ScriptEngine::from (info.GetIsolate ());

    if (engine.isEvaluatingModuleBody ()) {
	JS::throwSyntaxError (
	    info.GetIsolate (),
	    interval ? "setInterval cannot be called from global scope."
		     : "setTimeout cannot be called from global scope."
	);
	return;
    }

    if (info.Length () < (interval ? 2 : 1) || !info[0]->IsFunction ()) {
	return;
    }

    float seconds = 0.0f;

    if (info.Length () > 1 && info[1]->IsNumber ()) {
	seconds = static_cast<float> (info[1].As<v8::Number> ()->Value () / 1000.0);
    } else if (interval) {
	return;
    }

    if (interval && seconds <= 0.0f) {
	return;
    }

    info.GetReturnValue ().Set (engineOf (info).addTimer (info[0].As<v8::Function> (), seconds, interval != 0));
}

// engine.registerAudioBuffers(resolution), scenescript64 2.8.42 sub_181655170: only from a module's top level
// code, no number argument means 16, anything but 16/32/64 throws. Both errors are SyntaxErrors in live WE.
void engine_register_audio_buffers (const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto& engine = ScriptEngine::from (info.GetIsolate ());

    if (!engine.isEvaluatingModuleBody ()) {
	JS::throwSyntaxError (info.GetIsolate (), "registerAudioBuffers can only be called from global scope.");
	return;
    }

    int resolution = 16;

    if (info.Length () > 0 && info[0]->IsNumber ()) {
	resolution = info[0]->Int32Value (engine.getContext ()).FromMaybe (0);
    }

    if (resolution != 16 && resolution != 32 && resolution != 64) {
	JS::throwSyntaxError (info.GetIsolate (), "Resolution must be either 16, 32 or 64.");
	return;
    }

    info.GetReturnValue ().Set (engine.getEngineObject ().registerAudioBuffers (resolution));
}
} // namespace

EngineObject::EngineObject (ScriptEngine& engine, Render::Wallpapers::CScene& scene) :
    m_scene (scene), m_engine (engine) {
    auto* isolate = engine.getIsolate ();
    const auto context = engine.getContext ();
    const v8::Local<v8::Object> instance = v8::Object::New (isolate);
    const v8::Local<v8::Function> ignore = JS::function (context, engine_set_value, {}, 1);
    const auto accessor = [&] (const char* name, v8::FunctionCallback getter) {
	instance->SetAccessorProperty (JS::name (isolate, name), JS::function (context, getter), ignore);
    };
    const auto method = [&] (const char* name, v8::FunctionCallback callback, int length) {
	JS::define (context, instance, name, JS::function (context, callback, {}, length));
    };

    accessor ("frametime", engine_get_frametime);
    accessor ("runtime", engine_get_runtime);
    accessor ("timeOfDay", engine_get_daytime);
    accessor ("canvasSize", engine_get_canvas_size);
    accessor ("userProperties", engine_get_user_properties);
    JS::define (context, instance, "AUDIO_RESOLUTION_16", v8::Integer::New (isolate, 16));
    JS::define (context, instance, "AUDIO_RESOLUTION_32", v8::Integer::New (isolate, 32));
    JS::define (context, instance, "AUDIO_RESOLUTION_64", v8::Integer::New (isolate, 64));
    method ("setInterval", JS::bind<engine_add_timer, 1>, 2);
    method ("setTimeout", JS::bind<engine_add_timer, 0>, 2);
    method ("registerAsset", engine_register_asset, 1);
    method ("openUserShortcut", engine_open_user_shortcut, 1);
    method ("registerAudioBuffers", engine_register_audio_buffers, 1);
    method ("isRunningInEditor", JS::bind<engine_query_flag, 0>, 0);
    method ("isDesktopDevice", JS::bind<engine_query_flag, 1>, 0);
    method ("isMobileDevice", JS::bind<engine_query_flag, 0>, 0);
    method ("isWallpaper", JS::bind<engine_query_flag, 1>, 0);
    method ("isScreensaver", JS::bind<engine_query_flag, 0>, 0);
    accessor ("screenResolution", engine_get_screen_resolution);
    method ("isPortrait", JS::bind<engine_query_orientation, 0>, 0);
    method ("isLandscape", JS::bind<engine_query_orientation, 1>, 0);
    method ("clearTimeout", engine_clear_timeout, 1);
    method ("isObjectValid", engine_is_object_valid, 1);

    this->m_instance.Reset (isolate, instance);
}

v8::Local<v8::Object> EngineObject::getInstance () const { return this->m_instance.Get (this->m_engine.getIsolate ()); }

// WE keeps nine buffers per script context and wraps them in new Float32Arrays over a new ArrayBuffer on every call,
// so all registrations of a resolution share their memory
v8::Local<v8::Object> EngineObject::registerAudioBuffers (int resolution) {
    auto* isolate = this->m_engine.getIsolate ();
    const auto context = this->m_engine.getContext ();
    const int first = 3 * (resolution >> 5);

    for (int index = 0; index < 9; index++) {
	if (this->m_audioBuffers[index] == nullptr) {
	    this->m_audioBuffers[index]
		= v8::ArrayBuffer::NewBackingStore (isolate, (16u << (index / 3)) * sizeof (float));
	    std::fill_n (static_cast<float*> (this->m_audioBuffers[index]->Data ()), 16 << (index / 3), 0.0f);
	}
    }

    const v8::Local<v8::Object> result = v8::Object::New (isolate);
    static constexpr const char* sections[] = { "left", "right", "average" };

    for (int section = 0; section < 3; section++) {
	const auto buffer = v8::ArrayBuffer::New (isolate, this->m_audioBuffers[first + section]);

	JS::set (context, result, sections[section], v8::Float32Array::New (buffer, 0, resolution));
    }

    return result;
}

v8::Local<v8::Value> EngineObject::addTimer (v8::Local<v8::Function> callback, float seconds, bool interval) {
    auto* isolate = this->m_engine.getIsolate ();
    const auto* owner = this->m_engine.getRunningModule ();

    // timers belong to a script, without one running WE starts none
    if (owner == nullptr) {
	return v8::Undefined (isolate);
    }

    const uint64_t id = ++this->m_nextTimerId;
    const v8::Local<v8::Array> stopData = JS::data (isolate, { v8::Number::New (isolate, static_cast<double> (id)) });

    this->m_timers.push_back (
	{ .id = id,
	  .owner = owner->key,
	  .ownerOrder = owner->order,
	  .remaining = seconds,
	  .duration = seconds,
	  .interval = interval,
	  .callback = v8::Global<v8::Function> (isolate, callback),
	  .stopData = v8::Global<v8::Array> (isolate, stopData) }
    );

    return JS::function (this->m_engine.getContext (), engine_clear_timeout, stopData, 1);
}

// sub_181656780 disarms the function and then removes the first timer of the script that started this one, not
// necessarily this one (a script stopping its second timer stops its first), which is kept as is
bool EngineObject::stopTimer (v8::Local<v8::Array> data) {
    auto* isolate = this->m_engine.getIsolate ();
    const auto context = this->m_engine.getContext ();
    const auto value = JS::get (context, data, 0u);

    if (!value->IsNumber ()) {
	return false;
    }

    data->Set (context, 0, v8::Undefined (isolate)).Check ();

    const auto id = static_cast<uint64_t> (value.As<v8::Number> ()->Value ());
    const auto timer = std::ranges::find (this->m_timers, id, &Timer::id);

    if (timer == this->m_timers.end ()) {
	return false;
    }

    const auto first = std::ranges::find (this->m_timers, timer->owner, &Timer::owner);
    this->m_timers.erase (first);

    return true;
}

void EngineObject::tick () {
    auto* isolate = this->m_engine.getIsolate ();
    const auto context = this->m_engine.getContext ();

    // scenescript64 sub_18164F800 copies [left | right | average] into the registered buffers before any
    // callback runs, so what scripts write there lasts until the next frame
    if (this->m_audioBuffers[0] != nullptr) {
	const auto& recorder = this->m_scene.getAudioContext ().getDriver ().getRecorder ();
	const float* sources[] = { recorder.audio16, recorder.audio32, recorder.audio64 };

	for (int index = 0; index < 9; index++) {
	    const int bands = 16 << (index / 3);

	    std::copy_n (
		sources[index / 3] + (index % 3) * bands, bands,
		static_cast<float*> (this->m_audioBuffers[index]->Data ())
	    );
	}
    }

    // script by script, each one's timers in the order they were started; timers started by a callback wait for the
    // next frame. Only ids are kept, callbacks stop and start timers
    const float frameTime = std::clamp (g_Time - g_TimeLast, 0.0f, 0.25f);
    std::vector<size_t> order (this->m_timers.size ());
    std::iota (order.begin (), order.end (), 0);
    std::ranges::stable_sort (order, {}, [this] (size_t index) { return this->m_timers[index].ownerOrder; });

    std::vector<uint64_t> ids;
    ids.reserve (order.size ());
    for (const auto index : order) {
	ids.push_back (this->m_timers[index].id);
    }

    for (const auto id : ids) {
	auto timer = std::ranges::find (this->m_timers, id, &Timer::id);

	if (timer == this->m_timers.end ()) {
	    continue;
	}

	timer->remaining -= frameTime;

	if (timer->remaining > 0.0f) {
	    continue;
	}

	const v8::HandleScope handleScope (isolate);
	const v8::TryCatch tryCatch (isolate);
	const bool interval = timer->interval;
	// the callback may start timers, which can move this one
	const std::string owner = timer->owner;

	if (this->m_engine.callAsModule (owner, timer->callback.Get (isolate)).IsEmpty ()) {
	    logJSException (isolate, tryCatch, interval ? "setInterval" : "setTimeout");
	}

	// the callback may have stopped it
	timer = std::ranges::find (this->m_timers, id, &Timer::id);

	if (timer == this->m_timers.end ()) {
	    continue;
	}

	// a finished timeout's stop function does nothing from then on
	if (interval) {
	    timer->remaining = timer->duration;
	} else {
	    timer->stopData.Get (isolate)->Set (context, 0, v8::Undefined (isolate)).Check ();
	    this->m_timers.erase (timer);
	}
    }
}
