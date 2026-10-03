#include "EffectParser.h"
#include "MaterialParser.h"

#include <algorithm>
#include <cstdlib>

#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Data/Model/Effect.h"
#include "WallpaperEngine/Data/Model/Material.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/FileSystem/Container.h"

using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Data::Model;

EffectUniquePtr EffectParser::load (const Project& project, const std::string& filename) {
    const auto effectJson = JSON::parseAsset (project.assetLocator->readString (filename));

    return parse (effectJson, project);
}

EffectUniquePtr EffectParser::parse (const JSON& it, const Project& project) {
    const auto dependencies = it.optional ("dependencies");
    const auto fbos = it.optional ("fbos");
    const auto functions = it.optional ("functions");

    auto result = std::make_unique<Effect> (Effect {
	.name = it.optional<std::string> ("name", ""),
	.description = it.optional<std::string> ("description", ""),
	.group = it.optional<std::string> ("group", ""),
	.preview = it.optional<std::string> ("preview", ""),
	.dependencies = dependencies.has_value () ? parseDependencies (*dependencies) : std::vector<std::string> {},
	.passes = parseEffectPasses (it.require ("passes", "Effect file must have passes"), project),
	.fbos = fbos.has_value () ? parseFBOs (*fbos) : std::vector<FBOUniquePtr> {},
    });

    if (functions.has_value ()) {
	result->functions = parseFunctions (*functions, result->fbos);
    }

    return result;
}

// wallpaper64.exe 2.8.42 sub_1401E7170: an object member per function, only "action": "clear" with an "fbos" array
// is kept, and only when one of its names is one of the effect's buffers
std::vector<EffectFunction> EffectParser::parseFunctions (const JSON& it, const std::vector<FBOUniquePtr>& fbos) {
    std::vector<EffectFunction> result = {};

    if (!it.is_object ()) {
	return result;
    }

    for (const auto& [name, function] : it.items ()) {
	if (name.empty () || !function.is_object ()) {
	    continue;
	}

	const auto action = function.find ("action");
	const auto names = function.find ("fbos");

	if (action == function.end () || !action->is_string () || action->get<std::string> () != "clear"
	    || names == function.end () || !names->is_array ()) {
	    continue;
	}

	EffectFunction entry { .name = name };

	for (const auto& fboName : *names) {
	    if (!fboName.is_string ()) {
		continue;
	    }

	    const auto found = std::ranges::find_if (fbos, [&fboName] (const auto& fbo) {
		return fbo->name == fboName.get<std::string> ();
	    });

	    if (found != fbos.end ()) {
		entry.fbos.push_back (static_cast<int> (found - fbos.begin ()));
	    }
	}

	if (!entry.fbos.empty ()) {
	    result.push_back (std::move (entry));
	}
    }

    return result;
}

std::vector<std::string> EffectParser::parseDependencies (const JSON& it) {
    std::vector<std::string> result = {};

    if (!it.is_array ()) {
	return result;
    }

    for (const auto& cur : it) {
	result.push_back (cur);
    }

    return result;
}

std::vector<EffectPassUniquePtr> EffectParser::parseEffectPasses (const JSON& it, const Project& project) {
    std::vector<EffectPassUniquePtr> result = {};

    if (!it.is_array ()) {
	return result;
    }

    for (const auto& cur : it) {
	const auto binds = cur.optional ("bind");
	const auto command = cur.optional ("command");
	const auto material = cur.optional ("material");

	result.push_back (
	    std::make_unique<EffectPass> (EffectPass {
		.material = material.has_value () ? MaterialParser::load (project, *material)
						  : std::optional<MaterialUniquePtr> {},
		.binds = binds.has_value () ? parseBinds (binds.value ()) : std::map<int, std::string> {},
		.command = command.has_value () ? (command.value () == "copy" ? Command_Copy : Command_Swap)
						: std::optional<PassCommandType> {},
		.source = command.has_value ()
		    ? cur.require<std::string> ("source", "Effect command must have a source")
		    : cur.optional<std::string> ("source"),
		.target = command.has_value ()
		    ? cur.require<std::string> ("target", "Effect command must have a target")
		    : cur.optional<std::string> ("target"),
	    })
	);
    }

    return result;
}

std::map<int, std::string> EffectParser::parseBinds (const JSON& it) {
    std::map<int, std::string> result = {};

    if (!it.is_array ()) {
	return result;
    }

    for (const auto& cur : it) {
	result.emplace (
	    cur.require ("index", "Texture binds must have an index"),
	    cur.require ("name", "Texture bind must name the FBO that should be used")
	);
    }

    return result;
}

std::vector<FBOUniquePtr> EffectParser::parseFBOs (const JSON& it) {
    std::vector<FBOUniquePtr> result = {};

    if (!it.is_array ()) {
	return result;
    }

    for (const auto& cur : it) {
	auto fbo = std::make_unique<FBO> (FBO {
	    .name = cur.require<std::string> ("name", "FBO must have a name"),
	    .format = cur.optional<std::string> ("format", "rgba8888"),
	    .scale = cur.optional ("scale", 1.0f),
	    .unique = cur.optional ("unique", false),
	});

	// sub_1401E7170 reads "clear" with atof, one number per run of spaces; an empty string or all four numbers
	// also clear the buffer when it is made
	if (const auto clear = cur.find ("clear"); clear != cur.end () && clear->is_string ()) {
	    const auto text = clear->get<std::string> ();
	    const char* cursor = text.c_str ();
	    bool complete = text.empty ();

	    for (int component = 0; !complete && component < 4; component++) {
		fbo->clearColor[component] = static_cast<float> (std::atof (cursor));

		if (component == 3) {
		    complete = true;
		    break;
		}

		while (*cursor != '\0' && *cursor != ' ') {
		    cursor++;
		}

		if (*cursor == '\0') {
		    break;
		}

		while (*cursor == ' ') {
		    cursor++;
		}
	    }

	    fbo->clearOnCreate = complete;
	}

	result.push_back (std::move (fbo));
    }

    return result;
}