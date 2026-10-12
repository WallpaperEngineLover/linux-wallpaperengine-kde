#pragma once

#include <dbus/dbus.h>
#include <string>

namespace WallpaperEngine::Desktop {
/** A KWin script loaded over D-Bus for the lifetime of this object */
class KWinScript {
public:
    KWinScript (DBusConnection* connection, std::string pluginName, const std::string& source);
    ~KWinScript ();

    KWinScript (const KWinScript&) = delete;
    KWinScript& operator= (const KWinScript&) = delete;

    [[nodiscard]] bool isLoaded () const { return this->m_loaded; }

private:
    DBusConnection* m_connection;
    std::string m_pluginName;
    std::string m_path;
    bool m_loaded = false;
};
} // namespace WallpaperEngine::Desktop
