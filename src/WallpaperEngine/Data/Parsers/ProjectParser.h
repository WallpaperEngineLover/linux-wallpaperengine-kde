#pragma once

#include <nlohmann/json.hpp>

#include "WallpaperEngine/Assets/AssetLocator.h"
#include "WallpaperEngine/Data/JSON.h"
#include "WallpaperEngine/Data/Model/Project.h"

namespace WallpaperEngine::Data::Parsers {
using JSON = WallpaperEngine::Data::JSON::JSON;

using namespace WallpaperEngine::Assets;
using namespace WallpaperEngine::Data::Model;

class ProjectParser {
public:
    static ProjectUniquePtr parse (const JSON& data, AssetLocatorUniquePtr container);
    /** The type Wallpaper Engine takes from a main file's extension, nothing for an extension it has no rule for */
    static std::optional<Project::Type> typeOfFile (const std::string& file);

private:
    static Project::Type determineType (const JSON& data, const std::string& file);
    static std::optional<Project::Type> typeOfName (const std::string& type);
    static Project::Type parseType (const std::string& type);
    static Properties parseProperties (const std::optional<JSON>& data);
};
} // namespace WallpaperEngine::Data::Parsers
