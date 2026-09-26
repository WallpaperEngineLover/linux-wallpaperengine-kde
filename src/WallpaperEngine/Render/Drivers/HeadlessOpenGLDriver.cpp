#include "HeadlessOpenGLDriver.h"
#include "VideoFactories.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Drivers/Output/GLFWWindowOutput.h"

#include <EGL/eglext.h>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace WallpaperEngine::Render::Drivers;

namespace {
EGLDisplay openDisplay () {
    const auto queryDevices = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC> (eglGetProcAddress ("eglQueryDevicesEXT"));
    const auto queryDeviceString
	= reinterpret_cast<PFNEGLQUERYDEVICESTRINGEXTPROC> (eglGetProcAddress ("eglQueryDeviceStringEXT"));
    const auto getPlatformDisplay
	= reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC> (eglGetProcAddress ("eglGetPlatformDisplayEXT"));

    if (queryDevices == nullptr || queryDeviceString == nullptr || getPlatformDisplay == nullptr) {
	sLog.exception ("EGL device enumeration is not available, cannot render headless");
    }

    EGLint count = 0;
    queryDevices (0, nullptr, &count);
    std::vector<EGLDeviceEXT> devices (count);
    queryDevices (count, devices.data (), &count);

    const char* wanted = std::getenv ("LWE_HEADLESS_DEVICE");
    std::vector<std::pair<EGLDeviceEXT, std::string>> candidates;

    for (const auto device : devices) {
	// devices without a render node are software renderers (llvmpipe), the whole point is to avoid those
	const char* node = queryDeviceString (device, EGL_DRM_RENDER_NODE_FILE_EXT);

	if (node != nullptr && (wanted == nullptr || std::strcmp (node, wanted) == 0)) {
	    candidates.emplace_back (device, node);
	}
    }

    // with an iGPU and a dedicated card, EGL often lists the iGPU first; the card driving the boot
    // display is the better guess
    std::ranges::stable_partition (candidates, [] (const auto& candidate) {
	const auto name = std::filesystem::path (candidate.second).filename ();
	std::ifstream bootVga ("/sys/class/drm" / name / "device/boot_vga");
	char value = '0';
	bootVga >> value;
	return value == '1';
    });

    for (const auto& [device, node] : candidates) {
	const EGLDisplay display = getPlatformDisplay (EGL_PLATFORM_DEVICE_EXT, device, nullptr);

	if (display != EGL_NO_DISPLAY && eglInitialize (display, nullptr, nullptr)) {
	    sLog.out ("Headless rendering on ", node);
	    return display;
	}
    }

    sLog.exception ("No usable GPU found for headless rendering", wanted ? " (LWE_HEADLESS_DEVICE=" : "",
		    wanted ? wanted : "", wanted ? ")" : "");
    return EGL_NO_DISPLAY;
}
} // namespace

HeadlessOpenGLDriver::HeadlessOpenGLDriver (ApplicationContext& context, WallpaperApplication& app) :
    VideoDriver (app, m_mouseInput), m_context (context), m_mouseInput (*this) {
    this->m_display = openDisplay ();

    const EGLint configAttributes[] = {
	EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
	EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
	EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE
    };
    EGLint configs = 0;

    if (!eglChooseConfig (this->m_display, configAttributes, &this->m_config, 1, &configs) || configs == 0) {
	sLog.exception ("No pbuffer capable EGL config for headless rendering");
    }

    eglBindAPI (EGL_OPENGL_API);

    const EGLint contextAttributes[] = {
	EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
	EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
	EGL_CONTEXT_OPENGL_DEBUG, EGL_TRUE, EGL_NONE
    };
    this->m_eglContext = eglCreateContext (this->m_display, this->m_config, EGL_NO_CONTEXT, contextAttributes);

    if (this->m_eglContext == EGL_NO_CONTEXT) {
	sLog.exception ("Cannot create headless OpenGL context: ", eglGetError ());
    }

    if (context.settings.render.mode == ApplicationContext::EXPLICIT_WINDOW) {
	this->m_size = { context.settings.render.window.geometry.z, context.settings.render.window.geometry.w };
    }

    this->createSurface ();

    glewExperimental = GL_TRUE;

    // GLEW built with GLX support fails its GLX part without an X display, the GL functions are loaded by then
    if (const GLenum result = glewInit (); result != GLEW_OK && result != GLEW_ERROR_NO_GLX_DISPLAY) {
	sLog.error ("Failed to initialize GLEW: ", glewGetErrorString (result));
    }

    this->m_output = new Output::GLFWWindowOutput (context, *this);
}

