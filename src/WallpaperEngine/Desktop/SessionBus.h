#pragma once

#include <dbus/dbus.h>
#include <string>

namespace WallpaperEngine::Desktop {
/** XDG_RUNTIME_DIR, inside a Flatpak the app's folder in it (the one the host shares); empty when unset */
std::string runtimeDirectory ();

/** A key of /.flatpak-info's [Instance] group (instance-id, app-path, ...), empty outside a Flatpak or when missing */
std::string flatpakInfo (const std::string& key);

// this process in names others see (D-Bus, KWin scripts, sockets, shm): the pid, "<instance id>_<pid>" in a Flatpak
// since pids repeat between sandboxes
std::string processTag ();

/** The descriptor sent along one byte over a unix socket (SCM_RIGHTS), -1 on failure */
int receiveDescriptor (int connection);

/** Blocking call with a 2s timeout, takes ownership of call. Null with error set on failure */
DBusMessage* callBlocking (DBusConnection* connection, DBusMessage* call, std::string& error);

/** Reads and dispatches everything queued without blocking, false once the connection is gone */
bool drainBus (DBusConnection* connection);
} // namespace WallpaperEngine::Desktop
