#include "KWinScript.h"
#include "SessionBus.h"

#include "WallpaperEngine/Logging/Log.h"

#include <cstdio>
#include <fstream>

using namespace WallpaperEngine::Desktop;

namespace {
DBusMessage* callKWin (DBusConnection* connection, DBusMessage* call, const char* what) {
    std::string error;
    DBusMessage* reply = callBlocking (connection, call, error);

    if (reply == nullptr) {
	sLog.error ("KWin script: ", what, ": ", error);
    }

    return reply;
}
} // namespace

KWinScript::KWinScript (DBusConnection* connection, std::string pluginName, const std::string& source) :
    m_connection (connection), m_pluginName (std::move (pluginName)) {
    const std::string dir = runtimeDirectory ();

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

    // the destructor unloads it even if run fails
    this->m_registered = true;

    // KWin 6 puts the script at /Scripting/Script<id>, KWin 5 at /<id>
    std::string error;

    for (const std::string& object :
	 { "/Scripting/Script" + std::to_string (scriptId), "/" + std::to_string (scriptId) }) {
	DBusMessage* run = dbus_message_new_method_call ("org.kde.KWin", object.c_str (), "org.kde.kwin.Script", "run");

	if (DBusMessage* reply = callBlocking (this->m_connection, run, error); reply != nullptr) {
	    dbus_message_unref (reply);
	    this->m_running = true;
	    return;
	}
    }

    sLog.error ("KWin script: run: ", error);
}

KWinScript::~KWinScript () {
    if (this->m_registered && this->m_connection != nullptr) {
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
