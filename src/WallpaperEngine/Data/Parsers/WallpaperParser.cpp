#include "WallpaperParser.h"

#include "ObjectParser.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/FileSystem/Container.h"
#include "WallpaperEngine/Logging/Log.h"

#include <sstream>

using namespace WallpaperEngine::Data::Parsers;

namespace {
// vectors in camera path files are "x y z" strings read with atof, missing numbers are 0
glm::vec3 parsePathVector (const JSON& transform, const char* key) {
    glm::vec3 result (0.0f);
    const auto it = transform.find (key);

    if (it == transform.end () || !it->is_string ()) {
	return result;
    }

    std::istringstream stream (it->get<std::string> ());

    for (int component = 0; component < 3 && stream >> result[component]; component++) {
    }

    return result;
}

float numberOr (const JSON& data, const char* key, const float fallback) {
    const auto it = data.find (key);

    return it != data.end () && it->is_number () ? it->get<float> () : fallback;
}

bool isDisabled (const JSON& data) {
    const auto it = data.find ("disabled");

    return it != data.end () && it->is_boolean () && it->get<bool> ();
}

// one file listed under "camera" "paths" (sub_140198E20), every enabled path goes into the list
void parseCameraPathFile (const std::string& filename, const Project& project, std::vector<CameraPath>& paths) {
    JSON file;

    try {
	file = JSON::parseAsset (project.assetLocator->readString (filename));
    } catch (const std::exception& e) {
	sLog.error ("Cannot load camera path file ", filename, ": ", e.what ());
	return;
    }

    const auto list = file.find ("paths");

    if (list == file.end () || !list->is_array ()) {
	return;
    }

    for (const auto& path : *list) {
	if (!path.is_object ()) {
	    break;
	}

	const auto transforms = path.find ("transforms");

	// WE stops at the first path without transforms
	if (transforms == path.end () || !transforms->is_array () || transforms->empty ()) {
	    break;
	}

	if (isDisabled (path)) {
	    continue;
	}

	CameraPath result { .keys = {}, .duration = numberOr (path, "duration", 0.0f) };
	const int count = static_cast<int> (transforms->size ());

	for (int index = 0; index < count; index++) {
	    const auto& transform = (*transforms)[index];

	    if (!transform.is_object () || isDisabled (transform)) {
		continue;
	    }

	    // without a timestamp the keys spread evenly over the duration
	    float time = 0.0f;

	    if (const auto stamp = transform.find ("timestamp"); stamp != transform.end () && stamp->is_number ()) {
		time = stamp->get<float> ();
	    } else if (index != 0) {
		time = static_cast<float> (index) / static_cast<float> (count - 1) * result.duration;
	    }

	    result.keys.push_back (CameraPathKey {
		.time = time,
		.eye = parsePathVector (transform, "eye"),
		.center = parsePathVector (transform, "center"),
		.up = parsePathVector (transform, "up"),
		.zoom = numberOr (transform, "zoom", 1.0f),
	    });
	}

	paths.push_back (std::move (result));
    }
}

std::vector<CameraPath> parseCameraPaths (const JSON& camera, const Project& project) {
    std::vector<CameraPath> paths;
    const auto list = camera.find ("paths");

    if (list == camera.end () || !list->is_array ()) {
	return paths;
    }

    for (const auto& file : *list) {
	if (file.is_string ()) {
	    parseCameraPathFile (file.get<std::string> (), project, paths);
	}
    }

    return paths;
}
} // namespace

WallpaperUniquePtr WallpaperParser::parse (const JSON& file, Project& project) {
    switch (project.type) {
	case Project::Type_Scene:
	    return parseScene (file, project);
	case Project::Type_Video:
	    return parseVideo (file, project);
	case Project::Type_Web:
	    return parseWeb (file, project);
	default:
	    sLog.exception ("Unexpected project type value found... This is likely a bug");
    }
}

