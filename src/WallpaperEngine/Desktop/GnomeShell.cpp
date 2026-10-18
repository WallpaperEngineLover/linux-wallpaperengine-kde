#include "GnomeShell.h"
#include "SessionBus.h"

#include "WallpaperEngine/Logging/Log.h"

#include <cstdlib>
#include <cstring>
#include <optional>
#include <poll.h>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <system_error>
#include <unistd.h>

using namespace WallpaperEngine::Desktop;

bool WallpaperEngine::Desktop::isGnomeSession () {
    const char* desktops = std::getenv ("XDG_CURRENT_DESKTOP");

    if (desktops == nullptr) {
	return false;
    }

    std::string_view rest (desktops);

    while (!rest.empty ()) {
	const auto colon = rest.find (':');

	if (rest.substr (0, colon) == "GNOME") {
	    return true;
	}

	if (colon == std::string_view::npos) {
	    break;
	}

	rest.remove_prefix (colon + 1);
    }

    return false;
}

namespace {
constexpr int HELPER_TIMEOUT_MS = 6000;

bool readVariant (DBusMessageIter* iter, int type, void* value) {
    if (dbus_message_iter_get_arg_type (iter) != DBUS_TYPE_VARIANT) {
	return false;
    }

    DBusMessageIter inner;
    dbus_message_iter_recurse (iter, &inner);

    if (dbus_message_iter_get_arg_type (&inner) != type) {
	return false;
    }

    dbus_message_iter_get_basic (&inner, value);
    return true;
}

std::optional<std::vector<GnomeShell::Pointer>> readPointer (DBusMessageIter* iter) {
    if (dbus_message_iter_get_arg_type (iter) != DBUS_TYPE_ARRAY) {
	return std::nullopt;
    }

    std::vector<GnomeShell::Pointer> result;
    DBusMessageIter array;
    dbus_message_iter_recurse (iter, &array);

    for (; dbus_message_iter_get_arg_type (&array) == DBUS_TYPE_STRUCT; dbus_message_iter_next (&array)) {
	DBusMessageIter fields;
	const char* output = nullptr;
	double x = 0.0;
	double y = 0.0;

	dbus_message_iter_recurse (&array, &fields);
	if (dbus_message_iter_get_arg_type (&fields) != DBUS_TYPE_STRING) {
	    return std::nullopt;
	}
	dbus_message_iter_get_basic (&fields, &output);
	dbus_message_iter_next (&fields);
	if (dbus_message_iter_get_arg_type (&fields) != DBUS_TYPE_DOUBLE) {
	    return std::nullopt;
	}
	dbus_message_iter_get_basic (&fields, &x);
	dbus_message_iter_next (&fields);
	if (dbus_message_iter_get_arg_type (&fields) != DBUS_TYPE_DOUBLE) {
	    return std::nullopt;
	}
	dbus_message_iter_get_basic (&fields, &y);

	result.push_back ({ output, { x, y } });
    }

    return result;
}

std::optional<std::vector<GnomeShell::Window>> readWindows (DBusMessageIter* iter) {
    if (dbus_message_iter_get_arg_type (iter) != DBUS_TYPE_ARRAY) {
	return std::nullopt;
    }

    std::vector<GnomeShell::Window> result;
    DBusMessageIter array;
    dbus_message_iter_recurse (iter, &array);

    for (; dbus_message_iter_get_arg_type (&array) == DBUS_TYPE_STRUCT; dbus_message_iter_next (&array)) {
	DBusMessageIter fields;
	const char* appId = nullptr;
	const char* output = nullptr;
	dbus_uint32_t flags = 0;

	dbus_message_iter_recurse (&array, &fields);
	if (dbus_message_iter_get_arg_type (&fields) != DBUS_TYPE_STRING) {
	    return std::nullopt;
	}
	dbus_message_iter_get_basic (&fields, &appId);
	dbus_message_iter_next (&fields);
	if (dbus_message_iter_get_arg_type (&fields) != DBUS_TYPE_STRING) {
	    return std::nullopt;
	}
	dbus_message_iter_get_basic (&fields, &output);
	dbus_message_iter_next (&fields);
	if (dbus_message_iter_get_arg_type (&fields) != DBUS_TYPE_UINT32) {
	    return std::nullopt;
	}
	dbus_message_iter_get_basic (&fields, &flags);

	result.push_back ({ appId, output, flags });
    }

    return result;
}
} // namespace

GnomeShell::GnomeShell () {
    if (!this->connect ()) {
	return;
    }

    this->m_available = this->queryState ();
}

