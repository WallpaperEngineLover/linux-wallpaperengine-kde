#pragma once

#ifdef ENABLE_WAYLAND
#ifdef ENABLE_KDE_EXPERIMENTAL_FEATURES

#include <dbus/dbus.h>
#include <glm/vec2.hpp>
#include <optional>
#include <string>

namespace WallpaperEngine::Input::Drivers {

/**
 * @brief Live global cursor position on KDE Plasma Wayland, fed by a small KWin script.
 *
 * Background layer-shell surfaces never receive real pointer motion events, and there is
 * no portable Wayland protocol to ask the compositor "where is the cursor right now". KWin
 * itself always knows (workspace.cursorPos in its scripting API), so this class loads a
 * short script into the running KWin instance via its Scripting D-Bus interface. The script
 * connects to workspace.cursorPosChanged and forwards every update to a small D-Bus service
 * hosted by this class.
 *
 * If D-Bus initialization or script loading fails, @c isInitialized() returns @c false and
 * the caller is expected to fall back to another cursor source (Hyprland IPC, XWayland, ...).
 */
class KDECursorInput {
public:
    KDECursorInput ();
    ~KDECursorInput ();

    KDECursorInput (const KDECursorInput&) = delete;
    KDECursorInput& operator= (const KDECursorInput&) = delete;

    /**
     * Pumps the D-Bus connection and returns the last known global cursor position, if any
     * has been reported yet.
     */
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
