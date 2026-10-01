#pragma once

#include "ObjectAdapter.h"

#include <unordered_map>

namespace WallpaperEngine::Render::Objects {
class CImage;
}

namespace WallpaperEngine::Scripting::Adapters {
/** IAnimationLayer (wallpaper64 2.8.42 sub_14026C980) for the puppet animation layer with that serial */
JSValue makeAnimationLayerHandle (ScriptEngine& engine, ScriptableObject& object, size_t serial);

class ScriptableObjectAdapter : public ObjectAdapter {
public:
    explicit ScriptableObjectAdapter (ScriptEngine& engine, std::string name);

    JSValue instantiate (ScriptableObject& object) override;
    JSValue instantiate (Data::Model::DynamicValue& value) override;

    /** Unwraps a JS value produced by instantiate(ScriptableObject&) back to its C++ object, or
     *  nullptr if the value isn't one (wrong type, plain JS object, etc). */
    static ScriptableObject* getObject (JSValueConst value);

    void forget (const ScriptableObject& object);
    void clear ();

private:
    /** one JS object per layer like scenescript64 (getLayer sub_181632350 -> sub_181652380), scripts compare
     *  layers, key maps by them and tag them (3378399626's widget, 3577513994's icon.clicked) */
    std::unordered_map<const ScriptableObject*, JSValue> m_instances;
    JSClassExoticMethods m_exoticMethods;
    std::string m_name;
};
}