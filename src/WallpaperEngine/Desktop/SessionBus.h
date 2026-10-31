#pragma once

#include <dbus/dbus.h>
#include <string>
#include <string_view>

namespace WallpaperEngine::Desktop {
std::string runtimeDirectory ();

bool desktopListHas (std::string_view name);

std::string flatpakInfo (const std::string& key);

// this process in names others see (D-Bus, KWin scripts, sockets, shm): the pid, "<instance id>_<pid>" in a Flatpak
// since pids repeat between sandboxes
std::string processTag ();

int receiveDescriptor (int connection);

/** Blocking call with a 2s timeout, takes ownership of call. Null with error set on failure */
DBusMessage* callBlocking (DBusConnection* connection, DBusMessage* call, std::string& error);

bool drainBus (DBusConnection* connection);

/** Properties.Get of one property, the reply or null when the call fails. Without autoStart the call never starts the
 * service that owns it */
DBusMessage* getProperty (
    DBusConnection* connection, const char* service, const char* path, const char* interface, const char* property,
    bool autoStart = true
);

bool propertyValue (DBusMessage* reply, DBusMessageIter& value);

/** A connection of its own that survives the bus going away without ending the process, null when there is no bus */
DBusConnection* privateBus (DBusBusType type);

void closeBus (DBusConnection* bus);

bool connectToService (
    DBusConnection*& connection, const char* busName, std::string& owner, std::string& reason,
    const std::string& notRunning
);

int listenOnSocket (const std::string& path, std::string& reason);

int connectUnixSocket (const std::string& socketPath, long timeoutMicros);
}