int GnomeShell::openWaylandSocket () {
    const std::string directory = runtimeDirectory ();

    // the extension only spawns a helper for a socket in the runtime folder
    if (directory.empty ()) {
	this->m_reason = "XDG_RUNTIME_DIR is not set";
	return -1;
    }

    const std::string path = directory + "/lwe-gnome-" + processTag () + ".sock";
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;

    if (path.size () >= sizeof (address.sun_path)) {
	this->m_reason = "the runtime folder's path is too long for a socket";
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
	this->m_reason
	    = "cannot listen on " + path + ": " + std::error_code (errno, std::generic_category ()).message ();
	if (listener >= 0) {
	    close (listener);
	}
	return -1;
    }

    DBusMessage* request = dbus_message_new_method_call (BUS_NAME, OBJECT_PATH, INTERFACE, "SpawnHelper");
    const char* pathArg = path.c_str ();
    dbus_message_append_args (request, DBUS_TYPE_STRING, &pathArg, DBUS_TYPE_INVALID);

    std::string error;
    DBusMessage* reply = callBlocking (this->m_connection, request, error);
    int fd = -1;

    if (reply == nullptr) {
	this->m_reason = "the GNOME Shell extension could not start the helper: " + error;
    } else {
	dbus_message_unref (reply);
	pollfd waiting = { .fd = listener, .events = POLLIN, .revents = 0 };

	if (poll (&waiting, 1, HELPER_TIMEOUT_MS) == 1) {
	    this->m_helperConnection = accept4 (listener, nullptr, nullptr, SOCK_CLOEXEC);
	}

	fd = this->m_helperConnection >= 0 ? receiveDescriptor (this->m_helperConnection) : -1;

	if (fd < 0) {
	    this->m_reason = "the helper never handed over the Wayland connection";
	}
    }

    close (listener);
    unlink (path.c_str ());
    return fd;
}

GnomeShell::~GnomeShell () {
    if (this->m_helperConnection >= 0) {
	close (this->m_helperConnection);
    }

    if (this->m_connection != nullptr) {
	// libdbus aborts on removing a filter that was never added
	if (this->m_filterAdded) {
	    dbus_connection_remove_filter (this->m_connection, &GnomeShell::filter, this);
	}

	dbus_connection_close (this->m_connection);
	dbus_connection_unref (this->m_connection);
    }
}

bool GnomeShell::connect () {
    DBusError error;
    dbus_error_init (&error);

    // private, so we never eat messages meant for the shared connection
    this->m_connection = dbus_bus_get_private (DBUS_BUS_SESSION, &error);

    if (dbus_error_is_set (&error)) {
	this->m_reason = std::string ("no session bus: ") + error.message;
	dbus_error_free (&error);
	this->m_connection = nullptr;
	return false;
    }

    dbus_connection_set_exit_on_disconnect (this->m_connection, false);

    DBusMessage* message = dbus_message_new_method_call (
	"org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner"
    );
    const char* name = BUS_NAME;
    dbus_message_append_args (message, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);

    std::string callError;
    DBusMessage* reply = callBlocking (this->m_connection, message, callError);

    if (reply == nullptr) {
	this->m_reason = "the linux-wallpaperengine GNOME Shell extension is not running (" + callError + ")";
	return false;
    }

    const char* owner = nullptr;
    dbus_message_get_args (reply, nullptr, DBUS_TYPE_STRING, &owner, DBUS_TYPE_INVALID);
    this->m_owner = owner != nullptr ? owner : "";
    dbus_message_unref (reply);

    // subscribe before querying so nothing is lost
    const std::string signals = std::string ("type='signal',sender='") + BUS_NAME + "',interface='" + INTERFACE
	+ "',path='" + OBJECT_PATH + "'";
    const std::string ownerChanges
	= std::string ("type='signal',sender='org.freedesktop.DBus',member='NameOwnerChanged',arg0='") + BUS_NAME + "'";

    dbus_bus_add_match (this->m_connection, signals.c_str (), nullptr);
    dbus_bus_add_match (this->m_connection, ownerChanges.c_str (), nullptr);
    this->m_filterAdded = dbus_connection_add_filter (this->m_connection, &GnomeShell::filter, this, nullptr);

    if (!this->m_filterAdded) {
	this->m_reason = "out of memory adding a D-Bus filter";
	return false;
    }

    return true;
}

