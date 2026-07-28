#include "KDECursorInput.h"

#include "WallpaperEngine/Logging/Log.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace WallpaperEngine::Input::Drivers {

KDECursorInput::KDECursorInput () {
    if (!initializeDBus ()) {
	sLog.out ("KDE cursor input could not initialize DBus, mouse-reactive effects may not work on this session");
	return;
    }

    if (!loadKWinScript ()) {
	sLog.out ("KDE cursor input could not load its KWin script, mouse-reactive effects may not work on this session");
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
    const std::string serviceName = std::string (kServiceName) + "." + std::to_string (getpid ());
    const auto requestResult = dbus_bus_request_name (m_connection, serviceName.c_str (), DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);

    if (dbus_error_is_set (&error)) {
	sLog.error ("Failed to request DBus service ", serviceName, ": ", error.message);
	dbus_error_free (&error);
	dbus_connection_unref (m_connection);
	m_connection = nullptr;
	return false;
    }

    if (requestResult != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER && requestResult != DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER) {
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
    const char* runtimeDir = std::getenv ("XDG_RUNTIME_DIR");
    if (runtimeDir == nullptr) {
	sLog.error ("KDE cursor input: XDG_RUNTIME_DIR is not set");
	return false;
    }

    const std::string serviceName = std::string (kServiceName) + "." + std::to_string (getpid ());

    std::ostringstream script;
    script << "function lweReportCursorPos() {\n"
	   << "    var p = workspace.cursorPos;\n"
	   << "    callDBus(\"" << serviceName << "\", \"" << kObjectPath << "\", \"" << serviceName << "\", \""
	   << kMethodName << "\", p.x, p.y);\n"
	   << "}\n"
	   << "workspace.cursorPosChanged.connect(lweReportCursorPos);\n"
	   << "lweReportCursorPos();\n";

    m_scriptPath = std::string (runtimeDir) + "/linux-wallpaperengine-cursor-" + std::to_string (getpid ()) + ".js";

    std::ofstream out (m_scriptPath, std::ios::trunc);
    if (!out.is_open ()) {
	sLog.error ("KDE cursor input: could not write KWin script to ", m_scriptPath);
	return false;
    }
    out << script.str ();
    out.close ();

    m_pluginName = "lwe-cursor-input-" + std::to_string (getpid ());

    DBusMessage* loadCall = dbus_message_new_method_call ("org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "loadScript");
    if (loadCall == nullptr) {
	return false;
    }

    const char* pathArg = m_scriptPath.c_str ();
    const char* nameArg = m_pluginName.c_str ();
    dbus_message_append_args (
	loadCall, DBUS_TYPE_STRING, &pathArg, DBUS_TYPE_STRING, &nameArg, DBUS_TYPE_INVALID
    );

    DBusError error;
    dbus_error_init (&error);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block (m_connection, loadCall, 2000, &error);
    dbus_message_unref (loadCall);

    if (dbus_error_is_set (&error)) {
	sLog.error ("KDE cursor input: failed to load KWin script: ", error.message);
	dbus_error_free (&error);
	std::remove (m_scriptPath.c_str ());
	return false;
    }

    dbus_int32_t scriptId = -1;
    if (reply != nullptr) {
	dbus_message_get_args (reply, nullptr, DBUS_TYPE_INT32, &scriptId, DBUS_TYPE_INVALID);
	dbus_message_unref (reply);
    }

    if (scriptId < 0) {
	sLog.error ("KDE cursor input: KWin refused to load the cursor tracking script");
	std::remove (m_scriptPath.c_str ());
	return false;
    }

    const std::string scriptObjectPath = "/Scripting/Script" + std::to_string (scriptId);
    DBusMessage* runCall = dbus_message_new_method_call ("org.kde.KWin", scriptObjectPath.c_str (), "org.kde.kwin.Script", "run");
    if (runCall != nullptr) {
	dbus_error_init (&error);
	DBusMessage* runReply = dbus_connection_send_with_reply_and_block (m_connection, runCall, 2000, &error);
	dbus_message_unref (runCall);
	if (runReply != nullptr) {
	    dbus_message_unref (runReply);
	}
	if (dbus_error_is_set (&error)) {
	    sLog.error ("KDE cursor input: failed to start the cursor tracking script: ", error.message);
	    dbus_error_free (&error);
	}
    }

    m_scriptLoaded = true;
    return true;
}

void KDECursorInput::unloadKWinScript () {
    if (m_scriptLoaded && m_connection != nullptr) {
	DBusMessage* unloadCall
	    = dbus_message_new_method_call ("org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "unloadScript");
	if (unloadCall != nullptr) {
	    const char* nameArg = m_pluginName.c_str ();
	    dbus_message_append_args (unloadCall, DBUS_TYPE_STRING, &nameArg, DBUS_TYPE_INVALID);

	    DBusError error;
	    dbus_error_init (&error);
	    DBusMessage* reply = dbus_connection_send_with_reply_and_block (m_connection, unloadCall, 2000, &error);
	    dbus_message_unref (unloadCall);
	    if (reply != nullptr) {
		dbus_message_unref (reply);
	    }
	    if (dbus_error_is_set (&error)) {
		dbus_error_free (&error);
	    }
	}
    }

    if (!m_scriptPath.empty ()) {
	std::remove (m_scriptPath.c_str ());
    }

    m_scriptLoaded = false;
}

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
    if (m_connection != nullptr) {
	if (!dbus_connection_read_write_dispatch (m_connection, 0)) {
	    sLog.error ("KDE cursor input: DBus connection dropped unexpectedly");
	    m_scriptLoaded = false;
	    dbus_connection_unregister_object_path (m_connection, kObjectPath);
	    dbus_connection_unref (m_connection);
	    m_connection = nullptr;
	}
    }

    return m_pos;
}

bool KDECursorInput::isInitialized () const { return m_connection != nullptr && m_scriptLoaded; }

} // namespace WallpaperEngine::Input::Drivers
