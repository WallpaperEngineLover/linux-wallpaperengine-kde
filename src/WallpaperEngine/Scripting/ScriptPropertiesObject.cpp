#include "ScriptPropertiesObject.h"

#include "Adapters/ScriptableObjectAdapter.h"
#include "EngineObject.h"
#include "ScriptEngine.h"
#include "WallpaperEngine/Data/Utils/ScopeGuard.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"

#include <algorithm>
#include <map>
#include <vector>

using namespace WallpaperEngine::Scripting;

static uint32_t ScriptPropertiesObjectInstanceId = 0;
std::map<uint32_t, ScriptPropertiesObject&> scriptPropertiesObjectInstances;

struct OpaqueScriptPropertiesInstance {
    ScriptPropertiesObject& object;
    DynamicValue& value;
    /** add*() defaults and values a script assigned, WE's scriptProperties is a plain object that keeps them */
    std::map<std::string, JSValue> assigned {};
    /** still listed in object.m_undelivered, the finalizer must only touch object while this is set */
    bool pending = true;
};

void ScriptPropertiesObject::deliverValues (DynamicValue& value) {
    std::erase_if (this->m_undelivered, [&] (OpaqueScriptPropertiesInstance* instance) {
	if (&instance->value != &value) {
	    return false;
	}

	for (const auto& [name, unused] : value.getProperties ()) {
	    if (const auto it = instance->assigned.find (name); it != instance->assigned.end ()) {
		JS_FreeValue (this->m_engine.getContext (), it->second);
		instance->assigned.erase (it);
	    }
	}

	instance->pending = false;
	return true;
    });
}

struct OpaqueScriptProperties {
    ScriptPropertiesObject& object;
    /** add*() defaults in declaration order, used when the scene's scriptproperties don't set a name */
    std::vector<std::pair<std::string, JSValue>> defaults {};
};

namespace {
// data[0] = the holder object (opaque OpaqueScriptPropertiesInstance), data[1] = the property name
OpaqueScriptPropertiesInstance* holderOf (JSValueConst holder) {
    JSClassID classId = 0;
    return static_cast<OpaqueScriptPropertiesInstance*> (JS_GetAnyOpaque (holder, &classId));
}

JSValue scriptproperty_get (JSContext* ctx, JSValueConst, int, JSValueConst*, int, JSValue* data) {
    auto* holder = holderOf (data[0]);
    const char* name = JS_ToCString (ctx, data[1]);
    if (holder == nullptr || name == nullptr) {
	JS_FreeCString (ctx, name);
	return JS_UNDEFINED;
    }
    ScopeGuard guard ([=] { JS_FreeCString (ctx, name); });

    if (const auto it = holder->assigned.find (name); it != holder->assigned.end ()) {
	return JS_DupValue (ctx, it->second);
    }

    try {
	const auto& properties = holder->value.getProperties ();
	if (const auto it = properties.find (name); it != properties.end ()) {
	    return holder->object.getEngine ().dynamicToJs (*it->second->value);
	}
    } catch (const std::exception& e) {
	return JS_ThrowTypeError (ctx, "scriptProperties.%s: %s", name, e.what ());
    }

    return JS_UNDEFINED;
}

JSValue scriptproperty_set (JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int, JSValue* data) {
    auto* holder = holderOf (data[0]);
    const char* name = JS_ToCString (ctx, data[1]);
    if (holder != nullptr && name != nullptr && argc > 0) {
	auto& slot = holder->assigned[name];
	JS_FreeValue (ctx, slot);
	slot = JS_DupValue (ctx, argv[0]);
    }
    JS_FreeCString (ctx, name);
    return JS_UNDEFINED;
}
} // namespace

