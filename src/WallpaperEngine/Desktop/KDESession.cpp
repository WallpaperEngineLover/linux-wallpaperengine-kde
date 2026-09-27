#include "KDESession.h"

#include <cstdlib>
#include <string_view>

bool WallpaperEngine::Desktop::isKDESession () {
    if (const char* desktopSession = std::getenv ("DESKTOP_SESSION")) {
	const std::string_view session (desktopSession);
	if (session.find ("kde") != std::string_view::npos || session.find ("plasma") != std::string_view::npos) {
	    return true;
	}
    }

    if (const char* xdgDesktop = std::getenv ("XDG_SESSION_DESKTOP");
	xdgDesktop && std::string_view (xdgDesktop) == "KDE") {
	return true;
    }

    const char* kdeSession = std::getenv ("KDE_FULL_SESSION");
    return kdeSession && std::string_view (kdeSession) == "true";
}
