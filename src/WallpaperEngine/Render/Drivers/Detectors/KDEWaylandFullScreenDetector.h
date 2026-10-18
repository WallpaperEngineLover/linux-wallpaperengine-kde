#pragma once

#ifdef ENABLE_WAYLAND
#ifdef ENABLE_KDE_FEATURES

#include <dbus/dbus.h>
#include <memory>
#include <string>
#include <vector>

#include "FullScreenDetector.h"
#include "WallpaperEngine/Desktop/KWinScript.h"
#include "WallpaperEngine/Desktop/WindowState.h"

namespace WallpaperEngine::Render::Drivers {
class WaylandOpenGLDriver;
}

namespace WallpaperEngine::Render::Drivers::Detectors {

/**
 * KWin has no wlr foreign toplevel, so a KWin script reports focused/maximized/fullscreen windows per output to
 * this process over D-Bus. Falls back to WaylandFullScreenDetector when not isInitialized ().
 */
class KDEWaylandFullScreenDetector final : public FullScreenDetector {
public:
    KDEWaylandFullScreenDetector (Application::ApplicationContext& appContext, WaylandOpenGLDriver& driver);
    ~KDEWaylandFullScreenDetector () override;

    [[nodiscard]] bool anythingFullscreen () const override;
    [[nodiscard]] bool isInitialized () const;
    void reset () override;

    bool applyWindows (const std::string& json);

private:
    static DBusHandlerResult handleMessage (DBusConnection* connection, DBusMessage* message, void* userData);
    DBusHandlerResult handleMessage (DBusMessage* message);
    bool initializeDBus ();
    void dispatch () const;
    [[nodiscard]] std::string scriptSource () const;

    WaylandOpenGLDriver& m_driver;
    mutable DBusConnection* m_connection = nullptr;
    std::string m_serviceName;
    std::unique_ptr<Desktop::KWinScript> m_script;
    mutable std::vector<Desktop::WindowState> m_windows;
    mutable bool m_disconnected = false;

    static constexpr const char* kServiceName = "org.linuxwallpaperengine.WaylandDetector";
    static constexpr const char* kObjectPath = "/org/linuxwallpaperengine/WaylandDetector";
    static constexpr const char* kMethodName = "OnWindows";
    static constexpr const char* kUnsupported = "UNSUPPORTED";
};

}
#endif
#endif
