#include "MaterialParser.h"

#include <ranges>

#include "ComboValue.h"
#include "TextureParser.h"
#include "WallpaperEngine/Data/Model/Material.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Parsers/ShaderConstantParser.h"

using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Data::Model;

MaterialUniquePtr MaterialParser::load (const Project& project, const std::string& filename) {
    JSON materialJson;

    // WE (sub_1401515B0) logs a material it can't open or parse and builds it from an empty object instead, which
    // ends up as one pass with its "error" shader, a plain copy of g_Texture0: the effect using it changes nothing
    try {
	materialJson = JSON::parseAsset (project.assetLocator->readString (filename));
    } catch (const std::exception& e) {
	sLog.error ("Material ", filename, " error: ", e.what ());
	materialJson = JSON::object ();
    }

    return parse (materialJson, filename, project);
}

std::vector<std::string> MaterialParser::textureNames (const Project& project, const std::string& filename) {
    std::vector<std::string> names;

    try {
	const auto json = JSON::parseAsset (project.assetLocator->readString (filename));
	const auto passes = json.find ("passes");
	const bool hasPasses = passes != json.end () && passes->is_array () && !passes->empty ();
	const auto textures = (hasPasses ? passes->front () : json).optional ("textures");

	if (textures.has_value ()) {
	    for (const auto& texture : TextureParser::parseTextureMap (*textures) | std::views::values) {
		names.push_back (texture);
	    }
	}
    } catch (const std::exception&) { }

    return names;
}

MaterialUniquePtr MaterialParser::parse (const JSON& it, const std::string& filename, const Project& project) {
    const auto passes = it.find ("passes");
    // WE only reads the first entry of "passes", without one the material's root is the pass
    const bool hasPasses = passes != it.end () && passes->is_array () && !passes->empty ();

    std::vector<MaterialPassUniquePtr> result;
    result.push_back (parsePass (hasPasses ? passes->front () : it, project));

    return std::make_unique<Material> (Material {
	.filename = filename,
	.passes = std::move (result),
    });
}

namespace {
// sub_1401587D0 and its siblings (blending, alphawriting, depthtest, depthwrite, cullmode): a member that is there is
// looked up by its text in that property's table, and anything that isn't text (a number, null) or isn't in the table
// takes the table's first entry. Only a member that is missing leaves the material's own default.
std::optional<std::string> modeName (const JSON& pass, const char* key) {
    const auto member = pass.find (key);

    if (member == pass.end ()) {
	return std::nullopt;
    }

    if (member->is_string ()) {
	return member->get<std::string> ();
    }

    sLog.error ("Ignoring '", key, "' (", member->dump (), "): not text");

    return std::string ();
}
}

MaterialPassUniquePtr MaterialParser::parsePass (const JSON& it, const Project& project) {
    const auto textures = it.optional ("textures");
    const auto usertextures = it.optional ("usertextures");
    const auto combos = it.optional ("combos");
    const auto constants = it.optional ("constantshadervalues");
    const auto cullmode = modeName (it, "cullmode");
    const auto depthtest = modeName (it, "depthtest");
    const auto depthwrite = modeName (it, "depthwrite");

    return std::make_unique<MaterialPass> (MaterialPass {
	.blending = parseBlendMode (it.optional<std::string> ("blending", "normal")),
	// left unknown when missing, which draws like nocull/disabled, 3D models pick their own defaults
	.cullmode = cullmode.has_value () ? parseCullMode (*cullmode) : CullingMode_Unknown,
	.depthtest = depthtest.has_value () ? parseDepthtestMode (*depthtest) : DepthtestMode_Unknown,
	.depthwrite = depthwrite.has_value () ? parseDepthwriteMode (*depthwrite) : DepthwriteMode_Unknown,
	// no shader falls back to WE's "error" shader too (sub_140154480)
	.shader = it.optional<std::string> ("shader", "error"),
	.textures = textures.has_value () ? TextureParser::parseTextureMap (*textures) : TextureMap {},
	.usertextures = usertextures.has_value () ? TextureParser::parseTextureMap (*usertextures) : TextureMap {},
	.combos = combos.has_value () ? parseCombos (*combos) : ComboMap {},
	.constants = constants.has_value () ? ShaderConstantParser::parse (*constants, project) : ShaderConstantMap {},
	.alphawriting = it.optional ("alphawriting", std::string ("default")),
    });
}

std::map<std::string, int> MaterialParser::parseCombos (const JSON& it) {
    std::map<std::string, int> result = {};

    if (!it.is_object ()) {
	return result;
    }

    for (const auto& cur : it.items ()) {
	result.emplace (cur.key (), passComboValue (cur.value ()));
    }

    return result;
}

BlendingMode MaterialParser::parseBlendMode (const std::string& mode) {
    if (mode == "normal") {
	return BlendingMode_Normal;
    }

    if (mode == "additive") {
	return BlendingMode_Additive;
    }

    if (mode == "translucent") {
	return BlendingMode_Translucent;
    }

    if (mode == "alphatocoverage") {
	return BlendingMode_AlphaToCoverage;
    }

    sLog.error ("Unknown blending mode: ", mode, " defaulting to normal");
    return BlendingMode_Normal;
}

CullingMode MaterialParser::parseCullMode (const std::string& mode) {
    if (mode == "nocull") {
	return CullingMode_Disable;
    }

    if (mode == "normal") {
	return CullingMode_Normal;
    }

    // the first entry of WE's table
    if (!mode.empty ()) {
	sLog.error ("Unknown culling mode: ", mode, " defaulting to normal");
    }

    return CullingMode_Normal;
}

DepthtestMode MaterialParser::parseDepthtestMode (const std::string& mode) {
    if (mode == "disabled") {
	return DepthtestMode_Disabled;
    }

    if (mode == "enabled") {
	return DepthtestMode_Enabled;
    }

    if (!mode.empty ()) {
	sLog.error ("Unknown depthtest mode: ", mode, " defaulting to disabled");
    }

    return DepthtestMode_Disabled;
}

DepthwriteMode MaterialParser::parseDepthwriteMode (const std::string& mode) {
    if (mode == "disabled") {
	return DepthwriteMode_Disabled;
    }

    if (mode == "enabled") {
	return DepthwriteMode_Enabled;
    }

    if (!mode.empty ()) {
	sLog.error ("Unknown depthwrite mode: ", mode, " defaulting to disabled");
    }

    return DepthwriteMode_Disabled;
}
