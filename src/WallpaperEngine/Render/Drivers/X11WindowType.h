#pragma once

#include <algorithm>

#include <X11/Xatom.h>
#include <X11/Xlib.h>

namespace WallpaperEngine::Render::Drivers {
inline bool hasDesktopWindowType (Display* display, Window window) {
    const Atom typeAtom = XInternAtom (display, "_NET_WM_WINDOW_TYPE", False);
    const Atom desktopAtom = XInternAtom (display, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    Atom type;
    int format;
    unsigned long count, remaining;
    unsigned char* data = nullptr;
    bool desktop = false;

    if (XGetWindowProperty (display, window, typeAtom, 0, 16, False, XA_ATOM, &type, &format, &count, &remaining, &data)
	    == Success
	&& data != nullptr && format == 32) {
	const auto* atoms = reinterpret_cast<const Atom*> (data);
	desktop = std::find (atoms, atoms + count, desktopAtom) != atoms + count;
    }

    if (data != nullptr) {
	XFree (data);
    }

    return desktop;
}
}
