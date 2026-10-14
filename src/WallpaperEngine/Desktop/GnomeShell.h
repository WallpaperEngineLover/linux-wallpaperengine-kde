#pragma once

#include <dbus/dbus.h>
#include <glm/vec2.hpp>
#include <string>
#include <vector>

#include "WindowState.h"

namespace WallpaperEngine::Desktop {
/** XDG_CURRENT_DESKTOP lists GNOME */
bool isGnomeSession ();

/**
 * Client of our GNOME Shell extension: it makes our toplevels desktop windows and reports the pointer and window
 * states Mutter hides from Wayland clients. Read only from the render thread via dispatch ().
 */
class GnomeShell {
public:
    static constexpr const char* BUS_NAME = "org.linuxwallpaperengine.Shell";
    static constexpr const char* OBJECT_PATH = "/org/linuxwallpaperengine/Shell";
    static constexpr const char* INTERFACE = "org.linuxwallpaperengine.Shell";
    /** followed by the output name */
    static constexpr const char* TITLE_PREFIX = "linux-wallpaperengine wallpaper ";
    static constexpr const char* APP_ID = "linux-wallpaperengine";
    static constexpr uint32_t API_VERSION = 1;

    using Window = WindowState;

    struct Pointer {
	std::string output;
	/** fraction of the window, from its top left */
	glm::dvec2 position;
    };

    GnomeShell ();
    ~GnomeShell ();

    GnomeShell (const GnomeShell&) = delete;
    GnomeShell& operator= (const GnomeShell&) = delete;

    [[nodiscard]] bool isAvailable () const { return this->m_available; }
    [[nodiscard]] const std::string& reason () const { return this->m_reason; }
    /** disabled or Shell restarted */
    [[nodiscard]] bool vanished () const { return this->m_vanished; }
    /** GNOME 45-48: needs a Shell-spawned connection, see openWaylandSocket */
    [[nodiscard]] bool needsHelper () const { return this->m_needsHelper; }
    // the extension spawns the helper via Meta.WaylandClient, returns its connection or -1; Mutter drops the client
    // when the helper exits
    [[nodiscard]] int openWaylandSocket ();

    /** non-blocking */
    void dispatch ();

    [[nodiscard]] const std::vector<Pointer>& pointer () const { return this->m_pointer; }
    [[nodiscard]] const std::vector<Window>& windows () const { return this->m_windows; }
    /** left button held on another desktop window (icons) */
    [[nodiscard]] bool desktopLeftDown () const { return this->m_desktopLeftDown; }

private:
    bool connect ();
    bool queryState ();
    static DBusHandlerResult filter (DBusConnection* connection, DBusMessage* message, void* data);
    void handleSignal (DBusMessage* message);

    DBusConnection* m_connection = nullptr;
    std::string m_owner;
    std::string m_reason;
    bool m_available = false;
    bool m_vanished = false;
    bool m_needsHelper = false;
    bool m_filterAdded = false;
    bool m_desktopLeftDown = false;
    int m_helperConnection = -1;
    std::vector<Pointer> m_pointer;
    std::vector<Window> m_windows;
};
} // namespace WallpaperEngine::Desktop
