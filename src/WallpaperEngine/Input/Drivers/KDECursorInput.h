#pragma once

#ifdef ENABLE_WAYLAND
#ifdef ENABLE_KDE_EXPERIMENTAL_FEATURES

#include <dbus/dbus.h>
#include <glm/vec2.hpp>
#include <optional>
#include <string>

namespace WallpaperEngine::Input::Drivers {

// Background layer-shell surfaces never receive real pointer motion events, and there's no
// portable Wayland protocol to just ask the compositor for the cursor position. KWin itself
// always knows it though, so this loads a small script into the running KWin instance via its
// Scripting D-Bus interface; the script forwards workspace.cursorPosChanged to a D-Bus service
// hosted here. If setup fails, isInitialized() is false and callers should fall back elsewhere.
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

    std::string m_scriptPath;
    std::string m_pluginName;
    bool m_scriptLoaded = false;

    static constexpr const char* kServiceName = "org.linuxwallpaperengine.CursorInput";
    static constexpr const char* kObjectPath = "/org/linuxwallpaperengine/CursorInput";
    static constexpr const char* kMethodName = "OnCursorMoved";
};

} // namespace WallpaperEngine::Input::Drivers

#endif /* ENABLE_KDE_EXPERIMENTAL_FEATURES */
#endif /* ENABLE_WAYLAND */
