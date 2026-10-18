#pragma once

#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include "WallpaperEngine/Application/ApplicationContext.h"

namespace WallpaperEngine::Render {
/**
 * Slot rects (x, y, w, h), first slot left or top on portrait. aspects: width / height, 0 for any size.
 * yUp: row 0 is the bottom of the screen
 */
[[nodiscard]] std::vector<glm::ivec4> splitSlotRects (
    const glm::ivec2& size, const std::vector<float>& aspects,
    Application::ApplicationContext::SplitScreen::Layout layout, bool yUp
);
}
