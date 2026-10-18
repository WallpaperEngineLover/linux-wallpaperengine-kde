#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sys/file.h>
#include <sys/mman.h>
#include <unistd.h>

#include "WallpaperEngine/Desktop/SessionBus.h"
#include "WallpaperEngine/WebBrowser/IPC/WebHostSharedMemory.h"

using namespace WallpaperEngine::WebBrowser::IPC;

namespace {
std::filesystem::path profile (const std::string& name) {
    return std::filesystem::temp_directory_path () / ("lwe-cef-test" + std::to_string (getpid ()) + "-" + name);
}
}

TEST_CASE ("Process tag outside a Flatpak is the pid") {
    if (access ("/.flatpak-info", F_OK) == 0) {
	SKIP ("running inside a Flatpak");
    }

    CHECK (WallpaperEngine::Desktop::processTag () == std::to_string (getpid ()));
}

TEST_CASE ("Stale web host files are told apart by their locks, not by pids") {
    std::string held;
    auto* memory = createSharedMemory (held, 16, 16);
    REQUIRE (memory != nullptr);

    // what a killed engine of another pid namespace leaves: a name whose pid means nothing here, no lock
    const std::string orphan = "/lwe-web-" + std::to_string (getpid ()) + "_1-0";
    const int orphanFd = shm_open (orphan.c_str (), O_CREAT | O_EXCL | O_RDWR, 0600);
    REQUIRE (orphanFd >= 0);
    close (orphanFd);

    const auto dead = profile ("dead");
    const auto live = profile ("live");
    const auto fresh = profile ("fresh");
    for (const auto& dir : { dead, live, fresh }) {
	std::filesystem::create_directories (dir);
    }
    std::ofstream (dead / CEF_PROFILE_LOCK).close ();
    std::filesystem::last_write_time (dead, std::filesystem::file_time_type::clock::now () - std::chrono::hours (1));
    const int liveLock = open ((live / CEF_PROFILE_LOCK).c_str (), O_CREAT | O_RDWR, 0600);
    REQUIRE (flock (liveLock, LOCK_SH) == 0);

    CHECK (isLockedByAnyone ("/dev/shm" + held));
    CHECK_FALSE (isLockedByAnyone ("/dev/shm" + orphan));

    removeStaleWebHostFiles ();

    CHECK (std::filesystem::exists ("/dev/shm" + held));
    CHECK_FALSE (std::filesystem::exists ("/dev/shm" + orphan));
    CHECK_FALSE (std::filesystem::exists (dead));
    CHECK (std::filesystem::exists (live));
    // no lock file yet: the host is still setting it up
    CHECK (std::filesystem::exists (fresh));

    closeSharedMemory (memory, held, 16, 16, true);
    close (liveLock);
    CHECK_FALSE (std::filesystem::exists ("/dev/shm" + held));
    std::filesystem::remove_all (live);
    std::filesystem::remove_all (fresh);
}
