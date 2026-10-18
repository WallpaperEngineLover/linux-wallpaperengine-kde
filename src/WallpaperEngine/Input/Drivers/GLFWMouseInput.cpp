#include "GLFWMouseInput.h"
#include <glm/common.hpp>

#include "WallpaperEngine/Render/Drivers/GLFWOpenGLDriver.h"

#include <X11/Xlib.h>

using namespace WallpaperEngine::Input::Drivers;

GLFWMouseInput::GLFWMouseInput (const Render::Drivers::GLFWOpenGLDriver& driver) : m_driver (driver) { }

void GLFWMouseInput::update () {
    if (!this->m_driver.getApp ().getContext ().settings.mouse.enabled) {
	this->m_reportedPosition = { 0, 0 };
	return;
    }

    const auto& render = this->m_driver.getApp ().getContext ().settings.render;

    // a background window is click through and under everything, so GLFW never sees the pointer
    if (render.window.asBackground && render.mode == Application::ApplicationContext::EXPLICIT_WINDOW
	&& this->updateFromRoot ()) {
	return;
    }

    const int leftClickState = glfwGetMouseButton (this->m_driver.getWindow (), GLFW_MOUSE_BUTTON_LEFT);
    const int rightClickState = glfwGetMouseButton (this->m_driver.getWindow (), GLFW_MOUSE_BUTTON_RIGHT);

    this->m_leftClick = leftClickState == GLFW_RELEASE ? MouseClickStatus::Released : MouseClickStatus::Clicked;
    this->m_rightClick = rightClickState == GLFW_RELEASE ? MouseClickStatus::Released : MouseClickStatus::Clicked;

    glfwGetCursorPos (this->m_driver.getWindow (), &this->m_mousePosition.x, &this->m_mousePosition.y);

    // GLFW has y down, GL y up
    const glm::ivec2 framebufferSize = this->m_driver.getFramebufferSize ();
    this->m_mousePosition.y = static_cast<double> (framebufferSize.y) - this->m_mousePosition.y;

    this->m_reportedPosition = glm::mix (this->m_reportedPosition, this->m_mousePosition, 1.0);
}

glm::dvec2 GLFWMouseInput::position () const { return this->m_reportedPosition; }

WallpaperEngine::Input::MouseClickStatus GLFWMouseInput::leftClick () const { return m_leftClick; }

WallpaperEngine::Input::MouseClickStatus GLFWMouseInput::rightClick () const { return m_rightClick; }
bool GLFWMouseInput::updateFromRoot () {
    auto* display = static_cast<Display*> (this->m_driver.getX11Display ());

    if (display == nullptr) {
	return false;
    }

    Window rootReturn = 0, child = 0;
    int rootX = 0, rootY = 0, childX = 0, childY = 0;
    unsigned int mask = 0;

    if (!XQueryPointer (
	    display, DefaultRootWindow (display), &rootReturn, &child, &rootX, &rootY, &childX, &childY, &mask
	)) {
	return false;
    }

    const auto& geometry = this->m_driver.getApp ().getContext ().settings.render.window.geometry;
    const glm::ivec2 framebufferSize = this->m_driver.getFramebufferSize ();

    this->m_leftClick = (mask & Button1Mask) ? MouseClickStatus::Clicked : MouseClickStatus::Released;
    this->m_rightClick = (mask & Button3Mask) ? MouseClickStatus::Clicked : MouseClickStatus::Released;
    this->m_mousePosition
	= { static_cast<double> (rootX - geometry.x), static_cast<double> (framebufferSize.y) - (rootY - geometry.y) };
    this->m_reportedPosition = this->m_mousePosition;

    return true;
}
