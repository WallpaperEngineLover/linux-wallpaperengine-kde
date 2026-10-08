#include "EffectParser.h"
#include "MaterialParser.h"

#include <algorithm>
#include <climits>
#include <cmath>
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

namespace {
// WE's int check (sub_1400886E0): fits an int32 without a fraction
std::optional<int> jsonInt (const JSON& value) {
    if (value.is_number_unsigned ()) {
	const auto number = value.get<uint64_t> ();
	return number <= INT_MAX ? std::optional (static_cast<int> (number)) : std::nullopt;
    }

    if (value.is_number_integer ()) {
	const auto number = value.get<int64_t> ();
	return number >= INT_MIN && number <= INT_MAX ? std::optional (static_cast<int> (number)) : std::nullopt;
    }

    if (value.is_number_float ()) {
	const auto number = value.get<double> ();

	if (number >= INT_MIN && number <= INT_MAX && std::trunc (number) == number) {
	    return static_cast<int> (number);
	}
    }

    return std::nullopt;
}

// WE's asInt (sub_140085EE0)
int jsonAsInt (const JSON& value) {
    if (value.is_number_integer ()) {
	return static_cast<int> (value.get<int64_t> ());
    }

    if (value.is_number_float ()) {
	return static_cast<int> (value.get<double> ());
    }

    return 0;
}

// 16 bit, only used up to 4096 (sub_1401EA500)
std::optional<uint32_t> fboSize (const JSON& it, const char* key) {
    const auto found = it.find (key);

    if (found == it.end ()) {
	return std::nullopt;
    }

    const auto number = jsonInt (*found);

    if (!number.has_value () || static_cast<uint16_t> (*number) > 0x1000) {
	return std::nullopt;
    }

    return static_cast<uint16_t> (*number);
}
} // namespace

// sub_1401E63B0: each member names a combo, a number must equal it, an object compares "value" by "op"
EffectConditions EffectParser::parseConditions (const JSON& it) {
    EffectConditions result;

    if (!it.is_array ()) {
	return result;
    }

    for (const auto& entry : it) {
	if (!entry.is_object ()) {
	    continue;
	}

	for (const auto& [combo, test] : entry.items ()) {
	    if (test.is_number ()) {
		result.tests.push_back ({ .combo = combo, .value = jsonAsInt (test) });
		continue;
	    }

	    if (!test.is_object ()) {
		continue;
	    }

	    EffectCondition condition { .combo = combo };

	    if (const auto value = test.find ("value"); value != test.end ()) {
		condition.value = jsonAsInt (*value);
	    }

	    if (const auto op = test.find ("op"); op != test.end () && op->is_string ()) {
		const auto name = op->get<std::string> ();

		if (name == "ge") {
		    condition.op = EffectCondition::GreaterEqual;
		} else if (name == "gt") {
		    condition.op = EffectCondition::Greater;
		} else if (name == "le") {
		    condition.op = EffectCondition::LessEqual;
		} else if (name == "lt") {
		    condition.op = EffectCondition::Less;
		}
	    }

	    result.tests.push_back (std::move (condition));
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
	const auto conditions = cur.optional ("conditions");

	result.push_back (
	    std::make_unique<EffectPass> (EffectPass {
		.conditions = conditions.has_value () ? parseConditions (*conditions) : EffectConditions {},
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
		.compose = cur.optional<bool> ("compose").value_or (false),
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
	const auto name = cur.find ("name");
	const auto format = cur.find ("format");

	if (name == cur.end () || !name->is_string () || format == cur.end () || !format->is_string ()) {
	    continue;
	}

	// stored in a byte, a non-int is 1
	int scale = 1;

	if (const auto scaleIt = cur.find ("scale"); scaleIt != cur.end ()) {
	    scale = jsonInt (*scaleIt).value_or (1);
	}

	const auto uvs = cur.find ("uvs");
	const auto conditions = cur.find ("conditions");

	auto fbo = std::make_unique<FBO> (FBO {
	    .name = name->get<std::string> (),
	    .format = format->get<std::string> (),
	    .scale = static_cast<float> (static_cast<uint8_t> (scale)),
	    .unique = cur.optional ("unique", false),
	    .width = fboSize (cur, "width"),
	    .height = fboSize (cur, "height"),
	    .fit = fboSize (cur, "fit"),
	    .repeat = uvs != cur.end () && uvs->is_string () && uvs->get<std::string> () == "repeat",
	    .conditions = conditions != cur.end () ? parseConditions (*conditions) : EffectConditions {},
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