#include "UserShortcut.h"
#include "IconTheme.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "MimeTypes.h"
#include "WallpaperEngine/Logging/Log.h"
#include "nlohmann/json.hpp"

extern char** environ;

using namespace WallpaperEngine::Desktop;

namespace {
std::string trim (const std::string& value) {
    const auto begin = value.find_first_not_of (" \t\r\n");

    if (begin == std::string::npos) {
	return "";
    }

    return value.substr (begin, value.find_last_not_of (" \t\r\n") - begin + 1);
}

std::filesystem::path homeDir () {
    const char* home = std::getenv ("HOME");
    return home != nullptr ? home : "";
}

std::filesystem::path configHome () {
    const char* dir = std::getenv ("XDG_CONFIG_HOME");
    return dir != nullptr && *dir ? std::filesystem::path (dir) : homeDir () / ".config";
}

std::vector<std::filesystem::path> applicationDirs () {
    std::vector<std::filesystem::path> result;
    const char* dataHome = std::getenv ("XDG_DATA_HOME");
    const char* dirs = std::getenv ("XDG_DATA_DIRS");

    result.push_back (
	(dataHome != nullptr && *dataHome ? std::filesystem::path (dataHome) : homeDir () / ".local/share")
	/ "applications"
    );

    std::stringstream stream (dirs != nullptr && *dirs ? dirs : "/usr/local/share:/usr/share");
    std::string dir;

    while (std::getline (stream, dir, ':')) {
	if (!dir.empty ()) {
	    result.push_back (std::filesystem::path (dir) / "applications");
	}
    }

    return result;
}

/** A path or a desktop file id ("org.kde.dolphin" or "firefox.desktop") to the .desktop file */
std::optional<std::filesystem::path> findDesktopFile (const std::string& value) {
    std::error_code error;

    if (value.starts_with ('/')) {
	return std::filesystem::is_regular_file (value, error) ? std::optional<std::filesystem::path> (value) : std::nullopt;
    }

    const std::string id = value.ends_with (".desktop") ? value : value + ".desktop";

    for (const auto& dir : applicationDirs ()) {
	if (std::filesystem::is_regular_file (dir / id, error)) {
	    return dir / id;
	}
    }

    return std::nullopt;
}

std::map<std::string, std::string> readDesktopEntry (const std::filesystem::path& file) {
    std::map<std::string, std::string> result;
    std::ifstream stream (file);
    std::string line;
    bool inEntry = false;

    while (std::getline (stream, line)) {
	if (line.starts_with ('[')) {
	    inEntry = line == "[Desktop Entry]";
	    continue;
	}

	const auto separator = line.find ('=');

	// localized keys (Name[de]) are skipped, the untranslated one is always there
	if (!inEntry || separator == std::string::npos || line.find ('[') < separator) {
	    continue;
	}

	result.try_emplace (trim (line.substr (0, separator)), trim (line.substr (separator + 1)));
    }

    return result;
}

/** Splits an Exec line the way the desktop entry spec quotes it */
std::vector<std::string> splitExec (const std::string& exec) {
    std::vector<std::string> result;
    std::string current;
    bool quoted = false;
    bool hasToken = false;

    for (size_t i = 0; i < exec.size (); i++) {
	const char c = exec[i];

	if (quoted) {
	    if (c == '\\' && i + 1 < exec.size ()) {
		current += exec[++i];
	    } else if (c == '"') {
		quoted = false;
	    } else {
		current += c;
	    }
	} else if (c == '"') {
	    quoted = true;
	    hasToken = true;
	} else if (c == ' ' || c == '\t') {
	    if (hasToken) {
		result.push_back (current);
		current.clear ();
		hasToken = false;
	    }
	} else {
	    current += c;
	    hasToken = true;
	}
    }

    if (hasToken) {
	result.push_back (current);
    }

    return result;
}

/** Nothing gets opened with the app, so file/url field codes go away; the rest is filled in */
std::vector<std::string> expandExec (
    const std::string& exec, const std::map<std::string, std::string>& entry, const std::filesystem::path& file
) {
    std::vector<std::string> result;
    const auto value = [&entry] (const char* key) {
	const auto it = entry.find (key);
	return it != entry.end () ? it->second : std::string ();
    };

    for (const auto& argument : splitExec (exec)) {
	if (argument == "%i") {
	    if (const auto icon = value ("Icon"); !icon.empty ()) {
		result.emplace_back ("--icon");
		result.push_back (icon);
	    }
	    continue;
	}

	std::string expanded;
	bool dropped = false;

	for (size_t i = 0; i < argument.size (); i++) {
	    if (argument[i] != '%' || i + 1 >= argument.size ()) {
		expanded += argument[i];
		continue;
	    }

	    switch (argument[++i]) {
		case '%': expanded += '%'; break;
		case 'c': expanded += value ("Name"); break;
		case 'k': expanded += file.string (); break;
		default: dropped = argument.size () == 2; break;
	    }
	}

	if (!dropped) {
	    result.push_back (expanded);
	}
    }

    return result;
}

std::optional<std::string> defaultBrowserIcon () {
    const std::vector<std::filesystem::path> lists = {
	configHome () / "mimeapps.list",
	homeDir () / ".local/share/applications/mimeapps.list",
	"/usr/share/applications/mimeapps.list",
    };

    for (const auto& list : lists) {
	std::ifstream stream (list);
	std::string line;
	bool inDefaults = false;

	while (std::getline (stream, line)) {
	    if (line.starts_with ('[')) {
		inDefaults = line == "[Default Applications]";
		continue;
	    }

	    if (!inDefaults || !line.starts_with ("x-scheme-handler/https=")) {
		continue;
	    }

	    const std::string id = line.substr (line.find ('=') + 1, line.find (';') - line.find ('=') - 1);

	    if (const auto desktop = findDesktopFile (id); desktop.has_value ()) {
		const auto entry = readDesktopEntry (*desktop);

		if (const auto icon = entry.find ("Icon"); icon != entry.end ()) {
		    return icon->second;
		}
	    }
	}
    }

    return std::nullopt;
}

bool isExecutableFile (const std::string& path) {
    struct stat info {};
    return stat (path.c_str (), &info) == 0 && S_ISREG (info.st_mode) && access (path.c_str (), X_OK) == 0;
}

/**
 * Double fork so the app is reparented away from the engine and never becomes a zombie. Everything the child
 * needs is prepared before fork() since the engine is multithreaded. The engine's own library path and preloads
 * would break other programs, so they are left out of the environment.
 */
bool spawnDetached (const std::vector<std::string>& arguments, const std::string& workingDirectory = "") {
    if (arguments.empty ()) {
	return false;
    }

    std::vector<char*> argv;
    for (const auto& argument : arguments) {
	argv.push_back (const_cast<char*> (argument.c_str ()));
    }
    argv.push_back (nullptr);

    std::vector<char*> envp;
    for (char** variable = environ; *variable != nullptr; variable++) {
	if (strncmp (*variable, "LD_LIBRARY_PATH=", 16) != 0 && strncmp (*variable, "LD_PRELOAD=", 11) != 0) {
	    envp.push_back (*variable);
	}
    }
    envp.push_back (nullptr);

    const char* directory = workingDirectory.empty () ? nullptr : workingDirectory.c_str ();
    // exec closes this pipe, so reading nothing back means the program started
    int errorPipe[2];

    if (pipe2 (errorPipe, O_CLOEXEC) != 0) {
	return false;
    }

    const pid_t child = fork ();

    if (child < 0) {
	close (errorPipe[0]);
	close (errorPipe[1]);
	return false;
    }

    if (child == 0) {
	close (errorPipe[0]);

	if (fork () == 0) {
	    setsid ();

	    // a missing Path= folder isn't worth failing over, the program starts where the engine runs
	    if (directory != nullptr && chdir (directory) != 0) {
		directory = nullptr;
	    }

	    const int devNull = open ("/dev/null", O_RDWR);

	    if (devNull >= 0) {
		dup2 (devNull, STDIN_FILENO);
		dup2 (devNull, STDOUT_FILENO);
		dup2 (devNull, STDERR_FILENO);
	    }

	    const auto keep = static_cast<unsigned int> (errorPipe[1]);

	    if (keep > 3) {
		close_range (3, keep - 1, 0);
	    }
	    close_range (keep + 1, ~0U, 0);

	    execvpe (argv[0], argv.data (), envp.data ());

	    const int failure = errno;

	    if (write (errorPipe[1], &failure, sizeof (failure)) < 0) {
		_exit (126);
	    }

	    _exit (127);
	}

	_exit (0);
    }

    close (errorPipe[1]);

    int status = 0;
    waitpid (child, &status, 0);

    int failure = 0;
    const bool failed = read (errorPipe[0], &failure, sizeof (failure)) == sizeof (failure);
    close (errorPipe[0]);

    if (failed) {
	sLog.error ("User shortcut: cannot start ", arguments.front (), ": ", strerror (failure));
	return false;
    }

    return WIFEXITED (status) && WEXITSTATUS (status) == 0;
}

std::vector<std::string> terminalPrefix () {
    if (const char* terminal = std::getenv ("TERMINAL"); terminal != nullptr && *terminal) {
	return { terminal, "-e" };
    }

    for (const auto& [program, flag] : std::initializer_list<std::pair<const char*, const char*>> {
	     { "konsole", "-e" }, { "gnome-terminal", "--" }, { "xterm", "-e" } }) {
	for (const char* dir : { "/usr/bin/", "/usr/local/bin/" }) {
	    if (access ((std::string (dir) + program).c_str (), X_OK) == 0) {
		return { program, flag };
	    }
	}
    }

    return { "xterm", "-e" };
}
} // namespace

