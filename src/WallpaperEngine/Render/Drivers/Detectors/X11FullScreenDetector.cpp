#include "X11FullScreenDetector.h"
#include "WallpaperEngine/Desktop/WindowState.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Drivers/Output/Output.h"
#include "WallpaperEngine/Render/Drivers/X11RandR.h"
#include "WallpaperEngine/Render/Drivers/X11WindowType.h"

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
static void CustomXIOErrorExitHandler (Display* dsp, void* userdata) {
    const auto context = static_cast<X11FullScreenDetector*> (userdata);

    sLog.debugerror ("Critical XServer error detected. Attempting to recover...");

    context->reset ();
}

static int CustomXErrorHandler (Display* dpy, XErrorEvent* event) {
    sLog.debugerror ("Detected X error");

    return 0;
}

static int CustomXIOErrorHandler (Display* dsp) {
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

    const Window ourWindow = glfwGetX11Window (dynamic_cast<GLFWOpenGLDriver&> (this->m_driver).getWindow ());
    const auto& context = this->getApplicationContext ();
    const auto& render = context.settings.render;
    const bool windowsPause = Desktop::pausesAnyOf (this->windowStates (ourWindow), this->ownOutputs (), context);

    bool galaFocused = false;
    const bool galaCovers = this->galaWindowCovers (galaFocused);

    return windowsPause || (render.pauseOnFullscreen && galaCovers)
	|| (render.pauseOnFocus && (this->applicationFocused (ourWindow) || galaFocused));
}

std::vector<std::string> X11FullScreenDetector::ownOutputs () const {
    std::vector<std::string> names;

    for (const auto& [name, viewport] : dynamic_cast<GLFWOpenGLDriver&> (this->m_driver).getOutput ().getViewports ()) {
	if (this->m_screens.contains (name)) {
	    names.push_back (name);
	}
    }

    // a plain window is not one of the outputs, a window on any of them pauses it
    if (names.empty ()) {
	for (const auto& [name, area] : this->m_screens) {
	    names.push_back (name);
	}
    }

    return names;
}

std::vector<Desktop::WindowState> X11FullScreenDetector::windowStates (Window ourWindow) const {
    std::vector<Desktop::WindowState> states;
    Window root, parent;
    Window* children = nullptr;
    unsigned int childCount = 0;

    if (!XQueryTree (this->m_display, this->m_root, &root, &parent, &children, &childCount)) {
	return states;
    }

    const Window active = this->activeWindow ();
    // without EWMH there is no telling which window has the focus, so none is held back for it
    const bool focusKnown = this->hasEwmhWindowManager ();

    for (unsigned int i = 0; i < childCount; i++) {
	XWindowAttributes attributes;

	if (!XGetWindowAttributes (this->m_display, children[i], &attributes) || attributes.map_state != IsViewable) {
	    continue;
	}

	// only managed windows count, WM/compositor helpers and our desktop window cover the screen too
	const Window client = this->clientOf (children[i]);

	if (client == None || client == ourWindow || hasDesktopWindowType (this->m_display, client)) {
	    continue;
	}

	const auto atoms = windowAtoms (this->m_display, client, "_NET_WM_STATE");

	if (this->isHidden (client, atoms) || !this->onCurrentDesktop (client)) {
	    continue;
	}

	uint32_t flags = 0;

	if (this->hasMaximizedState (atoms)) {
	    flags |= Desktop::WindowState::Maximized;
	} else if (this->hasFullscreenState (atoms) || this->coversScreen (client)) {
	    flags |= Desktop::WindowState::Fullscreen;
	}

	if (client == active || (!focusKnown && flags != 0)) {
	    flags |= Desktop::WindowState::Focused;
	}

	if (flags == 0) {
	    continue;
	}

	std::string appId;
	XClassHint classHint {};

	if (XGetClassHint (this->m_display, client, &classHint) != 0) {
	    appId = classHint.res_class != nullptr ? classHint.res_class : "";
	    XFree (classHint.res_name);
	    XFree (classHint.res_class);
	}

	XWindowAttributes clientAttributes;
	int x = 0, y = 0;
	Window child;
	std::vector<std::string> outputs;

	if (XGetWindowAttributes (this->m_display, client, &clientAttributes)
	    && XTranslateCoordinates (this->m_display, client, this->m_root, 0, 0, &x, &y, &child)) {
	    outputs = Desktop::outputsCoveredBy (
		{ x, y, clientAttributes.width, clientAttributes.height }, this->m_screens
	    );
	} else {
	    for (const auto& [name, area] : this->m_screens) {
		outputs.push_back (name);
	    }
	}

	for (const auto& output : outputs) {
	    states.push_back ({ appId, output, flags });
	}
    }

    XFree (children);

    return states;
}

