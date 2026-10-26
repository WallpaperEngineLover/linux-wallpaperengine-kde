#include "X11Output.h"
#include "GLFWOutputViewport.h"
#include "WallpaperEngine/Logging/Log.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/shape.h>

using namespace WallpaperEngine::Render::Drivers::Output;

void CustomXIOErrorExitHandler (Display* dsp, void* userdata) {
    const auto context = static_cast<X11Output*> (userdata);

    sLog.debugerror ("Critical XServer error detected. Attempting to recover...");

    // Xlib is still inside the call that failed, the display can only go away once that has returned
    context->requestReset ();
}

int CustomXErrorHandler (Display* dpy, XErrorEvent* event) {
    sLog.debugerror ("Detected X error");

    return 0;
}

int CustomXIOErrorHandler (Display* dsp) {
    sLog.debugerror ("Detected X error");

    return 0;
}

X11Output::X11Output (ApplicationContext& context, VideoDriver& driver) :
    Output (context, driver), m_display (nullptr), m_pixmap (None), m_root (None), m_gc (None), m_imageData (nullptr),
    m_imageSize (0), m_image (nullptr) {
    // not chaining the previous handler: it could stop the app under weird circumstances
    XSetErrorHandler (CustomXErrorHandler);
    XSetIOErrorHandler (CustomXIOErrorHandler);

    this->loadScreenInfo ();
}

X11Output::~X11Output () { this->free (); }

void X11Output::reset () {
    this->free ();
    this->loadScreenInfo ();
}

void X11Output::free () {
    if (this->m_display == nullptr) {
	return;
    }

    // m_viewports holds non-owning aliases, m_screens owns the objects
    for (const auto& screen : this->m_screens) {
	delete screen;
    }

    this->m_screens.clear ();
    this->m_viewports.clear ();

    this->m_background.reset ();
#ifdef HAVE_X11_DESKTOP_ICONS
    this->m_icons.reset ();
    this->m_iconsTried = false;
#endif

    // XDestroyImage() frees m_imageData itself
    if (this->m_desktopWindow != None) {
	XDestroyWindow (this->m_display, this->m_desktopWindow);
	this->m_desktopWindow = None;
	this->m_desktopWindowMapped = false;
    }

    if (this->m_image != nullptr) {
	XDestroyImage (this->m_image);
	this->m_image = nullptr;
    }

    this->m_imageData = nullptr;

    if (this->m_gc != None) {
	XFreeGC (this->m_display, this->m_gc);
	this->m_gc = None;
    }

    if (this->m_pixmap != None) {
	XFreePixmap (this->m_display, this->m_pixmap);
	this->m_pixmap = None;
    }

    XCloseDisplay (this->m_display);
    this->m_display = nullptr;
}

void* X11Output::getImageBuffer () const { return this->m_imageData; }

bool X11Output::renderVFlip () const { return false; }

bool X11Output::renderMultiple () const { return this->m_viewports.size () > 1; }

bool X11Output::haveImageBuffer () const { return true; }

uint32_t X11Output::getImageBufferSize () const { return this->m_imageSize; }

void X11Output::loadScreenInfo () {
    this->m_resetPending = false;
    this->m_display = XOpenDisplay (nullptr);
#ifdef HAVE_XSETIOERROREXITHANDLER
    XSetIOErrorExitHandler (this->m_display, CustomXIOErrorExitHandler, this);
#endif

    int xrandr_result, xrandr_error;

    if (!XRRQueryExtension (this->m_display, &xrandr_result, &xrandr_error)) {
	sLog.error ("XRandr is not present, cannot detect specified screens, running in window mode");
	return;
    }

    this->m_root = DefaultRootWindow (this->m_display);
    this->m_fullWidth = DisplayWidth (this->m_display, DefaultScreen (this->m_display));
    this->m_fullHeight = DisplayHeight (this->m_display, DefaultScreen (this->m_display));
    XRRScreenResources* screenResources = XRRGetScreenResources (this->m_display, DefaultRootWindow (this->m_display));

    if (screenResources == nullptr) {
	sLog.error ("Cannot detect screen sizes using xrandr, running in window mode");
	return;
    }

    discoverOutputs (screenResources);
    XRRFreeScreenResources (screenResources);
    validateOutputs ();
    initX11Background ();
}

