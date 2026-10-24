#include "WaylandMouseInput.h"
#include "WallpaperEngine/Desktop/HyprlandIPC.h"
#include "WallpaperEngine/Input/ForwardedButton.h"
#include "WallpaperEngine/Render/Drivers/WaylandOpenGLDriver.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <glm/common.hpp>
#include <regex>
#include <string>

#ifdef ENABLE_X11
#include <X11/Xlib.h>
#endif

#ifdef ENABLE_KDE_FEATURES
#include "WallpaperEngine/Desktop/KDESession.h"
#endif

using namespace WallpaperEngine::Input::Drivers;

WaylandMouseInput::WaylandMouseInput (const WallpaperEngine::Render::Drivers::WaylandOpenGLDriver& driver) :
    m_waylandDriver (driver) {
#ifdef ENABLE_KDE_FEATURES
    // outside Plasma there is no KWin to load the cursor script into
    if (WallpaperEngine::Desktop::isKDESession ()) {
	this->m_kdeCursor = std::make_unique<KDECursorInput> ();
    }
#endif

    this->m_hyprlandSocket = findHyprlandSocket ();
    this->m_lastHyprlandLookup = std::chrono::steady_clock::now ();
}

std::string WaylandMouseInput::findHyprlandSocket () {
    const char* runtime = std::getenv ("XDG_RUNTIME_DIR");
    return WallpaperEngine::Desktop::findHyprlandSocket (
	runtime != nullptr ? runtime : "", std::getenv ("HYPRLAND_INSTANCE_SIGNATURE"), std::getenv ("WAYLAND_DISPLAY"),
	std::getenv ("XDG_CURRENT_DESKTOP")
    );
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

    // COSMIC: cosmic-files' desktop surface takes the pointer, the compositor's cursor capture still knows where it is
    if (this->m_waylandDriver.hasCapturedCursor ()) {
	if (const auto captured = this->m_waylandDriver.capturedCursorPosition (); captured.has_value ()) {
	    this->m_globalCursor = captured;
	    this->m_globalCursorFromX11 = false;
	}

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
#endif

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
#endif
    this->m_globalCursor = globalCursor;
}

bool WaylandMouseInput::hasGlobalCursor () const {
    if (this->m_waylandDriver.getGnomeShell () != nullptr || this->m_waylandDriver.hasCapturedCursor ()) {
	return true;
    }

#ifdef ENABLE_KDE_FEATURES
    if (this->m_kdeCursor && this->m_kdeCursor->isInitialized ()) {
	return true;
    }
#endif

    return !this->m_hyprlandSocket.empty ();
}

glm::dvec2
WaylandMouseInput::positionOn (const WallpaperEngine::Render::Drivers::Output::WaylandOutputViewport& viewport) const {
    const double scale = viewport.renderScale ();
    const auto fromDesktop = [&viewport, scale] (const glm::dvec2& desktop) -> glm::dvec2 {
	const glm::dvec2 local = desktop - glm::dvec2 (viewport.globalPosition);
	return { local.x * scale, (viewport.size.y - local.y) * scale };
    };

    // WE maps GetCursorPos to each wallpaper window's client coords (sub_140110630), even off that output: a pointer
    // on another monitor gives coordinates outside this one (live WE under Wine, tools/wecompare/wine/we_span.sh)
    if (const auto* gnomeShell = this->m_waylandDriver.getGnomeShell ()) {
	for (const auto& [output, fraction] : gnomeShell->pointer ()) {
	    if (output == viewport.name) {
		return { fraction.x * viewport.size.x * scale, (1.0 - fraction.y) * viewport.size.y * scale };
	    }
	}

	const auto desktop = this->desktopPosition ();
	return desktop.has_value () ? fromDesktop (*desktop) : glm::dvec2 { 0, 0 };
    }

    if (this->m_waylandDriver.viewportInFocus == &viewport) {
	return viewport.mousePos;
    }

    if (this->m_globalCursor.has_value () && !(this->m_globalCursorFromX11 && viewport.pointerEntered)) {
	return fromDesktop (*this->m_globalCursor);
    }

    // the pointer is on another output's surface of ours, whose position is as fresh as it gets
    if (this->m_waylandDriver.viewportInFocus != nullptr) {
	if (const auto desktop = this->desktopPosition ()) {
	    return fromDesktop (*desktop);
	}
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

std::optional<glm::dvec2> WaylandMouseInput::desktopPosition () const {
    if (!this->m_waylandDriver.getApp ().getContext ().settings.mouse.enabled) {
	return std::nullopt;
    }

    if (const auto* gnomeShell = this->m_waylandDriver.getGnomeShell ()) {
	for (const auto& [output, fraction] : gnomeShell->pointer ()) {
	    for (const auto* viewport : this->m_waylandDriver.m_screens) {
		if (viewport && viewport->name == output) {
		    return glm::dvec2 (viewport->globalPosition) + fraction * glm::dvec2 (viewport->size);
		}
	    }
	}

	return std::nullopt;
    }

    if (const auto* viewport = this->m_waylandDriver.viewportInFocus) {
	const glm::dvec2 local = viewport->mousePos / viewport->renderScale ();
	return glm::dvec2 (viewport->globalPosition) + glm::dvec2 { local.x, viewport->size.y - local.y };
    }

    return this->m_globalCursor;
}

WallpaperEngine::Input::MouseClickStatus WaylandMouseInput::desktopLeftClick () const {
    const auto click = this->leftClick ();
    const auto* viewport = this->m_waylandDriver.viewportInFocus;

    // leftClick () only looks at the output being rendered, the press may be on another output of the span
    if (click == MouseClickStatus::Released && viewport != nullptr
	&& this->m_waylandDriver.getApp ().getContext ().settings.mouse.enabled) {
	return viewport->leftClick;
    }

    return click;
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

std::optional<glm::dvec2> WaylandMouseInput::queryHyprlandCursorPosition () {
    std::error_code error;

    if (this->m_hyprlandSocket.empty () || !std::filesystem::exists (this->m_hyprlandSocket, error)) {
	const auto now = std::chrono::steady_clock::now ();
	if (now - this->m_lastHyprlandLookup < std::chrono::seconds (1)) {
	    return std::nullopt;
	}
	this->m_lastHyprlandLookup = now;
	this->m_hyprlandSocket = findHyprlandSocket ();
    }

    if (this->m_hyprlandSocket.empty ()) {
	return std::nullopt;
    }

    const auto reply = WallpaperEngine::Desktop::hyprlandRequest (this->m_hyprlandSocket, "j/cursorpos");
    if (!reply.has_value ()) {
	return std::nullopt;
    }
    const std::string& response = *reply;

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
#endif

WallpaperEngine::Input::MouseClickStatus WaylandMouseInput::rightClick () const {
    const auto* viewport = this->getActiveOutputViewport ();

    if (viewport) {
	return viewport->rightClick;
    }

    return MouseClickStatus::Released;
}
