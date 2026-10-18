#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "WallpaperEngine/FileSystem/Adapters/Directory.h"

using WallpaperEngine::FileSystem::Adapters::DirectoryAdapter;

TEST_CASE ("A directory adapter does not reach into a sibling that shares its name as a prefix") {
    const auto root = std::filesystem::canonical (std::filesystem::temp_directory_path ())
	/ ("lwe-dir-test-" + std::to_string (getpid ()));
    std::filesystem::create_directories (root / "pack/sub");
    std::filesystem::create_directories (root / "pack-secret");
    std::ofstream (root / "pack/sub/inside.txt") << "inside";
    std::ofstream (root / "pack-secret/outside.txt") << "outside";

    const DirectoryAdapter adapter (root / "pack");

    CHECK (adapter.exists ("sub/inside.txt"));
    CHECK_FALSE (adapter.exists ("../pack-secret/outside.txt"));
    CHECK_FALSE (adapter.exists ((root / "pack-secret/outside.txt").string ()));
    CHECK_THROWS (adapter.open ("../pack-secret/outside.txt"));
    CHECK_THROWS (adapter.physicalPath ("../pack-secret/outside.txt"));
    CHECK_NOTHROW (adapter.physicalPath ("sub/inside.txt"));

    std::filesystem::remove_all (root);
}
