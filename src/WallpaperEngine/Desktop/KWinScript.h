#pragma once

#include <chrono>
#include <dbus/dbus.h>
#include <filesystem>
#include <string>
#include <vector>

namespace WallpaperEngine::Desktop {
/** A KWin script loaded over D-Bus for the lifetime of this object. Its source file stays locked while this process
 * lives, so whatever the process leaves behind when it is killed can be told from the scripts of live engines */
class KWinScript {
public:
    KWinScript (DBusConnection* connection, std::string pluginName, const std::string& source);
    ~KWinScript ();

    KWinScript (const KWinScript&) = delete;
    KWinScript& operator= (const KWinScript&) = delete;

    /** loaded and started */
    [[nodiscard]] bool isLoaded () const { return this->m_running; }

    /** Plugin names of the scripts in directory whose engine is gone: the file is not locked by anyone, not new (a
     * starting engine creates and locks it a moment later) and its pid, where the name has one, is not running */
    [[nodiscard]] static std::vector<std::string>
    findStale (const std::filesystem::path& directory, std::chrono::seconds minimumAge = std::chrono::seconds (2));
    /** Unloads those from KWin and removes their files. A killed engine's script goes on calling its dead D-Bus
     * service at every cursor move otherwise */
    static void unloadStale (DBusConnection* connection);

private:
    DBusConnection* m_connection;
    std::string m_pluginName;
    std::string m_path;
    int m_lock = -1;
    bool m_registered = false;
    bool m_running = false;
};
} // namespace WallpaperEngine::Desktop
