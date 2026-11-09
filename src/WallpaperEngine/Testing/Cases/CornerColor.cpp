#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Render/CFBO.h"

using WallpaperEngine::Render::CFBO;

namespace {
void checkColor (const std::optional<glm::vec4>& color, const float r, const float g, const float b, const float a) {
    REQUIRE (color.has_value ());
    CHECK (color->x == Catch::Approx (r));
    CHECK (color->y == Catch::Approx (g));
    CHECK (color->z == Catch::Approx (b));
    CHECK (color->w == Catch::Approx (a));
}
}

TEST_CASE ("parseColor accepts 6-digit hex RGB, defaulting alpha to opaque") {
    const auto color = CFBO::parseColor ("1a2b3c");

    checkColor (color, 0x1a / 255.0f, 0x2b / 255.0f, 0x3c / 255.0f, 1.0f);
}

TEST_CASE ("parseColor accepts 8-digit hex RGBA") {
    const auto color = CFBO::parseColor ("1a2b3c80");

    checkColor (color, 0x1a / 255.0f, 0x2b / 255.0f, 0x3c / 255.0f, 0x80 / 255.0f);
}

TEST_CASE ("parseColor strips a leading #") {
    const auto color = CFBO::parseColor ("#000000");

    checkColor (color, 0.0f, 0.0f, 0.0f, 1.0f);
}

TEST_CASE ("parseColor rejects malformed input") {
    CHECK_FALSE (CFBO::parseColor ("").has_value ());
    CHECK_FALSE (CFBO::parseColor ("fff").has_value ());
    CHECK_FALSE (CFBO::parseColor ("gggggg").has_value ());
    CHECK_FALSE (CFBO::parseColor ("1234567").has_value ());
    CHECK_FALSE (CFBO::parseColor ("123456789").has_value ());
}
