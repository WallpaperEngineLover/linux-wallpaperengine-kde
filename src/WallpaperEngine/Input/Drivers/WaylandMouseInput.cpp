#include "WaylandMouseInput.h"
#include "WallpaperEngine/Input/ForwardedButton.h"
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

#ifdef ENABLE_KDE_FEATURES
#include "WallpaperEngine/Desktop/KDESession.h"
#endif /* ENABLE_KDE_FEATURES */

using namespace WallpaperEngine::Input::Drivers;

WaylandMouseInput::WaylandMouseInput (const WallpaperEngine::Render::Drivers::WaylandOpenGLDriver& driver) :
    m_waylandDriver (driver) {
#ifdef ENABLE_KDE_FEATURES
    // outside Plasma there is no KWin to load the cursor script into
    if (WallpaperEngine::Desktop::isKDESession ()) {
	this->m_kdeCursor = std::make_unique<KDECursorInput> ();
    }
#endif /* ENABLE_KDE_FEATURES */
}

void WaylandMouseInput::pollForwardedInput () {
    const auto now = std::chrono::steady_clock::now ();

    if (now - this->m_lastForwardedPoll < std::chrono::milliseconds (16)) {
	return;
    }

    this->m_lastForwardedPoll = now;
    this->m_forwardedLeftDown
	= forwardedLeftDown (this->m_waylandDriver.getApp ().getContext ().settings.mouse.inputFile);
}

void WaylandMouseInput::update () {
    if (!this->m_waylandDriver.getApp ().getContext ().settings.mouse.enabled) {
	this->m_globalCursor.reset ();
	this->m_forwardedLeftDown = false;
	return;
    }

    this->pollForwardedInput ();

    // GNOME Shell signals arrive with the Wayland events
    if (this->m_waylandDriver.getGnomeShell () != nullptr) {
	return;
    }

#ifdef ENABLE_KDE_FEATURES
    if (this->m_kdeCursor) {
	if (const auto kdeCursor = this->m_kdeCursor->position (); kdeCursor.has_value ()) {
	    this->m_globalCursor = kdeCursor;
	    this->m_globalCursorFromX11 = false;
	    return;
	}
    }
#endif /* ENABLE_KDE_FEATURES */

    const auto now = std::chrono::steady_clock::now ();
    if (now - this->m_lastGlobalCursorQuery < std::chrono::milliseconds (16)) {
	return;
    }
    this->m_lastGlobalCursorQuery = now;

    auto globalCursor = this->queryHyprlandCursorPosition ();
    this->m_globalCursorFromX11 = false;
#ifdef ENABLE_X11
    if (!globalCursor.has_value ()) {
	globalCursor = this->queryX11CursorPosition ();
	this->m_globalCursorFromX11 = globalCursor.has_value ();
    }
#endif /* ENABLE_X11 */
    this->m_globalCursor = globalCursor;
}

bool WaylandMouseInput::hasGlobalCursor () const {
    if (this->m_waylandDriver.getGnomeShell () != nullptr) {
	return true;
    }

#ifdef ENABLE_KDE_FEATURES
    if (this->m_kdeCursor && this->m_kdeCursor->isInitialized ()) {
	return true;
    }
#endif /* ENABLE_KDE_FEATURES */

    return std::getenv ("HYPRLAND_INSTANCE_SIGNATURE") != nullptr;
}

glm::dvec2
WaylandMouseInput::positionOn (const WallpaperEngine::Render::Drivers::Output::WaylandOutputViewport& viewport) const {
    const double scale = viewport.scale;

    // WE maps GetCursorPos to each wallpaper window's client coords (sub_140110630), even off that output
    if (const auto* gnomeShell = this->m_waylandDriver.getGnomeShell ()) {
	for (const auto& [output, fraction] : gnomeShell->pointer ()) {
	    if (output == viewport.name) {
		return { fraction.x * viewport.size.x * scale, (1.0 - fraction.y) * viewport.size.y * scale };
	    }
	}

	return { 0, 0 };
    }

    if (this->m_waylandDriver.viewportInFocus == &viewport) {
	return viewport.mousePos;
    }

    // prefer this surface's own position over a stale XWayland one
    if (this->m_globalCursor.has_value () && !(this->m_globalCursorFromX11 && viewport.pointerEntered)) {
	const glm::dvec2 local = *this->m_globalCursor - glm::dvec2 (viewport.globalPosition);
	return { local.x * scale, (viewport.size.y - local.y) * scale };
    }

    return viewport.mousePos;
}

glm::dvec2 WaylandMouseInput::position () const {
    if (!this->m_waylandDriver.getApp ().getContext ().settings.mouse.enabled) {
	return { 0, 0 };
    }

    const auto* viewport = this->getActiveOutputViewport ();
    return viewport != nullptr ? this->positionOn (*viewport) : glm::dvec2 { 0, 0 };
}

WallpaperEngine::Input::MouseClickStatus WaylandMouseInput::leftClick () const {
    if (!this->m_waylandDriver.getApp ().getContext ().settings.mouse.enabled) {
	return MouseClickStatus::Released;
    }

    if (this->m_forwardedLeftDown) {
	return MouseClickStatus::Clicked;
    }

    // WE counts a press on the desktop icons' window as a desktop press
    if (const auto* gnomeShell = this->m_waylandDriver.getGnomeShell (); gnomeShell && gnomeShell->desktopLeftDown ()) {
	return MouseClickStatus::Clicked;
    }

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

    const Bool ok
	= XQueryPointer (display, root, &returnedRoot, &returnedChild, &rootX, &rootY, &childX, &childY, &mask);

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
