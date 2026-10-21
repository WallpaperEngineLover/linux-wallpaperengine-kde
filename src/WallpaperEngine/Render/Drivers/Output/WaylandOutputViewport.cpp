#include "WaylandOutputViewport.h"
#include "WallpaperEngine/Desktop/GnomeShell.h"
#include "WallpaperEngine/Logging/Log.h"

#include <cstdlib>
#include <unistd.h>

#define class _class
#define namespace _namespace
#define static
extern "C" {
#include "color-management-v1-protocol.h"
#include "fractional-scale-v1-protocol.h"
#include "viewporter-protocol.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"
#include "xdg-output-unstable-v1-protocol.h"
#include "xdg-shell-protocol.h"
#ifdef ENABLE_KDE_FEATURES
#include "plasma-shell-protocol.h"
#endif
}
#undef class
#undef namespace
#undef static

using namespace WallpaperEngine::Render::Drivers;
using namespace WallpaperEngine::Render::Drivers::Output;

static void handleLSConfigure (void* data, zwlr_layer_surface_v1* surface, uint32_t serial, uint32_t w, uint32_t h) {
    const auto viewport = static_cast<WaylandOutputViewport*> (data);
    viewport->size = { w, h };
    viewport->logicalSize = { w, h };
    viewport->updateBufferSize ();

    zwlr_layer_surface_v1_ack_configure (surface, serial);
}

static void handleLSClosed (void* data, zwlr_layer_surface_v1* surface) {
    const auto viewport = static_cast<WaylandOutputViewport*> (data);

    viewport->closed = true;
    viewport->rendering = false;
}

static void handleXdgSurfaceConfigure (void* data, xdg_surface* surface, uint32_t serial) {
    const auto viewport = static_cast<WaylandOutputViewport*> (data);
    const auto pending = viewport->pendingToplevelSize;

    xdg_surface_ack_configure (surface, serial);
    viewport->toplevelConfigured = true;

    if (pending.x <= 0 || pending.y <= 0 || pending == viewport->size) {
	return;
    }

    viewport->size = pending;
    viewport->logicalSize = pending;
    viewport->updateBufferSize ();
}

static void handleXdgToplevelConfigure (void* data, xdg_toplevel*, int32_t width, int32_t height, wl_array*) {
    static_cast<WaylandOutputViewport*> (data)->pendingToplevelSize = { width, height };
}

// ignored, a wallpaper closed by Alt+F4 (focusable on GNOME 45) would never come back
static void handleXdgToplevelClose (void*, xdg_toplevel*) { }

static void handleXdgToplevelBounds (void*, xdg_toplevel*, int32_t, int32_t) { }

static void handleXdgToplevelCapabilities (void*, xdg_toplevel*, wl_array*) { }

constexpr xdg_surface_listener xdgSurfaceListener = { .configure = handleXdgSurfaceConfigure };

constexpr xdg_toplevel_listener xdgToplevelListener = {
    .configure = handleXdgToplevelConfigure,
    .close = handleXdgToplevelClose,
    .configure_bounds = handleXdgToplevelBounds,
    .wm_capabilities = handleXdgToplevelCapabilities,
};

static void geometry (
    void* data, wl_output* output, int32_t x, int32_t y, int32_t width_mm, int32_t height_mm, int32_t subpixel,
    const char* make, const char* model, int32_t transform
) {
    const auto viewport = static_cast<WaylandOutputViewport*> (data);
    // only use geometry position as fallback if xdg-output hasn't provided one
    if (!viewport->hasXdgLogicalPosition) {
	viewport->globalPosition = { x, y };
    }
    sLog.debug ("SPAN DEBUG geometry: output '", viewport->name, "' position=(", x, ",", y, ") transform=", transform);
}

static void mode (void* data, wl_output* output, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    const auto viewport = static_cast<WaylandOutputViewport*> (data);

    // a mode change alone sends a toplevel no new configure
    if (viewport->xdgToplevel) {
	if (viewport->initialized) {
	    viewport->getDriver ()->getOutput ().reset ();
	}

	return;
    }

    // physical pixels, only a placeholder until the layer shell configure brings the logical size; a mode change
    // on a live surface comes with a new configure
    if (!viewport->hasShellSurface ()) {
	viewport->size = { width, height };
	viewport->viewport = { 0, 0, width, height };
    }

    if (viewport->initialized) {
	viewport->getDriver ()->getOutput ().reset ();
    }
}

