#include "ScriptEngine.h"

#include "WallpaperEngine/Media/ThumbnailPalette.h"
#include "WallpaperEngine/VideoPlayback/MPV/GLPlayer.h"

#include "Adapters/ScriptableObjectAdapter.h"
#include "JS.h"
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

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <future>
#include <optional>
#include <ranges>
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
} // namespace

// same lookup as scenescript64: the import name lowercased under scripts/jsmodules/, ".js" appended
// unless the name already has it ('WEMath' -> scripts/jsmodules/wemath.js). Every import of a name gets the same
// module, the resolver (sub_1816469C0) keeps them in a map keyed by the lowercased name
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
    // update()'s contract is "return the new value"; falling off the end of a function (or an
    // early "if (cond) return x;" with no else) yields undefined and is meant as "nothing to
    // change this frame", not "reset this property to zero" - DynamicValue::update(source) with
    // no value does the latter (it's meant for genuinely-null JSON properties at parse time, see
    // DynamicValueParser), and calling it here would zero out (e.g. scale -> 0, i.e. invisible)
    // any property whose script doesn't explicitly return on every path.
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

ScriptEngine::Scope::Scope (const ScriptEngine& engine) :
    m_isolateScope (engine.m_isolate), m_handleScope (engine.m_isolate),
    m_contextScope (engine.m_context.Get (engine.m_isolate)) { }

// one isolate and context per scene, like scenescript64's engine (sub_181647AA0)
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

    JS::platform ();

    this->m_allocator.reset (v8::ArrayBuffer::Allocator::NewDefaultAllocator ());

    v8::Isolate::CreateParams params;
    params.array_buffer_allocator = this->m_allocator.get ();

    this->m_isolate = v8::Isolate::New (params);
    this->m_isolate->SetData (0, this);

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
    this->m_scriptPropertiesObject = std::make_unique<ScriptPropertiesObject> (*this, scene);

    this->installBuiltins ();

    const v8::Local<v8::Object> global = context->Global ();

    JS::define (context, global, "engine", this->m_engineObject->getInstance ());
    JS::define (context, global, "input", this->m_inputObject->getInstance ());
    JS::define (context, global, "thisScene", this->m_sceneObject->getInstance ());
    JS::define (context, global, "console", this->m_consoleObject->getInstance ());
    JS::define (context, global, "shared", v8::Object::New (this->m_isolate));
}

ScriptEngine::~ScriptEngine () {
    this->m_unregisterMediaUpdateCallback ();
    this->m_unregisterAlbumArtUpdateCallback ();

    // every handle has to go before the isolate does
    this->m_scriptModules.clear ();
    this->m_videoEndedCallbacks.clear ();
    this->m_animationLayerEndedCallbacks.clear ();
    this->m_imports.clear ();

    this->m_consoleObject.reset ();
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
    // scripts that fail every tick would otherwise print the same error every frame
    static std::map<std::string, std::string> lastReported;

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

    auto& last = lastReported[context];

    if (last == text + stack) {
	return;
    }

    last = text + stack;

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
} // namespace