void X11Output::discoverOutputs (XRRScreenResources* screenResources) {
    for (int i = 0; i < screenResources->noutput; i++) {
	const XRROutputInfo* info = XRRGetOutputInfo (this->m_display, screenResources, screenResources->outputs[i]);

	if (info == nullptr || info->connection != RR_Connected) {
	    continue;
	}

	XRRCrtcInfo* crtc = XRRGetCrtcInfo (this->m_display, screenResources, info->crtc);

	if (crtc == nullptr) {
	    continue;
	}

	if (this->m_context.isScreenRequested (info->name)) {
	    sLog.out (
		"Found requested screen: ", info->name, " -> ", crtc->x, "x", crtc->y, ":", crtc->width, "x",
		crtc->height
	    );

	    auto* vp = new GLFWOutputViewport { { crtc->x, crtc->y, crtc->width, crtc->height }, info->name };
	    vp->globalPosition = { crtc->x, crtc->y };
	    vp->logicalSize = { crtc->width, crtc->height };
	    this->m_screens.push_back (vp);
	    this->m_viewports[info->name] = vp;
	}

	XRRFreeCrtcInfo (crtc);
    }
}

void X11Output::validateOutputs () const {
    const bool any = std::ranges::any_of (this->m_screens, [this] (const OutputViewport* o) {
	return this->m_context.isScreenRequested (o->name);
    });

    if (!any) {
	sLog.error ("No outputs could be initialized, please check the parameters and try again");
	sLog.error ("Detected outputs:");

	for (const auto& o : this->m_screens) {
	    sLog.error ("  ", o->name);
	}

	sLog.error ("Requested: ");

	for (const auto& line : this->m_context.describeRequestedScreens ()) {
	    sLog.error ("  ", line);
	}

	sLog.exception ("Cannot continue...");
    }
}

bool X11Output::desktopHidesRootWindow () {
    if (const char* forced = std::getenv ("LWE_X11_BACKGROUND_WINDOW"); forced != nullptr && *forced != '\0') {
	return forced[0] != '0';
    }

    const char* desktops = std::getenv ("XDG_CURRENT_DESKTOP");

    if (desktops == nullptr) {
	return false;
    }

    std::string_view rest (desktops);

    while (!rest.empty ()) {
	const auto colon = rest.find (':');
	const auto name = rest.substr (0, colon);

	if (name == "GNOME" || name == "Pantheon" || name == "X-Cinnamon") {
	    return true;
	}

	if (colon == std::string_view::npos) {
	    break;
	}

	rest.remove_prefix (colon + 1);
    }

    return false;
}

void X11Output::initX11Background () {
    if (desktopHidesRootWindow ()) {
	auto window = std::make_unique<X11BackgroundWindow> (0, 0, this->m_fullWidth, this->m_fullHeight);

	if (window->valid ()) {
	    sLog.out ("The desktop paints over the root window, drawing into a background window of our own");
	    this->m_imageData = window->data ();
	    this->m_imageSize = window->size ();
	    this->m_background = std::move (window);
	    this->m_driver.resizeWindow ({ this->m_fullWidth, this->m_fullHeight });
	    return;
	}
    }

    this->m_pixmap = XCreatePixmap (this->m_display, this->m_root, this->m_fullWidth, this->m_fullHeight, 24);
    this->m_gc = XCreateGC (this->m_display, this->m_pixmap, 0, nullptr);
    XFillRectangle (this->m_display, this->m_pixmap, this->m_gc, 0, 0, this->m_fullWidth, this->m_fullHeight);
    XSetWindowBackgroundPixmap (this->m_display, this->m_root, this->m_pixmap);
    // XCreateImage() frees this with free(), so malloc() it
    this->m_imageSize = this->m_fullWidth * this->m_fullHeight * 4;
    this->m_imageData = static_cast<char*> (malloc (this->m_imageSize));
    this->m_image = XCreateImage (
	this->m_display, CopyFromParent, 24, ZPixmap, 0, this->m_imageData, this->m_fullWidth, this->m_fullHeight, 32, 0
    );
    this->m_driver.resizeWindow ({ this->m_fullWidth, this->m_fullHeight });
}

