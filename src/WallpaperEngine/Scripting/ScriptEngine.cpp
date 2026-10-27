#include "ScriptEngine.h"
#include "WallpaperEngine/Debugging/Profiling.h"

#include "WallpaperEngine/Media/ThumbnailPalette.h"
#include "WallpaperEngine/VideoPlayback/MPV/GLPlayer.h"

#include "Adapters/ScriptableObjectAdapter.h"
#include "JS.h"
#include "LimitedAllocator.h"
#include "ScriptPropertiesObject.h"
#include "ScriptableObject.h"
#include "WallpaperEngine/Audio/AudioContext.h"
#include "WallpaperEngine/Audio/Drivers/Recorders/PlaybackRecorder.h"
#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Desktop/UserShortcut.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/CObject.h"
#include "WallpaperEngine/Render/Objects/CImage.h"
#include "WallpaperEngine/Render/Objects/CSound.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include "WallpaperEngine/Scripting/Builtins.generated.h"

#include <libplatform/libplatform.h>

#include "WallpaperEngine/Data/Utils/FloatCompare.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>

namespace WallpaperEngine::Render::Objects {
class CSound;
}
using namespace WallpaperEngine::Scripting;
using namespace WallpaperEngine::Data::Model;

extern float g_Time;
extern float g_TimeLast;

std::optional<std::string> ScriptEngine::readScriptAsset (const std::string& path) const {
    try {
	return this->m_scene.getAssetLocator ().readString (path);
    } catch (std::filesystem::filesystem_error& e) {
	sLog.error ("ScriptEngine: cannot read ", path, ": ", e.what ());
	return std::nullopt;
    }
}

namespace {
v8::MaybeLocal<v8::Module> resolveModule (
    v8::Local<v8::Context> context, v8::Local<v8::String> specifier, v8::Local<v8::FixedArray>, v8::Local<v8::Module>
) {
    auto* isolate = v8::Isolate::GetCurrent ();

    return ScriptEngine::from (isolate).importModule (JS::toString (isolate, specifier));
}
}

// scenescript64's lookup: the lowercased name under scripts/jsmodules/ plus ".js" ('WEMath' -> wemath.js), one module
// per name like its resolver (sub_1816469C0)
v8::MaybeLocal<v8::Module> ScriptEngine::importModule (const std::string& specifier) {
    std::string name (specifier);
    std::ranges::transform (name, name.begin (), [] (unsigned char c) { return std::tolower (c); });

    if (const auto it = this->m_imports.find (name); it != this->m_imports.end ()) {
	return it->second.Get (this->m_isolate);
    }

    std::string path = "scripts/jsmodules/" + name;

    if (name.find (".js") == std::string::npos) {
	path += ".js";
    }

    const auto source = this->readScriptAsset (path);

    if (!source.has_value ()) {
	JS::throwReferenceError (this->m_isolate, "could not load module '" + specifier + "'");
	return {};
    }

    v8::ScriptCompiler::Source compileSource (
	JS::string (this->m_isolate, *source), JS::origin (this->m_isolate, specifier, true)
    );
    v8::Local<v8::Module> module;

    if (!v8::ScriptCompiler::CompileModule (this->m_isolate, &compileSource).ToLocal (&module)) {
	return {};
    }

    // V8 instantiates and evaluates it along with the module importing it
    this->m_imports[name].Reset (this->m_isolate, module);

    return module;
}

v8::Local<v8::Value> ScriptEngine::dynamicToJs (DynamicValue& value) const {
    switch (value.getType ()) {
	case DynamicValue::Null:
	    return v8::Null (this->m_isolate);
	case DynamicValue::String:
	    return JS::string (this->m_isolate, value.getString ());
	case DynamicValue::Float:
	    return v8::Number::New (this->m_isolate, value.getFloat ());
	case DynamicValue::Int:
	    return v8::Integer::New (this->m_isolate, value.getInt ());
	case DynamicValue::Boolean:
	    return v8::Boolean::New (this->m_isolate, value.getBool ());
	case DynamicValue::Vec2:
	    return this->m_adapters.vec2->instantiate (value);
	case DynamicValue::Vec3:
	    return this->m_adapters.vec3->instantiate (value);
	case DynamicValue::Vec4:
	    return this->m_adapters.vec4->instantiate (value);
	default:
	    return v8::Undefined (this->m_isolate);
    }
}

bool ScriptEngine::hasScript (const DynamicValue& value) const {
    return std::ranges::any_of (this->m_scriptModules, [&value] (const auto& entry) {
	return &entry.second.value == &value;
    });
}

v8::Local<v8::Value> ScriptEngine::userPropertyToJs (Property& property) const {
    // same shape the real engine hands scripts (jsclasses/baseclasses.js), never the command itself
    if (property.is<PropertyUserShortcut> ()) {
	const auto shortcut = Desktop::UserShortcut::parse (property.getString ());
	v8::Local<v8::Object> result = v8::Object::New (this->m_isolate);

	JS::set (this->getContext (), result, "isbound", v8::Boolean::New (this->m_isolate, shortcut.has_value ()));

	return result;
    }

    if (property.is<PropertyColor> () && property.getType () == DynamicValue::Vec4) {
	DynamicValue rgb (glm::vec3 (property.getVec4 ()));

	return this->m_adapters.vec3->instantiate (rgb);
    }

    return this->dynamicToJs (property);
}

// real WE spreads a number returned for a vector property over every component and the property stays a
// vector, so the next update() still gets a Vec
static bool updateVectorFromNumber (DynamicValue& source, float number) {
    switch (source.getType ()) {
	case DynamicValue::Vec4:
	    source.update (glm::vec4 (number), DynamicValue::UpdateSource::Script);
	    return true;
	case DynamicValue::Vec3:
	    source.update (glm::vec3 (number), DynamicValue::UpdateSource::Script);
	    return true;
	case DynamicValue::Vec2:
	    source.update (glm::vec2 (number), DynamicValue::UpdateSource::Script);
	    return true;
	default:
	    return false;
    }
}

static bool jsToDynamicValue (v8::Isolate* isolate, v8::Local<v8::Value> val, DynamicValue& source) {
    // undefined from update() means nothing changed this frame; update(source) without a value would zero the property
    // (scale 0, invisible)
    if (val.IsEmpty () || val->IsNullOrUndefined ()) {
	return false;
    }

    const auto context = isolate->GetCurrentContext ();

    if (val->IsInt32 ()) {
	const int32_t number = val.As<v8::Int32> ()->Value ();

	// the property keeps its own type (scenescript64 sub_181620E10), "alpha = 1" leaves alpha a float
	if (source.getType () == DynamicValue::Float) {
	    source.update (static_cast<float> (number), DynamicValue::UpdateSource::Script);
	} else if (!updateVectorFromNumber (source, static_cast<float> (number))) {
	    source.update (number, DynamicValue::UpdateSource::Script);
	}
	return true;
    }

    if (val->IsBoolean ()) {
	source.update (val->IsTrue (), DynamicValue::UpdateSource::Script);
	return true;
    }

    if (val->IsNumber ()) {
	const auto number = static_cast<float> (val.As<v8::Number> ()->Value ());

	if (!updateVectorFromNumber (source, number)) {
	    source.update (number, DynamicValue::UpdateSource::Script);
	}
	return true;
    }

    if (val->IsString ()) {
	source.update (JS::toString (isolate, val), DynamicValue::UpdateSource::Script);
	return true;
    }

    if (val->IsObject ()) {
	const v8::Local<v8::Value> x = JS::get (context, val, "x");
	const v8::Local<v8::Value> y = JS::get (context, val, "y");
	const v8::Local<v8::Value> z = JS::get (context, val, "z");
	const v8::Local<v8::Value> w = JS::get (context, val, "w");

	// runs every frame, a bad value from one script must not take the whole wallpaper down
	if (!x->IsNumber () || !y->IsNumber ()) {
	    static bool reported = false;

	    if (!reported) {
		reported = true;
		sLog.error ("Script returned a vector without numeric x and y components, keeping the previous value");
	    }

	    return false;
	}

	const double xVal = x.As<v8::Number> ()->Value ();
	const double yVal = y.As<v8::Number> ()->Value ();

	if (!z->IsNumber ()) {
	    source.update (glm::vec2 (xVal, yVal), DynamicValue::UpdateSource::Script);
	    return true;
	}

	const double zVal = z.As<v8::Number> ()->Value ();

	if (!w->IsNumber ()) {
	    source.update (glm::vec3 (xVal, yVal, zVal), DynamicValue::UpdateSource::Script);
	    return true;
	}

	source.update (glm::vec4 (xVal, yVal, zVal, w.As<v8::Number> ()->Value ()), DynamicValue::UpdateSource::Script);
	return true;
    }

    return false;
}

