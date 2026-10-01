#include <fstream>
#include <string>
#include <utility>

#include <WallpaperEngine/Render/Shaders/Shader.h>
#include <regex>

#include "WallpaperEngine/Render/Shaders/Variables/ShaderVariable.h"

#include "GLSLContext.h"
#include "WallpaperEngine/Assets/AssetLoadException.h"
#include "WallpaperEngine/Assets/AssetLocator.h"
#include "WallpaperEngine/FileSystem/Container.h"

using namespace WallpaperEngine::Assets;

namespace WallpaperEngine::Render::Shaders {
Shader::Shader (
    const AssetLocator& assetLocator, std::string filename, const ComboMap& combos, const ComboMap& overrideCombos,
    const TextureMap& textures, const TextureMap& overrideTextures, const ShaderConstantMap& constants
) :
    m_vertex (
	GLSLContext::UnitType_Vertex, filename, assetLocator.vertexShader (filename), assetLocator, constants, textures,
	overrideTextures, combos, overrideCombos
    ),
    m_fragment (
	GLSLContext::UnitType_Fragment, filename, assetLocator.fragmentShader (filename), assetLocator, constants,
	textures, overrideTextures, combos, overrideCombos
    ),
    m_file (std::move (filename)), m_combos (combos), m_passTextures (textures) {
    this->m_vertex.linkToUnit (&this->m_fragment);
    this->m_fragment.linkToUnit (&this->m_vertex);

    const auto enabled = [] (const ComboMap& map) {
	const auto it = map.find ("GS_ENABLED");
	return it != map.end () && it->second != 0;
    };
    if (!enabled (overrideCombos) && !enabled (combos)) {
	return;
    }

    try {
	this->m_geometry = std::make_unique<ShaderUnit> (
	    GLSLContext::UnitType_Geometry, this->m_file, assetLocator.geometryShader (this->m_file), assetLocator,
	    constants, textures, overrideTextures, combos, overrideCombos
	);
    } catch (AssetLoadException&) {
	return;
    }
    this->m_geometry->linkToUnit (&this->m_vertex);
    this->m_vertex.feedGeometryStage ();
}

const std::string& Shader::vertex () { return this->m_vertex.compile (); }

const std::string& Shader::fragment () { return this->m_fragment.compile (); }

const std::string& Shader::geometry () {
    static const std::string none;
    return this->m_geometry ? this->m_geometry->compile () : none;
}

const ShaderUnit& Shader::getVertex () const { return this->m_vertex; }

const ShaderUnit& Shader::getFragment () const { return this->m_fragment; }

const std::map<std::string, int>& Shader::getCombos () const { return this->m_combos; }

Shader::ParameterSearchResult Shader::findParameter (const std::string& name) const {
    Variables::ShaderVariable* vertex = nullptr;
    Variables::ShaderVariable* fragment = nullptr;

    for (const auto& cur : this->m_vertex.getParameters ()) {
	if (cur->getIdentifierName () == name) {
	    vertex = cur;
	    break;
	}
    }

    for (const auto& cur : this->m_fragment.getParameters ()) {
	if (cur->getIdentifierName () == name) {
	    fragment = cur;
	    break;
	}
    }

    return {
	.vertex = vertex,
	.fragment = fragment,
    };
}
} // namespace WallpaperEngine::Render::Shaders