static void done (void* data, wl_output* wl_output) { static_cast<WaylandOutputViewport*> (data)->initialized = true; }

static void scale (void* data, wl_output* wl_output, int32_t scale) {
    const auto viewport = static_cast<WaylandOutputViewport*> (data);

    viewport->scale = scale;

    if (viewport->hasShellSurface ()) {
	viewport->updateBufferSize ();
    }

    if (viewport->initialized) {
	viewport->getDriver ()->getOutput ().reset ();
    }
}

static void name (void* data, wl_output* wl_output, const char* name) {
    const auto viewport = static_cast<WaylandOutputViewport*> (data);

    if (name) {
	viewport->name = name;
    }

    viewport->getDriver ()->getOutput ().reset ();
}

static void description (void* data, wl_output* wl_output, const char* description) { }

static void surfaceFrameCallback (void* data, struct wl_callback* cb, uint32_t time) {
    const auto viewport = static_cast<WaylandOutputViewport*> (data);

    wl_callback_destroy (cb);

    viewport->frameCallback = nullptr;
    viewport->renderFrame ();
}

constexpr struct wl_callback_listener frameListener = { .done = surfaceFrameCallback };

static void handlePreferredScale (void* data, wp_fractional_scale_v1*, uint32_t scale) {
    const auto viewport = static_cast<WaylandOutputViewport*> (data);

    if (scale == viewport->preferredScale120) {
	return;
    }

    sLog.out ("Output ", viewport->name, ": drawing at scale ", scale / 120.0);
    viewport->preferredScale120 = scale;
    viewport->updateBufferSize ();
}

constexpr wp_fractional_scale_v1_listener fractionalScaleListener = { .preferred_scale = handlePreferredScale };

constexpr wl_output_listener outputListener
    = { .geometry = geometry, .mode = mode, .done = done, .scale = scale, .name = name, .description = description };

constexpr struct zwlr_layer_surface_v1_listener layerSurfaceListener = {
    .configure = handleLSConfigure,
    .closed = handleLSClosed,
};

static void xdgOutputLogicalPosition (void* data, struct zxdg_output_v1* xdg_output, int32_t x, int32_t y) {
    const auto viewport = static_cast<WaylandOutputViewport*> (data);
    viewport->globalPosition = { x, y };
    viewport->hasXdgLogicalPosition = true;
    sLog.debug ("SPAN DEBUG xdg-output logical_position: '", viewport->name, "' position=(", x, ",", y, ")");
}

static void xdgOutputLogicalSize (void* data, struct zxdg_output_v1* xdg_output, int32_t width, int32_t height) {
    const auto viewport = static_cast<WaylandOutputViewport*> (data);
    viewport->logicalSize = { width, height };
    if (viewport->initialized) {
	viewport->getDriver ()->getOutput ().reset ();
    }
}

static void xdgOutputDone (void* data, struct zxdg_output_v1* xdg_output) {
    // deprecated since xdg-output v3, compositor uses wl_output.done instead
}

static void xdgOutputName (void* data, struct zxdg_output_v1* xdg_output, const char* name) { }

static void xdgOutputDescription (void* data, struct zxdg_output_v1* xdg_output, const char* description) { }

constexpr struct zxdg_output_v1_listener xdgOutputListener = {
    .logical_position = xdgOutputLogicalPosition,
    .logical_size = xdgOutputLogicalSize,
    .done = xdgOutputDone,
    .name = xdgOutputName,
    .description = xdgOutputDescription,
};

WaylandOutputViewport::WaylandOutputViewport (
    WaylandOpenGLDriver* driver, uint32_t waylandName, struct wl_registry* registry
) : OutputViewport ({ 0, 0, 0, 0 }, "", true), size ({ 0, 0 }), waylandName (waylandName), m_driver (driver) {
    this->output = static_cast<wl_output*> (wl_registry_bind (registry, waylandName, &wl_output_interface, 4));
    wl_output_add_listener (output, &outputListener, this);
}

void WaylandOutputViewport::setupXdgOutput (zxdg_output_manager_v1* manager) {
    this->xdgOutput = zxdg_output_manager_v1_get_xdg_output (manager, this->output);
    zxdg_output_v1_add_listener (this->xdgOutput, &xdgOutputListener, this);
}

