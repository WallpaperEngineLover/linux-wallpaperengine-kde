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
// native vectors as copies on WE's own _VecN prototypes (scripts/jsclasses/baseclasses.js), like scenescript64
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
