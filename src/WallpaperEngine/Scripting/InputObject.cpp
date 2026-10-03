#include "InputObject.h"

#include "JS.h"
#include "ScriptEngine.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"

using namespace WallpaperEngine::Scripting;

namespace {
const WallpaperEngine::Render::Wallpapers::CScene& sceneOf (const v8::FunctionCallbackInfo<v8::Value>& info) {
    return ScriptEngine::from (info.GetIsolate ()).getScene ();
}

void get_cursor_world_position (const v8::FunctionCallbackInfo<v8::Value>& info) {
    const auto& scene = sceneOf (info);

    info.GetReturnValue ().Set (
	ScriptEngine::from (info.GetIsolate ()).getAdapters ().vec3->create (scene.getCursorWorldPosition ())
    );
}

void get_cursor_screen_position (const v8::FunctionCallbackInfo<v8::Value>& info) {
    const auto& scene = sceneOf (info);

    info.GetReturnValue ().Set (
	ScriptEngine::from (info.GetIsolate ()).getAdapters ().vec2->create (scene.getCursorPixelPosition ())
    );
}

void get_cursor_left_down (const v8::FunctionCallbackInfo<v8::Value>& info) {
    info.GetReturnValue ().Set (sceneOf (info).isCursorLeftDown ());
}

// read-only properties, writes are ignored instead of aborting the calling script
void input_set_value (const v8::FunctionCallbackInfo<v8::Value>&) { }
} // namespace

InputObject::InputObject (ScriptEngine& engine, Render::Wallpapers::CScene& scene) :
    m_scene (scene), m_engine (engine) {
    auto* isolate = engine.getIsolate ();
    const auto context = engine.getContext ();
    const v8::Local<v8::Object> instance = v8::Object::New (isolate);
    const v8::Local<v8::Function> setter = JS::function (context, input_set_value, {}, 1);

    instance->SetAccessorProperty (
	JS::name (isolate, "cursorWorldPosition"), JS::function (context, get_cursor_world_position), setter
    );
    instance->SetAccessorProperty (
	JS::name (isolate, "cursorScreenPosition"), JS::function (context, get_cursor_screen_position), setter
    );
    instance->SetAccessorProperty (
	JS::name (isolate, "cursorLeftDown"), JS::function (context, get_cursor_left_down), setter
    );

    this->m_instance.Reset (isolate, instance);
}

v8::Local<v8::Object> InputObject::getInstance () const { return this->m_instance.Get (this->m_engine.getIsolate ()); }