void ScriptEngine::assignJsValue (v8::Local<v8::Value> val, DynamicValue& target) const {
    jsToDynamicValue (this->m_isolate, val, target);
}

// Objects keep "angles" in radians, scripts get degrees: scenescript64 2.8.42 converts every property whose
// descriptor has flag 4 on the way in (sub_1816208E0, * 57.29578) and out (sub_181620E10, * 0.017453292), and
// wallpaper64 only sets that flag on the object "angles" property (sub_1401E0530)
static bool isDegreeProperty (std::string_view name) { return name == "angles"; }

static DynamicValue scaledAngles (const DynamicValue& value, float factor) {
    switch (value.getType ()) {
	case DynamicValue::Float:
	    return DynamicValue (value.getFloat () * factor);
	case DynamicValue::Vec2:
	    return DynamicValue (value.getVec2 () * factor);
	case DynamicValue::Vec3:
	    return DynamicValue (value.getVec3 () * factor);
	case DynamicValue::Vec4:
	    return DynamicValue (value.getVec4 () * factor);
	default:
	    return value;
    }
}

v8::Local<v8::Value> ScriptEngine::propertyToJs (DynamicValue& value, std::string_view name) const {
    if (!isDegreeProperty (name)) {
	return this->dynamicToJs (value);
    }

    DynamicValue degrees = scaledAngles (value, 57.29578f);

    return this->dynamicToJs (degrees);
}

// scenescript64 (sub_181620E10, used for property scripts and layer setters) converts by the property's type and
// drops a value of the wrong kind: bool properties only take IsBoolean, int/float ones only IsNumber.
// 3509578940's battery fill returns a number from its visible script and stays visible in WE
static bool matchesPropertyType (v8::Local<v8::Value> val, const DynamicValue& target) {
    switch (target.getType ()) {
	case DynamicValue::Boolean:
	    return val->IsBoolean ();
	case DynamicValue::Int:
	case DynamicValue::Float:
	    return val->IsNumber ();
	default:
	    return true;
    }
}

void ScriptEngine::assignPropertyJsValue (v8::Local<v8::Value> val, DynamicValue& target, std::string_view name) const {
    if (val.IsEmpty () || !matchesPropertyType (val, target)) {
	return;
    }

    // scenescript64 (sub_181620E10, string descriptors) stores ToString of whatever the script assigns, only
    // undefined/null are ignored: 3577513994 sets a text layer to getDate()'s number
    if (target.getType () == DynamicValue::String && !val->IsString () && !val->IsNullOrUndefined ()) {
	v8::TryCatch tryCatch (this->m_isolate);
	v8::Local<v8::String> text;

	if (val->ToString (this->getContext ()).ToLocal (&text)) {
	    target.update (JS::toString (this->m_isolate, text), DynamicValue::UpdateSource::Script);
	}
	return;
    }

    if (!isDegreeProperty (name)) {
	jsToDynamicValue (this->m_isolate, val, target);
	return;
    }

    DynamicValue degrees = scaledAngles (target, 57.29578f);

    if (!jsToDynamicValue (this->m_isolate, val, degrees)) {
	return;
    }

    const DynamicValue radians = scaledAngles (degrees, 0.017453292f);

    switch (radians.getType ()) {
	case DynamicValue::Float:
	    target.update (radians.getFloat (), DynamicValue::UpdateSource::Script);
	    break;
	case DynamicValue::Vec2:
	    target.update (radians.getVec2 (), DynamicValue::UpdateSource::Script);
	    break;
	case DynamicValue::Vec3:
	    target.update (radians.getVec3 (), DynamicValue::UpdateSource::Script);
	    break;
	case DynamicValue::Vec4:
	    target.update (radians.getVec4 (), DynamicValue::UpdateSource::Script);
	    break;
	default:
	    target.update (radians, DynamicValue::UpdateSource::Script);
	    break;
    }
}

namespace {
constexpr long DEFAULT_CALL_BUDGET_MS = 0;
constexpr long DEFAULT_HEAP_LIMIT_MB = 0;
constexpr long DEFAULT_BUFFER_LIMIT_MB = 0;
// room V8 gets past the limit to unwind a script it terminated
constexpr size_t HEAP_UNWIND_ROOM = 64 * 1024 * 1024;

long envNumber (const char* name, long fallback) {
    const char* text = std::getenv (name);
    char* end = nullptr;

    if (text == nullptr) {
	return fallback;
    }

    const long value = std::strtol (text, &end, 10);

    return end != text && *end == '\0' && value >= 0 ? value : fallback;
}
}

ScriptEngine::RunLimit::RunLimit (ScriptEngine& engine) : m_engine (engine) {
    if (engine.m_runDepth++ > 0) {
	return;
    }

    engine.m_runawayReported = false;

    if (engine.m_callBudget.count () > 0 && engine.m_watchdog != nullptr) {
	engine.m_watchdog->begin (engine.m_callBudget);
    }
}

ScriptEngine::RunLimit::~RunLimit () {
    if (--this->m_engine.m_runDepth > 0) {
	return;
    }

    // a termination that came in as the call finished is still pending and would stop the next script that runs
    const bool timedOut = this->m_engine.m_watchdog != nullptr && this->m_engine.m_watchdog->end ();

    if (timedOut || this->m_engine.m_heapExhausted) {
	this->m_engine.m_isolate->CancelTerminateExecution ();
	this->m_engine.m_heapExhausted = false;
    }
}

size_t ScriptEngine::nearHeapLimit (void* data, size_t currentLimit, size_t) {
    auto& engine = *static_cast<ScriptEngine*> (data);

    // script code allocates inside a RunLimit, anything else (the engine's own startup) just gets room
    if (engine.m_runDepth > 0) {
	engine.m_heapExhausted = true;
	engine.m_isolate->TerminateExecution ();
    }

    engine.m_heapLimitRaised = true;

    return currentLimit + HEAP_UNWIND_ROOM;
}

void ScriptEngine::stopRunaway (const LoadedModule* culprit, const char* what, std::string_view owner) {
    // our own record of why it was stopped: when V8 clears its termination state differs between versions
    const bool stopped = this->m_heapExhausted || (this->m_watchdog != nullptr && this->m_watchdog->fired ());

    // the innermost call that sees it blames its module, the ones around it only unwind
    if (!stopped || this->m_runawayReported) {
	return;
    }

    this->m_runawayReported = true;

    // looked up by address, the entry may have been rebound while the script ran
    LoadedModule* module = nullptr;

    for (auto& entry : this->m_scriptModules | std::views::values) {
	if (&entry == culprit) {
	    module = &entry;
	    break;
	}
    }

    const std::string limit = this->m_heapExhausted
	? "went over the " + std::to_string (this->m_heapLimit / (1024 * 1024)) + " MB script memory limit"
	: "ran longer than " + std::to_string (this->m_callBudget.count ()) + " ms";

    std::string name (owner.empty () ? "script" : owner);

    if (module != nullptr) {
	name = module->key;
    }

    sLog.error (
	"ScriptEngine [", name, "]: ", what, " ", limit,
	module != nullptr ? ", the script is switched off" : ", it was stopped"
    );

    if (module != nullptr) {
	module->dropped = true;
	this->retireScript (module->key);
    }
}

void ScriptEngine::restoreHeapLimit () {
    if (!this->m_heapLimitRaised) {
	return;
    }

    this->m_heapLimitRaised = false;

    // whatever the script held goes before the limit is lowered again, V8 keeps it above what is still alive
    this->m_isolate->LowMemoryNotification ();
    this->m_isolate->RemoveNearHeapLimitCallback (&ScriptEngine::nearHeapLimit, this->m_heapLimit);
    this->m_isolate->AddNearHeapLimitCallback (&ScriptEngine::nearHeapLimit, this);
}

ScriptEngine::Scope::Scope (const ScriptEngine& engine) :
    m_isolateScope (engine.m_isolate), m_handleScope (engine.m_isolate),
    m_contextScope (engine.m_context.Get (engine.m_isolate)) { }

