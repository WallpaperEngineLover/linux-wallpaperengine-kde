#include "ObjectParser.h"
#include "DynamicValueParser.h"
#include "EffectParser.h"
#include "MaterialParser.h"
#include "ModelParser.h"

#include "ShaderConstantParser.h"
#include "TextureParser.h"
#include "UserSettingParser.h"
#include "WallpaperEngine/Data/Builders/ColorBuilder.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Maths.h"

#include <cmath>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <sstream>

using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Data::Model;

namespace {
/** For settings whose default depends on the scene (wallpaper64.exe picks 2D or 3D defaults), null if not set */
UserSettingUniquePtr userIfSet (const JSON& it, const std::string& key, const Properties& properties) {
    return it.optional (key).has_value () ? it.user (key, properties) : nullptr;
}

/** wallpaper64.exe reads control point indices unsigned and clamps them to 7 */
int controlPointIndex (const JSON& it, const std::string& key, int fallback) {
    const auto value = it.optional (key);
    if (!value.has_value () || !value->is_number ()) {
	return fallback;
    }
    const auto index = value->get<int64_t> ();
    return index < 0 || index > 7 ? 7 : static_cast<int> (index);
}

/** WE's "x y z" reader: atof, skip to the next space, missing components are 0 */
glm::vec3 parseFloats (const std::string& text) {
    glm::vec3 result (0.0f);
    const char* cursor = text.c_str ();
    for (int i = 0; i < 3; i++) {
	result[i] = static_cast<float> (std::atof (cursor));
	while (*cursor != '\0' && *cursor != ' ') {
	    cursor++;
	}
	while (*cursor == ' ') {
	    cursor++;
	}
    }
    return result;
}

/**
 * A number fills every component, a string is read as up to three numbers split by spaces and the missing ones are
 * 0 (the remap ranges in sub_1401C5490)
 */
glm::vec3 parseRemapRange (const JSON& it, const std::string& key, float fallback) {
    const auto value = it.optional (key);
    if (!value.has_value ()) {
	return glm::vec3 (fallback);
    }
    if (value->is_number ()) {
	return glm::vec3 (value->get<float> ());
    }
    if (!value->is_string ()) {
	return glm::vec3 (fallback);
    }

    return parseFloats (value->get<std::string> ());
}

template <typename T, size_t N>
T parseName (const JSON& it, const std::string& key, const char* fallback, const char* const (&names)[N], T unknown) {
    const std::string name = it.optional<std::string> (key, fallback);
    for (size_t i = 0; i < N; i++) {
	if (name == names[i]) {
	    return static_cast<T> (i);
	}
    }
    sLog.error ("Unknown particle ", key, " ", name);
    return unknown;
}

ParticleRemap parseRemap (const JSON& it, const char* defaultInput) {
    // wallpaper64.exe off_140484E80 and sub_140260FB0, sub_140261030, sub_140261120
    static constexpr const char* values[] = {
	"lifetimefraction",
	"maxlifetime",
	"size",
	"opacity",
	"speed",
	"rotation",
	"angularspeed",
	"distancetocontrolpoint",
	"positionbetweentwocontrolpoints",
	"runtime",
	"timeofday",
	"particlesystemtime",
	"layertime",
	"color",
	"position",
	"velocity",
	"controlpoint",
	"deltatocontrolpoint",
	"directiontocontrolpoint",
	"layerorigin",
    };
    static constexpr const char* operations[] = { "remap", "multiply", "add", "subtract" };
    static constexpr const char* components[] = { "all", "x", "y", "z", "sum", "average", "max", "min" };
    static constexpr const char* transforms[] = { "none",	  "sine",	  "square",	 "saw",
						  "triangle", "simplexnoise", "fbmnoise" };

    return ParticleRemap {
	.operation = parseName (it, "operation", "multiply", operations, ParticleRemapOperation::Unknown),
	.input = parseName (it, "input", defaultInput, values, ParticleRemapValue::Unknown),
	.output = parseName (it, "output", "size", values, ParticleRemapValue::Unknown),
	.inputComponent = parseName (it, "inputcomponent", "all", components, ParticleRemapComponent::Unknown),
	.outputComponent = parseName (it, "outputcomponent", "all", components, ParticleRemapComponent::Unknown),
	.transform = parseName (it, "transformfunction", "none", transforms, ParticleRemapTransform::Unknown),
	.flags = it.optional ("flags", 1u),
	.inputRangeMin = parseRemapRange (it, "inputrangemin", 0.0f),
	.inputRangeMax = parseRemapRange (it, "inputrangemax", 1.0f),
	.outputRangeMin = parseRemapRange (it, "outputrangemin", 0.0f),
	.outputRangeMax = parseRemapRange (it, "outputrangemax", 1.0f),
	.inputControlPoint0 = controlPointIndex (it, "inputcontrolpoint0", 0),
	.inputControlPoint1 = controlPointIndex (it, "inputcontrolpoint1", 1),
	.outputControlPoint0 = controlPointIndex (it, "outputcontrolpoint0", 0),
	.outputControlPoint1 = controlPointIndex (it, "outputcontrolpoint1", 1),
	.transformInputScale = it.optional ("transforminputscale", 2.0f),
	.transformOctaves = it.optional ("transformoctaves", 3),
    };
}

ParticleBlendWindow parseBlendWindow (const JSON& it) {
    return ParticleBlendWindow {
	.inStart = it.optional ("blendinstart", 0.0f),
	.inEnd = it.optional ("blendinend", 0.0f),
	.outStart = it.optional ("blendoutstart", 1.0f),
	.outEnd = it.optional ("blendoutend", 1.0f),
    };
}

/** "r g b" strings with all three numbers, sub_1401C5490 skips anything else */
std::vector<glm::vec3> parseColorList (const JSON& it) {
    std::vector<glm::vec3> colors;
    const auto list = it.optional ("colors");
    if (!list.has_value ()) {
	colors.emplace_back (1.0f);
	return colors;
    }
    if (!list->is_array ()) {
	return colors;
    }

    for (const auto& entry : *list) {
	if (!entry.is_string ()) {
	    continue;
	}
	std::istringstream stream (entry.get<std::string> ());
	glm::vec3 color;
	if (stream >> color.r >> color.g >> color.b) {
	    colors.push_back (color);
	}
    }
    return colors;
}

float numberOr (const JSON& it, const char* key, float fallback) {
    const auto value = it.find (key);
    return value != it.end () && value->is_number () ? value->get<float> () : fallback;
}

std::string stringOr (const JSON& it, const char* key, const char* fallback) {
    const auto value = it.find (key);
    return value != it.end () && value->is_string () ? value->get<std::string> () : fallback;
}

/**
 * wallpaper64.exe sub_1401C45F0: the color a particle file's color initializer centers on. The last colorrandom,
 * hsvcolorrandom or colorlist wins, without one the first colorchange's start value. Saved as "%.5f" and read back
 */
std::pair<glm::vec3, bool> deriveColorReference (const JSON& particle) {
    glm::vec3 reference (1.0f);
    bool hasColor = false;

    const auto nameOf = [] (const JSON& entry) {
	return entry.is_object () ? stringOr (entry, "name", "") : std::string ();
    };

    if (const auto initializers = particle.find ("initializer");
	initializers != particle.end () && initializers->is_array ()) {
	for (const auto& entry : *initializers) {
	    const std::string name = nameOf (entry);

	    if (name == "colorrandom") {
		const glm::vec3 min = parseFloats (stringOr (entry, "min", "0 0 0"));
		const glm::vec3 max = parseFloats (stringOr (entry, "max", "255 255 255"));
		reference = ((max - min) * 0.5f + min) * 0.0039215689f;
		hasColor = true;
	    } else if (name == "hsvcolorrandom") {
		const auto middle = [&entry] (const char* low, float lowDefault, const char* high, float highDefault) {
		    return std::clamp ((numberOr (entry, high, highDefault) + numberOr (entry, low, lowDefault)) * 0.5f, 0.0f, 1.0f);
		};
		reference = WallpaperEngine::Maths::hsvToRgb (glm::vec3 (
		    middle ("huemin", 0.0f, "huemax", 1.0f), middle ("saturationmin", 0.5f, "saturationmax", 1.0f),
		    middle ("valuemin", 0.5f, "valuemax", 1.0f)
		));
		hasColor = true;
	    } else if (name == "colorlist") {
		const auto colors = entry.find ("colors");
		if (colors == entry.end () || !colors->is_array ()) {
		    continue;
		}
		for (const auto& color : *colors) {
		    if (color.is_string ()) {
			reference = parseFloats (color.get<std::string> ());
			hasColor = true;
			break;
		    }
		}
	    }
	}
    }

    if (!hasColor) {
	if (const auto operators = particle.find ("operator"); operators != particle.end () && operators->is_array ()) {
	    for (const auto& entry : *operators) {
		if (nameOf (entry) == "colorchange") {
		    reference = parseFloats (stringOr (entry, "startvalue", "1 1 1"));
		    hasColor = true;
		    break;
		}
	    }
	}
    }

    char text[196];
    std::snprintf (text, sizeof (text), "%.5f %.5f %.5f", reference.r, reference.g, reference.b);
    return { parseFloats (text), hasColor };
}
} // namespace

