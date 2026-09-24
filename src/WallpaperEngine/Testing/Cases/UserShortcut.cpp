#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>

#include "WallpaperEngine/Desktop/UserShortcut.h"

using WallpaperEngine::Desktop::UserShortcut;

namespace {
std::filesystem::path scratchDir () {
    auto dir = std::filesystem::temp_directory_path () / ("lwe-shortcut-test-" + std::to_string (getpid ()));
    std::filesystem::create_directories (dir);
    return dir;
}

bool waitForFile (const std::filesystem::path& file) {
    for (int i = 0; i < 100; i++) {
	if (std::filesystem::exists (file)) {
	    return true;
	}
	std::this_thread::sleep_for (std::chrono::milliseconds (20));
    }
    return false;
}
} // namespace

TEST_CASE ("User shortcuts parse the real engine's JSON") {
    const auto web = UserShortcut::parse (R"({"isbound":true,"commandtype":"web","file":"https://example.org"})");
    REQUIRE (web.has_value ());
    CHECK (web->type == UserShortcut::Type::Web);
    CHECK (web->target == "https://example.org");

    const auto command = UserShortcut::parse (R"({"commandtype":"command","file":"echo","arguments":"hi"})");
    REQUIRE (command.has_value ());
    CHECK (command->type == UserShortcut::Type::Command);
    CHECK (command->arguments == "hi");

    CHECK_FALSE (UserShortcut::parse (R"({"isbound":false,"commandtype":"web","file":"https://example.org"})"));
    CHECK_FALSE (UserShortcut::parse (R"({"commandtype":"unknown","file":"x"})"));
    CHECK_FALSE (UserShortcut::parse (""));
    CHECK_FALSE (UserShortcut::parse ("{not json"));
}

TEST_CASE ("Plain shortcut values pick their type") {
    const auto dir = scratchDir ();

    CHECK (UserShortcut::parse ("https://example.org")->type == UserShortcut::Type::Web);
    CHECK (UserShortcut::parse (dir.string ())->type == UserShortcut::Type::Directory);
    CHECK (UserShortcut::parse ((dir / "notes.txt").string ())->type == UserShortcut::Type::File);
    CHECK (UserShortcut::parse ((dir / "app.desktop").string ())->type == UserShortcut::Type::Application);

    // a command is never guessed from a plain string
    CHECK (UserShortcut::parse ("rm -rf something")->type == UserShortcut::Type::File);

    std::filesystem::remove_all (dir);
}

TEST_CASE ("Command shortcuts run through the shell") {
    const auto dir = scratchDir ();
    const auto marker = dir / "ran";

    const auto shortcut = UserShortcut::parse (
	R"({"commandtype":"command","file":"touch","arguments":")" + marker.string () + R"("})"
    );

    REQUIRE (shortcut.has_value ());
    CHECK (shortcut->launch ());
    CHECK (waitForFile (marker));

    std::filesystem::remove_all (dir);
}

TEST_CASE ("Application shortcuts expand the desktop file's Exec line") {
    const auto dir = scratchDir ();
    const auto marker = dir / "app ran";
    const auto desktop = dir / "test.desktop";

    std::ofstream (desktop) << "[Desktop Entry]\nType=Application\nName=Test\nExec=touch %U \"" << marker.string ()
			    << "\"\n";

    const auto shortcut = UserShortcut::parse (desktop.string ());

    REQUIRE (shortcut.has_value ());
    CHECK (shortcut->launch ());
    CHECK (waitForFile (marker));

    std::filesystem::remove_all (dir);
}

TEST_CASE ("A program that doesn't exist is reported") {
    const auto dir = scratchDir ();
    const auto desktop = dir / "missing.desktop";

    std::ofstream (desktop) << "[Desktop Entry]\nType=Application\nName=Missing\nExec=lwe-no-such-program-here\n";

    CHECK_FALSE (UserShortcut::parse (desktop.string ())->launch ());

    std::filesystem::remove_all (dir);
}
