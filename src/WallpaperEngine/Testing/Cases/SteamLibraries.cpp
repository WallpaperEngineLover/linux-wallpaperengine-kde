#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "Steam/FileSystem/FileSystem.h"

namespace {
struct FakeHome {
    std::filesystem::path path
	= std::filesystem::temp_directory_path () / ("lwe-steam-test-" + std::to_string (getpid ()));
    std::string previous;
    bool hadHome = false;

    FakeHome () {
	std::filesystem::create_directories (path);

	if (const char* home = getenv ("HOME"); home != nullptr) {
	    previous = home;
	    hadHome = true;
	}

	setenv ("HOME", path.c_str (), 1);
    }

    ~FakeHome () {
	if (hadHome) {
	    setenv ("HOME", previous.c_str (), 1);
	} else {
	    unsetenv ("HOME");
	}

	std::filesystem::remove_all (path);
    }
};
} // namespace

TEST_CASE ("Steam lookups cover the library folders Steam lists") {
    FakeHome home;
    const auto steam = home.path / ".local/share/Steam";
    const auto extra = home.path / "disk2/SteamLibrary";

    std::filesystem::create_directories (steam / "steamapps");
    std::filesystem::create_directories (extra / "steamapps/workshop/content/431960/12345");
    std::filesystem::create_directories (extra / "steamapps/common/wallpaper_engine/assets");

    CHECK_THROWS (Steam::FileSystem::workshopDirectory (431960, "12345"));
    CHECK_THROWS (Steam::FileSystem::appDirectory ("wallpaper_engine", "assets"));

    std::ofstream (steam / "steamapps/libraryfolders.vdf")
	<< "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" << steam.string ()
	<< "\"\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"" << extra.string () << "\"\n\t\t\"label\"\t\t\"\"\n\t}\n}\n";

    CHECK (
	std::filesystem::equivalent (
	    Steam::FileSystem::workshopDirectory (431960, "12345"), extra / "steamapps/workshop/content/431960/12345"
	)
    );
    CHECK (
	std::filesystem::equivalent (
	    Steam::FileSystem::appDirectory ("wallpaper_engine", "assets"),
	    extra / "steamapps/common/wallpaper_engine/assets"
	)
    );
}

TEST_CASE ("The main Steam install is searched before the extra libraries") {
    FakeHome home;
    const auto steam = home.path / ".local/share/Steam";
    const auto extra = home.path / "disk2/SteamLibrary";

    std::filesystem::create_directories (steam / "steamapps/workshop/content/431960/777");
    std::filesystem::create_directories (extra / "steamapps/workshop/content/431960/777");
    std::ofstream (steam / "steamapps/libraryfolders.vdf")
	<< "\"libraryfolders\"\n{\n\"1\"\n{\n\"path\" \"" << extra.string () << "\"\n}\n}\n";

    CHECK (
	std::filesystem::equivalent (
	    Steam::FileSystem::workshopDirectory (431960, "777"), steam / "steamapps/workshop/content/431960/777"
	)
    );
}
