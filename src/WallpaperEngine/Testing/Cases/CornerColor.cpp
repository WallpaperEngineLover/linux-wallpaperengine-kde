#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Render/CFBO.h"

using WallpaperEngine::Render::CFBO;

TEST_CASE ("parseColor accepts 6-digit hex RGB, defaulting alpha to opaque") {
    const auto color = CFBO::parseColor ("1a2b3c");

    REQUIRE (color.has_value ());
    CHECK (color->x == Catch::Approx (0x1a / 255.0f));
    CHECK (color->y == Catch::Approx (0x2b / 255.0f));
    CHECK (color->z == Catch::Approx (0x3c / 255.0f));
    CHECK (color->w == Catch::Approx (1.0f));
}

TEST_CASE ("parseColor accepts 8-digit hex RGBA") {
    const auto color = CFBO::parseColor ("1a2b3c80");

    REQUIRE (color.has_value ());
    CHECK (color->x == Catch::Approx (0x1a / 255.0f));
    CHECK (color->y == Catch::Approx (0x2b / 255.0f));
    CHECK (color->z == Catch::Approx (0x3c / 255.0f));
    CHECK (color->w == Catch::Approx (0x80 / 255.0f));
}

TEST_CASE ("parseColor strips a leading #") {
    const auto color = CFBO::parseColor ("#000000");

    REQUIRE (color.has_value ());
    CHECK (color->x == Catch::Approx (0.0f));
    CHECK (color->y == Catch::Approx (0.0f));
    CHECK (color->z == Catch::Approx (0.0f));
    CHECK (color->w == Catch::Approx (1.0f));
}

TEST_CASE ("parseColor rejects malformed input") {
    CHECK_FALSE (CFBO::parseColor ("").has_value ());
    CHECK_FALSE (CFBO::parseColor ("fff").has_value ());
    CHECK_FALSE (CFBO::parseColor ("gggggg").has_value ());
    CHECK_FALSE (CFBO::parseColor ("1234567").has_value ());
    CHECK_FALSE (CFBO::parseColor ("123456789").has_value ());
}
