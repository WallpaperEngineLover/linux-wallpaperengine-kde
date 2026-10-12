#pragma once

#ifdef ENABLE_WAYLAND

#include "FullScreenDetector.h"

namespace WallpaperEngine::Render::Drivers {
class WaylandOpenGLDriver;
}

namespace WallpaperEngine::Render::Drivers::Detectors {
/** GNOME Wayland: window states come from the Shell extension, paused per monitor like WE */
class GnomeFullScreenDetector final : public FullScreenDetector {
public:
    GnomeFullScreenDetector (Application::ApplicationContext& appContext, WaylandOpenGLDriver& driver);

    [[nodiscard]] bool anythingFullscreen () const override;

private:
    WaylandOpenGLDriver& m_driver;
};
} // namespace WallpaperEngine::Render::Drivers::Detectors

#endif /* ENABLE_WAYLAND */