// one isolate and context per scene, like scenescript64's engine (sub_181647AA0)
ScriptEngine::ScriptEngine (Wallpapers::CScene& scene, Media::MediaSource& mediaSource) :
    m_scene (scene), m_mediaSource (mediaSource) {
    this->m_unregisterMediaUpdateCallback
	= mediaSource.addMetadataListener ([this] (const Media::MediaSource::MediaInfo& info) {
	      this->notifyMediaUpdate (info, MediaChange::Metadata);
	  });

    this->m_unregisterAlbumArtUpdateCallback
	= mediaSource.addAlbumArtListener ([this] (const Media::MediaSource::MediaInfo& info) {
	      this->notifyMediaUpdate (info, MediaChange::Thumbnail);
	  });

    JS::platform ();

    this->m_allocator = makeLimitedAllocator (
	static_cast<size_t> (envNumber ("LWE_SCRIPT_BUFFER_MB", DEFAULT_BUFFER_LIMIT_MB)) * 1024 * 1024
    );
    this->m_callBudget = std::chrono::milliseconds (envNumber ("LWE_SCRIPT_TIMEOUT_MS", DEFAULT_CALL_BUDGET_MS));
    this->m_heapLimit = static_cast<size_t> (envNumber ("LWE_SCRIPT_HEAP_MB", DEFAULT_HEAP_LIMIT_MB)) * 1024 * 1024;

    v8::Isolate::CreateParams params;
    params.array_buffer_allocator = this->m_allocator.get ();

    if (this->m_heapLimit > 0) {
	params.constraints.set_max_old_generation_size_in_bytes (this->m_heapLimit);
    }

    this->m_isolate = v8::Isolate::New (params);
    this->m_isolate->SetData (0, this);
    this->m_watchdog = std::make_unique<ScriptWatchdog> (this->m_isolate);

    if (this->m_heapLimit > 0) {
	this->m_isolate->AddNearHeapLimitCallback (&ScriptEngine::nearHeapLimit, this);
    }

    const v8::Isolate::Scope isolateScope (this->m_isolate);
    const v8::HandleScope handleScope (this->m_isolate);
    const v8::Local<v8::Context> context = v8::Context::New (this->m_isolate);
    const v8::Context::Scope contextScope (context);

    this->m_context.Reset (this->m_isolate, context);

    // WE's own Vec2/Vec3/Vec4/Mat3/Mat4 classes, the natives below only create instances of them
    if (const auto baseClasses = this->readScriptAsset ("scripts/jsclasses/baseclasses.js"); baseClasses.has_value ()) {
	this->evaluateGlobalScript (*baseClasses, "baseclasses.js");
    }

    this->m_adapters = {
	.vec4 = std::make_unique<Adapters::VectorAdapter<4>> (*this),
	.vec3 = std::make_unique<Adapters::VectorAdapter<3>> (*this),
	.vec2 = std::make_unique<Adapters::VectorAdapter<2>> (*this),
	.object = std::make_unique<Adapters::ScriptableObjectAdapter> (*this),
    };

    this->m_engineObject = std::make_unique<EngineObject> (*this, scene);
    this->m_inputObject = std::make_unique<InputObject> (*this, scene);
    this->m_sceneObject = std::make_unique<SceneObject> (*this, scene);
    this->m_consoleObject = std::make_unique<ConsoleObject> (*this, scene);
    this->m_localStorageObject = std::make_unique<LocalStorageObject> (
	this->m_isolate, context,
	LocalStorageObject::Host {
	    .identity = [&scene] { return scene.getSceneFile ().generic_string (); },
	    .screen = [&scene] { return scene.getStorageScreen (); },
	    .evaluatingModuleBody = [this] { return this->isEvaluatingModuleBody (); },
	}
    );
    this->m_scriptPropertiesObject = std::make_unique<ScriptPropertiesObject> (*this, scene);

    this->installBuiltins ();

    const v8::Local<v8::Object> global = context->Global ();

    JS::define (context, global, "engine", this->m_engineObject->getInstance ());
    JS::define (context, global, "input", this->m_inputObject->getInstance ());
    JS::define (context, global, "thisScene", this->m_sceneObject->getInstance ());
    JS::define (context, global, "console", this->m_consoleObject->getInstance ());
    JS::define (context, global, "localStorage", this->m_localStorageObject->getInstance ());
    JS::define (context, global, "shared", v8::Object::New (this->m_isolate));
}

ScriptEngine::~ScriptEngine () {
    this->m_unregisterMediaUpdateCallback ();
    this->m_unregisterAlbumArtUpdateCallback ();

    // its thread talks to the isolate
    this->m_watchdog.reset ();

    // every handle has to go before the isolate does
    this->m_scriptModules.clear ();
    this->m_animationLayerEndedCallbacks.clear ();
    this->m_imports.clear ();

    this->m_consoleObject.reset ();
    this->m_localStorageObject.reset ();
    this->m_engineObject.reset ();
    this->m_inputObject.reset ();
    this->m_sceneObject.reset ();
    this->m_scriptPropertiesObject.reset ();

    this->m_adapters.vec4.reset ();
    this->m_adapters.vec3.reset ();
    this->m_adapters.vec2.reset ();
    this->m_adapters.object.reset ();

    this->m_context.Reset ();
    this->m_isolate->Dispose ();
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

void logException (
    v8::Isolate* isolate, v8::Local<v8::Value> exception, v8::Local<v8::Message> message, const char* context,
    const std::optional<std::string>& source
) {
    if (exception.IsEmpty ()) {
	sLog.error ("ScriptEngine [", context, "]: script execution was terminated");
	return;
    }

    // toString or a stack getter can throw too
    const v8::TryCatch inner (isolate);
    const auto jsContext = isolate->GetCurrentContext ();
    const std::string text = JS::toString (isolate, exception);
    std::string stack;

    if (const auto value = JS::get (jsContext, exception, "stack"); value->IsString ()) {
	stack = JS::toString (isolate, value);
    } else if (!message.IsEmpty ()) {
	// a thrown non-Error has no stack, the message still knows where it came from
	stack = "    at " + JS::toString (isolate, message->GetScriptResourceName ()) + ":"
	    + std::to_string (message->GetLineNumber (jsContext).FromMaybe (0)) + ":"
	    + std::to_string (message->GetStartColumn (jsContext).FromMaybe (0) + 1);
    }

    // scripts that fail every tick would otherwise print the same error every frame
    std::size_t suppressed = 0;

    if (!ScriptEngine::from (isolate).errorFilter ().admit (context, text + stack, suppressed)) {
	return;
    }

    if (suppressed > 0) {
	sLog.error ("ScriptEngine [", context, "]: ", suppressed, " repeats of earlier errors not shown");
    }

    sLog.error ("ScriptEngine [", context, "]: ", text);

    if (!stack.empty ()) {
	sLog.error ("ScriptEngine [", context, "] stack: ", stack);
    }

    if (source.has_value ()) {
	if (const auto excerpt = sourceExcerpt (stack, context, *source); !excerpt.empty ()) {
	    sLog.error ("ScriptEngine [", context, "] source:\n", excerpt);
	}
    }
}
}

void WallpaperEngine::Scripting::logJSException (
    v8::Isolate* isolate, const v8::TryCatch& tryCatch, const char* context, const std::optional<std::string>& source
) {
    // stopRunaway () reports it, and cancelling the termination leaves nothing in the TryCatch
    if (!tryCatch.HasCaught () && ScriptEngine::from (isolate).lastCallStopped ()) {
	return;
    }

    if (!tryCatch.HasCaught ()) {
	sLog.error ("ScriptEngine [", context, "]: a native call failed without setting an exception");
	return;
    }

    logException (isolate, tryCatch.Exception (), tryCatch.Message (), context, source);
}

void ScriptEngine::evaluateGlobalScript (const std::string& source, const char* name) {
    const v8::TryCatch tryCatch (this->m_isolate);
    const auto context = this->getContext ();
    v8::ScriptOrigin origin = JS::origin (this->m_isolate, name, false);
    v8::Local<v8::Script> script;

    if (!v8::Script::Compile (context, JS::string (this->m_isolate, source), &origin).ToLocal (&script)
	|| script->Run (context).IsEmpty ()) {
	logJSException (this->m_isolate, tryCatch, name, source);
    }
}

void ScriptEngine::installBuiltins () { this->evaluateGlobalScript (SCENE_SCRIPT_BUILTINS, "<scene-script-builtins>"); }

