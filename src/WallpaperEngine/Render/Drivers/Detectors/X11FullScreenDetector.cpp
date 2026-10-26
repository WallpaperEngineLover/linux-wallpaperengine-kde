#include "X11FullScreenDetector.h"
#include "WallpaperEngine/Logging/Log.h"

#include <algorithm>
#include <cstdlib>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>

#include "WallpaperEngine/Render/Drivers/GLFWOpenGLDriver.h"

#define GLFW_EXPOSE_NATIVE_X11
#include "WallpaperEngine/Render/Drivers/VideoFactories.h"
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

namespace WallpaperEngine::Render::Drivers::Detectors {
void CustomXIOErrorExitHandler (Display* dsp, void* userdata) {
    const auto context = static_cast<X11FullScreenDetector*> (userdata);

    sLog.debugerror ("Critical XServer error detected. Attempting to recover...");

    context->reset ();
}

int CustomXErrorHandler (Display* dpy, XErrorEvent* event) {
    sLog.debugerror ("Detected X error");

    return 0;
}

int CustomXIOErrorHandler (Display* dsp) {
    sLog.debugerror ("Detected X error");

    return 0;
}

X11FullScreenDetector::X11FullScreenDetector (Application::ApplicationContext& appContext, VideoDriver& driver) :
    FullScreenDetector (appContext), m_display (nullptr), m_root (0), m_driver (driver) {
    try {
	// throws if m_driver isn't actually a GLFWOpenGLDriver, so we can catch the misuse
	std::ignore = dynamic_cast<GLFWOpenGLDriver&> (this->m_driver);
    } catch (std::exception&) {
	sLog.exception ("X11 FullScreen Detector initialized with the wrong video driver... This is a bug...");
    }

    // not chaining the previous handler, it could stop the app
    XSetErrorHandler (CustomXErrorHandler);
    XSetIOErrorHandler (CustomXIOErrorHandler);

    this->initialize ();

    if (std::getenv ("LWE_XWAYLAND_FALLBACK") != nullptr && Desktop::isPantheonSession ()) {
	this->m_gala = std::make_unique<Desktop::PantheonShell> ();

	if (!this->m_gala->isAvailable ()) {
	    sLog.error ("Native Wayland windows are not seen, ", this->m_gala->reason ());
	    this->m_gala.reset ();
	}
    }
}

X11FullScreenDetector::~X11FullScreenDetector () { this->stop (); }

bool X11FullScreenDetector::anythingFullscreen () const {
    if (this->m_display == nullptr) {
	return false;
    }

    Window root, parent;
    Window* children = nullptr;
    unsigned int childCount = 0;

    if (!XQueryTree (this->m_display, this->m_root, &root, &parent, &children, &childCount)) {
	return false;
    }

    const Window ourWindow = glfwGetX11Window (dynamic_cast<GLFWOpenGLDriver&> (this->m_driver).getWindow ());
    const auto& render = this->getApplicationContext ().settings.render;
    bool fullscreen = false;

    for (unsigned int i = 0; i < childCount && !fullscreen && render.pauseOnFullscreen; i++) {
	XWindowAttributes attributes;

	if (!XGetWindowAttributes (this->m_display, children[i], &attributes) || attributes.map_state != IsViewable) {
	    continue;
	}

	// only managed windows count, WM/compositor helpers and our desktop window cover the screen too
	const Window client = this->clientOf (children[i]);

	if (client == None || client == ourWindow || this->isDesktopWindow (client) || this->isHidden (client)
	    || !this->onCurrentDesktop (client)) {
	    continue;
	}

	fullscreen
	    = this->hasFullscreenState (client) || this->hasMaximizedState (client) || this->coversScreen (client);
    }

    XFree (children);

    bool galaFocused = false;
    const bool galaCovers = this->galaWindowCovers (galaFocused);

    return fullscreen || (render.pauseOnFullscreen && galaCovers)
	|| (render.pauseOnFocus && (this->applicationFocused (ourWindow) || galaFocused));
}

bool X11FullScreenDetector::galaWindowCovers (bool& focused) const {
    if (this->m_gala == nullptr) {
	return false;
    }

    this->m_gala->dispatch ();

    bool covers = false;

    for (const auto& window : this->m_gala->windows ()) {
	// our own background window is a window of Gala's too
	if (window.hidden || window.workspace != this->m_gala->activeWorkspace ()
	    || window.wmClass == "linux-wallpaperengine") {
	    continue;
	}

	focused = focused || window.focused;
	covers = covers || std::ranges::any_of (this->m_screens, [&] (const auto& entry) {
		     return window.width >= static_cast<uint32_t> (entry.second.z)
			 && window.height >= static_cast<uint32_t> (entry.second.w);
		 });
    }

    return covers;
}

bool X11FullScreenDetector::applicationFocused (Window ourWindow) const {
    const Atom activeAtom = XInternAtom (this->m_display, "_NET_ACTIVE_WINDOW", False);
    Atom type;
    int format;
    unsigned long count, remaining;
    unsigned char* data = nullptr;
    Window active = None;

    if (XGetWindowProperty (
	    this->m_display, this->m_root, activeAtom, 0, 1, False, XA_WINDOW, &type, &format, &count, &remaining, &data
	) == Success
	&& data != nullptr && format == 32 && count == 1) {
	active = *reinterpret_cast<const Window*> (data);
    }

    if (data != nullptr) {
	XFree (data);
    }

    // sub_14003D050
    if (active == None || active == ourWindow || this->isDesktopWindow (active)) {
	return false;
    }

    XWindowAttributes attributes;
    return XGetWindowAttributes (this->m_display, active, &attributes) && attributes.map_state == IsViewable;
}

bool X11FullScreenDetector::isHidden (Window client) const {
    // minimized, or tabbed out: some window managers keep the frame of such a window mapped
    const Atom stateAtom = XInternAtom (this->m_display, "_NET_WM_STATE", False);
    const Atom hiddenAtom = XInternAtom (this->m_display, "_NET_WM_STATE_HIDDEN", False);
    Atom type;
    int format;
    unsigned long count, remaining;
    unsigned char* data = nullptr;
    bool hidden = false;

    if (XGetWindowProperty (
	    this->m_display, client, stateAtom, 0, 32, False, XA_ATOM, &type, &format, &count, &remaining, &data
	) == Success
	&& data != nullptr && format == 32) {
	const auto* atoms = reinterpret_cast<const Atom*> (data);
	hidden = std::find (atoms, atoms + count, hiddenAtom) != atoms + count;
    }

    if (data != nullptr) {
	XFree (data);
    }

    if (hidden) {
	return true;
    }

    // ICCCM: WM_STATE 3 is iconic
    const Atom wmState = XInternAtom (this->m_display, "WM_STATE", False);
    data = nullptr;

    if (XGetWindowProperty (
	    this->m_display, client, wmState, 0, 2, False, wmState, &type, &format, &count, &remaining, &data
	) == Success
	&& data != nullptr && format == 32 && count >= 1) {
	hidden = reinterpret_cast<const unsigned long*> (data)[0] == IconicState;
    }

    if (data != nullptr) {
	XFree (data);
    }

    return hidden;
}

bool X11FullScreenDetector::onCurrentDesktop (Window client) const {
    const auto cardinal = [this] (Window window, const char* name, unsigned long& value) {
	const Atom atom = XInternAtom (this->m_display, name, False);
	Atom type;
	int format;
	unsigned long count, remaining;
	unsigned char* data = nullptr;
	bool found = false;

	if (XGetWindowProperty (
		this->m_display, window, atom, 0, 1, False, XA_CARDINAL, &type, &format, &count, &remaining, &data
	    ) == Success
	    && data != nullptr && format == 32 && count == 1) {
	    value = *reinterpret_cast<const unsigned long*> (data);
	    found = true;
	}

	if (data != nullptr) {
	    XFree (data);
	}

	return found;
    };

    unsigned long windowDesktop = 0, currentDesktop = 0;

    // no EWMH desktops, or a sticky window (0xFFFFFFFF), counts everywhere
    if (!cardinal (client, "_NET_WM_DESKTOP", windowDesktop)
	|| !cardinal (this->m_root, "_NET_CURRENT_DESKTOP", currentDesktop)
	|| (windowDesktop & 0xFFFFFFFF) == 0xFFFFFFFF) {
	return true;
    }

    return windowDesktop == currentDesktop;
}

bool X11FullScreenDetector::hasFullscreenState (Window client) const {
    const Atom stateAtom = XInternAtom (this->m_display, "_NET_WM_STATE", False);
    const Atom fullscreenAtom = XInternAtom (this->m_display, "_NET_WM_STATE_FULLSCREEN", False);
    Atom type;
    int format;
    unsigned long count, remaining;
    unsigned char* data = nullptr;
    bool fullscreen = false;

    if (XGetWindowProperty (
	    this->m_display, client, stateAtom, 0, 32, False, XA_ATOM, &type, &format, &count, &remaining, &data
	) == Success
	&& data != nullptr && format == 32) {
	const auto* atoms = reinterpret_cast<const Atom*> (data);
	fullscreen = std::find (atoms, atoms + count, fullscreenAtom) != atoms + count;
    }

    if (data != nullptr) {
	XFree (data);
    }

    return fullscreen;
}

bool X11FullScreenDetector::hasMaximizedState (Window client) const {
    const Atom stateAtom = XInternAtom (this->m_display, "_NET_WM_STATE", False);
    const Atom vertAtom = XInternAtom (this->m_display, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    const Atom horzAtom = XInternAtom (this->m_display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    Atom type;
    int format;
    unsigned long count, remaining;
    unsigned char* data = nullptr;
    bool maximized = false;

    if (XGetWindowProperty (
	    this->m_display, client, stateAtom, 0, 32, False, XA_ATOM, &type, &format, &count, &remaining, &data
	) == Success
	&& data != nullptr && format == 32) {
	const auto* atoms = reinterpret_cast<const Atom*> (data);
	maximized = std::find (atoms, atoms + count, vertAtom) != atoms + count
	    && std::find (atoms, atoms + count, horzAtom) != atoms + count;
    }

    if (data != nullptr) {
	XFree (data);
    }

    return maximized;
}

bool X11FullScreenDetector::coversScreen (Window client) const {
    // window managers without EWMH: the client (not its frame) has an output's geometry
    XWindowAttributes attributes;
    int x = 0, y = 0;
    Window child;

    if (!XGetWindowAttributes (this->m_display, client, &attributes)
	|| !XTranslateCoordinates (this->m_display, client, this->m_root, 0, 0, &x, &y, &child)) {
	return false;
    }

    return std::ranges::any_of (this->m_screens, [&] (const auto& entry) {
	const auto& screen = entry.second;
	return x == screen.x && y == screen.y && attributes.width == screen.z && attributes.height == screen.w;
    });
}

Window X11FullScreenDetector::clientOf (Window window) const {
    // the client inside a WM frame is the window with WM_STATE
    const Atom wmState = XInternAtom (this->m_display, "WM_STATE", False);
    Atom type = None;
    int format;
    unsigned long count, remaining;
    unsigned char* data = nullptr;

    XGetWindowProperty (
	this->m_display, window, wmState, 0, 0, False, AnyPropertyType, &type, &format, &count, &remaining, &data
    );

    if (data != nullptr) {
	XFree (data);
    }

    if (type != None) {
	return window;
    }

    Window root, parent, *children = nullptr;
    unsigned int childCount = 0;
    Window client = None;

    if (XQueryTree (this->m_display, window, &root, &parent, &children, &childCount)) {
	for (unsigned int i = 0; i < childCount && client == None; i++) {
	    client = this->clientOf (children[i]);
	}
    }

    if (children != nullptr) {
	XFree (children);
    }

    return client;
}

bool X11FullScreenDetector::isDesktopWindow (Window window) const {

    const Atom typeAtom = XInternAtom (this->m_display, "_NET_WM_WINDOW_TYPE", False);
    const Atom desktopAtom = XInternAtom (this->m_display, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    Atom type;
    int format;
    unsigned long count, remaining;
    unsigned char* data = nullptr;
    bool desktop = false;

    if (XGetWindowProperty (
	    this->m_display, window, typeAtom, 0, 16, False, XA_ATOM, &type, &format, &count, &remaining, &data
	) == Success
	&& data != nullptr && format == 32) {
	const auto* atoms = reinterpret_cast<const Atom*> (data);
	desktop = std::find (atoms, atoms + count, desktopAtom) != atoms + count;
    }

    if (data != nullptr) {
	XFree (data);
    }

    return desktop;
}

void X11FullScreenDetector::reset () {
    this->stop ();
    this->initialize ();
}

void X11FullScreenDetector::initialize () {
    this->m_display = XOpenDisplay (nullptr);
    this->m_screens.clear ();

    if (this->m_display == nullptr) {
	sLog.error ("Cannot open the X display, fullscreen detection is off until the next reset");
	return;
    }

    // recover from X disconnections instead of aborting
#ifdef HAVE_XSETIOERROREXITHANDLER
    XSetIOErrorExitHandler (this->m_display, CustomXIOErrorExitHandler, this);
#endif

    int xrandr_result, xrandr_error;

    if (!XRRQueryExtension (this->m_display, &xrandr_result, &xrandr_error)) {
	sLog.error ("XRandr is not present, fullscreen detection might not work");
	return;
    }

    this->m_root = DefaultRootWindow (this->m_display);
    XRRScreenResources* screenResources = XRRGetScreenResources (this->m_display, this->m_root);

    if (screenResources == nullptr) {
	sLog.error ("Cannot detect screen sizes using xrandr, fullscreen detection might not work");
	return;
    }

    for (int i = 0; i < screenResources->noutput; i++) {
	const XRROutputInfo* info = XRRGetOutputInfo (this->m_display, screenResources, screenResources->outputs[i]);

	if (info == nullptr || info->connection != RR_Connected) {
	    continue;
	}

	XRRCrtcInfo* crtc = XRRGetCrtcInfo (this->m_display, screenResources, info->crtc);

	if (crtc == nullptr) {
	    continue;
	}

	this->m_screens.emplace (std::string (info->name), glm::ivec4 (crtc->x, crtc->y, crtc->width, crtc->height));

	XRRFreeCrtcInfo (crtc);
    }

    XRRFreeScreenResources (screenResources);
}

void X11FullScreenDetector::stop () {
    if (this->m_display == nullptr) {
	return;
    }

    XCloseDisplay (this->m_display);
    this->m_display = nullptr;
}

__attribute__ ((constructor)) void registerX11FullscreenDetector () {
    sVideoFactories.registerFullscreenDetector (
	"x11", [] (ApplicationContext& context, VideoDriver& driver) -> std::unique_ptr<FullScreenDetector> {
	    return std::make_unique<X11FullScreenDetector> (context, driver);
	}
    );
}

}