bool GnomeShell::queryState () {
    const char* interface = INTERFACE;
    std::string error;

    for (const char* property : { "Version", "Supported", "NeedsHelper" }) {
	DBusMessage* message
	    = dbus_message_new_method_call (BUS_NAME, OBJECT_PATH, "org.freedesktop.DBus.Properties", "Get");
	dbus_message_append_args (
	    message, DBUS_TYPE_STRING, &interface, DBUS_TYPE_STRING, &property, DBUS_TYPE_INVALID
	);

	DBusMessage* reply = callBlocking (this->m_connection, message, error);

	if (reply == nullptr) {
	    this->m_reason = "the GNOME Shell extension did not answer (" + error + ")";
	    return false;
	}

	DBusMessageIter iter;
	dbus_message_iter_init (reply, &iter);

	if (std::strcmp (property, "Version") == 0) {
	    dbus_uint32_t version = 0;

	    if (!readVariant (&iter, DBUS_TYPE_UINT32, &version) || version != API_VERSION) {
		this->m_reason = "the GNOME Shell extension speaks version " + std::to_string (version)
		    + " of its interface, this engine needs " + std::to_string (API_VERSION)
		    + " (update the extension, then log out and back in)";
		dbus_message_unref (reply);
		return false;
	    }
	} else if (std::strcmp (property, "Supported") == 0) {
	    dbus_bool_t supported = false;

	    if (!readVariant (&iter, DBUS_TYPE_BOOLEAN, &supported) || !supported) {
		this->m_reason
		    = "this GNOME Shell can't make desktop windows for the extension (it needs "
		      "Meta.Window.set_type, GNOME 49 and later, or Meta.WaylandClient.make_desktop, 45 to 48)";
		dbus_message_unref (reply);
		return false;
	    }
	} else {
	    dbus_bool_t needsHelper = false;

	    readVariant (&iter, DBUS_TYPE_BOOLEAN, &needsHelper);
	    this->m_needsHelper = needsHelper;
	}

	dbus_message_unref (reply);
    }

    DBusMessage* reply = callBlocking (
	this->m_connection, dbus_message_new_method_call (BUS_NAME, OBJECT_PATH, INTERFACE, "GetState"), error
    );

    if (reply == nullptr) {
	this->m_reason = "the GNOME Shell extension did not answer GetState (" + error + ")";
	return false;
    }

    DBusMessageIter iter;
    dbus_message_iter_init (reply, &iter);
    auto pointer = readPointer (&iter);
    dbus_message_iter_next (&iter);
    auto windows = readWindows (&iter);
    dbus_message_unref (reply);

    if (!pointer.has_value () || !windows.has_value ()) {
	this->m_reason = "the GNOME Shell extension sent a state this engine can't read";
	return false;
    }

    this->m_pointer = std::move (*pointer);
    this->m_windows = std::move (*windows);
    return true;
}

void GnomeShell::dispatch () {
    if (this->m_connection == nullptr) {
	return;
    }

    if (!this->m_vanished && !drainBus (this->m_connection)) {
	sLog.error ("The session bus connection to the GNOME Shell extension dropped");
	this->m_vanished = true;
    }
}

DBusHandlerResult GnomeShell::filter (DBusConnection*, DBusMessage* message, void* data) {
    if (dbus_message_get_type (message) == DBUS_MESSAGE_TYPE_SIGNAL) {
	static_cast<GnomeShell*> (data)->handleSignal (message);
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

void GnomeShell::handleSignal (DBusMessage* message) {
    if (dbus_message_is_signal (message, "org.freedesktop.DBus", "NameOwnerChanged")) {
	const char* name = nullptr;
	const char* oldOwner = nullptr;
	const char* newOwner = nullptr;

	if (dbus_message_get_args (
		message, nullptr, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &oldOwner, DBUS_TYPE_STRING, &newOwner,
		DBUS_TYPE_INVALID
	    )
	    && std::strcmp (name, BUS_NAME) == 0 && newOwner[0] == '\0' && this->m_available) {
	    sLog.error ("The linux-wallpaperengine GNOME Shell extension went away");
	    this->m_vanished = true;
	    this->m_pointer.clear ();
	    this->m_windows.clear ();
	    this->m_desktopLeftDown = false;
	}

	return;
    }

    // only the owner we checked counts
    if (const char* sender = dbus_message_get_sender (message); sender == nullptr || this->m_owner != sender) {
	return;
    }

    DBusMessageIter iter;

    if (!dbus_message_iter_init (message, &iter)) {
	return;
    }

    if (dbus_message_is_signal (message, INTERFACE, "PointerMoved")) {
	if (auto pointer = readPointer (&iter); pointer.has_value ()) {
	    this->m_pointer = std::move (*pointer);
	}
    } else if (dbus_message_is_signal (message, INTERFACE, "DesktopButton")) {
	if (dbus_message_iter_get_arg_type (&iter) == DBUS_TYPE_BOOLEAN) {
	    dbus_bool_t pressed = false;
	    dbus_message_iter_get_basic (&iter, &pressed);
	    this->m_desktopLeftDown = pressed;
	}
    } else if (dbus_message_is_signal (message, INTERFACE, "WindowsChanged")) {
	if (auto windows = readWindows (&iter); windows.has_value ()) {
	    this->m_windows = std::move (*windows);
	}
    }
}
