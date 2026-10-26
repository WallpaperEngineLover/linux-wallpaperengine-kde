#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <unistd.h>

#include "WallpaperEngine/Scripting/LocalStorage.h"

using WallpaperEngine::Scripting::LocalStorage;

namespace {
std::filesystem::path storageDir (const std::string& name) {
    auto dir = std::filesystem::temp_directory_path () / ("lwe-storage-test-" + std::to_string (getpid ())) / name;
    std::filesystem::remove_all (dir);
    std::filesystem::create_directories (dir);
    LocalStorage::setDirectory (dir);
    return dir;
}

std::string readBytes (const std::filesystem::path& file) {
    std::ifstream in (file, std::ios::binary);
    return { std::istreambuf_iterator<char> (in), std::istreambuf_iterator<char> () };
}
}

TEST_CASE ("localStorage keys are lower case sha1 hex") {
    CHECK (LocalStorage::sha1Hex ("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK (LocalStorage::sha1Hex ("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    // 56 bytes, the padding spills into a second block
    CHECK (
	LocalStorage::sha1Hex ("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
	== "84983e441c3bd26ebaae4aa1f95129e5e54670f1"
    );
    CHECK (LocalStorage::sha1Hex (std::string (1000, 'a')) == "291e9a6c66994949b57ba5e650361e98fc36b1ba");
}

TEST_CASE ("localStorage files use WE's LSBK0001 layout") {
    const auto dir = storageDir ("layout");
    const std::string key = LocalStorage::sha1Hex ("origin");

    {
	LocalStorage::Store store ("layout");
	REQUIRE (store.set (key, "LSKV0001{\"x\":1}"));
    }

    const std::string bytes = readBytes (dir / "layout.bin");
    REQUIRE (bytes.size () == 8 + 4 + 40 + 4 + 15);
    CHECK (bytes.substr (0, 8) == "LSBK0001");
    CHECK (bytes.substr (8, 4) == std::string ("\x01\0\0\0", 4));
    CHECK (bytes.substr (12, 40) == key);
    CHECK (bytes.substr (52, 4) == std::string ("\x0f\0\0\0", 4));
    CHECK (bytes.substr (56) == "LSKV0001{\"x\":1}");

    const LocalStorage::Store reopened ("layout");
    CHECK (reopened.get (key) == "LSKV0001{\"x\":1}");
}

TEST_CASE ("localStorage stores hold at most 100000 bytes") {
    storageDir ("cap");
    LocalStorage::Store store ("cap");

    CHECK (store.set ("a", std::string (60000, 'x')));
    CHECK_FALSE (store.set ("b", std::string (50000, 'x')));
    // replacing a key doesn't count its old value
    CHECK (store.set ("a", std::string (100000, 'x')));
    CHECK_FALSE (store.set ("a", std::string (100001, 'x')));
    CHECK (store.get ("a")->size () == 100000);
}

TEST_CASE ("localStorage flushes merge what other engines wrote") {
    const auto dir = storageDir ("merge");
    LocalStorage::Store first ("shared");
    LocalStorage::Store second ("shared");

    first.set ("one", "1");
    second.set ("two", "2");
    first.flush ();
    second.flush ();

    auto entries = LocalStorage::readFile (dir / "shared.bin");
    REQUIRE (entries.size () == 2);

    first.remove ("one");
    first.flush ();
    entries = LocalStorage::readFile (dir / "shared.bin");
    REQUIRE (entries.size () == 1);
    CHECK (entries[0].first == "two");

    // nothing changed, nothing written
    std::filesystem::remove (dir / "shared.bin");
    first.flush ();
    CHECK_FALSE (std::filesystem::exists (dir / "shared.bin"));
}

TEST_CASE ("localStorage clear empties the file") {
    const auto dir = storageDir ("clear");

    {
	LocalStorage::Store store ("clear");
	store.set ("one", "1");
    }

    {
	LocalStorage::Store store ("clear");
	REQUIRE (store.get ("one").has_value ());
	store.clear ();
	CHECK_FALSE (store.remove ("one"));
    }

    CHECK (LocalStorage::readFile (dir / "clear.bin").empty ());
    const auto firstOpen = LocalStorage::open ("clear");
    const auto secondOpen = LocalStorage::open (std::string ("cle") + "ar");
    CHECK (firstOpen == secondOpen);
}
