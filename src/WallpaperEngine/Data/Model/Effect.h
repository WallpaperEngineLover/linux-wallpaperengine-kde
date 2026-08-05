#pragma once

#include <optional>
#include <string>

#include "Types.h"

namespace WallpaperEngine::Data::Model {
enum PassCommandType { Command_Copy = 0, Command_Swap = 1 };

struct FBO {
    std::string name;
    std::string format;
    float scale;
    bool unique;
};

struct EffectPass {
    std::optional<MaterialUniquePtr> material;
    TextureMap binds;
    std::optional<PassCommandType> command;
    std::optional<std::string> source;
    std::optional<std::string> target;
};

struct Effect {
    /** For the UI */
    std::string name;
    /** For the UI */
    std::string description;
    /** For the UI */
    std::string group;
    std::string preview;
    std::vector<std::string> dependencies;
    std::vector<EffectPassUniquePtr> passes;
    std::vector<FBOUniquePtr> fbos;
};
} // namespace WallpaperEngine::Data::Model