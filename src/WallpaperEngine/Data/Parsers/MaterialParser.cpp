#include "MaterialParser.h"

#include "TextureParser.h"
#include "WallpaperEngine/Data/Model/Material.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Parsers/ShaderConstantParser.h"
#include "WallpaperEngine/FileSystem/Container.h"

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

MaterialUniquePtr MaterialParser::parse (const JSON& it, const std::string& filename, const Project& project) {
    const auto passes = it.find ("passes");

    // without a non-empty "passes" array WE reads the material's root as its pass
    if (passes == it.end () || !passes->is_array () || passes->empty ()) {
	std::vector<MaterialPassUniquePtr> single;
	single.push_back (parsePass (it, project));

	return std::make_unique<Material> (Material {
	    .filename = filename,
	    .passes = std::move (single),
	});
    }

    return std::make_unique<Material> (Material {
	.filename = filename,
	.passes = parsePasses (*passes, project),
    });
}

std::vector<MaterialPassUniquePtr> MaterialParser::parsePasses (const JSON& it, const Project& project) {
    std::vector<MaterialPassUniquePtr> result = {};

    if (!it.is_array ()) {
	return result;
    }

    for (const auto& cur : it) {
	result.push_back (parsePass (cur, project));
    }

    return result;
}

MaterialPassUniquePtr MaterialParser::parsePass (const JSON& it, const Project& project) {
    const auto textures = it.optional ("textures");
    const auto usertextures = it.optional ("usertextures");
    const auto combos = it.optional ("combos");
    const auto constants = it.optional ("constantshadervalues");

    return std::make_unique<MaterialPass> (MaterialPass {
	// TODO: avoid this std::string construction
	.blending = parseBlendMode (it.optional ("blending", std::string ("normal"))),
	// left unknown when missing, which draws like nocull/disabled, 3D models pick their own defaults
	.cullmode = it.contains ("cullmode") ? parseCullMode (it["cullmode"]) : CullingMode_Unknown,
	.depthtest = it.contains ("depthtest") ? parseDepthtestMode (it["depthtest"]) : DepthtestMode_Unknown,
	.depthwrite = it.contains ("depthwrite") ? parseDepthwriteMode (it["depthwrite"]) : DepthwriteMode_Unknown,
	// no shader falls back to WE's "error" shader too (sub_140154480)
	.shader = it.optional<std::string> ("shader", "error"),
	.textures = textures.has_value () ? TextureParser::parseTextureMap (*textures) : TextureMap {},
	.usertextures = usertextures.has_value () ? TextureParser::parseTextureMap (*usertextures) : TextureMap {},
	.combos = combos.has_value () ? parseCombos (*combos) : ComboMap {},
	.constants = constants.has_value () ? ShaderConstantParser::parse (*constants, project) : ShaderConstantMap {},
    });
}

std::map<std::string, int> MaterialParser::parseCombos (const JSON& it) {
    std::map<std::string, int> result = {};

    if (!it.is_object ()) {
	return result;
    }

    for (const auto& cur : it.items ()) {
	result.emplace (cur.key (), cur.value ());
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

    sLog.error ("Unknown culling mode: ", mode, " defaulting to nocull");
    return CullingMode_Disable;
}

DepthtestMode MaterialParser::parseDepthtestMode (const std::string& mode) {
    if (mode == "disabled") {
	return DepthtestMode_Disabled;
    }

    if (mode == "enabled") {
	return DepthtestMode_Enabled;
    }

    sLog.error ("Unknown depthtest mode: ", mode, " defaulting to disabled");
    return DepthtestMode_Disabled;
}

DepthwriteMode MaterialParser::parseDepthwriteMode (const std::string& mode) {
    if (mode == "disabled") {
	return DepthwriteMode_Disabled;
    }

    if (mode == "enabled") {
	return DepthwriteMode_Enabled;
    }

    sLog.error ("Unknown depthwrite mode: ", mode, " defaulting to disabled");
    return DepthwriteMode_Disabled;
}
