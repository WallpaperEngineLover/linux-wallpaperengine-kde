#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

#include "WallpaperEngine/Desktop/HyprlandIPC.h"

using WallpaperEngine::Desktop::findHyprlandSocket;
using WallpaperEngine::Desktop::hyprlandRequest;

namespace {
struct RuntimeDir {
    std::filesystem::path path
	= std::filesystem::temp_directory_path () / ("lwe-hypr-test-" + std::to_string (getpid ()));
    std::vector<int> listeners;

    RuntimeDir () { std::filesystem::create_directories (path / "hypr"); }

    ~RuntimeDir () {
	for (const int fd : listeners) {
	    close (fd);
	}
	std::filesystem::remove_all (path);
    }

    /** a Hyprland instance folder like 0.4x+ writes it; a dead one leaves its socket file behind */
    std::string instance (const std::string& signature, const std::string& display, bool live) {
	const auto dir = path / "hypr" / signature;
	std::filesystem::create_directories (dir);
	std::ofstream (dir / "hyprland.lock") << "1234\n" << display << "\n";

	const auto socketPath = (dir / ".socket.sock").string ();
	const int fd = socket (AF_UNIX, SOCK_STREAM, 0);
	sockaddr_un addr {};
	addr.sun_family = AF_UNIX;
	std::strncpy (addr.sun_path, socketPath.c_str (), sizeof (addr.sun_path) - 1);
	REQUIRE (bind (fd, reinterpret_cast<sockaddr*> (&addr), sizeof (addr)) == 0);
	REQUIRE (listen (fd, 4) == 0);

	if (live) {
	    listeners.push_back (fd);
	} else {
	    close (fd);
	}

	return socketPath;
    }
};
} // namespace

TEST_CASE ("Hyprland socket from the instance signature") {
    RuntimeDir runtime;
    const auto first = runtime.instance ("first", "wayland-1", true);
    const auto second = runtime.instance ("second", "wayland-2", true);

    CHECK (findHyprlandSocket (runtime.path.string (), "second", "wayland-1", "Hyprland") == second);
    CHECK (findHyprlandSocket (runtime.path.string (), "first", nullptr, "Hyprland") == first);
}

TEST_CASE ("Hyprland socket without the signature, as in a Flatpak") {
    RuntimeDir runtime;

    SECTION ("no instance folder") {
	CHECK (findHyprlandSocket (runtime.path.string (), nullptr, "wayland-1", "Hyprland").empty ());
	CHECK (findHyprlandSocket ("", nullptr, "wayland-1", "Hyprland").empty ());
	CHECK (findHyprlandSocket ((runtime.path / "missing").string (), nullptr, nullptr, "Hyprland").empty ());
    }

    SECTION ("the only live instance") {
	runtime.instance ("crashed", "wayland-1", false);
	const auto live = runtime.instance ("live", "wayland-2", true);
	CHECK (findHyprlandSocket (runtime.path.string (), nullptr, "wayland-1", "Hyprland") == live);
	CHECK (findHyprlandSocket (runtime.path.string (), nullptr, nullptr, "Hyprland") == live);
    }

    SECTION ("the instance whose lock names our display, also given as a path") {
	runtime.instance ("a", "wayland-1", true);
	const auto ours = runtime.instance ("b", "wayland-2", true);
	runtime.instance ("c", "wayland-3", true);
	CHECK (findHyprlandSocket (runtime.path.string (), nullptr, "wayland-2", "Hyprland") == ours);
	CHECK (findHyprlandSocket (runtime.path.string (), nullptr, "/run/user/1000/wayland-2", "Hyprland") == ours);
	CHECK (findHyprlandSocket (runtime.path.string (), "", "wayland-2", "Hyprland") == ours);
    }

    SECTION ("a signature of a gone instance") {
	const auto live = runtime.instance ("live", "wayland-1", true);
	CHECK (findHyprlandSocket (runtime.path.string (), "gone", "wayland-1", "Hyprland") == live);
    }

    SECTION ("another session's instance when we don't run on Hyprland") {
	runtime.instance ("nested", "wayland-1", true);
	CHECK (findHyprlandSocket (runtime.path.string (), nullptr, "wayland-0", "sway").empty ());
	CHECK (findHyprlandSocket (runtime.path.string (), nullptr, "wayland-0", nullptr).empty ());
	CHECK (findHyprlandSocket (runtime.path.string (), nullptr, "wayland-0", "KDE:Hyprland") != "");
    }

    SECTION ("only dead sockets") {
	runtime.instance ("crashed", "wayland-1", false);
	CHECK (findHyprlandSocket (runtime.path.string (), nullptr, "wayland-1", "Hyprland").empty ());
    }
}

TEST_CASE ("Hyprland request and reply") {
    RuntimeDir runtime;
    const auto socketPath = runtime.instance ("sig", "wayland-1", true);
    const int listener = runtime.listeners.back ();

    std::string received;
    std::thread server ([listener, &received] () {
	const int client = accept (listener, nullptr, nullptr);
	char buffer[64];
	ssize_t length = 0;
	while ((length = read (client, buffer, sizeof (buffer))) > 0) {
	    received.append (buffer, static_cast<std::size_t> (length));
	}
	const std::string reply = R"({"x": 1520, "y": -12.5})";
	write (client, reply.data (), reply.size ());
	close (client);
    });

    const auto reply = hyprlandRequest (socketPath, "j/cursorpos");
    server.join ();

    CHECK (received == "j/cursorpos");
    REQUIRE (reply.has_value ());
    CHECK (*reply == R"({"x": 1520, "y": -12.5})");
    CHECK_FALSE (hyprlandRequest ((runtime.path / "nothing.sock").string (), "j/cursorpos").has_value ());
}
