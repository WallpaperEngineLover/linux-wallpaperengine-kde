#pragma once

#include "LocalStorage.h"

#include <v8-local-handle.h>
#include <v8-persistent-handle.h>

#include <memory>

namespace v8 {
class Object;
}

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}
namespace WallpaperEngine::Scripting {
class ScriptEngine;
/** scenescript64 2.8.42 localStorage (sub_181647AA0, sub_181658680..sub_181659150) */
class LocalStorageObject {
public:
    LocalStorageObject (ScriptEngine& engine, Render::Wallpapers::CScene& scene);

    v8::Local<v8::Object> getInstance () const;
    /** Lazy, the scene learns its screen after the engine exists */
    LocalStorage::Store& getStore (bool global);

protected:
    Render::Wallpapers::CScene& m_scene;
    ScriptEngine& m_engine;
    std::shared_ptr<LocalStorage::Store> m_screenStore;
    std::shared_ptr<LocalStorage::Store> m_globalStore;

    v8::Global<v8::Object> m_instance;
};
} // namespace WallpaperEngine::Scripting