static void descriptionInfoDone (void* data, wp_image_description_info_v1* info) {
    auto* viewport = static_cast<WaylandOutputViewport*> (data);
    const auto& description = viewport->pendingDescription;
    const uint32_t peak = description.targetMaxLuminance ? description.targetMaxLuminance : description.maxLuminance;
    // KWin describes an HDR output as PQ; a plain SDR output peaks at about its reference white
    const bool hdr = description.tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ
	|| (description.referenceLuminance > 0 && peak * 2 > description.referenceLuminance * 3);

    wp_image_description_info_v1_destroy (info);
    viewport->outputLuminance = glm::vec2 (description.referenceLuminance, peak);

    if (hdr != viewport->outputHDR) {
	viewport->outputHDR = hdr;
	sLog.out ("Output ", viewport->name, hdr ? " switched to HDR, drawing PQ" : " switched to SDR");
	viewport->applyImageDescription ();
    }
}

static void descriptionInfoIcc (void*, wp_image_description_info_v1*, int32_t fd, uint32_t) { close (fd); }

static void descriptionInfoPrimaries (
    void*, wp_image_description_info_v1*, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t
) { }

static void descriptionInfoPrimariesNamed (void*, wp_image_description_info_v1*, uint32_t) { }

static void descriptionInfoTransferPower (void*, wp_image_description_info_v1*, uint32_t) { }

static void descriptionInfoTransferNamed (void* data, wp_image_description_info_v1*, uint32_t tf) {
    static_cast<WaylandOutputViewport*> (data)->pendingDescription.tf = tf;
}

static void
descriptionInfoLuminances (void* data, wp_image_description_info_v1*, uint32_t, uint32_t max, uint32_t reference) {
    auto& description = static_cast<WaylandOutputViewport*> (data)->pendingDescription;

    description.maxLuminance = max;
    description.referenceLuminance = reference;
}

static void descriptionInfoTargetLuminance (void* data, wp_image_description_info_v1*, uint32_t, uint32_t max) {
    static_cast<WaylandOutputViewport*> (data)->pendingDescription.targetMaxLuminance = max;
}

static void descriptionInfoTargetMaxCll (void*, wp_image_description_info_v1*, uint32_t) { }

static void descriptionInfoTargetMaxFall (void*, wp_image_description_info_v1*, uint32_t) { }

constexpr wp_image_description_info_v1_listener descriptionInfoListener = {
    .done = descriptionInfoDone,
    .icc_file = descriptionInfoIcc,
    .primaries = descriptionInfoPrimaries,
    .primaries_named = descriptionInfoPrimariesNamed,
    .tf_power = descriptionInfoTransferPower,
    .tf_named = descriptionInfoTransferNamed,
    .luminances = descriptionInfoLuminances,
    .target_primaries = descriptionInfoPrimaries,
    .target_luminance = descriptionInfoTargetLuminance,
    .target_max_cll = descriptionInfoTargetMaxCll,
    .target_max_fall = descriptionInfoTargetMaxFall,
};

static void outputDescriptionFailed (void* data, wp_image_description_v1* description, uint32_t, const char* message) {
    sLog.error ("Cannot tell whether ", static_cast<WaylandOutputViewport*> (data)->name, " runs in HDR: ", message);
    wp_image_description_v1_destroy (description);
}

static void outputDescriptionReady (void* data, wp_image_description_v1* description, uint32_t) {
    auto* viewport = static_cast<WaylandOutputViewport*> (data);

    viewport->pendingDescription = {};
    wp_image_description_info_v1_add_listener (
	wp_image_description_v1_get_information (description), &descriptionInfoListener, viewport
    );
    wp_image_description_v1_destroy (description);
}

constexpr wp_image_description_v1_listener outputDescriptionListener = {
    .failed = outputDescriptionFailed,
    .ready = outputDescriptionReady,
};

static void outputDescriptionChanged (void* data, wp_color_management_output_v1*) {
    static_cast<WaylandOutputViewport*> (data)->queryOutputDescription ();
}

constexpr wp_color_management_output_v1_listener colorOutputListener = {
    .image_description_changed = outputDescriptionChanged,
};

