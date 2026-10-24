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
struct wp_fractional_scale_v1;
struct wp_viewport;
struct ext_image_capture_source_v1;
struct ext_image_copy_capture_session_v1;
struct ext_image_copy_capture_cursor_session_v1;
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
	/** wp_fractional_scale_v1 preferred scale in 120ths, 0 until the compositor sent one */
	uint32_t preferredScale120 = 0;
	wp_viewport* surfaceViewport = nullptr;
	wp_fractional_scale_v1* fractionalScale = nullptr;
	bool initialized = false;
	bool rendering = false;
	/** layer surface closed or output removed, set during event dispatch and freed by the driver's main loop */
	bool closed = false;
	bool outputRemoved = false;
	/** reopen backoff after a closed shell surface, doubled per close */
	std::chrono::seconds reopenDelay { 0 };
	std::chrono::steady_clock::time_point reopenAt = {};
	bool reopenAtMaxLogged = false;

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
	bool pointerEntered = false;
	WallpaperEngine::Input::MouseClickStatus leftClick = WallpaperEngine::Input::MouseClickStatus::Released;
	WallpaperEngine::Input::MouseClickStatus rightClick = WallpaperEngine::Input::MouseClickStatus::Released;
	bool inputPassthrough = false;
	wl_cursor* pointer = nullptr;
	wl_surface* cursorSurface = nullptr;
	wl_cursor_theme* cursorTheme = nullptr;
	bool callbackInitialized = false;
	std::chrono::steady_clock::time_point lastSwap = {};
	bool hasXdgLogicalPosition = false;
	zxdg_output_v1* xdgOutput = nullptr;

	ext_image_capture_source_v1* captureSource = nullptr;
	ext_image_copy_capture_session_v1* captureSession = nullptr;
	ext_image_copy_capture_cursor_session_v1* cursorSession = nullptr;
	glm::ivec2 captureSize = {};
	glm::ivec2 captureCursor = {};
	bool captureCursorInside = false;
	void setupCursorCapture ();

	[[nodiscard]] bool hasShellSurface () const { return this->layerSurface || this->xdgToplevel; }
	void setupLS ();
	void setupToplevel ();
	void setupXdgOutput (zxdg_output_manager_v1* manager);
	void setupColorManagement ();
	void queryOutputDescription ();
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
	struct {
	    uint32_t tf = 0;
	    uint32_t maxLuminance = 0;
	    uint32_t referenceLuminance = 0;
	    uint32_t targetMaxLuminance = 0;
	} pendingDescription;

	[[nodiscard]] double renderScale () const;
	[[nodiscard]] glm::ivec2 bufferSize () const;
	void updateBufferSize ();

	void makeCurrent () override;
	void renderFrame ();
	void swapOutput () override;
	void resize ();

    private:
	void prepareSurface ();
	void setInputRegion (bool everything);
	void setupFractionalScale ();
	void createEGLSurface ();

	WaylandOpenGLDriver* m_driver = nullptr;
    };
}
}
#endif
