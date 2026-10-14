#pragma once

#ifdef ENABLE_WAYLAND

#include "WallpaperEngine/Input/MouseInput.h"

#include <chrono>
#include <glm/vec2.hpp>
#include <memory>
#include <optional>
#include <string>

#ifdef ENABLE_KDE_FEATURES
#include "KDECursorInput.h"
#endif /* ENABLE_KDE_FEATURES */

namespace WallpaperEngine::Render::Drivers {
namespace Output {
    class WaylandOutputViewport;
}
class WaylandOpenGLDriver;
};

namespace WallpaperEngine::Input::Drivers {
class WaylandMouseInput final : public MouseInput {
public:
    explicit WaylandMouseInput (const WallpaperEngine::Render::Drivers::WaylandOpenGLDriver& driver);

    void update () override;

    [[nodiscard]] glm::dvec2 position () const override;
    [[nodiscard]] MouseClickStatus leftClick () const override;
    [[nodiscard]] MouseClickStatus rightClick () const override;
    [[nodiscard]] std::optional<glm::dvec2> desktopPosition () const override;
    [[nodiscard]] MouseClickStatus desktopLeftClick () const override;
    /** KWin script, Hyprland or the GNOME Shell extension provide it */
    [[nodiscard]] bool hasGlobalCursor () const;

private:
    [[nodiscard]] const Render::Drivers::Output::WaylandOutputViewport* getActiveOutputViewport () const;
    [[nodiscard]] std::optional<glm::dvec2> queryHyprlandCursorPosition ();
    [[nodiscard]] static std::string findHyprlandSocket ();
#ifdef ENABLE_X11
    // last resort: XWayland's root pointer only tracks the cursor over XWayland windows
    [[nodiscard]] std::optional<glm::dvec2> queryX11CursorPosition () const;
#endif /* ENABLE_X11 */
    /** the pointer in that output's buffer pixels, y up */
    [[nodiscard]] glm::dvec2 positionOn (const Render::Drivers::Output::WaylandOutputViewport& viewport) const;
    void pollForwardedInput ();

    const WallpaperEngine::Render::Drivers::WaylandOpenGLDriver& m_waylandDriver;

    /** logical global position */
    std::optional<glm::dvec2> m_globalCursor;
    /** XWayland's position goes stale off X11 windows */
    bool m_globalCursorFromX11 = false;
    std::chrono::steady_clock::time_point m_lastGlobalCursorQuery = {};
    std::chrono::steady_clock::time_point m_lastForwardedPoll = {};
    bool m_forwardedLeftDown = false;
    /** empty when not on Hyprland, looked up again (at most once a second) when it goes away */
    std::string m_hyprlandSocket;
    std::chrono::steady_clock::time_point m_lastHyprlandLookup = {};

#ifdef ENABLE_KDE_FEATURES
    std::unique_ptr<KDECursorInput> m_kdeCursor;
#endif /* ENABLE_KDE_FEATURES */
};
} // namespace WallpaperEngine::Input::Drivers

#endif /* ENABLE_WAYLAND */
