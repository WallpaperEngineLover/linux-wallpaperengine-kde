#pragma once

#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "WallpaperEngine/Data/Model/Wallpaper.h"

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}

namespace WallpaperEngine::Render {
using namespace WallpaperEngine::Data::Model;

class Camera {
public:
    Camera (Wallpapers::CScene& scene, const SceneData::Camera& camera);
    ~Camera ();

    /**
     * @param width Layout width, what object positions are computed against
     * @param height Layout height
     * @param canvasWidth What actually gets rendered, centered on the layout (0 = same as the layout width)
     * @param canvasHeight Same as canvasWidth, for the height
     */
    void setOrthogonalProjection (float width, float height, float canvasWidth = 0.0f, float canvasHeight = 0.0f);
    /**
     * 3D scenes: the scene camera becomes a perspective one (getPerspective/getView), getProjection and
     * getLookAt turn into a plain screen space camera for the 2D layers (fullscreen post processing, bloom)
     */
    void setPerspectiveProjection (float width, float height);

    [[nodiscard]] const glm::vec3& getCenter () const;
    [[nodiscard]] const glm::vec3& getEye () const;
    /** The scene.json eye, getEye () follows camera objects in 3D scenes */
    [[nodiscard]] const glm::vec3& getConfiguredEye () const;
    [[nodiscard]] const glm::vec3& getUp () const;
    [[nodiscard]] const glm::mat4& getProjection () const;
    [[nodiscard]] const glm::mat4& getLookAt () const;
    [[nodiscard]] Wallpapers::CScene& getScene () const;
    [[nodiscard]] bool isOrthogonal () const;
    [[nodiscard]] bool isPerspective () const;
    /** Perspective projection of a 3D scene, flipped vertically like everything else drawn into the scene buffer */
    [[nodiscard]] const glm::mat4& getPerspective () const;
    [[nodiscard]] const glm::mat4& getView () const;
    [[nodiscard]] float getWidth () const;
    [[nodiscard]] float getHeight () const;
    [[nodiscard]] float getCanvasWidth () const;
    [[nodiscard]] float getCanvasHeight () const;
    [[nodiscard]] float getFov () const;
    [[nodiscard]] float getNearZ () const;
    [[nodiscard]] float getFarZ () const;

    /**
     * Per frame view from a camera object, in WE's world space (y up, origin at the bottom left of the scene).
     * 2D scenes ignore the scene.json eye (WE resets it), so without a camera object this is the identity
     */
    void setWorldView (const glm::mat4& view);
    /** 2D scenes: general zoom times the camera object's zoom, scales around the scene center (sub_14017FA70) */
    void setZoom (float zoom);
    /** 3D scenes: a camera object replaces the scene.json eye and fov */
    void setPerspectiveView (const glm::mat4& view, float fov);
    [[nodiscard]] const glm::mat4& getWorldView () const;
    /**
     * Rebuilds the camera of layers with "perspective" set (sub_1401E5B60) for the part of the scene that ends up
     * on screen: uvs are the output's texture window over the scene buffer (ustart, uend, vstart, vend)
     */
    void updatePerspectiveLayers (const glm::vec4& uvs, float viewportAspect);
    /** View projection of "perspective" layers, same space as getProjection () * getLookAt () */
    [[nodiscard]] const glm::mat4& getPerspectiveLayerViewProjection () const;

private:
    float m_width;
    float m_height;
    /** 3D scenes: where the view puts the eye, the scene.json one or a camera object's */
    glm::vec3 m_eye;
    float m_canvasWidth = 0.0f;
    float m_canvasHeight = 0.0f;
    bool m_isOrthogonal = false;
    bool m_isPerspective = false;
    glm::mat4 m_perspective = {};
    glm::mat4 m_view = {};
    glm::mat4 m_projection = {};
    glm::mat4 m_lookat = {};
    glm::mat4 m_worldView = glm::mat4 (1.0f);
    glm::mat4 m_orthogonal = {};
    float m_zoom = 1.0f;
    glm::mat4 m_perspectiveLayer = {};
    const SceneData::Camera& m_camera;
    Wallpapers::CScene& m_scene;
};
} // namespace WallpaperEngine::Render
