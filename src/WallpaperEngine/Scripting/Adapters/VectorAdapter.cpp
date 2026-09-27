#include "VectorAdapter.h"

#include "../ScriptEngine.h"

using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::Scripting::Adapters;

template <int components>
VectorAdapter<components>::VectorAdapter (ScriptEngine& engine) :
    ObjectAdapter (engine), m_prototypeName ("_Vec" + std::to_string (components)) { }

template <int components> JSValue VectorAdapter<components>::create (const glm::vec<components, float>& value) const {
    static constexpr const char* names[] = { "x", "y", "z", "w" };

    JSContext* ctx = this->m_engine.getContext ();
    JSValue prototype = JS_GetPropertyStr (ctx, this->m_engine.getGlobalThis (), this->m_prototypeName.c_str ());
    JSValue result = JS_IsObject (prototype) ? JS_NewObjectProto (ctx, prototype) : JS_NewObject (ctx);

    JS_FreeValue (ctx, prototype);

    for (int i = 0; i < components; i++) {
	JS_SetPropertyStr (ctx, result, names[i], JS_NewFloat64 (ctx, value[i]));
    }

    return result;
}

template <int components> JSValue VectorAdapter<components>::instantiate (ScriptableObject& object) {
    throw std::runtime_error ("Cannot create a vector instance from a ScriptableObject");
}

template <int components> JSValue VectorAdapter<components>::instantiate (DynamicValue& value) {
    if constexpr (components == 2) {
	return this->create (value.getVec2 ());
    } else if constexpr (components == 3) {
	return this->create (value.getVec3 ());
    } else {
	return this->create (value.getVec4 ());
    }
}

template <int components> JSValue VectorAdapter<components>::instantiate (DynamicValue& source, bool temporal) {
    return this->instantiate (source);
}

template <int components> JSValue VectorAdapter<components>::instantiate () {
    return this->create (glm::vec<components, float> (0.0f));
}

namespace WallpaperEngine::Scripting::Adapters {
template class VectorAdapter<2>;
template class VectorAdapter<3>;
template class VectorAdapter<4>;
}
