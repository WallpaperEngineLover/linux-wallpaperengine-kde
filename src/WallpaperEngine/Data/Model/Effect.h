#pragma once

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <optional>
#include <string>
#include <vector>

#include "Types.h"

namespace WallpaperEngine::Data::Model {
enum PassCommandType { Command_Copy = 0, Command_Swap = 1 };

/** effect.json "conditions" entry (sub_1401E63B0): a combo against a number or {"value", "op"} */
struct EffectCondition {
    enum Operator { Equal, GreaterEqual, Greater, LessEqual, Less };

    std::string combo;
    int value = 0;
    Operator op = Equal;
};

struct EffectConditions {
    std::vector<EffectCondition> tests;

    [[nodiscard]] bool holds (const ComboMap& combos) const;
};

struct FBO {
    std::string name;
    std::string format;
    /** an int from 1 to 255, anything else is 1 */
    float scale;
    bool unique;
    /** "clear": up to four numbers, what the buffer is cleared to when made and by a clear function */
    glm::vec4 clearColor = {};
    /** "clear" was empty or had all four numbers (wallpaper64.exe 2.8.42 sub_1401E7170, FBO flag 2) */
    bool clearOnCreate = false;
    /** width/height replace the object's size, fit shrinks the longer side to it (sub_1401EA500) */
    std::optional<uint32_t> width = std::nullopt;
    std::optional<uint32_t> height = std::nullopt;
    std::optional<uint32_t> fit = std::nullopt;
    /** "uvs": "repeat" */
    bool repeat = false;
    EffectConditions conditions = {};

    /** width/height/fit, divided by the scale, at least 2 */
    [[nodiscard]] glm::uvec2 bufferSize (glm::vec2 objectSize) const;
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
    /** a pass failing its conditions keeps its index for pass overrides */
    EffectConditions conditions;
    std::optional<MaterialUniquePtr> material;
    TextureMap binds;
    std::optional<PassCommandType> command;
    std::optional<std::string> source;
    std::optional<std::string> target;
    /** counted into the layer's buffer passes (sub_1401E7170) */
    bool compose = false;
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