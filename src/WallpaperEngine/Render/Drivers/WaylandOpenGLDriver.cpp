#include "WaylandOpenGLDriver.h"
#include "VideoFactories.h"
#include "WallpaperEngine/Application/WallpaperApplication.h"
#include "WallpaperEngine/Desktop/HostWayland.h"
#include "WallpaperEngine/Logging/Log.h"

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
#include <linux/input-event-codes.h>
}
#undef class
#undef namespace
#undef static

#include <algorithm>
#include <cerrno>
#include <poll.h>
#include <string.h>
#include <unistd.h>

using namespace WallpaperEngine::Render::Drivers;

constexpr int EVENT_WAIT_TIMEOUT_MS = 100;
constexpr auto MISSED_FRAME_TIMEOUT = std::chrono::seconds (1);
constexpr auto SURFACE_REOPEN_MIN = std::chrono::seconds (1);
constexpr auto SURFACE_REOPEN_MAX = std::chrono::seconds (30);

static void handlePointerEnter (
    void* data, struct wl_pointer* wl_pointer, uint32_t serial, struct wl_surface* surface, wl_fixed_t surface_x,
    wl_fixed_t surface_y
) {
    const auto driver = static_cast<WaylandOpenGLDriver*> (data);
    const auto viewport = driver->surfaceToViewport (surface);

    if (viewport == nullptr) {
	return;
    }

    driver->viewportInFocus = viewport;
    viewport->pointerEntered = true;
    viewport->mousePos = { wl_fixed_to_double (surface_x) * viewport->renderScale (),
			   (viewport->size.y - wl_fixed_to_double (surface_y)) * viewport->renderScale () };
    wl_surface_set_buffer_scale (viewport->cursorSurface, viewport->scale);
    wl_surface_attach (viewport->cursorSurface, wl_cursor_image_get_buffer (viewport->pointer->images[0]), 0, 0);
    wl_pointer_set_cursor (
	wl_pointer, serial, viewport->cursorSurface, viewport->pointer->images[0]->hotspot_x,
	viewport->pointer->images[0]->hotspot_y
    );
    wl_surface_commit (viewport->cursorSurface);
}

static void
handlePointerLeave (void* data, struct wl_pointer* wl_pointer, uint32_t serial, struct wl_surface* surface) {
    const auto driver = static_cast<WaylandOpenGLDriver*> (data);

    if (driver->viewportInFocus && driver->viewportInFocus->surface == surface) {
	driver->viewportInFocus = nullptr;
    }
}

static void handlePointerAxis (void* data, wl_pointer* wl_pointer, uint32_t time, uint32_t axis, wl_fixed_t value) { }

static void handlePointerMotion (
    void* data, struct wl_pointer* wl_pointer, uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y
) {
    const auto driver = static_cast<WaylandOpenGLDriver*> (data);

    const auto x = wl_fixed_to_double (surface_x);
    auto y = wl_fixed_to_double (surface_y);

    if (!driver->viewportInFocus) {
	return;
    }

    // Wayland has Y=0 at the top, OpenGL has Y=0 at the bottom
    const double viewportHeight = static_cast<double> (driver->viewportInFocus->size.y);
    y = viewportHeight - y;

    // buffer pixels, WE maps the cursor into the window's physical client area
    const double scale = driver->viewportInFocus->renderScale ();
    driver->viewportInFocus->mousePos = { x * scale, y * scale };
}

static void handlePointerButton (
    void* data, struct wl_pointer* wl_pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t button_state
) {
    const auto driver = static_cast<WaylandOpenGLDriver*> (data);

    if (!driver->viewportInFocus) {
	return;
    }

    if (button == BTN_LEFT) {
	if (button_state == WL_POINTER_BUTTON_STATE_PRESSED) {
	    driver->viewportInFocus->leftClick = WallpaperEngine::Input::MouseClickStatus::Clicked;
	} else if (button_state == WL_POINTER_BUTTON_STATE_RELEASED) {
	    driver->viewportInFocus->leftClick = WallpaperEngine::Input::MouseClickStatus::Released;
	}
    } else if (button == BTN_RIGHT) {
	if (button_state == WL_POINTER_BUTTON_STATE_PRESSED) {
	    driver->viewportInFocus->rightClick = WallpaperEngine::Input::MouseClickStatus::Clicked;
	} else if (button_state == WL_POINTER_BUTTON_STATE_RELEASED) {
	    driver->viewportInFocus->rightClick = WallpaperEngine::Input::MouseClickStatus::Released;
	}
    }
}

constexpr struct wl_pointer_listener pointerListener = { .enter = handlePointerEnter,
							 .leave = handlePointerLeave,
							 .motion = handlePointerMotion,
							 .button = handlePointerButton,
							 .axis = handlePointerAxis };

