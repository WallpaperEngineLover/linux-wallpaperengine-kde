#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Data/Builders/ColorBuilder.h"
#include "WallpaperEngine/Data/JSON.h"

using WallpaperEngine::Data::JSON::JSON;

// vectors are usually "x y z" strings, text "size"/"padding" can be a number or array; optional(key, default) is
// noexcept

TEST_CASE ("Vector properties parse from the usual space-separated string") {
    const JSON data = { { "padding", "1.5 2.5" } };

    const auto value = data.optional<glm::vec2> ("padding", glm::vec2 (0.0f));

    CHECK (value.x == 1.5f);
    CHECK (value.y == 2.5f);
}

TEST_CASE ("Vector properties tolerate a bare number instead of a string") {
    const JSON data = { { "padding", 5 } };

    const auto value = data.optional<glm::vec2> ("padding", glm::vec2 (0.0f));

    CHECK (value.x == 5.0f);
    CHECK (value.y == 5.0f);
}

TEST_CASE ("Vector properties tolerate a JSON array instead of a string") {
    const JSON data = { { "padding", { 3, 4 } } };

    const auto value = data.optional<glm::vec2> ("padding", glm::vec2 (0.0f));

    CHECK (value.x == 3.0f);
    CHECK (value.y == 4.0f);
}

TEST_CASE ("Vector properties fall back to zero instead of crashing on a malformed string") {
    const JSON data = { { "padding", "not-a-vector" } };

    const auto value = data.optional<glm::vec2> ("padding", glm::vec2 (0.0f));

    CHECK (value.x == 0.0f);
    CHECK (value.y == 0.0f);
}

TEST_CASE ("Vector properties fall back to the default when the key is missing") {
    const JSON data = { { "other", "1 2" } };

    const auto value = data.optional<glm::vec2> ("padding", glm::vec2 (9.0f, 9.0f));

    CHECK (value.x == 9.0f);
    CHECK (value.y == 9.0f);
}

TEST_CASE ("Hex colors decode as RRGGBBAA whatever the red channel is") {
    using WallpaperEngine::Data::Builders::ColorBuilder;

    const auto shortForm = ColorBuilder::parse ("#fff");
    CHECK (shortForm.r == 1.0f);
    CHECK (shortForm.b == 1.0f);
    CHECK (shortForm.a == 1.0f);

    const auto withAlpha = ColorBuilder::parse ("#ff000080");
    CHECK (withAlpha.r == 1.0f);
    CHECK (withAlpha.g == 0.0f);
    CHECK (withAlpha.a == Catch::Approx (0x80 / 255.0f));

    const auto plain = ColorBuilder::parse ("#1a2b3c");
    CHECK (plain.r == Catch::Approx (0x1a / 255.0f));
    CHECK (plain.g == Catch::Approx (0x2b / 255.0f));
    CHECK (plain.b == Catch::Approx (0x3c / 255.0f));
    CHECK (plain.a == 1.0f);
}
