#pragma once

#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Data/Utils/TypeCaster.h"
#include <exception>
#include <string>

namespace WallpaperEngine::Render::Shaders::Variables {
using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::Data::Utils;

class ShaderVariable : public DynamicValue, public TypeCaster {
public:
    using DynamicValue::DynamicValue;

    [[nodiscard]] const std::string& getIdentifierName () const;
    [[nodiscard]] const std::string& getName () const;

    void setIdentifierName (std::string identifierName);
    void setName (const std::string& name);
    /** "conversion":"rad2deg", scripts use degrees (sub_1401636A0) */
    [[nodiscard]] bool isInDegrees () const { return this->m_inDegrees; }
    void setInDegrees (bool inDegrees) { this->m_inDegrees = inDegrees; }

private:
    bool m_inDegrees = false;
    std::string m_identifierName;
    std::string m_name;
};
} // namespace WallpaperEngine::Render::Shaders::Variables
