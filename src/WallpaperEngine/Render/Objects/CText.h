#pragma once

#include <string>
#include <vector>

#include <GL/glew.h>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include "WallpaperEngine/Render/Objects/CRenderable.h"
#include "WallpaperEngine/Render/Objects/Effects/CPass.h"
#include "WallpaperEngine/Scripting/ScriptEngine.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"

// Forward-declare FreeType types to avoid leaking the header into users.
struct FT_LibraryRec_;
struct FT_FaceRec_;
typedef struct FT_LibraryRec_* FT_Library;
typedef struct FT_FaceRec_* FT_Face;

namespace WallpaperEngine::Render {
class TextureProvider;
}

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}

namespace WallpaperEngine::Render::Objects {
using namespace WallpaperEngine::Data::Model;

/**
 * Renders text objects as a single FreeType-rasterized coverage texture, tinted by
 * the real Wallpaper Engine "font" shader and drawn through the same CRenderable/CPass
 * pipeline CImage uses - so per-object effects (edgedetection, waterripple, ...) apply
 * to text the same way they do to images.
 *
 * Supports scripted/dynamic text (via ScriptableObject's layer scripts) and places the
 * glyph quad within the object's size/padding box according to horizontalalign/verticalalign.
 * Still single-line only - no wrapping. Effects only see the text's own rendered pixels,
 * not the scene behind it (copybackground-style effects are not supported).
 */
class CText final : public CRenderable, public Scripting::ScriptableObject {
    friend CObject;

public:
    CText (Wallpapers::CScene& scene, const Text& text);
    ~CText () override;

    void setup () override;
    void render () override;

    [[nodiscard]] const float& getBrightness () const override;
    [[nodiscard]] const float& getUserAlpha () const override;
    [[nodiscard]] const float& getAlpha () const override;
    [[nodiscard]] const glm::vec3& getColor () const override;
    [[nodiscard]] const glm::vec4& getColor4 () const override;
    [[nodiscard]] const glm::vec3& getCompositeColor () const override;

private:
    void rebuildTextureFrom (const std::string& text);
    void uploadQuadVertices ();
    void buildPasses ();
    void destroyPasses ();

    bool initFreeType ();
    bool loadEmbeddedFont ();
    bool loadSystemFont ();
    void reloadFont ();
    unsigned int computeEffectivePixelSize () const;
    void initScriptLayer ();

    const Text& m_text;
    std::string m_lastRenderedText;
    unsigned int m_lastPixelSize = 0;
    Scripting::ScriptLayerHandle m_layerHandle = Scripting::kInvalidLayerHandle;

    FT_Library m_ftLibrary = nullptr;
    FT_Face m_ftFace = nullptr;
    std::vector<uint8_t> m_fontData;
    /** Backs the scripts' layer.font */
    DynamicValue m_font;
    std::string m_loadedFont;

    std::shared_ptr<TextureProvider> m_glyphTexture;

    // Effect passes run FBO -> FBO at a fixed size (m_quadSize) with an identity matrix like CImage,
    // only the final composite pass needs its own scene-positioned quad/matrix.
    GLuint m_copySpacePosition = 0;
    GLuint m_passSpacePosition = 0;
    GLuint m_sceneSpacePosition = 0;
    GLuint m_texcoordCopy = 0;

    std::vector<Effects::CPass*> m_passes = {};

    std::shared_ptr<const CFBO> m_mainFBO = nullptr;
    std::shared_ptr<const CFBO> m_subFBO = nullptr;
    std::shared_ptr<const CFBO> m_currentMainFBO = nullptr;
    std::shared_ptr<const CFBO> m_currentSubFBO = nullptr;

    glm::mat4 m_modelViewProjectionPass = glm::mat4 (1.0f);
    glm::mat4 m_modelViewProjectionCopy = {};
    glm::mat4 m_modelViewProjectionCopyInverse = {};
    glm::mat4 m_modelViewProjectionScreen = {};
    glm::mat4 m_modelViewProjectionScreenInverse = {};
    glm::mat4 m_modelMatrix = {};
    glm::mat4 m_viewProjectionMatrix = {};

    bool m_textFromProperty = false;
    mutable glm::vec4 m_color4Cache = { 1.0f, 1.0f, 1.0f, 1.0f };

    glm::ivec2 m_textureSize = { 0, 0 };
    glm::vec2 m_quadSize = { 0.0f, 0.0f };
    int m_descender = 0;
    /** the layout box (what alignment uses); the texture can be larger when glyphs overhang it */
    glm::vec2 m_boxSize = { 0.0f, 0.0f };
    /** layout box center minus texture center, in texture pixels (y down) */
    glm::vec2 m_boxShift = { 0.0f, 0.0f };

    bool m_valid = false;
    bool m_initialized = false;
};
} // namespace WallpaperEngine::Render::Objects
