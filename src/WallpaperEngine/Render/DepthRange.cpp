#include "DepthRange.h"

#include <cmath>
#include <cstdlib>

#include <glm/gtc/matrix_transform.hpp>

namespace {
int s_scopes = 0;
}

namespace WallpaperEngine::Render::DepthRange {
bool supported () {
    static const bool forward = std::getenv ("LWE_FORWARD_DEPTH") != nullptr;

    return !forward && (GLEW_VERSION_4_5 || GLEW_ARB_clip_control);
}

Scope::Scope () {
    if (!supported () || s_scopes++ > 0) {
	return;
    }

    glClipControl (GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    glClearDepth (0.0);
}

Scope::~Scope () {
    if (!supported () || --s_scopes > 0) {
	return;
    }

    glClipControl (GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);
    glClearDepth (1.0);
}

glm::mat4 toClip (const glm::mat4& projection) {
    if (!supported ()) {
	return projection;
    }

    glm::mat4 result = projection;

    for (int column = 0; column < 4; column++) {
	result[column][2] = 0.5f * (projection[column][3] - projection[column][2]);
    }

    return result;
}

glm::mat4 perspective (const float fovY, const float aspect, const float near, const float far) {
    if (!supported ()) {
	return glm::perspective (fovY, aspect, near, far);
    }

    const double height = 1.0 / std::tan (static_cast<double> (fovY) * 0.5);
    const double n = near;
    const double f = far;
    glm::mat4 projection (0.0f);

    projection[0][0] = static_cast<float> (height / aspect);
    projection[1][1] = static_cast<float> (height);
    projection[2][2] = static_cast<float> (n / (f - n));
    projection[2][3] = -1.0f;
    projection[3][2] = static_cast<float> (n * f / (f - n));

    return projection;
}

GLenum depthFunc (const GLenum forward) {
    if (!supported ()) {
	return forward;
    }

    switch (forward) {
	case GL_LESS:
	    return GL_GREATER;
	case GL_LEQUAL:
	    return GL_GEQUAL;
	case GL_GREATER:
	    return GL_LESS;
	case GL_GEQUAL:
	    return GL_LEQUAL;
	default:
	    return forward;
    }
}

GLclampd farDepth () { return supported () ? 0.0 : 1.0; }

float nearClip () { return supported () ? 1.0f : -1.0f; }

float farClip () { return supported () ? 0.0f : 1.0f; }

float middleClip () { return supported () ? 0.5f : 0.0f; }
}
