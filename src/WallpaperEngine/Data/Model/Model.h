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
    /** Marked GPU-instanced but not actually batched - each object still renders individually with
     *  its own transform/color, visually equivalent to batching but not a single draw call. */
    bool instanced;
    bool fullscreen;
    bool passthrough;
    bool autosize;
    /** Composition layer covering the whole project, autosize makes it the scene's size */
    bool projectlayer;
    bool nopadding;
    /** Not sure what's used for */
    std::optional<int> width;
    /** Not sure what's used for */
    std::optional<int> height;
    /** Offset of the autosize canvas center from the puppet's actual content pivot - needed when the
     *  rig doesn't sit in the middle of its bounding box (e.g. an arm attached at the wrist). */
    std::optional<glm::vec2> cropOffset;
    std::optional<std::string> puppet;
};
} // namespace WallpaperEngine::Data::Model