JSValue scriptpropertiescreator_add (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    JSClassID classId = 0;
    auto* container = static_cast<OpaqueScriptProperties*> (JS_GetAnyOpaque (this_val, &classId));

    // WE's baseclasses.js: vars[name] = options.value, combos take their first option's value
    if (container != nullptr && argc > 0 && JS_IsObject (argv[0])) {
	JSValue nameValue = JS_GetPropertyStr (ctx, argv[0], "name");
	const char* name = JS_ToCString (ctx, nameValue);
	JSValue value = JS_UNDEFINED;

	if (magic == 1) {
	    JSValue options = JS_GetPropertyStr (ctx, argv[0], "options");
	    JSValue first = JS_GetPropertyUint32 (ctx, options, 0);
	    value = JS_GetPropertyStr (ctx, first, "value");
	    JS_FreeValue (ctx, first);
	    JS_FreeValue (ctx, options);
	} else {
	    value = JS_GetPropertyStr (ctx, argv[0], "value");
	}

	if (name != nullptr) {
	    container->defaults.emplace_back (name, value);
	} else {
	    JS_FreeValue (ctx, value);
	}

	JS_FreeCString (ctx, name);
	JS_FreeValue (ctx, nameValue);
    }

    // this_val is a borrowed reference: returning it as-is under-counts its refcount by one per
    // chained .addSlider() call, freeing the creator object while script code still uses it.
    return JS_DupValue (ctx, this_val);
}

JSValue scriptpropertiescreator_finish (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JSClassID classId = 0;
    const auto container = static_cast<OpaqueScriptProperties*> (JS_GetAnyOpaque (this_val, &classId));

    const auto* module = container->object.getEngine ().getRunningModule ();

    if (module == nullptr) {
	sLog.error ("scriptpropertiescreator_finish: no running module - scriptProperties will be undefined");
	return JS_UNDEFINED;
    }

    // the holder keeps the module's value alive for the accessors, scripts never see it
    JSValue holder = JS_NewObjectClass (ctx, container->object.getPropertiesClassId ());
    auto* instance = new OpaqueScriptPropertiesInstance { .object = container->object, .value = module->value };
    JS_SetOpaque (holder, instance);

    // like WE's createScriptProperties (baseclasses.js) a plain object whose settings are its own enumerable
    // properties, so hasOwnProperty, Object.keys and assignments work. They hold the add*() defaults until
    // the module body has run, then read the scene's (user bound) value (deliverValues)
    JSValue result = JS_NewObject (ctx);
    std::vector<std::string> names;
    for (auto& [name, value] : container->defaults) {
	if (std::ranges::find (names, name) == names.end ()) {
	    names.push_back (name);
	    instance->assigned[name] = JS_DupValue (ctx, value);
	}
    }
    container->object.m_undelivered.push_back (instance);

    for (const auto& name : names) {
	JSValue data[] = { holder, JS_NewString (ctx, name.c_str ()) };
	JSValue getter = JS_NewCFunctionData (ctx, scriptproperty_get, 0, 0, 2, data);
	JSValue setter = JS_NewCFunctionData (ctx, scriptproperty_set, 1, 0, 2, data);
	JS_FreeValue (ctx, data[1]);

	const JSAtom atom = JS_NewAtom (ctx, name.c_str ());
	JS_DefinePropertyGetSet (ctx, result, atom, getter, setter, JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE);
	JS_FreeAtom (ctx, atom);
    }

    JS_FreeValue (ctx, holder);
    return result;
}

void scriptpropertiescreator_finalizer (JSRuntime* rt, JSValueConst val) {
    JSClassID classId = 0;
    auto* container = static_cast<OpaqueScriptProperties*> (JS_GetAnyOpaque (val, &classId));

    for (auto& [name, value] : container->defaults) {
	JS_FreeValueRT (rt, value);
    }

    delete container;
}

void scriptproperties_finalizer (JSRuntime* rt, JSValueConst val) {
    JSClassID classId = 0;
    auto* instance = static_cast<OpaqueScriptPropertiesInstance*> (JS_GetAnyOpaque (val, &classId));

    // the ScriptPropertiesObject is gone by the time JS_FreeRuntime() finalizes leftover instances
    if (instance->pending) {
	std::erase (instance->object.m_undelivered, instance);
    }

    for (auto& [name, value] : instance->assigned) {
	JS_FreeValueRT (rt, value);
    }

    delete instance;
}

