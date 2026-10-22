#include "WallpaperEngine/Assets/AssetLocator.h"
#include "WallpaperEngine/Data/JSON.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/MaterialParser.h"
#include "WallpaperEngine/Data/Parsers/ObjectParser.h"
#include "WallpaperEngine/Data/Parsers/ProjectParser.h"
#include "WallpaperEngine/Data/Parsers/WallpaperParser.h"
#include "WallpaperEngine/FileSystem/Container.h"

#include <tuple>

using namespace WallpaperEngine;
using Data::JSON::JSON;

namespace {
Assets::AssetLocatorUniquePtr emptyAssets () {
    return std::make_unique<Assets::AssetLocator> (std::make_unique<FileSystem::Container> ());
}
}

// the input is read as every kind of JSON file the loader knows: project, scene, object, material
extern "C" int LLVMFuzzerTestOneInput (const uint8_t* data, size_t size) {
    JSON json;

    try {
	json = JSON::parseAsset (std::string (reinterpret_cast<const char*> (data), size));
    } catch (const std::exception&) {
	return 0;
    }

    Data::Model::Project project {};
    project.assetLocator = emptyAssets ();

    try {
	std::ignore = Data::Parsers::ProjectParser::parse (json, emptyAssets ());
    } catch (const std::exception&) { }

    try {
	std::ignore = Data::Parsers::ObjectParser::parse (json, project);
    } catch (const std::exception&) { }

    try {
	std::ignore = Data::Parsers::WallpaperParser::parse (json, project);
    } catch (const std::exception&) { }

    try {
	std::ignore = Data::Parsers::MaterialParser::parse (json, "fuzz.json", project);
    } catch (const std::exception&) { }

    return 0;
}