void ScriptEngine::ensureLayerRegistry () {
    if (this->m_layerRegistryReady) {
	return;
    }

    JS::set (this->getContext (), this->getGlobalThis (), "__textLayers", v8::Object::New (this->m_isolate));
    this->m_layerRegistryReady = true;
}

ScriptLayerHandle ScriptEngine::createLayerScript (
    const std::string& scriptSource, std::map<std::string, UserSettingUniquePtr>& initialScriptProps,
    const std::string& initialText
) {
    const Scope scope (*this);
    const auto context = this->getContext ();
    const auto global = this->getGlobalThis ();

    this->ensureLayerRegistry ();

    // initial scriptProperties and text as temporary globals for the IIFE
    const v8::Local<v8::Object> seedProps = v8::Object::New (this->m_isolate);

    for (auto& [name, dynVal] : initialScriptProps) {
	JS::set (context, seedProps, name, this->dynamicToJs (*dynVal->value));
    }

    JS::set (context, global, "__layerSeedProps", seedProps);
    JS::set (context, global, "__layerSeedText", JS::string (this->m_isolate, initialText));

    const ScriptLayerHandle id = this->m_nextLayerId++;

    // WE scripts come as ES6 modules, the text layer sandbox below runs them as a plain script
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

    // an IIFE per layer so their update()/scriptProperties don't clobber each other, hooks are kept in
    // globalThis.__textLayers[id]
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
	// minimal `engine` shim for text scripts, frametime is what InsertFPS reads
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
	// _tick supports both conventions: mutating thisLayer.text or returning the new string
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

    const RunLimit limit (*this);
    const v8::TryCatch tryCatch (this->m_isolate);
    v8::ScriptOrigin origin = JS::origin (this->m_isolate, "<layer-script>", false);
    v8::Local<v8::Script> script;
    const bool ok
	= v8::Script::Compile (context, JS::string (this->m_isolate, wrapper.str ()), &origin).ToLocal (&script)
	&& !script->Run (context).IsEmpty ();

    if (!ok) {
	this->stopRunaway (nullptr, "createLayerScript");
	logJSException (this->m_isolate, tryCatch, "createLayerScript");
    }

    // unset so they don't leak into the next createLayerScript
    JS::set (context, global, "__layerSeedProps", v8::Undefined (this->m_isolate));
    JS::set (context, global, "__layerSeedText", v8::Undefined (this->m_isolate));

    if (!ok) {
	return kInvalidLayerHandle;
    }

    this->m_layerInitialized[id] = false;
    return id;
}

v8::Local<v8::Value> ScriptEngine::textLayer (ScriptLayerHandle handle) {
    const auto context = this->getContext ();
    const auto layers = JS::get (context, this->getGlobalThis (), "__textLayers");

    return JS::get (context, layers, static_cast<uint32_t> (handle));
}

void ScriptEngine::tickLayer (ScriptLayerHandle handle, double time, double deltaTime, double fps) {
    if (handle == kInvalidLayerHandle) {
	return;
    }

    const Scope scope (*this);
    const auto context = this->getContext ();
    const v8::Local<v8::Object> sceneCtx = v8::Object::New (this->m_isolate);

    JS::set (context, sceneCtx, "time", v8::Number::New (this->m_isolate, time));
    JS::set (context, sceneCtx, "dt", v8::Number::New (this->m_isolate, deltaTime));
    JS::set (context, sceneCtx, "fps", v8::Number::New (this->m_isolate, fps));
    JS::set (context, this->getGlobalThis (), "__sceneCtx", sceneCtx);

    const auto layer = this->textLayer (handle);

    if (!layer->IsObject ()) {
	return;
    }

    const auto callHook = [&] (const char* property, const char* tag) {
	const RunLimit limit (*this);
	const v8::TryCatch tryCatch (this->m_isolate);
	const auto function = JS::get (context, layer, property);

	if (function->IsFunction () && function.As<v8::Function> ()->Call (context, layer, 0, nullptr).IsEmpty ()) {
	    this->stopRunaway (nullptr, tag);

	    if (!this->m_runawayReported) {
		logJSException (this->m_isolate, tryCatch, tag);
		return;
	    }

	    // it would be stopped again every frame
	    JS::set (context, layer.As<v8::Object> (), "_init", v8::Null (this->m_isolate));
	    JS::set (context, layer.As<v8::Object> (), "_tick", v8::Null (this->m_isolate));
	}
    };

    const auto it = this->m_layerInitialized.find (handle);
    if (it != this->m_layerInitialized.end () && !it->second) {
	callHook ("_init", "layer.init");
	it->second = true;
    }
    callHook ("_tick", "layer.update");
}

std::string ScriptEngine::layerText (ScriptLayerHandle handle) {
    if (handle == kInvalidLayerHandle) {
	return {};
    }

    const Scope scope (*this);
    const v8::TryCatch tryCatch (this->m_isolate);
    const auto context = this->getContext ();
    const auto text = JS::get (context, JS::get (context, this->textLayer (handle), "thisLayer"), "text");

    if (text->IsNullOrUndefined ()) {
	return {};
    }

    return JS::toString (this->m_isolate, text);
}

void ScriptEngine::destroyLayer (ScriptLayerHandle handle) {
    if (handle == kInvalidLayerHandle) {
	return;
    }

    const Scope scope (*this);
    const auto context = this->getContext ();
    const auto layer = this->textLayer (handle);

    if (layer->IsObject ()) {
	const RunLimit limit (*this);
	const v8::TryCatch tryCatch (this->m_isolate);
	const auto function = JS::get (context, layer, "_destroy");

	if (function->IsFunction () && function.As<v8::Function> ()->Call (context, layer, 0, nullptr).IsEmpty ()) {
	    this->stopRunaway (nullptr, "layer.destroy");
	    logJSException (this->m_isolate, tryCatch, "layer.destroy");
	}
    }

    // drop the closures so GC can collect them
    if (const auto layers = JS::get (context, this->getGlobalThis (), "__textLayers"); layers->IsObject ()) {
	layers.As<v8::Object> ()->Delete (context, static_cast<uint32_t> (handle)).FromMaybe (false);
    }

    this->m_layerInitialized.erase (handle);
}

v8::MaybeLocal<v8::Value>
ScriptEngine::call (const LoadedModule& module, const char* name, int argc, v8::Local<v8::Value> argv[]) {
    if (module.module.IsEmpty ()) {
	return v8::Undefined (this->m_isolate);
    }

    const auto context = this->getContext ();
    const v8::Local<v8::Object> exports = module.module.Get (this->m_isolate);
    v8::Local<v8::Value> function;

    if (!exports->Get (context, JS::name (this->m_isolate, name)).ToLocal (&function)) {
	return {};
    }

    if (!function->IsFunction ()) {
	return v8::Undefined (this->m_isolate);
    }

    const RunLimit limit (*this);
    const auto result = function.As<v8::Function> ()->Call (context, exports, argc, argv);

    if (result.IsEmpty ()) {
	this->stopRunaway (&module, name);
    }

    return result;
}

void ScriptEngine::retireScript (const std::string& key) { this->m_retiredScriptKeys.push_back (key); }

void ScriptEngine::destroyObjectScripts (const ScriptableObject& object) {
    {
	const Scope scope (*this);

	for (auto& module : this->m_scriptModules | std::views::values) {
	    if (module.object != &object || !module.initialized || module.dropped) {
		continue;
	    }

	    const v8::TryCatch tryCatch (this->m_isolate);

	    if (this->callEvent (module, "destroy", 0, nullptr).IsEmpty ()) {
		logJSException (this->m_isolate, tryCatch, "destroy", module.value.getScriptSource ());
	    }
	}
    }

    this->dropObjectScripts (object);
}

void ScriptEngine::dropObjectScripts (const ScriptableObject& object) {
    const Scope scope (*this);
    const auto context = this->getContext ();

    this->m_adapters.object->forget (object);

    for (auto& [key, module] : this->m_scriptModules) {
	if (module.object != &object) {
	    continue;
	}

	module.object = nullptr;
	module.dropped = true;
	this->retireScript (key);
    }

    for (const char* global : { "thisLayer", "thisObject" }) {
	const auto current = JS::get (context, this->getGlobalThis (), global);

	if (this->m_adapters.object->getObject (current) == &object) {
	    JS::set (context, this->getGlobalThis (), global, v8::Undefined (this->m_isolate));
	}
    }
}