std::optional<UserShortcut> UserShortcut::parse (const std::string& raw) {
    const std::string value = trim (raw);

    if (value.empty ()) {
	return std::nullopt;
    }

    if (value.starts_with ('{')) {
	const auto json = nlohmann::json::parse (value, nullptr, false);

	if (json.is_discarded () || !json.is_object () || !json.value ("isbound", true)) {
	    return std::nullopt;
	}

	const std::string type = json.value ("commandtype", "");
	UserShortcut result {
	    .type = Type::File, .target = trim (json.value ("file", "")), .arguments = json.value ("arguments", "")
	};

	if (type == "application") {
	    result.type = Type::Application;
	} else if (type == "directory") {
	    result.type = Type::Directory;
	} else if (type == "web") {
	    result.type = Type::Web;
	} else if (type == "command") {
	    result.type = Type::Command;
	} else if (type != "file") {
	    return std::nullopt;
	}

	return result.target.empty () ? std::nullopt : std::optional<UserShortcut> (result);
    }

    std::error_code error;

    if (value.ends_with (".desktop") || (!value.starts_with ('/') && findDesktopFile (value).has_value ())) {
	return UserShortcut { .type = Type::Application, .target = value, .arguments = "" };
    }
    if (value.find ("://") != std::string::npos || value.starts_with ("www.")) {
	return UserShortcut { .type = Type::Web, .target = value, .arguments = "" };
    }
    if (std::filesystem::is_directory (value, error)) {
	return UserShortcut { .type = Type::Directory, .target = value, .arguments = "" };
    }

    return UserShortcut { .type = Type::File, .target = value, .arguments = "" };
}

