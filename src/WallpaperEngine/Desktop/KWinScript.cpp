#include "KWinScript.h"

#include "WallpaperEngine/Logging/Log.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace WallpaperEngine::Desktop;

namespace {
constexpr int CALL_TIMEOUT_MS = 2000;

std::string scriptDirectory () {
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

DBusMessage* callKWin (DBusConnection* connection, DBusMessage* call, const char* what) {
    DBusError error;
    dbus_error_init (&error);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block (connection, call, CALL_TIMEOUT_MS, &error);
    dbus_message_unref (call);

    if (dbus_error_is_set (&error)) {
	sLog.error ("KWin script: ", what, ": ", error.message);
	dbus_error_free (&error);
	return nullptr;
    }

    return reply;
}
} // namespace

KWinScript::KWinScript (DBusConnection* connection, std::string pluginName, const std::string& source) :
    m_connection (connection), m_pluginName (std::move (pluginName)) {
    const std::string dir = scriptDirectory ();

    if (dir.empty ()) {
	sLog.error ("KWin script ", this->m_pluginName, ": XDG_RUNTIME_DIR is not set");
	return;
    }

    this->m_path = dir + "/" + this->m_pluginName + ".js";
    std::ofstream out (this->m_path, std::ios::trunc);

    if (!out.is_open ()) {
	sLog.error ("KWin script: could not write ", this->m_path);
	this->m_path.clear ();
	return;
    }

    out << source;
    out.close ();

    DBusMessage* load
	= dbus_message_new_method_call ("org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "loadScript");
    const char* pathArg = this->m_path.c_str ();
    const char* nameArg = this->m_pluginName.c_str ();
    dbus_message_append_args (load, DBUS_TYPE_STRING, &pathArg, DBUS_TYPE_STRING, &nameArg, DBUS_TYPE_INVALID);

    DBusMessage* reply = callKWin (this->m_connection, load, "loadScript");
    dbus_int32_t scriptId = -1;

    if (reply != nullptr) {
	dbus_message_get_args (reply, nullptr, DBUS_TYPE_INT32, &scriptId, DBUS_TYPE_INVALID);
	dbus_message_unref (reply);
    }

    if (scriptId < 0) {
	sLog.error ("KWin refused to load the script ", this->m_pluginName);
	std::remove (this->m_path.c_str ());
	this->m_path.clear ();
	return;
    }

    const std::string object = "/Scripting/Script" + std::to_string (scriptId);
    DBusMessage* run = dbus_message_new_method_call ("org.kde.KWin", object.c_str (), "org.kde.kwin.Script", "run");

    if (DBusMessage* runReply = callKWin (this->m_connection, run, "run"); runReply != nullptr) {
	dbus_message_unref (runReply);
    }

    this->m_loaded = true;
}

KWinScript::~KWinScript () {
    if (this->m_loaded && this->m_connection != nullptr) {
	DBusMessage* unload
	    = dbus_message_new_method_call ("org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "unloadScript");
	const char* nameArg = this->m_pluginName.c_str ();
	dbus_message_append_args (unload, DBUS_TYPE_STRING, &nameArg, DBUS_TYPE_INVALID);

	if (DBusMessage* reply = callKWin (this->m_connection, unload, "unloadScript"); reply != nullptr) {
	    dbus_message_unref (reply);
	}
    }

    if (!this->m_path.empty ()) {
	std::remove (this->m_path.c_str ());
    }
}
