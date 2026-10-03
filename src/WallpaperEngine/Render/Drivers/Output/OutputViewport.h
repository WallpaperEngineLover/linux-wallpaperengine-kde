#pragma once

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <string>

namespace WallpaperEngine::Render::Drivers::Output {
class OutputViewport {
public:
    OutputViewport (glm::ivec4 viewport, std::string name, bool single = false);
    virtual ~OutputViewport () = default;

    glm::ivec4 viewport;
    std::string name;
    /** Global position of this viewport in the combined desktop coordinate space */
    glm::ivec2 globalPosition = { 0, 0 };
    /** Logical (unscaled) size in the same coordinate space as globalPosition */
    glm::ivec2 logicalSize = { 0, 0 };

    /** Whether this viewport is single in the framebuffer or shares space with more viewports */
    bool single;

    virtual void makeCurrent () = 0;
    virtual void swapOutput () = 0;
    /** The surface is tagged as PQ with BT.2020 primaries, what gets drawn has to be encoded that way */
    [[nodiscard]] virtual bool isHDR () const { return false; }
    /** The output's reference white and peak in nits while it runs in HDR, zero when unknown */
    [[nodiscard]] virtual glm::vec2 getHDRLuminance () const { return glm::vec2 (0.0f); }
};
} // namespace WallpaperEngine::Render::Drivers::Output
