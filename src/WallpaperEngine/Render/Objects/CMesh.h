#pragma once

#include "WallpaperEngine/Scripting/ScriptableObject.h"

#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <optional>

namespace WallpaperEngine::Render::Objects {
using namespace WallpaperEngine::Data::Model;

/**
 * A static 3D model ("model" objects), in 3D scenes or placed in a 2D one. Every mesh in the .mdl has its own material
 * and is drawn straight into the scene buffer with depth testing, the bind pose only (no skinning or animation yet)
 */
class CMesh final : public Scripting::ScriptableObject {
public:
    CMesh (Wallpapers::CScene& scene, const Mesh& mesh);
    ~CMesh () override;

    void setup () override;
    void render () override;

    [[nodiscard]] const Mesh& getMesh () const;
    [[nodiscard]] const glm::mat4& getModelMatrix () const;
    [[nodiscard]] const glm::mat3& getNormalMatrix () const;
    [[nodiscard]] const glm::mat4& getViewProjectionMatrix () const;
    [[nodiscard]] const glm::mat4& getModelViewProjectionMatrix () const;
    [[nodiscard]] const glm::mat4& getModelViewProjectionMatrixInverse () const;
    [[nodiscard]] const glm::vec3& getEyePosition () const;
    /** Cursor hit test (sub_140185520): the line through the cursor against the model's bounds in model space, a box
     *  from 0 to the bounds' extent (WE doesn't offset it by the minimum). ndc is in the scene buffer's clip space */
    [[nodiscard]] bool hitTest (const glm::vec2& ndc) const;
    /** A cursor event's localPosition: where the line enters that box, relative to the box's center, zero on a miss */
    [[nodiscard]] glm::vec3 cursorLocalPosition (const glm::vec2& ndc) const;

    class Part;

private:
    /** The model space point where the line through ndc enters the hit box, see hitTest */
    [[nodiscard]] std::optional<glm::vec3> boxEntry (const glm::vec2& ndc) const;
    void updateMatrices ();

    const Mesh& m_mesh;
    std::vector<MaterialUniquePtr> m_materials;
    std::vector<std::unique_ptr<Part>> m_parts;

    glm::mat4 m_modelMatrix = glm::mat4 (1.0f);
    glm::mat3 m_normalMatrix = glm::mat3 (1.0f);
    glm::mat4 m_viewProjection = glm::mat4 (1.0f);
    glm::mat4 m_modelViewProjection = glm::mat4 (1.0f);
    glm::mat4 m_modelViewProjectionInverse = glm::mat4 (1.0f);
    glm::vec3 m_eyePosition = glm::vec3 (0.0f);
    glm::vec3 m_boundsMin = glm::vec3 (0.0f);
    glm::vec3 m_boundsMax = glm::vec3 (0.0f);
};
} // namespace WallpaperEngine::Render::Objects
