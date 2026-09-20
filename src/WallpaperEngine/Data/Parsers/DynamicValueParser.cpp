#include "DynamicValueParser.h"

#include <cstdlib>
#include <sstream>

#include "UserSettingParser.h"
#include "WallpaperEngine/Data/Model/DynamicValue.h"

using namespace WallpaperEngine::Data::Parsers;

namespace {
// free-form text is space-separated too, so every token has to be a number to count as a vector
bool isNumericVector (const std::string& str) {
    std::istringstream stream (str);
    std::string token;
    int count = 0;

    while (stream >> token) {
	char* end = nullptr;
	std::strtof (token.c_str (), &end);

	if (end == token.c_str () || *end != '\0') {
	    return false;
	}

	count++;
    }

    return count >= 2;
}

std::shared_ptr<const PropertyAnimation> parseAnimation (const json& data) {
    auto animation = std::make_shared<PropertyAnimation> ();

    const auto readHandle = [] (const json& source, const char* key) {
	AnimationKeyframe::Handle handle;
	const auto it = source.find (key);

	if (it == source.end () || !it->is_object ()) {
	    return handle;
	}

	if (const auto enabled = it->find ("enabled"); enabled != it->end () && enabled->is_boolean ()) {
	    handle.enabled = enabled->get<bool> ();
	}
	if (const auto x = it->find ("x"); x != it->end () && x->is_number ()) {
	    handle.x = x->get<float> ();
	}
	if (const auto y = it->find ("y"); y != it->end () && y->is_number ()) {
	    handle.y = y->get<float> ();
	}

	return handle;
    };

    for (size_t component = 0; component < animation->curves.size (); component++) {
	const auto curve = data.find ("c" + std::to_string (component));

	if (curve == data.end () || !curve->is_array ()) {
	    continue;
	}

	for (const auto& key : *curve) {
	    if (!key.is_object () || !key.contains ("frame") || !key.contains ("value")) {
		continue;
	    }

	    animation->curves[component].push_back (AnimationKeyframe {
		.frame = key["frame"].get<float> (),
		.value = key["value"].get<float> (),
		.back = readHandle (key, "back"),
		.front = readHandle (key, "front"),
	    });
	}
    }

    if (const auto relative = data.find ("relative"); relative != data.end () && relative->is_boolean ()) {
	animation->relative = relative->get<bool> ();
    }

    const auto options = data.find ("options");

    if (options == data.end () || !options->is_object ()) {
	return animation;
    }

    if (const auto it = options->find ("name"); it != options->end () && it->is_string ()) {
	animation->name = it->get<std::string> ();
    }
    if (const auto it = options->find ("fps"); it != options->end () && it->is_number ()) {
	animation->fps = it->get<float> ();
    }
    if (const auto it = options->find ("length"); it != options->end () && it->is_number ()) {
	animation->length = it->get<float> ();
    }
    if (const auto it = options->find ("mode"); it != options->end () && it->is_string ()) {
	const auto mode = it->get<std::string> ();
	animation->mode = mode == "loop"     ? PropertyAnimation::Mode::Loop
			  : mode == "mirror" ? PropertyAnimation::Mode::Mirror
					     : PropertyAnimation::Mode::Single;
    }
    if (const auto it = options->find ("startpaused"); it != options->end () && it->is_boolean ()) {
	animation->startPaused = it->get<bool> ();
    }
    if (const auto it = options->find ("wraploop"); it != options->end () && it->is_boolean ()) {
	animation->wrapLoop = it->get<bool> ();
    }
    if (const auto it = options->find ("events"); it != options->end () && it->is_array ()) {
	for (const auto& event : *it) {
	    if (event.is_object () && event.contains ("frame") && event.contains ("name")) {
		animation->events.push_back (AnimationEvent {
		    .frame = event["frame"].get<float> (),
		    .name = event["name"].get<std::string> (),
		});
	    }
	}
    }
    if (const auto it = options->find ("children"); it != options->end () && it->is_array ()) {
	for (const auto& child : *it) {
	    if (child.is_object () && child.contains ("key") && child["key"].is_string ()) {
		animation->children.push_back (child["key"].get<std::string> ());
	    }
	}
    }
    if (const auto it = options->find ("parent"); it != options->end () && it->is_object ()) {
	if (it->contains ("key") && (*it)["key"].is_string ()) {
	    animation->parent = (*it)["key"].get<std::string> ();
	}
    }

    return animation;
}
}

DynamicValueUniquePtr DynamicValueParser::parse (const json& data, const Properties& properties, bool expectColor) {
    auto value = std::make_unique<DynamicValue> ();
    auto valueIt = data;
    std::optional<std::string> scriptSource = std::nullopt;
    std::optional<json> scriptPropsJson = std::nullopt;

    if (data.is_object ()) {
	const auto user = data.optional ("user");
	const auto script = data.optional ("script");
	valueIt = data.require ("value", "User setting must have a value");

	if (script.has_value () && !script->is_null ()) {
	    scriptSource = script->get<std::string> ();
	    scriptPropsJson = data.optional ("scriptproperties");
	}
    }

    if (valueIt.is_string ()) {
	if (expectColor) {
	    value->update (Builders::ColorBuilder::parse (valueIt), DynamicValue::UpdateSource::Initialization);
	} else {
	    std::string str = valueIt;
	    int size = isNumericVector (str) ? Builders::VectorBuilder::preparseSize (str) : 1;

	    if (size == 1) {
		std::size_t parsed = 0;
		try {
		    float f = std::stof (str, &parsed);

		    if (parsed == str.size ()) {
			value->update (f, DynamicValue::UpdateSource::Initialization);
		    } else {
			value->update (str, DynamicValue::UpdateSource::Initialization);
		    }
		} catch (const std::exception&) {
		    value->update (str, DynamicValue::UpdateSource::Initialization);
		}
	    } else if (size == 2) {
		value->update (static_cast<glm::vec2> (valueIt), DynamicValue::UpdateSource::Initialization);
	    } else if (size == 3) {
		value->update (static_cast<glm::vec3> (valueIt), DynamicValue::UpdateSource::Initialization);
	    } else {
		value->update (static_cast<glm::vec4> (valueIt), DynamicValue::UpdateSource::Initialization);
	    }
	}
    } else if (valueIt.is_number_integer ()) {
	value->update (valueIt.get<int> (), DynamicValue::UpdateSource::Initialization);
    } else if (valueIt.is_number_float ()) {
	value->update (valueIt.get<float> (), DynamicValue::UpdateSource::Initialization);
    } else if (valueIt.is_boolean ()) {
	value->update (valueIt.get<bool> (), DynamicValue::UpdateSource::Initialization);
    } else if (valueIt.is_null ()) {
	value->update (DynamicValue::UpdateSource::Initialization);
    }

    if (data.is_object ()) {
	if (const auto animation = data.find ("animation"); animation != data.end () && animation->is_object ()
	    && animation->contains ("c0")) {
	    value->setAnimation (parseAnimation (*animation));
	}
    }

    if (scriptSource.has_value ()) {
	std::map<std::string, UserSettingUniquePtr> scriptProps;

	if (scriptPropsJson.has_value () && scriptPropsJson->is_object ()) {
	    for (const auto& [key, propData] : scriptPropsJson->items ()) {
		scriptProps[key] = UserSettingParser::parse (propData, properties);
	    }
	}

	value->setProperties (std::move (scriptProps));
	value->setScriptSource (scriptSource.value ());
    }

    return value;
}