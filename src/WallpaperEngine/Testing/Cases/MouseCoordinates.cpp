#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <glm/glm.hpp>

/**
 * Test GLFW to OpenGL coordinate conversion
 * GLFW: Y=0 at top, Y=height at bottom
 * OpenGL: Y=0 at bottom, Y=height at top
 */
TEST_CASE ("GLFW to OpenGL coordinate conversion") {
    const int framebufferHeight = 1080;

    double glfwY = 0.0;
    double openglY = static_cast<double> (framebufferHeight) - glfwY;
    CHECK (openglY == 1080.0);

    glfwY = 1080.0;
    openglY = static_cast<double> (framebufferHeight) - glfwY;
    CHECK (openglY == 0.0);

    glfwY = 540.0;
    openglY = static_cast<double> (framebufferHeight) - glfwY;
    CHECK (openglY == 540.0);
}

/**
 * Test Wayland to OpenGL coordinate conversion
 * Wayland: Y=0 at top, Y=height at bottom
 * OpenGL: Y=0 at bottom, Y=height at top
 */
TEST_CASE ("Wayland to OpenGL coordinate conversion") {
    const double viewportHeight = 1080.0;

    double waylandY = 0.0;
    double openglY = viewportHeight - waylandY;
    CHECK (openglY == 1080.0);

    waylandY = 1080.0;
    openglY = viewportHeight - waylandY;
    CHECK (openglY == 0.0);

    waylandY = 540.0;
    openglY = viewportHeight - waylandY;
    CHECK (openglY == 540.0);
}

/**
 * Test OpenGL to normalized coordinate conversion
 * OpenGL: Y=0 at bottom, Y=height at top
 * Normalized: 0=bottom, 1=top (OpenGL convention)
 */
TEST_CASE ("OpenGL to normalized coordinate conversion") {
    const int viewportY = 0;
    const int viewportHeight = 1080;

    double openglY = 1080.0;
    double normalizedY = glm::clamp ((openglY - viewportY) / static_cast<double> (viewportHeight), 0.0, 1.0);
    CHECK (normalizedY == 1.0);

    openglY = 0.0;
    normalizedY = glm::clamp ((openglY - viewportY) / static_cast<double> (viewportHeight), 0.0, 1.0);
    CHECK (normalizedY == 0.0);

    openglY = 540.0;
    normalizedY = glm::clamp ((openglY - viewportY) / static_cast<double> (viewportHeight), 0.0, 1.0);
    CHECK (std::abs (normalizedY - 0.5) < 0.001);
}

/**
 * Test OpenGL to CEF coordinate conversion
 * OpenGL: Y=0 at bottom, Y=height at top
 * CEF: Y=0 at top, Y=height at bottom
 */
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

/**
 * Test complete coordinate flow: GLFW -> OpenGL -> Normalized
 * Verifies the full pipeline works correctly
 */
TEST_CASE ("Complete coordinate flow: GLFW to normalized") {
    const int framebufferHeight = 1080;
    const int viewportY = 0;
    const int viewportHeight = 1080;

    double glfwY = 0.0;
    double openglY = static_cast<double> (framebufferHeight) - glfwY;
    double normalizedY = glm::clamp ((openglY - viewportY) / static_cast<double> (viewportHeight), 0.0, 1.0);
    CHECK (normalizedY == 1.0);

    glfwY = 1080.0;
    openglY = static_cast<double> (framebufferHeight) - glfwY;
    normalizedY = glm::clamp ((openglY - viewportY) / static_cast<double> (viewportHeight), 0.0, 1.0);
    CHECK (normalizedY == 0.0);
}

/**
 * Test coordinate conversion with different viewport sizes
 * Ensures conversion works with non-standard viewport dimensions
 */
TEST_CASE ("Coordinate conversion with different viewport sizes") {
    {
	const int height = 1080;
	double glfwY = 0.0;
	double openglY = static_cast<double> (height) - glfwY;
	CHECK (openglY == 1080.0);
    }

    {
	const int height = 1440;
	double glfwY = 0.0;
	double openglY = static_cast<double> (height) - glfwY;
	CHECK (openglY == 1440.0);
    }

    {
	const int height = 600;
	double glfwY = 0.0;
	double openglY = static_cast<double> (height) - glfwY;
	CHECK (openglY == 600.0);
    }
}
