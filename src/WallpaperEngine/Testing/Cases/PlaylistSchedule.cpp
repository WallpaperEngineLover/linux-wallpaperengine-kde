#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include "WallpaperEngine/Application/PlaylistSchedule.h"

using namespace WallpaperEngine::Application;

TEST_CASE ("Daytime items without an end share the day up to the next one that has one") {
    // 4 items, the second one ends at noon: the first two split the morning, the others the afternoon
    const auto ends = daytimeEnds ({ 0.0f, 0.5f, 0.0f, 0.0f });

    REQUIRE (ends.size () == 4);
    CHECK (ends[0] == 0.25f);
    CHECK (ends[1] == 0.5f);
    CHECK (ends[2] == 0.75f);
    CHECK (ends[3] == 1.0f);
}

TEST_CASE ("Daytime ends of items that name none are spread over the whole day") {
    const auto ends = daytimeEnds ({ 0.0f, 0.0f, 0.0f, 0.0f });

    CHECK (ends == std::vector<float> { 0.25f, 0.5f, 0.75f, 1.0f });
}

TEST_CASE ("The last daytime item always runs to midnight") {
    const auto ends = daytimeEnds ({ 0.3f, 0.6f, 0.4f });

    REQUIRE (ends.size () == 3);
    CHECK (ends[0] == 0.3f);
    CHECK (ends[1] == 0.6f);
    CHECK (ends[2] == 1.0f);
    CHECK (daytimeEnds ({}).empty ());
    CHECK (daytimeEnds ({ 0.2f }) == std::vector<float> { 1.0f });
}

TEST_CASE ("A daytime end that was asked for is kept exactly") {
    for (int items = 2; items <= 7; items++) {
	for (int hour = 1; hour < 24; hour++) {
	    for (int minute = 0; minute < 60; minute += 5) {
		std::vector<float> requested (static_cast<std::size_t> (items) + 1, 0.0f);
		const float end = daytimePosition (hour, minute);

		requested[static_cast<std::size_t> (items) - 1] = end;

		const auto ends = daytimeEnds (requested);

		CHECK (ends[static_cast<std::size_t> (items) - 1] == end);
		CHECK (scheduledIndex (ends, end) == static_cast<std::size_t> (items));
	    }
	}
    }
}

TEST_CASE ("The item shown is the first one whose end is past the clock") {
    const auto ends = daytimeEnds ({ 0.25f, 0.5f, 0.0f });

    CHECK (scheduledIndex (ends, daytimePosition (0, 0)) == 0);
    CHECK (scheduledIndex (ends, daytimePosition (5, 59)) == 0);
    CHECK (scheduledIndex (ends, daytimePosition (6, 0)) == 1);
    CHECK (scheduledIndex (ends, daytimePosition (12, 0)) == 2);
    CHECK (scheduledIndex (ends, daytimePosition (23, 59)) == 2);
    CHECK (scheduledIndex ({}, 0.5f) == 0);
    CHECK (scheduledIndex ({ 0.1f, 0.2f }, 5.0f) == 1);
}

TEST_CASE ("Seven days are shared out among the items like WE does") {
    CHECK (dayOfWeekEnds (1) == std::vector<float> { 7.0f });
    CHECK (dayOfWeekEnds (2) == std::vector<float> { 3.0f, 7.0f });
    CHECK (dayOfWeekEnds (3) == std::vector<float> { 2.0f, 4.0f, 7.0f });
    CHECK (dayOfWeekEnds (4) == std::vector<float> { 1.0f, 3.0f, 5.0f, 7.0f });
    CHECK (dayOfWeekEnds (7) == std::vector<float> { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f });
    CHECK (dayOfWeekEnds (0).empty ());
}

TEST_CASE ("Items past the seventh never come up on a day of week playlist") {
    const auto ends = dayOfWeekEnds (9);

    REQUIRE (ends.size () == 9);
    CHECK (ends[6] == 7.0f);
    CHECK (ends[7] > 7.0f);
    CHECK (ends[8] > 7.0f);

    for (int day = 0; day < 7; day++) {
	CHECK (scheduledIndex (ends, static_cast<float> (day)) == static_cast<std::size_t> (day));
    }
}

TEST_CASE ("Days of the week count from the first day of the locale") {
    // tm_wday: 0 Sunday, 1 Monday, 6 Saturday
    CHECK (dayOfWeekPosition (1, 1) == 0.0f);
    CHECK (dayOfWeekPosition (0, 1) == 6.0f);
    CHECK (dayOfWeekPosition (0, 0) == 0.0f);
    CHECK (dayOfWeekPosition (6, 0) == 6.0f);
    CHECK (dayOfWeekPosition (6, 6) == 0.0f);
    CHECK (dayOfWeekPosition (0, 6) == 1.0f);
}

TEST_CASE ("The weekday of the locale is a day number") {
    const int first = localeFirstWeekday ();

    CHECK (first >= 0);
    CHECK (first <= 6);
}