HeadlessOpenGLDriver::~HeadlessOpenGLDriver () {
    eglMakeCurrent (this->m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface (this->m_display, this->m_surface);
    eglDestroyContext (this->m_display, this->m_eglContext);
    eglTerminate (this->m_display);
}

void HeadlessOpenGLDriver::createSurface () {
    const EGLint attributes[] = { EGL_WIDTH, this->m_size.x, EGL_HEIGHT, this->m_size.y, EGL_NONE };
    const EGLSurface surface = eglCreatePbufferSurface (this->m_display, this->m_config, attributes);

    if (surface == EGL_NO_SURFACE) {
	sLog.exception ("Cannot create a ", this->m_size.x, "x", this->m_size.y, " pbuffer: ", eglGetError ());
    }

    eglMakeCurrent (this->m_display, surface, surface, this->m_eglContext);

    if (this->m_surface != EGL_NO_SURFACE) {
	eglDestroySurface (this->m_display, this->m_surface);
    }

    this->m_surface = surface;
}

Output::Output& HeadlessOpenGLDriver::getOutput () { return *this->m_output; }

float HeadlessOpenGLDriver::getRenderTime () const {
    return std::chrono::duration<float> (std::chrono::steady_clock::now () - this->m_started).count ();
}

bool HeadlessOpenGLDriver::closeRequested () { return false; }

void HeadlessOpenGLDriver::resizeWindow (glm::ivec2 size) {
    if (size == this->m_size || size.x <= 0 || size.y <= 0) {
	return;
    }

    this->m_size = size;
    this->createSurface ();
}

void HeadlessOpenGLDriver::resizeWindow (glm::ivec4 sizeandpos) { this->resizeWindow (glm::ivec2 (sizeandpos.z, sizeandpos.w)); }

void HeadlessOpenGLDriver::showWindow () { }

void HeadlessOpenGLDriver::hideWindow () { }

glm::ivec2 HeadlessOpenGLDriver::getFramebufferSize () const { return this->m_size; }

uint32_t HeadlessOpenGLDriver::getFrameCounter () const { return this->m_frameCounter; }

void HeadlessOpenGLDriver::dispatchEventQueue () {
    glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    for (const auto& [screen, viewport] : this->m_output->getViewports ()) {
	this->getApp ().update (viewport);
    }

    this->m_output->updateRender ();

    // there's no swap to throttle on, so keep at most one frame queued on the GPU instead of letting
    // --fps 1000 pile up work faster than it gets done
    if (this->m_lastFrame != nullptr) {
	glClientWaitSync (this->m_lastFrame, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
	glDeleteSync (this->m_lastFrame);
    }

    this->m_lastFrame = glFenceSync (GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush ();
    this->m_frameCounter++;
}

void* HeadlessOpenGLDriver::getProcAddress (const char* name) const {
    return reinterpret_cast<void*> (eglGetProcAddress (name));
}

__attribute__ ((constructor)) void registerHeadlessOpenGLDriver () {
    const auto create = [] (ApplicationContext& context, WallpaperApplication& application) -> std::unique_ptr<VideoDriver> {
	return std::make_unique<HeadlessOpenGLDriver> (context, application);
    };

    sVideoFactories.registerDriver (ApplicationContext::EXPLICIT_WINDOW, "headless", create);
    sVideoFactories.registerDriver (ApplicationContext::NORMAL_WINDOW, "headless", create);
}
