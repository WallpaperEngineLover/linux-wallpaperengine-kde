#include "VideoDriver.h"

#include <GL/glew.h>

#include "WallpaperEngine/Application/ApplicationContext.h"
#include "WallpaperEngine/Application/WallpaperApplication.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Drivers/Output/GLFWWindowOutput.h"

using namespace WallpaperEngine::Input;
using WallpaperEngine::Application::ApplicationContext;
using namespace WallpaperEngine::Render::Drivers;

VideoDriver::VideoDriver (WallpaperApplication& app, MouseInput& mouseInput) :
    m_app (app), m_inputContext (mouseInput) { }

WallpaperApplication& VideoDriver::getApp () const { return this->m_app; }

InputContext& VideoDriver::getInputContext () { return this->m_inputContext; }

void VideoDriver::initializeGlew () {
    glewExperimental = GL_TRUE;

    if (const GLenum result = glewInit (); result != GLEW_OK && result != GLEW_ERROR_NO_GLX_DISPLAY) {
	sLog.error ("Failed to initialize GLEW: ", glewGetErrorString (result));
    }
}

void VideoDriver::updateViewports (const Output::Output& output) const {
    for (const auto& [screen, viewport] : output.getViewports ()) {
	this->getApp ().update (viewport);
    }
}

Output::Output* VideoDriver::createWindowOutput (ApplicationContext& context) {
    if (context.settings.render.mode != ApplicationContext::EXPLICIT_WINDOW
	&& context.settings.render.mode != ApplicationContext::NORMAL_WINDOW) {
	return nullptr;
    }

    return new Output::GLFWWindowOutput (context, *this);
}
