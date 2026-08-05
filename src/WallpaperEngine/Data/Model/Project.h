#pragma once

#include <memory>
#include <string>

#include "Types.h"
#include "WallpaperEngine/Assets/AssetLocator.h"

namespace WallpaperEngine::Data::Model {
using namespace WallpaperEngine::Assets;
struct Project {
    enum Type { Type_Scene = 0, Type_Web = 1, Type_Video = 2, Type_Unknown = 3 };

    std::string title;
    Type type;
    /** Negative if not present */
    std::string workshopId;
    bool supportsAudioProcessing;
    /** User-configurable properties exposed by the project */
    Properties properties;
    WallpaperUniquePtr wallpaper;
    AssetLocatorUniquePtr assetLocator;
};
};
