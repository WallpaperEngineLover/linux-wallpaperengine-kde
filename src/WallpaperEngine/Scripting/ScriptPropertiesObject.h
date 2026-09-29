#pragma once
#include "quickjs.h"

#include <vector>

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}
namespace WallpaperEngine::Data::Model {
class DynamicValue;
}
struct OpaqueScriptPropertiesInstance;
namespace WallpaperEngine::Scripting {
class ScriptEngine;
class ScriptPropertiesObject {
public:
    ScriptPropertiesObject (ScriptEngine& engine, Render::Wallpapers::CScene& scene);
    ~ScriptPropertiesObject ();

    [[nodiscard]] const Render::Wallpapers::CScene& getScene () const { return m_scene; }
    [[nodiscard]] JSValue getCreatorPrototype () const { return m_creatorPrototype; }
    [[nodiscard]] JSValue getPropertiesPrototype () const { return m_propertiesPrototype; }
    [[nodiscard]] JSClassID getCreatorClassId () const { return m_creatorClassId; }
    [[nodiscard]] JSClassID getPropertiesClassId () const { return m_propertiesClassId; }
    [[nodiscard]] ScriptEngine& getEngine () const { return m_engine; }

    /**
     * WE (wallpaper64 sub_140175880 -> sub_140174DC0 -> baseclasses.js _Internal.updateScriptProperties) hands a
     * script its scene values right after evaluating the module, so top-level code still sees the add*() defaults
     */
    void deliverValues (Data::Model::DynamicValue& value);

    std::vector<OpaqueScriptPropertiesInstance*> m_undelivered;

protected:
    Render::Wallpapers::CScene& m_scene;
    ScriptEngine& m_engine;

    uint32_t m_instanceId;

    JSClassID m_creatorClassId;
    JSClassDef m_creatorDefinition;
    JSValue m_creatorPrototype;

    JSClassID m_propertiesClassId;
    JSClassDef m_propertiesDefinition;
    JSValue m_propertiesPrototype;
};
}