#pragma once

#include "PuppetRig.h"
#include "WallpaperEngine/Render/ModelData.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"

#include <GL/glew.h>

#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <optional>

namespace WallpaperEngine::Render::Objects {
using namespace WallpaperEngine::Data::Model;

struct ParsedModel;

class CMesh final : public Scripting::ScriptableObject, private PuppetRootMotionHost {
public:
    CMesh (Wallpapers::CScene& scene, const Mesh& mesh);
    ~CMesh () override;

    void setup () override;
    void render () override;

    [[nodiscard]] const Mesh& getMesh () const;
    /** Bytes per vertex of an MDL vertex format (sub_1401D9860) */
    [[nodiscard]] static uint32_t vertexStride (uint32_t format);
    [[nodiscard]] static std::optional<uint32_t> vertexComponentOffset (uint32_t format, uint32_t bit);
    /** The skeleton's pose for this frame (sub_14021C480), before cursor events and scripts like every object update */
    void updateAnimation ();
    [[nodiscard]] PuppetRig& getRig () { return this->m_rig; }
    [[nodiscard]] const PuppetRig& getRig () const { return this->m_rig; }
    /** model +736: what g_MorphOffsets multiplies the target indices with */
    [[nodiscard]] uint32_t getMorphVertexCount () const { return this->m_morphVertexCount; }
    [[nodiscard]] int getBoneCount () const { return this->m_boneCount; }
    /** g_Bones: getBoneCount () float4x3 matrices, column major */
    [[nodiscard]] const std::vector<float>& getBones () const { return this->m_bones; }
    // posed bone in model space times the MDAT point's matrix, what attached children multiply with (sub_140224970);
    // exact name match
    [[nodiscard]] std::optional<glm::mat4> getAttachmentMatrix (const std::string& name) const;
    [[nodiscard]] const glm::mat4& getModelMatrix () const;
    [[nodiscard]] const glm::mat3& getNormalMatrix () const;
    [[nodiscard]] const glm::mat4& getViewProjectionMatrix () const;
    [[nodiscard]] const glm::mat4& getModelViewProjectionMatrix () const;
    [[nodiscard]] const glm::mat4& getModelViewProjectionMatrixInverse () const;
    [[nodiscard]] const glm::vec3& getEyePosition () const;
    /** Union of the MDLV 17+ mesh boxes, +-131072 when it has no width */
    [[nodiscard]] const glm::vec3& getBoundsMin () const { return this->m_boundsMin; }
    [[nodiscard]] const glm::vec3& getBoundsMax () const { return this->m_boundsMax; }
    /** Cursor hit test (sub_140185520): the line through the cursor against the model's bounds in model space, a box
     *  from 0 to the bounds' extent (WE doesn't offset it by the minimum). ndc is in the scene buffer's clip space */
    [[nodiscard]] bool hitTest (const glm::vec2& ndc) const;
    /** A cursor event's localPosition: where the line enters that box, relative to the box's center, zero on a miss */
    [[nodiscard]] glm::vec3 cursorLocalPosition (const glm::vec2& ndc) const;
    /** hitBox: first bone box the cursor line enters (sub_140223810), local becomes the entry point in its space */
    [[nodiscard]] std::optional<std::string> cursorHitBox (const glm::vec2& ndc, glm::vec3& local) const;
    // meshes blending "normal" or "alphatocoverage" into the bound shadow map (sub_1402222A0 mode 0);
    // cullViewProjection is the same viewport in WE's clip space for culling
    void renderShadowCaster (
	GLuint program, GLint modelViewProjection, GLint alphaTest, const glm::mat4& viewProjection,
	const glm::mat4& cullViewProjection
    );

    class Part;
    struct MdlMesh;

    /** What setup reads from a model file before it needs the skeleton, so a worker thread can do it ahead of time
     * (CScene::storeParsedModel). Reads nothing but data */
    [[nodiscard]] static std::shared_ptr<ParsedModel> parseModel (const std::vector<char>& data);

private:
    [[nodiscard]] bool rootMotionEnabled () const override;
    [[nodiscard]] glm::mat4 rootMotionWorld () const override;
    void rootMotionMove (const glm::vec3& offset) override;
    [[nodiscard]] glm::vec3 rootMotionAngles () const override;
    void rootMotionTurn (const glm::vec3& angles) override;

    void addPart (MdlMesh mesh, const ModelData::Mesh* source);
    void buildModelDataParts ();

    [[nodiscard]] std::optional<glm::vec3> boxEntry (const glm::vec2& ndc) const;
    /** sub_1402222A0, planes from sub_1401849E0 */
    [[nodiscard]] bool outsideFrustum (const glm::mat4& direct3D) const;
    void updateMatrices ();
    void updateBones ();

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
    /** model +760/+772 (sub_140226A10): visible animation layers' MDLA v5 boxes, else the .mdl bounds */
    glm::vec3 m_cullMin = glm::vec3 (0.0f);
    glm::vec3 m_cullMax = glm::vec3 (0.0f);
    std::shared_ptr<ModelData::Model> m_modelData = nullptr;
    PuppetRig m_rig;
    int m_boneCount = 0;
    uint32_t m_morphVertexCount = 0;
    std::vector<float> m_bones = {};
    uint64_t m_modelStructure = 0;
};
}
