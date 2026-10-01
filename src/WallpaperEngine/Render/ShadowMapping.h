#pragma once

#include <GL/glew.h>
#include <glm/glm.hpp>
#include <memory>
#include <vector>

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}

namespace WallpaperEngine::Render {
class CFBO;

/**
 * LightingV1 shadow maps, wallpaper64.exe 2.8.42 sub_140190C80 (atlas layout, per light matrices) and sub_140196530
 * (the caster pass). Every shadow is a square tile of one depth atlas (_rt_shadowAtlas, 32 bit float), rendered with
 * WE's reversed depth (1 at the near plane) so the stored values are exactly what its lit materials compare against
 */
class ShadowMapping {
public:
    /** One shadow of a light: a spot, one cascade of a directional light or the six cube faces of a point light */
    struct Entry {
	/** Direct3D clip space of WE (depth 0..1, reversed), one per viewport */
	glm::mat4 viewProjection[6] = {};
	bool point = false;
	int size = 0;
	/** Where the tile ends up in the atlas, (x, y, size, size) / atlas size, what the lit materials read */
	glm::vec4* transform = nullptr;
	int x = 0;
	int y = 0;
    };

    explicit ShadowMapping (Wallpapers::CScene& scene, std::shared_ptr<const CFBO> atlas);
    ~ShadowMapping ();

    /** Places the entries in the atlas, grows it if needed and draws every shadow caster into their tiles */
    void render (std::vector<Entry>& entries);

    /** Size of one shadow tile for WE's shadow quality 1..4 (sub_14025D3E0) */
    [[nodiscard]] static int tileSize (int quality);
    /** Right handed PerspectiveFov with Direct3D's reversed depth, like WE's device (sub_14009A360) */
    [[nodiscard]] static glm::mat4 perspective (float fovRadians, float aspect, float near, float far);

private:
    void setup ();
    /** The row packing of sub_140190C80, returns the space the tiles take */
    static glm::ivec2 pack (std::vector<Entry>& entries);
    /** viewProjection in GL clip space, direct3D the same one as WE has it (for its culling) */
    void drawCasters (const glm::mat4& viewProjection, const glm::mat4& direct3D) const;

    Wallpapers::CScene& m_scene;
    std::shared_ptr<const CFBO> m_atlas;
    GLuint m_program = GL_NONE;
    GLint m_modelViewProjection = -1;
    GLint m_alphaTest = -1;
};
} // namespace WallpaperEngine::Render
