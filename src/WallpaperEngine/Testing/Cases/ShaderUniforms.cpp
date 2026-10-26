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
