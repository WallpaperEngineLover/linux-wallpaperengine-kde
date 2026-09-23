#include <algorithm>

#include "ProjectParser.h"
#include "WallpaperEngine/Logging/Log.h"

#include "WallpaperParser.h"

#include "Localization.h"
#include "PropertyParser.h"
#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/FileSystem/Container.h"

using namespace WallpaperEngine::Data::Parsers;

static int backgroundId = 0;

ProjectUniquePtr ProjectParser::parse (const JSON& data, AssetLocatorUniquePtr container) {
    const auto general = data.optional ("general");
    const auto workshopId = data.optional ("workshopid");
    auto actualWorkshopId = std::to_string (--backgroundId);
    auto type = data.require<std::string> ("type", "Project type missing");

    if (workshopId.has_value ()) {
	if (workshopId->is_number ()) {
	    actualWorkshopId = std::to_string (workshopId->get<int> ());
	} else if (workshopId->is_string ()) {
	    actualWorkshopId = workshopId->get<std::string> ();
	} else {
	    sLog.error ("Invalid workshop id: ", workshopId->dump ());
	}
    }

    std::ranges::transform (type, type.begin (), tolower);

    auto result = std::make_unique<Project> (Project {
	.title = data.require<std::string> ("title", "Project title missing"),
	.type = parseType (type),
	.workshopId = actualWorkshopId,
	.supportsAudioProcessing = general.has_value () && general.value ().optional ("supportsaudioprocessing", false),
	.properties = parseProperties (general),
	.assetLocator = std::move (container),
    });

    result->wallpaper = WallpaperParser::parse (data.require ("file", "Project's main file missing"), *result);

    return result;
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

	// null means the entry was a group, not an actual property
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
