#pragma once

#include "WallpaperEngine/Input/MouseInput.h"

#include <glm/vec2.hpp>

namespace WallpaperEngine::Render::Drivers {
class GLFWOpenGLDriver;
}

namespace WallpaperEngine::Input::Drivers {
class GLFWMouseInput final : public MouseInput {
public:
    explicit GLFWMouseInput (const Render::Drivers::GLFWOpenGLDriver& driver);

    void update () override;

    [[nodiscard]] glm::dvec2 position () const override;
    [[nodiscard]] MouseClickStatus leftClick () const override;
    [[nodiscard]] MouseClickStatus rightClick () const override;

private:
    const Render::Drivers::GLFWOpenGLDriver& m_driver;

    glm::dvec2 m_mousePosition = {};
    glm::dvec2 m_reportedPosition = {};
    MouseClickStatus m_leftClick = Released;
    MouseClickStatus m_rightClick = Released;
};
} // namespace WallpaperEngine::Input::Drivers
