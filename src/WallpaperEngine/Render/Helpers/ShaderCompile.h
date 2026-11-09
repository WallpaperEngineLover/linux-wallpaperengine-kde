#pragma once

#include <GL/glew.h>
#include <string_view>

#include "WallpaperEngine/Logging/Log.h"

namespace WallpaperEngine::Render::Helpers {
/** Compiles one stage, a failure is logged under the given name and the shader is returned anyway */
inline GLuint compileLogged (const GLenum type, const char* source, const std::string_view name) {
    const GLuint shader = glCreateShader (type);
    glShaderSource (shader, 1, &source, nullptr);
    glCompileShader (shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv (shader, GL_COMPILE_STATUS, &compiled);

    if (compiled == GL_FALSE) {
	char log[2048] = {};
	glGetShaderInfoLog (shader, sizeof (log) - 1, nullptr, log);
	sLog.error (name, " failed to compile: ", log);
    }

    return shader;
}
}
