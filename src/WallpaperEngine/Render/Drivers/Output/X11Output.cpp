#include "X11Output.h"
#include "GLFWOutputViewport.h"
#include "WallpaperEngine/Logging/Log.h"

#include <algorithm>
#include <cstdlib>
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
    // TODO: bring back resetting the fullscreen detector here
}

void X11Output::free () {
    // m_viewports holds non-owning aliases, m_screens owns the objects
    for (const auto& screen : this->m_screens) {
	delete screen;
    }

    this->m_screens.clear ();
    this->m_viewports.clear ();

    // XDestroyImage() already frees m_imageData itself (via its default destroy_image proc, which
    // calls XFree()/free() on the buffer XCreateImage() was given) - it must not be freed again here
    if (this->m_desktopWindow != None) {
	XDestroyWindow (this->m_display, this->m_desktopWindow);
	this->m_desktopWindow = None;
	this->m_desktopWindowMapped = false;
    }

    XDestroyImage (this->m_image);
    this->m_imageData = nullptr;
    XFreeGC (this->m_display, this->m_gc);
    XFreePixmap (this->m_display, this->m_pixmap);
    XCloseDisplay (this->m_display);
}

void* X11Output::getImageBuffer () const { return this->m_imageData; }

bool X11Output::renderVFlip () const { return false; }

bool X11Output::renderMultiple () const { return this->m_viewports.size () > 1; }

bool X11Output::haveImageBuffer () const { return true; }

uint32_t X11Output::getImageBufferSize () const { return this->m_imageSize; }

void X11Output::loadScreenInfo () {
    this->m_display = XOpenDisplay (nullptr);
    // recover from X disconnections instead of aborting
#ifdef HAVE_XSETIOERROREXITHANDLER
    XSetIOErrorExitHandler (this->m_display, CustomXIOErrorExitHandler, this);
#endif /* HAVE_XSETIOERROREXITHANDLER */

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

	// screen not in use, ignore it
	if (info == nullptr || info->connection != RR_Connected) {
	    continue;
	}

	XRRCrtcInfo* crtc = XRRGetCrtcInfo (this->m_display, screenResources, info->crtc);

	// screen not active, ignore it
	if (crtc == nullptr) {
	    continue;
	}

	bool inSpanGroup = false;
	for (const auto& spanGroup : this->m_context.settings.general.spanGroups) {
	    for (const auto& screen : spanGroup.screens) {
		if (screen == info->name) {
		    inSpanGroup = true;
		    break;
		}
	    }
	    if (inSpanGroup) {
		break;
	    }
	}

	if (inSpanGroup
	    || this->m_context.settings.general.screenBackgrounds.find (info->name)
		!= this->m_context.settings.general.screenBackgrounds.end ()) {
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
    bool any = false;

    for (const auto& o : this->m_screens) {
	const auto cur = this->m_context.settings.general.screenBackgrounds.find (o->name);

	if (cur != this->m_context.settings.general.screenBackgrounds.end ()) {
	    any = true;
	    break;
	}

	for (const auto& spanGroup : this->m_context.settings.general.spanGroups) {
	    for (const auto& screen : spanGroup.screens) {
		if (screen == o->name) {
		    any = true;
		    break;
		}
	    }
	    if (any) {
		break;
	    }
	}
	if (any) {
	    break;
	}
    }

    if (!any) {
	sLog.error ("No outputs could be initialized, please check the parameters and try again");
	sLog.error ("Detected outputs:");

	for (const auto& o : this->m_screens) {
	    sLog.error ("  ", o->name);
	}

	sLog.error ("Requested: ");

	for (const auto& o : this->m_context.settings.general.screenBackgrounds | std::views::keys) {
	    sLog.error ("  ", o);
	}

	sLog.exception ("Cannot continue...");
    }
}

void X11Output::initX11Background () {
    this->m_pixmap = XCreatePixmap (this->m_display, this->m_root, this->m_fullWidth, this->m_fullHeight, 24);
    this->m_gc = XCreateGC (this->m_display, this->m_pixmap, 0, nullptr);
    XFillRectangle (this->m_display, this->m_pixmap, this->m_gc, 0, 0, this->m_fullWidth, this->m_fullHeight);
    XSetWindowBackgroundPixmap (this->m_display, this->m_root, this->m_pixmap);
    // XCreateImage() takes ownership of this buffer and frees it itself (via free()) when the
    // XImage is destroyed, so it must be allocated with malloc(), not new[]
    this->m_imageSize = this->m_fullWidth * this->m_fullHeight * 4;
    this->m_imageData = static_cast<char*> (malloc (this->m_imageSize));
    this->m_image = XCreateImage (
	this->m_display, CopyFromParent, 24, ZPixmap, 0, this->m_imageData, this->m_fullWidth, this->m_fullHeight, 32, 0
    );
    this->m_driver.resizeWindow ({ this->m_fullWidth, this->m_fullHeight });
}

void X11Output::updateRender () const {
    XPutImage (
	this->m_display, this->m_pixmap, this->m_gc, this->m_image, 0, 0, 0, 0, this->m_fullWidth, this->m_fullHeight
    );

    // _XROOTPMAP_ID/ESETROOT_PMAP_ID let other programs (compositors) know about the background
    // pixmap instead of clearing it, and forces a compositor refresh (tested with picom)
    const Atom prop_root = XInternAtom (this->m_display, "_XROOTPMAP_ID", False);
    const Atom prop_esetroot = XInternAtom (this->m_display, "ESETROOT_PMAP_ID", False);
    XChangeProperty (
	this->m_display, this->m_root, prop_root, XA_PIXMAP, 32, PropModeReplace, (unsigned char*)&this->m_pixmap, 1
    );
    XChangeProperty (
	this->m_display, this->m_root, prop_esetroot, XA_PIXMAP, 32, PropModeReplace, (unsigned char*)&this->m_pixmap, 1
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

Window X11Output::findCoveringDesktopWindow (const std::vector<Window>& stacking) const {
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
	    return window;
	}
    }

    return None;
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

    // empty input region, clicks reach their desktop window
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

void X11Output::updateDesktopWindow () const {
    // root restacks and (un)maps may have moved windows around ours
    bool restacked = false;

    while (XPending (this->m_display)) {
	XEvent event;
	XNextEvent (this->m_display, &event);
	restacked |= event.type == ConfigureNotify || event.type == MapNotify || event.type == UnmapNotify
	    || event.type == DestroyNotify;
    }

    const auto now = std::chrono::steady_clock::now ();

    // desktops can also (re)start their desktop window after us
    if (now < this->m_nextDesktopCheck && !restacked) {
	return;
    }

    this->m_nextDesktopCheck = now + std::chrono::seconds (2);

    const Window covering = this->findCoveringDesktopWindow (this->stackingOrder ());

    if (covering == None) {
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

    // right above their desktop window's top level, below everything else
    const Window theirs = this->topLevelOf (covering);
    Window rootReturn, parent, *children = nullptr;
    unsigned int count = 0;
    bool inPlace = false;

    if (XQueryTree (this->m_display, this->m_root, &rootReturn, &parent, &children, &count)) {
	const Window* end = children + count;
	const Window* their = std::find (children, children + count, theirs);
	inPlace = their != end && their + 1 != end && *(their + 1) == this->m_desktopWindow;
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
}