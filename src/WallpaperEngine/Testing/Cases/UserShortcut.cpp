#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <sys/stat.h>
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

struct ScopedEnv {
    ScopedEnv (const char* key, const std::string& value) : name (key) {
	if (const char* old = std::getenv (key); old != nullptr) {
	    previous = old;
	}

	setenv (key, value.c_str (), 1);
    }

    ~ScopedEnv () {
	if (previous.has_value ()) {
	    setenv (name, previous->c_str (), 1);
	} else {
	    unsetenv (name);
	}
    }

    const char* name;
    std::optional<std::string> previous;
};

void script (const std::filesystem::path& path, const std::string& body) {
    std::ofstream (path) << "#!/bin/sh\n" << body << "\n";
    chmod (path.c_str (), 0755);
}

std::string slurp (const std::filesystem::path& file) {
    std::ifstream stream (file);
    std::stringstream content;

    content << stream.rdbuf ();
    return content.str ();
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
}

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

    const auto shortcut
	= UserShortcut::parse (R"({"commandtype":"command","file":"touch","arguments":")" + marker.string () + R"("})");

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

TEST_CASE ("The Exec line's field codes are filled in and the working folder is honoured") {
    const auto dir = scratchDir ();
    const auto record = dir / "record.sh";

    script (
	record,
	"printf '%s\\n' \"$@\" > \"" + (dir / "args").string () + "\"\npwd >> \"" + (dir / "args").string () + "\""
    );
    std::filesystem::create_directories (dir / "work");
    std::ofstream (
	dir / "full.desktop"
    ) << "[Desktop Entry]\nType=Application\nName=Full App\nName[de]=Voll\nIcon=myicon\nPath="
      << (dir / "work").string () << "\nExec=" << record.string ()
      << " %i %c %k 100%% %f %U \"two words\" \"say \\\"hi\\\"\"\n";

    const auto shortcut = UserShortcut::parse ((dir / "full.desktop").string ());

    REQUIRE (shortcut.has_value ());
    CHECK (shortcut->launch ());
    REQUIRE (waitForFile (dir / "args"));
    std::this_thread::sleep_for (std::chrono::milliseconds (100));

    CHECK (
	slurp (dir / "args")
	== "--icon\nmyicon\nFull App\n" + (dir / "full.desktop").string () + "\n100%\ntwo words\nsay \"hi\"\n"
	    + (dir / "work").string () + "\n"
    );

    std::filesystem::remove_all (dir);
}

TEST_CASE ("A desktop entry that wants a terminal runs through the terminal program") {
    const auto dir = scratchDir ();
    const auto terminal = dir / "fake-terminal";

    script (terminal, "printf '%s\\n' \"$@\" > \"" + (dir / "terminal-args").string () + "\"");
    std::ofstream (
	dir / "term.desktop"
    ) << "[Desktop Entry]\nType=Application\nName=Term\nTerminal=true\nExec=htop --tree\n";

    const ScopedEnv program ("TERMINAL", terminal.string ());
    const auto shortcut = UserShortcut::parse ((dir / "term.desktop").string ());

    REQUIRE (shortcut.has_value ());
    CHECK (shortcut->launch ());
    REQUIRE (waitForFile (dir / "terminal-args"));
    std::this_thread::sleep_for (std::chrono::milliseconds (100));
    CHECK (slurp (dir / "terminal-args") == "-e\nhtop\n--tree\n");

    std::filesystem::remove_all (dir);
}

TEST_CASE ("Applications are found by id in the data folders") {
    const auto dir = scratchDir ();

    std::filesystem::create_directories (dir / "data/applications");
    std::ofstream (dir / "data/applications/found.desktop") << "[Desktop Entry]\nType=Application\nName=Found\n";
    std::ofstream (dir / "data/applications/noexec.desktop") << "[Desktop Entry]\nType=Application\nName=Nothing\n";

    const ScopedEnv data ("XDG_DATA_DIRS", (dir / "data").string () + "::");
    const ScopedEnv home ("XDG_DATA_HOME", (dir / "empty").string ());

    const auto found = UserShortcut::parse ("found");

    REQUIRE (found.has_value ());
    CHECK (found->type == UserShortcut::Type::Application);
    CHECK (UserShortcut::parse ("found.desktop")->type == UserShortcut::Type::Application);
    CHECK (UserShortcut::parse ("lwe-not-installed")->type == UserShortcut::Type::File);
    CHECK (
	UserShortcut::parse (R"({"commandtype":"application","file":"found"})")->type == UserShortcut::Type::Application
    );

    CHECK_FALSE (UserShortcut::parse ("noexec")->launch ());
    CHECK_FALSE (UserShortcut::parse (R"({"commandtype":"application","file":"lwe-not-installed"})")->launch ());
    CHECK_FALSE (UserShortcut::parse ("/nowhere/at-all.desktop")->launch ());

    std::filesystem::remove_all (dir);
}

TEST_CASE ("Web addresses, folders and documents are handed to xdg-open") {
    const auto dir = scratchDir ();
    const auto bin = dir / "bin";

    std::filesystem::create_directories (bin);
    script (bin / "xdg-open", "echo \"$1\" >> \"" + (dir / "opened").string () + "\"");

    const ScopedEnv path ("PATH", bin.string () + ":" + (std::getenv ("PATH") != nullptr ? std::getenv ("PATH") : ""));

    std::ofstream (dir / "notes.txt") << "text";

    CHECK (UserShortcut::parse ("https://example.org/a")->launch ());
    CHECK (UserShortcut::parse (R"({"commandtype":"web","file":"example.org"})")->launch ());
    CHECK (UserShortcut::parse (R"({"commandtype":"directory","file":")" + dir.string () + R"("})")->launch ());
    CHECK (UserShortcut::parse ((dir / "notes.txt").string ())->launch ());
    CHECK (UserShortcut::parse ("www.example.org")->type == UserShortcut::Type::Web);

    for (int i = 0; i < 100 && std::ranges::count (slurp (dir / "opened"), '\n') < 4; i++) {
	std::this_thread::sleep_for (std::chrono::milliseconds (20));
    }

    const auto opened = slurp (dir / "opened");

    CHECK (opened.find ("https://example.org/a\n") != std::string::npos);
    CHECK (opened.find ("https://example.org\n") != std::string::npos);
    CHECK (opened.find (dir.string () + "\n") != std::string::npos);
    CHECK (opened.find ((dir / "notes.txt").string () + "\n") != std::string::npos);

    std::filesystem::remove_all (dir);
}

TEST_CASE ("An executable file is run with its arguments from its own folder") {
    const auto dir = scratchDir ();
    const auto program = dir / "tool.sh";

    script (
	program,
	"printf '%s\\n' \"$@\" > \"" + (dir / "tool-out").string () + "\"\npwd >> \"" + (dir / "tool-out").string ()
	    + "\""
    );

    const auto shortcut = UserShortcut::parse (
	R"({"commandtype":"file","file":")" + program.string () + R"(","arguments":"one \"two words\""})"
    );

    REQUIRE (shortcut.has_value ());
    CHECK (shortcut->launch ());
    REQUIRE (waitForFile (dir / "tool-out"));
    std::this_thread::sleep_for (std::chrono::milliseconds (100));
    CHECK (slurp (dir / "tool-out") == "one\ntwo words\n" + dir.string () + "\n");

    CHECK (UserShortcut::parse (R"({"commandtype":"command","file":"true"})")->launch ());

    std::filesystem::remove_all (dir);
}
