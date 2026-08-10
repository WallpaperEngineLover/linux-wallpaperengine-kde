#pragma once

#include "ObjectAdapter.h"

namespace WallpaperEngine::Scripting::Adapters {
class ScriptableObjectAdapter : public ObjectAdapter {
public:
    explicit ScriptableObjectAdapter (ScriptEngine& engine, std::string name);

    JSValue instantiate (ScriptableObject& object) override;
    JSValue instantiate (Data::Model::DynamicValue& value) override;

    /** Unwraps a JS value produced by instantiate(ScriptableObject&) back to its C++ object, or
     *  nullptr if the value isn't one (wrong type, plain JS object, etc). */
    static ScriptableObject* getObject (JSValueConst value);

private:
    JSClassExoticMethods m_exoticMethods;
    std::string m_name;
};
}