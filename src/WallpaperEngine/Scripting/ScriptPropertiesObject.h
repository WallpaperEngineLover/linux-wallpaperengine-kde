#pragma once

#include <v8-local-handle.h>
#include <v8-persistent-handle.h>

#include "HandleTable.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace v8 {
class Object;
class Private;
}

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}
namespace WallpaperEngine::Data::Model {
class DynamicValue;
}
namespace WallpaperEngine::Scripting {
class ScriptEngine;
class ScriptPropertiesObject {
public:
    /** What createScriptProperties () returns until finish (): the add*() defaults in declaration order, used when the
     *  scene's scriptproperties don't set a name */
    struct Creator {
	std::vector<std::pair<std::string, v8::Global<v8::Value>>> defaults;
	const Data::Model::DynamicValue* owner = nullptr;
	void* handle = nullptr;
    };

    struct Instance {
	Data::Model::DynamicValue* value;
	/** add*() defaults and values a script assigned, WE's scriptProperties is a plain object that keeps them */
	std::map<std::string, v8::Global<v8::Value>> assigned;
	void* handle = nullptr;
    };

    ScriptPropertiesObject (ScriptEngine& engine, Render::Wallpapers::CScene& scene);

    [[nodiscard]] const Render::Wallpapers::CScene& getScene () const { return m_scene; }
    [[nodiscard]] ScriptEngine& getEngine () const { return m_engine; }

    // WE (sub_140175880 -> sub_140174DC0 -> _Internal.updateScriptProperties) hands the scene values right after
    // evaluating the module, top-level code sees the add*() defaults
    void deliverValues (Data::Model::DynamicValue& value);
    /** sub_1401731D0: bound scriptproperties get the new values, replacing what the script assigned */
    void userPropertiesChanged (const std::vector<std::string>& names);

    v8::Local<v8::Object> newCreator ();
    Creator* creatorOf (v8::Local<v8::Value> value);
    Instance& newInstance (Data::Model::DynamicValue& value);
    [[nodiscard]] Instance* instanceAt (void* handle) const { return m_instanceHandles.find (handle); }

    /** The script of this value is going away: its creators and instances are freed, the JS objects that still hold
     * their handles find nothing */
    void release (const Data::Model::DynamicValue& value);
    void rebind (const Data::Model::DynamicValue& from, Data::Model::DynamicValue& to);

protected:
    Render::Wallpapers::CScene& m_scene;
    ScriptEngine& m_engine;

    std::vector<std::unique_ptr<Creator>> m_creators;
    std::vector<std::unique_ptr<Instance>> m_instances;
    std::vector<Instance*> m_undelivered;
    HandleTable<Creator> m_creatorHandles;
    HandleTable<Instance> m_instanceHandles;

    v8::Global<v8::Object> m_creatorPrototype;
    v8::Global<v8::Private> m_creatorKey;
};
}
