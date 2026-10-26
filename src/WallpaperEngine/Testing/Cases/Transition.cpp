#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Render/Transition.h"

using WallpaperEngine::Render::parseTransition;
using WallpaperEngine::Render::TransitionKind;

TEST_CASE ("transition names", "[transition]") {
    REQUIRE (parseTransition ("none") == TransitionKind::None);
    REQUIRE (parseTransition ("fade") == TransitionKind::Fade);
    REQUIRE (parseTransition ("slide-left") == TransitionKind::SlideLeft);
    REQUIRE (parseTransition ("slide-right") == TransitionKind::SlideRight);
    REQUIRE (parseTransition ("wipe-left") == TransitionKind::WipeLeft);
    REQUIRE (parseTransition ("wipe-right") == TransitionKind::WipeRight);
    REQUIRE (parseTransition ("zoom") == TransitionKind::Zoom);
}

TEST_CASE ("unknown transition names are refused", "[transition]") {
    REQUIRE_FALSE (parseTransition ("").has_value ());
    REQUIRE_FALSE (parseTransition ("Fade").has_value ());
    REQUIRE_FALSE (parseTransition ("slide-up").has_value ());
}
