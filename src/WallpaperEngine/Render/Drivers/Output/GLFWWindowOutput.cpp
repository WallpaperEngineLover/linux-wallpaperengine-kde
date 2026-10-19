#include "GLFWWindowOutput.h"
#include "GLFWOutputViewport.h"
#include "WallpaperEngine/Logging/Log.h"
#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>

#include <algorithm>
#include <cstdlib>
#include <unistd.h>

using namespace WallpaperEngine::Render::Drivers::Output;

GLFWWindowOutput::GLFWWindowOutput (ApplicationContext& context, VideoDriver& driver) : Output (context, driver) {
    if (this->m_context.settings.render.mode != Application::ApplicationContext::NORMAL_WINDOW
	&& this->m_context.settings.render.mode != Application::ApplicationContext::EXPLICIT_WINDOW) {
	sLog.exception ("Initializing window output when not in output mode, how did you get here?!");
    }

    const bool background = this->m_context.settings.render.mode == Application::ApplicationContext::EXPLICIT_WINDOW
	&& this->m_context.settings.render.window.asBackground;

    if (background && driver.getX11Display () == nullptr) {
	sLog.error ("--window-background only works on X11, the window stays a normal one");
    }

    // the GL window stays hidden for a background window, it only renders
    if (!background || driver.getX11Display () == nullptr) {
	driver.showWindow ();
    }

    if (this->m_context.settings.render.mode == Application::ApplicationContext::EXPLICIT_WINDOW) {
	this->m_fullWidth = this->m_context.settings.render.window.geometry.z;
	this->m_fullHeight = this->m_context.settings.render.window.geometry.w;
	this->repositionWindow ();

	if (background && driver.getX11Display () != nullptr) {
	    this->createBackgroundWindow ();
	}
    } else {
	this->m_fullWidth = this->m_driver.getFramebufferSize ().x;
	this->m_fullHeight = this->m_driver.getFramebufferSize ().y;
    }

    this->m_viewports["default"]
	= new GLFWOutputViewport { { 0, 0, this->m_fullWidth, this->m_fullHeight }, "default" };
}

GLFWWindowOutput::~GLFWWindowOutput () {
    this->destroyBackgroundWindow ();

    for (const auto& [name, viewport] : this->m_viewports) {
	delete viewport;
    }
}

void GLFWWindowOutput::repositionWindow () const {
    this->m_driver.resizeWindow (this->m_context.settings.render.window.geometry);
}

void GLFWWindowOutput::reset () {
    if (this->m_context.settings.render.mode == Application::ApplicationContext::EXPLICIT_WINDOW) {
	this->repositionWindow ();
    }
}