void ScriptEngine::dropModulesSince (const uint64_t mark) {
    std::set<const ScriptableObject*> owners;

    for (const auto& module : this->m_scriptModules | std::views::values) {
	if (module.order >= mark && module.object != nullptr) {
	    owners.insert (module.object);
	}
    }

    // the owners are freed already, dropObjectScripts () only compares their addresses
    for (const auto* owner : owners) {
	this->dropObjectScripts (*owner);
    }
}

bool ScriptEngine::rebindScript (const std::string& key, DynamicValue& newValue) {
    const auto it = this->m_scriptModules.find (key);

    if (it == this->m_scriptModules.end () || it->second.value.getScriptSource () != newValue.getScriptSource ()) {
	return false;
    }

    // LoadedModule::value is a reference, so the entry is re-emplaced under the same key (m_runningModule may point at
    // the old one); other owners set their thisObjectFactory again
    LoadedModule replacement { .value = newValue,
			       .module = std::move (it->second.module),
			       .object = it->second.object,
			       .propertyName = it->second.propertyName,
			       .order = it->second.order,
			       .key = key };
    this->m_scriptPropertiesObject->rebind (it->second.value, newValue);
    this->m_scriptModules.erase (it);
    const auto inserted = this->m_scriptModules.emplace (key, std::move (replacement));
    this->m_runningModule = &inserted.first->second;

    return true;
}

void ScriptEngine::eraseModule (const std::string& key) {
    const auto it = this->m_scriptModules.find (key);

    if (it == this->m_scriptModules.end ()) {
	return;
    }

    this->m_scriptPropertiesObject->release (it->second.value);
    this->m_scriptModules.erase (it);
}

void ScriptEngine::queueScript (
    const std::string& key, DynamicValue& currentValue, ScriptableObject& object, const std::string& propertyName
) {
    const auto source = currentValue.getScriptSource ();

    if (!source.has_value ()) {
	return;
    }

    if (this->m_scriptModules.contains (key)) {
	return;
    }

    const Scope scope (*this);
    const auto context = this->getContext ();
    const v8::TryCatch tryCatch (this->m_isolate);

    // the module's resource name is its key, stack traces and the source excerpt point at it
    v8::ScriptCompiler::Source compileSource (
	JS::string (this->m_isolate, *source), JS::origin (this->m_isolate, key, true)
    );
    v8::Local<v8::Module> compiled;

    if (!v8::ScriptCompiler::CompileModule (this->m_isolate, &compileSource).ToLocal (&compiled)) {
	logJSException (this->m_isolate, tryCatch, key.c_str (), source);
	return;
    }

    // register and point m_runningModule here before evaluating, top-level `createScriptProperties()...finish()`
    // resolves through getRunningModule()
    auto inserted = this->m_scriptModules.emplace (
	key,
	LoadedModule {
	    .value = currentValue,
	    .object = &object,
	    .propertyName = propertyName,
	    .order = this->m_nextModuleOrder++,
	    .key = key,
	}
    );

    if (!inserted.second) {
	return;
    }

    this->m_runningModule = &inserted.first->second;

    // module top-level code (class bodies, helpers instantiated on load) can already touch thisLayer
    this->bindThisLayer (object, this->m_runningModule);

    const auto fail = [&] (v8::Local<v8::Value> exception) {
	if (this->m_runawayReported) {
	    // stopRunaway () reported it
	} else if (tryCatch.HasCaught ()) {
	    logJSException (this->m_isolate, tryCatch, key.c_str (), source);
	} else {
	    logException (this->m_isolate, exception, {}, key.c_str (), source);
	}

	this->m_runningModule = nullptr;
	this->eraseModule (key);
    };

    // the imports a module names are evaluated while it is instantiated
    const RunLimit limit (*this);

    if (!compiled->InstantiateModule (context, resolveModule).FromMaybe (false)) {
	this->stopRunaway (nullptr, "import", key);
	fail ({});
	return;
    }

    // WE evaluates and then checks the module's status (sub_18164BFA0): with top level await V8 returns a promise
    // and keeps a throw in the module instead of raising it
    const bool wasEvaluatingModuleBody = this->m_evaluatingModuleBody;
    this->m_evaluatingModuleBody = true;
    const bool evaluated = !compiled->Evaluate (context).IsEmpty ();
    this->m_evaluatingModuleBody = wasEvaluatingModuleBody;

    if (!evaluated) {
	this->stopRunaway (nullptr, "module body", key);
    }

    // 3378399626's weather widget stores its offsets in shared at the top level from the defaults and only
    // picks the user's location up in applyUserProperties
    this->m_scriptPropertiesObject->deliverValues (currentValue);

    if (!evaluated || compiled->GetStatus () == v8::Module::kErrored) {
	fail (compiled->GetStatus () == v8::Module::kErrored ? compiled->GetException () : v8::Local<v8::Value> ());
	return;
    }

    // the entry may have been rebound or dropped while the body ran
    const auto entry = this->m_scriptModules.find (key);

    if (entry == this->m_scriptModules.end ()) {
	return;
    }

    entry->second.module.Reset (this->m_isolate, compiled->GetModuleNamespace ().As<v8::Object> ());

    this->bindThisLayer (object, &entry->second);

    // init() and the first update() run from tick(), once the whole scene has been constructed
}

v8::MaybeLocal<v8::Value> ScriptEngine::findAnimation (const std::string& name, std::optional<int> objectId) {
    const auto group = objectId.has_value () ? std::optional ("obj" + std::to_string (*objectId)) : std::nullopt;
    auto* clock = this->m_animations.findByName (name, group);

    if (clock == nullptr) {
	return {};
    }

    return this->m_adapters.object->animation (this->m_animations.getId (), clock->getId ());
}

v8::MaybeLocal<v8::Value> ScriptEngine::findAnimation (const std::string& name, const std::string& group) {
    auto* clock = this->m_animations.findByName (name, group);

    if (clock == nullptr) {
	return {};
    }

    return this->m_adapters.object->animation (this->m_animations.getId (), clock->getId ());
}

void ScriptEngine::bindThisLayer (ScriptableObject& object, LoadedModule* module) {
    const auto context = this->getContext ();
    const auto global = this->getGlobalThis ();
    const v8::Local<v8::Value> layer = this->m_adapters.object->instantiate (object);

    JS::set (context, global, "thisLayer", layer);

    // thisObject is the module's owner (scenescript64 sub_18164E4D0 pushes the object given when the script was
    // created, thisLayer the layer of the call): live WE has thisObject === thisLayer for layer properties
    if (module == nullptr || !module->thisObjectFactory) {
	JS::set (context, global, "thisObject", layer);
	return;
    }

    if (module->thisObject.IsEmpty ()) {
	const auto owner = module->thisObjectFactory (*this);

	// an effect's material only exists once its passes are set up, ask again next time
	if (owner->IsNullOrUndefined ()) {
	    JS::set (context, global, "thisObject", owner);
	    return;
	}

	module->thisObject.Reset (this->m_isolate, owner);
    }

    JS::set (context, global, "thisObject", module->thisObject.Get (this->m_isolate));
}

void ScriptEngine::dispatchAnimationEvents () {
    const auto context = this->getContext ();

    for (const auto& event : this->m_animations.takeEvents ()) {
	const auto* clock = this->m_animations.clock (event.clock);

	if (clock == nullptr) {
	    continue;
	}

	for (auto& [key, module] : this->m_scriptModules) {
	    if (&module.value != &clock->getRootValue () || !module.initialized || module.dropped) {
		continue;
	    }

	    const v8::HandleScope handleScope (this->m_isolate);
	    const v8::TryCatch tryCatch (this->m_isolate);

	    this->m_runningModule = &module;

	    if (module.object != nullptr) {
		this->bindThisLayer (*module.object, &module);
	    }

	    const v8::Local<v8::Object> eventObject = v8::Object::New (this->m_isolate);
	    JS::set (context, eventObject, "name", JS::string (this->m_isolate, event.name));
	    JS::set (context, eventObject, "frame", v8::Number::New (this->m_isolate, event.frame));

	    v8::Local<v8::Value> args[] = { eventObject, this->propertyToJs (module.value, module.propertyName) };
	    v8::Local<v8::Value> result;

	    if (!this->call (module, "animationEvent", 2, args).ToLocal (&result)) {
		logJSException (this->m_isolate, tryCatch, key.c_str (), module.value.getScriptSource ());
	    } else {
		// like update(), the handler's return value becomes the property's new value
		this->assignPropertyJsValue (result, module.value, module.propertyName);
	    }

	    break;
	}
    }
}

