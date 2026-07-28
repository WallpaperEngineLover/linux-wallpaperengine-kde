#pragma once

#ifdef ENABLE_WAYLAND

#include "WallpaperEngine/Input/MouseInput.h"

#include <chrono>
#include <glm/vec2.hpp>
#include <optional>

#ifdef ENABLE_KDE_EXPERIMENTAL_FEATURES
#include "KDECursorInput.h"
#endif /* ENABLE_KDE_EXPERIMENTAL_FEATURES */

namespace WallpaperEngine::Render::Drivers {
namespace Output {
    class WaylandOutputViewport;
}
class WaylandOpenGLDriver;
};

namespace WallpaperEngine::Input::Drivers {
/**
 * Handles mouse input for the background
 */
class WaylandMouseInput final : public MouseInput {
public:
    explicit WaylandMouseInput (const WallpaperEngine::Render::Drivers::WaylandOpenGLDriver& driver);

    /**
     * Takes current mouse position and updates it
     */
    void update () override;

    /**
     * The virtual pointer's position
     */
    [[nodiscard]] glm::dvec2 position () const override;

    /**
     * @return The status of the mouse's left click
     */
    [[nodiscard]] MouseClickStatus leftClick () const override;

    /**
     * @return The status of the mouse's right click
     */
    [[nodiscard]] MouseClickStatus rightClick () const override;

private:
    [[nodiscard]] const Render::Drivers::Output::WaylandOutputViewport* getActiveOutputViewport () const;
    [[nodiscard]] std::optional<glm::dvec2> queryHyprlandCursorPosition () const;
#ifdef ENABLE_X11
    /**
     * Fallback for compositors without a compositor-specific IPC (KDE, GNOME, ...): asks XWayland
     * for the pointer position on the root window, which tracks the real Wayland cursor
     */
    [[nodiscard]] std::optional<glm::dvec2> queryX11CursorPosition () const;
#endif /* ENABLE_X11 */
    /**
     * Converts a global (compositor-space) cursor position into local viewport coordinates and
     * stores it into m_pos if it falls within one of the tracked outputs
     */
    bool matchViewport (const glm::dvec2& globalCursor, const char* source, bool shouldLog);

    /**
     * Wayland: Driver
     */
    const WallpaperEngine::Render::Drivers::WaylandOpenGLDriver& m_waylandDriver;

    glm::dvec2 m_pos = {};
    std::chrono::steady_clock::time_point m_lastGlobalCursorQuery = {};

#ifdef ENABLE_KDE_EXPERIMENTAL_FEATURES
    KDECursorInput m_kdeCursor;
#endif /* ENABLE_KDE_EXPERIMENTAL_FEATURES */
};
} // namespace WallpaperEngine::Input::Drivers

#endif /* ENABLE_WAYLAND */