SceneUniquePtr WallpaperParser::parseScene (const JSON& file, Project& project) {
    const auto scene = JSON::parseAsset (project.assetLocator->readString (file));
    const auto camera = scene.require ("camera", "Scenes must have a camera section");
    const auto general = scene.require ("general", "Scenes must have a general section");
    // null (or missing) means a perspective camera, a 3D scene
    const auto projectionIt = general.find ("orthogonalprojection");
    const bool perspective = projectionIt == general.end () || projectionIt->is_null ();
    const JSON projection = perspective ? JSON::object () : JSON (*projectionIt);
    const auto objects = scene.require ("objects", "Scenes must have an objects section");
    const auto& properties = project.properties;
    // particles read it while the objects get parsed below
    project.sceneVersion = scene.optional ("version", 0);
    // missing keys fall back to the scene constructor's defaults in wallpaper64.exe 2.8.42 (sub_140186C90)

    return std::make_unique <Scene> (
        WallpaperData {
            .filename = "",
            .project = project
        }, SceneData {
            .colors = {
                .ambient  = general.user ("ambientcolor", properties, glm::vec3 (0.0f)),
                .skylight = general.user ("skylightcolor", properties, glm::vec3 (0.0f)),
                .clear = general.user ("clearcolor", properties, glm::vec3 (1.0f)),
            },
            .fog = {
                .distanceEnabled = general.user ("fogdistance", properties, false),
                .distanceColor = general.user ("fogdistancecolor", properties, glm::vec3 (0.0f)),
                .distanceStart = general.user ("fogdistancestart", properties, 1.0f),
                .distanceEnd = general.user ("fogdistanceend", properties, 5.0f),
                .distanceStartDensity = general.user ("fogdistancestartdensity", properties, 0.0f),
                .distanceEndDensity = general.user ("fogdistanceenddensity", properties, 1.0f),
                .heightEnabled = general.user ("fogheight", properties, false),
                .heightColor = general.user ("fogheightcolor", properties, glm::vec3 (0.0f)),
                .heightStart = general.user ("fogheightstart", properties, 1.0f),
                .heightEnd = general.user ("fogheightend", properties, -3.0f),
                .heightStartDensity = general.user ("fogheightstartdensity", properties, 0.0f),
                .heightEndDensity = general.user ("fogheightenddensity", properties, 1.0f),
            },
            .camera = {
                .fade = general.user ("camerafade", properties, false),
                .preview = general.optional ("camerapreview", false),
                .bloom = {
                    .enabled = general.user ("bloom", properties, false),
                    .strength = general.user ("bloomstrength", properties, 2.0f),
                    .threshold = general.user ("bloomthreshold", properties, 0.65f),
                    .hdr = general.user ("hdr", properties, false),
                    .hdrStrength = general.user ("bloomhdrstrength", properties, 2.0f),
                    .hdrThreshold = general.user ("bloomhdrthreshold", properties, 1.0f),
                    .hdrFeather = general.user ("bloomhdrfeather", properties, 0.1f),
                    .hdrScatter = general.user ("bloomhdrscatter", properties, 1.619f),
                    .hdrIterations = general.user ("bloomhdriterations", properties, 8),
                    .tint = general.user ("bloomtint", properties, glm::vec3 (1.0f)),
                },
                .parallax = {
                    .enabled = general.user ("cameraparallax", properties, false),
                    .amount = general.user ("cameraparallaxamount", properties, 0.5f),
                    .delay = general.user ("cameraparallaxdelay", properties, 0.1f),
                    .mouseInfluence = general.user ("cameraparallaxmouseinfluence", properties, 0.5f),
                },
                .shake = {
                    .enabled = general.user ("camerashake", properties, false),
                    .amplitude = general.user ("camerashakeamplitude", properties, 0.5f),
                    .roughness = general.user ("camerashakeroughness", properties, 1.0f),
                    .speed = general.user ("camerashakespeed", properties, 3.0f),
                },
                .configuration = {
                    .center = camera.require <glm::vec3> ("center", "Camera must have a center position"),
                    .eye = camera.require <glm::vec3> ("eye", "Camera must have an eye position"),
                    .up = camera.require <glm::vec3> ("up", "Camera must have an up position"),
                },
                .paths = parseCameraPaths (camera, project),
                .projection = {
                    .width  = perspective || projection.optional ("auto", false) ? 0 : projection.require <int> ("width",  "Projection must have a width"),
                    .height = perspective || projection.optional ("auto", false) ? 0 : projection.require <int> ("height", "Projection must have a height"),
                    .isAuto = perspective || projection.optional ("auto", false),
                    .isPerspective = perspective,
                    .nearz = general.user ("nearz", properties, 0.1f),
                    .farz = general.user ("farz", properties, 10000.0f),
                    .fov = general.user ("fov", properties, 50.0f),
                    .perspectiveOverrideFov = general.user ("perspectiveoverridefov", properties, 95.0f),
                    .zoom = general.user ("zoom", properties, 1.0f),
                }
            },
            .objects = parseObjects (objects, project),
        }
    );
}

VideoUniquePtr WallpaperParser::parseVideo (const JSON& file, Project& project) {
    return std::make_unique<Video> (WallpaperData { .filename = file, .project = project });
}

WebUniquePtr WallpaperParser::parseWeb (const JSON& file, Project& project) {
    return std::make_unique<Web> (WallpaperData {
	.filename = file,
	.project = project,
    });
}

ObjectList WallpaperParser::parseObjects (const JSON& objects, const Project& project) {
    ObjectList result = {};

    for (const auto& cur : objects) {
	result.emplace_back (ObjectParser::parse (cur, project));
    }

    return result;
}