ObjectUniquePtr ObjectParser::parse (const JSON& it, const Project& project) {
    const auto imageIt = it.find ("image");
    const auto soundIt = it.find ("sound");
    const auto particleIt = it.find ("particle");
    const auto textIt = it.find ("text");
    const auto lightIt = it.find ("light");
    const auto modelIt = it.find ("model");
    // "shape" refers to VolumeLight
    const auto shapeIt = it.find ("shape");
    const auto cameraIt = it.find ("camera");

    // some particle objects have numeric 'name' fields, so handle type mismatches gracefully
    ObjectData basedata;
    try {
	basedata = ObjectData {
	    .id = it.require<int> ("id", "Object must have an id"),
	    .name = it.require<std::string> ("name", "Object must have a name"),
	    .dependencies = parseDependencies (it),
	    .parent = it.optional<int> ("parent"),
	    .attachment = it.optional<std::string> ("attachment"),
	    .sortOrder = it.optional<int> ("sortorder"),
	    .origin = it.user ("origin", project.properties, glm::vec3 (0.0f)),
	    .groupScale = it.user ("scale", project.properties, glm::vec3 (1.0f)),
	    .groupAngles = it.user ("angles", project.properties, glm::vec3 (0.0f)),
	    .groupVisible = it.user ("visible", project.properties, true),
	    .groupParallaxDepth = it.user ("parallaxDepth", project.properties, glm::vec2 (1.0f)),
	    .solid = it.user ("solid", project.properties, true),
	    .disablePropagation = it.user ("disablepropagation", project.properties, false),
	    .perspective = it.user ("perspective", project.properties, false),
	};
    } catch (const std::exception& e) {
	sLog.error ("Error parsing object base data: ", e.what ());
	const auto idIt = it.find ("id");
	const auto nameIt = it.find ("name");
	int id = (idIt != it.end () && idIt->is_number ()) ? idIt->get<int> () : -1;
	std::string name = "unknown";
	if (nameIt != it.end ()) {
	    if (nameIt->is_string ()) {
		name = nameIt->get<std::string> ();
	    } else if (nameIt->is_number ()) {
		name = std::to_string (nameIt->get<int> ());
	    }
	}
	basedata = ObjectData {
	    .id = id,
	    .name = name,
	    .dependencies = parseDependencies (it),
	    .parent = it.optional<int> ("parent"),
	    .attachment = it.optional<std::string> ("attachment"),
	    .sortOrder = it.optional<int> ("sortorder"),
	    .origin = it.user ("origin", project.properties, glm::vec3 (0.0f)),
	    .groupScale = it.user ("scale", project.properties, glm::vec3 (1.0f)),
	    .groupAngles = it.user ("angles", project.properties, glm::vec3 (0.0f)),
	    .groupVisible = it.user ("visible", project.properties, true),
	    .groupParallaxDepth = it.user ("parallaxDepth", project.properties, glm::vec2 (1.0f)),
	    .solid = it.user ("solid", project.properties, true),
	    .disablePropagation = it.user ("disablepropagation", project.properties, false),
	    .perspective = it.user ("perspective", project.properties, false),
	};
    }

    if (imageIt != it.end () && imageIt->is_string ()) {
	return parseImage (it, project, std::move (basedata), *imageIt);
    } else if (soundIt != it.end () && soundIt->is_array ()) {
	return parseSound (it, project, std::move (basedata));
    } else if (particleIt != it.end () && !particleIt->is_null ()) {
	return parseParticle (it, project, std::move (basedata));
    } else if (textIt != it.end () && !textIt->is_null ()) {
	return parseText (it, project, std::move (basedata));
    } else if (lightIt != it.end () && lightIt->is_string ()) {
	return parseLight (it, project, std::move (basedata));
    } else if (modelIt != it.end () && modelIt->is_string ()) {
	return parseMesh (it, std::move (basedata));
    } else if (cameraIt != it.end () && cameraIt->is_string ()) {
	return parseCamera (it, project, std::move (basedata));
    } else if (shapeIt != it.end () && !shapeIt->is_null ()) {
	sLog.error ("VolumeLight objects are not supported yet");
    } else {
	if (!it.optional ("solid", false)) {
	    // TODO: re-evaluate - some objects contain other objects and aren't really anything special
	    sLog.error ("Unknown object type found: ", it.dump ());
	}
    }

    return std::make_unique<Object> (std::move (basedata));
}

namespace {
// the "path" file of a camera object (sub_1401F2030): entries without options or with an invalid length/fps are
// dropped, tracks use the property animation keyframe format
std::vector<CameraTimeline> parseCameraTimelines (const std::string& filename, const Project& project) {
    std::vector<CameraTimeline> timelines;
    JSON file;

    try {
	file = JSON::parseAsset (project.assetLocator->readString (filename));
    } catch (const std::exception& e) {
	sLog.error ("Cannot load camera path file ", filename, ": ", e.what ());
	return timelines;
    }

    const auto list = file.find ("paths");

    if (list == file.end () || !list->is_array ()) {
	return timelines;
    }

    for (const auto& entry : *list) {
	if (!entry.is_object ()) {
	    continue;
	}

	const auto options = entry.find ("options");

	if (options == entry.end () || !options->is_object ()) {
	    continue;
	}

	const auto length = options->find ("length");
	const auto fps = options->find ("fps");

	if (length == options->end () || fps == options->end () || !length->is_number () || !fps->is_number ()
	    || fps->get<float> () <= 0.0f || length->get<int> () <= 0) {
	    continue;
	}

	CameraTimeline timeline;
	timeline.fps = fps->get<float> ();
	timeline.length = static_cast<float> (length->get<int> ());

	if (const auto name = entry.find ("name"); name != entry.end () && name->is_string ()) {
	    timeline.name = name->get<std::string> ();
	}
	if (const auto visible = entry.find ("visible"); visible != entry.end () && visible->is_boolean ()) {
	    timeline.visible = visible->get<bool> ();
	}
	if (const auto mode = options->find ("mode"); mode != options->end () && mode->is_string ()) {
	    const auto name = mode->get<std::string> ();
	    timeline.mode = name == "mirror" ? PropertyAnimation::Mode::Mirror
		: name == "single"           ? PropertyAnimation::Mode::Single
					     : PropertyAnimation::Mode::Loop;
	} else {
	    timeline.mode = PropertyAnimation::Mode::Loop;
	}
	if (const auto paused = options->find ("startpaused"); paused != options->end () && paused->is_boolean ()) {
	    timeline.startPaused = paused->get<bool> ();
	}

	const auto wrap = options->find ("wraploop");
	const bool wrapLoop = wrap != options->end () && wrap->is_boolean () && wrap->get<bool> ();
	const int frames = static_cast<int> (timeline.length);

	const auto readCurve = [&] (const JSON& curve, std::vector<AnimationKeyframe>& keys) {
	    keys = DynamicValueParser::parseKeyframes (curve);

	    if (wrapLoop) {
		DynamicValueParser::closeLoop (keys, frames);
	    }
	};
	const auto readTrack = [&] (const char* key, std::array<std::vector<AnimationKeyframe>, 3>& track) {
	    const auto it = entry.find (key);

	    if (it == entry.end () || !it->is_object ()) {
		return;
	    }

	    for (int component = 0; component < 3; component++) {
		if (const auto curve = it->find ("c" + std::to_string (component)); curve != it->end ()) {
		    readCurve (*curve, track[component]);
		}
	    }
	};

	readTrack ("center", timeline.center);
	readTrack ("eye", timeline.eye);
	readTrack ("up", timeline.up);

	if (const auto zoom = entry.find ("zoom"); zoom != entry.end ()) {
	    readCurve (*zoom, timeline.zoom);
	}
	if (const auto fov = entry.find ("fov"); fov != entry.end ()) {
	    readCurve (*fov, timeline.fov);
	}

	timelines.push_back (std::move (timeline));
    }

    return timelines;
}
} // namespace

SceneCameraUniquePtr ObjectParser::parseCamera (const JSON& it, const Project& project, ObjectData base) {
    // defaults from the camera object's constructor, wallpaper64.exe 2.8.42 sub_14018FF60
    SceneCameraData data {
	.fov = it.user ("fov", project.properties, 50.0f),
	.zoom = it.user ("zoom", project.properties, 1.0f),
	.timelines = {},
	.queueMode = it.optional ("queuemode", std::string ("random")) == "sequential" ? CameraQueueMode::Sequential
										: CameraQueueMode::Random,
    };

    if (const auto path = it.optional<std::string> ("path"); path.has_value ()) {
	data.timelines = parseCameraTimelines (*path, project);
    }

    return std::make_unique<SceneCamera> (std::move (base), std::move (data));
}

