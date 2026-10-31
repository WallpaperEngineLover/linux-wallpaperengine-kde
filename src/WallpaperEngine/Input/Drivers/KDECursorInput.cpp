#include "KDECursorInput.h"

#include "WallpaperEngine/Desktop/SessionBus.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Logging/ThreadGuard.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace WallpaperEngine::Input::Drivers {

namespace {
    constexpr std::chrono::seconds RELOAD_RETRY (5);
}

struct KDECursorInput::ScriptHandle {
    DBusConnection* bus = nullptr;
    std::unique_ptr<WallpaperEngine::Desktop::KWinScript> script;

    ~ScriptHandle () {
	this->script.reset ();
	WallpaperEngine::Desktop::closeBus (this->bus);
    }
};

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
    if (m_reload.valid ()) {
	m_reload.get ();
    }

    if (m_reconnect.valid ()) {
	if (DBusConnection* connection = m_reconnect.get (); connection != nullptr) {
	    WallpaperEngine::Desktop::closeBus (connection);
	}
    }

    unloadKWinScript ();
    stopDBus ();
}

DBusConnection* KDECursorInput::connectBus () {
    DBusError error;
    dbus_error_init (&error);

    // a private connection: the shared one stays the same disconnected object once the broker dropped it
    DBusConnection* connection = dbus_bus_get_private (DBUS_BUS_SESSION, &error);
    if (dbus_error_is_set (&error)) {
	sLog.error ("Failed to connect to the session DBus: ", error.message);
	dbus_error_free (&error);
	return nullptr;
    }

    if (connection == nullptr) {
	sLog.error ("Failed to connect to the session DBus: unknown error");
	return nullptr;
    }

    dbus_connection_set_exit_on_disconnect (connection, false);

    dbus_error_init (&error);
    const std::string serviceName = std::string (kServiceName) + "_" + WallpaperEngine::Desktop::processTag ();
    const auto requestResult
	= dbus_bus_request_name (connection, serviceName.c_str (), DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);

    if (dbus_error_is_set (&error)) {
	sLog.error ("Failed to request DBus service ", serviceName, ": ", error.message);
	dbus_error_free (&error);
	WallpaperEngine::Desktop::closeBus (connection);
	return nullptr;
    }

    if (requestResult != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER
	&& requestResult != DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER) {
	sLog.error ("DBus service ", serviceName, " is already owned by another process");
	WallpaperEngine::Desktop::closeBus (connection);
	return nullptr;
    }

    return connection;
}

bool KDECursorInput::attachBus (DBusConnection* connection) {
    static constexpr DBusObjectPathVTable objectVTable = {
	.unregister_function = nullptr,
	.message_function = &KDECursorInput::handleMessage,
    };

    if (connection == nullptr) {
	return false;
    }

    if (!dbus_connection_register_object_path (connection, kObjectPath, &objectVTable, this)) {
	sLog.error ("Failed to register DBus object path for KDE cursor input");
	WallpaperEngine::Desktop::closeBus (connection);
	return false;
    }

    m_connection = connection;
    return true;
}

bool KDECursorInput::initializeDBus () { return attachBus (connectBus ()); }

void KDECursorInput::stopDBus () {
    if (m_connection != nullptr) {
	dbus_connection_unregister_object_path (m_connection, kObjectPath);
	WallpaperEngine::Desktop::closeBus (m_connection);
	m_connection = nullptr;
    }
}

static std::string cursorScriptSource (const char* serviceName, const char* objectPath, const char* methodName) {
    std::ostringstream script;
    script << "var lweLastReport = 0;\n"
	   << "var lweTimer = null;\n"
	   << "var lweTimerFailed = false;\n"
	   << "function lweSend() {\n"
	   << "    lweLastReport = Date.now();\n"
	   << "    var p = workspace.cursorPos;\n"
	   << "    callDBus(\"" << serviceName << "\", \"" << objectPath << "\", \"" << serviceName << "\", \""
	   << methodName << "\", p.x, p.y);\n"
	   << "}\n"
	   << "function lweReportCursorPos() {\n"
	   << "    if (Date.now() - lweLastReport >= 5) {\n"
	   << "        lweSend();\n"
	   << "        return;\n"
	   << "    }\n"
	   << "    if (lweTimer === null && !lweTimerFailed) {\n"
	   << "        try {\n"
	   << "            lweTimer = new QTimer();\n"
	   << "            lweTimer.singleShot = true;\n"
	   << "            lweTimer.interval = 5;\n"
	   << "            lweTimer.timeout.connect(lweSend);\n"
	   << "        } catch (e) {\n"
	   << "            lweTimer = null;\n"
	   << "            lweTimerFailed = true;\n"
	   << "        }\n"
	   << "    }\n"
	   << "    if (lweTimer !== null && !lweTimer.active) {\n"
	   << "        lweTimer.start();\n"
	   << "    }\n"
	   << "}\n"
	   << "workspace.cursorPosChanged.connect(lweReportCursorPos);\n"
	   << "lweSend();\n";

    return script.str ();
}

