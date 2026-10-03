#pragma once

#include "MediaSource.h"
#include <dbus/dbus.h>

namespace WallpaperEngine::Media {
class DBusMediaSource : public MediaSource {
public:
    explicit DBusMediaSource (std::chrono::milliseconds updateInterval);
    ~DBusMediaSource () override;

    void parseMetadata (DBusMessageIter& variant);
    void parsePlaybackStatus (DBusMessageIter& variant, const char* sender);
    void parsePosition (DBusMessageIter& variant);

    void update () override;

    /** Signals from other players than the followed one only matter when they start playing */
    [[nodiscard]] bool isCurrentPlayer (const char* sender) const;
    void switchPlayer (const std::string& player);

protected:
    void performUpdate () override;
    void initialStatusFetch ();
    void detectPlayer ();
    /** The unique bus name owning a well-known MPRIS name */
    std::optional<std::string> uniqueName (const std::string& name);

    DBusMessage* dbusMessage (
	const char* bus_name, const char* path, const char* interface, const char* method, const char* iface = nullptr,
	const char* prop = nullptr
    );

    std::optional<std::string> m_currentPlayer = std::nullopt;

    DBusConnection* m_connection;
};
}