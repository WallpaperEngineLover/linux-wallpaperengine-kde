#pragma once

#ifdef ENABLE_WAYLAND
#ifdef ENABLE_KDE_FEATURES

#include <chrono>
#include <dbus/dbus.h>
#include <future>
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

    void setSuspended (bool suspended);

private:
    struct ScriptHandle;

    bool initializeDBus ();
    void stopDBus ();

    bool loadKWinScript ();
    void unloadKWinScript ();
    void startReload ();
    void pollReload ();

    static DBusHandlerResult handleMessage (DBusConnection* connection, DBusMessage* message, void* userData);
    DBusHandlerResult handleMessage (DBusMessage* message);
    bool handleMethodCall (DBusMessage* message);

    DBusConnection* m_connection = nullptr;
    std::optional<glm::dvec2> m_pos;
    bool m_suspended = false;
    unsigned m_reloadCount = 0;

    std::unique_ptr<ScriptHandle> m_script;
    std::future<std::unique_ptr<ScriptHandle>> m_reload;
    std::chrono::steady_clock::time_point m_nextReload {};

    static constexpr const char* kServiceName = "org.linuxwallpaperengine.CursorInput";
    static constexpr const char* kObjectPath = "/org/linuxwallpaperengine/CursorInput";
    static constexpr const char* kMethodName = "OnCursorMoved";
};

}

#endif
#endif
