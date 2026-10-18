#pragma once

#include <optional>
#include <string>

#include <glm/vec2.hpp>

#include "Types.h"

namespace WallpaperEngine::Data::Model {
// TODO: FIND A BETTER NAMING SO THIS DOESN'T COLLIDE WITH THE NAMESPACE ITSELF
struct ModelStruct {
    std::string filename;
    MaterialUniquePtr material;
    bool solidlayer;
    // not actually batched, each object still draws on its own
    bool instanced;
    bool fullscreen;
    bool passthrough;
    bool autosize;
    bool projectlayer;
    bool nopadding;
    std::optional<int> width;
    std::optional<int> height;
    // autosize canvas center offset from the puppet's content pivot (rigs off-center in their bounds)
    std::optional<glm::vec2> cropOffset;
    std::optional<std::string> puppet;
};
}