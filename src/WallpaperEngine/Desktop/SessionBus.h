#pragma once

#include <dbus/dbus.h>
#include <string>

namespace WallpaperEngine::Desktop {
/** XDG_RUNTIME_DIR, inside a Flatpak the app's folder in it (the one the host shares); empty when unset */
std::string runtimeDirectory ();

/** Blocking call with a 2s timeout, takes ownership of call. Null with error set on failure */
DBusMessage* callBlocking (DBusConnection* connection, DBusMessage* call, std::string& error);

/** Reads and dispatches everything queued without blocking, false once the connection is gone */
bool drainBus (DBusConnection* connection);
} // namespace WallpaperEngine::Desktop
