#pragma once

#ifdef ENABLE_WAYLAND

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glew.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <wayland-egl.h>

#include "../WaylandOpenGLDriver.h"
#include "OutputViewport.h"
#include <WallpaperEngine/Input/MouseInput.h>
#include <glm/vec2.hpp>

struct zwlr_layer_shell_v1;
struct zwlr_layer_surface_v1;
struct zxdg_output_v1;
struct zxdg_output_manager_v1;
struct wp_color_management_output_v1;
struct wp_color_management_surface_v1;
#ifdef ENABLE_KDE_EXPERIMENTAL_FEATURES
struct org_kde_plasma_surface;
#endif

namespace WallpaperEngine::Render::Drivers {
class WaylandOpenGLDriver;

namespace Output {
    class OutputViewport;

    class WaylandOutputViewport final : public OutputViewport {
    public:
	WaylandOutputViewport (WaylandOpenGLDriver* driver, uint32_t waylandName, struct wl_registry* registry);

	WaylandOpenGLDriver* getDriver () const;

	wl_output* output = nullptr;
	glm::ivec2 size = {};
	glm::ivec2 position = {};
	uint32_t waylandName;
	int scale = 1;
	bool initialized = false;
	bool rendering = false;

	wl_egl_window* eglWindow = nullptr;
	EGLSurface eglSurface = nullptr;
	wl_surface* surface = nullptr;
	zwlr_layer_surface_v1* layerSurface = nullptr;
	wl_callback* frameCallback = nullptr;
#ifdef ENABLE_KDE_EXPERIMENTAL_FEATURES
	org_kde_plasma_surface* plasmaSurface = nullptr;
#endif
	glm::dvec2 mousePos = { 0, 0 };
	WallpaperEngine::Input::MouseClickStatus leftClick = WallpaperEngine::Input::MouseClickStatus::Released;
	WallpaperEngine::Input::MouseClickStatus rightClick = WallpaperEngine::Input::MouseClickStatus::Released;
	wl_cursor* pointer = nullptr;
	wl_surface* cursorSurface = nullptr;
	wl_cursor_theme* cursorTheme = nullptr;
	bool callbackInitialized = false;
	bool hasXdgLogicalPosition = false;
	zxdg_output_v1* xdgOutput = nullptr;

	void setupLS ();
	void setupXdgOutput (zxdg_output_manager_v1* manager);
	/** Starts following whether the output runs in HDR (only with --hdr and a compositor that can take PQ) */
	void setupColorManagement ();
	/** Asks for the output's current image description again, its info events decide outputHDR */
	void queryOutputDescription ();
	/** Tags the surface as PQ while the output runs in HDR, plain sRGB otherwise */
	void applyImageDescription ();
	[[nodiscard]] bool isOutputHDR () const { return this->outputHDR; }
	[[nodiscard]] bool isHDR () const override;

	wp_color_management_output_v1* colorOutput = nullptr;
	wp_color_management_surface_v1* colorSurface = nullptr;
	bool outputHDR = false;
	/** Filled by the image description info events of the query in flight */
	struct {
	    uint32_t tf = 0;
	    uint32_t maxLuminance = 0;
	    uint32_t referenceLuminance = 0;
	    uint32_t targetMaxLuminance = 0;
	} pendingDescription;

	void makeCurrent () override;
	void swapOutput () override;
	void resize ();

    private:
	WaylandOpenGLDriver* m_driver = nullptr;
    };
} // namespace Output
} // namespace WallpaperEngine::Render::Drivers
#endif /* ENABLE_WAYLAND */