void WaylandOutputViewport::setupColorManagement () {
    if (this->colorOutput != nullptr || !m_driver->isHDRAvailable ()) {
	return;
    }

    this->colorOutput = wp_color_manager_v1_get_output (m_driver->getWaylandContext ()->colorManager, this->output);
    wp_color_management_output_v1_add_listener (this->colorOutput, &colorOutputListener, this);
    this->queryOutputDescription ();
}

void WaylandOutputViewport::queryOutputDescription () {
    wp_image_description_v1_add_listener (
	wp_color_management_output_v1_get_image_description (this->colorOutput), &outputDescriptionListener, this
    );
}

void WaylandOutputViewport::applyImageDescription () {
    if (this->colorSurface == nullptr) {
	return;
    }

    if (this->outputHDR) {
	wp_color_management_surface_v1_set_image_description (
	    this->colorSurface, m_driver->getHDRDescription (), WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL
	);
    } else {
	wp_color_management_surface_v1_unset_image_description (this->colorSurface);
    }
}

bool WaylandOutputViewport::isHDR () const { return this->colorSurface != nullptr && this->outputHDR; }

void WaylandOutputViewport::setupLS () {
    surface = wl_compositor_create_surface (m_driver->getWaylandContext ()->compositor);

    zwlr_layer_shell_v1_layer wlrLayer;
    switch (m_driver->getApp ().getContext ().settings.render.wayland.layer) {
	case WallpaperEngine::Application::ApplicationContext::WAYLAND_LAYER_BACKGROUND:
	    wlrLayer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
	    break;
	case WallpaperEngine::Application::ApplicationContext::WAYLAND_LAYER_TOP:
	    wlrLayer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
	    break;
	case WallpaperEngine::Application::ApplicationContext::WAYLAND_LAYER_OVERLAY:
	    wlrLayer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
	    break;
	case WallpaperEngine::Application::ApplicationContext::WAYLAND_LAYER_BOTTOM:
	default:
	    wlrLayer = ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
	    break;
    }

    // KWin treats the "desktop" namespace as a desktop window, so 'show desktop' leaves it alone
    layerSurface = zwlr_layer_shell_v1_get_layer_surface (
	m_driver->getWaylandContext ()->layerShell, surface, output, wlrLayer, "desktop"
    );

    if (!layerSurface) {
	sLog.exception ("Failed to get a layer surface");
    }

    zwlr_layer_surface_v1_set_size (layerSurface, 0, 0);
    zwlr_layer_surface_v1_set_anchor (
	layerSurface,
	ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
	    | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
    );
    zwlr_layer_surface_v1_set_keyboard_interactivity (layerSurface, false);
    zwlr_layer_surface_v1_add_listener (layerSurface, &layerSurfaceListener, this);
    zwlr_layer_surface_v1_set_exclusive_zone (layerSurface, -1);

#ifdef ENABLE_KDE_FEATURES
    if (m_driver->getWaylandContext ()->plasmaShell) {
	plasmaSurface = org_kde_plasma_shell_get_surface (m_driver->getWaylandContext ()->plasmaShell, surface);
	if (plasmaSurface) {
	    org_kde_plasma_surface_set_role (plasmaSurface, ORG_KDE_PLASMA_SURFACE_ROLE_DESKTOP);
	    org_kde_plasma_surface_set_skip_taskbar (plasmaSurface, 1);
	    org_kde_plasma_surface_set_skip_switcher (plasmaSurface, 1);
	}
    }
#endif

    this->prepareSurface ();
    wl_surface_commit (surface);
    wl_display_roundtrip (m_driver->getWaylandContext ()->display);

    // the roundtrip can already deliver closed (river without a layout)
    if (closed) {
	return;
    }

    this->createEGLSurface ();
}

