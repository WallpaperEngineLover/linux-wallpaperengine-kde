#pragma once

#ifdef ENABLE_WAYLAND
#ifdef ENABLE_KDE_FEATURES

#include <dbus/dbus.h>
#include <glm/vec2.hpp>
#include <memory>
#include <optional>
#include <string>

#include "WallpaperEngine/Desktop/KWinScript.h"

namespace WallpaperEngine::Input::Drivers {

// background layer-shell surfaces get no pointer motion and Wayland has no cursor query, so a KWin script forwards
// workspace.cursorPosChanged over D-Bus; check isInitialized()
class KDECursorInput {
public:
    KDECursorInput ();
    ~KDECursorInput ();

    KDECursorInput (const KDECursorInput&) = delete;
    KDECursorInput& operator= (const KDECursorInput&) = delete;

    std::optional<glm::dvec2> position ();

    [[nodiscard]] bool isInitialized () const;

private:
    bool initializeDBus ();
    void stopDBus ();

    bool loadKWinScript ();
    void unloadKWinScript ();

    static DBusHandlerResult handleMessage (DBusConnection* connection, DBusMessage* message, void* userData);
    DBusHandlerResult handleMessage (DBusMessage* message);
    bool handleMethodCall (DBusMessage* message);

    DBusConnection* m_connection = nullptr;
    std::optional<glm::dvec2> m_pos;

    std::unique_ptr<WallpaperEngine::Desktop::KWinScript> m_script;

    static constexpr const char* kServiceName = "org.linuxwallpaperengine.CursorInput";
    static constexpr const char* kObjectPath = "/org/linuxwallpaperengine/CursorInput";
    static constexpr const char* kMethodName = "OnCursorMoved";
};

}

#endif
#endif