void WallpaperEngine::Scripting::logJSException (
    v8::Isolate* isolate, const v8::TryCatch& tryCatch, const char* context, const std::optional<std::string>& source
) {
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

    // Seed initial scriptProperties and text as temporary globals the IIFE reads.
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

    const v8::TryCatch tryCatch (this->m_isolate);
    v8::ScriptOrigin origin = JS::origin (this->m_isolate, "<layer-script>", false);
    v8::Local<v8::Script> script;
    const bool ok
	= v8::Script::Compile (context, JS::string (this->m_isolate, wrapper.str ()), &origin).ToLocal (&script)
	&& !script->Run (context).IsEmpty ();

    if (!ok) {
	logJSException (this->m_isolate, tryCatch, "createLayerScript");
    }

    // Unset seeds so they don't leak into the next createLayerScript call.
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
	const v8::TryCatch tryCatch (this->m_isolate);
	const auto function = JS::get (context, layer, property);

	if (function->IsFunction () && function.As<v8::Function> ()->Call (context, layer, 0, nullptr).IsEmpty ()) {
	    logJSException (this->m_isolate, tryCatch, tag);
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
	const v8::TryCatch tryCatch (this->m_isolate);
	const auto function = JS::get (context, layer, "_destroy");

	if (function->IsFunction () && function.As<v8::Function> ()->Call (context, layer, 0, nullptr).IsEmpty ()) {
	    logJSException (this->m_isolate, tryCatch, "layer.destroy");
	}
    }

    // Remove the entry from globalThis.__textLayers so GC can reclaim its closures.
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

    return function.As<v8::Function> ()->Call (context, exports, argc, argv);
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

bool ScriptEngine::rebindScript (const std::string& key, DynamicValue& newValue) {
    const auto it = this->m_scriptModules.find (key);

    if (it == this->m_scriptModules.end () || it->second.value.getScriptSource () != newValue.getScriptSource ()) {
	return false;
    }

    // LoadedModule::value is a reference member, so the entry is erased and re-emplaced under the same key,
    // m_runningModule may currently point at the erased entry
    // the cached thisObject points at the old value, the replacement builds its own
    LoadedModule replacement { .value = newValue,
			       .module = std::move (it->second.module),
			       .object = it->second.object,
			       .propertyName = it->second.propertyName,
			       .order = it->second.order,
			       .key = key };
    this->m_scriptModules.erase (it);
    const auto inserted = this->m_scriptModules.emplace (key, std::move (replacement));
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

    // Register the entry and point m_runningModule at it before evaluating below - top-level module code
    // commonly does `export var scriptProperties = createScriptProperties()...finish();`, and
    // ScriptPropertiesObject's finish() resolves it through getRunningModule(), which must already point here.
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
	if (tryCatch.HasCaught ()) {
	    logJSException (this->m_isolate, tryCatch, key.c_str (), source);
	} else {
	    logException (this->m_isolate, exception, {}, key.c_str (), source);
	}

	this->m_runningModule = nullptr;
	this->m_scriptModules.erase (key);
    };

    if (!compiled->InstantiateModule (context, resolveModule).FromMaybe (false)) {
	fail ({});
	return;
    }

    // WE evaluates and then checks the module's status (sub_18164BFA0): with top level await V8 returns a promise
    // and keeps a throw in the module instead of raising it
    const bool wasEvaluatingModuleBody = this->m_evaluatingModuleBody;
    this->m_evaluatingModuleBody = true;
    const bool evaluated = !compiled->Evaluate (context).IsEmpty ();
    this->m_evaluatingModuleBody = wasEvaluatingModuleBody;

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

namespace {
// thisObject.getAnimation(): data is [animation system id, the property's DynamicValue]
void this_object_get_animation (const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto* isolate = info.GetIsolate ();
    const auto context = isolate->GetCurrentContext ();
    const int systemId = JS::dataAt (info, 0)->Int32Value (context).FromMaybe (0);
    auto* value = JS::unwrap<DynamicValue> (JS::dataAt (info, 1));

    auto* system = AnimationSystem::find (systemId);
    auto* clock = system == nullptr || value == nullptr ? nullptr : system->clockOf (*value);

    if (clock == nullptr) {
	info.GetReturnValue ().SetNull ();
	return;
    }

    info.GetReturnValue ().Set (
	ScriptEngine::from (isolate).getAdapters ().object->animation (systemId, clock->getId ())
    );
}

// thisObject.<property name>: lets a listener registered by a property's script write that property
// from outside (`listener.thisObject['Inner Color'] = color`). data is [the property's DynamicValue, is angles]
void this_object_property (const v8::FunctionCallbackInfo<v8::Value>& info, int magic) {
    auto& engine = ScriptEngine::from (info.GetIsolate ());
    auto* value = JS::unwrap<DynamicValue> (JS::dataAt (info, 0));
    const std::string_view name = JS::dataAt (info, 1)->IsTrue () ? "angles" : "";

    if (magic == 0) {
	info.GetReturnValue ().Set (engine.propertyToJs (*value, name));
	return;
    }

    if (info.Length () > 0) {
	engine.assignPropertyJsValue (info[0], *value, name);
    }
}
} // namespace

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

v8::Local<v8::Value> ScriptEngine::makeThisObject (DynamicValue& value, const std::string& propertyName) {
    const auto context = this->getContext ();
    const v8::Local<v8::Object> handle = v8::Object::New (this->m_isolate);

    JS::set (
	context, handle, "getAnimation",
	JS::function (
	    context, this_object_get_animation,
	    JS::data (
		this->m_isolate,
		{ v8::Integer::New (this->m_isolate, this->m_animations.getId ()),
		  JS::external (this->m_isolate, &value) }
	    )
	)
    );

    if (!propertyName.empty ()) {
	const auto data = JS::data (
	    this->m_isolate,
	    { JS::external (this->m_isolate, &value), v8::Boolean::New (this->m_isolate, propertyName == "angles") }
	);

	handle->SetAccessorProperty (
	    JS::name (this->m_isolate, propertyName), JS::function (context, JS::bind<this_object_property, 0>, data),
	    JS::function (context, JS::bind<this_object_property, 1>, data, 1)
	);
    }

    return handle;
}

void ScriptEngine::bindThisLayer (ScriptableObject& object, LoadedModule* module) {
    const auto context = this->getContext ();
    const auto global = this->getGlobalThis ();

    JS::set (context, global, "thisLayer", this->m_adapters.object->instantiate (object));

    // thisObject is the property the running script is attached to (what getAnimation() hangs
    // off), not the layer
    if (module == nullptr) {
	JS::set (context, global, "thisObject", this->m_adapters.object->instantiate (object));
	return;
    }

    if (module->thisObject.IsEmpty ()) {
	module->thisObject.Reset (
	    this->m_isolate,
	    module->thisObjectFactory ? module->thisObjectFactory (*this)
				      : this->makeThisObject (module->value, module->propertyName)
	);
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
    const auto context = this->getContext ();

    module.initialized = true;

    // init() receives the property's static/base value exactly once, before update() starts being
    // called every tick - scripts commonly stash it (e.g. audio-reactive scale scripts scaling a
    // captured base value) and would otherwise read undefined forever.
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

    // the real engine hands every user property to applyUserProperties() once at load (and only the
    // changed ones afterward) - scripts like music selectors rely on that first call to start up
    {
	const v8::TryCatch tryCatch (this->m_isolate);
	const v8::Local<v8::Object> userProps = v8::Object::New (this->m_isolate);

	for (const auto& [name, property] : this->m_engineObject->getScene ().getUserProperties ()) {
	    JS::set (context, userProps, name, this->userPropertyToJs (*property));
	}

	v8::Local<v8::Value> applyArgs[] = { userProps };

	if (this->call (module, "applyUserProperties", 1, applyArgs).IsEmpty ()) {
	    logJSException (this->m_isolate, tryCatch, key.c_str (), module.value.getScriptSource ());
	}
    }

    this->notifyMediaUpdate (this->m_mediaSource.getMediaInfo (), &module);
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

void ScriptEngine::addVideoEndedCallback (VideoPlayback::MPV::GLPlayer* player, v8::Local<v8::Value> callback) {
    if (player == nullptr || !callback->IsFunction ()) {
	return;
    }

    this->m_videoEndedCallbacks.push_back (
	{ .player = player, .callback = v8::Global<v8::Function> (this->m_isolate, callback.As<v8::Function> ()) }
    );
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
	const v8::TryCatch tryCatch (this->m_isolate);
	const v8::Local<v8::Function> callback = entry.callback.Get (this->m_isolate);

	LoadedModule* previous = this->m_runningModule;
	if (module != nullptr) {
	    this->m_runningModule = module;
	    this->bindThisLayer (*module->object, module);
	}

	if (callback->Call (context, v8::Undefined (this->m_isolate), 0, nullptr).IsEmpty ()) {
	    logJSException (this->m_isolate, tryCatch, "animation layer ended callback");
	}

	this->m_runningModule = previous;
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

    const auto result = callback->Call (this->getContext (), v8::Undefined (this->m_isolate), 0, nullptr);
    this->m_runningModule = previous;

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

	const v8::HandleScope handleScope (this->m_isolate);
	const v8::TryCatch tryCatch (this->m_isolate);
	const v8::Local<v8::Function> callback = entry.callback.Get (this->m_isolate);

	if (callback->Call (context, v8::Undefined (this->m_isolate), 0, nullptr).IsEmpty ()) {
	    logJSException (this->m_isolate, tryCatch, "video ended callback");
	}
    }

    // safe to erase retired modules now, nothing is mid-iteration and no queueScript() is on the stack
    for (const auto& key : this->m_retiredScriptKeys) {
	this->m_scriptModules.erase (key);
    }
    this->m_retiredScriptKeys.clear ();

    // long stalls (window dragged, suspend) shouldn't skip whole animations
    this->m_animations.tick (std::clamp (g_Time - g_TimeLast, 0.0f, 0.25f));

    // scene order like WE, not the order of the keys: 3444535389's bee script reads shared values that the
    // setup script of an earlier object writes, run before it the bee's position turned NaN for good.
    // Keys are looked up again per module, an update() may rebind (erase and re-add) an entry.
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

	const v8::HandleScope handleScope (this->m_isolate);

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

	if (key.starts_with ("scale_")) {
	    static int scaleDiagnosticCounter = 0;
	    if (++scaleDiagnosticCounter >= 300) {
		scaleDiagnosticCounter = 0;
		sLog.debug (
		    "scale script '", key, "': update() returned ",
		    JS::toString (this->m_isolate, result->TypeOf (this->m_isolate)), ", current vec3 = (",
		    module.value.getVec3 ().x, ", ", module.value.getVec3 ().y, ", ", module.value.getVec3 ().z, ")"
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

	this->assignPropertyJsValue (result, module.value, module.propertyName);
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

void ScriptEngine::notifyMediaUpdate (const Media::MediaSource::MediaInfo& media, LoadedModule* only) {
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

    JS::set (context, mediaThumbnailEvent, "hasThumbnail", v8::Boolean::New (isolate, media.url.has_value ()));
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
	if (media.available && media.url.has_value ()) {
	    events.emplace_back ("mediaThumbnailChanged", mediaThumbnailEvent);
	}
	if (media.available && media.duration != 0.0) {
	    events.emplace_back ("mediaTimelineChanged", mediaTimelineEvent);
	}
    } else {
	events = {
	    { "mediaPropertiesChanged", propertiesEvent },
	    { "mediaPlaybackChanged", playbackEvent },
	    { "mediaTimelineChanged", mediaTimelineEvent },
	    { "mediaThumbnailChanged", mediaThumbnailEvent },
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
