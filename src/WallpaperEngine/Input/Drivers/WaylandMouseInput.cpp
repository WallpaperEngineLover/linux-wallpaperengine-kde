#include "WaylandMouseInput.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Drivers/WaylandOpenGLDriver.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <glm/common.hpp>
#include <regex>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef ENABLE_X11
#include <X11/Xlib.h>
#endif /* ENABLE_X11 */

using namespace WallpaperEngine::Input::Drivers;

WaylandMouseInput::WaylandMouseInput (const WallpaperEngine::Render::Drivers::WaylandOpenGLDriver& driver) :
    m_waylandDriver (driver) { }

void WaylandMouseInput::update () {
    static auto lastDebugLog = std::chrono::steady_clock::time_point ();
    const bool shouldLog = std::chrono::steady_clock::now () - lastDebugLog > std::chrono::seconds (1);

    if (!this->m_waylandDriver.getApp ().getContext ().settings.mouse.enabled) {
	this->m_pos = { 0, 0 };
	return;
    }

    if (m_waylandDriver.viewportInFocus && m_waylandDriver.viewportInFocus->rendering) {
	this->m_pos = m_waylandDriver.viewportInFocus->mousePos;
	if (shouldLog) {
	    lastDebugLog = std::chrono::steady_clock::now ();
	    sLog.out ("[mouse-debug] using native viewportInFocus->mousePos = ", this->m_pos.x, ",", this->m_pos.y);
	}
	return;
    }

#ifdef ENABLE_KDE_EXPERIMENTAL_FEATURES
    if (const auto kdeCursor = this->m_kdeCursor.position (); kdeCursor.has_value ()) {
	if (this->matchViewport (*kdeCursor, "kde", shouldLog)) {
	    if (shouldLog) {
		lastDebugLog = std::chrono::steady_clock::now ();
	    }
	    return;
	}
    }
#endif /* ENABLE_KDE_EXPERIMENTAL_FEATURES */

    const auto now = std::chrono::steady_clock::now ();
    if (now - this->m_lastGlobalCursorQuery < std::chrono::milliseconds (16)) {
	return;
    }
    this->m_lastGlobalCursorQuery = now;

    auto globalCursor = this->queryHyprlandCursorPosition ();
    const char* source = "hyprland";
#ifdef ENABLE_X11
    if (!globalCursor.has_value ()) {
	globalCursor = this->queryX11CursorPosition ();
	source = "x11";
    }
#endif /* ENABLE_X11 */
    if (!globalCursor.has_value ()) {
	if (shouldLog) {
	    lastDebugLog = now;
	    sLog.out ("[mouse-debug] no viewportInFocus and no global cursor query succeeded (hyprland/x11 both failed)");
	}
	this->m_pos = { 0, 0 };
	return;
    }

    if (this->matchViewport (*globalCursor, source, shouldLog)) {
	if (shouldLog) {
	    lastDebugLog = now;
	}
	return;
    }

    if (shouldLog) {
	lastDebugLog = now;
	sLog.out ("[mouse-debug] global cursor (source=", source, ") did not match any viewport bounds");
    }
    this->m_pos = { 0, 0 };
}

bool WaylandMouseInput::matchViewport (const glm::dvec2& globalCursor, const char* source, bool shouldLog) {
    if (shouldLog) {
	sLog.out (
	    "[mouse-debug] source=", source, " global cursor = ", globalCursor.x, ",", globalCursor.y,
	    " screens=", this->m_waylandDriver.m_screens.size ()
	);
	for (const auto* viewport : this->m_waylandDriver.m_screens) {
	    if (!viewport) {
		continue;
	    }
	    sLog.out (
		"[mouse-debug]   viewport pos=", viewport->position.x, ",", viewport->position.y,
		" size=", viewport->size.x, ",", viewport->size.y, " scale=", viewport->scale
	    );
	}
    }

    for (const auto* viewport : this->m_waylandDriver.m_screens) {
	if (!viewport || viewport->size.x <= 0 || viewport->size.y <= 0) {
	    continue;
	}

	const double localX = globalCursor.x - viewport->position.x;
	const double localY = globalCursor.y - viewport->position.y;
	if (localX < 0.0 || localY < 0.0 || localX > viewport->size.x || localY > viewport->size.y) {
	    continue;
	}

	this->m_pos = { localX * viewport->scale, (viewport->size.y - localY) * viewport->scale };
	if (shouldLog) {
	    sLog.out ("[mouse-debug] matched viewport (source=", source, "), m_pos = ", this->m_pos.x, ",", this->m_pos.y);
	}
	return true;
    }

    return false;
}

