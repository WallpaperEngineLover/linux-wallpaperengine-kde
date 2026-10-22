#pragma once

#include <iostream>
#include <map>
#include <memory>
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
    const std::string& geometry ();
    /** Once the sources are translated and linked, only the parsed parameters and textures are still read */
    void releaseSources ();
    [[nodiscard]] const ShaderUnit& getVertex () const;
    [[nodiscard]] const ShaderUnit& getFragment () const;
    [[nodiscard]] const std::map<std::string, int>& getCombos () const;
    [[nodiscard]] ParameterSearchResult findParameter (const std::string& name) const;

private:
    ShaderUnit m_vertex;
    ShaderUnit m_fragment;
    /** GS_ENABLED shaders with a .geom, like WE's particle shaders (its renderer always sets that define) */
    std::unique_ptr<ShaderUnit> m_geometry;
    std::string m_file;
    const ComboMap& m_combos;
    const TextureMap m_passTextures;
};
}
