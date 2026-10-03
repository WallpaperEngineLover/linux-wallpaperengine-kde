#include "VectorAdapter.h"

#include "../JS.h"
#include "../ScriptEngine.h"

using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::Scripting::Adapters;

template <int components> VectorAdapter<components>::VectorAdapter (ScriptEngine& engine) : m_engine (engine) { }

template <int components>
v8::Local<v8::Object> VectorAdapter<components>::create (const glm::vec<components, float>& value) const {
    static constexpr const char* names[] = { "x", "y", "z", "w" };

    auto* isolate = this->m_engine.getIsolate ();
    const auto context = this->m_engine.getContext ();

    if (this->m_prototype.IsEmpty ()) {
	this->m_prototype.Reset (
	    isolate, JS::get (context, this->m_engine.getGlobalThis (), "_Vec" + std::to_string (components))
	);
    }

    const v8::Local<v8::Object> result = v8::Object::New (isolate);

    if (const auto prototype = this->m_prototype.Get (isolate); prototype->IsObject ()) {
	JS::setPrototype (context, result, prototype);
    }

    for (int i = 0; i < components; i++) {
	JS::define (context, result, names[i], v8::Number::New (isolate, value[i]));
    }

    return result;
}

template <int components> v8::Local<v8::Object> VectorAdapter<components>::instantiate (DynamicValue& value) const {
    if constexpr (components == 2) {
	return this->create (value.getVec2 ());
    } else if constexpr (components == 3) {
	return this->create (value.getVec3 ());
    } else {
	return this->create (value.getVec4 ());
    }
}

namespace WallpaperEngine::Scripting::Adapters {
template class VectorAdapter<2>;
template class VectorAdapter<3>;
template class VectorAdapter<4>;
}
