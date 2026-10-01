#pragma once

#include <set>
#include <unordered_map>

#include <functional>
#include <glm/gtc/type_ptr.hpp>
#include <utility>

#include "../../TextureProvider.h"
#include "WallpaperEngine/Data/Model/Material.h"
#include "WallpaperEngine/Render/CFBO.h"
#include "WallpaperEngine/Render/FBOProvider.h"
#include "WallpaperEngine/Render/Helpers/ContextAware.h"
#include "WallpaperEngine/Render/Shaders/Shader.h"
#include "WallpaperEngine/Render/Shaders/Variables/ShaderVariable.h"

namespace WallpaperEngine::Render::Objects {
class CRenderable;
}

namespace WallpaperEngine::Render::Objects::Effects {
using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Render::Shaders::Variables;
using namespace WallpaperEngine::Data::Model;

class CPass final : public Helpers::ContextAware {
public:
    CPass (
	CRenderable& renderable, std::shared_ptr<const FBOProvider> fboProvider, const MaterialPass& pass,
	std::optional<std::reference_wrapper<const ImageEffectPassOverride>> override,
	std::optional<std::reference_wrapper<const TextureMap>> binds,
	std::optional<std::reference_wrapper<std::string>> target
    );
    ~CPass ();

    void render ();
    /** Binds the destination and clears it to transparent black without drawing anything */
    void clearDestination () const;
    /** Advances video textures this pass pulls in from user/material slots, the image's own texture is updated by the
     * scene */
    void updatePlaybackTextures () const;

    void setDestination (std::shared_ptr<const CFBO> drawTo);
    void setInput (std::shared_ptr<const TextureProvider> input);
    void setPreviousInput (std::shared_ptr<const TextureProvider> input);
    /** Binds a texture to g_Texture<index> ahead of whatever the material or shader names for that slot */
    void setTexture (int index, std::shared_ptr<const TextureProvider> texture);
    void setTexCoord (GLuint texcoord);
    void setPosition (GLuint position);
    void setModelViewProjectionMatrix (const glm::mat4* projection);
    /** where the pass's geometry ends up on screen, when that isn't its own MVP (effect passes draw into buffers) */
    void setEffectModelViewProjectionMatrix (const glm::mat4* projection);
    void setModelViewProjectionMatrixInverse (const glm::mat4* projection);
    void setModelMatrix (const glm::mat4* model);
    /** g_LayerModelMatrix, the layer's world matrix (WE fills it for every pass of an image, sub_1401EBF60) */
    void setLayerModelMatrix (const glm::mat4* model);
    /** The pass draws in WE's world space (the fog composite of image layers), fog measures from WE's own eye then */
    void setFogWorld (bool world);
    void setViewProjectionMatrix (const glm::mat4* viewProjection);
    /**
     * Where the vertices of a lit pass really are in the scene (y up, WE's coordinates). Lit passes are built with
     * PRELIGHTING so lighting goes through these while the pass keeps drawing with its own projection
     */
    void setLightingTransform (const glm::mat4* model, const glm::mat3* normal, const glm::mat4* viewProjection);
    void setEffectTextureProjectionMatrix (const glm::mat4* projection, const glm::mat4* inverse);
    /** What a private destination is cleared to before drawing (transparent black when unset) */
    void setClearColor (const glm::vec4* color);
    /** Draw over what a private destination already holds instead of clearing it first */
    void setKeepDestination (bool keep);
    /** Take the matrices of a passthrough layer drawing its children while drawing into a private destination, for
     *  passes that sample the scene at their on-screen position (text's clearalpha fill, sub_140257C30) */
    void setFollowLayerTarget (bool follow);
    /** g_Color4 stays opaque white, for composites of a buffer that already carries the object's color and alpha */
    void setNeutralColor (bool neutral);
    void setBlendingMode (BlendingMode blendingmode);
    [[nodiscard]] BlendingMode getBlendingMode () const;
    [[nodiscard]] std::shared_ptr<const CFBO> resolveFBO (const std::string& name) const;
    [[nodiscard]] std::shared_ptr<const TextureProvider> resolveNamedTexture (const std::string& name) const;