LightUniquePtr ObjectParser::parseLight (const JSON& it, const Project& project, ObjectData base) {
    const auto name = it.require<std::string> ("light", "Light must have a type");
    const std::map<std::string, LightType> types = {
	{ "point", LightType::Legacy },	  { "lpoint", LightType::Point },
	{ "lspot", LightType::Spot },	  { "ltube", LightType::Tube },
	{ "ldirectional", LightType::Directional },
    };
    const auto type = types.find (name);

    if (type == types.end ()) {
	sLog.error ("Unknown light type ", name, " on object ", base.id);
    }

    return std::make_unique<Light> (
	std::move (base),
	LightData {
	    .type = type != types.end () ? type->second : LightType::Legacy,
	    .color = it.color ("color", project.properties, Builders::ColorBuilder::White),
	    .intensity = it.user ("intensity", project.properties, 1.0f),
	    .radius = it.user ("radius", project.properties, 1.0f),
	    .visible = it.user ("visible", project.properties, true),
	    // defaults from the light's constructor (sub_14018FF60) and property table (sub_14025DA80)
	    .castVolumetrics = it.optional ("castvolumetrics", false),
	    .castShadow = it.optional ("castshadow", false),
	    .density = it.user ("density", project.properties, 2.0f),
	    .volumetricsExponent = it.user ("volumetricsexponent", project.properties, 1.0f),
	}
    );
}

MeshUniquePtr ObjectParser::parseMesh (const JSON& it, ObjectData base) {
    return std::make_unique<Mesh> (
	std::move (base), MeshData { .model = it.require<std::string> ("model", "Model object must have a model") }
    );
}

std::vector<int> ObjectParser::parseDependencies (const JSON& it) {
    const auto dependenciesIt = it.find ("dependencies");

    if (dependenciesIt == it.end () || !dependenciesIt->is_array ()) {
	return {};
    }

    std::vector<int> result = {};

    for (const auto& cur : *dependenciesIt) {
	result.push_back (cur);
    }

    return result;
}

SoundUniquePtr ObjectParser::parseSound (const JSON& it, const Project& project, ObjectData base) {
    const auto soundIt = it.require ("sound", "Object must have a sound");
    std::vector<std::string> sounds = {};

    for (const auto& cur : soundIt) {
	sounds.push_back (cur);
    }

    return std::make_unique<Sound> (
	std::move (base),
	SoundData {
	    .playbackmode = parsePlaybackMode (it.optional ("playbackmode", std::string ("single"))),
	    .sounds = sounds,
	    .volume = it.user<float> ("volume", project.properties, 1.0f),
	    .startsilent = it.optional<bool> ("startsilent"),
	}
    );
}

SoundPlaybackMode ObjectParser::parsePlaybackMode (const std::string& mode) {
    if (mode == "loop") {
	return PlaybackMode_Loop;
    }

    if (mode == "random") {
	return PlaybackMode_Random;
    }

    return PlaybackMode_Single;
}

uint32_t ObjectParser::parseAlignment (const std::string& alignment) {
    uint32_t result = ImageAlignment_Center;

    if (alignment.find ("top") != std::string::npos) {
	result |= ImageAlignment_Top;
    } else if (alignment.find ("bottom") != std::string::npos) {
	result |= ImageAlignment_Bottom;
    }

    if (alignment.find ("left") != std::string::npos) {
	result |= ImageAlignment_Left;
    } else if (alignment.find ("right") != std::string::npos) {
	result |= ImageAlignment_Right;
    }

    return result;
}

TextUniquePtr ObjectParser::parseText (const JSON& it, const Project& project, ObjectData base) {
    const auto& effects = it.optional ("effects");
    const auto& properties = project.properties;

    // defaults are the text object constructor's in wallpaper64.exe 2.8.42 (sub_140256AE0)
    return std::make_unique<Text> (
	std::move (base),
	TextData {
	    .text = it.user ("text", project.properties),
	    .font = it.optional ("font", std::string ("systemfont_arial")),
	    .pointSize = it.user ("pointsize", project.properties, 32.0f),
	    .size = it.optional ("size", glm::vec2 (0.0f)),
	    .scale = it.user ("scale", project.properties, glm::vec3 (1.0f)),
	    .color = it.color ("color", project.properties, Builders::ColorBuilder::White),
	    .alpha = it.user ("alpha", project.properties, 1.0f),
	    .visible = it.user ("visible", project.properties, true),
	    .parallaxDepth = it.user ("parallaxDepth", project.properties, glm::vec2 (1.0f)),
	    .horizontalAlign = it.user ("horizontalalign", properties, it.optional ("alignment", std::string ("center"))),
	    .verticalAlign = it.user ("verticalalign", properties, std::string ("center")),
	    .anchor = it.user ("anchor", properties, std::string ("none")),
	    .colorBlendMode = it.user ("colorBlendMode", properties, 0),
	    .padding = it.user ("padding", properties, glm::vec2 (32.0f)),
	    .spacing = it.user ("spacing", properties, glm::vec2 (0.0f)),
	    .effects = effects.has_value () ? parseEffects (*effects, project) : std::vector<ImageEffectUniquePtr> {},
	    .limitWidth = it.user ("limitwidth", properties, false),
	    .maxWidth = it.user ("maxwidth", properties, 512.0f),
	    .limitRows = it.user ("limitrows", properties, false),
	    .maxRows = it.user ("maxrows", properties, 1),
	    .limitUseEllipsis = it.user ("limituseellipsis", properties, false),
	    .blockAlign = it.user ("blockalign", properties, false),
	    .opaqueBackground = it.user ("opaquebackground", properties, false),
	    .backgroundColor = it.color ("backgroundcolor", properties, Builders::ColorBuilder::Black),
	    .backgroundBrightness = it.user ("backgroundbrightness", properties, 1.0f),
	    .brightness = it.user ("brightness", properties, 1.0f),
	    .msdf = it.user ("msdf", properties, false),
	    .outline = it.user ("outline", properties, false),
	    .outlineThickness = it.user ("outlinethickness", properties, 4.0f),
	    .outlineColor = it.color ("outlinecolor", properties, Builders::ColorBuilder::Black),
	    .blur = it.user ("blur", properties, false),
	    .blurSize = it.user ("blursize", properties, 6.0f),
	    .dropShadow = it.user ("dropshadow", properties, false),
	    .dropShadowSize = it.user ("dropshadowsize", properties, 6.0f),
	    .dropShadowOpacity = it.user ("dropshadowopacity", properties, 1.0f),
	    .dropShadowOffset = it.user ("dropshadowoffset", properties, glm::vec2 (4.0f)),
	    .dropShadowColor = it.color ("dropshadowcolor", properties, Builders::ColorBuilder::Black),
	}
    );
}

ImageUniquePtr
ObjectParser::parseImage (const JSON& it, const Project& project, ObjectData base, const std::string& image) {
    const auto& properties = project.properties;
    const auto& effects = it.optional ("effects");
    const auto& animationLayers = it.optional ("animationlayers");

    auto result = std::make_unique<Image> (
	std::move (base),
	ImageData {
	    .scale = it.user ("scale", properties, glm::vec3 (1.0f)),
	    .angles = it.user ("angles", properties, glm::vec3 (0.0f)),
	    .visible = it.user ("visible", properties, true),
	    .alpha = it.user ("alpha", properties, 1.0f),
	    .color = it.color ("color", properties, Builders::ColorBuilder::White),
	    .alignment = parseAlignment (
		it.optional ("horizontalalign", it.optional ("alignment", std::string ("center")))
	    ),
	    .size = it.user ("size", properties, glm::vec2 (0.0f))->value->getVec2 (),
	    .parallaxDepth = it.user ("parallaxDepth", properties, glm::vec2 (1.0f)),
	    .colorBlendMode = it.user ("colorBlendMode", properties, 0),
	    .brightness = it.user ("brightness", properties, 1.0f),
	    .clampUVs = it.optional ("clampuvs", false),
	    .copyBackground = it.user ("copybackground", properties, true),
	    .model = ModelParser::load (project, image),
	    .effects = effects.has_value () ? parseEffects (*effects, project) : std::vector<ImageEffectUniquePtr> {},
	    .animationLayers = animationLayers.has_value () ? parseAnimationLayers (*animationLayers, project)
							    : std::vector<ImageAnimationLayerUniquePtr> {},
	}
    );

    const auto instance = it.optional ("instance");

    if (instance.has_value () && instance->is_object () && !result->model->material->passes.empty ()) {
	auto& firstPass = **result->model->material->passes.begin ();
	const auto instanceTextures = instance->optional ("textures");

	if (instanceTextures.has_value ()) {
	    // the instance's textures replace the material's, a solid layer's util/white included
	    for (const auto& [index, texture] : TextureParser::parseTextureMap (*instanceTextures)) {
		firstPass.textures.insert_or_assign (index, texture);
	    }
	}

	const auto instanceUserTextures = instance->optional ("usertextures");

	if (instanceUserTextures.has_value ()) {
	    for (const auto& [index, texture] : TextureParser::parseTextureMap (*instanceUserTextures)) {
		firstPass.usertextures.insert_or_assign (index, texture);
	    }
	}
    }

    return result;
}

