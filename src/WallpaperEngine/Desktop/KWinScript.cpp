#include "KWinScript.h"
#include "SessionBus.h"

#include "WallpaperEngine/Logging/Log.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/file.h>
#include <unistd.h>

extern char** environ;

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

constexpr const char* SCRIPT_PREFIX = "lwe-";

void unloadScript (DBusConnection* connection, const std::string& pluginName) {
    DBusMessage* unload
	= dbus_message_new_method_call ("org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "unloadScript");
    const char* nameArg = pluginName.c_str ();
    dbus_message_append_args (unload, DBUS_TYPE_STRING, &nameArg, DBUS_TYPE_INVALID);

    if (DBusMessage* reply = callKWin (connection, unload, "unloadScript"); reply != nullptr) {
	dbus_message_unref (reply);
    }
}

bool writeAll (int descriptor, const std::string& text) {
    for (size_t done = 0; done < text.size ();) {
	const ssize_t wrote = ::write (descriptor, text.data () + done, text.size () - done);

	if (wrote < 0 && errno == EINTR) {
	    continue;
	}

	if (wrote <= 0) {
	    return false;
	}

	done += static_cast<size_t> (wrote);
    }

    return true;
}

/** The kwin janitor helper of this engine: unloads its scripts once the pipe to it closes, however the engine ended:
 * the destructors never run for a SIGKILL, and a script of a dead engine makes KWin call a service that isn't there at
 * every cursor move */
class Janitor {
public:
    static Janitor& instance () {
	static Janitor janitor;
	return janitor;
    }

    void watch (const std::string& pluginName, const std::string& path) {
	this->send ("+" + pluginName + "\t" + path + "\n");
    }

    void forget (const std::string& pluginName) { this->send ("-" + pluginName + "\n"); }

private:
    void start () {
	this->m_started = true;

	// next to the engine like the web helper, the engine itself is far too big for a watcher (112 MB resident)
	std::error_code error;
	const auto helper = std::filesystem::read_symlink ("/proc/self/exe", error).parent_path ()
	    / "linux-wallpaperengine-kwin-janitor";

	if (error || !std::filesystem::exists (helper, error)) {
	    return;
	}

	int channel[2];

	if (pipe2 (channel, O_CLOEXEC) != 0) {
	    return;
	}

	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init (&actions);
	// dup2 clears the close-on-exec flag of the copy, the write end must not reach the helper or anything else
	posix_spawn_file_actions_adddup2 (&actions, channel[0], STDIN_FILENO);
	posix_spawn_file_actions_addopen (&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_addopen (&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

	// its own session: a kill of the engine's process group (or a Ctrl-C) must not take it along
	posix_spawnattr_t attributes;
	posix_spawnattr_init (&attributes);
	posix_spawnattr_setflags (&attributes, POSIX_SPAWN_SETSID);

	const std::string program = helper.string ();
	char* const arguments[] = { const_cast<char*> (program.c_str ()), nullptr };
	pid_t child = 0;
	const int result = posix_spawn (&child, program.c_str (), &actions, &attributes, arguments, environ);

	posix_spawnattr_destroy (&attributes);
	posix_spawn_file_actions_destroy (&actions);
	close (channel[0]);

	if (result != 0) {
	    close (channel[1]);
	    return;
	}

	this->m_pipe = channel[1];
    }

    void send (const std::string& line) {
	if (!this->m_started) {
	    this->start ();
	}

	if (this->m_pipe >= 0 && !writeAll (this->m_pipe, line)) {
	    close (this->m_pipe);
	    this->m_pipe = -1;
	}
    }

    int m_pipe = -1;
    bool m_started = false;
};
} // namespace

std::vector<std::string>
KWinScript::findStale (const std::filesystem::path& directory, std::chrono::seconds minimumAge) {
    std::vector<std::string> stale;
    std::error_code error;

    for (const auto& entry : std::filesystem::directory_iterator (directory, error)) {
	const std::string name = entry.path ().stem ().string ();

	if (!entry.is_regular_file (error) || entry.path ().extension () != ".js"
	    || !name.starts_with (SCRIPT_PREFIX)) {
	    continue;
	}

	const auto modified = entry.last_write_time (error);

	if (!error && std::filesystem::file_time_type::clock::now () - modified < minimumAge) {
	    continue;
	}

	// an engine from before the lock existed has none: its name ends in its pid, which tells while it runs in this
	// pid namespace (a Flatpak's "<instance>_<pid>" doesn't parse and relies on the lock)
	const auto tag = name.substr (name.find_last_of ('-') + 1);

	if (!tag.empty () && std::ranges::all_of (tag, [] (unsigned char c) { return std::isdigit (c); })) {
	    const auto pid = std::strtol (tag.c_str (), nullptr, 10);

	    if (pid > 0 && (kill (static_cast<pid_t> (pid), 0) == 0 || errno == EPERM)) {
		continue;
	    }
	}

	// a running engine holds a shared lock on its file for as long as it lives, wherever its pid namespace is
	const int file = open (entry.path ().c_str (), O_RDONLY | O_CLOEXEC);

	if (file < 0) {
	    continue;
	}

	if (flock (file, LOCK_EX | LOCK_NB) == 0) {
	    stale.push_back (name);
	}

	close (file);
    }

    return stale;
}

void KWinScript::unloadStale (DBusConnection* connection) {
    const std::string dir = runtimeDirectory ();

    if (dir.empty ()) {
	return;
    }

    for (const auto& name : findStale (dir)) {
	sLog.out ("Unloading the KWin script ", name, " of an engine that is gone");
	unloadScript (connection, name);
	std::remove ((dir + "/" + name + ".js").c_str ());
    }
}

KWinScript::KWinScript (DBusConnection* connection, std::string pluginName, const std::string& source) :
    m_connection (connection), m_pluginName (std::move (pluginName)) {
    const std::string dir = runtimeDirectory ();

    if (dir.empty ()) {
	sLog.error ("KWin script ", this->m_pluginName, ": XDG_RUNTIME_DIR is not set");
	return;
    }

    // scripts of engines that were killed since the last start
    unloadStale (this->m_connection);

    this->m_path = dir + "/" + this->m_pluginName + ".js";
    this->m_lock = open (this->m_path.c_str (), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

    if (this->m_lock < 0 || flock (this->m_lock, LOCK_SH) != 0 || !writeAll (this->m_lock, source)) {
	sLog.error ("KWin script: could not write ", this->m_path);
	this->m_path.clear ();

	if (this->m_lock >= 0) {
	    close (this->m_lock);
	    this->m_lock = -1;
	}

	return;
    }

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
    Janitor::instance ().watch (this->m_pluginName, this->m_path);

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
	unloadScript (this->m_connection, this->m_pluginName);
    }

    if (this->m_registered) {
	Janitor::instance ().forget (this->m_pluginName);
    }

    if (!this->m_path.empty ()) {
	std::remove (this->m_path.c_str ());
    }

    // the lock goes with the descriptor, after the file is gone
    if (this->m_lock >= 0) {
	close (this->m_lock);
    }
}
