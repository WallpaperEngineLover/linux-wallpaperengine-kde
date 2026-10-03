#pragma once

#include <v8-local-handle.h>
#include <v8-persistent-handle.h>

namespace v8 {
class Object;
}

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}
namespace WallpaperEngine::Scripting {
class ScriptEngine;
class InputObject {
public:
    InputObject (ScriptEngine& engine, Render::Wallpapers::CScene& scene);

    const Render::Wallpapers::CScene& getScene () const { return m_scene; }
    v8::Local<v8::Object> getInstance () const;

protected:
    Render::Wallpapers::CScene& m_scene;
    ScriptEngine& m_engine;

    v8::Global<v8::Object> m_instance;
};
}
