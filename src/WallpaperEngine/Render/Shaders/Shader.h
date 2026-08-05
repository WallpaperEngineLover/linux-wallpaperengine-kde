#pragma once

#include <iostream>
#include <map>
#include <vector>

#include "../TextureProvider.h"
#include "WallpaperEngine/Render/Shaders/Variables/ShaderVariable.h"

#include "GLSLContext.h"
#include "ShaderUnit.h"

#include "WallpaperEngine/Data/Model/Types.h"

namespace WallpaperEngine::Render::Shaders {
using json = nlohmann::json;
using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Data::Model;

/**
 * A basic shader loader that adds basic function definitions to every loaded shader
 */
class Shader {
public:
    struct ParameterSearchResult {
	Variables::ShaderVariable* vertex;
	Variables::ShaderVariable* fragment;
    };
    Shader (
	const AssetLocator& assetLocator, std::string filename, const ComboMap& combos, const ComboMap& overrideCombos,
	const TextureMap& textures, const TextureMap& overrideTextures, const ShaderConstantMap& constants
    );
    const std::string& vertex ();
    const std::string& fragment ();
    [[nodiscard]] const ShaderUnit& getVertex () const;
    [[nodiscard]] const ShaderUnit& getFragment () const;
    [[nodiscard]] const std::map<std::string, int>& getCombos () const;
    /** Searches both the vertex and fragment shader units for a parameter with this name */
    [[nodiscard]] ParameterSearchResult findParameter (const std::string& name) const;

private:
    ShaderUnit m_vertex;
    ShaderUnit m_fragment;
    std::string m_file;
    std::vector<Variables::ShaderVariable*> m_parameters = {};
    const ComboMap& m_combos;
    const TextureMap m_passTextures;
};
} // namespace WallpaperEngine::Render::Shaders
