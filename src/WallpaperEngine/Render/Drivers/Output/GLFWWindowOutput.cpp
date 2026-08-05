#include "GLFWWindowOutput.h"
#include "GLFWOutputViewport.h"
#include "WallpaperEngine/Logging/Log.h"
#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <unistd.h>

using namespace WallpaperEngine::Render::Drivers::Output;

GLFWWindowOutput::GLFWWindowOutput (ApplicationContext& context, VideoDriver& driver) : Output (context, driver) {
    if (this->m_context.settings.render.mode != Application::ApplicationContext::NORMAL_WINDOW
	&& this->m_context.settings.render.mode != Application::ApplicationContext::EXPLICIT_WINDOW) {
	sLog.exception ("Initializing window output when not in output mode, how did you get here?!");
    }

    driver.showWindow ();

    if (this->m_context.settings.render.mode == Application::ApplicationContext::EXPLICIT_WINDOW) {
	this->m_fullWidth = this->m_context.settings.render.window.geometry.z;
	this->m_fullHeight = this->m_context.settings.render.window.geometry.w;
	this->repositionWindow ();
    } else {
	this->m_fullWidth = this->m_driver.getFramebufferSize ().x;
	this->m_fullHeight = this->m_driver.getFramebufferSize ().y;
    }

    this->m_viewports["default"]
	= new GLFWOutputViewport { { 0, 0, this->m_fullWidth, this->m_fullHeight }, "default" };
}

void GLFWWindowOutput::repositionWindow () const {
    this->m_driver.resizeWindow (this->m_context.settings.render.window.geometry);
}

void GLFWWindowOutput::reset () {
    if (this->m_context.settings.render.mode == Application::ApplicationContext::EXPLICIT_WINDOW) {
	this->repositionWindow ();
    }
}

bool GLFWWindowOutput::renderVFlip () const { return true; }

bool GLFWWindowOutput::renderMultiple () const { return false; }

bool GLFWWindowOutput::haveImageBuffer () const { return false; }

void* GLFWWindowOutput::getImageBuffer () const { return nullptr; }

uint32_t GLFWWindowOutput::getImageBufferSize () const { return 0; }

void GLFWWindowOutput::updateRender () const {
    // re-read framebuffer size every frame so runtime resizes (EXPLICIT_WINDOW resizeWindow, or a
    // WM-initiated resize) re-stretch the scene instead of cropping the render at the old size
    this->m_fullWidth = this->m_driver.getFramebufferSize ().x;
    this->m_fullHeight = this->m_driver.getFramebufferSize ().y;

    this->m_viewports["default"]->viewport = { 0, 0, this->m_fullWidth, this->m_fullHeight };
}