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
#endif

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
    void drain () override;
    void suspend (bool suspended) override;

    [[nodiscard]] glm::dvec2 position () const override;
    [[nodiscard]] MouseClickStatus leftClick () const override;
    [[nodiscard]] MouseClickStatus rightClick () const override;
    [[nodiscard]] std::optional<glm::dvec2> desktopPosition () const override;
    [[nodiscard]] MouseClickStatus desktopLeftClick () const override;
    [[nodiscard]] bool hasGlobalCursor () const;

private:
    [[nodiscard]] const Render::Drivers::Output::WaylandOutputViewport* getActiveOutputViewport () const;
    [[nodiscard]] std::optional<glm::dvec2> queryHyprlandCursorPosition ();
    [[nodiscard]] static std::string findHyprlandSocket ();
#ifdef ENABLE_X11
    // last resort: XWayland's root pointer only tracks the cursor over XWayland windows
    [[nodiscard]] std::optional<glm::dvec2> queryX11CursorPosition () const;
#endif
    /** the pointer in that output's buffer pixels, y up */
    [[nodiscard]] glm::dvec2 positionOn (const Render::Drivers::Output::WaylandOutputViewport& viewport) const;
    void pollForwardedInput ();

    const WallpaperEngine::Render::Drivers::WaylandOpenGLDriver& m_waylandDriver;

    std::optional<glm::dvec2> m_globalCursor;
    bool m_globalCursorFromX11 = false;
    std::chrono::steady_clock::time_point m_lastGlobalCursorQuery = {};
    std::chrono::steady_clock::time_point m_lastForwardedPoll = {};
    bool m_forwardedLeftDown = false;
    std::string m_hyprlandSocket;
    std::chrono::steady_clock::time_point m_lastHyprlandLookup = {};

#ifdef ENABLE_KDE_FEATURES
    std::unique_ptr<KDECursorInput> m_kdeCursor;
#endif
};
}

#endif
