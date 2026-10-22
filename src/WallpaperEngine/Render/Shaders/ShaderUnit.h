#pragma once

#include <map>
#include <memory>
#include <set>
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

    void linkToUnit (const ShaderUnit* unit);
    void feedGeometryStage ();
    [[nodiscard]] const ShaderUnit* getLinkedUnit () const;

    [[nodiscard]] const std::string& compile ();

    [[nodiscard]] const std::vector<Variables::ShaderVariable*>& getParameters () const;
    [[nodiscard]] const TextureMap& getTextures () const;
    /** Texture slots whose sampler asks for a TEX<slot>FORMAT combo ("formatcombo") */
    [[nodiscard]] const std::set<int>& getFormatComboSlots () const;
    [[nodiscard]] const std::map<int, std::vector<std::string>>& getComponentCombos () const;
    [[nodiscard]] const ComboMap& getCombos () const;
    [[nodiscard]] const ComboMap& getDiscoveredCombos () const;
    [[nodiscard]] bool refersTo (const std::string& identifier) const;
    /** Frees the source texts once the translation is done, only the identifiers refersTo answers for are kept */
    void releaseSources ();

protected:
    void preprocess ();

private:
    void preprocessVariables ();
    void preprocessIncludes ();
    [[nodiscard]] std::string expandIncludes (const std::string& source, std::set<std::string>& included) const;
    void preprocessRequires ();
    void preprocessBalanceConditionals ();
    void preprocessSwizzledDeclarations ();
    void preprocessScalarSwizzles ();
    [[nodiscard]] std::string resolveRequireModule (const std::string& moduleName) const;
    [[nodiscard]] std::string generateLightingV1 () const;
    /** Adjusts vertex varyings when a workshop shader declares a narrower vertex type than its fragment peer. */
    [[nodiscard]] std::string applyLinkedVaryingCompatibility (std::string source) const;
    /** The opposite case: the vertex shader writes a wider varying than the fragment reads. The input keeps the
     *  vertex width so it links, the fragment works on a narrower global copy filled from it in main(). */
    [[nodiscard]] std::string applyNarrowFragmentVaryingCompatibility (std::string source) const;
    [[nodiscard]] std::string applyFragmentTexCoordCompatibility (std::string source) const;
    // `#define varying in` makes reassigned varyings read-only, those get a writable copy at the top of main()
    [[nodiscard]] std::string applyFragmentVaryingShadowCompatibility (std::string source) const;
    // HLSL truncates wider vectors on narrowing assignment (`vec2 d = someVec4 * someVec2;`), swizzles them down
    [[nodiscard]] std::string applyVectorTruncationCompatibility (std::string source) const;
    // HLSL takes floats as conditions, rewrites `cond` to `(cond != 0.0)`
    [[nodiscard]] std::string applyFloatConditionCompatibility (std::string source) const;
    // HLSL also turns bools into numbers (`barLeft *= isLeftChannel;`), wraps such a bool in the target type
    [[nodiscard]] std::string applyBoolArithmeticCompatibility (std::string source) const;
    // HLSL allows non-constant `const` locals, drops the `const`
    [[nodiscard]] std::string applyNonConstantConstCompatibility (std::string source) const;
    // same for globals: the initializer moves to the top of main() under the declaration's #if branch
    [[nodiscard]] std::string applyNonConstantGlobalConstCompatibility (std::string source) const;
    // `#elif AUDIOSAMPLES == 32;` works in WE, strips the `;`
    [[nodiscard]] std::string applyDirectiveSemicolonCompatibility (std::string source) const;
    // strips HLSL flow control attributes (`[loop]`, `[unroll(4)]`, `[branch]`)
    [[nodiscard]] std::string applyHlslAttributeCompatibility (std::string source) const;
    [[nodiscard]] std::string applyPackedFloatArrayCompatibility (std::string source) const;
    [[nodiscard]] std::string applyGeometryDialect (std::string source) const;
    [[nodiscard]] std::string applyGeometryOutputNames (std::string source) const;
    [[nodiscard]] int defineValue (const std::string& name, const std::string& source) const;

    void parseComboConfiguration (const std::string& content, int defaultValue = 0);
    void parseParameterConfiguration (const std::string& type, const std::string& name, const std::string& content);

    GLSLContext::UnitType m_type;
    std::string m_file;
    std::string m_content;
    std::string m_preprocessed;
    bool m_feedsGeometry { false };
    std::string m_final;
    bool m_released { false };
    std::map<std::string, bool> m_referenced = {};
    /** The unit owns its parameters, passes and uniforms point at them for as long as the shader lives */
    std::vector<std::unique_ptr<Variables::ShaderVariable>> m_ownedParameters = {};
    std::vector<Variables::ShaderVariable*> m_parameters = {};
    const ComboMap& m_combos;
    const ComboMap& m_overrideCombos;
    ComboMap m_discoveredCombos = {};
    std::map<std::string, bool> m_usedCombos = {};
    const ShaderConstantMap& m_constants;
    const TextureMap& m_passTextures;
    const TextureMap& m_overrideTextures;
    TextureMap m_defaultTextures = {};
    std::set<int> m_formatComboSlots = {};
    std::map<int, std::vector<std::string>> m_componentCombos = {};
    const ShaderUnit* m_link;
    const AssetLocator& m_assetLocator;
};
}
