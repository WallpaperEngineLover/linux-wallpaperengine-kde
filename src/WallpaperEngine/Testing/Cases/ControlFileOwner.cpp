#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "WallpaperEngine/Application/ControlFileOwner.h"

using WallpaperEngine::Application::belongsToAnotherUser;

TEST_CASE ("A control file of another user is recognised") {
    const auto directory = std::filesystem::temp_directory_path () / ("lwe-owner-test-" + std::to_string (getpid ()));
    std::filesystem::create_directories (directory);
    const auto mine = directory / "mine";
    std::ofstream (mine) << "path=/x\n";

    CHECK_FALSE (belongsToAnotherUser (mine, geteuid ()));
    CHECK (belongsToAnotherUser (mine, geteuid () + 1));
    CHECK_FALSE (belongsToAnotherUser (directory / "missing", geteuid ()));

    std::filesystem::remove_all (directory);
}

TEST_CASE ("Files that really belong to someone else are refused") {
    if (geteuid () == 0) {
	SKIP ("root owns /etc/passwd");
    }

    CHECK (belongsToAnotherUser ("/etc/passwd", geteuid ()));
}

TEST_CASE ("A link counts by its own owner, not by its target's") {
    if (geteuid () == 0) {
	SKIP ("root owns /etc/passwd");
    }

    const auto directory = std::filesystem::temp_directory_path () / ("lwe-owner-link-" + std::to_string (getpid ()));
    std::filesystem::create_directories (directory);
    const auto link = directory / "link";
    std::filesystem::create_symlink ("/etc/passwd", link);

    CHECK_FALSE (belongsToAnotherUser (link, geteuid ()));

    std::filesystem::remove_all (directory);
}