void ScriptEngine::initializeModule (const std::string& key, LoadedModule& module) {
    module.initialized = true;

    // init() gets the base value once before update() runs, scripts stash it
    {
	const v8::TryCatch tryCatch (this->m_isolate);
	v8::Local<v8::Value> initArgs[] = { this->propertyToJs (module.value, module.propertyName) };
	const DynamicValue valueBeforeInit (module.value);
	v8::Local<v8::Value> initResult;

	if (!this->call (module, "init", 1, initArgs).ToLocal (&initResult)) {
	    logJSException (this->m_isolate, tryCatch, key.c_str (), module.value.getScriptSource ());
	} else if (
	    valueBeforeInit.getType () == module.value.getType ()
	    && valueBeforeInit.getVec4 () == module.value.getVec4 ()
	    && valueBeforeInit.getString () == module.value.getString ()
	) {
	    // init() may hand back a different starting value, unless something already moved the property meanwhile
	    this->assignPropertyJsValue (initResult, module.value, module.propertyName);
	}
    }

    // WE hands every user property to applyUserProperties() once at load, then only changed ones
    {
	const v8::TryCatch tryCatch (this->m_isolate);
	v8::Local<v8::Value> applyArgs[] = { this->initialUserProperties () };

	if (this->call (module, "applyUserProperties", 1, applyArgs).IsEmpty ()) {
	    logJSException (this->m_isolate, tryCatch, key.c_str (), module.value.getScriptSource ());
	}
    }

    this->notifyMediaUpdate (this->m_mediaSource.getMediaInfo (), MediaChange::Metadata, &module);
}

// the project's properties plus WE's own: sub_14010A520 drops the project's alignment/volume/rate/cameraparallax,
// sub_140104B60 adds its defaults for the scene's flags (0xD0 plus volume/alignment/cameraparallax, sub_14010DF40)
// and removes the disabled ones, sub_14010C650 a grey schemecolor. Sorted by name like WE's JSON object
v8::Local<v8::Object> ScriptEngine::initialUserProperties () {
    auto* isolate = this->m_isolate;
    const auto context = this->getContext ();
    const auto& scene = this->m_engineObject->getScene ();
    const auto& flags = scene.getScene ().engineProperties;
    const auto number = [isolate] (int value) -> v8::Local<v8::Value> { return v8::Integer::New (isolate, value); };
    const auto boolean = [isolate] (bool value) -> v8::Local<v8::Value> { return v8::Boolean::New (isolate, value); };

    struct EngineProperty {
	const char* name;
	v8::Local<v8::Value> value;
	bool enabled;
	// the project's own value doesn't count
	bool replaces;
    };

    const auto& settings = this->m_engineObject->getScene ().getContext ().getApp ().getContext ().settings;
    const auto& adjustments = scene.getImageAdjustments ();
    const int volume
	= settings.audio.enabled ? static_cast<int> (std::lround (settings.audio.volume * 100.0 / 128.0)) : 0;
    const int rate = static_cast<int> (std::lround (settings.render.playbackSpeed * 100.0f));

    const EngineProperty engineProperties[] = {
	{ "volume", number (volume), flags.volume, true },
	{ "rate", number (rate), true, true },
	{ "cameraparallax", boolean (!settings.mouse.disableparallax), flags.cameraParallax, true },
	{ "alignment", number (0), flags.alignment, true },
	{ "alignmentposition", number (50), flags.alignment, false },
	{ "alignmentx", number (50), flags.alignment, false },
	{ "alignmenty", number (50), flags.alignment, false },
	{ "alignmentz", number (100), flags.alignment, false },
	{ "alignmentfliph", boolean (adjustments.flipHorizontal.value_or (false)), true, false },
	{ "wcc_v", JS::string (isolate, adjustments.filter.value_or ("")), true, false },
	{ "wcc_amt", number (static_cast<int> (std::lround (adjustments.filterStrength.value_or (100.0f)))), true,
	  false },
	{ "wec_e", boolean (adjustments.colorEnabled.value_or (false)), true, false },
	{ "wec_brs", number (static_cast<int> (std::lround (adjustments.brightness.value_or (50.0f)))), true, false },
	{ "wec_con", number (static_cast<int> (std::lround (adjustments.contrast.value_or (50.0f)))), true, false },
	{ "wec_sa", number (static_cast<int> (std::lround (adjustments.saturation.value_or (50.0f)))), true, false },
	{ "wec_hue", number (static_cast<int> (std::lround (adjustments.hue.value_or (50.0f)))), true, false },
	{ "schemecolor", this->m_adapters.vec3->create (glm::vec3 (0.5f)), true, false },
    };

    std::map<std::string, v8::Local<v8::Value>> values;

    for (const auto& [name, property] : scene.getUserProperties ()) {
	values.emplace (name, this->userPropertyToJs (*property));
    }

    for (const auto& property : engineProperties) {
	if (!property.enabled) {
	    values.erase (property.name);
	} else if (property.replaces) {
	    values.insert_or_assign (property.name, property.value);
	} else {
	    values.try_emplace (property.name, property.value);
	}
    }

    const v8::Local<v8::Object> result = v8::Object::New (isolate);

    for (const auto& [name, value] : values) {
	JS::set (context, result, name, value);
    }

    return result;
}

namespace {
constexpr const char* CURSOR_HANDLERS[]
    = { "cursorEnter", "cursorLeave", "cursorMove", "cursorDown", "cursorUp", "cursorClick" };
}

bool ScriptEngine::hasCursorHandlers (const ScriptableObject& object) {
    const Scope scope (*this);
    const auto context = this->getContext ();

    for (auto& module : this->m_scriptModules | std::views::values) {
	if (module.object != &object || !module.initialized || module.module.IsEmpty ()) {
	    continue;
	}

	if (module.cursorHandlers < 0) {
	    const v8::TryCatch tryCatch (this->m_isolate);

	    module.cursorHandlers = 0;

	    for (const char* handler : CURSOR_HANDLERS) {
		if (JS::get (context, module.module.Get (this->m_isolate), handler)->IsFunction ()) {
		    module.cursorHandlers = 1;
		}
	    }
	}

	if (module.cursorHandlers > 0) {
	    return true;
	}
    }

    return false;
}

bool ScriptEngine::wantsPointerButtons () {
    if (this->m_buttonStateRead) {
	return true;
    }

    const Scope scope (*this);
    const auto context = this->getContext ();

    for (auto& module : this->m_scriptModules | std::views::values) {
	if (!module.initialized || module.module.IsEmpty ()) {
	    continue;
	}

	if (module.buttonHandlers < 0) {
	    const v8::TryCatch tryCatch (this->m_isolate);

	    module.buttonHandlers = 0;

	    for (const char* handler : { "cursorDown", "cursorUp", "cursorClick" }) {
		if (JS::get (context, module.module.Get (this->m_isolate), handler)->IsFunction ()) {
		    module.buttonHandlers = 1;
		}
	    }
	}

	if (module.buttonHandlers > 0) {
	    return true;
	}
    }

    return false;
}

void ScriptEngine::dispatchCursorEvent (
    const char* handler, ScriptableObject& object, const glm::vec2& worldPosition, const glm::vec3& localPosition,
    const std::optional<std::string>& hitBox
) {
    const Scope scope (*this);
    const auto context = this->getContext ();

    // Vec3 per WE's lib.sceneScript.d.ts, scripts call add()/subtract() on them
    const auto makePosition = [this] (const glm::vec3& value) {
	DynamicValue position (value);

	return this->m_adapters.vec3->instantiate (position);
    };

    for (auto& [key, module] : this->m_scriptModules) {
	if (module.object != &object || !module.initialized || module.cursorHandlers <= 0) {
	    continue;
	}

	const v8::HandleScope handleScope (this->m_isolate);
	const v8::TryCatch tryCatch (this->m_isolate);

	this->m_runningModule = &module;
	this->bindThisLayer (*module.object, &module);

	const v8::Local<v8::Object> event = v8::Object::New (this->m_isolate);
	JS::set (context, event, "button", v8::Integer::New (this->m_isolate, 0));
	JS::set (context, event, "worldPosition", makePosition (glm::vec3 (worldPosition, 0.0f)));
	JS::set (context, event, "localPosition", makePosition (localPosition));

	if (hitBox.has_value ()) {
	    JS::set (context, event, "hitBox", JS::string (this->m_isolate, *hitBox));
	}

	v8::Local<v8::Value> args[] = { event };

	if (this->call (module, handler, 1, args).IsEmpty ()) {
	    logJSException (this->m_isolate, tryCatch, key.c_str (), module.value.getScriptSource ());
	}
    }

    this->m_runningModule = nullptr;
}

