#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <glm/glm.hpp>

#include "WallpaperEngine/Render/Wallpapers/MouseMapping.h"

// GLFW y down, GL y up
TEST_CASE ("GLFW to OpenGL coordinate conversion") {
    const int framebufferHeight = 1080;

    double glfwY = 0.0;
    double openglY = static_cast<double> (framebufferHeight) - glfwY;
    CHECK_THAT (openglY, Catch::Matchers::WithinULP (1080.0, 0));

    glfwY = 1080.0;
    openglY = static_cast<double> (framebufferHeight) - glfwY;
    CHECK_THAT (openglY, Catch::Matchers::WithinULP (0.0, 0));

    glfwY = 540.0;
    openglY = static_cast<double> (framebufferHeight) - glfwY;
    CHECK_THAT (openglY, Catch::Matchers::WithinULP (540.0, 0));
}

TEST_CASE ("Wayland to OpenGL coordinate conversion") {
    const double viewportHeight = 1080.0;

    double waylandY = 0.0;
    double openglY = viewportHeight - waylandY;
    CHECK_THAT (openglY, Catch::Matchers::WithinULP (1080.0, 0));

    waylandY = 1080.0;
    openglY = viewportHeight - waylandY;
    CHECK_THAT (openglY, Catch::Matchers::WithinULP (0.0, 0));

    waylandY = 540.0;
    openglY = viewportHeight - waylandY;
    CHECK_THAT (openglY, Catch::Matchers::WithinULP (540.0, 0));
}

// normalized: 0 bottom, 1 top
TEST_CASE ("OpenGL to normalized coordinate conversion") {
    const int viewportY = 0;
    const int viewportHeight = 1080;

    double openglY = 1080.0;
    double normalizedY = glm::clamp ((openglY - viewportY) / static_cast<double> (viewportHeight), 0.0, 1.0);
    CHECK_THAT (normalizedY, Catch::Matchers::WithinULP (1.0, 0));

    openglY = 0.0;
    normalizedY = glm::clamp ((openglY - viewportY) / static_cast<double> (viewportHeight), 0.0, 1.0);
    CHECK_THAT (normalizedY, Catch::Matchers::WithinULP (0.0, 0));

    openglY = 540.0;
    normalizedY = glm::clamp ((openglY - viewportY) / static_cast<double> (viewportHeight), 0.0, 1.0);
    CHECK (std::abs (normalizedY - 0.5) < 0.001);
}

// CEF has y down
TEST_CASE ("OpenGL to CEF coordinate conversion") {
    const int viewportHeight = 1080;
    const int viewportY = 0;

    double openglY = 1080.0;
    int clampedY = std::clamp (static_cast<int> (openglY - viewportY), 0, viewportHeight);
    int cefY = viewportHeight - clampedY;
    CHECK (cefY == 0);

    openglY = 0.0;
    clampedY = std::clamp (static_cast<int> (openglY - viewportY), 0, viewportHeight);
    cefY = viewportHeight - clampedY;
    CHECK (cefY == 1080);

    openglY = 540.0;
    clampedY = std::clamp (static_cast<int> (openglY - viewportY), 0, viewportHeight);
    cefY = viewportHeight - clampedY;
    CHECK (cefY == 540);
}

TEST_CASE ("Complete coordinate flow: GLFW to normalized") {
    const int framebufferHeight = 1080;
    const int viewportY = 0;
    const int viewportHeight = 1080;

    double glfwY = 0.0;
    double openglY = static_cast<double> (framebufferHeight) - glfwY;
    double normalizedY = glm::clamp ((openglY - viewportY) / static_cast<double> (viewportHeight), 0.0, 1.0);
    CHECK_THAT (normalizedY, Catch::Matchers::WithinULP (1.0, 0));

    glfwY = 1080.0;
    openglY = static_cast<double> (framebufferHeight) - glfwY;
    normalizedY = glm::clamp ((openglY - viewportY) / static_cast<double> (viewportHeight), 0.0, 1.0);
    CHECK_THAT (normalizedY, Catch::Matchers::WithinULP (0.0, 0));
}

TEST_CASE ("Coordinate conversion with different viewport sizes") {
    {
	const int height = 1080;
	double glfwY = 0.0;
	double openglY = static_cast<double> (height) - glfwY;
	CHECK_THAT (openglY, Catch::Matchers::WithinULP (1080.0, 0));
    }

    {
	const int height = 1440;
	double glfwY = 0.0;
	double openglY = static_cast<double> (height) - glfwY;
	CHECK_THAT (openglY, Catch::Matchers::WithinULP (1440.0, 0));
    }

    {
	const int height = 600;
	double glfwY = 0.0;
	double openglY = static_cast<double> (height) - glfwY;
	CHECK_THAT (openglY, Catch::Matchers::WithinULP (600.0, 0));
    }
}

TEST_CASE ("Cursor clip y is the same picture on flipped and unflipped outputs") {
    using WallpaperEngine::Render::Wallpapers::cursorClipY;

    // pointer at the top of the screen: an unflipped output samples buffer row 0 there, a flipped one row 1
    CHECK_THAT (cursorClipY (0.0f, false), Catch::Matchers::WithinULP (-1.0f, 0));
    CHECK_THAT (cursorClipY (1.0f, true), Catch::Matchers::WithinULP (-1.0f, 0));

    CHECK_THAT (cursorClipY (1.0f, false), Catch::Matchers::WithinULP (1.0f, 0));
    CHECK_THAT (cursorClipY (0.0f, true), Catch::Matchers::WithinULP (1.0f, 0));

    CHECK_THAT (cursorClipY (0.25f, false), Catch::Matchers::WithinULP (cursorClipY (0.75f, true), 0));
}
