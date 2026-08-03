#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Application/ApplicationContext.h"
#include "WallpaperEngine/Data/Utils/AudioSensitivity.h"

using WallpaperEngine::Application::ApplicationContext;
using WallpaperEngine::Data::Utils::scaleAudioRange;

namespace {
ApplicationContext makeContext () {
    static const char* argv[] = { "linux-wallpaperengine" };
    return ApplicationContext (1, const_cast<char**> (argv));
}
} // namespace

TEST_CASE ("resolveAudioSensitivity matches by object id") {
    auto context = makeContext ();
    context.settings.general.audioSensitivity["24"] = 0.5f;

    const auto result = context.resolveAudioSensitivity (24, "transbig");

    REQUIRE (result.has_value ());
    CHECK (result.value () == Catch::Approx (0.5f));
}

TEST_CASE ("resolveAudioSensitivity matches by object name") {
    auto context = makeContext ();
    context.settings.general.audioSensitivity["transbig"] = 0.5f;

    const auto result = context.resolveAudioSensitivity (24, "transbig");

    REQUIRE (result.has_value ());
    CHECK (result.value () == Catch::Approx (0.5f));
}

TEST_CASE ("resolveAudioSensitivity falls back to the * wildcard with no specific match") {
    auto context = makeContext ();
    context.settings.general.audioSensitivity["*"] = 0.25f;

    const auto result = context.resolveAudioSensitivity (99, "unrelated-object");

    REQUIRE (result.has_value ());
    CHECK (result.value () == Catch::Approx (0.25f));
}

TEST_CASE ("resolveAudioSensitivity prefers a specific match over the * wildcard") {
    auto context = makeContext ();
    context.settings.general.audioSensitivity["*"] = 0.25f;
    context.settings.general.audioSensitivity["24"] = 2.0f;

    const auto result = context.resolveAudioSensitivity (24, "transbig");

    REQUIRE (result.has_value ());
    CHECK (result.value () == Catch::Approx (2.0f));
}

TEST_CASE ("resolveAudioSensitivity returns nullopt with no match at all") {
    auto context = makeContext ();
    context.settings.general.audioSensitivity["24"] = 0.5f;

    CHECK_FALSE (context.resolveAudioSensitivity (99, "unrelated-object").has_value ());
}

TEST_CASE ("scaleAudioRange at multiplier 1 leaves the original range untouched") {
    const auto [min, max] = scaleAudioRange (0.9f, 1.1f, 1.0f);

    CHECK (min == Catch::Approx (0.9f));
    CHECK (max == Catch::Approx (1.1f));
}

TEST_CASE ("scaleAudioRange at multiplier 0 collapses to the midpoint - locked, no pulse") {
    const auto [min, max] = scaleAudioRange (0.9f, 1.1f, 0.0f);

    CHECK (min == Catch::Approx (1.0f));
    CHECK (max == Catch::Approx (1.0f));
    CHECK (min == max);
}

TEST_CASE ("scaleAudioRange at multiplier 2 doubles the swing around the same midpoint") {
    const auto [min, max] = scaleAudioRange (0.9f, 1.1f, 2.0f);

    CHECK (min == Catch::Approx (0.8f));
    CHECK (max == Catch::Approx (1.2f));
}

TEST_CASE ("scaleAudioRange works for a midpoint other than 1.0") {
    // e.g. an alpha-driven pulse authored around 0.5 instead of a scale pulse around 1.0
    const auto [min, max] = scaleAudioRange (0.3f, 0.7f, 0.5f);

    CHECK (min == Catch::Approx (0.4f));
    CHECK (max == Catch::Approx (0.6f));
}
