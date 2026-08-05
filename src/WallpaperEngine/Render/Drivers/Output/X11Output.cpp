#include "X11Output.h"
#include "GLFWOutputViewport.h"
#include "WallpaperEngine/Logging/Log.h"

#include <cstdlib>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

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
    XFlush (this->m_display);
}
