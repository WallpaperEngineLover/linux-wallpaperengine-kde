#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include "WallpaperEngine/Desktop/KWinScript.h"

using WallpaperEngine::Desktop::KWinScript;
using namespace std::chrono_literals;

namespace {
struct Directory {
    std::filesystem::path path;

    Directory () {
	path = std::filesystem::temp_directory_path () / ("lwe-kwin-test-" + std::to_string (getpid ()));
	std::filesystem::create_directories (path);
    }

    ~Directory () { std::filesystem::remove_all (path); }

    std::filesystem::path make (const std::string& name, std::chrono::seconds age = 60s) const {
	const auto file = path / name;

	std::ofstream (file) << "// script\n";
	std::filesystem::last_write_time (file, std::filesystem::file_time_type::clock::now () - age);

	return file;
    }
};

// a pid nothing runs under: engines' file names end in one, and the sweep spares a script whose pid is alive
std::string deadPid (int offset = 0) {
    for (int pid = 4000000 - offset; pid > 1000; pid--) {
	if (kill (pid, 0) != 0 && errno == ESRCH) {
	    return std::to_string (pid);
	}
    }

    FAIL ("no free pid");
    return {};
}

bool contains (const std::vector<std::string>& names, const std::string& name) {
    return std::ranges::find (names, name) != names.end ();
}
} // namespace

TEST_CASE ("A script file nobody holds belongs to an engine that is gone") {
    const Directory dir;

    const auto pid = deadPid ();

    dir.make ("lwe-cursor-input-" + pid + ".js");
    dir.make ("lwe-window-states-" + pid + ".js");

    const auto stale = KWinScript::findStale (dir.path);

    CHECK (stale.size () == 2);
    CHECK (contains (stale, "lwe-cursor-input-" + pid));
    CHECK (contains (stale, "lwe-window-states-" + pid));
}

TEST_CASE ("A script file a running engine holds is left alone") {
    const Directory dir;
    const auto pid = deadPid ();
    const auto held = dir.make ("lwe-cursor-input-" + pid + ".js");

    // what KWinScript does for as long as the engine lives
    const int lock = open (held.c_str (), O_RDONLY);
    REQUIRE (lock >= 0);
    REQUIRE (flock (lock, LOCK_SH) == 0);

    CHECK_FALSE (contains (KWinScript::findStale (dir.path), "lwe-cursor-input-" + pid));

    // the engine ends, however it does
    close (lock);

    CHECK (contains (KWinScript::findStale (dir.path), "lwe-cursor-input-" + pid));
}

TEST_CASE ("A file an engine is still starting up with is not taken for stale") {
    const Directory dir;

    const auto pid = deadPid ();

    dir.make ("lwe-cursor-input-" + pid + ".js", 0s);

    CHECK (KWinScript::findStale (dir.path, 2s).empty ());
    CHECK (contains (KWinScript::findStale (dir.path, 0s), "lwe-cursor-input-" + pid));
}

TEST_CASE ("The script of an older engine that still runs is left alone") {
    const Directory dir;

    // no lock to be seen, the pid is what says it lives (this process stands in for it)
    dir.make ("lwe-cursor-input-" + std::to_string (getpid ()) + ".js");

    CHECK (KWinScript::findStale (dir.path).empty ());

    // a Flatpak's tag is not a pid and falls back on the lock alone
    dir.make ("lwe-window-states-abc123_" + std::to_string (getpid ()) + ".js");

    CHECK (contains (KWinScript::findStale (dir.path), "lwe-window-states-abc123_" + std::to_string (getpid ())));
}

TEST_CASE ("Only the engine's own script files count") {
    const Directory dir;

    dir.make ("other-script.js");
    dir.make ("lwe-cursor-input-9.txt");
    std::filesystem::create_directories (dir.path / "lwe-folder.js");

    CHECK (KWinScript::findStale (dir.path).empty ());
    CHECK (KWinScript::findStale (dir.path / "missing").empty ());
}
