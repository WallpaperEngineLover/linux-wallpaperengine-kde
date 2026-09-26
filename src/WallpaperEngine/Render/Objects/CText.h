#pragma once

#include <string>
#include <vector>

#include <GL/glew.h>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include "WallpaperEngine/Render/Objects/CRenderable.h"
#include "WallpaperEngine/Render/Objects/Effects/CPass.h"
#include "WallpaperEngine/Render/Objects/TextLayout.h"
#include "WallpaperEngine/Scripting/ScriptEngine.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"

namespace WallpaperEngine::Render {
class TextureProvider;
}

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}

namespace WallpaperEngine::Render::Objects {
using namespace WallpaperEngine::Data::Model;

/**
 * Text objects the way wallpaper64.exe 2.8.42 draws them: HarfBuzz shaped glyphs from an atlas (plain coverage, or
 * MSDF when msdf/outline/blur/drop shadow is on) drawn with WE's own materials/fonts materials, an optional opaque
 * background quad, and when the text has effects everything goes through a buffer the size of the text box plus
 * padding first, the way images run their effects.
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
    /** The parts of the text's settings that change which passes exist */
    struct PassLayout {
	bool msdf = false;
	bool outline = false;
	bool blur = false;
	bool dropShadow = false;
	bool background = false;
	/** effects or a blend mode: the text goes through a buffer of its box plus padding first */
	bool buffered = false;
	int blendMode = 0;
	glm::ivec2 bufferSize = { 0, 0 };

	bool operator== (const PassLayout&) const = default;
    };

    [[nodiscard]] TextLayoutParams currentParams () const;
    [[nodiscard]] PassLayout currentPassLayout () const;
    [[nodiscard]] glm::vec2 currentPadding () const;
    [[nodiscard]] glm::vec2 screenAnchorOffset () const;
    void relayout (const std::string& text);
    void uploadGeometry ();
    void updateRenderVars ();
    void buildPasses ();
    void destroyPasses ();
    Effects::CPass* createFontPass (const std::shared_ptr<const CFBO>& destination, const glm::mat4* mvp);

    bool loadFont ();
    void initScriptLayer ();

    const Text& m_text;
    std::string m_lastRenderedText;
    TextLayoutParams m_params;
    TextLayout m_layout;
    TextLayoutResult m_result;
    PassLayout m_passLayout;
    Scripting::ScriptLayerHandle m_layerHandle = Scripting::kInvalidLayerHandle;

    /** Backs the scripts' layer.font */
    DynamicValue m_font;
    std::string m_loadedFont;

    std::shared_ptr<TextureProvider> m_atlas;
    MaterialUniquePtr m_fontMaterial;
    MaterialUniquePtr m_backgroundMaterial;
    MaterialUniquePtr m_clearAlphaMaterial;
    MaterialUniquePtr m_passthroughMaterial;
    ImageEffectPassOverride m_passthroughOverride = { .id = -1 };
    ImageEffectPassOverride m_fontOverride = { .id = -1 };

    GLuint m_glyphPositions = 0;
    GLuint m_glyphTexcoords = 0;
    GLsizei m_glyphVertexCount = 0;
    GLuint m_backgroundPositions = 0;
    // effect passes run FBO -> FBO with an identity matrix like CImage, the composite quad is the buffer's size
    GLuint m_passSpacePosition = 0;
    GLuint m_compositePosition = 0;
    GLuint m_quadTexcoords = 0;

    std::vector<Effects::CPass*> m_passes = {};

    std::shared_ptr<const CFBO> m_mainFBO = nullptr;
    std::shared_ptr<const CFBO> m_subFBO = nullptr;
    std::shared_ptr<const CFBO> m_currentMainFBO = nullptr;
    std::shared_ptr<const CFBO> m_currentSubFBO = nullptr;

    glm::mat4 m_glyphSceneMatrix = glm::mat4 (1.0f);
    glm::mat4 m_glyphSceneMatrixInverse = glm::mat4 (1.0f);
    glm::mat4 m_glyphBufferMatrix = glm::mat4 (1.0f);
    glm::mat4 m_glyphBufferMatrixInverse = glm::mat4 (1.0f);
    glm::mat4 m_compositeMatrix = glm::mat4 (1.0f);
    glm::mat4 m_compositeMatrixInverse = glm::mat4 (1.0f);
    glm::mat4 m_modelViewProjectionPass = glm::mat4 (1.0f);
    glm::mat4 m_modelMatrix = glm::mat4 (1.0f);
    /** Composite quad -> scene, what m_compositeMatrix projects */
    glm::mat4 m_compositeModel = glm::mat4 (1.0f);
    glm::mat4 m_viewProjectionMatrix = glm::mat4 (1.0f);

    glm::vec4 m_renderVars[4] = {};
    glm::vec3 m_backgroundColor = { 0.0f, 0.0f, 0.0f };
    float m_backgroundAlpha = 1.0f;
    glm::vec4 m_backgroundColor4 = { 0.0f, 0.0f, 0.0f, 1.0f };
    const glm::vec4 m_white = { 1.0f, 1.0f, 1.0f, 1.0f };

    bool m_textFromProperty = false;
    mutable glm::vec3 m_colorCache {};
    mutable glm::vec4 m_color4Cache = { 1.0f, 1.0f, 1.0f, 1.0f };

    bool m_initialized = false;
};
} // namespace WallpaperEngine::Render::Objects