void ScriptEngine::addAnimationLayerEndedCallback (
    const ScriptableObject& owner, size_t serial, v8::Local<v8::Value> callback
) {
    if (!callback->IsFunction ()) {
	return;
    }

    this->m_animationLayerEndedCallbacks.push_back (
	{ .owner = &owner,
	  .serial = serial,
	  .callback = v8::Global<v8::Function> (this->m_isolate, callback.As<v8::Function> ()),
	  .module = this->m_runningModule }
    );
}

void ScriptEngine::dispatchAnimationLayerEnded (const ScriptableObject& owner, size_t serial) {
    const Scope scope (*this);
    const auto context = this->getContext ();

    // index based, a callback is free to register more callbacks
    for (size_t index = 0; index < this->m_animationLayerEndedCallbacks.size (); index++) {
	const auto& entry = this->m_animationLayerEndedCallbacks[index];

	if (entry.owner != &owner || entry.serial != serial) {
	    continue;
	}

	// the registering module may have been retired since, only a live one gets rebound
	LoadedModule* module = nullptr;
	for (auto& candidate : this->m_scriptModules | std::views::values) {
	    if (&candidate == entry.module && !candidate.dropped && candidate.object != nullptr) {
		module = &candidate;
		break;
	    }
	}

	const v8::HandleScope handleScope (this->m_isolate);
	const RunLimit limit (*this);
	const v8::TryCatch tryCatch (this->m_isolate);
	const v8::Local<v8::Function> callback = entry.callback.Get (this->m_isolate);

	LoadedModule* previous = this->m_runningModule;
	if (module != nullptr) {
	    this->m_runningModule = module;
	    this->bindThisLayer (*module->object, module);
	}

	const bool failed = callback->Call (context, v8::Undefined (this->m_isolate), 0, nullptr).IsEmpty ();

	this->m_runningModule = previous;

	if (!failed) {
	    continue;
	}

	this->stopRunaway (module, "animation layer ended callback");

	if (!this->m_runawayReported) {
	    logJSException (this->m_isolate, tryCatch, "animation layer ended callback");
	    continue;
	}

	// it would run away again at the next end
	this->m_animationLayerEndedCallbacks.erase (this->m_animationLayerEndedCallbacks.begin () + index);
	index--;
    }
}

void ScriptEngine::dispatchClipEvent (const ScriptableObject& owner, const std::string& payload) {
    const Scope scope (*this);
    const auto context = this->getContext ();

    for (auto& [key, module] : this->m_scriptModules) {
	if (module.object != &owner || !module.initialized || module.dropped || module.module.IsEmpty ()) {
	    continue;
	}

	const v8::HandleScope handleScope (this->m_isolate);
	v8::Local<v8::Value> function;

	if (!module.module.Get (this->m_isolate)
		 ->Get (context, JS::name (this->m_isolate, "animationEvent"))
		 .ToLocal (&function)
	    || !function->IsFunction ()) {
	    continue;
	}

	v8::Local<v8::Value> event = v8::Null (this->m_isolate);
	{
	    const v8::TryCatch parseCatch (this->m_isolate);
	    v8::Local<v8::Value> parsed;

	    if (v8::JSON::Parse (context, JS::string (this->m_isolate, payload)).ToLocal (&parsed)) {
		event = parsed;
	    }
	}

	const v8::TryCatch tryCatch (this->m_isolate);
	LoadedModule* previous = this->m_runningModule;
	this->m_runningModule = &module;
	this->bindThisLayer (*module.object, &module);

	v8::Local<v8::Value> args[] = { event, this->propertyToJs (module.value, module.propertyName) };
	v8::Local<v8::Value> result;

	if (!this->call (module, "animationEvent", 2, args).ToLocal (&result)) {
	    logJSException (this->m_isolate, tryCatch, key.c_str (), module.value.getScriptSource ());
	} else {
	    this->assignPropertyJsValue (result, module.value, module.propertyName);
	}

	this->m_runningModule = previous;
    }
}

v8::MaybeLocal<v8::Value> ScriptEngine::callAsModule (const std::string& key, v8::Local<v8::Function> callback) {
    const auto it = this->m_scriptModules.find (key);
    LoadedModule* module = it == this->m_scriptModules.end () || it->second.dropped ? nullptr : &it->second;
    LoadedModule* previous = this->m_runningModule;

    if (module != nullptr) {
	this->m_runningModule = module;

	if (module->object != nullptr) {
	    this->bindThisLayer (*module->object, module);
	}
    }

    const RunLimit limit (*this);
    const auto result = callback->Call (this->getContext (), v8::Undefined (this->m_isolate), 0, nullptr);
    this->m_runningModule = previous;

    if (result.IsEmpty ()) {
	this->stopRunaway (module, "callback");
    }

    return result;
}

void ScriptEngine::setThisObjectFactory (
    const std::string& key, std::function<v8::Local<v8::Value> (ScriptEngine&)> factory
) {
    const auto it = this->m_scriptModules.find (key);

    if (it == this->m_scriptModules.end ()) {
	return;
    }

    it->second.thisObjectFactory = std::move (factory);
    it->second.thisObject.Reset ();
}

void ScriptEngine::initializePending () {
    const Scope scope (*this);
    LoadedModule* previous = this->m_runningModule;

    // init () can create layers with scripts of their own
    for (bool again = true; again;) {
	again = false;

	std::vector<std::pair<uint64_t, std::string>> order;
	for (const auto& [key, module] : this->m_scriptModules) {
	    if (!module.initialized && !module.dropped) {
		order.emplace_back (module.order, key);
	    }
	}
	std::ranges::sort (order);

	for (const auto& [unused, key] : order) {
	    const auto entry = this->m_scriptModules.find (key);
	    if (entry == this->m_scriptModules.end () || entry->second.dropped || entry->second.initialized) {
		continue;
	    }
	    auto& module = entry->second;

	    const v8::HandleScope handleScope (this->m_isolate);

	    this->m_runningModule = &module;

	    if (module.object != nullptr) {
		this->bindThisLayer (*module.object, &module);
	    }

	    this->initializeModule (key, module);
	    again = true;
	}
    }

    this->m_runningModule = previous;
}

void ScriptEngine::tick () {
    const Scope scope (*this);
    const auto context = this->getContext ();

    // tasks V8 posts back to this thread (GC finalization, compile jobs) only run when asked to
    while (v8::platform::PumpMessageLoop (&JS::platform (), this->m_isolate)) { }

    this->m_engineObject->tick ();

    // collected first, a callback is free to register more callbacks or fetch more video textures
    {
	const v8::HandleScope handleScope (this->m_isolate);

	for (const auto& callback : this->m_adapters.object->takeDueVideoEndedCallbacks ()) {
	    const RunLimit limit (*this);
	    const v8::TryCatch tryCatch (this->m_isolate);

	    if (callback->Call (context, v8::Undefined (this->m_isolate), 0, nullptr).IsEmpty ()) {
		this->stopRunaway (nullptr, "video ended callback");

		if (!this->m_runawayReported) {
		    logJSException (this->m_isolate, tryCatch, "video ended callback");
		    continue;
		}

		// it would run away again at the next end
		this->m_adapters.object->dropVideoEndedCallback (callback);
	    }
	}
    }

    // safe to erase retired modules now, nothing is mid-iteration and no queueScript() is on the stack
    for (const auto& key : this->m_retiredScriptKeys) {
	this->eraseModule (key);
    }
    this->m_retiredScriptKeys.clear ();
    this->restoreHeapLimit ();

    // long stalls (window dragged, suspend) shouldn't skip whole animations
    this->m_animations.tick (std::clamp (g_Time - g_TimeLast, 0.0f, 0.25f));

    // scene order like WE (3444535389's bee reads values an earlier object's script writes, NaN otherwise); keys looked
    // up per module, update() may rebind
    std::vector<std::pair<uint64_t, std::string>> order;
    order.reserve (this->m_scriptModules.size ());
    for (const auto& [key, module] : this->m_scriptModules) {
	order.emplace_back (module.order, key);
    }
    std::ranges::sort (order);

    for (const auto& [unused, key] : order) {
	const auto entry = this->m_scriptModules.find (key);
	if (entry == this->m_scriptModules.end () || entry->second.dropped) {
	    continue;
	}
	auto& module = entry->second;

	LWE_ZONE_NAMED (key);
	const v8::HandleScope handleScope (this->m_isolate);

	this->m_runningModule = &module;

	// thisLayer is one global, rebind it to this module's layer
	if (module.object != nullptr) {
	    this->bindThisLayer (*module.object, &module);
	}

	if (!module.initialized) {
	    this->initializeModule (key, module);
	}

	const v8::TryCatch tryCatch (this->m_isolate);
	v8::Local<v8::Value> args[] = { this->propertyToJs (module.value, module.propertyName) };
	v8::Local<v8::Value> result;

	// scenescript64 keeps the index of the export being called (+1580, 1 = update) for IModelData.replaceData
	this->m_runningUpdate = true;
	const bool ok = this->call (module, "update", 1, args).ToLocal (&result);
	this->m_runningUpdate = false;

	if (!ok) {
	    logJSException (this->m_isolate, tryCatch, key.c_str (), module.value.getScriptSource ());
	    continue;
	}

	this->assignPropertyJsValue (result, module.value, module.propertyName);
    }

    this->dispatchAnimationEvents ();
}

