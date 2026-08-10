#include "ScriptableObjectAdapter.h"

#include <cstring>
#include <utility>

#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Data/Utils/ScopeGuard.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Scripting/ScriptEngine.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"

using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::Data::Utils;
using namespace WallpaperEngine::Scripting::Adapters;

#define SCRIPTABLE_OPAQUE_MAGIC 0xdeadbeef

struct OpaqueScriptableObjectAdapter {
    unsigned int magic;
    ScriptableObjectAdapter& adapter;
    WallpaperEngine::Scripting::ScriptableObject& object;
};

JSValue scriptableobject_property_get (JSContext* ctx, JSValueConst obj_val, JSAtom atom, JSValueConst receiver) {
    JSClassID classId = 0;

    auto* container = static_cast<OpaqueScriptableObjectAdapter*> (JS_GetAnyOpaque (obj_val, &classId));

    if (!container || container->magic != SCRIPTABLE_OPAQUE_MAGIC) {
	return JS_EXCEPTION;
    }

    const char* name = JS_AtomToCString (ctx, atom);

    if (name == nullptr) {
	return JS_EXCEPTION;
    }

    ScopeGuard guard ([=] { JS_FreeCString (ctx, name); });

    if (auto* property = container->object.tryGetProperty (name); property != nullptr) {
	return container->adapter.getEngine ().dynamicToJs (*property);
    }

    // "size" isn't a DynamicValue-backed property, but thisLayer.size is a commonly used part
    // of the WE scripting API, so it's special-cased here. Checked against the data model
    // (Image/Text) rather than the render object (CImage/CText) because scripted properties can
    // run their first update() from inside the base ScriptableObject constructor, before the
    // derived render object has finished constructing - a dynamic_cast to it at that point would
    // incorrectly report "not yet that type".
    if (std::strcmp (name, "size") == 0) {
	const auto& modelObject = container->object.getObject ();
	const glm::vec2* size = nullptr;

	if (modelObject.is<Image> ()) {
	    size = &modelObject.as<Image> ()->size;
	} else if (modelObject.is<Text> ()) {
	    size = &modelObject.as<Text> ()->size;
	}

	if (size != nullptr) {
	    const DynamicValue sizeValue (*size);

	    return container->adapter.getEngine ().getAdapters ().vec2->instantiate (
		const_cast<DynamicValue&> (sizeValue), true
	    );
	}
    }

    return JS_UNDEFINED;
}

int scriptableobject_property_set (
    JSContext* ctx, JSValueConst obj_val, JSAtom atom, JSValueConst val, JSValueConst receiver, int flags
) {
    JSClassID classId = 0;

    auto* container = static_cast<OpaqueScriptableObjectAdapter*> (JS_GetAnyOpaque (obj_val, &classId));

    if (!container || container->magic != SCRIPTABLE_OPAQUE_MAGIC) {
	return -1;
    }

    const char* name = JS_AtomToCString (ctx, atom);

    if (name == nullptr) {
	return -1;
    }

    ScopeGuard guard ([=] { JS_FreeCString (ctx, name); });

    // Write through to the real property so `thisLayer.visible = ...` etc. actually takes
    // effect. Properties not backed by a DynamicValue (e.g. "horizontalalign", plain model
    // strings) fall through and return 0 rather than -1: since scripts run as strict-mode ES
    // modules, returning -1 here would throw and abort the whole script over an unsupported
    // property, so silently accepting the write is the safer default.
    if (auto* property = container->object.tryGetProperty (name); property != nullptr) {
	container->adapter.getEngine ().assignJsValue (val, *property);
    }

    return 0;
}

ScriptableObjectAdapter::ScriptableObjectAdapter (ScriptEngine& engine, std::string name) :
    ObjectAdapter (engine),
    m_exoticMethods ({ .get_property = scriptableobject_property_get, .set_property = scriptableobject_property_set }),
    m_name (std::move (name)) {
    this->registerType (
	{
	    .class_name = m_name.c_str (),
	    .exotic = &m_exoticMethods,
	}
    );
}

JSValue ScriptableObjectAdapter::instantiate (ScriptableObject& object) {
    JSValue result = this->ObjectAdapter::instantiate (object);
    JS_SetOpaque (
	result,
	new OpaqueScriptableObjectAdapter { .magic = SCRIPTABLE_OPAQUE_MAGIC, .adapter = *this, .object = object }
    );

    return result;
}

JSValue ScriptableObjectAdapter::instantiate (DynamicValue& value) {
    throw std::runtime_error ("Cannot create a ScriptableObject instance from a DynamicValue");
}

WallpaperEngine::Scripting::ScriptableObject* ScriptableObjectAdapter::getObject (JSValueConst value) {
    JSClassID classId = 0;
    auto* container = static_cast<OpaqueScriptableObjectAdapter*> (JS_GetAnyOpaque (value, &classId));

    if (container == nullptr || container->magic != SCRIPTABLE_OPAQUE_MAGIC) {
	return nullptr;
    }

    return &container->object;
}