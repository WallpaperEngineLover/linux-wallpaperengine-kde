#pragma once

#include <glm/vec2.hpp>
#include <optional>

namespace WallpaperEngine::Input {
enum MouseClickStatus : int { Released = 0, Clicked = 1 };

// engine coordinates are GL y up, GLFW/Wayland/CEF are y down; drivers convert to GL, web wallpapers convert back for
// CEF
class MouseInput {
public:
    virtual ~MouseInput () = default;
    virtual void update () = 0;
    virtual void drain () { }
    virtual void suspend (bool suspended) { (void)suspended; }

    [[nodiscard]] virtual glm::dvec2 position () const = 0;
    [[nodiscard]] virtual MouseClickStatus leftClick () const = 0;
    [[nodiscard]] virtual MouseClickStatus rightClick () const = 0;
    /** The pointer in the desktop layout (OutputViewport::globalPosition units, y down) when the driver knows it */
    [[nodiscard]] virtual std::optional<glm::dvec2> desktopPosition () const { return std::nullopt; }
    [[nodiscard]] virtual MouseClickStatus desktopLeftClick () const { return this->leftClick (); }
};
}