    [[nodiscard]] std::shared_ptr<const FBOProvider> getFBOProvider () const;
    [[nodiscard]] const CRenderable& getRenderable () const;
    [[nodiscard]] const MaterialPass& getPass () const;
    [[nodiscard]] std::optional<std::reference_wrapper<std::string>> getTarget () const;
    [[nodiscard]] const std::shared_ptr<const CFBO>& getDestination () const { return this->m_drawTo; }
    [[nodiscard]] Render::Shaders::Shader* getShader () const;
    [[nodiscard]] GLuint getProgramID () const;
    /** The shader has a geometry stage (GS_ENABLED and a .geom, WE's particle shaders) */
    [[nodiscard]] bool hasGeometryStage () const;

    // Custom geometry rendering support (for particles, etc.)
    using GeometryCallback = std::function<void ()>;
    void
    setGeometryCallback (GeometryCallback setupAttribs, GeometryCallback drawGeometry, GeometryCallback cleanupAttribs);

    // Public uniform setters for external callers (pointer-based, updated per-frame)
    void addUniform (const std::string& name, const float* value, int count = 1);
    void addUniform (const std::string& name, const glm::vec3* value);
    void addUniform (const std::string& name, const glm::vec4* value);
    void addUniform (const std::string& name, const glm::mat3* value);
    void addUniform (const std::string& name, const glm::mat4* value);

private:
    struct TextureChainEntry {
	std::shared_ptr<const TextureProvider> texture;
	std::shared_ptr<TextureChainEntry> next;
    };

    enum UniformType {
	Float = 0,
	Matrix3 = 1,
	Matrix4 = 2,
	Integer = 3,
	Vector2 = 4,
	Vector3 = 5,
	Vector4 = 6,
	Double = 7
    };

    class UniformEntry {
    public:
	UniformEntry (
	    const GLint id, std::string name, UniformType type, const void* value, int count, bool owned = false
	) : id (id), name (std::move (name)), type (type), value (value), count (count), owned (owned) { }
	~UniformEntry ();

	const GLint id;
	std::string name;
	UniformType type;
	const void* value;
	int count;
	// true when "value" is a heap copy this entry allocated (addUniform(name, type, T) by-value
	// overload) rather than a pointer into a scene/shader-owned field (addUniform(..., T*, count)),
	// so only owned entries may free their value.
	bool owned;
    };

    class ReferenceUniformEntry {
    public:
	ReferenceUniformEntry (const GLint id, std::string name, UniformType type, const void** value) :
	    id (id), name (std::move (name)), type (type), value (value) { }

	const GLint id;
	std::string name;
	UniformType type;
	const void** value;
    };

    class AttribEntry {
    public:
	AttribEntry (const GLint id, std::string name, GLint type, GLint elements, const GLuint* value) :
	    id (id), name (std::move (name)), type (type), elements (elements), value (value) { }

	const GLint id;
	std::string name;
	GLint type;
	GLint elements;
	const GLuint* value;
    };

    struct TextureAnimationState {
	uint32_t currentTexture = 0;
	glm::vec2 translation = { 0.0f, 0.0f };
	glm::vec4 rotation = { 0.0f, 0.0f, 0.0f, 0.0f };
    };

    static GLuint compileShader (const char* shader, GLuint type);
    static GLuint linkProgram (
	const std::string& vertex, const std::string& fragment, const std::string& geometry,
	const std::string& shaderName
    );

    struct SharedProgram {
	GLuint program;
	int users;
    };
    /** Linked programs by vertex + fragment source, shared by every pass built from the same sources */
    static std::unordered_map<std::string, SharedProgram>& sharedPrograms ();
    /** Drops this pass's use of a shared program, false if its program isn't one */
    bool releaseSharedProgram ();

