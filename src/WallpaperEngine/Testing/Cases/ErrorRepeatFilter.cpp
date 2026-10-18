#include <catch2/catch_test_macros.hpp>

#include <string>

#include "WallpaperEngine/Scripting/ErrorRepeatFilter.h"

using WallpaperEngine::Scripting::ErrorRepeatFilter;

TEST_CASE ("A script failing the same way every frame is reported once") {
    ErrorRepeatFilter filter;
    std::size_t suppressed = 99;

    CHECK (filter.admit ("script", "TypeError: x", suppressed));
    CHECK (suppressed == 0);

    for (int frame = 0; frame < 100; frame++) {
	CHECK_FALSE (filter.admit ("script", "TypeError: x", suppressed));
    }

    CHECK (filter.admit ("script", "RangeError: y", suppressed));
    CHECK (suppressed == 100);
}

TEST_CASE ("A script alternating between two errors is not reported every frame") {
    ErrorRepeatFilter filter;
    std::size_t suppressed = 0;
    int reported = 0;

    for (int frame = 0; frame < 50; frame++) {
	reported += filter.admit ("script", frame % 2 == 0 ? "a" : "b", suppressed) ? 1 : 0;
    }

    CHECK (reported == 2);
}

TEST_CASE ("Scripts are told apart and memory stays bounded") {
    ErrorRepeatFilter filter;
    std::size_t suppressed = 0;

    CHECK (filter.admit ("one", "same", suppressed));
    CHECK (filter.admit ("two", "same", suppressed));
    CHECK_FALSE (filter.admit ("one", "same", suppressed));

    for (std::size_t i = 0; i < ErrorRepeatFilter::REMEMBERED_PER_SCRIPT * 3; i++) {
	CHECK (filter.admit ("one", "error " + std::to_string (i), suppressed));
    }

    // the oldest ones fell out of the window
    CHECK (filter.admit ("one", "same", suppressed));

    for (std::size_t i = 0; i < ErrorRepeatFilter::MAX_SCRIPTS * 2; i++) {
	CHECK (filter.admit ("script " + std::to_string (i), "e", suppressed));
    }
}