void GLFWWindowOutput::createBackgroundWindow () {
    // A GL window can't sit below Cinnamon's desktop icons (it is composited over nemo-desktop whatever the
    // stacking says), a plain X window can. So this one only gets the finished frames, like the root window does
    Display* display = XOpenDisplay (nullptr);
    this->m_display = display;

    if (display == nullptr) {
	sLog.error ("Cannot open the X display for the background window");
	return;
    }

    const auto& geometry = this->m_context.settings.render.window.geometry;
    const int screen = DefaultScreen (display);

    XSetWindowAttributes attributes {};
    attributes.background_pixel = BlackPixel (display, screen);
    attributes.border_pixel = 0;

    const Window window = XCreateWindow (
	display, RootWindow (display, screen), geometry.x, geometry.y, geometry.z, geometry.w, 0, CopyFromParent,
	InputOutput, CopyFromParent, CWBackPixel | CWBorderPixel, &attributes
    );

    const auto atom = [display] (const char* name) { return XInternAtom (display, name, False); };

    const Atom type = atom ("_NET_WM_WINDOW_TYPE_DESKTOP");
    XChangeProperty (
	display, window, atom ("_NET_WM_WINDOW_TYPE"), XA_ATOM, 32, PropModeReplace,
	reinterpret_cast<const unsigned char*> (&type), 1
    );

    const Atom states[]
	= { atom ("_NET_WM_STATE_STICKY"), atom ("_NET_WM_STATE_SKIP_TASKBAR"), atom ("_NET_WM_STATE_SKIP_PAGER") };
    XChangeProperty (
	display, window, atom ("_NET_WM_STATE"), XA_ATOM, 32, PropModeReplace,
	reinterpret_cast<const unsigned char*> (states), 3
    );

    const unsigned long allDesktops = 0xFFFFFFFF;
    XChangeProperty (
	display, window, atom ("_NET_WM_DESKTOP"), XA_CARDINAL, 32, PropModeReplace,
	reinterpret_cast<const unsigned char*> (&allDesktops), 1
    );

    const unsigned long pid = getpid ();
    XChangeProperty (
	display, window, atom ("_NET_WM_PID"), XA_CARDINAL, 32, PropModeReplace,
	reinterpret_cast<const unsigned char*> (&pid), 1
    );

    // never takes the focus
    XWMHints wmHints {};
    wmHints.flags = InputHint;
    wmHints.input = False;
    XSetWMHints (display, window, &wmHints);

    // undecorated
    const unsigned long motif[] = { 2, 0, 0, 0, 0 };
    XChangeProperty (
	display, window, atom ("_MOTIF_WM_HINTS"), atom ("_MOTIF_WM_HINTS"), 32, PropModeReplace,
	reinterpret_cast<const unsigned char*> (motif), 5
    );

    // like the other X windows of ours, the clicks go to what is below
    XShapeCombineRectangles (display, window, ShapeInput, 0, 0, nullptr, 0, ShapeSet, Unsorted);

    char name[] = "linux-wallpaperengine";
    XClassHint classHint { name, name };
    XSetClassHint (display, window, &classHint);
    XStoreName (display, window, "wallpaperengine");

    this->m_background = window;
    this->m_gc = XCreateGC (display, window, 0, nullptr);
    this->m_imageSize = this->m_fullWidth * this->m_fullHeight * 4;
    // XDestroyImage () frees this with free ()
    this->m_imageData = static_cast<char*> (malloc (this->m_imageSize));
    this->m_image = XCreateImage (
	display, CopyFromParent, 24, ZPixmap, 0, this->m_imageData, this->m_fullWidth, this->m_fullHeight, 32, 0
    );

    XMapWindow (display, window);
    XFlush (display);

    sLog.out (
	"Background window 0x", std::hex, window, std::dec, " ", geometry.z, "x", geometry.w, "+", geometry.x, "+",
	geometry.y, ", screen depth ", DefaultDepth (display, screen), ", compositor ",
	XGetSelectionOwner (display, atom ("_NET_WM_CM_S0")) != None ? "running" : "not running"
    );
}

void GLFWWindowOutput::destroyBackgroundWindow () {
    auto* display = static_cast<Display*> (this->m_display);

    if (display == nullptr) {
	return;
    }

    if (this->m_image != nullptr) {
	// frees m_imageData as well
	XDestroyImage (static_cast<XImage*> (this->m_image));
	this->m_image = nullptr;
	this->m_imageData = nullptr;
    }

    if (this->m_nudge != 0) {
	XDestroyWindow (display, this->m_nudge);
	this->m_nudge = 0;
    }

    if (this->m_gc != nullptr) {
	XFreeGC (display, static_cast<GC> (this->m_gc));
	this->m_gc = nullptr;
    }

    if (this->m_background != 0) {
	XDestroyWindow (display, this->m_background);
	this->m_background = 0;
    }

    XCloseDisplay (display);
    this->m_display = nullptr;
}

