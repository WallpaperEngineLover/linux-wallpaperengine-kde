#pragma once

#include "WallpaperEngine/Data/JSON.h"
#include "WallpaperEngine/Data/Model/PropertyAnimation.h"
#include "WallpaperEngine/Data/Model/Types.h"

namespace WallpaperEngine::Data::Parsers {
using json = WallpaperEngine::Data::JSON::JSON;
using namespace WallpaperEngine::Data::Model;

class DynamicValueParser {
public:
    static Model::DynamicValueUniquePtr parse (const json& data, const Properties& properties, bool expectColor);
    /** One "cN" keyframe array like wallpaper64.exe's sub_1401A8CE0: whole frames, strictly increasing */
    static std::vector<Model::AnimationKeyframe> parseKeyframes (const json& curve);
    /** "wraploop": the curve ends in a copy of its first key at frame frameCount (sub_1401A98B0) */
    static void closeLoop (std::vector<Model::AnimationKeyframe>& keys, int frameCount);
};
}