std::vector<ImageEffectUniquePtr> ObjectParser::parseEffects (const JSON& it, const Project& project) {
    if (!it.is_array ()) {
	return {};
    }

    std::vector<ImageEffectUniquePtr> result = {};

    for (const auto& cur : it) {
	result.push_back (parseEffect (cur, project));
    }

    return result;
}

ImageEffectUniquePtr ObjectParser::parseEffect (const JSON& it, const Project& project) {
    const auto& passsOverrides = it.optional ("passes");
    return std::make_unique<ImageEffect> (ImageEffect {
	.id = it.optional<int> ("id", -1),
	.name = it.optional<std::string> ("name", "Effect without name"),
	.visible = it.user ("visible", project.properties, true),
	.passOverrides = passsOverrides.has_value () ? parseEffectPassOverrides (passsOverrides.value (), project)
						     : std::vector<ImageEffectPassOverrideUniquePtr> {},
	.effect = EffectParser::load (project, it.require ("file", "Image effect must have an effect")) });
}

std::vector<ImageEffectPassOverrideUniquePtr>
ObjectParser::parseEffectPassOverrides (const JSON& it, const Project& project) {
    if (!it.is_array ()) {
	return {};
    }

    std::vector<ImageEffectPassOverrideUniquePtr> result = {};

    for (const auto& cur : it) {
	result.push_back (parseEffectPass (cur, project));
    }

    return result;
}

ImageEffectPassOverrideUniquePtr ObjectParser::parseEffectPass (const JSON& it, const Project& project) {
    const auto& combos = it.optional ("combos");
    const auto& textures = it.optional ("textures");
    const auto& constants = it.optional ("constantshadervalues");
    const auto& usertextures = it.optional ("usertextures");

    return std::make_unique<ImageEffectPassOverride> (ImageEffectPassOverride {
	.id = it.optional<int> ("id", -1),
	.combos = combos.has_value () ? parseComboMap (combos.value ()) : ComboMap {},
	.constants
	= constants.has_value () ? ShaderConstantParser::parse (constants.value (), project) : ShaderConstantMap {},
	.textures = textures.has_value () ? TextureParser::parseTextureMap (textures.value ()) : TextureMap {},
	.usertextures
	= usertextures.has_value () ? TextureParser::parseTextureMap (usertextures.value ()) : TextureMap {},
    });
}

ComboMap ObjectParser::parseComboMap (const JSON& it) {
    if (!it.is_object ()) {
	return {};
    }

    ComboMap result = {};

    for (const auto& cur : it.items ()) {
	result.emplace (cur.key (), cur.value ());
    }

    return result;
}

std::vector<ImageAnimationLayerUniquePtr> ObjectParser::parseAnimationLayers (const JSON& it, const Project& project) {
    if (!it.is_array ()) {
	return {};
    }

    std::vector<ImageAnimationLayerUniquePtr> result = {};

    for (const auto& cur : it.items ()) {
	result.push_back (parseAnimationLayer (cur.value (), project));
    }

    return result;
}

ImageAnimationLayerUniquePtr ObjectParser::parseAnimationLayer (const JSON& it, const Project& project) {
    const auto& properties = project.properties;

    return std::make_unique<ImageAnimationLayer> (ImageAnimationLayer {
	.id = it.require<int> ("id", "Animation layer must have an id"),
	.name = it.optional<std::string> ("name", ""),
	.rate = it.user ("rate", properties, 1.0f),
	.visible = it.user ("visible", properties, false),
	.blend = it.user ("blend", properties, 1.0f),
	.animation = it.user ("animation", properties, 0),
    });
}

