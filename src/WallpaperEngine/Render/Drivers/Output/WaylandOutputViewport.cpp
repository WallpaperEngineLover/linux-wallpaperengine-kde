#include "WaylandOutputViewport.h"
#include "WallpaperEngine/Logging/Log.h"

#include <unistd.h>

#define class _class
#define namespace _namespace
#define static
extern "C" {
#include "color-management-v1-protocol.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"
#include "xdg-output-unstable-v1-protocol.h"
#include "xdg-shell-protocol.h"
#ifdef ENABLE_KDE_EXPERIMENTAL_FEATURES
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
    viewport->viewport = { 0, 0, viewport->size.x * viewport->scale, viewport->size.y * viewport->scale };
    viewport->resize ();

    zwlr_layer_surface_v1_ack_configure (surface, serial);
}

static void handleLSClosed (void* data, zwlr_layer_surface_v1* surface) {
    const auto viewport = static_cast<WaylandOutputViewport*> (data);

    viewport->getDriver ()->onLayerClose (viewport);
}

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

    // physical pixels; logicalSize comes from xdg-output or the layer shell configure
    viewport->size = { width, height };
    viewport->viewport = { 0, 0, viewport->size.x * viewport->scale, viewport->size.y * viewport->scale };

    if (viewport->layerSurface) {
	viewport->resize ();
    }

    if (viewport->initialized) {
	viewport->getDriver ()->getOutput ().reset ();
    }
}

static void done (void* data, wl_output* wl_output) { static_cast<WaylandOutputViewport*> (data)->initialized = true; }

static void scale (void* data, wl_output* wl_output, int32_t scale) {
    const auto viewport = static_cast<WaylandOutputViewport*> (data);

    viewport->scale = scale;

    if (viewport->layerSurface) {
	viewport->resize ();
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

static void description (void* data, wl_output* wl_output, const char* description) {
    // ignored
}

static void surfaceFrameCallback (void* data, struct wl_callback* cb, uint32_t time) {
    const auto viewport = static_cast<WaylandOutputViewport*> (data);

    wl_callback_destroy (cb);

    viewport->frameCallback = nullptr;
    viewport->rendering = true;
    viewport->getDriver ()->getApp ().update (viewport);
    viewport->rendering = false;
}

constexpr struct wl_callback_listener frameListener = { .done = surfaceFrameCallback };

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

static void xdgOutputName (void* data, struct zxdg_output_v1* xdg_output, const char* name) {
    // already handled by wl_output.name
}

static void xdgOutputDescription (void* data, struct zxdg_output_v1* xdg_output, const char* description) {
    // ignored
}

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

static void descriptionInfoLuminances (void* data, wp_image_description_info_v1*, uint32_t, uint32_t max, uint32_t reference) {
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

    // takes effect with the next frame's commit
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

    layerSurface = zwlr_layer_shell_v1_get_layer_surface (
	m_driver->getWaylandContext ()->layerShell, surface, output, wlrLayer,
	"desktop"
    );

    if (!layerSurface) {
	sLog.exception ("Failed to get a layer surface");
    }

    wl_region* region = wl_compositor_create_region (m_driver->getWaylandContext ()->compositor);
    if (m_driver->getApp ().getContext ().settings.mouse.enabled) {
	wl_region_add (region, 0, 0, INT32_MAX, INT32_MAX);
    }

    // fully opaque: lets the compositor skip alpha-blending, always a win since wallpapers are the
    // bottommost visible content
    wl_region* opaqueRegion = wl_compositor_create_region (m_driver->getWaylandContext ()->compositor);
    wl_region_add (opaqueRegion, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_set_opaque_region (surface, opaqueRegion);
    wl_region_destroy (opaqueRegion);

    zwlr_layer_surface_v1_set_size (layerSurface, 0, 0);
    zwlr_layer_surface_v1_set_anchor (
	layerSurface,
	ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
	    | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
    );
    zwlr_layer_surface_v1_set_keyboard_interactivity (layerSurface, false);
    zwlr_layer_surface_v1_add_listener (layerSurface, &layerSurfaceListener, this);
    zwlr_layer_surface_v1_set_exclusive_zone (layerSurface, -1);
    wl_surface_set_input_region (surface, region);
    wl_region_destroy (region);

#ifdef ENABLE_KDE_EXPERIMENTAL_FEATURES
    if (m_driver->getWaylandContext ()->plasmaShell) {
	plasmaSurface = org_kde_plasma_shell_get_surface (m_driver->getWaylandContext ()->plasmaShell, surface);
	if (plasmaSurface) {
	    org_kde_plasma_surface_set_role (plasmaSurface, ORG_KDE_PLASMA_SURFACE_ROLE_DESKTOP);
	    org_kde_plasma_surface_set_skip_taskbar (plasmaSurface, 1);
	    org_kde_plasma_surface_set_skip_switcher (plasmaSurface, 1);
	}
    }
#endif

    if (m_driver->isHDRAvailable ()) {
	this->setupColorManagement ();
	this->colorSurface = wp_color_manager_v1_get_surface (m_driver->getWaylandContext ()->colorManager, surface);
	this->applyImageDescription ();
    }

    wl_surface_commit (surface);
    wl_display_roundtrip (m_driver->getWaylandContext ()->display);

    eglWindow = wl_egl_window_create (surface, size.x * scale, size.y * scale);
    eglSurface = m_driver->getEGLContext ()->eglCreatePlatformWindowSurfaceEXT (
	m_driver->getEGLContext ()->display, m_driver->getEGLContext ()->config, eglWindow, nullptr
    );
    wl_surface_commit (surface);
    wl_display_roundtrip (m_driver->getWaylandContext ()->display);
    wl_display_flush (m_driver->getWaylandContext ()->display);

    static const auto XCURSORSIZE = getenv ("XCURSOR_SIZE") ? std::stoi (getenv ("XCURSOR_SIZE")) : 24;
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

void WaylandOutputViewport::swapOutput () {
    this->callbackInitialized = true;

    this->makeCurrent ();
    frameCallback = wl_surface_frame (surface);
    wl_callback_add_listener (frameCallback, &frameListener, this);
    eglSwapBuffers (m_driver->getEGLContext ()->display, this->eglSurface);
    wl_surface_set_buffer_scale (surface, scale);
    wl_surface_damage_buffer (surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit (surface);
}

void WaylandOutputViewport::resize () {
    if (!this->eglWindow) {
	return;
    }

    wl_egl_window_resize (this->eglWindow, this->size.x * this->scale, this->size.y * this->scale, 0, 0);

    this->getDriver ()->getOutput ().reset ();
}
