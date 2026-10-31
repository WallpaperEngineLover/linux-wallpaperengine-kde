#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "WallpaperEngine/Desktop/IconTheme.h"

using WallpaperEngine::Desktop::IconTheme;

namespace {
namespace fs = std::filesystem;

void touch (const fs::path& path, const std::string& content = "x") {
    fs::create_directories (path.parent_path ());
    std::ofstream (path) << content;
}

// the singleton reads its environment once, so this test case is the only one that may ask for it
struct Tree {
    Tree () : root (fs::temp_directory_path () / ("lwe-icon-theme-test-" + std::to_string (getpid ()))) {
	for (const char* name : names) {
	    const char* value = std::getenv (name);

	    saved.emplace_back (name, value != nullptr ? std::optional<std::string> (value) : std::nullopt);
	}

	fs::remove_all (root);

	const auto icons = root / "data/icons";

	touch (root / "config/kdeglobals", "[General]\nTheme=wrong\n[Icons]\nTheme=mine\n[Other]\nTheme=wrong\n");
	touch (icons / "mine/index.theme", "[Icon Theme]\nName=Mine\nInherits=parent,,mine,hicolor\n");
	touch (icons / "parent/index.theme", "[Icon Theme]\nInherits=mine\n");

	touch (icons / "mine/48x48/apps/sized.png");
	touch (icons / "mine/scalable/apps/sized.svg");
	touch (icons / "mine/16x16/apps/bigger.png");
	touch (icons / "mine/64x64/apps/bigger.png");
	touch (icons / "mine/32x32/apps/vector.png");
	touch (icons / "mine/32x32/apps/vector.svg");
	touch (icons / "mine/scalable/apps/mono-symbolic.svg");
	touch (icons / "mine/22x22/apps/mono-symbolic.png");
	touch (icons / "mine/22x22/apps/notes.txt");
	touch (icons / "mine/22x22/apps/shared.png");
	touch (icons / "parent/32x32/apps/inherited.png");
	touch (icons / "parent/32x32/apps/shared.png");
	touch (icons / "hicolor/16x16/apps/fallback.png");
	touch (icons / "hicolor/16x16/apps/shared.png");
	touch (root / "other/icons/mine/128x128/apps/elsewhere.png");

	touch (root / "file.png");

	setenv ("HOME", (root / "home").c_str (), 1);
	setenv ("XDG_CONFIG_HOME", (root / "config").c_str (), 1);
	setenv ("XDG_DATA_HOME", (root / "data").c_str (), 1);
	setenv ("XDG_DATA_DIRS", (":" + (root / "other").string () + "::").c_str (), 1);
    }

    ~Tree () {
	for (const auto& [name, value] : saved) {
	    if (value.has_value ()) {
		setenv (name, value->c_str (), 1);
	    } else {
		unsetenv (name);
	    }
	}

	fs::remove_all (root);
    }

    static constexpr const char* names[] = { "HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_DATA_DIRS" };

    fs::path root;
    std::vector<std::pair<const char*, std::optional<std::string>>> saved;
};
}

TEST_CASE ("Icons are looked up in the user's theme, its parents and hicolor") {
    const Tree tree;
    auto& theme = IconTheme::get ();
    const auto icons = tree.root / "data/icons";

    SECTION ("scalable beats every fixed size and svg beats png") {
	CHECK (theme.find ("sized") == icons / "mine/scalable/apps/sized.svg");
	CHECK (theme.find ("vector") == icons / "mine/32x32/apps/vector.svg");
    }

    SECTION ("the bigger size wins") { CHECK (theme.find ("bigger") == icons / "mine/64x64/apps/bigger.png"); }

    SECTION ("symbolic icons lose to nothing but are still found") {
	CHECK (theme.find ("mono-symbolic") == icons / "mine/scalable/apps/mono-symbolic.svg");
    }

    SECTION ("parents and hicolor are searched after the theme, in every data folder") {
	CHECK (theme.find ("inherited") == icons / "parent/32x32/apps/inherited.png");
	CHECK (theme.find ("fallback") == icons / "hicolor/16x16/apps/fallback.png");
	CHECK (theme.find ("shared") == icons / "mine/22x22/apps/shared.png");
	CHECK (theme.find ("elsewhere") == tree.root / "other/icons/mine/128x128/apps/elsewhere.png");
    }

    SECTION ("only png and svg files count") {
	CHECK_FALSE (theme.find ("notes").has_value ());
	CHECK_FALSE (theme.find ("lwe-no-such-icon").has_value ());
	CHECK_FALSE (theme.find ("").has_value ());
    }

    SECTION ("an absolute path is taken as it is when the file exists") {
	CHECK (theme.find ((tree.root / "file.png").string ()) == tree.root / "file.png");
	CHECK_FALSE (theme.find ((tree.root / "missing.png").string ()).has_value ());
    }
}