ParticleUniquePtr ObjectParser::parseParticle (const JSON& it, const Project& project, ObjectData base, int depth) {
    try {
	const auto& properties = project.properties;
	const auto particleIt = it.find ("particle");

	if (particleIt == it.end ()) {
	    sLog.error ("Particle object must have a particle definition");
	    return std::make_unique<Particle> (
		std::move (base),
		ParticleData {
		    .scale = it.user ("scale", properties, glm::vec3 (1.0f)),
		    .angles = it.user ("angles", properties, glm::vec3 (0.0f)),
		    .visible = it.user ("visible", properties, true),
		    .parallaxDepth = it.user ("parallaxDepth", properties, glm::vec2 (1.0f)),
		    .particleFile = "",
		    .animationMode = "sequence",
		    .sequenceMultiplier = 1.0f,
		    .maxCount = 100,
		    .startTime = 0.0f,
		    .flags = 0,
		    .material = nullptr,
		    .emitters = {},
		    .initializers = {},
		    .operators = {},
		    .renderers = {},
		    .controlPoints = {},
		    .children = {},
		    .instanceOverride = {
		        .enabled = Builders::UserSettingBuilder::fromValue(false),
			.alpha = Builders::UserSettingBuilder::fromValue(1.0f),
			.size = Builders::UserSettingBuilder::fromValue(1.0f),
			.lifetime = Builders::UserSettingBuilder::fromValue(1.0f),
			.rate = Builders::UserSettingBuilder::fromValue(1.0f),
			.speed = Builders::UserSettingBuilder::fromValue(1.0f),
			.count = Builders::UserSettingBuilder::fromValue(1.0f),
			.color = Builders::UserSettingBuilder::fromValue(1.0f),
			.colorn = Builders::UserSettingBuilder::fromValue(1.0f),
		    },
		}
	    );
	}

	std::string particleFile;
	if (particleIt->is_string ()) {
	    particleFile = particleIt->get<std::string> ();
	}

	JSON particleJson = JSON::object ();
	if (!particleFile.empty ()) {
	    try {
		particleJson
		    = WallpaperEngine::Data::JSON::JSON::parseAsset (project.assetLocator->readString (particleFile));
	    } catch (std::runtime_error& e) {
		sLog.error ("Cannot load particle file: ", particleFile, " - ", e.what ());
	    }
	} else if (particleIt->is_object ()) {
	    particleJson = *particleIt;
	}

	// field is named "emitter" not "emitters"
	std::vector<ParticleEmitter> emitters;
	const auto emittersIt = particleJson.find ("emitter");
	if (emittersIt != particleJson.end () && emittersIt->is_array ()) {
	    for (const auto& emitter : *emittersIt) {
		emitters.push_back (parseParticleEmitter (emitter));
	    }
	}

	// field is named "initializer" not "initializers"
	std::vector<ParticleInitializerUniquePtr> initializers;
	const auto initializersIt = particleJson.find ("initializer");
	if (initializersIt != particleJson.end () && initializersIt->is_array ()) {
	    for (const auto& initializer : *initializersIt) {
		auto init = parseParticleInitializer (initializer, project.properties);
		if (init) {
		    initializers.push_back (std::move (init));
		}
	    }
	}

	// field is named "operator" not "operators"
	std::vector<ParticleOperatorUniquePtr> operators;
	const auto operatorsIt = particleJson.find ("operator");
	if (operatorsIt != particleJson.end () && operatorsIt->is_array ()) {
	    for (const auto& op : *operatorsIt) {
		auto oper = parseParticleOperator (op, project.properties);
		if (oper) {
		    operators.push_back (std::move (oper));
		}
	    }
	}

	// wallpaper64.exe sub_1401C5490: control points a remap component outputs to are left to it
	uint8_t remapOutputControlPoints = 0;
	for (const auto* list : { &initializersIt, &operatorsIt }) {
	    if (*list == particleJson.end () || !(*list)->is_array ()) {
		continue;
	    }
	    for (const auto& component : **list) {
		const auto nameIt = component.find ("name");
		const auto outputIt = component.find ("output");
		if (nameIt == component.end () || !nameIt->is_string () || outputIt == component.end ()
		    || !outputIt->is_string () || outputIt->get<std::string> () != "controlpoint") {
		    continue;
		}
		const std::string name = nameIt->get<std::string> ();
		if (name != "remapvalue" && name != "remapinitialvalue") {
		    continue;
		}
		const auto cpIt = component.find ("outputcontrolpoint0");
		const int64_t cp = cpIt != component.end () && cpIt->is_number () ? cpIt->get<int64_t> () : 0;
		// clamped as unsigned, so negative indices end up on the last slot too
		remapOutputControlPoints |= static_cast<uint8_t> (1u << (cp < 0 || cp > 7 ? 7 : cp));
	    }
	}

	// field is named "renderer" not "renderers"
	std::vector<ParticleRenderer> renderers;
	const auto renderersIt = particleJson.find ("renderer");
	if (renderersIt != particleJson.end () && renderersIt->is_array ()) {
	    for (const auto& renderer : *renderersIt) {
		renderers.push_back (parseParticleRenderer (renderer));
	    }
	}

	if (renderers.empty ()) {
	    renderers.push_back (
		ParticleRenderer {
		    .name = "sprite",
		    .length = 0.05f,
		    .maxLength = 10.0f,
		    .minLength = 0.0f,
		    .subdivision = 1.0f,
		    .segments = 4.0f,
		    .uvScale = 1.0f,
		    .uvScrolling = false,
		    .uvSmoothing = true,
		    .fadeAlpha = false,
		    .fadeSize = false,
		}
	    );
	}

	// field is named "controlpoint" not "controlpoints"
	std::vector<ParticleControlPoint> controlPoints;
	const auto controlPointsIt = particleJson.find ("controlpoint");
	if (controlPointsIt != particleJson.end () && controlPointsIt->is_array ()) {
	    for (const auto& cp : *controlPointsIt) {
		controlPoints.push_back (parseParticleControlPoint (cp));
	    }
	}

	// real WE stops creating child systems 10 levels down, which also ends a file listing itself as a child
	constexpr int maxChildDepth = 10;
	std::vector<ParticleChild> children;
	const auto childrenIt = particleJson.optional ("children");
	if (depth < maxChildDepth && childrenIt.has_value () && childrenIt->is_array ()) {
	    for (const auto& child : *childrenIt) {
		children.push_back (parseParticleChild (child, it, project, base, children.size (), depth + 1));
	    }
	}

	ParticleInstanceOverride instanceOverride = {
	    .enabled = Builders::UserSettingBuilder::fromValue (false),
	    .alpha = Builders::UserSettingBuilder::fromValue (1.0f),
	    .size = Builders::UserSettingBuilder::fromValue (1.0f),
	    .lifetime = Builders::UserSettingBuilder::fromValue (1.0f),
	    .rate = Builders::UserSettingBuilder::fromValue (1.0f),
	    .speed = Builders::UserSettingBuilder::fromValue (1.0f),
	    .count = Builders::UserSettingBuilder::fromValue (1.0f),
	    .color = Builders::UserSettingBuilder::fromValue (1.0f),
	    .colorn = Builders::UserSettingBuilder::fromValue (1.0f),
	};
	const auto instanceOverrideIt = it.optional ("instanceoverride");
	if (instanceOverrideIt.has_value ()) {
	    instanceOverride = parseParticleInstanceOverride (*instanceOverrideIt, project.properties);
	}

	// particles reference material definitions directly, not model files, so wrap it in a model structure
	ModelUniquePtr material = nullptr;
	const auto materialIt = particleJson.find ("material");
	if (materialIt != particleJson.end () && materialIt->is_string ()) {
	    try {
		std::string materialPath = materialIt->get<std::string> ();

		auto mat = MaterialParser::load (project, materialPath);

		material = std::make_unique<ModelStruct> (ModelStruct {
		    .filename = materialPath,
		    .material = std::move (mat),
		    .solidlayer = false,
		    .fullscreen = false,
		    .passthrough = false,
		    .autosize = false,
		    .projectlayer = false,
		    .nopadding = false,
		    .width = std::nullopt,
		    .height = std::nullopt,
		    .puppet = std::nullopt,
		});
	    } catch (std::runtime_error& e) {
		sLog.error ("Cannot load particle material: ", materialIt->get<std::string> (), " - ", e.what ());
	    }
	}

	std::string animationMode = "sequence";
	const auto animModeIt = particleJson.find ("animationmode");
	if (animModeIt != particleJson.end () && animModeIt->is_string ()) {
	    animationMode = animModeIt->get<std::string> ();
	}

	float sequenceMultiplier = 1.0f;
	uint32_t maxCount = 100;
	float startTime = 0.0f;
	uint32_t flags = 0;

	const auto seqMultIt = particleJson.find ("sequencemultiplier");
	if (seqMultIt != particleJson.end () && seqMultIt->is_number ()) {
	    sequenceMultiplier = seqMultIt->get<float> ();
	}

	const auto maxCountIt = particleJson.find ("maxcount");
	if (maxCountIt != particleJson.end () && maxCountIt->is_number ()) {
	    maxCount = maxCountIt->get<uint32_t> ();
	}

	const auto startTimeIt = particleJson.find ("starttime");
	if (startTimeIt != particleJson.end () && startTimeIt->is_number ()) {
	    startTime = startTimeIt->get<float> ();
	}

	const auto [colorReference, hasColor] = deriveColorReference (particleJson);

	const auto flagsIt = particleJson.find ("flags");
	if (flagsIt != particleJson.end () && flagsIt->is_number ()) {
	    flags = flagsIt->get<uint32_t> ();
	}

	return std::make_unique<Particle> (
	    std::move (base),
	    ParticleData {
		.scale = it.user ("scale", properties, glm::vec3 (1.0f)),
		.angles = it.user ("angles", properties, glm::vec3 (0.0f)),
		.visible = it.user ("visible", properties, true),
		.parallaxDepth = it.user ("parallaxDepth", properties, glm::vec2 (1.0f)),
		.particleFile = particleFile,
		.animationMode = animationMode,
		.sequenceMultiplier = sequenceMultiplier,
		.maxCount = maxCount,
		.startTime = startTime,
		.flags = flags,
		.material = std::move (material),
		.emitters = std::move (emitters),
		.initializers = std::move (initializers),
		.operators = std::move (operators),
		.renderers = std::move (renderers),
		.controlPoints = std::move (controlPoints),
		.children = std::move (children),
		.remapOutputControlPoints = remapOutputControlPoints,
		.colorReference = colorReference,
		.hasColor = hasColor,
		.overrideColorTints = !hasColor || project.sceneVersion < 5,
		.instanceOverride = std::move (instanceOverride),
	    }
	);
    } catch (nlohmann::json::exception& e) {
	sLog.error ("Error parsing particle '", base.name, "': ", e.what ());
	sLog.error ("Particle JSON: ", it.dump ());
	throw;
    }
}

ParticleEmitter ObjectParser::parseParticleEmitter (const JSON& it) {
    std::string name;
    const auto nameIt = it.find ("name");
    if (nameIt != it.end () && nameIt->is_string ()) {
	name = nameIt->get<std::string> ();
    }

    // vec3 fields may show up as strings, arrays, single numbers, or be missing entirely
    auto parseVec3 = [&] (const char* fieldName, const glm::vec3& defaultValue) -> glm::vec3 {
	const auto fieldIt = it.find (fieldName);
	if (fieldIt == it.end ()) {
	    return defaultValue;
	}
	if (fieldIt->is_string ()) {
	    return it.optional (fieldName, defaultValue);
	}
	if (fieldIt->is_number ()) {
	    // single number applies to all components (common for distancemax/distancemin)
	    float val = fieldIt->get<float> ();
	    return glm::vec3 (val, val, val);
	}
	if (fieldIt->is_array () && fieldIt->size () >= 3) {
	    return glm::vec3 ((*fieldIt)[0].get<float> (), (*fieldIt)[1].get<float> (), (*fieldIt)[2].get<float> ());
	}
	return defaultValue;
    };

    auto parseVec2 = [&] (const char* fieldName, const glm::vec2& defaultValue) -> glm::vec2 {
	const auto fieldIt = it.find (fieldName);
	if (fieldIt == it.end ()) {
	    return defaultValue;
	}
	if (fieldIt->is_string ()) {
	    return it.optional (fieldName, defaultValue);
	}
	if (fieldIt->is_array () && fieldIt->size () >= 2) {
	    return glm::vec2 ((*fieldIt)[0].get<float> (), (*fieldIt)[1].get<float> ());
	}
	return defaultValue;
    };

    try {
	return ParticleEmitter {
	    .id = it.optional ("id", -1),
	    .name = name,
	    .directions = parseVec3 ("directions", glm::vec3 (1.0f, 1.0f, 0.0f)),
	    .distanceMin = parseVec3 ("distancemin", glm::vec3 (0.0f, 0.0f, 0.0f)),
	    .distanceMax = parseVec3 ("distancemax", glm::vec3 (256.0f, 256.0f, 0.0f)),
	    .origin = parseVec3 ("origin", glm::vec3 (0.0f)),
	    .sign = parseVec3 ("sign", glm::vec3 (0.0f)),
	    .instantaneous = it.optional ("instantaneous", 0u),
	    .speedMin = it.optional ("speedmin", 0.0f),
	    .speedMax = it.optional ("speedmax", 0.0f),
	    .rate = it.optional ("rate", 10.0f),
	    .controlPoint = it.optional ("controlpoint", 0),
	    .flags = it.optional ("flags", 0u),
	    .cone = it.optional ("cone", 0.0f),
	    .delay = it.optional ("delay", 0.0f),
	    .duration = it.optional ("duration", 0.0f),
	    .audioProcessingBounds = parseVec2 ("audioprocessingbounds", glm::vec2 (0.8f, 1.0f)),
	    .audioProcessingExponent = it.optional ("audioprocessingexponent", 2.0f),
	    .audioProcessingFrequencyStart = it.optional ("audioprocessingfrequencystart", 0),
	    .audioProcessingFrequencyEnd = it.optional ("audioprocessingfrequencyend", 1),
	    .audioProcessingMode = it.optional ("audioprocessingmode", 0),
	    .minPeriodicDelay = it.optional ("minperiodicdelay", 1.0f),
	    .maxPeriodicDelay = it.optional ("maxperiodicdelay", 2.0f),
	    .minPeriodicDuration = it.optional ("minperiodicduration", 2.0f),
	    .maxPeriodicDuration = it.optional ("maxperiodicduration", 3.0f),
	};
    } catch (nlohmann::json::exception& e) {
	sLog.error ("Error parsing emitter: ", e.what ());
	sLog.error ("Emitter JSON: ", it.dump ());
	throw;
    }
}

