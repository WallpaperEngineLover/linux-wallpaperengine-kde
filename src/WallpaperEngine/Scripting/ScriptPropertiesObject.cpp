#include "ScriptPropertiesObject.h"

#include "JS.h"
#include "ScriptEngine.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"

#include <algorithm>

using namespace WallpaperEngine::Scripting;

namespace {
ScriptPropertiesObject& objectOf (const v8::FunctionCallbackInfo<v8::Value>& info) {
    return ScriptEngine::from (info.GetIsolate ()).getScriptPropertiesObject ();
}

// data is [the holder instance, the property name]
void scriptproperty_get (const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto* isolate = info.GetIsolate ();
    auto* instance = JS::unwrap<ScriptPropertiesObject::Instance> (JS::dataAt (info, 0));
    const std::string name = JS::toString (isolate, JS::dataAt (info, 1));

    if (instance == nullptr) {
	return;
    }

    if (const auto it = instance->assigned.find (name); it != instance->assigned.end ()) {
	info.GetReturnValue ().Set (it->second.Get (isolate));
	return;
    }

    try {
	const auto& properties = instance->value.getProperties ();
	if (const auto it = properties.find (name); it != properties.end ()) {
	    info.GetReturnValue ().Set (ScriptEngine::from (isolate).dynamicToJs (*it->second->value));
	}
    } catch (const std::exception& e) {
	JS::throwTypeError (isolate, "scriptProperties." + name + ": " + e.what ());
    }
}

void scriptproperty_set (const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto* isolate = info.GetIsolate ();
    auto* instance = JS::unwrap<ScriptPropertiesObject::Instance> (JS::dataAt (info, 0));

    if (instance != nullptr && info.Length () > 0) {
	instance->assigned[JS::toString (isolate, JS::dataAt (info, 1))].Reset (isolate, info[0]);
    }
}

// WE's baseclasses.js: vars[name] = options.value, combos take their first option's value
void scriptpropertiescreator_add (const v8::FunctionCallbackInfo<v8::Value>& info, int combo) {
    auto* isolate = info.GetIsolate ();
    const auto context = isolate->GetCurrentContext ();
    auto* creator = objectOf (info).creatorOf (info.This ());

    if (creator != nullptr && info.Length () > 0 && info[0]->IsObject ()) {
	const auto name = JS::get (context, info[0], "name");
	const auto value = combo != 0
	    ? JS::get (context, JS::get (context, JS::get (context, info[0], "options"), 0u), "value")
	    : JS::get (context, info[0], "value");

	creator->defaults.emplace_back (JS::toString (isolate, name), v8::Global<v8::Value> (isolate, value));
    }

    info.GetReturnValue ().Set (info.This ());
}

void scriptpropertiescreator_finish (const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto* isolate = info.GetIsolate ();
    const auto context = isolate->GetCurrentContext ();
    auto& object = objectOf (info);
    auto* creator = object.creatorOf (info.This ());
    const auto* module = object.getEngine ().getRunningModule ();

    if (creator == nullptr) {
	return;
    }

    if (module == nullptr) {
	sLog.error ("scriptpropertiescreator_finish: no running module - scriptProperties will be undefined");
	return;
    }

    auto& instance = object.newInstance (module->value);

    // like WE's createScriptProperties (baseclasses.js) a plain object whose settings are its own enumerable
    // properties, so hasOwnProperty, Object.keys and assignments work. They hold the add*() defaults until
    // the module body has run, then read the scene's (user bound) value (deliverValues)
    const v8::Local<v8::Object> result = v8::Object::New (isolate);

    for (auto& [name, value] : creator->defaults) {
	if (instance.assigned.contains (name)) {
	    continue;
	}

	instance.assigned[name].Reset (isolate, value.Get (isolate));

	const auto data = JS::data (isolate, { JS::external (isolate, &instance), JS::string (isolate, name) });

	result->SetAccessorProperty (
	    JS::name (isolate, name), JS::function (context, scriptproperty_get, data),
	    JS::function (context, scriptproperty_set, data, 1)
	);
    }

    info.GetReturnValue ().Set (result);
}

void scriptpropertiescreator_create (const v8::FunctionCallbackInfo<v8::Value>& info) {
    info.GetReturnValue ().Set (objectOf (info).newCreator ());
}
} // namespace

void ScriptPropertiesObject::deliverValues (Data::Model::DynamicValue& value) {
    std::erase_if (this->m_undelivered, [&] (Instance* instance) {
	if (&instance->value != &value) {
	    return false;
	}

	for (const auto& [name, unused] : value.getProperties ()) {
	    instance->assigned.erase (name);
	}

	return true;
    });
}

ScriptPropertiesObject::ScriptPropertiesObject (ScriptEngine& engine, Render::Wallpapers::CScene& scene) :
    m_scene (scene), m_engine (engine) {
    auto* isolate = engine.getIsolate ();
    const auto context = engine.getContext ();
    const v8::Local<v8::Object> prototype = v8::Object::New (isolate);

    JS::define (
	context, prototype, "addSlider", JS::function (context, JS::bind<scriptpropertiescreator_add, 0>, {}, 1)
    );
    JS::define (
	context, prototype, "addCheckbox", JS::function (context, JS::bind<scriptpropertiescreator_add, 0>, {}, 1)
    );
    JS::define (context, prototype, "addText", JS::function (context, JS::bind<scriptpropertiescreator_add, 0>, {}, 1));
    JS::define (
	context, prototype, "addCombo", JS::function (context, JS::bind<scriptpropertiescreator_add, 1>, {}, 1)
    );
    JS::define (
	context, prototype, "addColor", JS::function (context, JS::bind<scriptpropertiescreator_add, 0>, {}, 1)
    );
    JS::define (context, prototype, "finish", JS::function (context, scriptpropertiescreator_finish));

    this->m_creatorPrototype.Reset (isolate, prototype);
    this->m_creatorKey.Reset (isolate, v8::Private::New (isolate, JS::string (isolate, "IScriptPropertiesCreator")));

    // replaces the one baseclasses.js defines, ours is wired to the scene's property values
    JS::define (
	context, engine.getGlobalThis (), "createScriptProperties",
	JS::function (context, scriptpropertiescreator_create)
    );
}

v8::Local<v8::Object> ScriptPropertiesObject::newCreator () {
    auto* isolate = this->m_engine.getIsolate ();
    const auto context = this->m_engine.getContext ();
    const v8::Local<v8::Object> creator = v8::Object::New (isolate);
    auto& state = *this->m_creators.emplace_back (std::make_unique<Creator> ());

    JS::setPrototype (context, creator, this->m_creatorPrototype.Get (isolate));
    creator->SetPrivate (context, this->m_creatorKey.Get (isolate), JS::external (isolate, &state)).Check ();

    return creator;
}

ScriptPropertiesObject::Creator* ScriptPropertiesObject::creatorOf (v8::Local<v8::Value> value) {
    if (!value->IsObject ()) {
	return nullptr;
    }

    auto* isolate = this->m_engine.getIsolate ();
    v8::Local<v8::Value> state;

    if (!value.As<v8::Object> ()
	     ->GetPrivate (this->m_engine.getContext (), this->m_creatorKey.Get (isolate))
	     .ToLocal (&state)) {
	return nullptr;
    }

    return JS::unwrap<Creator> (state);
}

ScriptPropertiesObject::Instance& ScriptPropertiesObject::newInstance (Data::Model::DynamicValue& value) {
    auto& instance = *this->m_instances.emplace_back (std::make_unique<Instance> (Instance { .value = value }));

    this->m_undelivered.push_back (&instance);

    return instance;
}
