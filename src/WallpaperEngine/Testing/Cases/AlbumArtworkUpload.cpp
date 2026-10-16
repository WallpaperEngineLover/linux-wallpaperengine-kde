#include <GL/glew.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <catch2/catch_test_macros.hpp>

#include <array>

#include "WallpaperEngine/Render/AlbumTexture.h"

namespace {
// windowless GL via Mesa's surfaceless platform
struct SurfacelessContext {
    SurfacelessContext () {
	const auto getPlatformDisplay
	    = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC> (eglGetProcAddress ("eglGetPlatformDisplayEXT"));

	if (getPlatformDisplay == nullptr) {
	    return;
	}

	display = getPlatformDisplay (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);

	if (display == EGL_NO_DISPLAY || !eglInitialize (display, nullptr, nullptr) || !eglBindAPI (EGL_OPENGL_API)) {
	    return;
	}

	const EGLint configAttributes[]
	    = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE };
	EGLConfig config = nullptr;
	EGLint count = 0;

	if (!eglChooseConfig (display, configAttributes, &config, 1, &count) || count != 1) {
	    return;
	}

	const EGLint surfaceAttributes[] = { EGL_WIDTH, 4, EGL_HEIGHT, 4, EGL_NONE };
	const EGLint contextAttributes[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE };

	surface = eglCreatePbufferSurface (display, config, surfaceAttributes);
	context = eglCreateContext (display, config, EGL_NO_CONTEXT, contextAttributes);

	if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT
	    || !eglMakeCurrent (display, surface, surface, context)) {
	    return;
	}

	// GLEW gives up under EGL looking for GLX, load entry points first
	glewExperimental = GL_TRUE;
	std::ignore = glewInit ();

	while (glGetError () != GL_NO_ERROR) { }

	ready = glBindBuffer != nullptr;
    }

    ~SurfacelessContext () {
	if (display == EGL_NO_DISPLAY) {
	    return;
	}

	eglMakeCurrent (display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

	if (context != EGL_NO_CONTEXT) {
	    eglDestroyContext (display, context);
	}

	if (surface != EGL_NO_SURFACE) {
	    eglDestroySurface (display, surface);
	}

	eglTerminate (display);
    }

    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    bool ready = false;
};

std::array<uint8_t, 8> firstPixels (GLuint texture) {
    std::array<uint8_t, 8> pixels {};
    GLint previous = 0;

    glGetIntegerv (GL_TEXTURE_BINDING_2D, &previous);
    glBindTexture (GL_TEXTURE_2D, texture);
    glGetTexImage (GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data ());
    glBindTexture (GL_TEXTURE_2D, previous);
    return pixels;
}

GLint integer (GLenum name) {
    GLint value = 0;
    glGetIntegerv (name, &value);
    return value;
}
} // namespace

TEST_CASE ("Covers upload whatever unpack state the frame left, which stays as it was") {
    SurfacelessContext gl;

    if (!gl.ready) {
	SKIP ("no surfaceless EGL context here");
    }

    GLuint textures[3] {};
    glGenTextures (3, textures);

    // state left behind by another upload
    GLuint unpackBuffer = 0;
    glGenBuffers (1, &unpackBuffer);
    glBindBuffer (GL_PIXEL_UNPACK_BUFFER, unpackBuffer);
    glBufferData (GL_PIXEL_UNPACK_BUFFER, 1024, nullptr, GL_STATIC_DRAW);
    glPixelStorei (GL_UNPACK_ALIGNMENT, 8);
    glPixelStorei (GL_UNPACK_ROW_LENGTH, 9);
    glPixelStorei (GL_UNPACK_SKIP_ROWS, 1);
    glPixelStorei (GL_UNPACK_SKIP_PIXELS, 2);
    glBindTexture (GL_TEXTURE_2D, textures[2]);

    const WallpaperEngine::Media::CoverImage cyan { .width = 2,
						    .height = 1,
						    .rgba = { 20, 220, 240, 255, 1, 2, 3, 4 } };
    const WallpaperEngine::Media::CoverImage orange { .width = 2,
						      .height = 1,
						      .rgba = { 240, 50, 20, 255, 5, 6, 7, 8 } };

    WallpaperEngine::Render::uploadCoverTexture (textures[0], cyan);
    WallpaperEngine::Render::uploadCoverTexture (textures[1], cyan);

    CHECK (integer (GL_PIXEL_UNPACK_BUFFER_BINDING) == static_cast<GLint> (unpackBuffer));
    CHECK (integer (GL_UNPACK_ALIGNMENT) == 8);
    CHECK (integer (GL_UNPACK_ROW_LENGTH) == 9);
    CHECK (integer (GL_UNPACK_SKIP_ROWS) == 1);
    CHECK (integer (GL_UNPACK_SKIP_PIXELS) == 2);
    CHECK (integer (GL_TEXTURE_BINDING_2D) == static_cast<GLint> (textures[2]));

    CHECK (firstPixels (textures[0]) == std::array<uint8_t, 8> { 20, 220, 240, 255, 1, 2, 3, 4 });

    WallpaperEngine::Render::uploadCoverTexture (textures[0], orange);

    CHECK (firstPixels (textures[0]) == std::array<uint8_t, 8> { 240, 50, 20, 255, 5, 6, 7, 8 });
    CHECK (firstPixels (textures[1]) == std::array<uint8_t, 8> { 20, 220, 240, 255, 1, 2, 3, 4 });
    CHECK (glGetError () == GL_NO_ERROR);

    glBindBuffer (GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei (GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei (GL_UNPACK_SKIP_PIXELS, 0);
    glDeleteBuffers (1, &unpackBuffer);
    glDeleteTextures (3, textures);
}
