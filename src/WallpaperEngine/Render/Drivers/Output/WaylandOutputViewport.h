#pragma once

#ifdef ENABLE_WAYLAND

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glew.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <wayland-egl.h>

#include <chrono>

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
struct xdg_surface;
struct xdg_toplevel;
#ifdef ENABLE_KDE_FEATURES
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
	uint32_t waylandName;
	int scale = 1;
	bool initialized = false;
	bool rendering = false;
	/** layer surface closed or output removed, set during event dispatch and freed by the driver's main loop */
	bool closed = false;

	wl_egl_window* eglWindow = nullptr;
	EGLSurface eglSurface = nullptr;
	wl_surface* surface = nullptr;
	zwlr_layer_surface_v1* layerSurface = nullptr;
	/** GNOME: toplevel the Shell extension turns into a desktop window */
	xdg_surface* xdgSurface = nullptr;
	xdg_toplevel* xdgToplevel = nullptr;
	/** 0 = ours to pick */
	glm::ivec2 pendingToplevelSize = {};
	bool toplevelConfigured = false;
	wl_callback* frameCallback = nullptr;
#ifdef ENABLE_KDE_FEATURES
	org_kde_plasma_surface* plasmaSurface = nullptr;
#endif
	glm::dvec2 mousePos = { 0, 0 };
	WallpaperEngine::Input::MouseClickStatus leftClick = WallpaperEngine::Input::MouseClickStatus::Released;
	WallpaperEngine::Input::MouseClickStatus rightClick = WallpaperEngine::Input::MouseClickStatus::Released;
	/** empty input region, clicks go to the desktop below */
	bool inputPassthrough = false;
	wl_cursor* pointer = nullptr;
	wl_surface* cursorSurface = nullptr;
	wl_cursor_theme* cursorTheme = nullptr;
	bool callbackInitialized = false;
	/** Last time a frame was handed to the compositor, drives the missed frame callback watchdog */
	std::chrono::steady_clock::time_point lastSwap = {};
	bool hasXdgLogicalPosition = false;
	zxdg_output_v1* xdgOutput = nullptr;

	/** layer surface or toplevel */
	[[nodiscard]] bool hasShellSurface () const { return this->layerSurface || this->xdgToplevel; }
	void setupLS ();
	void setupToplevel ();
	void setupXdgOutput (zxdg_output_manager_v1* manager);
	/** Starts following whether the output runs in HDR (only with --hdr and a compositor that can take PQ) */
	void setupColorManagement ();
	/** Asks for the output's current image description again, its info events decide outputHDR */
	void queryOutputDescription ();
	/** Tags the surface as PQ while the output runs in HDR, plain sRGB otherwise */
	void applyImageDescription ();
	[[nodiscard]] bool isOutputHDR () const { return this->outputHDR; }
	[[nodiscard]] bool isHDR () const override;
	void setWantsPointerButtons (bool wants) override;
	[[nodiscard]] glm::vec2 getHDRLuminance () const override { return this->outputLuminance; }

	wp_color_management_output_v1* colorOutput = nullptr;
	wp_color_management_surface_v1* colorSurface = nullptr;
	bool outputHDR = false;
	/** reference white and peak of the output's image description in nits */
	glm::vec2 outputLuminance = glm::vec2 (0.0f);
	/** Filled by the image description info events of the query in flight */
	struct {
	    uint32_t tf = 0;
	    uint32_t maxLuminance = 0;
	    uint32_t referenceLuminance = 0;
	    uint32_t targetMaxLuminance = 0;
	} pendingDescription;

	void makeCurrent () override;
	void renderFrame ();
	void swapOutput () override;
	void resize ();

    private:
	void prepareSurface ();
	void setInputRegion (bool everything);
	void createEGLSurface ();

	WaylandOpenGLDriver* m_driver = nullptr;
    };
} // namespace Output
} // namespace WallpaperEngine::Render::Drivers
#endif /* ENABLE_WAYLAND */
