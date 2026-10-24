#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Application/FramePacer.h"

using WallpaperEngine::Application::FramePacer;
using namespace std::chrono_literals;

namespace {
constexpr std::chrono::nanoseconds SECOND = 1000000000ns;
}

// WE starts its frame counter at 0 and bumps it before dividing, so the first frame already shares the budget by fps -
// 1
TEST_CASE ("an idle first frame sleeps its share of the second") {
    FramePacer pacer;

    CHECK (pacer.beginSleep (0ms, 50) == SECOND / 49);
}

TEST_CASE ("work time comes off the sleep") {
    FramePacer pacer;

    CHECK (pacer.beginSleep (5ms, 50) == (SECOND - 5ms) / 49 - 5ms);
}

TEST_CASE ("a frame over budget does not sleep") {
    FramePacer pacer;

    CHECK (pacer.beginSleep (30ms, 50) == 0ms);
}

TEST_CASE ("a sleep that came back short is made up by later frames") {
    FramePacer pacer;
    const auto first = pacer.beginSleep (0ms, 50);
    pacer.endSleep (first - 5ms);

    CHECK (pacer.beginSleep (0ms, 50) > first);
}

TEST_CASE ("a frame that ran long shortens the sleeps after it") {
    FramePacer pacer;
    pacer.endSleep (pacer.beginSleep (0ms, 50));
    pacer.beginSleep (30ms, 50);

    CHECK (pacer.beginSleep (0ms, 50) < SECOND / 47);
}

TEST_CASE ("frames on time average 1/fps over a window") {
    FramePacer pacer;
    std::chrono::nanoseconds total {};

    for (int i = 0; i < 60; i++) {
	const auto work = 3ms;
	const auto pause = pacer.beginSleep (work, 60);
	pacer.endSleep (pause);
	total += work + pause;
    }

    CHECK (total >= 990ms);
    CHECK (total <= 1020ms);
}

TEST_CASE ("a long stall starts a new window") {
    FramePacer pacer;
    pacer.beginSleep (0ms, 30);

    CHECK (pacer.beginSleep (5s, 30) == 0ms);
    CHECK (pacer.beginSleep (0ms, 30) < 35ms);
}

TEST_CASE ("the fixed limiter sleeps 1/fps minus the work of that frame only") {
    FramePacer pacer;

    CHECK (pacer.beginSleep (0ms, 50, true) == 20ms);
    pacer.endSleep (15ms);
    CHECK (pacer.beginSleep (5ms, 50, true) == 15ms);
    CHECK (pacer.beginSleep (30ms, 50, true) == 0ms);
    CHECK (pacer.beginSleep (0ms, 50, true) == 20ms);
}