static void handleCapabilities (void* data, wl_seat* wl_seat, uint32_t capabilities) {
    const auto driver = static_cast<WaylandOpenGLDriver*> (data);
    auto* context = driver->getWaylandContext ();
    const bool hasPointer = capabilities & WL_SEAT_CAPABILITY_POINTER;

    // the capability comes and goes with the last pointer device (virtual pointers, unplugged mice)
    if (hasPointer && !context->pointer) {
	context->pointer = wl_seat_get_pointer (wl_seat);
	wl_pointer_add_listener (context->pointer, &pointerListener, data);
    } else if (!hasPointer && context->pointer) {
	wl_pointer_destroy (context->pointer);
	context->pointer = nullptr;
	driver->viewportInFocus = nullptr;
    }
}

constexpr struct wl_seat_listener seatListener = { .capabilities = handleCapabilities };

static void colorManagerSupportedIntent (void*, wp_color_manager_v1*, uint32_t) { }

static void colorManagerSupportedFeature (void* data, wp_color_manager_v1*, uint32_t feature) {
    if (feature == WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC) {
	static_cast<WaylandOpenGLDriver*> (data)->colorSupport.parametric = true;
    }
}

static void colorManagerSupportedTransferFunction (void* data, wp_color_manager_v1*, uint32_t tf) {
    if (tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ) {
	static_cast<WaylandOpenGLDriver*> (data)->colorSupport.pq = true;
    }
}

static void colorManagerSupportedPrimaries (void* data, wp_color_manager_v1*, uint32_t primaries) {
    if (primaries == WP_COLOR_MANAGER_V1_PRIMARIES_BT2020) {
	static_cast<WaylandOpenGLDriver*> (data)->colorSupport.bt2020 = true;
    }
}

static void colorManagerDone (void*, wp_color_manager_v1*) { }

constexpr struct wp_color_manager_v1_listener colorManagerListener = {
    .supported_intent = colorManagerSupportedIntent,
    .supported_feature = colorManagerSupportedFeature,
    .supported_tf_named = colorManagerSupportedTransferFunction,
    .supported_primaries_named = colorManagerSupportedPrimaries,
    .done = colorManagerDone,
};

static void hdrDescriptionFailed (void*, wp_image_description_v1*, uint32_t cause, const char* message) {
    sLog.error ("--hdr: the compositor refused a PQ image description (", cause, "): ", message);
}

static void hdrDescriptionReady (void* data, wp_image_description_v1*, uint32_t) {
    static_cast<WaylandOpenGLDriver*> (data)->hdrDescriptionReady = true;
}

constexpr struct wp_image_description_v1_listener hdrDescriptionListener = {
    .failed = hdrDescriptionFailed,
    .ready = hdrDescriptionReady,
};

static void handleWmBasePing (void*, xdg_wm_base* base, uint32_t serial) { xdg_wm_base_pong (base, serial); }

constexpr xdg_wm_base_listener wmBaseListener = { .ping = handleWmBasePing };

