#pragma once

#ifdef ENABLE_WAYLAND

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glew.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <wayland-egl.h>

#include "WallpaperEngine/Application/ApplicationContext.h"
#include "WallpaperEngine/Application/WallpaperApplication.h"
#include "WallpaperEngine/Desktop/GnomeShell.h"
#include "WallpaperEngine/Input/Drivers/WaylandMouseInput.h"
#include "WallpaperEngine/Render/Drivers/Detectors/WaylandFullScreenDetector.h"
#include "WallpaperEngine/Render/Drivers/Output/WaylandOutput.h"
#include "WallpaperEngine/Render/Drivers/Output/WaylandOutputViewport.h"
#include "WallpaperEngine/Render/Drivers/VideoDriver.h"

namespace WallpaperEngine::Application {
class ApplicationContext;
class WallpaperApplication;
}

struct zwlr_layer_shell_v1;
struct zwlr_layer_surface_v1;
struct zxdg_output_manager_v1;
struct xdg_wm_base;
struct wp_color_manager_v1;
struct wp_image_description_v1;
struct wp_viewporter;
struct wp_fractional_scale_manager_v1;
struct ext_output_image_capture_source_manager_v1;
struct ext_image_copy_capture_manager_v1;
#ifdef ENABLE_KDE_FEATURES
struct org_kde_plasma_shell;
#endif

namespace WallpaperEngine::Render::Drivers {
using namespace WallpaperEngine::Application;
using namespace WallpaperEngine::Input::Drivers;
class WaylandOpenGLDriver;

namespace Output {
    class WaylandOutputViewport;
    class WaylandOutput;
}

class WaylandOpenGLDriver final : public VideoDriver {
    friend class Output::WaylandOutput;
    friend class CWaylandMouseInput;

public:
    struct SEGLContext {
	EGLDisplay display = nullptr;
	EGLConfig config = nullptr;
	EGLContext context = nullptr;
	PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC eglCreatePlatformWindowSurfaceEXT = nullptr;
    };

    struct WaylandContext {
	wl_display* display = nullptr;
	wl_registry* registry = nullptr;
	wl_compositor* compositor = nullptr;
	wl_shm* shm = nullptr;
	zwlr_layer_shell_v1* layerShell = nullptr;
	xdg_wm_base* xdgWmBase = nullptr;
	wl_seat* seat = nullptr;
	wl_pointer* pointer = nullptr;
	zxdg_output_manager_v1* xdgOutputManager = nullptr;
	ext_output_image_capture_source_manager_v1* captureSourceManager = nullptr;
	ext_image_copy_capture_manager_v1* copyCaptureManager = nullptr;
	wp_color_manager_v1* colorManager = nullptr;
	wp_viewporter* viewporter = nullptr;
	wp_fractional_scale_manager_v1* fractionalScaleManager = nullptr;
#ifdef ENABLE_KDE_FEATURES
	org_kde_plasma_shell* plasmaShell = nullptr;
#endif
    };

    struct ColorSupport {
	bool parametric = false;
	bool pq = false;
	bool bt2020 = false;
    };

    explicit WaylandOpenGLDriver (ApplicationContext& context, WallpaperApplication& app);
    ~WaylandOpenGLDriver ();

    [[nodiscard]] Output::Output& getOutput () override;
    float getRenderTime () const override;
    bool closeRequested () override;
    void resizeWindow (glm::ivec2 size) override;
    void resizeWindow (glm::ivec4 sizeandpos) override;
    void showWindow () override;
    void hideWindow () override;
    glm::ivec2 getFramebufferSize () const override;
    uint32_t getFrameCounter () const override;
    void dispatchEventQueue () override;
    void dispatchWhilePaused () override;
    /** frame callbacks don't draw while dispatchWhilePaused runs */
    [[nodiscard]] bool isDispatchingWhilePaused () const { return this->m_dispatchingWhilePaused; }
    [[nodiscard]] void* getProcAddress (const char* name) const override;
    [[nodiscard]] void* getWaylandDisplay () const override;
    [[nodiscard]] bool isHDRAvailable () const override;
    /** PQ with BT.2020 primaries (and the PQ default luminances, 203 nits reference white), shared by every HDR surface
     */
    [[nodiscard]] wp_image_description_v1* getHDRDescription () const;

    void releaseSurface (Output::WaylandOutputViewport*);
    void removeOutput (Output::WaylandOutputViewport*);
    void onSurfaceClosed (Output::WaylandOutputViewport*);
    void makeSurfacelessCurrent ();
    [[nodiscard]] bool hasGlobalCursor () const { return this->m_mouseInput.hasGlobalCursor (); }
    [[nodiscard]] Desktop::GnomeShell* getGnomeShell () const { return this->m_gnomeShell.get (); }
    [[nodiscard]] bool hasCapturedCursor () const;
    [[nodiscard]] std::optional<glm::dvec2> capturedCursorPosition () const;
    /** runs outside event dispatch */
    void reapClosedOutputs ();
    Output::WaylandOutputViewport* surfaceToViewport (const wl_surface*) const;
    [[nodiscard]] std::vector<std::string> drawnOutputs () const;

    Output::WaylandOutputViewport* viewportInFocus = nullptr;

    [[nodiscard]] SEGLContext* getEGLContext ();
    [[nodiscard]] WaylandContext* getWaylandContext ();

    std::vector<Output::WaylandOutputViewport*> m_screens = {};

    ColorSupport colorSupport = {};
    bool hdrDescriptionReady = false;

private:
    Output::WaylandOutput m_output;
    SEGLContext m_eglContext = {};
    WaylandContext m_waylandContext = {};
    mutable bool m_requestedExit;

    void initWaylandRegistry ();
    void initGnomeShell ();
    void setupOutputLayerSurfaces ();
    void setupLateOutputs ();
    void initEGL ();
    void initGLEW ();
    void finishEGL () const;
    void initColorManagement ();

    wp_image_description_v1* m_hdrDescription = nullptr;

    uint32_t m_frameCounter = 0;
    bool m_layerSurfacesReady = false;
    bool m_dispatchingWhilePaused = false;
    bool m_waitingLogged = false;
    ApplicationContext& m_context;
    std::unique_ptr<Desktop::GnomeShell> m_gnomeShell;
    bool m_cursorCapture = false;
    WaylandMouseInput m_mouseInput;

    std::chrono::high_resolution_clock::time_point renderStart = std::chrono::high_resolution_clock::now ();
};
}
#endif