    /** A parsed shader plus the GLSL it translates to. Owns copies of what the shader units reference */
    struct CompiledShader {
	ComboMap combos;
	ComboMap overrideCombos;
	TextureMap passTextures;
	TextureMap overrideTextures;
	std::unique_ptr<Render::Shaders::Shader> shader;
	std::string vertex;
	std::string fragment;
	std::string geometry;
    };
    /** Compiled shaders by their inputs, alive while a pass uses them */
    static std::unordered_map<std::string, std::weak_ptr<CompiledShader>>& sharedShaders ();
    std::shared_ptr<CompiledShader> compileShaderSources (
	const std::string& shaderName, const TextureMap& passTextures, const TextureMap& overrideTextures
    );
    void setupShaders ();
    /** Sets TEX<slot>FORMAT for the shader's formatcombo samplers, true if a combo changed */
    bool applyFormatCombos (const TextureMap& passTextures, const TextureMap& overrideTextures);
    void refreshRenderableUniforms ();
    void setupShaderVariables ();
    /** GL type the linked program declares for a uniform (GL_NONE if it has no such active uniform) */
    [[nodiscard]] GLenum getDeclaredUniformType (const std::string& name) const;
    void setupUniforms ();
    void setupTextureUniforms ();
    void setupAttributes ();
    void addAttribute (const std::string& name, GLint type, GLint elements, const GLuint* value);
    void addUniform (ShaderVariable* value);
    void addUniform (const ShaderVariable* value, const DynamicValue* setting);
    void addUniform (const std::string& name, int value);
    void addUniform (const std::string& name, double value);
    void addUniform (const std::string& name, float value);
    void addUniform (const std::string& name, glm::vec2 value);
    void addUniform (const std::string& name, glm::vec3 value);
    void addUniform (const std::string& name, glm::vec4 value);
    void addUniform (const std::string& name, const glm::mat3& value);
    void addUniform (const std::string& name, glm::mat4 value);
    void addUniform (const std::string& name, const int* value, int count = 1);
    void addUniform (const std::string& name, const double* value, int count = 1);
    void addUniform (const std::string& name, const glm::vec2* value);
    void addUniform (const std::string& name, const int** value);
    void addUniform (const std::string& name, const double** value);
    void addUniform (const std::string& name, const float** value);
    void addUniform (const std::string& name, const glm::vec2** value);
    void addUniform (const std::string& name, const glm::vec3** value);
    void addUniform (const std::string& name, const glm::vec4** value);
    void addUniform (const std::string& name, const glm::mat3** value);
    void addUniform (const std::string& name, const glm::mat4** value);
    template <typename T> void addUniform (const std::string& name, UniformType type, T value);
    template <typename T> void addUniform (const std::string& name, UniformType type, T* value, int count = 1);
    template <typename T> void addUniform (const std::string& name, UniformType type, T** value);

    void setupRenderFramebuffer () const;
    void setupRenderTexture ();
    [[nodiscard]] std::shared_ptr<const TextureProvider> resolveTexture0 ();
    [[nodiscard]] TextureAnimationState
    resolveTextureAnimationState (const std::shared_ptr<const TextureProvider>& texture) const;
    void bindTextureUnit (int index, const std::shared_ptr<const TextureProvider>& texture, uint32_t frame) const;
    void bindTextureOverrides (uint32_t currentTexture, std::shared_ptr<const TextureProvider>& texture0) const;
    void setupRenderUniforms ();
    void setupRenderReferenceUniforms ();
    void setupRenderAttributes () const;
    void renderGeometry () const;
    void cleanupRenderSetup ();

    std::shared_ptr<const TextureProvider> resolveTexture (
	std::shared_ptr<const TextureProvider> expected, int index,
	std::shared_ptr<const TextureProvider> previous = nullptr
    );

    /**
     * "usertextures" entries in material/effect JSON are the *name* of a "scenetexture" general
     * property, not a texture path - the actual texture to use is whatever the user configured that
     * property to (which is commonly left unset). Resolves that indirection.
     *
     * @param propertyName The usertextures entry as it appears in the JSON
     * @return The texture name to resolve, or nullopt if the slot is a known, intentionally unset
     *         user-provided texture (no property value configured)
     */
    [[nodiscard]] std::optional<std::string> resolveUserTextureName (const std::string& propertyName) const;

