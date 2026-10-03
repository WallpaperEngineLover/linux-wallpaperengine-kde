#pragma once

#include <v8-local-handle.h>
#include <v8-persistent-handle.h>

#include <cstdint>
#include <optional>

namespace v8 {
class Object;
class Private;
}

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}
namespace WallpaperEngine::Scripting {
class ScriptEngine;
class SceneObject {
public:
    SceneObject (ScriptEngine& engine, Render::Wallpapers::CScene& scene);

    Render::Wallpapers::CScene& getScene () { return m_scene; }
    const Render::Wallpapers::CScene& getScene () const { return m_scene; }
    v8::Local<v8::Object> getInstance () const;
    ScriptEngine& getEngine () const { return m_engine; }

    /** An IModelData handle (thisScene.createModelData) for the token */
    v8::Local<v8::Object> newModelData (uint32_t token);
    /** The token of an IModelData handle, WE tags them (MDTL internal field), nothing else has one */
    std::optional<uint32_t> modelDataToken (v8::Local<v8::Value> value) const;

private:
    Render::Wallpapers::CScene& m_scene;
    ScriptEngine& m_engine;

    v8::Global<v8::Object> m_instance;
    v8::Global<v8::Private> m_modelDataKey;
};
}
