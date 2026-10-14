#include "HostWayland.h"

#include "GnomeShell.h"
#include "SessionBus.h"
#include "WallpaperEngine/Logging/Log.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <poll.h>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <wayland-client.h>

namespace {
constexpr int HELPER_TIMEOUT_MS = 5000;
constexpr const char* HELPER = "gnome-shell-extension/linux-wallpaperengine@wallpaperenginelover.github.io/"
			       "linux-wallpaperengine-gnome-helper";

// the helpers live until these close, which is when the engine goes
std::vector<int> g_helperConnections;
std::atomic<int> g_helperCounter { 0 };

void onGlobal (void* data, wl_registry*, uint32_t, const char* interface, uint32_t) {
    if (std::string_view (interface) == "zwlr_layer_shell_v1") {
	*static_cast<bool*> (data) = true;
    }
}

void onGlobalRemove (void*, wl_registry*, uint32_t) { }

constexpr wl_registry_listener probeListener = { .global = onGlobal, .global_remove = onGlobalRemove };

bool sandboxSocketHasLayerShell () {
    wl_display* display = wl_display_connect (nullptr);
    if (display == nullptr) {
	return true;
    }

    bool found = false;
    wl_registry* registry = wl_display_get_registry (display);
    wl_registry_add_listener (registry, &probeListener, &found);
    wl_display_roundtrip (display);
    wl_registry_destroy (registry);
    wl_display_disconnect (display);
    return found;
}

/** the helper next to this binary, as the host sees it through the app's deployment folder */
std::string hostHelperPath () {
    std::error_code error;
    const auto exe = std::filesystem::read_symlink ("/proc/self/exe", error);
    const auto helper = exe.parent_path () / HELPER;
    const std::string appPath = WallpaperEngine::Desktop::flatpakInfo ("app-path");

    if (error || appPath.empty () || !std::filesystem::exists (helper, error)) {
	return {};
    }

    const std::string inside = helper.string ();
    return inside.starts_with ("/app/") ? appPath + inside.substr (4) : std::string {};
}

/** flatpak-spawn as a grandchild, so the sandbox's init reaps it and nothing waits here */
bool spawnDetached (const std::vector<std::string>& arguments) {
    // built before fork, the children of a threaded process must not allocate
    std::vector<char*> argv;
    for (const auto& argument : arguments) {
	argv.push_back (const_cast<char*> (argument.c_str ()));
    }
    argv.push_back (nullptr);

    const pid_t child = fork ();

    if (child < 0) {
	return false;
    }

    if (child == 0) {
	if (fork () == 0) {
	    execvp (argv[0], argv.data ());
	}
	_exit (0);
    }

    waitpid (child, nullptr, 0);
    return true;
}

int hostConnection () {
    const std::string helper = hostHelperPath ();
    const std::string directory = WallpaperEngine::Desktop::runtimeDirectory ();
    const char* display = std::getenv ("WAYLAND_DISPLAY");

    if (helper.empty () || directory.empty ()) {
	sLog.error ("No Wayland helper to reach the compositor from the host");
	return -1;
    }

    // the app's runtime folder is the same path on the host
    const std::string path = directory + "/lwe-wayland-" + WallpaperEngine::Desktop::processTag () + "-"
	+ std::to_string (g_helperCounter.fetch_add (1)) + ".sock";
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;

    if (path.size () >= sizeof (address.sun_path)) {
	return -1;
    }

    std::strncpy (address.sun_path, path.c_str (), sizeof (address.sun_path) - 1);
    unlink (path.c_str ());

    const int listener = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    const mode_t mask = umask (0077);
    const bool bound = listener >= 0 && bind (listener, reinterpret_cast<sockaddr*> (&address), sizeof (address)) == 0
	&& listen (listener, 1) == 0;
    umask (mask);

    int fd = -1;

    if (bound) {
	// the sandbox's socket keeps the host's name unless that is a path or not "wayland-*", then it's wayland-0,
	// left to the host's own WAYLAND_DISPLAY
	const std::string name = std::filesystem::path (display != nullptr ? display : "wayland-0").filename ();
	const char* runtime = std::getenv ("XDG_RUNTIME_DIR");
	std::vector<std::string> arguments { "flatpak-spawn", "--host" };

	if (name != "wayland-0") {
	    arguments.push_back ("--env=WAYLAND_DISPLAY=" + name);
	}

	arguments.push_back (std::string ("--env=XDG_RUNTIME_DIR=") + (runtime != nullptr ? runtime : ""));
	arguments.push_back (helper);
	arguments.push_back (path);
	spawnDetached (arguments);

	pollfd waiting = { .fd = listener, .events = POLLIN, .revents = 0 };
	if (poll (&waiting, 1, HELPER_TIMEOUT_MS) == 1) {
	    const int connection = accept4 (listener, nullptr, nullptr, SOCK_CLOEXEC);
	    fd = connection >= 0 ? WallpaperEngine::Desktop::receiveDescriptor (connection) : -1;

	    if (fd >= 0) {
		g_helperConnections.push_back (connection);
	    } else if (connection >= 0) {
		close (connection);
	    }
	}
    }

    if (listener >= 0) {
	close (listener);
    }
    unlink (path.c_str ());

    if (fd < 0) {
	sLog.error ("The host helper never handed over a Wayland connection");
    }

    return fd;
}
} // namespace

bool WallpaperEngine::Desktop::usesHostWayland () {
    static const bool host
	= access ("/.flatpak-info", F_OK) == 0 && !isGnomeSession () && !sandboxSocketHasLayerShell ();
    return host;
}

wl_display* WallpaperEngine::Desktop::connectWayland () {
    if (usesHostWayland ()) {
	if (const int fd = hostConnection (); fd >= 0) {
	    return wl_display_connect_to_fd (fd);
	}
    }

    return wl_display_connect (nullptr);
}