void WaylandOutputViewport::setupToplevel () {
    auto* context = m_driver->getWaylandContext ();

    surface = wl_compositor_create_surface (context->compositor);
    xdgSurface = xdg_wm_base_get_xdg_surface (context->xdgWmBase, surface);
    xdg_surface_add_listener (xdgSurface, &xdgSurfaceListener, this);
    xdgToplevel = xdg_surface_get_toplevel (xdgSurface);
    xdg_toplevel_add_listener (xdgToplevel, &xdgToplevelListener, this);

    // the extension matches on these, set before the first commit
    const std::string title = std::string (WallpaperEngine::Desktop::GnomeShell::TITLE_PREFIX) + this->name;
    xdg_toplevel_set_app_id (xdgToplevel, WallpaperEngine::Desktop::GnomeShell::APP_ID);
    xdg_toplevel_set_title (xdgToplevel, title.c_str ());
    // not fullscreen, Mutter would hide the top bar; the extension places it by the title's output

    this->prepareSurface ();
    wl_surface_commit (surface);

    for (int i = 0; i < 10 && !toplevelConfigured && !closed; i++) {
	if (wl_display_roundtrip (context->display) == -1) {
	    break;
	}
    }

    if (closed) {
	return;
    }

    if (!toplevelConfigured) {
	sLog.error ("Output ", this->name, ": the compositor never configured the wallpaper window");
    }

    if (this->size.x <= 0 || this->size.y <= 0 || this->pendingToplevelSize.x <= 0) {
	this->size = this->logicalSize.x > 0 ? this->logicalSize : this->size / glm::max (this->scale, 1);
	this->logicalSize = this->size;
	this->viewport = { 0, 0, this->bufferSize () };
    }

    this->createEGLSurface ();
}

void WaylandOutputViewport::setupFractionalScale () {
    auto* context = m_driver->getWaylandContext ();

    // a fractional buffer needs the viewport to tell the compositor its logical size
    if (!context->viewporter) {
	return;
    }

    this->surfaceViewport = wp_viewporter_get_viewport (context->viewporter, this->surface);

    if (context->fractionalScaleManager) {
	this->fractionalScale
	    = wp_fractional_scale_manager_v1_get_fractional_scale (context->fractionalScaleManager, this->surface);
	wp_fractional_scale_v1_add_listener (this->fractionalScale, &fractionalScaleListener, this);
    }
}

