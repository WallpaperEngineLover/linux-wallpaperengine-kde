#pragma once

#include <functional>
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

class CText final : public CRenderable, public Scripting::ScriptableObject {
    friend CObject;

public:
    CText (Wallpapers::CScene& scene, const Text& text);
    ~CText () override;

    /** The fontconfig lookups a text object makes (the fonts WE falls back to, and the match for each of fonts), one
     * call each. They are cached, so running them before the objects are built takes the process spawns off the
     * loading thread. Safe on any thread */
    static std::vector<std::function<void ()>> fontLookups (const std::vector<std::string>& fonts);

    void setup () override;
    void render () override;

    [[nodiscard]] const float& getBrightness () const override;
    [[nodiscard]] const float& getUserAlpha () const override;
    [[nodiscard]] const float& getAlpha () const override;
    [[nodiscard]] const glm::vec3& getColor () const override;
    [[nodiscard]] const glm::vec4& getColor4 () const override;
    [[nodiscard]] const glm::vec3& getCompositeColor () const override;
    [[nodiscard]] const Text& getText () const { return m_text; }
    [[nodiscard]] const glm::mat4& getCompositeMatrix () const { return m_compositeMatrix; }
    [[nodiscard]] bool hitTest (const glm::vec2& ndc);
    [[nodiscard]] glm::vec2 cursorLocalPosition (const glm::vec2& ndc);
    /** sub_140258900: the layout box, plus padding when buffered, 2 x 2 before layout */
    [[nodiscard]] glm::vec2 measuredSize () const;
    /** WE's text world (vtable slot 16, sub_140256E10): the object's world moved by the alignment anchor */
    [[nodiscard]] glm::mat4 worldMatrix () const;

private:
    struct PassLayout {
	bool msdf = false;
	bool outline = false;
	bool blur = false;
	bool dropShadow = false;
	bool background = false;
	bool color = false;
	bool depth = false;
	bool buffered = false;
	/** flag 0x10: blend mode, fog or a dependent object */
	bool passthrough = false;
	int blendMode = 0;
	glm::ivec2 bufferSize = { 0, 0 };

	bool operator== (const PassLayout&) const = default;
    };

    [[nodiscard]] TextLayoutParams currentParams () const;
    [[nodiscard]] PassLayout currentPassLayout () const;
    [[nodiscard]] glm::vec2 currentPadding () const;
    [[nodiscard]] glm::vec2 screenAnchorOffset () const;
    /** Where the alignment puts the text box relative to the origin, in layout units (y up) */
    [[nodiscard]] glm::vec2 alignmentAnchor () const;
    void updateTransform ();
    void updateBufferMatrix ();
    void relayout (const std::string& text);
    void uploadGeometry ();
    void updateRenderVars ();
    void buildPasses ();
    void destroyPasses ();
    Effects::CPass* createFontPass (const std::shared_ptr<const CFBO>& destination, const glm::mat4* mvp, bool color);

    bool loadFont ();
    void initScriptLayer ();

    const Text& m_text;
    std::string m_lastRenderedText;
    TextLayoutParams m_params;
    TextLayout m_layout;
    TextLayoutResult m_result;
    PassLayout m_passLayout;
    Scripting::ScriptLayerHandle m_layerHandle = Scripting::kInvalidLayerHandle;

    DynamicValue m_font;
    std::string m_loadedFont;

    std::shared_ptr<TextureProvider> m_atlas;
    std::shared_ptr<TextureProvider> m_colorAtlas;
    /** MSDF mode: the colour glyphs' colours at the layout's colour scale, g_Texture1 of the colour pass */
    std::shared_ptr<TextureProvider> m_colorTexture;
    MaterialUniquePtr m_fontMaterial;
    MaterialUniquePtr m_colorFontMaterial;
    MaterialUniquePtr m_backgroundMaterial;
    MaterialUniquePtr m_clearAlphaMaterial;
    MaterialUniquePtr m_passthroughMaterial;
    ImageEffectPassOverride m_passthroughOverride = { .id = -1 };
    ImageEffectPassOverride m_fontOverride = { .id = -1 };

    GLuint m_glyphPositions = 0;
    GLuint m_glyphTexcoords = 0;
    GLsizei m_glyphVertexCount = 0;
    GLuint m_colorGlyphPositions = 0;
    GLuint m_colorGlyphTexcoords = 0;
    GLsizei m_colorGlyphVertexCount = 0;
    GLuint m_backgroundPositions = 0;
    GLuint m_passSpacePosition = 0;
    GLuint m_compositePosition = 0;
    GLuint m_quadTexcoords = 0;
    GLuint m_compositeTexcoords = 0;
    /** 0.15 texel in from every edge */
    GLuint m_finalTexcoords = 0;

    std::vector<Effects::CPass*> m_passes = {};
    size_t m_compositePassCount = 0;
    /** another object lists this text in its dependencies: it renders through its buffer, even while hidden */
    bool m_isDependency = false;

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
    glm::mat4 m_compositeModel = glm::mat4 (1.0f);
    glm::mat4 m_viewProjectionMatrix = glm::mat4 (1.0f);

    glm::vec4 m_renderVars[4] = {};
    glm::vec3 m_backgroundColor = { 0.0f, 0.0f, 0.0f };
    float m_backgroundAlpha = 1.0f;
    glm::vec4 m_backgroundColor4 = { 0.0f, 0.0f, 0.0f, 1.0f };
    const glm::vec4 m_white = { 1.0f, 1.0f, 1.0f, 1.0f };

    mutable glm::vec3 m_colorCache {};
    mutable glm::vec4 m_color4Cache = { 1.0f, 1.0f, 1.0f, 1.0f };

    bool m_initialized = false;
};
}
