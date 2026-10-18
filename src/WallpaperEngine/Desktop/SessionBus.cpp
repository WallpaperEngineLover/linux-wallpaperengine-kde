#include "SessionBus.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sys/socket.h>
#include <unistd.h>

namespace {
constexpr int CALL_TIMEOUT_MS = 2000;
/** per-frame cap, a read pulls ~2KB at most */
constexpr int MAX_READS_PER_DRAIN = 64;
}

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

std::string WallpaperEngine::Desktop::flatpakInfo (const std::string& key) {
    std::ifstream info ("/.flatpak-info");
    std::string line;

    while (std::getline (info, line)) {
	if (line.starts_with (key + "=")) {
	    return line.substr (key.size () + 1);
	}
    }

    return {};
}

std::string WallpaperEngine::Desktop::processTag () {
    static const std::string instance = [] {
	const std::string id = flatpakInfo ("instance-id");
	return id.empty () ? id : id + "_";
    }();

    return instance + std::to_string (getpid ());
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

int WallpaperEngine::Desktop::receiveDescriptor (int connection) {
    char byte = 0;
    iovec data = { .iov_base = &byte, .iov_len = 1 };
    union {
	char buffer[CMSG_SPACE (sizeof (int))];
	cmsghdr align;
    } control {};
    msghdr message = {};
    message.msg_iov = &data;
    message.msg_iovlen = 1;
    message.msg_control = control.buffer;
    message.msg_controllen = sizeof (control.buffer);

    if (recvmsg (connection, &message, MSG_CMSG_CLOEXEC) != 1) {
	return -1;
    }

    const cmsghdr* header = CMSG_FIRSTHDR (&message);

    if (header == nullptr || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) {
	return -1;
    }

    int fd = -1;
    std::memcpy (&fd, CMSG_DATA (header), sizeof (int));
    return fd;
}
