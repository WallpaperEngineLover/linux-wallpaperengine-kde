#pragma once

#include <dbus/dbus.h>
#include <string>

namespace WallpaperEngine::Desktop {
std::string runtimeDirectory ();

std::string flatpakInfo (const std::string& key);

// this process in names others see (D-Bus, KWin scripts, sockets, shm): the pid, "<instance id>_<pid>" in a Flatpak
// since pids repeat between sandboxes
std::string processTag ();

int receiveDescriptor (int connection);

/** Blocking call with a 2s timeout, takes ownership of call. Null with error set on failure */
DBusMessage* callBlocking (DBusConnection* connection, DBusMessage* call, std::string& error);

bool drainBus (DBusConnection* connection);
}