bool KDECursorInput::loadKWinScript () {
    const std::string serviceName = std::string (kServiceName) + "_" + WallpaperEngine::Desktop::processTag ();

    auto handle = std::make_unique<ScriptHandle> ();
    handle->script = std::make_unique<WallpaperEngine::Desktop::KWinScript> (
	m_connection, "lwe-cursor-input-" + WallpaperEngine::Desktop::processTag (),
	cursorScriptSource (serviceName.c_str (), kObjectPath, kMethodName)
    );

    if (!handle->script->isLoaded ()) {
	return false;
    }

    this->m_script = std::move (handle);
    return true;
}

void KDECursorInput::unloadKWinScript () { this->m_script.reset (); }

void KDECursorInput::startReload () {
    const std::string serviceName = std::string (kServiceName) + "_" + WallpaperEngine::Desktop::processTag ();
    // a reload that was dropped after a new pause is still being unloaded on its own thread, under its own name and
    // file
    const std::string pluginName
	= "lwe-cursor-input-" + WallpaperEngine::Desktop::processTag () + "-r" + std::to_string (++m_reloadCount);
    const std::string source = cursorScriptSource (serviceName.c_str (), kObjectPath, kMethodName);

    // KWin may be busy closing the game's window, the render thread must not wait for its answers
    this->m_reload = std::async (std::launch::async, [pluginName, source] {
	std::unique_ptr<ScriptHandle> handle;

	WallpaperEngine::Logging::guardThread ("KWin script reload", [&] {
	    auto made = std::make_unique<ScriptHandle> ();
	    made->bus = WallpaperEngine::Desktop::privateBus (DBUS_BUS_SESSION);

	    if (made->bus != nullptr) {
		made->script = std::make_unique<WallpaperEngine::Desktop::KWinScript> (made->bus, pluginName, source);

		if (!made->script->isLoaded ()) {
		    made->script.reset ();
		}
	    }

	    handle = std::move (made);
	});

	return handle;
    });
}

void KDECursorInput::pollReload () {
    if (!m_reload.valid () || m_reload.wait_for (std::chrono::seconds (0)) != std::future_status::ready) {
	return;
    }

    auto handle = m_reload.get ();

    if (m_suspended || m_script != nullptr) {
	if (handle != nullptr) {
	    std::thread ([discarded = std::move (handle)] () mutable { discarded.reset (); }).detach ();
	}

	return;
    }

    if (handle != nullptr && handle->script != nullptr) {
	m_script = std::move (handle);
	return;
    }

    sLog.error ("KDE cursor input could not load its KWin script again, trying again in ", RELOAD_RETRY.count (), "s");
    m_nextReload = std::chrono::steady_clock::now () + RELOAD_RETRY;
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
    // KWin sends a call per cursor move, many per frame
    if (m_connection == nullptr && m_lost) {
	if (m_reconnect.valid ()) {
	    if (m_reconnect.wait_for (std::chrono::seconds (0)) == std::future_status::ready) {
		if (attachBus (m_reconnect.get ())) {
		    sLog.out ("KDE cursor input: DBus connection is back");
		    m_lost = false;
		    m_nextReload = {};
		} else {
		    m_nextReconnect = std::chrono::steady_clock::now () + RELOAD_RETRY;
		}
	    }
	} else if (std::chrono::steady_clock::now () >= m_nextReconnect) {
	    // a broker that is slow to answer must not hold the frame loop
	    m_reconnect = std::async (std::launch::async, [] {
		DBusConnection* connection = nullptr;

		WallpaperEngine::Logging::guardThread ("KDE cursor reconnect", [&] { connection = connectBus (); });

		return connection;
	    });
	}
    }

    if (m_connection != nullptr) {
	if (!WallpaperEngine::Desktop::drainBus (m_connection)) {
	    sLog.error (
		"KDE cursor input: DBus connection dropped unexpectedly, trying to reconnect every ",
		RELOAD_RETRY.count (), "s"
	    );
	    m_script.reset ();
	    stopDBus ();
	    m_lost = true;
	    m_nextReconnect = std::chrono::steady_clock::now () + RELOAD_RETRY;
	} else {
	    pollReload ();

	    if (!m_suspended && m_script == nullptr && !m_reload.valid ()
		&& std::chrono::steady_clock::now () >= m_nextReload) {
		startReload ();
	    }
	}
    }

    // no script, no cursor: the last position would pin the pointer and keep the other sources unread
    if (!m_suspended && m_script == nullptr) {
	return std::nullopt;
    }

    return m_pos;
}

// the constructor drops the connection when the first load fails, so a missing script later is a reload in progress
// and the input region must not flip to capturing every click meanwhile
bool KDECursorInput::isInitialized () const { return m_connection != nullptr || m_lost; }

void KDECursorInput::setSuspended (bool suspended) {
    if (m_connection == nullptr) {
	if (m_lost) {
	    m_suspended = suspended;
	}

	return;
    }

    if (m_suspended == suspended) {
	return;
    }

    m_suspended = suspended;

    if (suspended) {
	unloadKWinScript ();
	position ();
	return;
    }

    m_nextReload = {};

    if (!m_reload.valid ()) {
	startReload ();
    }
}

}
