#pragma once

#include <GL/glew.h>
#include <glm/mat4x4.hpp>

namespace WallpaperEngine::Render::DepthRange {
/**
 * WE renders its scene with a reversed float depth buffer (1 at the near plane, 0 at the far one, D32F cleared to 0,
 * sub_14009A360). With ARB_clip_control that is what a scene is drawn with here: clip z is mapped to 0..1 while the
 * scene renders (Scope), every matrix handed to a pass has its depth built or remapped that way, and depth tests
 * are the mirrored ones. Without the extension everything stays forward, 0 at the near plane
 */
bool supported ();

class Scope {
public:
    Scope ();
    ~Scope ();
    Scope (const Scope&) = delete;
    Scope& operator= (const Scope&) = delete;
};

glm::mat4 toClip (const glm::mat4& projection);

glm::mat4 perspective (float fovY, float aspect, float near, float far);

GLenum depthFunc (GLenum forward);

GLclampd farDepth ();

float nearClip ();
float farClip ();
float middleClip ();
}
