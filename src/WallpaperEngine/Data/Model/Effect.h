#pragma once

#include <glm/vec4.hpp>
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
    /** "clear": up to four numbers, what the buffer is cleared to when made and by a clear function */
    glm::vec4 clearColor = {};
    /** "clear" was empty or had all four numbers (wallpaper64.exe 2.8.42 sub_1401E7170, FBO flag 2) */
    bool clearOnCreate = false;
};

/**
 * An effect.json "functions" entry, run by IEffect.executeMaterialFunction. WE only keeps "clear" actions that
 * name at least one of the effect's buffers
 */
struct EffectFunction {
    std::string name;
    /** indices into the effect's fbos of the names listed, WE only uses how many there are */
    std::vector<int> fbos;
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
    std::vector<EffectFunction> functions;
};
} // namespace WallpaperEngine::Data::Model