Media::ThumbnailPalette ScriptEngine::thumbnailPaletteFor (const Media::MediaSource::MediaInfo& media) {
    if (!media.cover.has_value ()) {
	return {};
    }

    // every module gets the event, decode the cover once per file
    if (this->m_palettePath == *media.cover) {
	return this->m_palette;
    }

    this->m_palettePath = *media.cover;
    this->m_palette = Media::loadThumbnailPalette (media.cover->string ());

    return this->m_palette;
}

v8::MaybeLocal<v8::Value>
ScriptEngine::callEvent (LoadedModule& module, const char* name, int argc, v8::Local<v8::Value> argv[]) {
    // with its own layer as thisLayer
    LoadedModule* previous = this->m_runningModule;
    this->m_runningModule = &module;

    if (module.object != nullptr) {
	this->bindThisLayer (*module.object, &module);
    }

    const auto result = this->call (module, name, argc, argv);
    this->m_runningModule = previous;
    return result;
}

void ScriptEngine::notifyResizeScreen (const glm::vec2& size) {
    const Scope scope (*this);
    auto* isolate = this->m_isolate;

    for (auto& module : this->m_scriptModules | std::views::values) {
	if (!module.initialized || module.dropped) {
	    continue;
	}

	const v8::TryCatch tryCatch (isolate);
	v8::Local<v8::Value> args[] = { this->m_adapters.vec2->create (size) };

	if (this->callEvent (module, "resizeScreen", 1, args).IsEmpty ()) {
	    logJSException (isolate, tryCatch, "resizeScreen", module.value.getScriptSource ());
	}
    }
}

void ScriptEngine::notifyUserPropertiesChanged (const std::vector<std::string>& names) {
    const Scope scope (*this);
    this->m_scriptPropertiesObject->userPropertiesChanged (names);
    const auto context = this->getContext ();
    auto* isolate = this->m_isolate;
    const auto& properties = this->m_engineObject->getScene ().getUserProperties ();

    for (auto& module : this->m_scriptModules | std::views::values) {
	if (!module.initialized || module.dropped) {
	    continue;
	}

	const v8::TryCatch tryCatch (isolate);
	const v8::Local<v8::Object> userProps = v8::Object::New (isolate);

	for (const auto& name : names) {
	    if (const auto it = properties.find (name); it != properties.end ()) {
		JS::set (context, userProps, name, this->userPropertyToJs (*it->second));
	    }
	}

	v8::Local<v8::Value> args[] = { userProps };

	if (this->callEvent (module, "applyUserProperties", 1, args).IsEmpty ()) {
	    logJSException (isolate, tryCatch, "applyUserProperties", module.value.getScriptSource ());
	}
    }
}

void ScriptEngine::notifyMediaUpdate (
    const Media::MediaSource::MediaInfo& media, MediaChange change, LoadedModule* only
) {
    const Scope scope (*this);
    const auto context = this->getContext ();
    auto* isolate = this->m_isolate;

    const auto palette = this->thumbnailPaletteFor (media);
    const auto color = [this] (const glm::vec3& value) {
	DynamicValue dynamic (value);

	return this->m_adapters.vec3->instantiate (dynamic);
    };

    // sub_18164E4D0 cases 14 to 18
    const v8::Local<v8::Object> statusEvent = v8::Object::New (isolate);

    JS::set (context, statusEvent, "enabled", v8::Boolean::New (isolate, true));

    const v8::Local<v8::Object> propertiesEvent = v8::Object::New (isolate);

    // SMTC subtitle and content type have no MPRIS counterpart
    JS::set (context, propertiesEvent, "title", JS::string (isolate, media.title));
    JS::set (context, propertiesEvent, "artist", JS::string (isolate, media.artist));
    JS::set (context, propertiesEvent, "albumArtist", JS::string (isolate, media.albumArtist));
    JS::set (context, propertiesEvent, "albumTitle", JS::string (isolate, media.album));
    JS::set (context, propertiesEvent, "subTitle", JS::string (isolate, ""));
    JS::set (context, propertiesEvent, "genres", JS::string (isolate, media.genres));
    JS::set (context, propertiesEvent, "contentType", JS::string (isolate, ""));

    const v8::Local<v8::Object> playbackEvent = v8::Object::New (isolate);

    JS::set (context, playbackEvent, "state", v8::Integer::New (isolate, media.playbackState));

    const v8::Local<v8::Object> mediaTimelineEvent = v8::Object::New (isolate);

    JS::set (context, mediaTimelineEvent, "position", v8::Number::New (isolate, media.position));
    JS::set (context, mediaTimelineEvent, "duration", v8::Number::New (isolate, media.duration));

    const v8::Local<v8::Object> mediaThumbnailEvent = v8::Object::New (isolate);

    JS::set (context, mediaThumbnailEvent, "hasThumbnail", v8::Boolean::New (isolate, media.cover.has_value ()));
    JS::set (context, mediaThumbnailEvent, "primaryColor", color (palette.primary));
    JS::set (context, mediaThumbnailEvent, "secondaryColor", color (palette.secondary));
    JS::set (context, mediaThumbnailEvent, "tertiaryColor", color (palette.tertiary));
    JS::set (context, mediaThumbnailEvent, "textColor", color (palette.text));
    JS::set (context, mediaThumbnailEvent, "highContrastColor", color (palette.highContrast));

    std::vector<std::pair<const char*, v8::Local<v8::Value>>> events;

    if (only != nullptr) {
	// on load (sub_140172830): status, then playback state, properties, thumbnail and timeline
	events.emplace_back ("mediaStatusChanged", statusEvent);

	if (media.available && media.playbackState != Media::MediaSource::Stopped) {
	    events.emplace_back ("mediaPlaybackChanged", playbackEvent);
	}
	if (media.available && !media.title.empty ()) {
	    events.emplace_back ("mediaPropertiesChanged", propertiesEvent);
	}
	if (media.available && media.cover.has_value ()) {
	    events.emplace_back ("mediaThumbnailChanged", mediaThumbnailEvent);
	}
	if (media.available && WallpaperEngine::Data::Utils::isNonZero (media.duration)) {
	    events.emplace_back ("mediaTimelineChanged", mediaTimelineEvent);
	}
    } else if (change == MediaChange::Thumbnail) {
	events = { { "mediaThumbnailChanged", mediaThumbnailEvent } };
    } else {
	events = {
	    { "mediaPropertiesChanged", propertiesEvent },
	    { "mediaPlaybackChanged", playbackEvent },
	    { "mediaTimelineChanged", mediaTimelineEvent },
	};
    }

    for (auto& module : this->m_scriptModules | std::views::values) {
	// modules that haven't run init() yet get the current media state replayed once they do
	if (!module.initialized || (only != nullptr && only != &module)) {
	    continue;
	}

	for (auto [handler, event] : events) {
	    const v8::TryCatch tryCatch (isolate);
	    v8::Local<v8::Value> args[] = { event };

	    if (this->callEvent (module, handler, 1, args).IsEmpty ()) {
		logJSException (isolate, tryCatch, handler, module.value.getScriptSource ());
	    }
	}
    }
}
