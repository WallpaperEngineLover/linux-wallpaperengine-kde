#include "ScriptableObject.h"

#include "ScriptEngine.h"
#include "WallpaperEngine/Data/Model/Effect.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Data/Utils/ScopeGuard.h"

#include <cstdint>
#include <ranges>

using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Scripting;

ScriptableObject::ScriptableObject (Wallpapers::CScene& scene, const Object& object) : CObject (scene, object) {
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

DynamicValue* ScriptableObject::tryGetProperty (const std::string& name) {
    const auto it = this->m_properties.find (name);

    return it == this->m_properties.end () ? nullptr : &it->second.value;
}

const std::map<std::string, ScriptableObject::PropertyEntry>& ScriptableObject::getProperties () const {
    return this->m_properties;
}

void ScriptableObject::registerEffectConstants (const std::vector<ImageEffectUniquePtr>& effects) {
    for (size_t effectIndex = 0; effectIndex < effects.size (); effectIndex++) {
	const auto& passes = effects[effectIndex]->passOverrides;

	for (size_t passIndex = 0; passIndex < passes.size (); passIndex++) {
	    const std::string prefix = "fx" + std::to_string (effectIndex) + ".p" + std::to_string (passIndex) + ".";

	    for (const auto& [constant, setting] : passes[passIndex]->constants) {
		if (!setting->value->getScriptSource ().has_value () && setting->value->getAnimation () == nullptr) {
		    continue;
		}

		this->registerProperty (
		    prefix + constant, *setting->value,
		    "obj" + std::to_string (this->getId ()) + "/" + prefix, constant
		);
	    }
	}
    }
}

void ScriptableObject::registerProperty (
    const std::string& name, DynamicValue& value, const std::string& animationGroup, const std::string& animationKey
) {
    if (const auto existing = this->m_properties.find (name); existing != this->m_properties.end ()) {
	if (&existing->second.value == &value) {
	    return;
	}

	this->getScene ().getScriptEngine ().getAnimations ().remove (existing->second.value);

	// A derived class's own field (e.g. CImage's "scale") is overriding the generic
	// groupScale/groupAngles/groupVisible fallback registered by the base constructor under the
	// the base and derived classes parse the same JSON key, so this can be a redundant queue of the same script;
	// rebind the running module in place when identical so init() does not run twice
	if (this->getScene ().getScriptEngine ().rebindScript (existing->second.key, value)) {
	    const std::string key = existing->second.key;
	    this->m_properties.erase (existing);
	    this->m_properties.emplace (name, PropertyEntry { .key = key, .value = value });
	    return;
	}

	// different script, nothing to rebind - retire the stale lookup
	this->getScene ().getScriptEngine ().retireScript (existing->second.key);
	this->m_properties.erase (existing);
    }

    // Includes the DynamicValue's own address so two different registrations under the same
    // name (see above) never end up sharing a script engine key/module filename.
    const std::string key = name + "_" + std::to_string (this->getId ()) + "_"
	+ std::to_string (reinterpret_cast<uintptr_t> (&value));

    const auto inserted = this->m_properties.emplace (name, PropertyEntry { .key = key, .value = value });

    this->getScene ().getScriptEngine ().queueScript (
	inserted.first->second.key, inserted.first->second.value, *this, animationKey.empty () ? name : animationKey
    );

    this->getScene ().getScriptEngine ().getAnimations ().add (
	animationGroup.empty () ? "obj" + std::to_string (this->getId ()) : animationGroup,
	animationKey.empty () ? name : animationKey, value
    );
}
