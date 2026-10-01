#pragma once
#include "quickjs.h"

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}
namespace WallpaperEngine::Scripting {
class ScriptEngine;
class SceneObject {
public:
    SceneObject (ScriptEngine& engine, Render::Wallpapers::CScene& scene);
    ~SceneObject ();

    Render::Wallpapers::CScene& getScene () { return m_scene; }
    const Render::Wallpapers::CScene& getScene () const { return m_scene; }
    JSValue getInstance () const { return m_instance; }
    ScriptEngine& getEngine () const { return m_engine; }
    /** IModelData handles (thisScene.createModelData) are objects of this class */
    JSClassID getModelDataClassId () const { return m_modelDataClassId; }

private:
    Render::Wallpapers::CScene& m_scene;
    ScriptEngine& m_engine;

    JSClassID m_classId;
    JSClassDef m_definition;
    JSValue m_instance;
    // JS_NewClassID only hands out an id while this is 0
    JSClassID m_modelDataClassId = 0;
    JSClassDef m_modelDataDefinition {};
};
}