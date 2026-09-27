#pragma once

#include "ObjectAdapter.h"
#include "WallpaperEngine/Data/Model/Types.h"

#include <glm/glm.hpp>
#include <string>

namespace WallpaperEngine::Scripting::Adapters {
/**
 * Hands native vectors to scripts as instances of WE's own Vec2/Vec3/Vec4 classes
 * (assets scripts/jsclasses/baseclasses.js), the same way scenescript64 does it: a plain
 * object on the _VecN prototype with x/y/z/w copied in. They're always copies.
 */
template <int components> class VectorAdapter : public ObjectAdapter {
public:
    explicit VectorAdapter (ScriptEngine& engine);
    ~VectorAdapter () override = default;

    int length () { return components; }
    JSValue instantiate (Data::Model::DynamicValue& value) override;
    JSValue instantiate (ScriptableObject& object) override;
    JSValue instantiate (Data::Model::DynamicValue& source, bool temporal);
    JSValue instantiate ();

private:
    JSValue create (const glm::vec<components, float>& value) const;

    std::string m_prototypeName;
};

extern template class VectorAdapter<2>;
extern template class VectorAdapter<3>;
extern template class VectorAdapter<4>;
}
