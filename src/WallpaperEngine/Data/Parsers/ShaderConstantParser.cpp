#include "ShaderConstantParser.h"

#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Parsers/UserSettingParser.h"

using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Data::Model;

ShaderConstantMap ShaderConstantParser::parse (const JSON& it, const Project& project) {
    if (!it.is_object ()) {
	return {};
    }

    ShaderConstantMap result = {};

    for (const auto& cur : it.items ()) {
	auto setting = UserSettingParser::parse (cur.value (), project.properties);

	// no value and no property to take it from: the material's default stays
	if (setting->value->getType () == DynamicValue::UnderlyingType::Null && setting->property == nullptr) {
	    continue;
	}

	result.emplace (cur.key (), std::move (setting));
    }

    return result;
}
