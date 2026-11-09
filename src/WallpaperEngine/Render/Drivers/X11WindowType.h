#pragma once

#include <algorithm>
#include <vector>

#include <X11/Xatom.h>
#include <X11/Xlib.h>

namespace WallpaperEngine::Render::Drivers {
/** The atoms of an XA_ATOM property of a window, empty when it is not set */
inline std::vector<Atom> windowAtoms (Display* display, Window window, const char* property) {
    const Atom propertyAtom = XInternAtom (display, property, False);
    Atom type;
    int format;
    unsigned long count, remaining;
    unsigned char* data = nullptr;
    std::vector<Atom> atoms;

    if (XGetWindowProperty (
	    display, window, propertyAtom, 0, 32, False, XA_ATOM, &type, &format, &count, &remaining, &data
	) == Success
	&& data != nullptr && format == 32) {
	const auto* values = reinterpret_cast<const Atom*> (data);
	atoms.assign (values, values + count);
    }

    if (data != nullptr) {
	XFree (data);
    }

    return atoms;
}

inline bool hasAtom (const std::vector<Atom>& atoms, const Atom atom) {
    return std::find (atoms.begin (), atoms.end (), atom) != atoms.end ();
}

inline bool hasDesktopWindowType (Display* display, Window window) {
    return hasAtom (
	windowAtoms (display, window, "_NET_WM_WINDOW_TYPE"),
	XInternAtom (display, "_NET_WM_WINDOW_TYPE_DESKTOP", False)
    );
}
}
