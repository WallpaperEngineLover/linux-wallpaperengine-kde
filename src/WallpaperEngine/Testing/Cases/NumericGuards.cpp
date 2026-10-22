#include <catch2/catch_test_macros.hpp>

#include <climits>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

#include "WallpaperEngine/Assets/AssetLocator.h"
#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/WallpaperParser.h"
#include "WallpaperEngine/Data/Utils/FiniteFloat.h"
#include "WallpaperEngine/FileSystem/Container.h"

using namespace WallpaperEngine;
using namespace WallpaperEngine::Data::Utils;

TEST_CASE ("Typed numbers are finite") {
    CHECK (parseFiniteFloat ("1.5") == 1.5f);
    CHECK (parseFiniteFloat ("-0.25") == -0.25f);
    CHECK (parseFiniteFloat ("2e3") == 2000.0f);

    // each of these parses with stof and then passes every range check, because comparisons with NaN are false
    for (const std::string text : { "nan", "NaN", "-nan", "inf", "-inf", "infinity" }) {
	INFO (text);
	CHECK_THROWS_AS (parseFiniteFloat (text), std::invalid_argument);
    }

    CHECK_THROWS_AS (parseFiniteFloat ("zoom"), std::invalid_argument);
    CHECK_THROWS_AS (parseFiniteFloat (""), std::invalid_argument);
    CHECK_THROWS_AS (parseFiniteFloat ("1e999"), std::out_of_range);
}

namespace {
Data::Model::WallpaperUniquePtr sceneWith (const std::string& projection) {
    auto container = std::make_unique<FileSystem::Container> ();
    container->getVFS ().add (
	"scene.json",
	R"({ "camera": { "center": "0 0 -1", "eye": "0 0 0", "up": "0 1 0" },
	     "general": { "orthogonalprojection": )"
	    + projection + R"( },
	     "objects": [] })"
    );

    Data::Model::Project project {};
    project.type = Data::Model::Project::Type_Scene;
    project.assetLocator = std::make_unique<Assets::AssetLocator> (std::move (container));

    return Data::Parsers::WallpaperParser::parse (Data::JSON::JSON ("scene.json"), project);
}
}

TEST_CASE ("A scene is orthographic only with a usable projection, like in WE") {
    const auto sized = sceneWith (R"({ "width": 1920, "height": 1080 })");

    CHECK (sized->as<Data::Model::Scene> ()->camera.projection.width == 1920);
    CHECK (sized->as<Data::Model::Scene> ()->camera.projection.height == 1080);
    CHECK_FALSE (sized->as<Data::Model::Scene> ()->camera.projection.isPerspective);

    // sized from its content: no numbers needed
    const auto automatic = sceneWith (R"({ "auto": true })");
    CHECK (automatic->as<Data::Model::Scene> ()->camera.projection.isAuto);
    CHECK_FALSE (automatic->as<Data::Model::Scene> ()->camera.projection.isPerspective);

    // sub_140186C90 sets the orthographic flag for nothing else: a canvas of no size, a size that isn't a pair of
    // numbers, "auto" that isn't a real true, and a member that isn't an object all leave a perspective scene that
    // loads
    for (const std::string projection :
	 { R"({ "width": 0, "height": 1080 })", R"({ "width": 1920, "height": 0 })",
	   R"({ "width": 0.4, "height": 1080 })", R"({ "width": "1920", "height": 1080 })",
	   R"({ "width": 1920, "height": null })", R"({ "width": 1920 })", R"({ "width": true, "height": true })",
	   R"({ "auto": 1 })", R"({ "auto": "true", "width": "x" })", R"({})", "null", "7", R"("1920x1080")", "[]",
	   "[1920, 1080]", "true" }) {
	INFO (projection);
	const auto scene = sceneWith (projection);

	CHECK (scene->as<Data::Model::Scene> ()->camera.projection.isPerspective);
	CHECK (scene->as<Data::Model::Scene> ()->camera.projection.width == 0);
	CHECK (scene->as<Data::Model::Scene> ()->camera.projection.height == 0);
    }

    // a negative size is the one case WE draws nonsense from and we have nothing to match
    CHECK_THROWS (sceneWith (R"({ "width": -1920, "height": 1080 })"));
    CHECK_THROWS (sceneWith (R"({ "width": 1920, "height": -1 })"));
}

TEST_CASE ("A float that no int holds converts like the real engine's, not as undefined behavior") {
    using Data::Model::DynamicValue;
    DynamicValue value (0.0f);

    value.update (3.9f, DynamicValue::UpdateSource::User);
    CHECK (value.getInt () == 3);
    value.update (-3.9f, DynamicValue::UpdateSource::User);
    CHECK (value.getInt () == -3);

    // cvttss2si answers INT_MIN for all of these
    for (const float huge : { 1e30f, -1e30f, std::numeric_limits<float>::infinity (),
			      -std::numeric_limits<float>::infinity (), std::numeric_limits<float>::quiet_NaN () }) {
	INFO (huge);
	value.update (huge, DynamicValue::UpdateSource::User);
	CHECK (value.getInt () == INT_MIN);
    }

    value.update (Data::Model::Color { 1.0f, 0.0f, 0.0f, 1.0f }, DynamicValue::UpdateSource::User);
    CHECK (value.getInt () == 255);
    value.update (Data::Model::Color { 1e30f, 0.0f, 0.0f, 1.0f }, DynamicValue::UpdateSource::User);
    CHECK (value.getInt () == INT_MIN);
}