static void
handleGlobal (void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    const auto driver = static_cast<WaylandOpenGLDriver*> (data);

    if (strcmp (interface, wl_compositor_interface.name) == 0) {
	driver->getWaylandContext ()->compositor
	    = static_cast<wl_compositor*> (wl_registry_bind (registry, name, &wl_compositor_interface, 4));
    } else if (strcmp (interface, wl_shm_interface.name) == 0) {
	driver->getWaylandContext ()->shm
	    = static_cast<wl_shm*> (wl_registry_bind (registry, name, &wl_shm_interface, 1));
    } else if (strcmp (interface, wl_output_interface.name) == 0) {
	driver->m_screens.emplace_back (
	    new WallpaperEngine::Render::Drivers::Output::WaylandOutputViewport (driver, name, registry)
	);
    } else if (strcmp (interface, zwlr_layer_shell_v1_interface.name) == 0) {
	driver->getWaylandContext ()->layerShell
	    = static_cast<zwlr_layer_shell_v1*> (wl_registry_bind (registry, name, &zwlr_layer_shell_v1_interface, 1));
    } else if (strcmp (interface, xdg_wm_base_interface.name) == 0) {
	driver->getWaylandContext ()->xdgWmBase = static_cast<xdg_wm_base*> (
	    wl_registry_bind (registry, name, &xdg_wm_base_interface, std::min (version, 5u))
	);
	xdg_wm_base_add_listener (driver->getWaylandContext ()->xdgWmBase, &wmBaseListener, driver);
    } else if (strcmp (interface, wl_seat_interface.name) == 0) {
	driver->getWaylandContext ()->seat
	    = static_cast<wl_seat*> (wl_registry_bind (registry, name, &wl_seat_interface, 1));
	wl_seat_add_listener (driver->getWaylandContext ()->seat, &seatListener, driver);
    } else if (strcmp (interface, wp_viewporter_interface.name) == 0) {
	driver->getWaylandContext ()->viewporter
	    = static_cast<wp_viewporter*> (wl_registry_bind (registry, name, &wp_viewporter_interface, 1));
    } else if (strcmp (interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
	driver->getWaylandContext ()->fractionalScaleManager = static_cast<wp_fractional_scale_manager_v1*> (
	    wl_registry_bind (registry, name, &wp_fractional_scale_manager_v1_interface, 1)
	);
    } else if (strcmp (interface, zxdg_output_manager_v1_interface.name) == 0) {
	driver->getWaylandContext ()->xdgOutputManager = static_cast<zxdg_output_manager_v1*> (
	    wl_registry_bind (registry, name, &zxdg_output_manager_v1_interface, std::min (version, 3u))
	);
    } else if (
	strcmp (interface, wp_color_manager_v1_interface.name) == 0
	&& driver->getApp ().getContext ().settings.render.hdr
    ) {
	driver->getWaylandContext ()->colorManager
	    = static_cast<wp_color_manager_v1*> (wl_registry_bind (registry, name, &wp_color_manager_v1_interface, 1));
	wp_color_manager_v1_add_listener (driver->getWaylandContext ()->colorManager, &colorManagerListener, driver);
#ifdef ENABLE_KDE_FEATURES
    } else if (strcmp (interface, org_kde_plasma_shell_interface.name) == 0) {
	driver->getWaylandContext ()->plasmaShell = static_cast<org_kde_plasma_shell*> (
	    wl_registry_bind (registry, name, &org_kde_plasma_shell_interface, std::min (version, 8u))
	);
#endif
    }
}

static void handleGlobalRemoved (void* data, struct wl_registry* registry, uint32_t id) {
    const auto driver = static_cast<WaylandOpenGLDriver*> (data);

    // an unplugged monitor removes its global, a kept viewport would leak its layer-shell/EGL surfaces
    const auto it
	= std::ranges::find_if (driver->m_screens, [id] (const auto* viewport) { return viewport->waylandName == id; });

    if (it != driver->m_screens.end ()) {
	(*it)->closed = true;
	(*it)->outputRemoved = true;
	(*it)->rendering = false;
    }
}

constexpr struct wl_registry_listener registryListener = {
    .global = handleGlobal,
    .global_remove = handleGlobalRemoved,
};

void WaylandOpenGLDriver::initEGL () {
    const char* CLIENT_EXTENSIONS = eglQueryString (EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (!CLIENT_EXTENSIONS) {
	sLog.exception ("Failed to query EGL Extensions");
    }

    const auto CLIENTEXTENSIONS = std::string (CLIENT_EXTENSIONS);

    if (CLIENTEXTENSIONS.find ("EGL_EXT_platform_base") == std::string::npos) {
	sLog.exception ("EGL_EXT_platform_base not supported by EGL!");
    }

    if (CLIENTEXTENSIONS.find ("EGL_EXT_platform_wayland") == std::string::npos) {
	sLog.exception ("EGL_EXT_platform_wayland not supported by EGL!");
    }

    const auto eglGetPlatformDisplayEXT
	= reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC> (eglGetProcAddress ("eglGetPlatformDisplayEXT"));
    m_eglContext.eglCreatePlatformWindowSurfaceEXT = reinterpret_cast<PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC> (
	eglGetProcAddress ("eglCreatePlatformWindowSurfaceEXT")
    );

    if (!eglGetPlatformDisplayEXT || !m_eglContext.eglCreatePlatformWindowSurfaceEXT) {
	sLog.exception ("EGL did not return EXT proc pointers!");
    }

    m_eglContext.display = eglGetPlatformDisplayEXT (EGL_PLATFORM_WAYLAND_EXT, m_waylandContext.display, nullptr);

    if (m_eglContext.display == EGL_NO_DISPLAY) {
	this->finishEGL ();
	sLog.exception ("eglGetPlatformDisplayEXT failed!");
    }

    if (!eglInitialize (m_eglContext.display, nullptr, nullptr)) {
	this->finishEGL ();
	sLog.exception ("eglInitialize failed!");
    }

    const auto CLIENTEXTENSIONSPOSTINIT = std::string (eglQueryString (m_eglContext.display, EGL_EXTENSIONS));

    if (CLIENTEXTENSIONSPOSTINIT.find ("EGL_KHR_create_context") == std::string::npos) {
	this->finishEGL ();
	sLog.exception ("EGL_KHR_create_context not supported!");
    }

    EGLint matchedConfigs = 0;
    const EGLint CONFIG_ATTRIBUTES[] = {
	EGL_SURFACE_TYPE,    EGL_WINDOW_BIT, EGL_RED_SIZE, 1, EGL_GREEN_SIZE, 1, EGL_BLUE_SIZE, 1, EGL_SAMPLES, 4,
	EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE,
    };

    // PQ in 8 bits bands visibly, so HDR outputs want a 10 bit buffer (fine for the SDR ones too)
    if (this->isHDRAvailable ()) {
	const EGLint HDR_CONFIG_ATTRIBUTES[] = {
	    EGL_SURFACE_TYPE,
	    EGL_WINDOW_BIT,
	    EGL_RED_SIZE,
	    10,
	    EGL_GREEN_SIZE,
	    10,
	    EGL_BLUE_SIZE,
	    10,
	    EGL_SAMPLES,
	    4,
	    EGL_RENDERABLE_TYPE,
	    EGL_OPENGL_BIT,
	    EGL_NONE,
	};
	EGLConfig configs[64];
	EGLint count = 0;

	if (eglChooseConfig (m_eglContext.display, HDR_CONFIG_ATTRIBUTES, configs, 64, &count)) {
	    for (EGLint i = 0; i < count && matchedConfigs == 0; i++) {
		EGLint red = 0;

		eglGetConfigAttrib (m_eglContext.display, configs[i], EGL_RED_SIZE, &red);

		if (red == 10) {
		    m_eglContext.config = configs[i];
		    matchedConfigs = 1;
		}
	    }
	}

	if (matchedConfigs == 0) {
	    sLog.error ("--hdr: no 10 bit EGL config, HDR output will band");
	}
    }

    if (matchedConfigs == 0
	&& !eglChooseConfig (m_eglContext.display, CONFIG_ATTRIBUTES, &m_eglContext.config, 1, &matchedConfigs)) {
	this->finishEGL ();
	sLog.exception ("eglChooseConfig failed!");
    }

    if (matchedConfigs == 0) {
	this->finishEGL ();
	sLog.exception ("eglChooseConfig failed! (matched 0 configs)");
    }

    if (!eglBindAPI (EGL_OPENGL_API)) {
	this->finishEGL ();
	sLog.exception ("eglBindAPI failed!");
    }

    const EGLint CONTEXT_ATTRIBUTES[] = {
	EGL_CONTEXT_MAJOR_VERSION_KHR,
	3,
	EGL_CONTEXT_MINOR_VERSION_KHR,
	3,
	EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
	EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
	// drivers only emit KHR_debug output for debug contexts
	EGL_CONTEXT_FLAGS_KHR,
	EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR,
	EGL_NONE,
    };

    m_eglContext.context
	= eglCreateContext (m_eglContext.display, m_eglContext.config, EGL_NO_CONTEXT, CONTEXT_ATTRIBUTES);

    if (m_eglContext.context == EGL_NO_CONTEXT) {
	this->finishEGL ();
	sLog.error ("eglCreateContext error " + std::to_string (eglGetError ()));
	sLog.exception ("eglCreateContext failed!");
    }
}

void WaylandOpenGLDriver::finishEGL () const {
    eglMakeCurrent (EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (m_eglContext.display) {
	eglTerminate (m_eglContext.display);
    }
    eglReleaseThread ();
}

void WaylandOpenGLDriver::releaseSurface (Output::WaylandOutputViewport* viewport) {
    if (viewport->frameCallback) {
	wl_callback_destroy (viewport->frameCallback);
	viewport->frameCallback = nullptr;
    }

    if (viewport->eglSurface) {
	if (eglGetCurrentSurface (EGL_DRAW) == viewport->eglSurface) {
	    this->makeSurfacelessCurrent ();
	}

	eglDestroySurface (m_eglContext.display, viewport->eglSurface);
	viewport->eglSurface = nullptr;
    }

    if (viewport->eglWindow) {
	wl_egl_window_destroy (viewport->eglWindow);
	viewport->eglWindow = nullptr;
    }

    if (viewport->colorSurface) {
	wp_color_management_surface_v1_destroy (viewport->colorSurface);
	viewport->colorSurface = nullptr;
    }

    if (viewport->fractionalScale) {
	wp_fractional_scale_v1_destroy (viewport->fractionalScale);
	viewport->fractionalScale = nullptr;
    }

    if (viewport->surfaceViewport) {
	wp_viewport_destroy (viewport->surfaceViewport);
	viewport->surfaceViewport = nullptr;
    }

    if (viewport->layerSurface) {
	zwlr_layer_surface_v1_destroy (viewport->layerSurface);
	viewport->layerSurface = nullptr;
    }

    if (viewport->xdgToplevel) {
	xdg_toplevel_destroy (viewport->xdgToplevel);
	viewport->xdgToplevel = nullptr;
    }

    if (viewport->xdgSurface) {
	xdg_surface_destroy (viewport->xdgSurface);
	viewport->xdgSurface = nullptr;
    }

#ifdef ENABLE_KDE_FEATURES
    if (viewport->plasmaSurface) {
	org_kde_plasma_surface_destroy (viewport->plasmaSurface);
	viewport->plasmaSurface = nullptr;
    }
#endif

    if (viewport->surface) {
	wl_surface_destroy (viewport->surface);
	viewport->surface = nullptr;
    }

    if (viewport->cursorSurface) {
	wl_surface_destroy (viewport->cursorSurface);
	viewport->cursorSurface = nullptr;
    }

    if (viewport->cursorTheme) {
	wl_cursor_theme_destroy (viewport->cursorTheme);
	viewport->cursorTheme = nullptr;
	viewport->pointer = nullptr;
    }

    if (this->viewportInFocus == viewport) {
	this->viewportInFocus = nullptr;
    }

    viewport->pointerEntered = false;
    viewport->toplevelConfigured = false;
    viewport->callbackInitialized = false;
    viewport->inputPassthrough = false;
    viewport->closed = false;
    this->getOutput ().reset ();
}

void WaylandOpenGLDriver::removeOutput (Output::WaylandOutputViewport* viewport) {
    sLog.out ("Output ", viewport->name, " was removed");

    this->releaseSurface (viewport);

    if (viewport->xdgOutput) {
	zxdg_output_v1_destroy (viewport->xdgOutput);
	viewport->xdgOutput = nullptr;
    }

    if (viewport->colorOutput) {
	wp_color_management_output_v1_destroy (viewport->colorOutput);
	viewport->colorOutput = nullptr;
    }

    if (viewport->output) {
	wl_output_release (viewport->output);
    }

    std::erase (this->m_screens, viewport);
    this->getOutput ().reset ();
    delete viewport;
}

void WaylandOpenGLDriver::onSurfaceClosed (Output::WaylandOutputViewport* viewport) {
    this->releaseSurface (viewport);

    // river closes layer surfaces while no WM runs, as can an output turning off; with no signal for when to
    // retry, back off
    viewport->reopenDelay = std::clamp (viewport->reopenDelay * 2, SURFACE_REOPEN_MIN, SURFACE_REOPEN_MAX);
    viewport->reopenAt = std::chrono::steady_clock::now () + viewport->reopenDelay;

    if (viewport->reopenDelay == SURFACE_REOPEN_MIN) {
	sLog.error (
	    "The compositor closed the wallpaper surface on ", viewport->name,
	    " (river does this while no window manager runs), trying again"
	);
    } else if (viewport->reopenDelay == SURFACE_REOPEN_MAX && !viewport->reopenAtMaxLogged) {
	viewport->reopenAtMaxLogged = true;
	sLog.error (
	    "The compositor keeps closing the wallpaper surface on ", viewport->name, ", trying again every ",
	    SURFACE_REOPEN_MAX.count (), " s"
	);
    }

    if (this->drawnOutputs ().empty () && !this->m_waitingLogged) {
	this->m_waitingLogged = true;
	sLog.error ("No output shows a wallpaper, waiting until the compositor takes one");
    }
}

void WaylandOpenGLDriver::makeSurfacelessCurrent () {
    if (eglMakeCurrent (m_eglContext.display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_eglContext.context) == EGL_FALSE) {
	sLog.exception (
	    "The compositor shows no wallpaper surface and EGL can't use the GL context without one "
	    "(EGL_KHR_surfaceless_context), error ",
	    eglGetError ()
	);
    }
}

WaylandOpenGLDriver::WaylandOpenGLDriver (ApplicationContext& context, WallpaperApplication& app) :
    VideoDriver (app, m_mouseInput), m_output (context, *this), m_requestedExit (false), m_frameCounter (0),
    m_context (context), m_mouseInput (*this) {
    initWaylandRegistry ();
    initColorManagement ();
    initEGL ();
    setupOutputLayerSurfaces ();

    if (eglGetCurrentContext () == EGL_NO_CONTEXT) {
	this->makeSurfacelessCurrent ();
    }

    initGLEW ();
}

void WaylandOpenGLDriver::initWaylandRegistry () {
    // GNOME 45-48 only accept a Shell-spawned client, connect through its helper
    if (Desktop::isGnomeSession ()) {
	this->m_gnomeShell = std::make_unique<Desktop::GnomeShell> ();

	if (this->m_gnomeShell->isAvailable () && this->m_gnomeShell->needsHelper ()) {
	    const int fd = this->m_gnomeShell->openWaylandSocket ();

	    if (fd < 0) {
		sLog.exception ("Cannot draw a wallpaper on this GNOME: ", this->m_gnomeShell->reason ());
	    }

	    m_waylandContext.display = wl_display_connect_to_fd (fd);
	}
    }

    if (!m_waylandContext.display) {
	m_waylandContext.display = Desktop::connectWayland ();

	if (Desktop::usesHostWayland ()) {
	    sLog.out ("Flatpak's Wayland socket offers no layer shell, connected through the host instead");
	}
    }

    if (!m_waylandContext.display) {
	sLog.exception ("Failed to query wayland display");
    }

    m_waylandContext.registry = wl_display_get_registry (m_waylandContext.display);
    wl_registry_add_listener (m_waylandContext.registry, &registryListener, this);

    wl_display_dispatch (m_waylandContext.display);
    wl_display_roundtrip (m_waylandContext.display);

    std::vector<std::string> missing;

    if (!m_waylandContext.compositor) {
	missing.emplace_back ("wl_compositor");
    }
    if (!m_waylandContext.shm) {
	missing.emplace_back ("wl_shm");
    }
    if (this->m_screens.empty ()) {
	missing.emplace_back ("wl_output (no monitor)");
    }
    if (!m_waylandContext.layerShell && !(WallpaperEngine::Desktop::isGnomeSession () && m_waylandContext.xdgWmBase)) {
	missing.emplace_back (
	    "zwlr_layer_shell_v1 (wlr-layer-shell, the compositor can't put a surface behind windows)"
	);
    }

    if (!missing.empty ()) {
	for (const auto& interface : missing) {
	    sLog.error ("The compositor does not offer ", interface);
	}

	sLog.exception ("Failed to bind to required interfaces: ", missing.front ());
    }

    if (m_waylandContext.layerShell) {
	this->m_gnomeShell.reset ();
    } else {
	this->initGnomeShell ();
    }

    if (m_waylandContext.xdgOutputManager) {
	for (const auto& o : this->m_screens) {
	    o->setupXdgOutput (m_waylandContext.xdgOutputManager);
	}
	wl_display_roundtrip (m_waylandContext.display);
    } else if (!m_context.settings.general.spanGroups.empty ()) {
	sLog.error ("zxdg_output_manager_v1 is unavailable; screen-span positions will be incorrect.");
    }
}

void WaylandOpenGLDriver::initGnomeShell () {
    if (!this->m_gnomeShell) {
	this->m_gnomeShell = std::make_unique<Desktop::GnomeShell> ();
    }

    if (!this->m_gnomeShell->isAvailable ()) {
	sLog.error (
	    "GNOME on Wayland has no layer-shell, wallpapers need the linux-wallpaperengine GNOME Shell extension: ",
	    this->m_gnomeShell->reason ()
	);
	sLog.error (
	    "Install and enable it (we_manager offers it, or copy gnome-shell-extension/ to "
	    "~/.local/share/gnome-shell/extensions and run gnome-extensions enable "
	    "linux-wallpaperengine@wallpaperenginelover.github.io), then log out and back in once"
	);
	sLog.exception ("Cannot draw a wallpaper on GNOME Wayland without the GNOME Shell extension");
    }

    sLog.out ("GNOME Shell extension found, the wallpapers are desktop windows");
}

void WaylandOpenGLDriver::initColorManagement () {
    if (!m_context.settings.render.hdr) {
	return;
    }

    if (!m_waylandContext.colorManager) {
	sLog.error ("--hdr: the compositor has no color management protocol (wp_color_manager_v1), staying SDR");
	return;
    }

    // the supported_* events and done follow the bind
    wl_display_roundtrip (m_waylandContext.display);

    if (!colorSupport.parametric || !colorSupport.pq || !colorSupport.bt2020) {
	sLog.error ("--hdr: the compositor can't take PQ with BT.2020 primaries, staying SDR");
	return;
    }

    auto* creator = wp_color_manager_v1_create_parametric_creator (m_waylandContext.colorManager);
    wp_image_description_creator_params_v1_set_tf_named (creator, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
    wp_image_description_creator_params_v1_set_primaries_named (creator, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
    this->m_hdrDescription = wp_image_description_creator_params_v1_create (creator);
    wp_image_description_v1_add_listener (this->m_hdrDescription, &hdrDescriptionListener, this);
    wl_display_roundtrip (m_waylandContext.display);

    if (!this->hdrDescriptionReady) {
	return;
    }

    for (const auto& o : this->m_screens) {
	o->setupColorManagement ();
    }

    wl_display_roundtrip (m_waylandContext.display);
    wl_display_roundtrip (m_waylandContext.display);

    for (const auto& o : this->m_screens) {
	sLog.out ("Output ", o->name, o->isOutputHDR () ? " runs in HDR, drawing PQ" : " runs in SDR");
    }
}

bool WaylandOpenGLDriver::isHDRAvailable () const { return this->hdrDescriptionReady; }

wp_image_description_v1* WaylandOpenGLDriver::getHDRDescription () const { return this->m_hdrDescription; }

void WaylandOpenGLDriver::setupOutputLayerSurfaces () {
    bool any = false;

    for (const auto& o : this->m_screens) {
	if (!m_context.isScreenRequested (o->name)) {
	    continue;
	}

	if (m_waylandContext.layerShell) {
	    o->setupLS ();
	} else {
	    o->setupToplevel ();
	}
	any = true;
    }

    if (!any) {
	sLog.error ("No outputs could be initialized, please check the parameters and try again");
	sLog.error ("Detected outputs:");

	for (const auto& o : this->m_screens) {
	    sLog.error ("  ", o->name);
	}

	sLog.error ("Requested: ");

	for (const auto& line : m_context.describeRequestedScreens ()) {
	    sLog.error ("  ", line);
	}

	sLog.exception ("Cannot continue...");
    }

    this->m_layerSurfacesReady = true;
}

void WaylandOpenGLDriver::setupLateOutputs () {
    // KDE often drops and re-adds outputs while it applies the screen layout at login, and monitors can
    // wake up after we started; the wallpapers stay registered by screen name, only the surface is missing
    for (const auto& o : this->m_screens) {
	if (o->hasShellSurface () || o->closed || !o->initialized || !m_context.isScreenRequested (o->name)
	    || std::chrono::steady_clock::now () < o->reopenAt) {
	    continue;
	}

	if (o->reopenDelay.count () == 0) {
	    sLog.out ("Output ", o->name, " appeared, attaching the wallpaper");
	}

	if (m_waylandContext.xdgOutputManager && !o->xdgOutput) {
	    o->setupXdgOutput (m_waylandContext.xdgOutputManager);
	}

	if (m_waylandContext.layerShell) {
	    o->setupLS ();
	} else {
	    o->setupToplevel ();
	}

	if (o->eglSurface && !o->closed) {
	    this->m_waitingLogged = false;
	}
    }
}

void WaylandOpenGLDriver::initGLEW () {
    glewExperimental = GL_TRUE;
    if (const GLenum result = glewInit (); result != GLEW_OK) {
	const char* error = reinterpret_cast<const char*> (glewGetErrorString (result));
	// GLEW reports GLEW_ERROR_NO_GLX_DISPLAY or null without a GLX display, harmless since EGL is current
	if (result == GLEW_ERROR_NO_GLX_DISPLAY || error == nullptr) {
	    sLog.out ("Failed to initialize GLEW, but continuing with EGL context: ", error ? error : "No GLX display");
	} else {
	    sLog.error ("Failed to initialize GLEW: ", error);
	    sLog.exception ("Cannot continue...");
	}
    }
}

WaylandOpenGLDriver::~WaylandOpenGLDriver () {
    for (const auto& screen : this->m_screens) {
	if (screen->xdgOutput) {
	    zxdg_output_v1_destroy (screen->xdgOutput);
	    screen->xdgOutput = nullptr;
	}
    }

    eglMakeCurrent (EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    if (m_eglContext.context != EGL_NO_CONTEXT) {
	eglDestroyContext (m_eglContext.display, m_eglContext.context);
    }

    eglTerminate (m_eglContext.display);
    eglReleaseThread ();

    if (this->m_waylandContext.display) {
	wl_display_disconnect (this->m_waylandContext.display);
    }
}

void WaylandOpenGLDriver::reapClosedOutputs () {
    std::vector<Output::WaylandOutputViewport*> closed;

    std::ranges::copy_if (this->m_screens, std::back_inserter (closed), [] (const auto* o) { return o->closed; });

    for (auto* viewport : closed) {
	if (viewport->outputRemoved) {
	    this->removeOutput (viewport);
	} else {
	    this->onSurfaceClosed (viewport);
	}
    }
}

void WaylandOpenGLDriver::dispatchEventQueue () {
    if (this->m_gnomeShell) {
	this->m_gnomeShell->dispatch ();

	if (this->m_gnomeShell->vanished ()) {
	    sLog.error ("Stopping, the wallpapers can't stay on GNOME without the Shell extension");
	    m_requestedExit = true;
	    return;
	}
    }

    this->reapClosedOutputs ();

    if (this->m_layerSurfacesReady) {
	this->setupLateOutputs ();
    }

    // rendering continues from frame callbacks, so this redraws surfaces that missed their first frame or a dropped
    // callback (and draws the very first frame)
    const auto now = std::chrono::steady_clock::now ();

    for (const auto& o : this->m_screens) {
	if (o->hasShellSurface () && !o->closed && now - o->lastSwap > MISSED_FRAME_TIMEOUT) {
	    o->renderFrame ();
	}
    }

    // the compositor stops sending frame callbacks to hidden surfaces, a blocking dispatch would stall the main loop
    wl_display* display = m_waylandContext.display;

    while (wl_display_prepare_read (display) != 0) {
	if (wl_display_dispatch_pending (display) == -1) {
	    m_requestedExit = true;
	    return;
	}
    }

    wl_display_flush (display);

    pollfd fd = { .fd = wl_display_get_fd (display), .events = POLLIN, .revents = 0 };
    const int ready = poll (&fd, 1, EVENT_WAIT_TIMEOUT_MS);

    if (ready > 0) {
	if (wl_display_read_events (display) == -1) {
	    m_requestedExit = true;
	    return;
	}
    } else {
	wl_display_cancel_read (display);

	if (ready == -1 && errno != EINTR) {
	    m_requestedExit = true;
	    return;
	}
    }

    if (wl_display_dispatch_pending (display) == -1) {
	m_requestedExit = true;
    }

    m_frameCounter++;
}

void WaylandOpenGLDriver::dispatchWhilePaused () {
    // a visible paused wallpaper still gets pointer events, unread they overflow the socket
    if (this->m_gnomeShell) {
	this->m_gnomeShell->dispatch ();

	if (this->m_gnomeShell->vanished ()) {
	    m_requestedExit = true;
	    return;
	}
    }

    this->reapClosedOutputs ();

    wl_display* display = m_waylandContext.display;
    this->m_dispatchingWhilePaused = true;

    while (wl_display_prepare_read (display) != 0) {
	if (wl_display_dispatch_pending (display) == -1) {
	    m_requestedExit = true;
	    this->m_dispatchingWhilePaused = false;
	    return;
	}
    }

    wl_display_flush (display);

    pollfd fd = { .fd = wl_display_get_fd (display), .events = POLLIN, .revents = 0 };

    if (poll (&fd, 1, 0) > 0) {
	if (wl_display_read_events (display) == -1) {
	    m_requestedExit = true;
	}
    } else {
	wl_display_cancel_read (display);
    }

    if (!m_requestedExit && wl_display_dispatch_pending (display) == -1) {
	m_requestedExit = true;
    }

    this->m_dispatchingWhilePaused = false;

    // callbacks that came in meanwhile are owed a frame, the watchdog draws it on resume
    for (auto* o : this->m_screens) {
	if (o->hasShellSurface () && o->frameCallback == nullptr) {
	    o->lastSwap = {};
	}
    }
}

Output::Output& WaylandOpenGLDriver::getOutput () { return this->m_output; }

float WaylandOpenGLDriver::getRenderTime () const {
    return static_cast<float> (std::chrono::duration_cast<std::chrono::microseconds> (
				   std::chrono::high_resolution_clock::now () - renderStart
	   )
				   .count ())
	/ 1000000.0;
}

bool WaylandOpenGLDriver::closeRequested () { return this->m_requestedExit; }

void WaylandOpenGLDriver::resizeWindow (glm::ivec2 size) { }

void WaylandOpenGLDriver::resizeWindow (glm::ivec4 sizeandpos) { }

void WaylandOpenGLDriver::showWindow () { }

void WaylandOpenGLDriver::hideWindow () { }

glm::ivec2 WaylandOpenGLDriver::getFramebufferSize () const { return glm::ivec2 { 0, 0 }; }

uint32_t WaylandOpenGLDriver::getFrameCounter () const { return m_frameCounter; }

WaylandOpenGLDriver::SEGLContext* WaylandOpenGLDriver::getEGLContext () { return &this->m_eglContext; }

void* WaylandOpenGLDriver::getProcAddress (const char* name) const {
    return reinterpret_cast<void*> (eglGetProcAddress (name));
}

void* WaylandOpenGLDriver::getWaylandDisplay () const { return this->m_waylandContext.display; }

WaylandOpenGLDriver::WaylandContext* WaylandOpenGLDriver::getWaylandContext () { return &this->m_waylandContext; }

std::vector<std::string> WaylandOpenGLDriver::drawnOutputs () const {
    std::vector<std::string> outputs;

    for (const auto* viewport : this->m_screens) {
	if (viewport->hasShellSurface () && !viewport->closed) {
	    outputs.push_back (viewport->name);
	}
    }

    return outputs;
}

Output::WaylandOutputViewport* WaylandOpenGLDriver::surfaceToViewport (const wl_surface* surface) const {
    for (const auto& o : m_screens) {
	if (o->surface == surface) {
	    return o;
	}
    }

    return nullptr;
}

__attribute__ ((constructor)) void registerWaylandOpenGL () {
    sVideoFactories.registerDriver (
	ApplicationContext::DESKTOP_BACKGROUND, "wayland",
	[] (ApplicationContext& context, WallpaperApplication& application) -> std::unique_ptr<VideoDriver> {
	    return std::make_unique<WaylandOpenGLDriver> (context, application);
	}
    );
}
