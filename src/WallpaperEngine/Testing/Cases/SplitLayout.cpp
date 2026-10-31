#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Render/SplitLayout.h"

using WallpaperEngine::Render::splitSlotRects;
using Split = WallpaperEngine::Application::ApplicationContext::SplitScreen;

TEST_CASE ("Aspect layout sizes slots from the wallpapers and fills the screen") {
    // two 9:16 and one 16:9 on 3440x1440
    const auto rects
	= splitSlotRects ({ 3440, 1440 }, { 9.0f / 16.0f, 9.0f / 16.0f, 16.0f / 9.0f }, Split::LAYOUT_ASPECT, true);

    REQUIRE (rects.size () == 3);
    // 810 + 810 + 2560 px at full height, shrunk to 3440
    CHECK (rects[0] == glm::ivec4 (0, 0, 667, 1440));
    CHECK (rects[1] == glm::ivec4 (667, 0, 666, 1440));
    CHECK (rects[2] == glm::ivec4 (1333, 0, 2107, 1440));
}

TEST_CASE ("Equal layout gives every slot the same share without gaps") {
    const auto rects = splitSlotRects ({ 1920, 1080 }, { 0.5f, 2.0f, 1.0f }, Split::LAYOUT_EQUAL, true);

    REQUIRE (rects.size () == 3);
    CHECK (rects[0] == glm::ivec4 (0, 0, 640, 1080));
    CHECK (rects[1] == glm::ivec4 (640, 0, 640, 1080));
    CHECK (rects[2] == glm::ivec4 (1280, 0, 640, 1080));
}

TEST_CASE ("Center layout keeps the wallpapers' shapes and centers them") {
    // two 1080x1920 on 1920x1080: 607.5 px each, 352.5 px bars
    const auto rects = splitSlotRects ({ 1920, 1080 }, { 0.5625f, 0.5625f }, Split::LAYOUT_CENTER, true);

    REQUIRE (rects.size () == 2);
    CHECK (rects[0].x == 353);
    CHECK (rects[0].z + rects[1].z == 1215);
    CHECK (rects[1].x == rects[0].x + rects[0].z);
    CHECK (rects[1].x + rects[1].z == 1568);
    CHECK (rects[0].w == 1080);
}

TEST_CASE ("Center layout falls back to filling the screen when the wallpapers don't fit") {
    const auto rects = splitSlotRects ({ 1920, 1080 }, { 16.0f / 9.0f, 16.0f / 9.0f }, Split::LAYOUT_CENTER, true);

    CHECK (rects[0] == glm::ivec4 (0, 0, 960, 1080));
    CHECK (rects[1] == glm::ivec4 (960, 0, 960, 1080));
}

TEST_CASE ("A web page takes the average of the others, or an equal share") {
    const auto mixed = splitSlotRects ({ 2000, 1000 }, { 0.5f, 0.0f, 1.5f }, Split::LAYOUT_ASPECT, true);

    CHECK (mixed[0].z == 333);
    CHECK (mixed[1].z == 667);
    CHECK (mixed[2].z == 1000);

    const auto pages = splitSlotRects ({ 1920, 1080 }, { 0.0f, 0.0f }, Split::LAYOUT_CENTER, true);

    CHECK (pages[0] == glm::ivec4 (0, 0, 960, 1080));
    CHECK (pages[1] == glm::ivec4 (960, 0, 960, 1080));
}

TEST_CASE ("A portrait screen stacks the slots, the first one on top") {
    const auto up = splitSlotRects ({ 1080, 1920 }, { 16.0f / 9.0f, 16.0f / 9.0f }, Split::LAYOUT_ASPECT, true);

    CHECK (up[0] == glm::ivec4 (0, 960, 1080, 960));
    CHECK (up[1] == glm::ivec4 (0, 0, 1080, 960));

    const auto down = splitSlotRects ({ 1080, 1920 }, { 16.0f / 9.0f, 16.0f / 9.0f }, Split::LAYOUT_ASPECT, false);

    CHECK (down[0] == glm::ivec4 (0, 0, 1080, 960));
    CHECK (down[1] == glm::ivec4 (0, 960, 1080, 960));
}

TEST_CASE ("A screen without pixels gets empty rects, one per slot") {
    for (const auto size : { glm::ivec2 (0, 1080), glm::ivec2 (1920, 0), glm::ivec2 (0, 0), glm::ivec2 (-5, 1080) }) {
	const auto rects = splitSlotRects (size, { 1.0f, 1.0f }, Split::LAYOUT_ASPECT, true);

	REQUIRE (rects.size () == 2);
	CHECK (rects[0] == glm::ivec4 (0));
	CHECK (rects[1] == glm::ivec4 (0));
    }

    CHECK (splitSlotRects ({ 1920, 1080 }, {}, Split::LAYOUT_ASPECT, true).empty ());
}

TEST_CASE ("A square screen is split into columns") {
    const auto rects = splitSlotRects ({ 1000, 1000 }, { 1.0f, 1.0f }, Split::LAYOUT_ASPECT, true);

    CHECK (rects[0] == glm::ivec4 (0, 0, 500, 1000));
    CHECK (rects[1] == glm::ivec4 (500, 0, 500, 1000));
}

TEST_CASE ("On a portrait screen a wider wallpaper gets less height") {
    const auto rects = splitSlotRects ({ 1080, 1920 }, { 2.0f, 1.0f }, Split::LAYOUT_ASPECT, false);

    CHECK (rects[0] == glm::ivec4 (0, 0, 1080, 640));
    CHECK (rects[1] == glm::ivec4 (0, 640, 1080, 1280));
}

TEST_CASE ("A page between sized wallpapers counts as their average, not as one") {
    const auto rects = splitSlotRects ({ 3000, 1000 }, { 2.0f, 0.0f, 4.0f }, Split::LAYOUT_ASPECT, true);

    CHECK (rects[0].z == 667);
    CHECK (rects[1].z == 1000);
    CHECK (rects[2].z == 1333);
}
