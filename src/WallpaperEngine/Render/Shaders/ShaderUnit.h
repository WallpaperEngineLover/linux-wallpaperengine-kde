#pragma once

#include <map>
#include <memory>
#include <string>

#include "GLSLContext.h"
#include "WallpaperEngine/Assets/AssetLocator.h"
#include "WallpaperEngine/Data/JSON.h"
#include "WallpaperEngine/Render/Shaders/Variables/ShaderVariable.h"
#include "nlohmann/json.hpp"

#include "WallpaperEngine/Data/Model/Types.h"

namespace WallpaperEngine::Render::Shaders {
using JSON = WallpaperEngine::Data::JSON::JSON;
using namespace WallpaperEngine::Assets;
using namespace WallpaperEngine::Data::Model;

class ShaderUnit {
public:
    ShaderUnit (
	const GLSLContext::UnitType type, std::string file, std::string content, const AssetLocator& assetLocator,
	const ShaderConstantMap& constants, const TextureMap& passTextures, const TextureMap& overrideTextures,
	const ComboMap& combos, const ComboMap& overrideCombos
    );
    ~ShaderUnit () = default;

    /** Links this shader unit with another unit so they're treated as one */
    void linkToUnit (const ShaderUnit* unit);
    [[nodiscard]] const ShaderUnit* getLinkedUnit () const;

    [[nodiscard]] const std::string& compile ();

    [[nodiscard]] const std::vector<Variables::ShaderVariable*>& getParameters () const;
    [[nodiscard]] const TextureMap& getTextures () const;
    [[nodiscard]] const ComboMap& getCombos () const;
    /** Combos discovered during preprocessing that weren't in the configured combo list */
    [[nodiscard]] const ComboMap& getDiscoveredCombos () const;

protected:
    void preprocess ();

private:
    void preprocessVariables ();
    void preprocessIncludes ();
    void preprocessRequires ();
    /**
     * Some workshop shaders ship with unbalanced #if/#endif blocks (usually a stray extra #endif).
     * Comments out any #endif without a matching #if/#ifdef/#ifndef so preprocessing doesn't fail outright.
     */
    void preprocessBalanceConditionals ();
    /** Resolves a #require module name (e.g. "LightingV1") to generated GLSL code, or "" if unknown */
    [[nodiscard]] std::string resolveRequireModule (const std::string& moduleName) const;
    /** Generates the LightingV1 module stub (PerformLighting_V1 function) */
    [[nodiscard]] std::string generateLightingV1 () const;
    /** Adjusts vertex varyings when a workshop shader declares a narrower vertex type than its fragment peer. */
    [[nodiscard]] std::string applyLinkedVaryingCompatibility (std::string source) const;
    /** Adjusts fragment shaders that use wide texture coordinates as vec2 values in Wallpaper Engine effects. */
    [[nodiscard]] std::string applyFragmentTexCoordCompatibility (std::string source) const;

    void parseComboConfiguration (const std::string& content, int defaultValue = 0);
    void parseParameterConfiguration (const std::string& type, const std::string& name, const std::string& content);

    GLSLContext::UnitType m_type;
    std::string m_file;
    std::string m_content;
    std::string m_includes;
    std::string m_preprocessed;
    std::string m_final;
    std::vector<Variables::ShaderVariable*> m_parameters = {};
    const ComboMap& m_combos;
    const ComboMap& m_overrideCombos;
    /** Combos found during preprocessing that weren't already in m_combos */
    ComboMap m_discoveredCombos = {};
    std::map<std::string, bool> m_usedCombos = {};
    const ShaderConstantMap& m_constants;
    /** The textures that are already applied to this shader */
    const TextureMap& m_passTextures;
    /** The textures that are being overridden */
    const TextureMap& m_overrideTextures;
    /** The default textures to use when a texture is not applied in a given slot */
    TextureMap m_defaultTextures = {};
    const ShaderUnit* m_link;
    const AssetLocator& m_assetLocator;
};
}
