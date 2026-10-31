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

namespace {
struct TempTree {
    std::filesystem::path root = std::filesystem::canonical (std::filesystem::temp_directory_path ())
	/ ("lwe-dir-tree-" + std::to_string (getpid ()));

    TempTree () { std::filesystem::create_directories (root); }
    ~TempTree () { std::filesystem::remove_all (root); }

    void file (const std::string& name, const std::string& text = "x") const {
	std::filesystem::create_directories ((root / name).parent_path ());
	std::ofstream (root / name) << text;
    }
};
}

TEST_CASE ("A directory adapter finds files whatever their case, lists files and refuses directories") {
    TempTree tree;
    tree.file ("Materials/Util/Flat.JSON", "{}");
    tree.file ("Materials/other.txt");
    std::filesystem::create_directories (tree.root / "emptydir");

    const DirectoryAdapter adapter (tree.root);

    CHECK (adapter.exists ("materials/util/flat.json"));
    CHECK (adapter.exists ("MATERIALS/UTIL/FLAT.JSON"));
    CHECK_FALSE (adapter.exists ("materials/util/missing.json"));
    CHECK_FALSE (adapter.exists ("nothing/at/all.json"));
    CHECK_FALSE (adapter.exists ("emptydir"));
    CHECK (adapter.physicalPath ("materials/util/flat.json").filename () == "Flat.JSON");
    CHECK_THROWS (adapter.open ("emptydir"));
    CHECK_THROWS (adapter.open ("missing.txt"));
    CHECK (adapter.open ("materials/OTHER.TXT") != nullptr);
    CHECK (adapter.open ("materials/other.txt") != nullptr);

    auto names = adapter.listFiles ("materials");
    CHECK (names.size () == 1);
    CHECK (adapter.listFiles ("Materials/Util") == std::vector<std::string> { "Flat.JSON" });
    CHECK (adapter.listFiles ("emptydir").empty ());
    CHECK (adapter.listFiles ("../").empty ());
    CHECK (adapter.listFiles ("nothing").empty ());
}

TEST_CASE ("A workshop dependency alias is found only when exactly one dependency has the file") {
    TempTree tree;
    tree.file ("materials/workshop/111/thing.json");
    tree.file ("materials/workshop/222/other.json");
    tree.file ("materials/workshop/333/thing.json");
    tree.file ("plain/workshop/444/solo.tex");
    std::filesystem::create_directories (tree.root / "plain/workshop/555");
    tree.file ("plain/workshop/loosefile.tex");

    const DirectoryAdapter adapter (tree.root);

    CHECK_FALSE (adapter.resolveWorkshopDependencyAlias ("materials/thing.json").has_value ());
    CHECK (
	adapter.resolveWorkshopDependencyAlias ("materials/other.json")
	== std::filesystem::path ("materials/workshop/222/other.json")
    );
    CHECK (
	adapter.resolveWorkshopDependencyAlias ("plain/solo.tex")
	== std::filesystem::path ("plain/workshop/444/solo.tex")
    );
    CHECK_FALSE (adapter.resolveWorkshopDependencyAlias ("plain/missing.tex").has_value ());
    CHECK_FALSE (adapter.resolveWorkshopDependencyAlias ("nodir/file.tex").has_value ());
}

TEST_CASE ("The directory factory only takes directories") {
    TempTree tree;
    tree.file ("file.txt");
    const WallpaperEngine::FileSystem::Adapters::DirectoryFactory factory;

    CHECK (factory.handlesMountpoint (tree.root));
    CHECK_FALSE (factory.handlesMountpoint (tree.root / "file.txt"));
    CHECK_FALSE (factory.handlesMountpoint (tree.root / "missing"));
    CHECK (factory.create (tree.root) != nullptr);
    CHECK_THROWS (factory.create (tree.root / "file.txt"));
    CHECK_THROWS (factory.create (tree.root / "missing"));
}

TEST_CASE ("Windows 8.3 aliases name the sibling they stood for") {
    using WallpaperEngine::FileSystem::Adapters::resolveShortNameAlias;
    const std::vector<std::string> siblings = {
	"Konachan photo one.tex", "konachan photo two.tex", "KONACH.TEX", "Other long name.tex", "plain.json",
	"Konachan photo.json",
    };

    CHECK (resolveShortNameAlias (siblings, "KONACH~1.TEX") == std::optional<std::string> ("Konachan photo one.tex"));
    CHECK (resolveShortNameAlias (siblings, "KONACH~2.TEX") == std::optional<std::string> ("konachan photo two.tex"));
    CHECK_FALSE (resolveShortNameAlias (siblings, "KONACH~3.TEX").has_value ());
    CHECK_FALSE (resolveShortNameAlias (siblings, "KONACH~0.TEX").has_value ());
    CHECK_FALSE (resolveShortNameAlias (siblings, "KONACH~1.JSN").has_value ());
    CHECK_FALSE (resolveShortNameAlias (siblings, "KONACH~1.JSON").has_value ());
    CHECK_FALSE (resolveShortNameAlias (siblings, "KONACH.TEX").has_value ());
    CHECK_FALSE (resolveShortNameAlias (siblings, "~1.TEX").has_value ());
    CHECK_FALSE (resolveShortNameAlias (siblings, "TOOLONGNAME~1.TEX").has_value ());
    CHECK_FALSE (resolveShortNameAlias (siblings, "KONACH~.TEX").has_value ());
    CHECK_FALSE (resolveShortNameAlias (siblings, "KONACH~A.TEX").has_value ());
    CHECK_FALSE (resolveShortNameAlias (siblings, "KONACH~1.TEXTX").has_value ());
    CHECK_FALSE (resolveShortNameAlias ({}, "KONACH~1.TEX").has_value ());
    static_cast<void> (resolveShortNameAlias ({ "ünïcode wörds.tex" }, "_NCODE~1.TEX"));
}

TEST_CASE ("A workshop dependency alias must sit exactly one workshop segment below the wanted folder") {
    using WallpaperEngine::FileSystem::Adapters::isWorkshopDependencyAliasOf;

    CHECK (isWorkshopDependencyAliasOf ("materials/workshop/123/a.json", "materials/a.json"));
    CHECK (isWorkshopDependencyAliasOf ("workshop/9/a.json", "a.json"));
    CHECK_FALSE (isWorkshopDependencyAliasOf ("materials/workshop/a.json", "materials/a.json"));
    CHECK_FALSE (isWorkshopDependencyAliasOf ("materials/workshop/1/2/a.json", "materials/a.json"));
    CHECK_FALSE (isWorkshopDependencyAliasOf ("materials/a.json", "materials/a.json"));
    CHECK_FALSE (isWorkshopDependencyAliasOf ("other/workshop/1/a.json", "materials/a.json"));
    CHECK_FALSE (isWorkshopDependencyAliasOf ("materials/workshop/1/b.json", "materials/a.json"));
}
