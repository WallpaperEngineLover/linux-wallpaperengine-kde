#include "SessionBus.h"

#include <cstdlib>
#include <filesystem>
#include <unistd.h>

namespace {
constexpr int CALL_TIMEOUT_MS = 2000;
/** per-frame cap, a read pulls ~2KB at most */
constexpr int MAX_READS_PER_DRAIN = 64;
} // namespace

std::string WallpaperEngine::Desktop::runtimeDirectory () {
    const char* runtimeDir = std::getenv ("XDG_RUNTIME_DIR");

    if (runtimeDir == nullptr) {
	return {};
    }

    std::string dir = runtimeDir;

    // Flatpak shares only the app's runtime folder; FLATPAK_ID alone can be inherited
    if (const char* flatpakId = std::getenv ("FLATPAK_ID");
	flatpakId != nullptr && access ("/.flatpak-info", F_OK) == 0) {
	const std::string appDir = dir + "/app/" + flatpakId;
	std::error_code error;

	if (std::filesystem::is_directory (appDir, error)) {
	    dir = appDir;
	}
    }

    return dir;
}

DBusMessage*
WallpaperEngine::Desktop::callBlocking (DBusConnection* connection, DBusMessage* call, std::string& error) {
    DBusError dbusError;
    dbus_error_init (&dbusError);

    DBusMessage* reply = dbus_connection_send_with_reply_and_block (connection, call, CALL_TIMEOUT_MS, &dbusError);
    dbus_message_unref (call);

    if (dbus_error_is_set (&dbusError)) {
	error = dbusError.message;
	dbus_error_free (&dbusError);
	return nullptr;
    }

    return reply;
}

bool WallpaperEngine::Desktop::drainBus (DBusConnection* connection) {
    for (int reads = 0; reads < MAX_READS_PER_DRAIN; reads++) {
	if (!dbus_connection_read_write (connection, 0)) {
	    return false;
	}

	bool dispatched = false;

	while (dbus_connection_get_dispatch_status (connection) == DBUS_DISPATCH_DATA_REMAINS) {
	    dbus_connection_dispatch (connection);
	    dispatched = true;
	}

	if (!dispatched) {
	    break;
	}
    }

    return true;
}
