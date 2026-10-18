#include "HyprlandIPC.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
int connectTo (const std::string& socketPath) {
    sockaddr_un addr {};
    if (socketPath.size () >= sizeof (addr.sun_path)) {
	return -1;
    }

    const int fd = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
	return -1;
    }

    timeval timeout {};
    timeout.tv_usec = 50000;
    addr.sun_family = AF_UNIX;
    std::strncpy (addr.sun_path, socketPath.c_str (), sizeof (addr.sun_path) - 1);

    if (setsockopt (fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof (timeout)) != 0
	|| setsockopt (fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof (timeout)) != 0
	|| connect (fd, reinterpret_cast<sockaddr*> (&addr), sizeof (addr)) != 0) {
	close (fd);
	return -1;
    }

    return fd;
}

/** the leftover socket of a crashed Hyprland refuses connections */
bool isLive (const std::string& socketPath) {
    const int fd = connectTo (socketPath);
    if (fd < 0) {
	return false;
    }

    close (fd);
    return true;
}

/** hyprland.lock: the compositor's pid, then its Wayland socket name */
std::string lockedDisplay (const std::filesystem::path& instance) {
    std::ifstream lock (instance / "hyprland.lock");
    std::string pid;
    std::string display;
    std::getline (lock, pid);
    std::getline (lock, display);
    return display;
}
bool onHyprland (const char* currentDesktop) {
    if (currentDesktop == nullptr) {
	return false;
    }

    std::string lowered (currentDesktop);
    std::ranges::transform (lowered, lowered.begin (), [] (unsigned char c) { return std::tolower (c); });
    return lowered.find ("hyprland") != std::string::npos;
}
}

std::string WallpaperEngine::Desktop::findHyprlandSocket (
    const std::string& runtimeDir, const char* signature, const char* waylandDisplay, const char* currentDesktop
) {
    if (runtimeDir.empty ()) {
	return {};
    }

    const std::filesystem::path instances = std::filesystem::path (runtimeDir) / "hypr";
    std::error_code error;

    if (signature != nullptr && *signature != '\0') {
	if (const auto socket = instances / signature / ".socket.sock"; std::filesystem::exists (socket, error)) {
	    return socket.string ();
	}
    }

    // WAYLAND_DISPLAY can be a path, the lock has the bare name
    const std::string display
	= waylandDisplay != nullptr ? std::filesystem::path (waylandDisplay).filename ().string () : std::string {};
    std::string newest;
    std::filesystem::file_time_type newestTime {};

    for (const auto& entry : std::filesystem::directory_iterator (instances, error)) {
	const auto socket = entry.path () / ".socket.sock";
	if (!isLive (socket.string ())) {
	    continue;
	}

	if (!display.empty () && lockedDisplay (entry.path ()) == display) {
	    return socket.string ();
	}

	const auto time = std::filesystem::last_write_time (entry.path () / "hyprland.lock", error);
	if (newest.empty () || (!error && time > newestTime)) {
	    newest = socket.string ();
	    newestTime = error ? newestTime : time;
	}
    }

    return onHyprland (currentDesktop) ? newest : std::string {};
}

std::optional<std::string>
WallpaperEngine::Desktop::hyprlandRequest (const std::string& socketPath, const std::string& request) {
    const int fd = connectTo (socketPath);
    if (fd < 0) {
	return std::nullopt;
    }

    if (send (fd, request.data (), request.size (), MSG_NOSIGNAL) < 0) {
	close (fd);
	return std::nullopt;
    }
    shutdown (fd, SHUT_WR);

    std::string response;
    char buffer[256];
    ssize_t readBytes = 0;
    while ((readBytes = recv (fd, buffer, sizeof (buffer), 0)) > 0) {
	response.append (buffer, static_cast<std::size_t> (readBytes));
    }
    close (fd);

    return response;
}
