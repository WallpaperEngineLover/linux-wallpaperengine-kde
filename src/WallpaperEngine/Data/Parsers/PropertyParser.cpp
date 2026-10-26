#include "PropertyParser.h"
#include "../Model/Property.h"

#include <filesystem>

using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Data::Model;

PropertySharedPtr PropertyParser::parse (const JSON& it, const std::string& name) {
    const auto type = it.optional ("type");

    if (type == "color") {
	return parseColor (it, name);
    }
    if (type == "bool") {
	return parseBoolean (it, name);
    }
    if (type == "slider") {
	return parseSlider (it, name);
    }
    if (type == "combo") {
	return parseCombo (it, name);
    }
    // "txt", "test" and "label" are what some workshop items call their UI-only description lines
    if (type == "text" || type == "txt" || type == "test" || type == "label") {
	return parseText (it, name);
    }
    if (type == "scenetexture") {
	return parseSceneTexture (it, name);
    }
    if (type == "file" || type == "directory") {
	return parseFile (it, name);
    }
    if (type == "textinput") {
	return parseTextInput (it, name);
    }
    if (type == "usershortcut") {
	return parseUserShortcut (it, name);
    }

    if (type.has_value () && type != "group") {
	sLog.error ("Unexpected type for property: ", type);
	sLog.error (it.dump ());
    }

    return nullptr;
}

PropertySharedPtr PropertyParser::parseCombo (const JSON& it, const std::string& name) {
    std::map<std::string, std::string> optionsMap = {};

    const auto options = it.require ("options", "Combo property must have options");

    if (!options.is_array ()) {
	sLog.exception ("Property combo options should be an array");
    }

    for (auto& cur : options) {
	if (!cur.is_object ()) {
	    continue;
	}

	const auto value = cur.require ("value", "Combo option must have a value");

	optionsMap.emplace (
	    value.is_number () ? std::to_string (value.get<int> ()) : value.get<std::string> (),
	    cur.require ("label", "Combo option must have a label")
	);
    }

    const auto value = it.require ("value", "Combo property must have a value");

    return std::make_shared<PropertyCombo> (
	PropertyData {
	    .name = name,
	    .text = it.optional<std::string> ("text", ""),
	},
	ComboData { .values = optionsMap },
	value.is_number () ? std::to_string (value.get<int> ()) : value.get<std::string> ()
    );
}

PropertySharedPtr PropertyParser::parseColor (const JSON& it, const std::string& name) {
    const auto value = it.require ("value", "Property must have a value");

    if (!value.is_string ()) {
	sLog.error ("Color property ", name, " has a value that is not text (", value.dump (), "), using white");
    }

    return std::make_shared<PropertyColor> (
	PropertyData {
	    .name = name,
	    .text = it.optional<std::string> ("text", ""),
	},
	value.is_string () ? value.get<std::string> () : std::string ("1 1 1")
    );
}

PropertySharedPtr PropertyParser::parseBoolean (const JSON& it, const std::string& name) {
    return std::make_shared<PropertyBoolean> (
	PropertyData {
	    .name = name,
	    .text = it.optional<std::string> ("text", ""),
	},
	it.optional ("value", false)
    );
}

PropertySharedPtr PropertyParser::parseSlider (const JSON& it, const std::string& name) {
    const auto value = it.optional ("value");

    return std::make_shared<PropertySlider> (
	PropertyData {
	    .name = name,
	    .text = it.optional<std::string> ("text", ""),
	},
	SliderData {
	    .min = it.optional ("min", 0.0f),
	    .max = it.optional ("max", 0.0f),
	    .step = it.optional ("step", 0.0f),
	},
	value.has_value () && !value->is_null () ? std::optional<float> (value->get<float> ()) : std::nullopt
    );
}

namespace {
template <typename Kind> PropertySharedPtr makeTextOnly (const JSON& it, const std::string& name) {
    return std::make_shared<Kind> (PropertyData {
	.name = name,
	.text = it.optional<std::string> ("text", ""),
    });
}
}

PropertySharedPtr PropertyParser::parseText (const JSON& it, const std::string& name) {
    return makeTextOnly<PropertyText> (it, name);
}

PropertySharedPtr PropertyParser::parseSceneTexture (const JSON& it, const std::string& name) {
    return std::make_shared<PropertySceneTexture> (
	PropertyData {
	    .name = name,
	    .text = it.optional<std::string> ("text", ""),
	},
	it.require ("value", "Property must have a value")
    );
}

PropertySharedPtr PropertyParser::parseFile (const JSON& it, const std::string& name) {
    auto value = it.optional<std::string> ("value", "");

    // a default can't name a place on the user's disk, only the user does ("/" would open the whole filesystem to a
    // web wallpaper's page)
    if (std::filesystem::path (value).is_absolute ()) {
	value.clear ();
    }

    return std::make_shared<PropertyFile> (
	PropertyData {
	    .name = name,
	    .text = it.optional<std::string> ("text", ""),
	},
	value, it.optional<std::string> ("type", "") == "directory", it.optional<std::string> ("fileType", "")
    );
}

PropertySharedPtr PropertyParser::parseUserShortcut (const JSON& it, const std::string& name) {
    return makeTextOnly<PropertyUserShortcut> (it, name);
}

namespace {
std::string textInputDefault (const std::optional<JSON>& value) {
    if (!value.has_value ()) {
	return "";
    }

    return value->is_string () ? value->get<std::string> () : value->dump ();
}
}

PropertySharedPtr PropertyParser::parseTextInput (const JSON& it, const std::string& name) {
    return std::make_shared<PropertyTextInput> (
	PropertyData {
	    .name = name,
	    .text = it.optional<std::string> ("text", ""),
	},
	textInputDefault (it.optional ("value"))
    );
}
