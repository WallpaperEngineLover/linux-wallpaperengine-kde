#include "GLFWWindowOutput.h"
#include "GLFWOutputViewport.h"
#include "WallpaperEngine/Logging/Log.h"
#include "X11BackgroundWindow.h"
#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>

#include <algorithm>
#include <cstdlib>
#include <unistd.h>

using namespace WallpaperEngine::Render::Drivers::Output;

GLFWWindowOutput::GLFWWindowOutput (ApplicationContext& context, VideoDriver& driver) : Output (context, driver) {
    if (this->m_context.settings.render.mode != Application::ApplicationContext::NORMAL_WINDOW
	&& this->m_context.settings.render.mode != Application::ApplicationContext::EXPLICIT_WINDOW) {
	sLog.exception ("Initializing window output when not in output mode, how did you get here?!");
    }

    const bool background = this->m_context.settings.render.mode == Application::ApplicationContext::EXPLICIT_WINDOW
	&& this->m_context.settings.render.window.asBackground;

    if (background && driver.getX11Display () == nullptr) {
	sLog.error ("--window-background only works on X11, the window stays a normal one");
    }

    // the GL window stays hidden for a background window, it only renders
    if (!background || driver.getX11Display () == nullptr) {
	driver.showWindow ();
    }

    if (this->m_context.settings.render.mode == Application::ApplicationContext::EXPLICIT_WINDOW) {
	this->m_fullWidth = this->m_context.settings.render.window.geometry.z;
	this->m_fullHeight = this->m_context.settings.render.window.geometry.w;
	this->repositionWindow ();

	if (background && driver.getX11Display () != nullptr) {
	    const auto& geometry = this->m_context.settings.render.window.geometry;
	    this->m_background = std::make_unique<X11BackgroundWindow> (geometry.x, geometry.y, geometry.z, geometry.w);
	}
    } else {
	this->m_fullWidth = this->m_driver.getFramebufferSize ().x;
	this->m_fullHeight = this->m_driver.getFramebufferSize ().y;
    }

    this->m_viewports["default"]
	= new GLFWOutputViewport { { 0, 0, this->m_fullWidth, this->m_fullHeight }, "default" };
}

GLFWWindowOutput::~GLFWWindowOutput () {
    for (const auto& [name, viewport] : this->m_viewports) {
	delete viewport;
    }
}

void GLFWWindowOutput::repositionWindow () const {
    this->m_driver.resizeWindow (this->m_context.settings.render.window.geometry);
}

void GLFWWindowOutput::reset () {
    if (this->m_context.settings.render.mode == Application::ApplicationContext::EXPLICIT_WINDOW) {
	this->repositionWindow ();
    }
}

bool GLFWWindowOutput::renderVFlip () const { return !this->hasBackground (); }

bool GLFWWindowOutput::renderMultiple () const { return false; }

bool GLFWWindowOutput::haveImageBuffer () const { return this->hasBackground (); }

void* GLFWWindowOutput::getImageBuffer () const {
    return this->hasBackground () ? this->m_background->data () : nullptr;
}

uint32_t GLFWWindowOutput::getImageBufferSize () const {
    return this->hasBackground () ? this->m_background->size () : 0;
}

bool GLFWWindowOutput::hasBackground () const { return this->m_background != nullptr && this->m_background->valid (); }

void GLFWWindowOutput::updateRender () const {
    // re-read every frame so window resizes re-stretch the scene instead of cropping it, the image buffer of a
    // background window has a fixed size
    if (!this->hasBackground ()) {
	this->m_fullWidth = this->m_driver.getFramebufferSize ().x;
	this->m_fullHeight = this->m_driver.getFramebufferSize ().y;
    }

    this->m_viewports["default"]->viewport = { 0, 0, this->m_fullWidth, this->m_fullHeight };

    if (this->hasBackground ()) {
	this->m_background->present ();
    }
}
