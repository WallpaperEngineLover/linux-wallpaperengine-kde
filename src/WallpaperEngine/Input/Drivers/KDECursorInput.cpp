#include "KDECursorInput.h"

#include "WallpaperEngine/Desktop/SessionBus.h"
#include "WallpaperEngine/Logging/Log.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <unistd.h>

namespace WallpaperEngine::Input::Drivers {

KDECursorInput::KDECursorInput () {
    if (!initializeDBus ()) {
	sLog.out ("KDE cursor input could not initialize DBus, mouse-reactive effects may not work on this session");
	return;
    }

    if (!loadKWinScript ()) {
	sLog.out (
	    "KDE cursor input could not load its KWin script, mouse-reactive effects may not work on this session"
	);
	stopDBus ();
    }
}

KDECursorInput::~KDECursorInput () {
    unloadKWinScript ();
    stopDBus ();
}

bool KDECursorInput::initializeDBus () {
    DBusError error;
    dbus_error_init (&error);

    m_connection = dbus_bus_get (DBUS_BUS_SESSION, &error);
    if (dbus_error_is_set (&error)) {
	sLog.error ("Failed to connect to the session DBus: ", error.message);
	dbus_error_free (&error);
	return false;
    }

    if (m_connection == nullptr) {
	sLog.error ("Failed to connect to the session DBus: unknown error");
	return false;
    }

    dbus_connection_set_exit_on_disconnect (m_connection, false);

    dbus_error_init (&error);
    const std::string serviceName = std::string (kServiceName) + "_" + std::to_string (getpid ());
    const auto requestResult
	= dbus_bus_request_name (m_connection, serviceName.c_str (), DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);

    if (dbus_error_is_set (&error)) {
	sLog.error ("Failed to request DBus service ", serviceName, ": ", error.message);
	dbus_error_free (&error);
	dbus_connection_unref (m_connection);
	m_connection = nullptr;
	return false;
    }

    if (requestResult != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER
	&& requestResult != DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER) {
	sLog.error ("DBus service ", serviceName, " is already owned by another process");
	dbus_connection_unref (m_connection);
	m_connection = nullptr;
	return false;
    }

    static constexpr DBusObjectPathVTable objectVTable = {
	.unregister_function = nullptr,
	.message_function = &KDECursorInput::handleMessage,
    };

    if (!dbus_connection_register_object_path (m_connection, kObjectPath, &objectVTable, this)) {
	sLog.error ("Failed to register DBus object path for KDE cursor input");
	dbus_connection_unref (m_connection);
	m_connection = nullptr;
	return false;
    }

    return true;
}

void KDECursorInput::stopDBus () {
    if (m_connection != nullptr) {
	dbus_connection_unregister_object_path (m_connection, kObjectPath);
	dbus_connection_unref (m_connection);
	m_connection = nullptr;
    }
}

bool KDECursorInput::loadKWinScript () {
    const std::string serviceName = std::string (kServiceName) + "_" + std::to_string (getpid ());

    std::ostringstream script;
    script << "function lweReportCursorPos() {\n"
	   << "    var p = workspace.cursorPos;\n"
	   << "    callDBus(\"" << serviceName << "\", \"" << kObjectPath << "\", \"" << serviceName << "\", \""
	   << kMethodName << "\", p.x, p.y);\n"
	   << "}\n"
	   << "workspace.cursorPosChanged.connect(lweReportCursorPos);\n"
	   << "lweReportCursorPos();\n";

    this->m_script = std::make_unique<WallpaperEngine::Desktop::KWinScript> (
	m_connection, "lwe-cursor-input-" + std::to_string (getpid ()), script.str ()
    );

    return this->m_script->isLoaded ();
}

void KDECursorInput::unloadKWinScript () { this->m_script.reset (); }

DBusHandlerResult KDECursorInput::handleMessage (DBusConnection* connection, DBusMessage* message, void* userData) {
    const auto input = static_cast<KDECursorInput*> (userData);
    return input->handleMessage (message);
}

DBusHandlerResult KDECursorInput::handleMessage (DBusMessage* message) {
    if (message == nullptr || dbus_message_get_type (message) != DBUS_MESSAGE_TYPE_METHOD_CALL) {
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    return handleMethodCall (message) ? DBUS_HANDLER_RESULT_HANDLED : DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

bool KDECursorInput::handleMethodCall (DBusMessage* message) {
    const auto* member = dbus_message_get_member (message);
    if (member == nullptr || std::strcmp (member, kMethodName) != 0) {
	return false;
    }

    DBusError error;
    dbus_error_init (&error);

    dbus_int32_t x = 0;
    dbus_int32_t y = 0;

    if (!dbus_message_get_args (message, &error, DBUS_TYPE_INT32, &x, DBUS_TYPE_INT32, &y, DBUS_TYPE_INVALID)) {
	dbus_error_free (&error);
	return false;
    }

    m_pos = glm::dvec2 { static_cast<double> (x), static_cast<double> (y) };

    if (m_connection != nullptr) {
	DBusMessage* reply = dbus_message_new_method_return (message);
	if (reply != nullptr) {
	    dbus_connection_send (m_connection, reply, nullptr);
	    dbus_message_unref (reply);
	}
    }

    return true;
}

std::optional<glm::dvec2> KDECursorInput::position () {
    // KWin sends a call per cursor move, many per frame
    if (m_connection != nullptr) {
	if (!WallpaperEngine::Desktop::drainBus (m_connection)) {
	    sLog.error ("KDE cursor input: DBus connection dropped unexpectedly");
	    m_script.reset ();
	    dbus_connection_unregister_object_path (m_connection, kObjectPath);
	    dbus_connection_unref (m_connection);
	    m_connection = nullptr;
	}
    }

    return m_pos;
}

bool KDECursorInput::isInitialized () const { return m_connection != nullptr && m_script && m_script->isLoaded (); }

} // namespace WallpaperEngine::Input::Drivers
