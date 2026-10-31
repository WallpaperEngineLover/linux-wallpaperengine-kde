#include "SessionBus.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <system_error>
#include <unistd.h>

namespace {
constexpr int CALL_TIMEOUT_MS = 2000;
/** per-frame cap, a read pulls ~2KB at most */
constexpr int MAX_READS_PER_DRAIN = 512;
}

bool WallpaperEngine::Desktop::desktopListHas (std::string_view name) {
    const char* desktops = std::getenv ("XDG_CURRENT_DESKTOP");

    if (desktops == nullptr) {
	return false;
    }

    std::string_view rest (desktops);

    while (!rest.empty ()) {
	const auto colon = rest.find (':');

	if (rest.substr (0, colon) == name) {
	    return true;
	}

	if (colon == std::string_view::npos) {
	    break;
	}

	rest.remove_prefix (colon + 1);
    }

    return false;
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

DBusMessage* WallpaperEngine::Desktop::getProperty (
    DBusConnection* connection, const char* service, const char* path, const char* interface, const char* property,
    bool autoStart
) {
    DBusMessage* call = dbus_message_new_method_call (service, path, "org.freedesktop.DBus.Properties", "Get");
    dbus_message_set_auto_start (call, autoStart ? TRUE : FALSE);
    dbus_message_append_args (call, DBUS_TYPE_STRING, &interface, DBUS_TYPE_STRING, &property, DBUS_TYPE_INVALID);

    std::string error;
    return callBlocking (connection, call, error);
}

bool WallpaperEngine::Desktop::propertyValue (DBusMessage* reply, DBusMessageIter& value) {
    DBusMessageIter iter;

    if (!dbus_message_iter_init (reply, &iter) || dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_VARIANT) {
	return false;
    }

    dbus_message_iter_recurse (&iter, &value);
    return true;
}

DBusConnection* WallpaperEngine::Desktop::privateBus (DBusBusType type) {
    DBusError error;
    dbus_error_init (&error);

    DBusConnection* bus = dbus_bus_get_private (type, &error);

    if (dbus_error_is_set (&error)) {
	dbus_error_free (&error);
	return nullptr;
    }

    dbus_connection_set_exit_on_disconnect (bus, FALSE);
    return bus;
}

void WallpaperEngine::Desktop::closeBus (DBusConnection* bus) {
    if (bus != nullptr) {
	dbus_connection_close (bus);
	dbus_connection_unref (bus);
    }
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

bool WallpaperEngine::Desktop::connectToService (
    DBusConnection*& connection, const char* busName, std::string& owner, std::string& reason,
    const std::string& notRunning
) {
    DBusError error;
    dbus_error_init (&error);

    // private, so we never eat messages meant for the shared connection
    connection = dbus_bus_get_private (DBUS_BUS_SESSION, &error);

    if (dbus_error_is_set (&error)) {
	reason = std::string ("no session bus: ") + error.message;
	dbus_error_free (&error);
	connection = nullptr;
	return false;
    }

    dbus_connection_set_exit_on_disconnect (connection, false);

    DBusMessage* message = dbus_message_new_method_call (
	"org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner"
    );
    dbus_message_append_args (message, DBUS_TYPE_STRING, &busName, DBUS_TYPE_INVALID);

    std::string callError;
    DBusMessage* reply = callBlocking (connection, message, callError);

    if (reply == nullptr) {
	reason = notRunning + " (" + callError + ")";
	return false;
    }

    const char* name = nullptr;
    dbus_message_get_args (reply, nullptr, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
    owner = name != nullptr ? name : "";
    dbus_message_unref (reply);
    return true;
}

int WallpaperEngine::Desktop::listenOnSocket (const std::string& path, std::string& reason) {
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;

    if (path.size () >= sizeof (address.sun_path)) {
	reason = "the runtime folder's path is too long for a socket";
	return -1;
    }

    std::strncpy (address.sun_path, path.c_str (), sizeof (address.sun_path) - 1);
    unlink (path.c_str ());

    const int listener = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    const mode_t mask = umask (0077);
    const bool bound = listener >= 0 && bind (listener, reinterpret_cast<sockaddr*> (&address), sizeof (address)) == 0
	&& listen (listener, 1) == 0;
    umask (mask);

    if (!bound) {
	reason = "cannot listen on " + path + ": " + std::error_code (errno, std::generic_category ()).message ();
	if (listener >= 0) {
	    close (listener);
	}
	unlink (path.c_str ());
	return -1;
    }

    return listener;
}

int WallpaperEngine::Desktop::connectUnixSocket (const std::string& socketPath, const long timeoutMicros) {
    sockaddr_un addr {};
    if (socketPath.size () >= sizeof (addr.sun_path)) {
	return -1;
    }

    const int fd = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
	return -1;
    }

    timeval timeout {};
    timeout.tv_sec = timeoutMicros / 1000000;
    timeout.tv_usec = timeoutMicros % 1000000;
    addr.sun_family = AF_UNIX;
    std::strncpy (addr.sun_path, socketPath.c_str (), sizeof (addr.sun_path) - 1);

    if (setsockopt (fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof (timeout)) != 0
	|| setsockopt (fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof (timeout)) != 0
	|| connect (fd, reinterpret_cast<sockaddr*> (&addr), sizeof (addr)) != 0) {
	close (fd);
	return -1;
    }

    return fd;
}
