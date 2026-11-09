#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "WallpaperEngine/Render/Shaders/ShaderCache.h"

using namespace WallpaperEngine::Render::Shaders;

namespace {
// set before main, the singleton reads it when the first test (of any file) touches the cache
const std::optional<std::string> previousCacheHome = [] () -> std::optional<std::string> {
    const char* value = std::getenv ("XDG_CACHE_HOME");

    return value != nullptr ? std::optional<std::string> (value) : std::nullopt;
}();

const std::filesystem::path root = [] {
    auto path = std::filesystem::temp_directory_path () / ("lwe-shadercache-" + std::to_string (getpid ()));
    std::filesystem::remove_all (path);
    std::filesystem::create_directories (path);
    setenv ("XDG_CACHE_HOME", path.c_str (), 1);
    return path;
}();

struct Cleanup {
    ~Cleanup () {
	std::error_code error;
	std::filesystem::remove_all (root, error);

	if (previousCacheHome.has_value ()) {
	    setenv ("XDG_CACHE_HOME", previousCacheHome->c_str (), 1);
	} else {
	    unsetenv ("XDG_CACHE_HOME");
	}
    }
} cleanup;

std::filesystem::path cacheRoot () { return root; }

std::filesystem::path shaders () { return cacheRoot () / "linux-wallpaperengine" / "shaders"; }

void makeSparse (const std::filesystem::path& path, uintmax_t size, std::chrono::hours age) {
    std::filesystem::create_directories (path.parent_path ());
    std::ofstream (path).put ('x');
    std::filesystem::resize_file (path, size);
    std::filesystem::last_write_time (path, std::filesystem::file_time_type::clock::now () - age);
}
}

TEST_CASE ("ShaderCache keeps translations and text between runs", "[shadercache]") {
    REQUIRE (!cacheRoot ().empty ());
    auto& cache = ShaderCache::get ();
    cache.setEnabled (true);

    const GLSLContext::Sources sources { "vertex source", "fragment source", "" };

    REQUIRE_FALSE (cache.loadTranslation ("round-trip").has_value ());

    cache.storeTranslation ("round-trip", sources);
    const auto loaded = cache.loadTranslation ("round-trip");

    REQUIRE (loaded.has_value ());
    CHECK (loaded->vertex == "vertex source");
    CHECK (loaded->fragment == "fragment source");
    CHECK (loaded->geometry.empty ());
    CHECK_FALSE (cache.loadTranslation ("another key").has_value ());

    REQUIRE_FALSE (cache.loadText ("compat", "text key").has_value ());
    cache.storeText ("compat", "text key", "some text");
    CHECK (cache.loadText ("compat", "text key") == std::optional<std::string> ("some text"));
    CHECK_FALSE (cache.loadText ("other kind", "text key").has_value ());
}

TEST_CASE ("ShaderCache ignores entries it cannot trust", "[shadercache]") {
    REQUIRE (!cacheRoot ().empty ());
    auto& cache = ShaderCache::get ();
    cache.setEnabled (true);

    cache.storeText ("compat", "damaged", "payload");
    std::filesystem::path stored;

    for (const auto& file : std::filesystem::recursive_directory_iterator (shaders () / "compat")) {
	if (file.is_regular_file () && std::filesystem::file_size (file) == 16 + 7) {
	    stored = file.path ();
	}
    }

    REQUIRE_FALSE (stored.empty ());

    SECTION ("a wrong magic") {
	std::fstream file (stored, std::ios::in | std::ios::out | std::ios::binary);
	file.put ('X');
	file.close ();
	CHECK_FALSE (cache.loadText ("compat", "damaged").has_value ());
    }

    SECTION ("a different key hash") {
	std::fstream file (stored, std::ios::in | std::ios::out | std::ios::binary);
	file.seekp (9);
	file.put ('\x7f');
	file.close ();
	CHECK_FALSE (cache.loadText ("compat", "damaged").has_value ());
    }

    SECTION ("a truncated file") {
	std::filesystem::resize_file (stored, 10);
	CHECK_FALSE (cache.loadText ("compat", "damaged").has_value ());
    }

    SECTION ("a translation cut short") {
	cache.storeText ("translations", "short", std::string ("\x05\0\0\0abc", 7));
	CHECK_FALSE (cache.loadTranslation ("short").has_value ());
	cache.storeText ("translations", "trailing", std::string ("\0\0\0\0\0\0\0\0\0\0\0\0zz", 14));
	CHECK_FALSE (cache.loadTranslation ("trailing").has_value ());
    }
}

TEST_CASE ("ShaderCache does nothing while disabled", "[shadercache]") {
    REQUIRE (!cacheRoot ().empty ());
    auto& cache = ShaderCache::get ();

    cache.setEnabled (true);
    cache.storeText ("compat", "kept", "value");
    cache.setEnabled (false);

    CHECK_FALSE (cache.loadText ("compat", "kept").has_value ());
    cache.storeText ("compat", "ignored", "value");
    cache.storeTranslation ("ignored", {});
    CHECK_FALSE (cache.loadTranslation ("ignored").has_value ());
    CHECK_FALSE (cache.programsSupported ());

    cache.setEnabled (true);
    CHECK (cache.loadText ("compat", "kept") == std::optional<std::string> ("value"));
    CHECK_FALSE (cache.loadText ("compat", "ignored").has_value ());
}

TEST_CASE ("ShaderCache drops the oldest files when the directory outgrows its limit", "[shadercache]") {
    REQUIRE (!cacheRoot ().empty ());
    auto& cache = ShaderCache::get ();
    cache.setEnabled (true);

    constexpr uintmax_t mib = 1024 * 1024;
    const auto old = shaders () / "programs" / "old-entry";
    const auto newer = shaders () / "programs" / "newer-entry";
    makeSparse (old, 150 * mib, std::chrono::hours (48));
    makeSparse (newer, 150 * mib, std::chrono::hours (1));

    struct stat info {};

    if (stat (old.c_str (), &info) != 0 || static_cast<uintmax_t> (info.st_blocks) * 512 > 10 * mib) {
	SKIP ("the filesystem under the temp dir has no sparse files");
    }

    // the first write of a run prunes, a payload past 1/16 of the limit prunes again
    cache.storeText ("compat", "trigger", "x");
    cache.storeText ("compat", "big", std::string (17 * mib, 'b'));

    CHECK_FALSE (std::filesystem::exists (old));
    CHECK (std::filesystem::exists (newer));
}
