// linux-wallpaperengine-kwin-janitor: the engine starts one of these next to itself and tells it, over stdin, which
// scripts it loads into KWin ("+<plugin name>\t<file>\n", "-<plugin name>\n" when it unloads one itself). When the pipe
// closes the engine is gone, however it went (a SIGKILL runs no destructor), and the scripts it left in KWin are
// unloaded: they would go on calling the dead engine's D-Bus service at every cursor move. Only libdbus on purpose, the
// engine itself is over 100 MB resident.
#include <dbus/dbus.h>

#include <cerrno>
#include <cstdio>
#include <map>
#include <string>
#include <unistd.h>

namespace {
void unloadScript (DBusConnection* connection, const std::string& pluginName) {
    DBusMessage* call
	= dbus_message_new_method_call ("org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "unloadScript");

    if (call == nullptr) {
	return;
    }

    const char* nameArg = pluginName.c_str ();
    dbus_message_append_args (call, DBUS_TYPE_STRING, &nameArg, DBUS_TYPE_INVALID);

    DBusError error;
    dbus_error_init (&error);

    if (DBusMessage* reply = dbus_connection_send_with_reply_and_block (connection, call, 2000, &error);
	reply != nullptr) {
	dbus_message_unref (reply);
    }

    dbus_error_free (&error);
    dbus_message_unref (call);
}
}

int main () {
    std::map<std::string, std::string> scripts;
    std::string pending;
    char buffer[512];

    for (;;) {
	const ssize_t count = read (STDIN_FILENO, buffer, sizeof (buffer));

	if (count < 0 && errno == EINTR) {
	    continue;
	}

	if (count <= 0) {
	    break;
	}

	pending.append (buffer, static_cast<size_t> (count));

	for (size_t end; (end = pending.find ('\n')) != std::string::npos; pending.erase (0, end + 1)) {
	    const std::string line = pending.substr (0, end);

	    if (line.starts_with ("-")) {
		scripts.erase (line.substr (1));
	    } else if (line.starts_with ("+")) {
		const auto tab = line.find ('\t');

		if (tab != std::string::npos) {
		    scripts[line.substr (1, tab - 1)] = line.substr (tab + 1);
		}
	    }
	}
    }

    if (scripts.empty ()) {
	return 0;
    }

    DBusError error;
    dbus_error_init (&error);
    DBusConnection* connection = dbus_bus_get_private (DBUS_BUS_SESSION, &error);

    if (connection == nullptr) {
	dbus_error_free (&error);
	return 1;
    }

    dbus_connection_set_exit_on_disconnect (connection, false);

    for (const auto& [name, path] : scripts) {
	unloadScript (connection, name);
	std::remove (path.c_str ());
    }

    dbus_connection_close (connection);
    dbus_connection_unref (connection);

    return 0;
}
