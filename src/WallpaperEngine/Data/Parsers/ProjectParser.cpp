#include <algorithm>

#include "ProjectParser.h"
#include "WallpaperEngine/Debugging/Profiling.h"
#include "WallpaperEngine/Logging/Log.h"

#include "WallpaperParser.h"

#include "Localization.h"
#include "PropertyParser.h"
#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"

using namespace WallpaperEngine::Data::Parsers;

static int backgroundId = 0;

ProjectUniquePtr ProjectParser::parse (const JSON& data, AssetLocatorUniquePtr container) {
    const auto general = data.optional ("general");
    const auto workshopId = data.optional ("workshopid");
    auto actualWorkshopId = std::to_string (--backgroundId);
    // read as text here, a number or an object would otherwise fail later without saying which key it was
    const auto mainFile = data.require<std::string> ("file", "Project's main file missing");

    if (workshopId.has_value ()) {
	if (workshopId->is_number ()) {
	    actualWorkshopId = std::to_string (workshopId->get<int> ());
	} else if (workshopId->is_string ()) {
	    actualWorkshopId = workshopId->get<std::string> ();
	} else {
	    sLog.error ("Invalid workshop id: ", workshopId->dump ());
	}
    }

    auto result = std::make_unique<Project> (Project {
	.title = data.optional<std::string> ("title", ""),
	.type = determineType (data, mainFile),
	.workshopId = actualWorkshopId,
	.supportsAudioProcessing = general.has_value () && general.value ().optional ("supportsaudioprocessing", false),
	.properties = parseProperties (general),
	.assetLocator = std::move (container),
    });

    {
	LWE_ZONE_TEXT ("parse wallpaper", mainFile);
	result->wallpaper = WallpaperParser::parse (JSON (mainFile), *result);
    }

    return result;
}

// sub_14011D7D0 / sub_14011E520: Wallpaper Engine never reads the "type" key, it works the type out of the main file's
// extension (.json .pkg .gif scene, .html web, .exe application, .mp4 .wmv .avi .m4v .mov .webm .mkv video) and writes
// that into the key. The declared type is only used here for a file it has no rule for.
std::optional<Project::Type> ProjectParser::typeOfFile (const std::string& file) {
    const auto dot = file.rfind ('.');

    if (dot == std::string::npos) {
	return std::nullopt;
    }

    auto extension = file.substr (dot);
    std::ranges::transform (extension, extension.begin (), tolower);

    if (extension == ".json" || extension == ".pkg" || extension == ".gif") {
	return Project::Type_Scene;
    }

    if (extension == ".html") {
	return Project::Type_Web;
    }

    if (extension == ".mp4" || extension == ".wmv" || extension == ".avi" || extension == ".m4v" || extension == ".mov"
	|| extension == ".webm" || extension == ".mkv") {
	return Project::Type_Video;
    }

    return std::nullopt;
}

Project::Type ProjectParser::determineType (const JSON& data, const std::string& file) {
    const auto declared = data.optional<std::string> ("type");
    const auto derived = typeOfFile (file);

    if (derived.has_value ()) {
	if (declared.has_value ()) {
	    auto name = *declared;
	    std::ranges::transform (name, name.begin (), tolower);

	    // a wrong declaration is reported, the file decides like in WE
	    if (const auto known = typeOfName (name); !known.has_value () || *known != *derived) {
		sLog.error (
		    "Project type '", *declared, "' does not match its main file '", file, "', using the file's"
		);
	    }
	}

	return *derived;
    }

    if (!declared.has_value ()) {
	sLog.exception ("Project type missing and the main file '", file, "' does not tell it");
    }

    auto name = *declared;
    std::ranges::transform (name, name.begin (), tolower);

    return parseType (name);
}

std::optional<Project::Type> ProjectParser::typeOfName (const std::string& type) {
    if (type == "scene") {
	return Project::Type_Scene;
    }

    if (type == "video") {
	return Project::Type_Video;
    }

    if (type == "web") {
	return Project::Type_Web;
    }

    return std::nullopt;
}

Project::Type ProjectParser::parseType (const std::string& type) {
    if (type == "scene") {
	return Project::Type_Scene;
    }

    if (type == "video") {
	return Project::Type_Video;
    }

    if (type == "web") {
	return Project::Type_Web;
    }

    sLog.exception ("Unsupported project type ", type);
}

Properties ProjectParser::parseProperties (const std::optional<JSON>& data) {
    if (!data.has_value ()) {
	return {};
    }

    const auto properties = data.value ().optional ("properties");

    if (!properties.has_value ()) {
	return {};
    }

    Properties result = {};
    const Localization localization (data);

    for (const auto& cur : properties.value ().items ()) {
	const auto& property = PropertyParser::parse (cur.value (), cur.key ());

	if (property == nullptr) {
	    continue;
	}

	property->text = localization.resolve (property->text);

	if (auto* combo = dynamic_cast<PropertyCombo*> (property.get ()); combo != nullptr) {
	    combo->relabel ([&localization] (const std::string& label) { return localization.resolve (label); });
	}

	result.emplace (cur.key (), property);
    }

    return result;
}
