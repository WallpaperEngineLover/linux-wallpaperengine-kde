#pragma once

#include "VideoDriver.h"

#include <EGL/egl.h>
#include <memory>
#include <vector>

namespace WallpaperEngine::Render::Drivers {
class EglSharedContext final : public VideoDriver::SharedContext {
public:
    // the thread that creates it is the one that makes it current: eglBindAPI is per thread
    static std::unique_ptr<EglSharedContext> create (
	EGLDisplay display, EGLConfig config, EGLContext share, const std::vector<EGLint>& attributes, bool needsSurface
    ) {
	eglBindAPI (EGL_OPENGL_API);

	auto context = std::unique_ptr<EglSharedContext> (new EglSharedContext (display));

	context->m_context = eglCreateContext (display, config, share, attributes.data ());

	if (context->m_context == EGL_NO_CONTEXT) {
	    return nullptr;
	}

	if (needsSurface) {
	    const EGLint size[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };

	    context->m_surface = eglCreatePbufferSurface (display, config, size);

	    if (context->m_surface == EGL_NO_SURFACE) {
		return nullptr;
	    }
	}

	return context;
    }

    ~EglSharedContext () override {
	this->release ();

	if (this->m_context != EGL_NO_CONTEXT) {
	    eglDestroyContext (this->m_display, this->m_context);
	}

	if (this->m_surface != EGL_NO_SURFACE) {
	    eglDestroySurface (this->m_display, this->m_surface);
	}
    }

    bool makeCurrent () override {
	eglBindAPI (EGL_OPENGL_API);
	return eglMakeCurrent (this->m_display, this->m_surface, this->m_surface, this->m_context) == EGL_TRUE;
    }

    void release () override {
	eglMakeCurrent (this->m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglReleaseThread ();
    }

private:
    explicit EglSharedContext (EGLDisplay display) : m_display (display) { }

    EGLDisplay m_display;
    EGLContext m_context = EGL_NO_CONTEXT;
    EGLSurface m_surface = EGL_NO_SURFACE;
};
}