ParticleInitializerUniquePtr ObjectParser::parseParticleInitializer (const JSON& it, const Properties& properties) {
    std::string name = it.optional<std::string> ("name", "");

    if (name == "colorrandom") {
	return std::make_unique<ColorRandomInitializer> (
	    it.color ("min", properties, Builders::ColorBuilder::Black),
	    it.color ("max", properties, Builders::ColorBuilder::White), it.user ("exponent", properties, 1.0f)
	);
    } else if (name == "sizerandom") {
	return std::make_unique<SizeRandomInitializer> (
	    it.user ("min", properties, 0.0f), it.user ("max", properties, 20.0f),
	    it.user ("exponent", properties, 1.0f)
	);
    } else if (name == "alpharandom") {
	return std::make_unique<AlphaRandomInitializer> (
	    it.user ("min", properties, 0.05f), it.user ("max", properties, 1.0f)
	);
    } else if (name == "lifetimerandom") {
	return std::make_unique<LifetimeRandomInitializer> (
	    it.user ("min", properties, 0.0f), it.user ("max", properties, 1.0f)
	);
    } else if (name == "velocityrandom") {
	return std::make_unique<VelocityRandomInitializer> (
	    it.user ("min", properties, glm::vec3 (-32.0f)), it.user ("max", properties, glm::vec3 (32.0f))
	);
    } else if (name == "rotationrandom") {
	return std::make_unique<RotationRandomInitializer> (
	    it.user ("min", properties, glm::vec3 (0.0f)),
	    it.user ("max", properties, glm::vec3 (0.0f, 0.0f, glm::two_pi<float> ()))
	);
    } else if (name == "angularvelocityrandom") {
	return std::make_unique<AngularVelocityRandomInitializer> (
	    it.user ("min", properties, glm::vec3 (0.0f, 0.0f, -5.0f)),
	    it.user ("max", properties, glm::vec3 (0.0f, 0.0f, 5.0f)), it.user ("exponent", properties, 1.0f)
	);
    } else if (name == "turbulentvelocityrandom") {
	return std::make_unique<TurbulentVelocityRandomInitializer> (
	    it.user ("speedmin", properties, 100.0f), it.user ("speedmax", properties, 250.0f),
	    it.user ("scale", properties, 1.0f), it.user ("offset", properties, 0.0f),
	    it.user ("forward", properties, glm::vec3 (0.0f, 1.0f, 0.0f)), it.user ("timescale", properties, 1.0f),
	    it.user ("phasemin", properties, 0.0f), it.user ("phasemax", properties, 0.1f),
	    it.user ("right", properties, glm::vec3 (0.0f, 0.0f, 1.0f)), it.user ("audioprocessingmode", properties, 0),
	    it.user ("audioprocessingbounds", properties, glm::vec2 (0.8f, 1.0f)),
	    it.user ("audioprocessingexponent", properties, 2.0f),
	    it.user ("audioprocessingfrequencystart", properties, 0),
	    it.user ("audioprocessingfrequencyend", properties, 1)
	);
    } else if (name == "inheritinitialvaluefromevent") {
	return std::make_unique<InheritInitialValueFromEventInitializer> (
	    parseParticleEventInput (it, ParticleEventInput::SetColor)
	);
    } else if (name == "inheritcontrolpointvelocity") {
	return std::make_unique<InheritControlPointVelocityInitializer> (
	    it.optional ("controlpoint", 0), it.user ("min", properties, 0.1f), it.user ("max", properties, 0.2f)
	);
    } else if (name == "mapsequencearoundcontrolpoint") {
	return std::make_unique<MapSequenceAroundControlPointInitializer> (
	    it.user ("controlpoint", properties, 0), it.user ("count", properties, 1),
	    it.user ("speedmin", properties, glm::vec3 (0.0f)), it.user ("speedmax", properties, glm::vec3 (100.0f))
	);
    } else if (name == "hsvcolorrandom") {
	// defaults from wallpaper64.exe sub_1401BA3E0
	return std::make_unique<HsvColorRandomInitializer> (
	    it.user ("huemin", properties, 0.0f), it.user ("huemax", properties, 1.0f),
	    std::max (0, it.optional ("huesteps", 6)), it.user ("saturationmin", properties, 0.5f),
	    it.user ("saturationmax", properties, 1.0f), it.user ("valuemin", properties, 0.5f),
	    it.user ("valuemax", properties, 1.0f)
	);
    } else if (name == "colorlist") {
	return std::make_unique<ColorListInitializer> (
	    parseColorList (it), it.user ("huenoise", properties, 0.0f), it.user ("saturationnoise", properties, 0.0f),
	    it.user ("valuenoise", properties, 0.0f)
	);
    } else if (name == "positionoffsetrandom") {
	// octaves clamped to 1..8 like sub_1401C5490
	const int octaves = it.optional ("octaves", 6);
	return std::make_unique<PositionOffsetRandomInitializer> (
	    userIfSet (it, "directions", properties), it.user ("sign", properties, glm::vec3 (0.0f)),
	    userIfSet (it, "scale", properties), userIfSet (it, "distance", properties),
	    it.user ("timescale", properties, 1.0f), octaves >= 8 ? 8 : (octaves == 0 ? 1 : octaves)
	);
    } else if (name == "mapsequencebetweencontrolpoints") {
	glm::vec2 bounds (0.0f, 1.0f);
	const auto boundsIt = it.optional ("bounds");
	if (boundsIt.has_value () && boundsIt->is_string ()) {
	    const glm::vec3 parsed = parseRemapRange (it, "bounds", 0.0f);
	    bounds = glm::vec2 (parsed.x, parsed.y);
	}
	return std::make_unique<MapSequenceBetweenControlPointsInitializer> (
	    it.optional ("count", 32.0f), bounds, it.optional<std::string> ("limitbehavior", "repeat") == "mirror",
	    controlPointIndex (it, "controlpointstart", 0), controlPointIndex (it, "controlpointend", 1),
	    it.optional ("flags", 0u), it.user ("arcamount", properties, 0.3f),
	    it.user ("arcdirection", properties, glm::vec3 (0.0f, 1.0f, 0.0f)),
	    it.user ("sizereductionamount", properties, 0.9f)
	);
    } else if (name == "remapinitialvalue") {
	return std::make_unique<RemapInitialValueInitializer> (parseRemap (it, "maxlifetime"));
    }

    return nullptr;
}