Window X11FullScreenDetector::activeWindow () const {
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

    return active;
}

bool X11FullScreenDetector::hasEwmhWindowManager () const {
    const Atom checkAtom = XInternAtom (this->m_display, "_NET_SUPPORTING_WM_CHECK", False);
    Atom type = None;
    int format;
    unsigned long count, remaining;
    unsigned char* data = nullptr;

    XGetWindowProperty (
	this->m_display, this->m_root, checkAtom, 0, 1, False, XA_WINDOW, &type, &format, &count, &remaining, &data
    );

    if (data != nullptr) {
	XFree (data);
    }

    return type != None;
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
    const Window active = this->activeWindow ();

    // sub_14003D050
    if (active == None || active == ourWindow || hasDesktopWindowType (this->m_display, active)) {
	return false;
    }

    XWindowAttributes attributes;
    return XGetWindowAttributes (this->m_display, active, &attributes) && attributes.map_state == IsViewable;
}

bool X11FullScreenDetector::isHidden (Window client, const std::vector<Atom>& states) const {
    // minimized, or tabbed out: some window managers keep the frame of such a window mapped
    if (hasAtom (states, XInternAtom (this->m_display, "_NET_WM_STATE_HIDDEN", False))) {
	return true;
    }

    // ICCCM: WM_STATE 3 is iconic
    const Atom wmState = XInternAtom (this->m_display, "WM_STATE", False);
    Atom type;
    int format;
    unsigned long count, remaining;
    unsigned char* data = nullptr;
    bool hidden = false;

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

bool X11FullScreenDetector::hasFullscreenState (const std::vector<Atom>& states) const {
    return hasAtom (states, XInternAtom (this->m_display, "_NET_WM_STATE_FULLSCREEN", False));
}

bool X11FullScreenDetector::hasMaximizedState (const std::vector<Atom>& states) const {
    return hasAtom (states, XInternAtom (this->m_display, "_NET_WM_STATE_MAXIMIZED_VERT", False))
	&& hasAtom (states, XInternAtom (this->m_display, "_NET_WM_STATE_MAXIMIZED_HORZ", False));
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

    forEachConnectedOutput (
	this->m_display, screenResources, [this] (const XRROutputInfo& info, const XRRCrtcInfo& crtc) {
	    this->m_screens.emplace (std::string (info.name), glm::ivec4 (crtc.x, crtc.y, crtc.width, crtc.height));
	}
    );

    XRRFreeScreenResources (screenResources);
}

void X11FullScreenDetector::stop () {
    if (this->m_display == nullptr) {
	return;
    }

    XCloseDisplay (this->m_display);
    this->m_display = nullptr;
}

__attribute__ ((constructor)) static void registerX11FullscreenDetector () {
    sVideoFactories.registerFullscreenDetector (
	"x11", [] (ApplicationContext& context, VideoDriver& driver) -> std::unique_ptr<FullScreenDetector> {
	    return std::make_unique<X11FullScreenDetector> (context, driver);
	}
    );
}

}