    CRenderable& m_renderable;
    std::shared_ptr<const FBOProvider> m_fboProvider;
    const MaterialPass& m_pass;
    const TextureMap& m_binds;
    const ImageEffectPassOverride& m_override;
    std::optional<std::reference_wrapper<std::string>> m_target;
    std::map<int, std::shared_ptr<const CFBO>> m_fbos = {};
    std::map<std::string, int> m_combos = {};
    std::vector<AttribEntry*> m_attribs = {};
    std::map<std::string, UniformEntry*> m_uniforms = {};
    const glm::vec4* m_clearColor = nullptr;
    bool m_keepDestination = false;
    bool m_followLayerTarget = false;
    bool m_neutralColor = false;
    // uniforms the pass sets as material or override constants, the renderable values leave these alone
    std::set<std::string> m_constantUniforms;
    std::map<std::string, ReferenceUniformEntry*> m_referenceUniforms = {};
    BlendingMode m_blendingmode = BlendingMode_Normal;
    const glm::mat4* m_modelViewProjectionMatrix;
    const glm::mat4* m_effectModelViewProjectionOverride = nullptr;
    const glm::mat4* m_effectModelViewProjectionMatrix = nullptr;
    const glm::mat4* m_modelViewProjectionMatrixInverse;
    const glm::mat4* m_modelMatrix;
    const glm::mat4* m_layerModelMatrix = &s_identity;
    static const glm::mat4 s_identity;
    const glm::mat4* m_lightingModelMatrix;
    bool m_fogWorld = false;
    const glm::mat3* m_lightingNormalMatrix;
    const glm::mat4* m_lightingViewProjectionMatrix;
    const glm::mat4* m_viewProjectionMatrix;
    const glm::mat4* m_effectTextureProjectionMatrix;
    const glm::mat4* m_effectTextureProjectionMatrixInverse;
    /** The matrices of a scene pass redirected into a passthrough layer's buffer (see CScene::LayerTarget) */
    glm::mat4 m_layerModelViewProjection = glm::mat4 (1.0f);
    glm::mat4 m_layerModelViewProjectionInverse = glm::mat4 (1.0f);
    glm::mat4 m_layerViewProjection = glm::mat4 (1.0f);

    // full xray support: 0.0/1.0 fed to the g_XrayFullReveal uniform injected by patchXrayFullRevealBypass(),
    // updated each frame from state.xray.fullReveal (see render()); m_xrayFullRevealPatched records whether
    // that injection actually found its anchors in the compiled shader, for diagnostics
    float m_xrayFullReveal = 0.0f;
    bool m_xrayFullRevealPatched = false;

    std::map<int, std::shared_ptr<TextureChainEntry>> m_textures = {};
    /** Textures that got their usage count bumped by this pass (starts video playback), released on destruction */
    std::vector<std::shared_ptr<const TextureProvider>> m_playbackTextures = {};

    void trackPlayback (const std::shared_ptr<const TextureProvider>& texture);

    std::shared_ptr<CompiledShader> m_compiled = nullptr;
    Render::Shaders::Shader* m_shader = nullptr;

    std::shared_ptr<const CFBO> m_drawTo = nullptr;
    std::shared_ptr<const TextureProvider> m_input = nullptr;
    std::shared_ptr<const TextureProvider> m_previousInput = nullptr;
    glm::vec4 m_texture0Resolution = {};
    /** g_TextureNTexel values and the resolution each one follows */
    std::map<int, glm::vec4> m_texels = {};
    std::map<int, const glm::vec4*> m_texelSources = {};

    GLuint m_programID;
    /** Key of m_programID in the shared program cache (identical sources link once, see setupShaders) */
    std::string m_programKey;

    GLint g_Texture0Rotation;
    GLint g_Texture0Translation;
    GLuint a_TexCoord;
    GLuint a_Position;
    GLuint m_vao;

    // Custom geometry callbacks (for particles, etc.)
    GeometryCallback m_setupAttribsCallback;
    GeometryCallback m_drawGeometryCallback;
    GeometryCallback m_cleanupAttribsCallback;
};
} // namespace WallpaperEngine::Render::Objects::Effects
