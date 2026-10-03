#pragma once

#include <v8-local-handle.h>
#include <v8-persistent-handle.h>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace v8 {
class Function;
class Object;
class Private;
}

namespace WallpaperEngine::Data::Model {
struct ImageEffect;
}

namespace WallpaperEngine::Render::Objects {
class CImage;
class CParticle;
}

namespace WallpaperEngine::Scripting {
class ScriptEngine;
class ScriptableObject;
}

namespace WallpaperEngine::Scripting::Adapters {
/** A member of a native object read (and, with a setter, written) from C++ on every access */
struct NativeProperty {
    std::string name;
    std::function<v8::Local<v8::Value> ()> get;
    /** none for a read only member */
    std::function<void (v8::Local<v8::Value>)> set = nullptr;
};

/** What scripts reach a layer through. Outlives the layer, `object` is cleared when it goes */
struct Layer {
    ScriptableObject* object;
    // one JS object per layer like scenescript64 (getLayer sub_181632350 -> sub_181652380), scripts compare
    // layers, key maps by them and tag them (3378399626's widget, 3577513994's icon.clicked)
    v8::Global<v8::Object> instance;
    /** ITextureAnimation of an image layer, one JS object per layer like WE's (sub_14020E670 keeps it at +1216) */
    v8::Global<v8::Object> textureAnimation;
    /** IParticleSystemInstance of a particle layer, the instance override struct (+1912) */
    v8::Global<v8::Object> particleInstance;
};

/** IAnimationLayer (wallpaper64 2.8.42 sub_14026C980) for the puppet animation layer with that serial */
v8::Local<v8::Value> makeAnimationLayerHandle (ScriptEngine& engine, ScriptableObject& object, size_t serial);

class ScriptableObjectAdapter {
public:
    explicit ScriptableObjectAdapter (ScriptEngine& engine);

    v8::Local<v8::Object> instantiate (ScriptableObject& object);

    /** The layer a JS value produced by instantiate () stands for, nullptr for anything else or a layer that is gone */
    ScriptableObject* getObject (v8::Local<v8::Value> value) const;
    Layer& layerOf (ScriptableObject& object);

    void forget (const ScriptableObject& object);

    v8::Local<v8::Value> textureAnimation (Layer& layer);
    v8::Local<v8::Value> particleInstance (Layer& layer);
    /** IEffect of one of an image's effects, one JS object per effect */
    v8::Local<v8::Value> effect (Data::Model::ImageEffect& effect);
    /** IAnimation of an animated property's timeline, one JS object per timeline (sub_14018DB00 keeps it at +248) */
    v8::Local<v8::Value> animation (int systemId, int clockId);
    /**
     * A script object the way scenescript64 builds WE's native ones: its members are own enumerable data properties
     * with live values, properties before methods, and writing a read only one throws
     */
    v8::Local<v8::Object> makeNativeObject (
	std::vector<NativeProperty> properties,
	const std::vector<std::pair<std::string, v8::Local<v8::Function>>>& methods
    );

    ScriptEngine& getEngine () const { return m_engine; }

private:
    ScriptEngine& m_engine;

    std::unordered_map<const ScriptableObject*, std::unique_ptr<Layer>> m_layers;
    // layers of objects that are gone, scripts may still hold their JS objects
    std::vector<std::unique_ptr<Layer>> m_forgotten;
    std::unordered_map<int, v8::Global<v8::Object>> m_animations;
    std::unordered_map<const Data::Model::ImageEffect*, v8::Global<v8::Object>> m_effects;
    // the native objects' members, their accessors point into these
    std::vector<std::unique_ptr<std::vector<NativeProperty>>> m_nativeMembers;
    v8::Global<v8::Private> m_layerKey;
    // layers and native objects are instances of these, like WE's template objects: their own prototype over
    // Object.prototype with an anonymous constructor (V8 then names them '[object Object]' in its errors)
    v8::Global<v8::Function> m_layerConstructor;
    v8::Global<v8::Function> m_nativeConstructor;
};
}