std::optional<std::filesystem::path> UserShortcut::iconPath () const {
    auto& icons = IconTheme::get ();
    std::vector<std::string> candidates;

    switch (this->type) {
	case Type::Application:
	    if (const auto desktop = findDesktopFile (this->target); desktop.has_value ()) {
		const auto entry = readDesktopEntry (*desktop);

		if (const auto icon = entry.find ("Icon"); icon != entry.end ()) {
		    candidates.push_back (icon->second);
		}
	    }
	    candidates.emplace_back ("application-x-executable");
	    break;
	case Type::Directory:
	    candidates = { "folder", "inode-directory" };
	    break;
	case Type::Web:
	    if (const auto icon = defaultBrowserIcon (); icon.has_value ()) {
		candidates.push_back (*icon);
	    }
	    candidates.insert (candidates.end (), { "internet-web-browser", "applications-internet" });
	    break;
	case Type::Command:
	    candidates = { "utilities-terminal", "terminal" };
	    break;
	case Type::File:
	    if (isExecutableFile (this->target)) {
		candidates.emplace_back ("application-x-executable");
	    } else if (const char* mime = MimeTypes::getType (this->target.c_str ()); mime != nullptr) {
		std::string name = mime;
		std::ranges::replace (name, '/', '-');
		candidates.push_back (name);
		candidates.push_back (name.substr (0, name.find ('-')) + "-x-generic");
	    }
	    candidates.insert (candidates.end (), { "text-x-generic", "unknown" });
	    break;
    }

    for (const auto& candidate : candidates) {
	if (auto path = icons.find (candidate); path.has_value ()) {
	    return path;
	}
    }

    return std::nullopt;
}

bool UserShortcut::launch () const {
    switch (this->type) {
	case Type::Application:
	    {
		const auto desktop = findDesktopFile (this->target);

		if (!desktop.has_value ()) {
		    sLog.error ("User shortcut: no application ", this->target);
		    return false;
		}

		const auto entry = readDesktopEntry (*desktop);
		const auto exec = entry.find ("Exec");

		if (exec == entry.end ()) {
		    sLog.error ("User shortcut: ", desktop->string (), " has no Exec line");
		    return false;
		}

		auto arguments = expandExec (exec->second, entry, *desktop);
		const auto terminal = entry.find ("Terminal");

		if (terminal != entry.end () && terminal->second == "true") {
		    auto prefix = terminalPrefix ();
		    arguments.insert (arguments.begin (), prefix.begin (), prefix.end ());
		}

		const auto path = entry.find ("Path");

		return spawnDetached (arguments, path != entry.end () ? path->second : "");
	    }
	case Type::Web:
	    return spawnDetached (
		{ "xdg-open", this->target.find ("://") == std::string::npos ? "https://" + this->target : this->target }
	    );
	case Type::Directory:
	    return spawnDetached ({ "xdg-open", this->target });
	case Type::File:
	    if (isExecutableFile (this->target)) {
		auto arguments = splitExec (this->arguments);
		arguments.insert (arguments.begin (), this->target);
		return spawnDetached (arguments, std::filesystem::path (this->target).parent_path ().string ());
	    }
	    return spawnDetached ({ "xdg-open", this->target });
	case Type::Command:
	    return spawnDetached (
		{ "/bin/sh", "-c", this->arguments.empty () ? this->target : this->target + " " + this->arguments }
	    );
    }

    return false;
}
