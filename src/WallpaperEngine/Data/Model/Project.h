#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "ImageAdjustments.h"
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
    Properties properties;
    WallpaperUniquePtr wallpaper;
    AssetLocatorUniquePtr assetLocator;
    ImageAdjustments imageAdjustments;
    /** Scene file WE opens (the base wallpaper's for a preset), keys script localStorage */
    std::filesystem::path sceneFile;
    /** scene.json "version", 0 when missing */
    int sceneVersion = 0;
};
};
