#pragma once

#include "WallpaperEngine/Data/Model/Types.h"

#include <glm/glm.hpp>
#include <v8-local-handle.h>
#include <v8-persistent-handle.h>

namespace WallpaperEngine::Data::Model {
class DynamicValue;
}

namespace WallpaperEngine::Scripting {
class ScriptEngine;
}

namespace WallpaperEngine::Scripting::Adapters {
/**
 * Hands native vectors to scripts as instances of WE's own Vec2/Vec3/Vec4 classes
 * (assets scripts/jsclasses/baseclasses.js), the same way scenescript64 does it: a plain
 * object on the _VecN prototype with x/y/z/w copied in. They're always copies.
 */
template <int components> class VectorAdapter {
public:
    explicit VectorAdapter (ScriptEngine& engine);

    v8::Local<v8::Object> instantiate (Data::Model::DynamicValue& value) const;
    v8::Local<v8::Object> create (const glm::vec<components, float>& value) const;

private:
    ScriptEngine& m_engine;
    // baseclasses.js publishes the prototypes as globalThis._Vec2/_Vec3/_Vec4
    mutable v8::Global<v8::Value> m_prototype;
};

extern template class VectorAdapter<2>;
extern template class VectorAdapter<3>;
extern template class VectorAdapter<4>;
}