void WaylandOutputViewport::prepareSurface () {
    this->setupFractionalScale ();
    this->setInputRegion (m_driver->getApp ().getContext ().settings.mouse.enabled);

    wl_region* opaqueRegion = wl_compositor_create_region (m_driver->getWaylandContext ()->compositor);
    wl_region_add (opaqueRegion, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_set_opaque_region (surface, opaqueRegion);
    wl_region_destroy (opaqueRegion);

    if (m_driver->isHDRAvailable ()) {
	this->setupColorManagement ();
	this->colorSurface = wp_color_manager_v1_get_surface (m_driver->getWaylandContext ()->colorManager, surface);
	this->applyImageDescription ();
    }
}

void WaylandOutputViewport::createEGLSurface () {
    const glm::ivec2 buffer = this->bufferSize ();

    this->viewport = { 0, 0, buffer };
    eglWindow = wl_egl_window_create (surface, buffer.x, buffer.y);
    eglSurface = m_driver->getEGLContext ()->eglCreatePlatformWindowSurfaceEXT (
	m_driver->getEGLContext ()->display, m_driver->getEGLContext ()->config, eglWindow, nullptr
    );

    // Mutter flags an empty commit between a toplevel's configure ack and its first buffer
    if (!this->xdgToplevel) {
	wl_surface_commit (surface);
    }

    wl_display_roundtrip (m_driver->getWaylandContext ()->display);
    wl_display_flush (m_driver->getWaylandContext ()->display);

    if (closed) {
	return;
    }

    static const auto XCURSORSIZE = [] {
	const char* text = getenv ("XCURSOR_SIZE");
	char* end = nullptr;
	const long parsed = text != nullptr ? std::strtol (text, &end, 10) : 0;

	return text != nullptr && end != text && parsed > 0 && parsed <= 1024 ? static_cast<int> (parsed) : 24;
    }();
    cursorTheme
	= wl_cursor_theme_load (getenv ("XCURSOR_THEME"), XCURSORSIZE * scale, m_driver->getWaylandContext ()->shm);

    if (!cursorTheme) {
	sLog.exception ("Failed to get a cursor theme");
    }

    pointer = wl_cursor_theme_get_cursor (cursorTheme, "left_ptr");
    cursorSurface = wl_compositor_create_surface (m_driver->getWaylandContext ()->compositor);

    if (!cursorSurface) {
	sLog.exception ("Failed to get a cursor surface");
    }

    if (eglMakeCurrent (
	    m_driver->getEGLContext ()->display, eglSurface, eglSurface, m_driver->getEGLContext ()->context
	)
	== EGL_FALSE) {
	sLog.exception ("Failed to make egl current");
    }

    // paced by our frame callbacks; with interval 1 Mesa blocks in eglSwapBuffers while the surface isn't presented,
    // stalling the main loop
    eglSwapInterval (m_driver->getEGLContext ()->display, 0);

    if (this->reopenDelay.count () > 0) {
	sLog.out ("Output ", this->name, ": the compositor took the wallpaper surface again");
    }

    this->reopenDelay = {};
    this->reopenAtMaxLogged = false;
    this->m_driver->getOutput ().reset ();
}

WaylandOpenGLDriver* WaylandOutputViewport::getDriver () const { return this->m_driver; }

void WaylandOutputViewport::makeCurrent () {
    const EGLBoolean result = eglMakeCurrent (
	m_driver->getEGLContext ()->display, eglSurface, eglSurface, m_driver->getEGLContext ()->context
    );

    if (result == EGL_FALSE) {
	sLog.error ("Couldn't make egl current");
    }
}

void WaylandOutputViewport::setInputRegion (bool everything) {
    wl_region* region = wl_compositor_create_region (m_driver->getWaylandContext ()->compositor);

    if (everything) {
	wl_region_add (region, 0, 0, INT32_MAX, INT32_MAX);
    }

    wl_surface_set_input_region (this->surface, region);
    wl_region_destroy (region);
}

void WaylandOutputViewport::setWantsPointerButtons (bool wants) {
    if (this->surface == nullptr || !m_driver->getApp ().getContext ().settings.mouse.enabled) {
	return;
    }

    // without a global cursor source, hover and parallax need the pointer on our surface
    const bool passthrough = !wants && m_driver->hasGlobalCursor ();

    if (passthrough == this->inputPassthrough) {
	return;
    }

    this->inputPassthrough = passthrough;

    this->setInputRegion (!passthrough);

    // the release would go to the desktop now
    this->leftClick = WallpaperEngine::Input::MouseClickStatus::Released;
    this->rightClick = WallpaperEngine::Input::MouseClickStatus::Released;

    sLog.out (
	"Output ", this->name,
	passthrough ? ": the wallpaper does not use clicks, they go to the desktop below"
		    : ": the wallpaper uses clicks, the pointer stays on it"
    );
}

void WaylandOutputViewport::renderFrame () {
    if (this->closed || this->m_driver->isDispatchingWhilePaused ()) {
	return;
    }

    this->rendering = true;
    this->m_driver->getApp ().update (this);
    this->rendering = false;
}

void WaylandOutputViewport::swapOutput () {
    this->callbackInitialized = true;
    this->lastSwap = std::chrono::steady_clock::now ();

    this->makeCurrent ();

    // a forced frame replaces the pending callback, two live callbacks would double the frame rate
    if (frameCallback) {
	wl_callback_destroy (frameCallback);
    }

    frameCallback = wl_surface_frame (surface);
    wl_callback_add_listener (frameCallback, &frameListener, this);

    // latched by the commit inside eglSwapBuffers together with the new buffer
    if (this->surfaceViewport) {
	wl_surface_set_buffer_scale (surface, 1);
	wp_viewport_set_destination (this->surfaceViewport, this->size.x, this->size.y);
    } else {
	wl_surface_set_buffer_scale (surface, scale);
    }

    eglSwapBuffers (m_driver->getEGLContext ()->display, this->eglSurface);
    wl_surface_damage_buffer (surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit (surface);
}

double WaylandOutputViewport::renderScale () const {
    if (this->surfaceViewport && this->preferredScale120 > 0) {
	return this->preferredScale120 / 120.0;
    }

    return this->scale;
}

glm::ivec2 WaylandOutputViewport::bufferSize () const {
    // fractional-scale-v1: the buffer is the logical size times the scale, rounded half away from zero
    if (this->surfaceViewport && this->preferredScale120 > 0) {
	return (this->size * static_cast<int> (this->preferredScale120) + 60) / 120;
    }

    return this->size * this->scale;
}

void WaylandOutputViewport::updateBufferSize () {
    this->viewport = { 0, 0, this->bufferSize () };
    this->resize ();
}

void WaylandOutputViewport::resize () {
    if (!this->eglWindow) {
	return;
    }

    const glm::ivec2 buffer = this->bufferSize ();

    wl_egl_window_resize (this->eglWindow, buffer.x, buffer.y, 0, 0);

    this->getDriver ()->getOutput ().reset ();
}