TEST_CASE ("A logon playlist shows the next item in every new session and the same one within a session") {
    const auto dir = std::filesystem::temp_directory_path () / ("lwe-logon-test-" + std::to_string (getpid ()));
    const auto file = dir / "state" / "playlists.json";

    std::filesystem::remove_all (dir);

    CHECK (logonIndex (file, "mine", 3, "boot1:2") == 0);
    CHECK (logonIndex (file, "mine", 3, "boot1:2") == 0);
    CHECK (logonIndex (file, "mine", 3, "boot1:3") == 1);
    CHECK (logonIndex (file, "mine", 3, "boot1:3") == 1);
    CHECK (logonIndex (file, "other", 2, "boot1:3") == 0);
    CHECK (logonIndex (file, "mine", 3, "boot2:1") == 2);
    CHECK (logonIndex (file, "mine", 3, "boot3:1") == 0);

    // fewer items than before wrap around instead of running off the end
    CHECK (logonIndex (file, "mine", 2, "boot4:1") == 1);

    std::filesystem::remove_all (dir);
}

TEST_CASE ("The session token follows the mount of the runtime dir, not its inode") {
    const auto shmMount = runtimeMountId ("/dev/shm");

    if (shmMount.empty () || shmMount == runtimeMountId ("/")) {
	SKIP ("/dev/shm is not a mount of its own here");
    }

    const char* previous = std::getenv ("XDG_RUNTIME_DIR");
    const std::string saved = previous != nullptr ? previous : "";
    const std::string runtime = "/dev/shm";

    setenv ("XDG_RUNTIME_DIR", runtime.c_str (), 1);
    const auto shm = currentSessionToken ();

    setenv ("XDG_RUNTIME_DIR", "/", 1);
    const auto root = currentSessionToken ();

    if (previous != nullptr) {
	setenv ("XDG_RUNTIME_DIR", saved.c_str (), 1);
    } else {
	unsetenv ("XDG_RUNTIME_DIR");
    }

    CHECK_FALSE (shm.empty ());
    CHECK (shm != root);
}

TEST_CASE ("The login marker stays for the life of its folder") {
    const auto dir = std::filesystem::temp_directory_path () / ("lwe-login-marker-" + std::to_string (getpid ()));
    const auto other = dir / "other";

    std::filesystem::remove_all (dir);
    std::filesystem::create_directories (other);

    const auto first = loginMarker (dir);

    CHECK (first.size () == 32);
    CHECK (loginMarker (dir) == first);
    CHECK (loginMarker (other) != first);
    CHECK (loginMarker (dir / "missing").empty ());

    std::filesystem::remove_all (dir);
    std::filesystem::create_directories (dir);

    // a folder that was cleared by a logout gives the next login another id
    CHECK (loginMarker (dir) != first);

    std::filesystem::remove_all (dir);
}

TEST_CASE ("The mount of a directory comes from mountinfo") {
    const auto shm = runtimeMountId ("/dev/shm");
    const auto root = runtimeMountId ("/");

    CHECK_FALSE (root.empty ());
    CHECK (runtimeMountId ("/nonexistent-lwe-dir").empty ());

    if (shm.empty () || shm == root) {
	SKIP ("/dev/shm is not a mount of its own here");
    }

    CHECK (runtimeMountId ("/dev/shm/.") == shm);
}

TEST_CASE ("Looking up the logon item does not record it until it is committed") {
    const auto dir = std::filesystem::temp_directory_path () / ("lwe-logon-peek-" + std::to_string (getpid ()));
    const auto file = dir / "playlists.json";

    std::filesystem::remove_all (dir);

    CHECK (logonIndex (file, "mine", 3, "a", false) == 0);
    CHECK (logonIndex (file, "mine", 3, "b", false) == 0);
    CHECK (logonIndex (file, "mine", 3, "a") == 0);
    CHECK (logonIndex (file, "mine", 3, "b", false) == 1);
    CHECK (logonIndex (file, "mine", 3, "a", false) == 0);
    CHECK (logonIndex (file, "mine", 3, "b") == 1);
    CHECK (logonIndex (file, "mine", 3, "b") == 1);

    std::filesystem::remove_all (dir);
}

TEST_CASE ("A logon playlist survives a broken state file") {
    const auto dir = std::filesystem::temp_directory_path () / ("lwe-logon-broken-" + std::to_string (getpid ()));
    const auto file = dir / "playlists.json";

    std::filesystem::remove_all (dir);
    std::filesystem::create_directories (dir);
    std::ofstream (file) << "{ not json";

    CHECK (logonIndex (file, "mine", 3, "a") == 0);
    CHECK (logonIndex (file, "mine", 3, "b") == 1);
    CHECK (logonIndex (file, "mine", 0, "b") == 0);

    std::filesystem::remove_all (dir);
}

TEST_CASE ("A logon playlist ignores state entries of the wrong type") {
    const auto dir = std::filesystem::temp_directory_path () / ("lwe-logon-types-" + std::to_string (getpid ()));
    const auto file = dir / "playlists.json";

    std::filesystem::remove_all (dir);
    std::filesystem::create_directories (dir);
    std::ofstream (file) << R"({"mine":{"index":"x","session":4},"other":{"index":1,"session":["a"]},"list":[1]})";

    CHECK (logonIndex (file, "mine", 3, "a") == 0);
    CHECK (logonIndex (file, "mine", 3, "a") == 0);
    CHECK (logonIndex (file, "other", 3, "a") == 2);
    CHECK (logonIndex (file, "list", 3, "a") == 0);

    std::filesystem::remove_all (dir);
}