JSValue
scriptpropertiescreator_create (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    const auto instance = scriptPropertiesObjectInstances.find (magic);

    if (instance == scriptPropertiesObjectInstances.end ()) {
	return JS_UNDEFINED;
    }

    JSValue creator = JS_NewObjectClass (ctx, instance->second.getCreatorClassId ());

    JS_SetOpaque (creator, new OpaqueScriptProperties { .object = instance->second });

    return creator;
}

ScriptPropertiesObject::ScriptPropertiesObject (ScriptEngine& engine, Render::Wallpapers::CScene& scene) :
    m_scene (scene), m_engine (engine), m_instanceId (++ScriptPropertiesObjectInstanceId), m_creatorClassId (0),
    m_propertiesClassId (0) {
    scriptPropertiesObjectInstances.emplace (this->m_instanceId, *this);

    this->m_creatorDefinition = {
	.class_name = "IScriptPropertiesCreator",
	.finalizer = scriptpropertiescreator_finalizer,
    };
    JS_NewClassID (this->m_engine.getRuntime (), &this->m_creatorClassId);
    JS_NewClass (this->m_engine.getRuntime (), this->m_creatorClassId, &this->m_creatorDefinition);
    this->m_creatorPrototype = JS_NewObject (this->m_engine.getContext ());

    this->m_propertiesDefinition = {
	.class_name = "IScriptProperties",
	.finalizer = scriptproperties_finalizer,
    };
    JS_NewClassID (this->m_engine.getRuntime (), &this->m_propertiesClassId);
    JS_NewClass (this->m_engine.getRuntime (), this->m_propertiesClassId, &this->m_propertiesDefinition);
    this->m_propertiesPrototype = JS_NewObject (this->m_engine.getContext ());

    JS_DupValue (this->m_engine.getContext (), this->m_propertiesPrototype);
    JS_DupValue (this->m_engine.getContext (), this->m_creatorPrototype);

    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_creatorPrototype, "addSlider",
	JS_NewCFunctionMagic (this->m_engine.getContext (), scriptpropertiescreator_add, "addSlider", 1, JS_CFUNC_generic_magic, 0), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_creatorPrototype, "addCheckbox",
	JS_NewCFunctionMagic (this->m_engine.getContext (), scriptpropertiescreator_add, "addCheckbox", 1, JS_CFUNC_generic_magic, 0),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_creatorPrototype, "addText",
	JS_NewCFunctionMagic (this->m_engine.getContext (), scriptpropertiescreator_add, "addText", 1, JS_CFUNC_generic_magic, 0), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_creatorPrototype, "addCombo",
	JS_NewCFunctionMagic (this->m_engine.getContext (), scriptpropertiescreator_add, "addCombo", 1, JS_CFUNC_generic_magic, 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_creatorPrototype, "addColor",
	JS_NewCFunctionMagic (this->m_engine.getContext (), scriptpropertiescreator_add, "addColor", 1, JS_CFUNC_generic_magic, 0), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_creatorPrototype, "finish",
	JS_NewCFunction (this->m_engine.getContext (), scriptpropertiescreator_finish, "finish", 0), JS_PROP_ENUMERABLE
    );
    // Must use JS_CFUNC_generic_magic, not JS_CFUNC_generic - the plain variant leaves `magic` as
    // garbage, which then never matches a real entry in scriptPropertiesObjectInstances.
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_engine.getGlobalThis (), "createScriptProperties",
	JS_NewCFunctionMagic (
	    this->m_engine.getContext (), scriptpropertiescreator_create, "createScriptProperties", 0,
	    JS_CFUNC_generic_magic, m_instanceId
	),
	JS_PROP_ENUMERABLE
    );

    JS_SetClassProto (this->m_engine.getContext (), this->m_propertiesClassId, this->m_propertiesPrototype);
    JS_SetClassProto (this->m_engine.getContext (), this->m_creatorClassId, this->m_creatorPrototype);
}

ScriptPropertiesObject::~ScriptPropertiesObject () {
    for (auto* instance : this->m_undelivered) {
	instance->pending = false;
    }

    this->m_undelivered.clear ();
    scriptPropertiesObjectInstances.erase (this->m_instanceId);

    JS_FreeValue (this->m_engine.getContext (), this->m_creatorPrototype);
    JS_FreeValue (this->m_engine.getContext (), this->m_propertiesPrototype);
}