void X11Output::requestReset () const { this->m_resetPending = true; }

void X11Output::updateRender () const {
    if (this->m_resetPending) {
	const_cast<X11Output*> (this)->reset ();
	return;
    }

    if (this->m_background != nullptr) {
	this->m_background->present ();
	return;
    }

#ifdef HAVE_X11_DESKTOP_ICONS
    if (this->m_icons != nullptr) {
	this->m_icons->update ();
	this->m_icons->composite (this->m_imageData, this->m_fullWidth, this->m_fullHeight);
    }
#endif

    XPutImage (
	this->m_display, this->m_pixmap, this->m_gc, this->m_image, 0, 0, 0, 0, this->m_fullWidth, this->m_fullHeight
    );

    // _XROOTPMAP_ID tells compositors about the background pixmap and forces a refresh (picom). ESETROOT_PMAP_ID is
    // left alone: whoever sets the next root background (mate-bg, feh, ...) calls XKillClient on the pixmap it names,
    // which closes our whole connection to the server
    const Atom prop_root = XInternAtom (this->m_display, "_XROOTPMAP_ID", False);
    XChangeProperty (
	this->m_display, this->m_root, prop_root, XA_PIXMAP, 32, PropModeReplace, (unsigned char*)&this->m_pixmap, 1
    );

    XClearWindow (this->m_display, this->m_root);
    this->updateDesktopWindow ();

    if (this->m_desktopWindowMapped) {
	XClearWindow (this->m_display, this->m_desktopWindow);
    }

    XFlush (this->m_display);
}

std::vector<Window> X11Output::stackingOrder () const {
    // bottom to top, the root's children without EWMH
    const Atom stackingAtom = XInternAtom (this->m_display, "_NET_CLIENT_LIST_STACKING", False);
    Atom type;
    int format;
    unsigned long count, remaining;
    unsigned char* data = nullptr;
    std::vector<Window> windows;

    if (XGetWindowProperty (
	    this->m_display, this->m_root, stackingAtom, 0, 4096, False, XA_WINDOW, &type, &format, &count, &remaining,
	    &data
	) == Success
	&& data != nullptr && format == 32) {
	const auto* list = reinterpret_cast<const Window*> (data);
	windows.assign (list, list + count);
    }

    if (data != nullptr) {
	XFree (data);
    }

    if (windows.empty ()) {
	Window rootReturn, parent;
	Window* children = nullptr;
	unsigned int childCount = 0;

	if (XQueryTree (this->m_display, this->m_root, &rootReturn, &parent, &children, &childCount)) {
	    windows.assign (children, children + childCount);
	}

	if (children != nullptr) {
	    XFree (children);
	}
    }

    return windows;
}