ParticleOperatorUniquePtr ObjectParser::parseParticleOperator (const JSON& it, const Properties& properties) {
    std::string name = it.optional<std::string> ("name", "");

    if (name == "movement") {
	return std::make_unique<MovementOperator> (
	    it.user ("drag", properties, 0.0f), it.user ("gravity", properties, glm::vec3 (0.0f)),
	    it.optional ("flags", 0u)
	);
    } else if (name == "angularmovement") {
	return std::make_unique<AngularMovementOperator> (
	    it.user ("drag", properties, 0.0f), it.user ("force", properties, glm::vec3 (0.0f))
	);
    } else if (name == "alphafade") {
	return std::make_unique<AlphaFadeOperator> (
	    it.user ("fadeintime", properties, 0.5f), it.user ("fadeouttime", properties, 0.5f)
	);
    } else if (name == "sizechange") {
	return std::make_unique<SizeChangeOperator> (
	    it.user ("starttime", properties, 0.0f), it.user ("endtime", properties, 1.0f),
	    it.user ("startvalue", properties, 1.0f), it.user ("endvalue", properties, 0.0f)
	);
    } else if (name == "alphachange") {
	return std::make_unique<AlphaChangeOperator> (
	    it.user ("starttime", properties, 0.0f), it.user ("endtime", properties, 1.0f),
	    it.user ("startvalue", properties, 1.0f), it.user ("endvalue", properties, 0.0f)
	);
    } else if (name == "colorchange") {
	return std::make_unique<ColorChangeOperator> (
	    it.user ("starttime", properties, 0.0f), it.user ("endtime", properties, 1.0f),
	    it.user ("startvalue", properties, glm::vec3 (1.0f)), it.user ("endvalue", properties, glm::vec3 (1.0f))
	);
    } else if (name == "turbulence") {
	return std::make_unique<TurbulenceOperator> (
	    it.user ("scale", properties, 0.005f), it.user ("speedmin", properties, 500.0f),
	    it.user ("speedmax", properties, 1000.0f), it.user ("timescale", properties, 0.01f),
	    it.user ("mask", properties, glm::vec3 (1.0f, 1.0f, 0.0f)), it.user ("phasemin", properties, 0.0f),
	    it.user ("phasemax", properties, 0.0f), it.user ("audioprocessingmode", properties, 0),
	    it.user ("audioprocessingbounds", properties, glm::vec2 (0.8f, 1.0f)),
	    it.user ("audioprocessingexponent", properties, 2.0f),
	    it.user ("audioprocessingfrequencystart", properties, 0),
	    it.user ("audioprocessingfrequencyend", properties, 1)
	);
    } else if (name == "vortex" || name == "vortex_v2") {
	return std::make_unique<VortexOperator> (
	    it.optional ("controlpoint", 0),
	    it.optional ("flags", 0), // 1 = infinite axis, 2 = maintain distance, 4 = ring shape
	    it.user ("axis", properties, glm::vec3 (0.0f, 0.0f, 1.0f)),
	    it.user ("offset", properties, glm::vec3 (0.0f)), it.user ("distanceinner", properties, 500.0f),
	    it.user ("distanceouter", properties, 650.0f), it.user ("speedinner", properties, 2500.0f),
	    it.user ("speedouter", properties, 0.0f), it.user ("centerforce", properties, 1.0f),
	    it.user ("ringradius", properties, 300.0f), it.user ("ringwidth", properties, 50.0f),
	    it.user ("ringpulldistance", properties, 50.0f), it.user ("ringpullforce", properties, 10.0f),
	    it.user ("audioprocessingmode", properties, 0),
	    it.user ("audioprocessingbounds", properties, glm::vec2 (0.8f, 1.0f)),
	    it.user ("audioprocessingexponent", properties, 2.0f),
	    it.user ("audioprocessingfrequencystart", properties, 0),
	    it.user ("audioprocessingfrequencyend", properties, 1)
	);
    } else if (name == "inheritvaluefromevent") {
	return std::make_unique<InheritValueFromEventOperator> (
	    parseParticleEventInput (it, ParticleEventInput::SetColorOpacity),
	    glm::vec4 (
		it.optional ("blendinstart", 0.0f), it.optional ("blendinend", 0.0f), it.optional ("blendoutstart", 1.0f),
		it.optional ("blendoutend", 1.0f)
	    )
	);
    } else if (name == "controlpointattract") {
	return std::make_unique<ControlPointAttractOperator> (
	    it.optional ("controlpoint", 0), it.user ("origin", properties, glm::vec3 (0.0f)),
	    it.user ("scale", properties, 100.0f), it.user ("threshold", properties, 1000.0f)
	);
    } else if (name == "oscillatealpha") {
	return std::make_unique<OscillateAlphaOperator> (
	    it.user ("frequencymin", properties, 0.0f), it.user ("frequencymax", properties, 10.0f),
	    it.user ("scalemin", properties, 0.0f), it.user ("scalemax", properties, 1.0f),
	    it.user ("phasemin", properties, 0.0f), it.user ("phasemax", properties, glm::two_pi<float> ())
	);
    } else if (name == "oscillatesize") {
	return std::make_unique<OscillateSizeOperator> (
	    it.user ("frequencymin", properties, 0.0f), it.user ("frequencymax", properties, 10.0f),
	    it.user ("scalemin", properties, 0.8f), it.user ("scalemax", properties, 1.2f),
	    it.user ("phasemin", properties, 0.0f), it.user ("phasemax", properties, glm::two_pi<float> ())
	);
    } else if (name == "oscillateposition") {
	return std::make_unique<OscillatePositionOperator> (
	    it.user ("frequencymin", properties, 0.0f), it.user ("frequencymax", properties, 5.0f),
	    it.user ("scalemin", properties, 0.0f), it.user ("scalemax", properties, 10.0f),
	    it.user ("phasemin", properties, 0.0f), it.user ("phasemax", properties, glm::two_pi<float> ()),
	    it.user ("mask", properties, glm::vec3 (1.0f, 1.0f, 0.0f))
	);
    } else if (name == "capvelocity") {
	return std::make_unique<CapVelocityOperator> (userIfSet (it, "maxspeed", properties), parseBlendWindow (it));
    } else if (name == "boids") {
	// defaults from wallpaper64.exe sub_1401BF700
	return std::make_unique<BoidsOperator> (
	    userIfSet (it, "separationthreshold", properties), userIfSet (it, "neighborthreshold", properties),
	    userIfSet (it, "maxspeed", properties), it.user ("separationfactor", properties, 15.0f),
	    it.user ("alignmentfactor", properties, 1.0f), it.user ("cohesionfactor", properties, 2.0f),
	    it.optional ("flags", 1u)
	);
    } else if (name == "remapvalue") {
	return std::make_unique<RemapValueOperator> (parseRemap (it, "lifetimefraction"), parseBlendWindow (it));
    } else if (name == "maintaindistancetocontrolpoint") {
	return std::make_unique<MaintainDistanceToControlPointOperator> (
	    controlPointIndex (it, "controlpoint", 0), userIfSet (it, "distance", properties),
	    it.user ("variablestrength", properties, 0.0f), parseBlendWindow (it)
	);
    } else if (name == "maintaindistancebetweencontrolpoints") {
	return std::make_unique<MaintainDistanceBetweenControlPointsOperator> (
	    controlPointIndex (it, "controlpointstart", 0), controlPointIndex (it, "controlpointend", 1),
	    parseBlendWindow (it)
	);
    } else if (name == "reducemovementnearcontrolpoint") {
	// defaults from wallpaper64.exe sub_1401BE810
	return std::make_unique<ReduceMovementNearControlPointOperator> (
	    controlPointIndex (it, "controlpoint", 0), userIfSet (it, "distanceinner", properties),
	    userIfSet (it, "distanceouter", properties), it.user ("reductioninner", properties, 100.0f),
	    it.user ("reductionouter", properties, 0.0f), parseBlendWindow (it)
	);
    } else if (
	name == "collisionplane" || name == "collisionsphere" || name == "collisionbox" || name == "collisionbounds"
	|| name == "collisionquad" || name == "collisionmodel"
    ) {
	static constexpr const char* behaviors[] = { "bounce", "slide", "stop", "delete" };
	ParticleCollisionShape shape = ParticleCollisionShape::Plane;
	if (name == "collisionsphere") {
	    shape = ParticleCollisionShape::Sphere;
	} else if (name == "collisionbox") {
	    shape = ParticleCollisionShape::Box;
	} else if (name == "collisionbounds") {
	    shape = ParticleCollisionShape::Bounds;
	} else if (name == "collisionquad") {
	    shape = ParticleCollisionShape::Quad;
	} else if (name == "collisionmodel") {
	    shape = ParticleCollisionShape::Model;
	}

	// sub_1401C03F0: anything but slide, stop or delete bounces
	ParticleCollisionBehavior behavior = ParticleCollisionBehavior::Bounce;
	const std::string behaviorName = it.optional<std::string> ("collisionbehavior", "bounce");
	for (size_t i = 0; i < std::size (behaviors); i++) {
	    if (behaviorName == behaviors[i]) {
		behavior = static_cast<ParticleCollisionBehavior> (i);
	    }
	}

	// defaults from wallpaper64.exe sub_1401C00A0, sub_1401C0540, sub_1401C0740 and sub_1401C0870
	return std::make_unique<CollisionOperator> (
	    shape, behavior, it.user ("bouncefactor", properties, 0.5f), it.optional ("flags", 0u),
	    controlPointIndex (it, "controlpoint", 0), it.user ("plane", properties, glm::vec3 (0.0f, 1.0f, 0.0f)),
	    userIfSet (it, "distance", properties), userIfSet (it, "origin", properties),
	    userIfSet (it, "radius", properties), it.user ("forward", properties, glm::vec3 (0.0f, 0.0f, 1.0f)),
	    userIfSet (it, "size", properties)
	);
    }

    return nullptr;
}

ParticleRenderer ObjectParser::parseParticleRenderer (const JSON& it) {
    std::string name = "sprite";
    const auto nameIt = it.find ("name");
    if (nameIt != it.end () && nameIt->is_string ()) {
	name = nameIt->get<std::string> ();
    }

    float subdivisionDefault = (name == "rope") ? 4.0f : 1.0f;
    float lengthDefault = (name == "ropetrail") ? 1.0f : 0.05f;

    return ParticleRenderer {
	.name = name,
	.length = it.optional ("length", lengthDefault),
	.maxLength = it.optional ("maxlength", 10.0f),
	.minLength = it.optional ("minlength", 0.0f),
	.subdivision = it.optional ("subdivision", subdivisionDefault),
	.segments = it.optional ("segments", 4.0f),
	.uvScale = it.optional ("uvscale", 1.0f),
	.uvScrolling = it.optional ("uvscrolling", false),
	.uvSmoothing = it.optional ("uvsmoothing", true),
	.fadeAlpha = it.optional ("fadealpha", false),
	.fadeSize = it.optional ("fadesize", false),
    };
}

