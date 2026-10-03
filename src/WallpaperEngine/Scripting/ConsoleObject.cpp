#include "ConsoleObject.h"

#include <cstdlib>
#include <sstream>

#include "JS.h"
#include "ScriptEngine.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"

using namespace WallpaperEngine::Scripting;

namespace {
std::string joinArguments (const v8::FunctionCallbackInfo<v8::Value>& info) {
    // a throwing toString () only blanks its own argument
    const v8::TryCatch tryCatch (info.GetIsolate ());
    std::stringstream stream;

    for (int i = 0; i < info.Length (); i++) {
	if (i > 0) {
	    stream << ' ';
	}

	stream << JS::toString (info.GetIsolate (), info[i]);
    }

    return stream.str ();
}

// wallpaper scripts often console.log every frame, WE only shows that in its editor console
void console_log (const v8::FunctionCallbackInfo<v8::Value>& info) {
    static const bool enabled = std::getenv ("LWE_SCRIPT_LOG") != nullptr;

    if (!enabled || info.Length () < 1) {
	return;
    }

    sLog.out (joinArguments (info));
}

void console_error (const v8::FunctionCallbackInfo<v8::Value>& info) {
    if (info.Length () < 1) {
	return;
    }

    sLog.error (joinArguments (info));
}
} // namespace

ConsoleObject::ConsoleObject (ScriptEngine& engine, Render::Wallpapers::CScene& scene) :
    m_scene (scene), m_engine (engine) {
    const auto context = engine.getContext ();
    const v8::Local<v8::Object> instance = v8::Object::New (engine.getIsolate ());

    JS::define (context, instance, "log", JS::function (context, console_log, {}, 1));
    JS::define (context, instance, "error", JS::function (context, console_error, {}, 1));

    this->m_instance.Reset (engine.getIsolate (), instance);
}

v8::Local<v8::Object> ConsoleObject::getInstance () const {
    return this->m_instance.Get (this->m_engine.getIsolate ());
}
