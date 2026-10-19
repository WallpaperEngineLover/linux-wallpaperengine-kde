#include "GLFWOpenGLDriver.h"
#include "VideoFactories.h"
#include "WallpaperEngine/Logging/Log.h"

#include "WallpaperEngine/Render/Drivers/Output/GLFWWindowOutput.h"
#ifdef ENABLE_X11
#include "WallpaperEngine/Render/Drivers/Output/X11Output.h"
#endif

#define GLFW_EXPOSE_NATIVE_X11
#include "WallpaperEngine/Debugging/CallStack.h"

#include <GLFW/glfw3native.h>

#include <cstdlib>
#include <unistd.h>

using namespace WallpaperEngine::Render::Drivers;

void CustomGLFWErrorHandler (int errorCode, const char* reason) { sLog.error ("GLFW error ", errorCode, ": ", reason); }

GLFWOpenGLDriver::GLFWOpenGLDriver (const char* windowTitle, ApplicationContext& context, WallpaperApplication& app) :
    VideoDriver (app, m_mouseInput), m_mouseInput (*this) {
    glfwSetErrorCallback (CustomGLFWErrorHandler);

    if (glfwInit () == GLFW_FALSE) {
	sLog.exception ("Failed to initialize glfw");
    }

    glfwWindowHint (GLFW_SAMPLES, 4);
    glfwWindowHint (GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint (GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint (GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    // drivers only emit KHR_debug output for debug contexts
    glfwWindowHint (GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
    glfwWindowHint (GLFW_VISIBLE, getenv ("LWE_DEBUG_VISIBLE_WINDOW") ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHintString (GLFW_X11_CLASS_NAME, "linux-wallpaperengine");
    glfwWindowHintString (GLFW_X11_INSTANCE_NAME, "linux-wallpaperengine");

    if (context.settings.render.mode == Application::ApplicationContext::EXPLICIT_WINDOW) {
	const bool regular = context.settings.render.window.regular;

	glfwWindowHint (GLFW_RESIZABLE, regular ? GLFW_TRUE : GLFW_FALSE);
	glfwWindowHint (GLFW_DECORATED, regular ? GLFW_TRUE : GLFW_FALSE);
	glfwWindowHint (GLFW_FLOATING, regular ? GLFW_FALSE : GLFW_TRUE);
    }

#if !NDEBUG
    glfwWindowHint (GLFW_OPENGL_DEBUG_CONTEXT, GL_TRUE);
#endif

    this->m_window = glfwCreateWindow (640, 480, windowTitle, nullptr, nullptr);

    if (this->m_window == nullptr) {
	sLog.exception ("Cannot create window");
    }

    glfwMakeContextCurrent (this->m_window);

    if (const GLenum result = glewInit (); result != GLEW_OK) {
	sLog.error ("Failed to initialize GLEW: ", glewGetErrorString (result));
    }

    if (context.settings.render.mode == ApplicationContext::EXPLICIT_WINDOW
	|| context.settings.render.mode == ApplicationContext::NORMAL_WINDOW) {
	m_output = new WallpaperEngine::Render::Drivers::Output::GLFWWindowOutput (context, *this);
	this->m_offscreen = context.settings.render.mode == ApplicationContext::EXPLICIT_WINDOW
	    && context.settings.render.window.asBackground && m_output->haveImageBuffer ();
    }
#ifdef ENABLE_X11
    else {
	m_output = new WallpaperEngine::Render::Drivers::Output::X11Output (context, *this);
    }
#else
    else {
	sLog.exception ("Trying to start GLFW in background mode without X11 support installed. Bailing out");
    }
#endif
}

GLFWOpenGLDriver::~GLFWOpenGLDriver () {
    this->destroyOffscreenTarget ();
    delete this->m_output;
    glfwTerminate ();
}

Output::Output& GLFWOpenGLDriver::getOutput () { return *this->m_output; }

float GLFWOpenGLDriver::getRenderTime () const { return static_cast<float> (glfwGetTime ()); }

bool GLFWOpenGLDriver::closeRequested () { return glfwWindowShouldClose (this->m_window); }

void GLFWOpenGLDriver::resizeWindow (glm::ivec2 size) { glfwSetWindowSize (this->m_window, size.x, size.y); }

void GLFWOpenGLDriver::resizeWindow (glm::ivec4 sizeandpos) {
    glfwSetWindowPos (this->m_window, sizeandpos.x, sizeandpos.y);
    glfwSetWindowSize (this->m_window, sizeandpos.z, sizeandpos.w);
}

void GLFWOpenGLDriver::showWindow () { glfwShowWindow (this->m_window); }

void GLFWOpenGLDriver::hideWindow () { glfwHideWindow (this->m_window); }

glm::ivec2 GLFWOpenGLDriver::getFramebufferSize () const {
    glm::ivec2 size;

    glfwGetFramebufferSize (this->m_window, &size.x, &size.y);

    return size;
}

uint32_t GLFWOpenGLDriver::getFrameCounter () const { return this->m_frameCounter; }

void GLFWOpenGLDriver::createOffscreenTarget (int width, int height) {
    glGenTextures (1, &this->m_targetColor);
    glBindTexture (GL_TEXTURE_2D, this->m_targetColor);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture (GL_TEXTURE_2D, 0);

    glGenFramebuffers (1, &this->m_targetFramebuffer);
    glBindFramebuffer (GL_FRAMEBUFFER, this->m_targetFramebuffer);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, this->m_targetColor, 0);

    if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
	sLog.error ("The offscreen target of the background window is incomplete, reading the window itself");
	this->destroyOffscreenTarget ();
	this->m_offscreen = false;
    }

    glBindFramebuffer (GL_FRAMEBUFFER, 0);
}

void GLFWOpenGLDriver::destroyOffscreenTarget () {
    if (this->m_targetFramebuffer != 0) {
	glDeleteFramebuffers (1, &this->m_targetFramebuffer);
	this->m_targetFramebuffer = 0;
    }

    if (this->m_targetColor != 0) {
	glDeleteTextures (1, &this->m_targetColor);
	this->m_targetColor = 0;
    }
}

void GLFWOpenGLDriver::dispatchEventQueue () {
    if (this->m_offscreen && this->m_targetFramebuffer == 0) {
	this->createOffscreenTarget (this->m_output->getFullWidth (), this->m_output->getFullHeight ());
	this->getApp ().setDestinationFramebuffer (this->m_targetFramebuffer);
    }

    glBindFramebuffer (GL_FRAMEBUFFER, this->m_targetFramebuffer);
    glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    for (const auto& [screen, viewport] : this->m_output->getViewports ()) {
	this->getApp ().update (viewport);
    }

    if (this->m_output->haveImageBuffer ()) {
	glBindFramebuffer (GL_READ_FRAMEBUFFER, this->m_targetFramebuffer);

	// glReadnPixels requires GL 4.5; older drivers fall back to glReadPixels
	if (GLEW_VERSION_4_5) {
	    glReadnPixels (
		0, 0, this->m_output->getFullWidth (), this->m_output->getFullHeight (), GL_BGRA, GL_UNSIGNED_BYTE,
		this->m_output->getImageBufferSize (), this->m_output->getImageBuffer ()
	    );
	} else {
	    glReadPixels (
		0, 0, this->m_output->getFullWidth (), this->m_output->getFullHeight (), GL_BGRA, GL_UNSIGNED_BYTE,
		this->m_output->getImageBuffer ()
	    );
	}

	GLenum error = glGetError ();

	if (error != GL_NO_ERROR) {
	    sLog.exception ("OpenGL error when reading texture ", error);
	}
    }

    glBindFramebuffer (GL_FRAMEBUFFER, 0);

    this->m_output->updateRender ();
    glfwSwapBuffers (this->m_window);
    glfwPollEvents ();
    this->m_frameCounter++;
}

void* GLFWOpenGLDriver::getProcAddress (const char* name) const {
    return reinterpret_cast<void*> (glfwGetProcAddress (name));
}

void* GLFWOpenGLDriver::getX11Display () const {
#if GLFW_VERSION_MAJOR > 3 || (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 4)
    if (glfwGetPlatform () != GLFW_PLATFORM_X11) {
	return nullptr;
    }
#endif

    return glfwGetX11Display ();
}

GLFWwindow* GLFWOpenGLDriver::getWindow () const { return this->m_window; }

__attribute__ ((constructor)) void registerGLFWOpenGLDriver () {
    sVideoFactories.registerDriver (
	ApplicationContext::DESKTOP_BACKGROUND, "x11",
	[] (ApplicationContext& context, WallpaperApplication& application) -> std::unique_ptr<VideoDriver> {
	    return std::make_unique<GLFWOpenGLDriver> ("wallpaperengine", context, application);
	}
    );
    sVideoFactories.registerDriver (
	ApplicationContext::EXPLICIT_WINDOW, DEFAULT_WINDOW_NAME,
	[] (ApplicationContext& context, WallpaperApplication& application) -> std::unique_ptr<VideoDriver> {
	    return std::make_unique<GLFWOpenGLDriver> ("wallpaperengine", context, application);
	}
    );
    sVideoFactories.registerDriver (
	ApplicationContext::NORMAL_WINDOW, DEFAULT_WINDOW_NAME,
	[] (ApplicationContext& context, WallpaperApplication& application) -> std::unique_ptr<VideoDriver> {
	    return std::make_unique<GLFWOpenGLDriver> ("wallpaperengine", context, application);
	}
    );
}