glm::dvec2 WaylandMouseInput::position () const {
    if (!this->m_waylandDriver.getApp ().getContext ().settings.mouse.enabled) {
	return { 0, 0 };
    }

    return this->m_pos;
}

WallpaperEngine::Input::MouseClickStatus WaylandMouseInput::leftClick () const {
    const auto* viewport = this->getActiveOutputViewport ();
    if (viewport) {
	return viewport->leftClick;
    }

    return MouseClickStatus::Released;
}

const WallpaperEngine::Render::Drivers::Output::WaylandOutputViewport*
WaylandMouseInput::getActiveOutputViewport () const {
    if (this->m_waylandDriver.viewportInFocus && this->m_waylandDriver.viewportInFocus->rendering) {
	return this->m_waylandDriver.viewportInFocus;
    }

    for (const auto* viewport : this->m_waylandDriver.m_screens) {
	if (viewport && viewport->rendering) {
	    return viewport;
	}
    }

    return nullptr;
}

std::optional<glm::dvec2> WaylandMouseInput::queryHyprlandCursorPosition () const {
    const char* signature = std::getenv ("HYPRLAND_INSTANCE_SIGNATURE");
    const char* runtime = std::getenv ("XDG_RUNTIME_DIR");
    if (!signature || !runtime) {
	return std::nullopt;
    }

    const std::string socketPath = std::string (runtime) + "/hypr/" + signature + "/.socket.sock";

    int fd = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
	return std::nullopt;
    }

    timeval timeout {};
    timeout.tv_usec = 50000;
    if (setsockopt (fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof (timeout)) != 0
	|| setsockopt (fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof (timeout)) != 0) {
	close (fd);
	return std::nullopt;
    }

    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    if (socketPath.size () >= sizeof (addr.sun_path)) {
	close (fd);
	return std::nullopt;
    }
    std::strncpy (addr.sun_path, socketPath.c_str (), sizeof (addr.sun_path) - 1);

    if (connect (fd, reinterpret_cast<sockaddr*> (&addr), sizeof (addr)) != 0) {
	close (fd);
	return std::nullopt;
    }

    constexpr const char* request = "j/cursorpos";
    if (send (fd, request, std::strlen (request), MSG_NOSIGNAL) < 0) {
	close (fd);
	return std::nullopt;
    }
    shutdown (fd, SHUT_WR);

    std::string response;
    char buffer[256];
    ssize_t readBytes = 0;
    while ((readBytes = recv (fd, buffer, sizeof (buffer), 0)) > 0) {
	response.append (buffer, static_cast<std::size_t> (readBytes));
    }
    close (fd);

    static const std::regex xRegex (R"("x"\s*:\s*(-?\d+(?:\.\d+)?))");
    static const std::regex yRegex (R"("y"\s*:\s*(-?\d+(?:\.\d+)?))");
    std::smatch xMatch;
    std::smatch yMatch;
    if (!std::regex_search (response, xMatch, xRegex) || !std::regex_search (response, yMatch, yRegex)) {
	return std::nullopt;
    }

    try {
	return glm::dvec2 { std::stod (xMatch[1].str ()), std::stod (yMatch[1].str ()) };
    } catch (const std::exception&) {
	return std::nullopt;
    }
}

#ifdef ENABLE_X11
std::optional<glm::dvec2> WaylandMouseInput::queryX11CursorPosition () const {
    Display* display = XOpenDisplay (nullptr);
    if (!display) {
	return std::nullopt;
    }

    const Window root = DefaultRootWindow (display);
    Window returnedRoot = 0;
    Window returnedChild = 0;
    int rootX = 0;
    int rootY = 0;
    int childX = 0;
    int childY = 0;
    unsigned int mask = 0;

    const Bool ok = XQueryPointer (
	display, root, &returnedRoot, &returnedChild, &rootX, &rootY, &childX, &childY, &mask
    );

    XCloseDisplay (display);

    if (!ok) {
	return std::nullopt;
    }

    return glm::dvec2 { static_cast<double> (rootX), static_cast<double> (rootY) };
}
#endif /* ENABLE_X11 */

WallpaperEngine::Input::MouseClickStatus WaylandMouseInput::rightClick () const {
    const auto* viewport = this->getActiveOutputViewport ();

    if (viewport) {
	return viewport->rightClick;
    }

    return MouseClickStatus::Released;
}