bool X11Output::isDesktopType (Window window) const {
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

std::vector<Window> X11Output::findCoveringDesktopWindows (const std::vector<Window>& stacking) const {
    std::vector<Window> found;

    for (const Window window : stacking) {
	if (window == this->m_desktopWindow || !this->isDesktopType (window)) {
	    continue;
	}

	XWindowAttributes attributes;

	// 32 bit windows are translucent (nemo-desktop shows the root)
	if (!XGetWindowAttributes (this->m_display, window, &attributes) || attributes.map_state != IsViewable
	    || attributes.depth == 32) {
	    continue;
	}

	int x = 0, y = 0;
	Window child;
	XTranslateCoordinates (this->m_display, window, this->m_root, 0, 0, &x, &y, &child);

	const bool covers = std::ranges::any_of (this->m_screens, [&] (const OutputViewport* screen) {
	    const auto& area = screen->viewport;
	    return x <= area.x && y <= area.y && x + attributes.width >= area.x + area.z
		&& y + attributes.height >= area.y + area.w;
	});

	if (covers) {
	    found.push_back (window);
	}
    }

    return found;
}

void X11Output::createDesktopWindow () const {
    // override redirect so no WM frame (xfwm4) eats the clicks, updateDesktopWindow () keeps the stacking
    XSetWindowAttributes attributes {};
    attributes.background_pixmap = this->m_pixmap;
    attributes.override_redirect = True;

    this->m_desktopWindow = XCreateWindow (
	this->m_display, this->m_root, 0, 0, this->m_fullWidth, this->m_fullHeight, 0, CopyFromParent, InputOutput,
	CopyFromParent, CWBackPixmap | CWOverrideRedirect, &attributes
    );

    const auto atom = [this] (const char* name) { return XInternAtom (this->m_display, name, False); };

    const Atom desktopType = atom ("_NET_WM_WINDOW_TYPE_DESKTOP");
    XChangeProperty (
	this->m_display, this->m_desktopWindow, atom ("_NET_WM_WINDOW_TYPE"), XA_ATOM, 32, PropModeReplace,
	reinterpret_cast<const unsigned char*> (&desktopType), 1
    );

    const Atom states[]
	= { atom ("_NET_WM_STATE_STICKY"), atom ("_NET_WM_STATE_SKIP_TASKBAR"), atom ("_NET_WM_STATE_SKIP_PAGER") };
    XChangeProperty (
	this->m_display, this->m_desktopWindow, atom ("_NET_WM_STATE"), XA_ATOM, 32, PropModeReplace,
	reinterpret_cast<const unsigned char*> (states), 3
    );

    const unsigned long allDesktops = 0xFFFFFFFF;
    XChangeProperty (
	this->m_display, this->m_desktopWindow, atom ("_NET_WM_DESKTOP"), XA_CARDINAL, 32, PropModeReplace,
	reinterpret_cast<const unsigned char*> (&allDesktops), 1
    );

    // 2 keeps compositors (xfwm4) from unredirecting it above every other window
    const unsigned long keepComposited = 2;
    XChangeProperty (
	this->m_display, this->m_desktopWindow, atom ("_NET_WM_BYPASS_COMPOSITOR"), XA_CARDINAL, 32, PropModeReplace,
	reinterpret_cast<const unsigned char*> (&keepComposited), 1
    );

    const unsigned long pid = getpid ();
    XChangeProperty (
	this->m_display, this->m_desktopWindow, atom ("_NET_WM_PID"), XA_CARDINAL, 32, PropModeReplace,
	reinterpret_cast<const unsigned char*> (&pid), 1
    );

    char name[] = "linux-wallpaperengine";
    XClassHint classHint { name, name };
    XSetClassHint (this->m_display, this->m_desktopWindow, &classHint);
    XStoreName (this->m_display, this->m_desktopWindow, name);

    XShapeCombineRectangles (this->m_display, this->m_desktopWindow, ShapeInput, 0, 0, nullptr, 0, ShapeSet, Unsorted);
}

Window X11Output::topLevelOf (Window window) const {
    Window rootReturn, parent, *children = nullptr;
    unsigned int count;

    while (XQueryTree (this->m_display, window, &rootReturn, &parent, &children, &count)) {
	if (children != nullptr) {
	    XFree (children);
	    children = nullptr;
	}

	if (parent == this->m_root || parent == None) {
	    return window;
	}

	window = parent;
    }

    return window;
}

#ifdef HAVE_X11_DESKTOP_ICONS
void X11Output::updateDesktopIcons (const std::vector<Window>& covering) const {
    if (this->m_icons != nullptr) {
	this->m_icons->follow (covering);
	return;
    }

    if (this->m_iconsTried) {
	return;
    }

    const char* setting = std::getenv ("LWE_DESKTOP_ICONS");

    if (setting != nullptr && std::string_view (setting) == "off") {
	this->m_iconsTried = true;
	return;
    }

    if (!std::ranges::all_of (covering, [this] (Window window) {
	    return X11DesktopIcons::supports (this->m_display, window);
	})) {
	return;
    }

    this->m_iconsTried = true;
    std::vector<std::string> names;

    for (const auto* screen : this->m_screens) {
	names.push_back (screen->name);
    }

    auto icons
	= std::make_unique<X11DesktopIcons> (this->m_display, this->m_root, this->m_fullWidth, this->m_fullHeight);

    if (icons->start (covering, names)) {
	this->m_icons = std::move (icons);
    }
}
#endif

void X11Output::updateDesktopWindow () const {
    bool restacked = false;

    while (XPending (this->m_display)) {
	XEvent event;
	XNextEvent (this->m_display, &event);
#ifdef HAVE_X11_DESKTOP_ICONS
	if (this->m_icons != nullptr && this->m_icons->handleEvent (event)) {
	    continue;
	}
#endif
	restacked |= event.type == ConfigureNotify || event.type == MapNotify || event.type == UnmapNotify
	    || event.type == DestroyNotify;
    }

    const auto now = std::chrono::steady_clock::now ();

    if (now < this->m_nextDesktopCheck && !restacked) {
	return;
    }

    this->m_nextDesktopCheck = now + std::chrono::seconds (2);

    const auto covering = this->findCoveringDesktopWindows (this->stackingOrder ());

    if (covering.empty ()) {
#ifdef HAVE_X11_DESKTOP_ICONS
	this->m_icons.reset ();
	this->m_iconsTried = false;
#endif

	if (this->m_desktopWindowMapped) {
	    XUnmapWindow (this->m_display, this->m_desktopWindow);
	    this->m_desktopWindowMapped = false;
	    sLog.out ("The desktop window covering the root window is gone, drawing into the root window again");
	}

	return;
    }

    if (this->m_desktopWindow == None) {
	this->createDesktopWindow ();
	XSelectInput (this->m_display, this->m_root, SubstructureNotifyMask);
    }

    if (!this->m_desktopWindowMapped) {
	XMapWindow (this->m_display, this->m_desktopWindow);
	this->m_desktopWindowMapped = true;
	sLog.out ("A desktop window covers the root window, drawing into a desktop window of our own above it");
    }

    std::vector<Window> theirTopLevels;
    theirTopLevels.reserve (covering.size ());

    for (const Window window : covering) {
	theirTopLevels.push_back (this->topLevelOf (window));
    }

    Window rootReturn, parent, *children = nullptr;
    unsigned int count = 0;
    Window theirs = theirTopLevels.front ();
    bool inPlace = false;

    if (XQueryTree (this->m_display, this->m_root, &rootReturn, &parent, &children, &count)) {
	// one desktop window per monitor (xfdesktop): ours goes above the topmost, or the others hide it on their
	// monitor
	const Window* end = children + count;
	const Window* topmost = children;

	for (const Window top : theirTopLevels) {
	    const Window* position = std::find (static_cast<const Window*> (children), end, top);

	    if (position != end && position >= topmost) {
		topmost = position;
		theirs = top;
	    }
	}

	inPlace = topmost != end && topmost + 1 != end && *(topmost + 1) == this->m_desktopWindow;
    }

    if (children != nullptr) {
	XFree (children);
    }

    if (!inPlace) {
	XWindowChanges changes {};
	changes.sibling = theirs;
	changes.stack_mode = Above;
	XConfigureWindow (this->m_display, this->m_desktopWindow, CWSibling | CWStackMode, &changes);
    }

#ifdef HAVE_X11_DESKTOP_ICONS
    // after the stacking above: starting it takes seconds (xfconf-query runs), and a full screen window of ours left on
    // top of the others meanwhile makes xfwm4's compositor stop painting the windows that were open already
    XFlush (this->m_display);
    this->updateDesktopIcons (covering);
#endif
}
