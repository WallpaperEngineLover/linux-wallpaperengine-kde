#pragma once

#include "WallpaperEngine/Data/JSON.h"
#include "WallpaperEngine/Data/Model/Material.h"
#include "WallpaperEngine/Data/Model/Types.h"

namespace WallpaperEngine::Data::Parsers {
using JSON = WallpaperEngine::Data::JSON::JSON;
using namespace WallpaperEngine::Data::Model;

class MaterialParser {
public:
    static MaterialUniquePtr load (const Project& project, const std::string& filename);
    /** The textures the first pass names, without building the material: safe off the main thread, nothing is connected
     * to the project's properties */
    static std::vector<std::string> textureNames (const Project& project, const std::string& filename);
    static MaterialUniquePtr parse (const JSON& it, const std::string& filename, const Project& project);
    static MaterialPassUniquePtr parsePass (const JSON& it, const Project& project);
    static std::map<std::string, int> parseCombos (const JSON& it);
    static BlendingMode parseBlendMode (const std::string& mode);
    static CullingMode parseCullMode (const std::string& mode);
    static DepthtestMode parseDepthtestMode (const std::string& mode);
    static DepthwriteMode parseDepthwriteMode (const std::string& mode);
};
}
