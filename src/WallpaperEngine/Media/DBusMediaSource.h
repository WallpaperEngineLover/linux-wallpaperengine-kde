#pragma once

#include "MediaSource.h"
#include <atomic>
#include <dbus/dbus.h>
#include <memory>

namespace WallpaperEngine::Media {
class DBusMediaSource : public MediaSource {
public:
    explicit DBusMediaSource (std::chrono::milliseconds updateInterval);
    ~DBusMediaSource () override;

    void parseMetadata (DBusMessageIter& variant);
    void parsePlaybackStatus (DBusMessageIter& variant, const char* sender);
    void parsePosition (DBusMessageIter& variant);

    void update () override;
    void drain () override;
    void discard () override;

    [[nodiscard]] bool discarding () const { return this->m_discarding; }
    void noteMissed () { this->m_missed = true; }

    [[nodiscard]] bool isCurrentPlayer (const char* sender) const;
    void switchPlayer (const std::string& player);

protected:
    void performUpdate () override;
    bool connect ();
    void attach (DBusConnection* connection);
    void disconnect ();
    void resync ();
    void handleLoss ();
    void tryReconnect ();
    [[nodiscard]] bool isConnected () const;
    void initialStatusFetch ();
    void detectPlayer ();
    std::optional<std::string> uniqueName (const std::string& name);

    DBusMessage* dbusMessage (
	const char* bus_name, const char* path, const char* interface, const char* method, const char* iface = nullptr,
	const char* prop = nullptr
    );

    std::optional<std::string> m_currentPlayer = std::nullopt;

    // the session bus can take long to answer a new connection, that is waited for off the render thread
    struct PendingConnect {
	std::atomic<bool> done = false;
	std::atomic<bool> abandoned = false;
	std::atomic<DBusConnection*> bus = nullptr;
    };

    DBusConnection* m_connection = nullptr;
    std::chrono::steady_clock::time_point m_nextReconnect {};
    std::shared_ptr<PendingConnect> m_pending;
    bool m_discarding = false;
    bool m_missed = false;
};
}