#include "ScriptableObject.h"

#include "ScriptEngine.h"
#include "WallpaperEngine/Data/Utils/ScopeGuard.h"

#include <cstdint>
#include <ranges>

using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Scripting;

ScriptableObject::ScriptableObject (Wallpapers::CScene& scene, const Object& object) : CObject (scene, object) {
    // register common dynamic values
    this->registerProperty ("origin", *object.origin->value);
    this->registerProperty ("scale", *object.groupScale->value);
    this->registerProperty ("angles", *object.groupAngles->value);
    this->registerProperty ("visible", *object.groupVisible->value);
}

DynamicValue& ScriptableObject::getProperty (const std::string& name) {
    const auto it = this->m_properties.find (name);

    if (it == this->m_properties.end ()) {
	sLog.exception ("Property '" + name + "' not found on object '" + this->getObject ().name + "'");
    }

    return it->second.value;
}

const std::map<std::string, ScriptableObject::PropertyEntry>& ScriptableObject::getProperties () const {
    return this->m_properties;
}

void ScriptableObject::registerProperty (const std::string& name, DynamicValue& value) {
    if (const auto existing = this->m_properties.find (name); existing != this->m_properties.end ()) {
	if (&existing->second.value == &value) {
	    return;
	}

	// A derived class's own field (e.g. CImage's "scale", the one localTransform() actually
	// renders) is overriding the generic groupScale/groupAngles/groupVisible fallback
	// ScriptableObject's base constructor already registered under the same name. Only stop
	// tracking the stale one here - do NOT unqueue/re-evaluate its already-queued script: it can
	// still be the engine's "currently running module" mid-registration, and reusing the same
	// key/filename for a fresh JS_Eval() risks colliding with QuickJS's own module identity for
	// the one just freed. It's fine to leave it ticking harmlessly in the background.
	this->m_properties.erase (existing);
    }

    // Includes the DynamicValue's own address so two different registrations under the same
    // name (see above) never end up sharing a script engine key/module filename.
    const std::string key = name + "_" + std::to_string (this->getId ()) + "_"
	+ std::to_string (reinterpret_cast<uintptr_t> (&value));

    const auto inserted = this->m_properties.emplace (name, PropertyEntry { .key = key, .value = value });

    this->getScene ().getScriptEngine ().queueScript (inserted.first->second.key, inserted.first->second.value, *this);
}
