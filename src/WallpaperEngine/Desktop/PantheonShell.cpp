#include "PantheonShell.h"
#include "SessionBus.h"

#include "WallpaperEngine/Logging/Log.h"

#include <cstdlib>
#include <cstring>
#include <string_view>

using namespace WallpaperEngine::Desktop;

bool WallpaperEngine::Desktop::isPantheonSession () { return desktopListHas ("Pantheon"); }

namespace {
bool readBasic (DBusMessageIter* variant, void* value, int type) {
    DBusMessageIter inner;
    dbus_message_iter_recurse (variant, &inner);

    if (dbus_message_iter_get_arg_type (&inner) != type) {
	return false;
    }

    dbus_message_iter_get_basic (&inner, value);
    return true;
}

void readProperty (PantheonShell::Window& window, const char* key, DBusMessageIter* variant) {
    if (std::strcmp (key, "has-focus") == 0) {
	dbus_bool_t value = false;

	if (readBasic (variant, &value, DBUS_TYPE_BOOLEAN)) {
	    window.focused = value;
	}
    } else if (std::strcmp (key, "is-hidden") == 0) {
	dbus_bool_t value = false;

	if (readBasic (variant, &value, DBUS_TYPE_BOOLEAN)) {
	    window.hidden = value;
	}
    } else if (std::strcmp (key, "width") == 0) {
	dbus_uint32_t value = 0;

	if (readBasic (variant, &value, DBUS_TYPE_UINT32)) {
	    window.width = value;
	}
    } else if (std::strcmp (key, "height") == 0) {
	dbus_uint32_t value = 0;

	if (readBasic (variant, &value, DBUS_TYPE_UINT32)) {
	    window.height = value;
	}
    } else if (std::strcmp (key, "workspace-index") == 0) {
	dbus_int32_t value = 0;

	if (readBasic (variant, &value, DBUS_TYPE_INT32)) {
	    window.workspace = value;
	}
    } else if (std::strcmp (key, "wm-class") == 0) {
	const char* value = nullptr;

	if (readBasic (variant, &value, DBUS_TYPE_STRING) && value != nullptr) {
	    window.wmClass = value;
	}
    }
}

bool readWindows (DBusMessageIter* iter, std::vector<PantheonShell::Window>& result) {
    if (dbus_message_iter_get_arg_type (iter) != DBUS_TYPE_ARRAY) {
	return false;
    }

    DBusMessageIter array;
    dbus_message_iter_recurse (iter, &array);

    for (; dbus_message_iter_get_arg_type (&array) == DBUS_TYPE_STRUCT; dbus_message_iter_next (&array)) {
	DBusMessageIter fields;
	dbus_message_iter_recurse (&array, &fields);

	if (!dbus_message_iter_next (&fields) || dbus_message_iter_get_arg_type (&fields) != DBUS_TYPE_ARRAY) {
	    return false;
	}

	PantheonShell::Window window;
	DBusMessageIter entries;
	dbus_message_iter_recurse (&fields, &entries);

	for (; dbus_message_iter_get_arg_type (&entries) == DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next (&entries)) {
	    DBusMessageIter entry;
	    const char* key = nullptr;

	    dbus_message_iter_recurse (&entries, &entry);
	    dbus_message_iter_get_basic (&entry, &key);
	    dbus_message_iter_next (&entry);

	    if (key != nullptr && dbus_message_iter_get_arg_type (&entry) == DBUS_TYPE_VARIANT) {
		readProperty (window, key, &entry);
	    }
	}

	result.push_back (std::move (window));
    }

    return true;
}
}

PantheonShell::PantheonShell () {
    if (!this->connect ()) {
	return;
    }

    this->m_available = true;
    this->refresh ();
}

PantheonShell::~PantheonShell () {
    if (this->m_connection == nullptr) {
	return;
    }

    // libdbus aborts on removing a filter that was never added
    if (this->m_filterAdded) {
	dbus_connection_remove_filter (this->m_connection, &PantheonShell::filter, this);
    }

    dbus_connection_close (this->m_connection);
    dbus_connection_unref (this->m_connection);
}

bool PantheonShell::connect () {
    DBusError error;
    dbus_error_init (&error);

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
	this->m_reason = "Gala is not on the session bus (" + callError + ")";
	return false;
    }

    const char* owner = nullptr;
    dbus_message_get_args (reply, nullptr, DBUS_TYPE_STRING, &owner, DBUS_TYPE_INVALID);
    this->m_owner = owner != nullptr ? owner : "";
    dbus_message_unref (reply);

    const std::string signals = std::string ("type='signal',sender='") + BUS_NAME + "',interface='" + INTERFACE
	+ "',path='" + OBJECT_PATH + "'";

    dbus_bus_add_match (this->m_connection, signals.c_str (), nullptr);
    this->m_filterAdded = dbus_connection_add_filter (this->m_connection, &PantheonShell::filter, this, nullptr);

    if (!this->m_filterAdded) {
	this->m_reason = "out of memory adding a D-Bus filter";
	return false;
    }

    return true;
}

void PantheonShell::refresh () {
    std::string error;
    DBusMessage* reply = callBlocking (
	this->m_connection, dbus_message_new_method_call (BUS_NAME, OBJECT_PATH, INTERFACE, "GetWindows"), error
    );

    if (reply == nullptr) {
	sLog.debugerror ("Gala did not answer GetWindows: ", error);
	return;
    }

    std::vector<Window> windows;
    DBusMessageIter iter;

    if (dbus_message_iter_init (reply, &iter) && readWindows (&iter, windows)) {
	this->m_windows = std::move (windows);
    }

    dbus_message_unref (reply);

    reply = callBlocking (
	this->m_connection, dbus_message_new_method_call (BUS_NAME, OBJECT_PATH, INTERFACE, "GetActiveWorkspace"), error
    );

    if (reply != nullptr) {
	dbus_int32_t index = 0;

	if (dbus_message_get_args (reply, nullptr, DBUS_TYPE_INT32, &index, DBUS_TYPE_INVALID)) {
	    this->m_activeWorkspace = index;
	}

	dbus_message_unref (reply);
    }
}

void PantheonShell::dispatch () {
    if (this->m_connection == nullptr || !this->m_available) {
	return;
    }

    if (!drainBus (this->m_connection)) {
	sLog.error ("The session bus connection to Gala dropped, window states are not followed any more");
	this->m_available = false;
	this->m_windows.clear ();
	return;
    }

    if (this->m_dirty) {
	this->m_dirty = false;
	this->refresh ();
    }
}

DBusHandlerResult PantheonShell::filter (DBusConnection*, DBusMessage* message, void* data) {
    if (dbus_message_get_type (message) != DBUS_MESSAGE_TYPE_SIGNAL) {
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    auto* self = static_cast<PantheonShell*> (data);
    const char* sender = dbus_message_get_sender (message);

    if (sender != nullptr && self->m_owner == sender
	&& (dbus_message_is_signal (message, INTERFACE, "WindowsChanged")
	    || dbus_message_is_signal (message, INTERFACE, "ActiveWorkspaceChanged"))) {
	self->m_dirty = true;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}
