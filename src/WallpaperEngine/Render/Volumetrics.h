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
 * Volumetric light of LightingV1 point and spot lights, wallpaper64.exe 2.8.42 sub_140196CE0 (one light into the light buffer)
 * and sub_140198D00 (blur, then added onto the scene before the next object that isn't a light)
 */
class Volumetrics {
public:
    explicit Volumetrics (Wallpapers::CScene& scene);
    ~Volumetrics ();

    /** Raymarches the light's volume into the light buffer, the first light after a composite clears it. Point
     *  lights use a sphere (their world matrix scaled by the radius), spot lights a cone */
    void renderLight (const Data::Model::Light& light, const glm::mat4& world, const glm::mat4& viewProjection);
    /** Adds the light buffer onto the scene buffer if a light went into it since the last time */
    void composite ();
    /** A spot light's view projection (sub_14025D420), Direct3D depth 0..1, what its volume and cookie use */
    [[nodiscard]] static glm::mat4
    spotViewProjection (const Data::Model::Light& light, const glm::mat4& world, bool orthographic);

private:
    struct Target {
	GLuint texture = GL_NONE;
	GLuint framebuffer = GL_NONE;
    };

    struct Mesh {
	GLuint vao = GL_NONE;
	GLuint vertices = GL_NONE;
	GLuint indices = GL_NONE;
	GLsizei indexCount = 0;
	/** +1 counterclockwise seen from outside, -1 clockwise */
	float winding = 1.0f;
    };

    void setup ();
    static void upload (Mesh& mesh, const std::vector<glm::vec3>& vertices, const std::vector<GLushort>& indices);
    void allocate (glm::ivec2 size);
    void release ();
    void copySceneDepth (GLint framebuffer);

    Wallpapers::CScene& m_scene;
    int m_quality;
    bool m_pending = false;
    glm::ivec2 m_size {};
    Target m_light;
    Target m_lightB;
    Target m_back;
    GLuint m_backDepth = GL_NONE;
    Target m_sceneDepth;
    glm::ivec2 m_sceneDepthSize {};
    Mesh m_sphere;
    Mesh m_cone;
    Mesh m_box;
    Mesh m_fullscreen;
    GLuint m_backProgram = GL_NONE;
    /** [point, spot, cookie spot][shadow][fullscreen] */
    GLuint m_frontPrograms[3][2][2] = {};
    GLuint m_blurProgram = GL_NONE;
    GLuint m_compositeProgram = GL_NONE;
};
} // namespace WallpaperEngine::Render
