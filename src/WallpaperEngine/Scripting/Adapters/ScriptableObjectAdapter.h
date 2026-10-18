#pragma once

#include <v8-local-handle.h>
#include <v8-persistent-handle.h>

#include "WallpaperEngine/Scripting/HandleTable.h"

#include <array>
#include <functional>
#include <map>
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
class Object;
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

/** IVideoTexture state, wallpaper64 2.8.42 sub_14020E7B0 (+232..+264 of the object it keeps at layer +1216) */
struct VideoTexture {
    float rate = 1.0f;
    bool loop = true;
    float lastTime = 0.0f;
    bool armed = true;
    std::vector<v8::Global<v8::Function>> endedCallbacks = {};
};

/** What scripts reach a layer through. Lives as long as the layer, JS objects refer to it by `handle` and find nothing
 * once it is gone */
struct Layer {
    ScriptableObject* object;
    void* handle = nullptr;
    // one JS object per layer like scenescript64 (getLayer sub_181632350 -> sub_181652380), scripts compare
    // layers, key maps by them and tag them (3378399626's widget, 3577513994's icon.clicked)
    v8::Global<v8::Object> instance;
    /** ITextureAnimation of an image layer, one JS object per layer like WE's (sub_14020E670 keeps it at +1216) */
    v8::Global<v8::Object> textureAnimation;
    /** IParticleSystemInstance of a particle layer, the instance override struct (+1912) */
    v8::Global<v8::Object> particleInstance;
    /** IVideoTexture of an image layer with a video texture, one per layer like WE's */
    v8::Global<v8::Object> videoTexture;
    std::unique_ptr<VideoTexture> videoTextureState;
    /** The members of the native objects made for this layer, and the handles their accessors carry */
    std::vector<std::unique_ptr<std::vector<NativeProperty>>> natives;
    std::vector<void*> nativeHandles;
};

/** IAnimationLayer (wallpaper64 2.8.42 sub_14026C980) for the puppet animation layer with that serial */
v8::Local<v8::Value> makeAnimationLayerHandle (ScriptEngine& engine, ScriptableObject& object, size_t serial);

/** Animation group of an animation layer's own properties, which WE keys by the layer's own id (sub_14018DB00) */
std::string animationLayerGroup (int objectId, size_t serial);

class ScriptableObjectAdapter {
public:
    explicit ScriptableObjectAdapter (ScriptEngine& engine);

    v8::Local<v8::Object> instantiate (ScriptableObject& object);

    /** The layer a JS value produced by instantiate () stands for, nullptr for anything else or a layer that is gone */
    ScriptableObject* getObject (v8::Local<v8::Value> value) const;
    Layer& layerOf (ScriptableObject& object);
    /** The layer a handle of its JS objects stands for, nullptr once the layer is gone */
    [[nodiscard]] Layer* layerAt (void* handle) const { return m_layerHandles.find (handle); }
    [[nodiscard]] NativeProperty* nativeAt (void* handle) const { return m_nativeHandles.find (handle); }

    void forget (const ScriptableObject& object);

    v8::Local<v8::Value> textureAnimation (Layer& layer);
    v8::Local<v8::Value> particleInstance (Layer& layer);
    v8::Local<v8::Value> videoTexture (Layer& layer);
    /** Steps every IVideoTexture's ended state for this frame, returns the callbacks due now (sub_1401FDF90) */
    std::vector<v8::Local<v8::Function>> takeDueVideoEndedCallbacks ();
    /** Forgets an ended callback that ran away, so it does not stop the frame at every loop */
    void dropVideoEndedCallback (v8::Local<v8::Function> callback);
    /** Whether a script ever got an IEffect or IMaterial of one of the object's effects: those keep raw references into
     * the object's model for as long as the engine lives */
    [[nodiscard]] bool hasEffectHandles (const Data::Model::Object& object) const;
    /** IEffect of one of an image's effects, one JS object per effect */
    v8::Local<v8::Value> effect (Data::Model::ImageEffect& effect);
    /** one JS object per material, null without one */
    v8::Local<v8::Value> material (Data::Model::ImageEffect& effect, size_t passIndex);
    /** IAnimation of an animated property's timeline, one JS object per timeline (sub_14018DB00 keeps it at +248) */
    v8::Local<v8::Value> animation (int systemId, int clockId);
    // a script object like scenescript64 builds WE's natives: own enumerable data properties with live values,
    // properties before methods, read only writes throw
    v8::Local<v8::Object> makeNativeObject (
	std::vector<NativeProperty> properties,
	const std::vector<std::pair<std::string, v8::Local<v8::Function>>>& methods, Layer* owner = nullptr
    );

    ScriptEngine& getEngine () const { return m_engine; }

private:
    ScriptEngine& m_engine;

    std::unordered_map<const ScriptableObject*, std::unique_ptr<Layer>> m_layers;
    HandleTable<Layer> m_layerHandles;
    HandleTable<NativeProperty> m_nativeHandles;
    std::unordered_map<int, v8::Global<v8::Object>> m_animations;
    std::unordered_map<const Data::Model::ImageEffect*, v8::Global<v8::Object>> m_effects;
    std::map<std::pair<const Data::Model::ImageEffect*, size_t>, v8::Global<v8::Object>> m_materials;
    // blending, alphawriting, depthtest, depthwrite, cullmode (material +496..+500)
    std::map<std::pair<const Data::Model::ImageEffect*, size_t>, std::array<uint8_t, 5>> m_materialStates;
    // the native objects' members, their accessors point into these
    std::vector<std::unique_ptr<std::vector<NativeProperty>>> m_nativeMembers;
    v8::Global<v8::Private> m_layerKey;
    // layers and native objects are instances of these, like WE's template objects: their own prototype over
    // Object.prototype with an anonymous constructor (V8 then names them '[object Object]' in its errors)
    v8::Global<v8::Function> m_layerConstructor;
    v8::Global<v8::Function> m_nativeConstructor;
};
}
