#pragma once

#include <cstdint>
#include <dbus/dbus.h>
#include <string>
#include <vector>

namespace WallpaperEngine::Desktop {
bool isPantheonSession ();

/**
 * Gala's DesktopIntegration interface (what elementary's dock uses): the only way to see native Wayland windows there,
 * Gala implements no foreign toplevel protocol. It gives size, focus, hidden and workspace but no fullscreen or
 * maximized flag and no position, so a window that is at least as big as an output counts as covering it.
 */
class PantheonShell {
public:
    static constexpr const char* BUS_NAME = "org.pantheon.gala";
    static constexpr const char* OBJECT_PATH = "/org/pantheon/gala/DesktopInterface";
    static constexpr const char* INTERFACE = "org.pantheon.gala.DesktopIntegration";

    struct Window {
	std::string wmClass;
	uint32_t width = 0;
	uint32_t height = 0;
	int32_t workspace = 0;
	bool focused = false;
	bool hidden = false;
    };

    PantheonShell ();
    ~PantheonShell ();

    PantheonShell (const PantheonShell&) = delete;
    PantheonShell& operator= (const PantheonShell&) = delete;

    [[nodiscard]] bool isAvailable () const { return this->m_available; }
    [[nodiscard]] const std::string& reason () const { return this->m_reason; }

    void dispatch ();

    [[nodiscard]] const std::vector<Window>& windows () const { return this->m_windows; }
    [[nodiscard]] int32_t activeWorkspace () const { return this->m_activeWorkspace; }

private:
    bool connect ();
    void refresh ();
    static DBusHandlerResult filter (DBusConnection* connection, DBusMessage* message, void* data);

    DBusConnection* m_connection = nullptr;
    std::string m_owner;
    std::string m_reason;
    bool m_available = false;
    bool m_filterAdded = false;
    bool m_dirty = false;
    int32_t m_activeWorkspace = 0;
    std::vector<Window> m_windows;
};
}
