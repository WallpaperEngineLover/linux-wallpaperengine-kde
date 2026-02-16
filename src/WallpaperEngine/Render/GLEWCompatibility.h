#ifndef WALLPAPERENGINE_RENDER_GLEWCOMPATIBILITY_H
#define WALLPAPERENGINE_RENDER_GLEWCOMPATIBILITY_H

#include <GL/glew.h>
#include <string>

namespace WallpaperEngine::Render {
    // GLEW error codes that may not be defined in older versions
#ifndef GLEW_ERROR_NO_GLX_DISPLAY
#define GLEW_ERROR_NO_GLX_DISPLAY 4
#endif

    /**
     * Handles GLEW initialization with Wayland/EGL compatibility
     * 
     * GLEW 2.3.0+ on Wayland may return GLEW_ERROR_NO_GLX_DISPLAY when it tries
     * to initialize GLX bindings on a Wayland+EGL system. However, the core OpenGL
     * context is properly initialized and can be used, so we need to ignore this
     * specific error for Wayland compatibility.
     * 
     * @param glewInitResult The result from glewInit()
     * @param isWayland Whether we're running on Wayland (true) or X11 (false)
     * @return true if initialization was successful or can proceed, false on fatal error
     */
    bool handleGLEWInitialization(GLenum glewInitResult, bool isWayland = false);

    /**
     * Get a human-readable error message for GLEW error codes
     */
    std::string getGLEWErrorMessage(GLenum error);

    /**
     * Detect if we're running on Wayland
     * This is a simple detection that checks common Wayland indicators
     */
    bool isRunningOnWayland();
}

#endif // WALLPAPERENGINE_RENDER_GLEWCOMPATIBILITY_H