ParticleEventInput ObjectParser::parseParticleEventInput (const JSON& it, ParticleEventInput fallback) {
    // wallpaper64.exe off_140484D90
    static constexpr const char* names[] = {
	"setcolor", "multiplycolor", "setopacity", "multiplyopacity", "setcoloropacity", "multiplycoloropacity",
	"setvelocity", "addvelocity", "setsize", "multiplysize", "setrotation", "addrotation", "setangularvelocity",
	"addangularvelocity",
    };

    const auto inputIt = it.find ("input");
    if (inputIt == it.end () || !inputIt->is_string ()) {
	return fallback;
    }

    const std::string input = inputIt->get<std::string> ();
    for (size_t i = 0; i < std::size (names); i++) {
	if (input == names[i]) {
	    return static_cast<ParticleEventInput> (i);
	}
    }

    sLog.error ("Unknown particle event input ", input);
    return fallback;
}

ParticleControlPoint ObjectParser::parseParticleControlPoint (const JSON& it) {
    // offset can be string "x y z" or array [x,y,z]
    glm::vec3 offset (0.0f);
    const auto offsetIt = it.find ("offset");
    if (offsetIt != it.end ()) {
	if (offsetIt->is_string ()) {
	    std::string offsetStr = offsetIt->get<std::string> ();
	    std::istringstream iss (offsetStr);
	    iss >> offset.x >> offset.y >> offset.z;
	} else {
	    try {
		offset = it.optional ("offset", glm::vec3 (0.0f));
	    } catch (...) {
		offset = glm::vec3 (0.0f);
	    }
	}
    }

    return ParticleControlPoint {
	.id = it.optional ("id", -1),
	.flags = it.optional ("flags", 0u),
	.offset = offset,
	.lockToPointer = it.optional ("locktopointer", false),
	.parentControlPoint = it.optional ("parentcontrolpoint", 0),
    };
}

ParticleChild ObjectParser::parseParticleChild (
    const JSON& it, const JSON& owner, const Project& project, const ObjectData& ownerBase, size_t index, int depth
) {
    // defaults and type names per wallpaper64.exe sub_1401C1430 / sub_1401C5490
    ParticleChildType type = ParticleChildType::Static;
    std::string typeName = "static";
    if (const auto typeIt = it.find ("type"); typeIt != it.end () && typeIt->is_string ()) {
	typeName = typeIt->get<std::string> ();
    }
    if (typeName == "eventfollow") {
	type = ParticleChildType::EventFollow;
    } else if (typeName == "eventspawn") {
	type = ParticleChildType::EventSpawn;
    } else if (typeName == "eventdeath") {
	type = ParticleChildType::EventDeath;
    } else if (typeName != "static") {
	sLog.error ("Unknown particle child type ", typeName, ", treating it as static");
    }

    std::string name = "";
    const auto nameIt = it.find ("name");
    if (nameIt != it.end () && nameIt->is_string ()) {
	name = nameIt->get<std::string> ();
    }

    // vec3 fields may show up as strings, arrays, single numbers, or be missing entirely
    auto parseVec3 = [&] (const char* fieldName, const glm::vec3& defaultValue) -> glm::vec3 {
	const auto fieldIt = it.find (fieldName);
	if (fieldIt == it.end ()) {
	    return defaultValue;
	}
	if (fieldIt->is_string ()) {
	    return it.optional (fieldName, defaultValue);
	}
	if (fieldIt->is_number ()) {
	    float val = fieldIt->get<float> ();
	    return glm::vec3 (val, val, val);
	}
	if (fieldIt->is_array () && fieldIt->size () >= 3) {
	    return glm::vec3 ((*fieldIt)[0].get<float> (), (*fieldIt)[1].get<float> (), (*fieldIt)[2].get<float> ());
	}
	return defaultValue;
    };

    const glm::vec3 angles = parseVec3 ("angles", glm::vec3 (0.0f));
    const glm::vec3 origin = parseVec3 ("origin", glm::vec3 (0.0f));
    const glm::vec3 scale = parseVec3 ("scale", glm::vec3 (1.0f));

    // WE's row vector matrix: rotation rows from z/y/x angles, each row scaled by its scale component, origin last
    const float cz = std::cos (angles.z), sz = std::sin (angles.z);
    const float cy = std::cos (angles.y), sy = std::sin (angles.y);
    const float cx = std::cos (angles.x), sx = std::sin (angles.x);
    const glm::vec3 row0 (cy * cz, cy * sz, -sy);
    const glm::vec3 row1 (sy * cz * sx - cx * sz, sy * sz * sx + cx * cz, sx * cy);
    const glm::vec3 row2 (sx * sz + sy * cx * cz, sy * cx * sz - sx * cz, cx * cy);
    const glm::mat4 weTransform (
	glm::vec4 (row0 * scale.x, 0.0f), glm::vec4 (row1 * scale.y, 0.0f), glm::vec4 (row2 * scale.z, 0.0f),
	glm::vec4 (origin, 1.0f)
    );
    // particle space here is WE's with y flipped, same as emitter origins and velocities
    const glm::mat4 flipY = glm::scale (glm::mat4 (1.0f), glm::vec3 (1.0f, -1.0f, 1.0f));

    // the child is loaded like a particle object of its own that shares the owner's instance override
    JSON childObject = JSON::object ();
    childObject["particle"] = name;
    if (const auto overrideIt = owner.find ("instanceoverride"); overrideIt != owner.end ()) {
	childObject["instanceoverride"] = *overrideIt;
    }

    // a stable id of its own keeps its random seed (LWE_FIXED_TIMESTEP) apart from the owner's
    const uint32_t hashed = (static_cast<uint32_t> (ownerBase.id) * 31u + static_cast<uint32_t> (index) + 1u) * 2654435761u;

    ParticleUniquePtr particle = nullptr;
    if (!name.empty ()) {
	try {
	    particle = parseParticle (
		childObject, project,
		ObjectData {
		    .id = -1 - static_cast<int> (hashed & 0x3FFFFFFFu),
		    .name = ownerBase.name + " > " + name,
		    .dependencies = {},
		    .parent = std::nullopt,
		    .attachment = std::nullopt,
		    .sortOrder = std::nullopt,
		    .origin = Builders::UserSettingBuilder::fromValue (glm::vec3 (0.0f)),
		    .groupScale = Builders::UserSettingBuilder::fromValue (glm::vec3 (1.0f)),
		    .groupAngles = Builders::UserSettingBuilder::fromValue (glm::vec3 (0.0f)),
		    .groupVisible = Builders::UserSettingBuilder::fromValue (true),
		    .groupParallaxDepth = Builders::UserSettingBuilder::fromValue (glm::vec2 (1.0f)),
		    .solid = Builders::UserSettingBuilder::fromValue (true),
		    .disablePropagation = Builders::UserSettingBuilder::fromValue (false),
		    .perspective = Builders::UserSettingBuilder::fromValue (false),
		},
		depth
	    );
	} catch (const std::exception& e) {
	    sLog.error ("Cannot load child particle ", name, ": ", e.what ());
	}

	if (particle != nullptr && particle->material == nullptr) {
	    sLog.error ("Child particle ", name, " has no material, skipping it");
	    particle = nullptr;
	}
    }

    return ParticleChild {
	.type = type,
	.name = name,
	.maxCount = it.optional ("maxcount", 10),
	.probability = it.optional ("probability", 1.0f),
	.flags = it.optional ("flags", 0u),
	.controlPointStartIndex = it.optional ("controlpointstartindex", 0),
	.transform = flipY * weTransform * flipY,
	.particle = std::move (particle),
    };
}

ParticleInstanceOverride ObjectParser::parseParticleInstanceOverride (const JSON& it, const Properties& properties) {
    auto result = ParticleInstanceOverride {
	.enabled = it.user ("enabled", properties, true),
	.alpha = it.user ("alpha", properties, 1.0f),
	.size = it.user ("size", properties, 1.0f),
	.lifetime = it.user ("lifetime", properties, 1.0f),
	.rate = it.user ("rate", properties, 1.0f),
	.speed = it.user ("speed", properties, 1.0f),
	.count = it.user ("count", properties, 1.0f),
	.color = it.user ("color", properties, glm::vec3 (1.0f)),
	.colorn = it.user ("colorn", properties, glm::vec3 (1.0f)),
	.hasColor = it.optional ("colorn").has_value () || it.optional ("color").has_value (),
    };

    // WE converts the legacy 0-255 "color" into colorn on load (replacing any colorn) and only reads colorn after
    if (it.optional ("color").has_value ()) {
	result.colorn = Builders::UserSettingBuilder::fromValue (result.color->value->getVec3 () / 255.0f);
    }

    return result;
}
