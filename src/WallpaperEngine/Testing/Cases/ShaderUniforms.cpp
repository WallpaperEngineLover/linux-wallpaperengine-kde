#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <memory>
#include <string>

#include "WallpaperEngine/Assets/AssetLocator.h"
#include "WallpaperEngine/Data/Model/UserSetting.h"
#include "WallpaperEngine/FileSystem/Container.h"
#include "WallpaperEngine/Render/Shaders/ShaderUnit.h"

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render::Shaders;
using namespace WallpaperEngine::Data::Model;

namespace {
struct Unit {
    Assets::AssetLocator locator { std::make_unique<FileSystem::Container> () };
    ShaderConstantMap constants;
    TextureMap passTextures, overrideTextures;
    ComboMap combos, overrideCombos;
    std::unique_ptr<ShaderUnit> unit;

    Unit (const std::vector<std::string>& materials, const std::string& source) {
	for (const auto& name : materials) {
	    constants.emplace (name, std::make_unique<UserSetting> ());
	}

	unit = std::make_unique<ShaderUnit> (
	    GLSLContext::UnitType_Vertex, "test.vert", source, locator, constants, passTextures, overrideTextures,
	    combos, overrideCombos
	);
    }

    [[nodiscard]] const Variables::ShaderVariable& parameter (const std::string& name) const {
	for (const auto* candidate : unit->getParameters ()) {
	    if (candidate->getName () == name) {
		return *candidate;
	    }
	}

	FAIL ("no parameter " << name);
	std::abort ();
    }
};
}

TEST_CASE ("A uniform that names a material constant needs no default") {
    // the shape of shadowcasterfoliage4.vert: the material provides the value
    const Unit unit (
	{ "speed", "color", "size", "tint", "steps" },
	"uniform float g_Speed; // {\"material\":\"speed\"}\n"
	"uniform vec3 g_Color; // {\"material\":\"color\"}\n"
	"uniform vec2 g_Size; // {\"material\":\"size\"}\n"
	"uniform vec4 g_Tint; // {\"material\":\"tint\"}\n"
	"uniform int g_Steps; // {\"material\":\"steps\"}\n"
	"void main () { }\n"
    );

    REQUIRE (unit.unit->getParameters ().size () == 5);
    CHECK_THAT (unit.parameter ("g_Speed").getFloat (), Catch::Matchers::WithinULP (0.0f, 0));
    CHECK (unit.parameter ("g_Color").getVec3 () == glm::vec3 (0.0f));
    CHECK (unit.parameter ("g_Size").getVec2 () == glm::vec2 (0.0f));
    CHECK (unit.parameter ("g_Tint").getVec4 () == glm::vec4 (0.0f));
    CHECK (unit.parameter ("g_Steps").getInt () == 0);
}

TEST_CASE ("A default written as text keeps its fraction") {
    const Unit unit (
	{ "mix", "count", "plain" },
	"uniform float g_Mix; // {\"material\":\"mix\",\"default\":\"0.1\"}\n"
	"uniform int g_Count; // {\"material\":\"count\",\"default\":\"3\"}\n"
	"uniform float g_Plain; // {\"material\":\"plain\",\"default\":0.25}\n"
	"void main () { }\n"
    );

    CHECK_THAT (unit.parameter ("g_Mix").getFloat (), Catch::Matchers::WithinULP (0.1f, 0));
    CHECK (unit.parameter ("g_Count").getInt () == 3);
    CHECK_THAT (unit.parameter ("g_Plain").getFloat (), Catch::Matchers::WithinULP (0.25f, 0));
}

TEST_CASE ("A uniform with neither a default nor a constant to take the value from is still refused") {
    CHECK_THROWS (Unit ({}, "uniform float g_Lost; // {\"material\":\"lost\"}\nvoid main () { }\n"));
}

TEST_CASE ("A uniform declaration padded with spaces or tabs keeps its type and name") {
    // dqss2 writes "uniform vec3  u_sunColor;", the type used to come out empty
    const Unit unit (
	{ "sun", "ray", "tint" },
	"uniform vec3  u_sunColor; // {\"material\":\"sun\",\"default\":\"1.0 0.95 0.85\"}\n"
	"uniform\tvec2\tu_ray;\t// {\"material\":\"ray\",\"default\":\"0.5 2\"}\n"
	"uniform vec4 u_tint ; // {\"material\":\"tint\",\"default\":\"0 0 1 1\"}\n"
	"void main () { }\n"
    );

    REQUIRE (unit.unit->getParameters ().size () == 3);
    CHECK (unit.parameter ("u_sunColor").getVec3 () == glm::vec3 (1.0f, 0.95f, 0.85f));
    CHECK (unit.parameter ("u_ray").getVec2 () == glm::vec2 (0.5f, 2.0f));
    CHECK (unit.parameter ("u_tint").getVec4 () == glm::vec4 (0.0f, 0.0f, 1.0f, 1.0f));
}

TEST_CASE ("A combo line that is not valid JSON is skipped and the others still count") {
    // procedural_noise: {Color":0 lacks its opening quote
    const Unit unit (
	{},
	"// [COMBO] {\"material\":\"Category\",\"combo\":\"CATEGORY\",\"default\":1,\"options\":{Color\":0,\"UV\":1}}\n"
	"// [COMBO] {\"material\":\"Mode\",\"combo\":\"MODE\",\"default\":2}\n"
	"void main () { }\n"
    );

    CHECK_FALSE (unit.unit->getDiscoveredCombos ().contains ("CATEGORY"));
    REQUIRE (unit.unit->getDiscoveredCombos ().contains ("MODE"));
    CHECK (unit.unit->getDiscoveredCombos ().at ("MODE") == 2);
}
