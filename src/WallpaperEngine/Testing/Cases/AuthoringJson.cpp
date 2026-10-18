#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Assets/AssetLocator.h"
#include "WallpaperEngine/Data/JSON.h"
#include "WallpaperEngine/Data/Model/Material.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/MaterialParser.h"
#include "WallpaperEngine/FileSystem/Container.h"

using WallpaperEngine::Data::JSON::JSON;

TEST_CASE ("Asset JSON takes comments and trailing commas and leaves strings alone") {
    const auto value = JSON::parseAsset (R"({
	// a line comment
	"url": "https://example.test/a//b/*c*/",
	"escaped": "quote \" // still text",
	"slashes": "\\/* not a comment */",
	"commas": "[1,] {\"a\":1,}",
	/* a block
	   comment */
	"items": [1, /* between */ 2,],
	"nested": { "a": [ { "b": 1, }, ], },
    })");

    CHECK (value["url"] == "https://example.test/a//b/*c*/");
    CHECK (value["escaped"] == "quote \" // still text");
    CHECK (value["slashes"] == "\\/* not a comment */");
    CHECK (value["commas"] == "[1,] {\"a\":1,}");
    REQUIRE (value["items"].size () == 2);
    CHECK (value["items"][1] == 2);
    CHECK (value["nested"]["a"][0]["b"] == 1);
}

TEST_CASE ("A trailing comma followed by a comment is still dropped") {
    const auto value = JSON::parseAsset ("{\n  \"a\": 1, // the last one\n  \"b\": [2, /* end */ ],\n  /* x */\n}");

    CHECK (value["a"] == 1);
    CHECK (value["b"].size () == 1);
}

TEST_CASE ("Broken asset JSON still fails with the position") {
    try {
	(void)JSON::parseAsset ("{\n  \"items\": [1,,]\n}");
	FAIL ("a doubled comma parsed");
    } catch (const nlohmann::json::parse_error& e) {
	CHECK (std::string (e.what ()).find ("line 2") != std::string::npos);
    }

    CHECK_THROWS (JSON::parseAsset ("{\n  /* never closed"));
    CHECK_THROWS (JSON::parseAsset ("{\"value\": 1e10000}"));
    CHECK_THROWS (JSON::parseAsset ("{\"unterminated\": \"text}"));
}

namespace {
WallpaperEngine::Data::Model::Project projectWith (const std::string& file, const std::string& content) {
    auto container = std::make_unique<WallpaperEngine::FileSystem::Container> ();
    container->getVFS ().add (file, content);

    WallpaperEngine::Data::Model::Project project {};
    project.assetLocator = std::make_unique<WallpaperEngine::Assets::AssetLocator> (std::move (container));
    return project;
}
} // namespace

TEST_CASE ("Materials load from asset JSON, a broken one becomes WE's error pass") {
    using WallpaperEngine::Data::Parsers::MaterialParser;

    const auto project = projectWith ("materials/test.json", R"({
	"passes": [ { "shader": "genericimage2", "combos": { "TEST": 2, }, }, ],
    })");
    const auto material = MaterialParser::load (project, "materials/test.json");

    REQUIRE (material->passes.size () == 1);
    CHECK (material->passes[0]->shader == "genericimage2");
    CHECK (material->passes[0]->combos.at ("TEST") == 2);

    // sub_1401515B0: an unparsable material becomes one "error" shader pass
    const auto broken = projectWith ("materials/broken.json", "{\n  \"passes\": [1,,]\n}");
    const auto fallback = MaterialParser::load (broken, "materials/broken.json");

    REQUIRE (fallback->passes.size () == 1);
    CHECK (fallback->passes[0]->shader == "error");
}

TEST_CASE ("A value of the wrong type counts as missing instead of ending the process") {
    const auto value = JSON::parse (
	R"({ "flag": 1, "name": 7, "count": "12", "ratio": "x", "nested": { "a": 1 }, "ok": true, "five": 5 })"
    );

    CHECK_FALSE (value.optional ("flag", false));
    CHECK (value.optional ("flag", true));
    CHECK (value.optional ("name", std::string ("fallback")) == "fallback");
    CHECK (value.optional ("count", 3) == 3);
    CHECK (value.optional ("ratio", 0.5f) == 0.5f);
    CHECK (value.optional ("nested", std::string ("n")) == "n");
    CHECK_FALSE (value.optional<bool> ("flag").has_value ());
    CHECK_FALSE (value.optional<std::string> ("name").has_value ());

    // what does convert still does
    CHECK (value.optional ("ok", false));
    CHECK (value.optional ("five", 0.0f) == 5.0f);
    CHECK (value.optional<int> ("five").value () == 5);
    CHECK (value.optional ("missing", 9) == 9);
}
