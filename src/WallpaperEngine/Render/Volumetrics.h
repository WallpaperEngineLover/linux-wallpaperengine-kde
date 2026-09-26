#pragma once

#include <GL/glew.h>
#include <glm/glm.hpp>
#include <vector>

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}

namespace WallpaperEngine::Data::Model {
class Light;
}

namespace WallpaperEngine::Render {
/**
 * Volumetric light of LightingV1 point lights, wallpaper64.exe 2.8.42 sub_140196CE0 (one light into the light buffer)
 * and sub_140198D00 (blur, then added onto the scene before the next object that isn't a light)
 */
class Volumetrics {
public:
    explicit Volumetrics (Wallpapers::CScene& scene);
    ~Volumetrics ();

    /** Raymarches the light's sphere (its world matrix scaled by the radius) into the light buffer, the first light
     *  after a composite clears it */
    void renderLight (const Data::Model::Light& light, const glm::mat4& world);
    /** Adds the light buffer onto the scene buffer if a light went into it since the last time */
    void composite ();

private:
    struct Target {
	GLuint texture = GL_NONE;
	GLuint framebuffer = GL_NONE;
    };

    void setup ();
    void allocate (glm::ivec2 size);
    void release ();

    Wallpapers::CScene& m_scene;
    int m_quality;
    bool m_pending = false;
    glm::ivec2 m_size {};
    Target m_light;
    Target m_lightB;
    Target m_back;
    GLuint m_backDepth = GL_NONE;
    GLuint m_sphereVertices = GL_NONE;
    GLuint m_sphereIndices = GL_NONE;
    GLsizei m_sphereIndexCount = 0;
    GLuint m_backProgram = GL_NONE;
    GLuint m_frontProgram = GL_NONE;
    GLuint m_frontFullscreenProgram = GL_NONE;
    GLuint m_frontShadowProgram = GL_NONE;
    GLuint m_frontShadowFullscreenProgram = GL_NONE;
    GLuint m_vao = GL_NONE;
    GLuint m_blurProgram = GL_NONE;
    GLuint m_compositeProgram = GL_NONE;
};
} // namespace WallpaperEngine::Render
