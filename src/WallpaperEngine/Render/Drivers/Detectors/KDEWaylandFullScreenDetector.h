#pragma once

#ifdef ENABLE_WAYLAND
#ifdef ENABLE_KDE_EXPERIMENTAL_FEATURES

#include <dbus/dbus.h>
#include <string>
#include <unordered_map>

#include "FullScreenDetector.h"

namespace WallpaperEngine::Render::Drivers::Detectors {

/**
 * KDE Plasma/KWin fullscreen and maximize detector using D-Bus.
 *
 * Registers itself as a D-Bus service on the session bus so that a companion
 * KWin script can call OnWindowChanged whenever a window's maximize or
 * fullscreen state changes.
 *
 * If D-Bus initialization fails, m_connection is left null and the caller is
 * expected to fall back to WaylandFullScreenDetector instead.
 */
class KDEWaylandFullScreenDetector final : public FullScreenDetector {
public:
    explicit KDEWaylandFullScreenDetector (Application::ApplicationContext& appContext);
    ~KDEWaylandFullScreenDetector () override;

    /**
     * True if a non-ignored window is fullscreen/fully maximized, per the window-state map
     * populated by OnWindowChanged. When pauseOnFullscreenOnlyWhenActive is set, only the
     * most-recently-activated window is examined; app IDs in fullscreenPauseIgnoreAppIds
     * are excluded (substring match).
     */
    [[nodiscard]] bool anythingFullscreen () const override;

    [[nodiscard]] bool isInitialized () const;

    void reset () override;

private:
    struct WindowState {
	bool horizontal = false; ///< Window spans the full horizontal extent of its output.
	bool vertical = false; ///< Window spans the full vertical extent of its output.
	bool fully = false; ///< Window is fully maximized or in true fullscreen mode.
	std::string appId {}; ///< Wayland app-id / desktop file name, used for ignore-list matching.
    };

    /** C-linkage trampoline required by the libdbus object-path vtable; forwards to the instance overload. */
    static DBusHandlerResult handleMessage (DBusConnection* connection, DBusMessage* message, void* userData);

    DBusHandlerResult handleMessage (DBusMessage* message);

    bool initializeDBus ();
    void stopDBus ();

    /**
     * Parses an OnWindowChanged method call. Expected args, in order:
     * STRING windowKey, STRING windowName, INT32 pid, STRING appId,
     * BOOLEAN horizontal, BOOLEAN vertical, BOOLEAN fully.
     */
    bool handleMethodCall (DBusMessage* message);

    bool updateWindowState (
	const std::string& windowKey, const std::string& appId, bool horizontal, bool vertical, bool fully
    );

    mutable DBusConnection* m_connection = nullptr;
    mutable std::unordered_map<std::string, WindowState> m_windowStates;
    mutable std::string m_activeWindowKey;

    static constexpr const char* kServiceName = "org.linuxwallpaperengine.WaylandDetector";
    static constexpr const char* kObjectPath = "/org/linuxwallpaperengine/WaylandDetector";
    static constexpr const char* kMethodName = "OnWindowChanged";
};

} // namespace WallpaperEngine::Render::Drivers::Detectors
#endif /* ENABLE_KDE_EXPERIMENTAL_FEATURES */
#endif /* ENABLE_WAYLAND */