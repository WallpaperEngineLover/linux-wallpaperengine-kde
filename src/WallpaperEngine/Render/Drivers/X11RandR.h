#pragma once

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

namespace WallpaperEngine::Render::Drivers {
/** Calls visit with the info and crtc of every connected output that has one */
template <typename Visitor>
void forEachConnectedOutput (Display* display, XRRScreenResources* resources, Visitor&& visit) {
    for (int i = 0; i < resources->noutput; i++) {
	XRROutputInfo* info = XRRGetOutputInfo (display, resources, resources->outputs[i]);

	if (info == nullptr) {
	    continue;
	}

	if (info->connection == RR_Connected) {
	    if (XRRCrtcInfo* crtc = XRRGetCrtcInfo (display, resources, info->crtc); crtc != nullptr) {
		visit (*info, *crtc);
		XRRFreeCrtcInfo (crtc);
	    }
	}

	XRRFreeOutputInfo (info);
    }
}
}
