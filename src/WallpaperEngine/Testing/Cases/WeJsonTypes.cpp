#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include "WallpaperEngine/Assets/AssetLocator.h"
#include "WallpaperEngine/Data/JSON.h"
#include "WallpaperEngine/Data/Model/Material.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/MaterialParser.h"
#include "WallpaperEngine/Data/Parsers/ProjectParser.h"
#include "WallpaperEngine/FileSystem/Container.h"

using namespace WallpaperEngine;
using Data::JSON::JSON;
using Data::Model::CullingMode;
using Data::Model::DepthtestMode;
using Data::Model::DepthwriteMode;
using Data::Model::Project;
using Data::Parsers::MaterialParser;
using Data::Parsers::ProjectParser;

namespace {
Data::Model::MaterialPassUniquePtr passWith (const std::string& members) {
    static const Project project {};

    return MaterialParser::parsePass (JSON::parse (R"({ "shader": "genericimage2")" + members + "}"), project);
}
} // namespace

// sub_1401587D0 and siblings (the property tables behind blending, alphawriting, depthtest, depthwrite and cullmode)
TEST_CASE ("A render state that is there but not usable takes the first entry of WE's table") {
    // missing: the material keeps its own default, later code tells it apart
    const auto missing = passWith ("");

    CHECK (missing->cullmode == CullingMode::CullingMode_Unknown);
    CHECK (missing->depthtest == DepthtestMode::DepthtestMode_Unknown);
    CHECK (missing->depthwrite == DepthwriteMode::DepthwriteMode_Unknown);

    for (const std::string value : { "7", "null", "[]", "{}", "true", "1.5", R"("")", R"("back")", R"("ENABLED")" }) {
	INFO (value);
	const auto pass
	    = passWith (R"(, "cullmode": )" + value + R"(, "depthtest": )" + value + R"(, "depthwrite": )" + value);

	// cullmode {normal, nocull}, depthtest and depthwrite {disabled, enabled}
	CHECK (pass->cullmode == CullingMode::CullingMode_Normal);
	CHECK (pass->depthtest == DepthtestMode::DepthtestMode_Disabled);
	CHECK (pass->depthwrite == DepthwriteMode::DepthwriteMode_Disabled);
    }

    const auto named = passWith (R"(, "cullmode": "nocull", "depthtest": "enabled", "depthwrite": "enabled")");

    CHECK (named->cullmode == CullingMode::CullingMode_Disable);
    CHECK (named->depthtest == DepthtestMode::DepthtestMode_Enabled);
    CHECK (named->depthwrite == DepthwriteMode::DepthwriteMode_Enabled);

    const auto plain = passWith (R"(, "cullmode": "normal", "depthtest": "disabled", "depthwrite": "disabled")");

    CHECK (plain->cullmode == CullingMode::CullingMode_Normal);
    CHECK (plain->depthtest == DepthtestMode::DepthtestMode_Disabled);
    CHECK (plain->depthwrite == DepthwriteMode::DepthwriteMode_Disabled);
}

// sub_140154480: a material pass's combos go through asInt with no type test
TEST_CASE ("Pass combos read like asInt, a bool is 0 or 1") {
    const auto combos = MaterialParser::parseCombos (
	JSON::parse (R"({ "A": true, "B": false, "C": null, "D": 2.7, "E": 3, "F": "x", "G": [], "H": -2 })")
    );

    CHECK (combos.at ("A") == 1);
    CHECK (combos.at ("B") == 0);
    CHECK (combos.at ("C") == 0);
    CHECK (combos.at ("D") == 2);
    CHECK (combos.at ("E") == 3);
    CHECK (combos.at ("F") == 0);
    CHECK (combos.at ("G") == 0);
    CHECK (combos.at ("H") == -2);
    CHECK (MaterialParser::parseCombos (JSON::parse ("7")).empty ());
}

TEST_CASE ("A shader that isn't text becomes WE's error shader") {
    CHECK (passWith ("")->shader == "genericimage2");

    static const Project project {};

    CHECK (MaterialParser::parsePass (JSON::parse (R"({ "shader": 7 })"), project)->shader == "error");
    CHECK (MaterialParser::parsePass (JSON::parse (R"({ "shader": null })"), project)->shader == "error");
    CHECK (MaterialParser::parsePass (JSON::parse (R"({})"), project)->shader == "error");
}

// sub_14011D7D0 / sub_14011E520: the type comes from the main file's extension and the "type" key is not read
TEST_CASE ("The project type comes from the main file like in WE") {
    CHECK (ProjectParser::typeOfFile ("scene.json") == Project::Type_Scene);
    CHECK (ProjectParser::typeOfFile ("scene.pkg") == Project::Type_Scene);
    CHECK (ProjectParser::typeOfFile ("anim.GIF") == Project::Type_Scene);
    CHECK (ProjectParser::typeOfFile ("index.html") == Project::Type_Web);

    for (const std::string video : { "a.mp4", "a.wmv", "a.avi", "a.m4v", "a.mov", "a.webm", "a.mkv", "A.MP4" }) {
	INFO (video);
	CHECK (ProjectParser::typeOfFile (video) == Project::Type_Video);
    }

    // WE has rules for .exe (application) and the image formats too, this engine has no such project type
    CHECK_FALSE (ProjectParser::typeOfFile ("app.exe").has_value ());
    CHECK_FALSE (ProjectParser::typeOfFile ("picture.png").has_value ());
    CHECK_FALSE (ProjectParser::typeOfFile ("noextension").has_value ());
}

namespace {
Project::Type typeOf (const std::string& projectJson) {
    auto container = std::make_unique<FileSystem::Container> ();
    container->getVFS ().add ("scene.json", R"({ "camera": { "center": "0 0 -1", "eye": "0 0 0", "up": "0 1 0" },
	                   "general": { "orthogonalprojection": { "width": 1920, "height": 1080 } }, "objects": [] })");

    return ProjectParser::parse (
	       JSON::parse (projectJson), std::make_unique<Assets::AssetLocator> (std::move (container))
    )
	->type;
}
} // namespace

TEST_CASE ("A project loads whatever its type key says when the file tells the type") {
    CHECK (typeOf (R"({ "type": "scene", "file": "scene.json" })") == Project::Type_Scene);
    CHECK (typeOf (R"({ "type": "Scene", "file": "scene.json" })") == Project::Type_Scene);
    // no type, a number, null, an object, a type the file contradicts: the file decides
    CHECK (typeOf (R"({ "file": "scene.json" })") == Project::Type_Scene);
    CHECK (typeOf (R"({ "type": 7, "file": "scene.json" })") == Project::Type_Scene);
    CHECK (typeOf (R"({ "type": null, "file": "scene.json" })") == Project::Type_Scene);
    CHECK (typeOf (R"({ "type": {}, "file": "scene.json" })") == Project::Type_Scene);
    CHECK (typeOf (R"({ "type": "video", "file": "scene.json" })") == Project::Type_Scene);

    // a file without a rule still needs the declared type
    CHECK_THROWS (typeOf (R"({ "file": "scene.dat" })"));
    CHECK_THROWS (typeOf (R"({ "type": 7, "file": "scene.dat" })"));
    CHECK_THROWS (typeOf (R"({ "type": "application", "file": "scene.dat" })"));

    // the main file has to be text
    CHECK_THROWS (typeOf (R"({ "type": "scene", "file": 7 })"));
    CHECK_THROWS (typeOf (R"({ "type": "scene" })"));
}