bool GLFWWindowOutput::desktopWindowBelowUs () const {
    auto* display = static_cast<Display*> (this->m_display);
    const Atom listAtom = XInternAtom (display, "_NET_CLIENT_LIST_STACKING", False);
    const Atom typeAtom = XInternAtom (display, "_NET_WM_WINDOW_TYPE", False);
    const Atom desktopAtom = XInternAtom (display, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    Atom type;
    int format;
    unsigned long count = 0, remaining;
    unsigned char* data = nullptr;
    bool below = false;

    if (XGetWindowProperty (
	    display, DefaultRootWindow (display), listAtom, 0, 4096, False, XA_WINDOW, &type, &format, &count,
	    &remaining, &data
	) != Success
	|| data == nullptr || format != 32) {
	return false;
    }

    // bottom to top
    const auto* windows = reinterpret_cast<const Window*> (data);
    unsigned long ours = count;

    for (unsigned long i = 0; i < count; i++) {
	if (windows[i] == this->m_background) {
	    ours = i;
	}
    }

    for (unsigned long i = 0; i < ours && !below; i++) {
	Atom windowType;
	int windowFormat;
	unsigned long typeCount, typeRemaining;
	unsigned char* typeData = nullptr;

	if (XGetWindowProperty (
		display, windows[i], typeAtom, 0, 16, False, XA_ATOM, &windowType, &windowFormat, &typeCount,
		&typeRemaining, &typeData
	    ) == Success
	    && typeData != nullptr && windowFormat == 32) {
	    const auto* atoms = reinterpret_cast<const Atom*> (typeData);
	    below = std::find (atoms, atoms + typeCount, desktopAtom) != atoms + typeCount;
	}

	if (typeData != nullptr) {
	    XFree (typeData);
	}
    }

    XFree (data);

    return below;
}

void GLFWWindowOutput::keepBackgroundWindowLow () const {
    auto* display = static_cast<Display*> (this->m_display);
    const uint32_t tick = this->m_updates++;

    if (this->m_nudge != 0 && tick >= this->m_nudgeUntil) {
	XDestroyWindow (display, this->m_nudge);
	this->m_nudge = 0;
    }

    if (tick % 60 != 5 || this->m_nudge != 0 || !this->desktopWindowBelowUs ()) {
	return;
    }

    // Desktop windows stack by age and nemo-desktop is one too, so ours goes below it. Muffin keeps those in a group
    // of their own and only syncs it to the new order when a normal window changes the stacking as well, which on an
    // empty desktop never happens. A normal window, invisible and gone again a moment later, makes it happen.
    XLowerWindow (display, this->m_background);

    XSetWindowAttributes attributes {};
    attributes.background_pixel = BlackPixel (display, DefaultScreen (display));
    const Window nudge = XCreateWindow (
	display, DefaultRootWindow (display), 0, 0, 1, 1, 0, CopyFromParent, InputOutput, CopyFromParent, CWBackPixel,
	&attributes
    );

    const auto atom = [display] (const char* name) { return XInternAtom (display, name, False); };
    const unsigned long transparent = 0;
    XChangeProperty (
	display, nudge, atom ("_NET_WM_WINDOW_OPACITY"), XA_CARDINAL, 32, PropModeReplace,
	reinterpret_cast<const unsigned char*> (&transparent), 1
    );
    const Atom states[] = { atom ("_NET_WM_STATE_SKIP_TASKBAR"), atom ("_NET_WM_STATE_SKIP_PAGER") };
    XChangeProperty (
	display, nudge, atom ("_NET_WM_STATE"), XA_ATOM, 32, PropModeReplace,
	reinterpret_cast<const unsigned char*> (states), 2
    );
    XWMHints wmHints {};
    wmHints.flags = InputHint;
    wmHints.input = False;
    XSetWMHints (display, nudge, &wmHints);
    XShapeCombineRectangles (display, nudge, ShapeInput, 0, 0, nullptr, 0, ShapeSet, Unsorted);

    XMapWindow (display, nudge);
    this->m_nudge = nudge;
    this->m_nudgeUntil = tick + 6;
}

bool GLFWWindowOutput::frameIsBlack () const {
    const auto* pixels = reinterpret_cast<const uint32_t*> (this->m_imageData);
    const size_t count = this->m_imageSize / 4;

    // every 97th pixel is plenty, a wallpaper that is black there is black
    for (size_t i = 0; i < count; i += 97) {
	if ((pixels[i] & 0x00FFFFFF) != 0) {
	    return false;
	}
    }

    return true;
}

bool GLFWWindowOutput::renderVFlip () const { return this->m_image == nullptr; }

bool GLFWWindowOutput::renderMultiple () const { return false; }

bool GLFWWindowOutput::haveImageBuffer () const { return this->m_image != nullptr; }

void* GLFWWindowOutput::getImageBuffer () const { return this->m_imageData; }

uint32_t GLFWWindowOutput::getImageBufferSize () const { return this->m_imageSize; }

void GLFWWindowOutput::updateRender () const {
    // re-read every frame so window resizes re-stretch the scene instead of cropping it, the image buffer of a
    // background window has a fixed size
    if (this->m_image == nullptr) {
	this->m_fullWidth = this->m_driver.getFramebufferSize ().x;
	this->m_fullHeight = this->m_driver.getFramebufferSize ().y;
    }

    this->m_viewports["default"]->viewport = { 0, 0, this->m_fullWidth, this->m_fullHeight };

    if (this->m_image == nullptr) {
	return;
    }

    auto* display = static_cast<Display*> (this->m_display);

    if (this->m_updates == 90 && this->frameIsBlack ()) {
	sLog.error (
	    "The background window got 90 frames and every pixel is black. Either the wallpaper is black or the frames "
	    "never reach the window; the line above names the window to look for with xwininfo"
	);
    }

    XPutImage (
	display, this->m_background, static_cast<GC> (this->m_gc), static_cast<XImage*> (this->m_image), 0, 0, 0, 0,
	this->m_fullWidth, this->m_fullHeight
    );

    this->keepBackgroundWindowLow ();

    XFlush (display);
}