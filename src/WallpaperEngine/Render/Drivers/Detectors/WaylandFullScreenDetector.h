#pragma once

#ifdef ENABLE_WAYLAND

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "FullScreenDetector.h"

struct wl_display;
struct wl_output;
struct wl_registry;
struct zwlr_foreign_toplevel_manager_v1;
struct zwlr_foreign_toplevel_handle_v1;

namespace WallpaperEngine::Render::Drivers {
class WaylandOpenGLDriver;
}

namespace WallpaperEngine::Desktop {
struct WindowState;
}

namespace WallpaperEngine::Render::Drivers::Detectors {

// wlroots foreign toplevel: fullscreen (and with --pause-on-maximized maximized) windows pause their outputs, toplevels
// on unknown outputs count everywhere
class WaylandFullScreenDetector final : public FullScreenDetector {
public:
    struct Output {
	wl_output* output;
	uint32_t globalName;
	std::string name;
    };

    struct Toplevel {
	WaylandFullScreenDetector* detector;
	zwlr_foreign_toplevel_handle_v1* handle;
	std::string appId;
	std::set<wl_output*> outputs;
	bool fullscreen = false;
	bool maximized = false;
	bool activated = false;
    };

    WaylandFullScreenDetector (Application::ApplicationContext& appContext, WaylandOpenGLDriver* driver);
    ~WaylandFullScreenDetector () override;

    [[nodiscard]] bool anythingFullscreen () const override;
    void reset () override;

    /** what the pause decision sees, one entry per output a toplevel is on */
    [[nodiscard]] std::vector<Desktop::WindowState> windows (const std::vector<std::string>& drawn) const;

    void addOutput (wl_registry* registry, uint32_t globalName, uint32_t version);
    void removeGlobal (uint32_t globalName);
    void addToplevel (zwlr_foreign_toplevel_handle_v1* handle);
    void removeToplevel (Toplevel* toplevel);
    [[nodiscard]] const std::string* outputName (wl_output* output) const;

    uint32_t m_managerGlobal = 0;
    uint32_t m_managerVersion = 0;

private:
    WaylandOpenGLDriver* m_driver;
    wl_display* m_display = nullptr;
    wl_registry* m_registry = nullptr;
    zwlr_foreign_toplevel_manager_v1* m_toplevelManager = nullptr;
    std::vector<Output*> m_outputs;
    std::vector<Toplevel*> m_toplevels;
};
} // namespace WallpaperEngine::Render::Drivers::Detectors

#endif /* ENABLE_WAYLAND */
