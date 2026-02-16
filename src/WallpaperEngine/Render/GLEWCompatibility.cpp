#include "GLEWCompatibility.h"
#include "WallpaperEngine/Logging/Log.h"
#include <cstdlib>
#include <cstring>

namespace WallpaperEngine::Render {

bool handleGLEWInitialization(GLenum glewInitResult, bool isWayland) {
    if (glewInitResult == GLEW_OK) {
        return true;
    }
    
    // GLEW 2.3.0+ on Wayland may return GLEW_ERROR_NO_GLX_DISPLAY
    // when trying to initialize GLX on a Wayland+EGL system.
    // This error can be safely ignored as the core OpenGL context works.
    if (glewInitResult == GLEW_ERROR_NO_GLX_DISPLAY) {
        if (isWayland || isRunningOnWayland()) {
            sLog.out("GLEW initialization returned GLEW_ERROR_NO_GLX_DISPLAY on Wayland, proceeding...");
            return true;
        } else {
            sLog.error("GLEW initialization failed with GLX display error on non-Wayland system");
            return false;
        }
    }
    
    // For any other errors, log and fail
    sLog.error("Failed to initialize GLEW: ", getGLEWErrorMessage(glewInitResult));
    return false;
}

std::string getGLEWErrorMessage(GLenum error) {
    switch (error) {
        case GLEW_OK:
            return "No error";
        case GLEW_ERROR_NO_GL_VERSION:
            return "Missing GL version";
        case GLEW_ERROR_GL_VERSION_10_ONLY:
            return "OpenGL 1.0 only";
        case GLEW_ERROR_GLX_VERSION_11_ONLY:
            return "GLX 1.1 only";
#ifdef GLEW_ERROR_NO_GLX_DISPLAY
        case GLEW_ERROR_NO_GLX_DISPLAY:
            return "No GLX display";
#endif
        default:
            return "Unknown error";
    }
}

bool isRunningOnWayland() {
    // Check common Wayland environment variables
    const char* wayland_display = std::getenv("WAYLAND_DISPLAY");
    const char* xdg_session_type = std::getenv("XDG_SESSION_TYPE");
    const char* gtk_backend = std::getenv("GTK_BACKEND");
    
    if (wayland_display && std::strlen(wayland_display) > 0) {
        return true;
    }
    
    if (xdg_session_type && std::strcmp(xdg_session_type, "wayland") == 0) {
        return true;
    }
    
    if (gtk_backend && std::strcmp(gtk_backend, "wayland") == 0) {
        return true;
    }
    
    return false;
}

} // namespace WallpaperEngine::Render