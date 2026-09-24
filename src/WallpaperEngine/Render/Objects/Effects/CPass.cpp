#include "CPass.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <type_traits>
#include <utility>

#include "WallpaperEngine/Desktop/UserShortcut.h"
#include "WallpaperEngine/Render/Helpers/ContextAware.h"

#include "WallpaperEngine/Data/Model/Effect.h"
#include "WallpaperEngine/Data/Model/Material.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"

#include "WallpaperEngine/Render/CFBO.h"
#include "WallpaperEngine/Render/Objects/CImage.h"

#include "WallpaperEngine/Render/Shaders/Variables/ShaderVariable.h"
#include "WallpaperEngine/Render/Shaders/Variables/ShaderVariableFloat.h"
#include "WallpaperEngine/Render/Shaders/Variables/ShaderVariableInteger.h"
#include "WallpaperEngine/Render/Shaders/Variables/ShaderVariableVector2.h"
#include "WallpaperEngine/Render/Shaders/Variables/ShaderVariableVector3.h"
#include "WallpaperEngine/Render/Shaders/Variables/ShaderVariableVector4.h"

#include "WallpaperEngine/Logging/Log.h"

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Render::Objects;

using namespace WallpaperEngine::Render::Shaders::Variables;
using namespace WallpaperEngine::Render::Objects::Effects;

extern float g_Time;
extern float g_Daytime;

CPass::UniformEntry::~UniformEntry () {
    if (!this->owned) {
	return;
    }

    switch (this->type) {
	case Double:
	    delete static_cast<const double*> (this->value);
	    break;
	case Float:
	    delete static_cast<const float*> (this->value);
	    break;
	case Integer:
	    delete static_cast<const int*> (this->value);
	    break;
	case Vector4:
	    delete static_cast<const glm::vec4*> (this->value);
	    break;
	case Vector3:
	    delete static_cast<const glm::vec3*> (this->value);
	    break;
	case Vector2:
	    delete static_cast<const glm::vec2*> (this->value);
	    break;
	case Matrix4:
	    delete static_cast<const glm::mat4*> (this->value);
	    break;
	case Matrix3:
	    delete static_cast<const glm::mat3*> (this->value);
	    break;
    }
}

const TextureMap DEFAULT_BINDS = {};
const ImageEffectPassOverride DEFAULT_OVERRIDE = {};
// objects that don't provide their own layer-to-screen mapping (text, particles) keep the old identity
const glm::mat4 IDENTITY_MATRIX = glm::mat4 (1.0);
const glm::mat3 IDENTITY_MATRIX3 = glm::mat3 (1.0);

namespace {
std::string textureSizeLabel (const std::shared_ptr<const TextureProvider>& texture) {
    if (texture == nullptr) {
	return "<null>";
    }

    return std::to_string (texture->getRealWidth ()) + "x" + std::to_string (texture->getRealHeight ());
}

// shader used by Wallpaper Engine's built-in "X-Ray" interactive effect (effects/xray/effect.json)
const std::string XRAY_EFFECT_SHADER = "effects/xray";

// The xray fragment shader gates its reveal on a projector-style sample of a small halo sprite
// (g_Texture2, e.g. "particle/halo_6") taken around the pointer; g_PointerScale only controls how
// far that sample zooms into the sprite, not any on-screen radius, so there's no uniform value that
// makes it cover the whole screen. Instead this patches the compiled fragment source to add a
// g_XrayFullReveal uniform that bypasses the halo sample entirely, forcing the hidden layer
// (g_Texture1) to blend in everywhere once toggled - see patchXrayFullRevealBypass() below.
const std::string XRAY_MULTIPLY_UNIFORM = "uniform float g_Multiply;";
const std::string XRAY_FULL_REVEAL_UNIFORM_DECL = "uniform float g_Multiply;\nuniform float g_XrayFullReveal;";
const std::string XRAY_BLEND_LINE = "blend *= (blendSample.x * blendSample.y);";
const std::string XRAY_BLEND_LINE_PATCHED = "blend *= mix (blendSample.x * blendSample.y, 1.0, g_XrayFullReveal);";

// Returns false (leaving fragmentSource untouched) if the anchors weren't found, e.g. because a
// different spirv-cross/glslang version formats the compiled output differently - callers should
// treat that as "full xray toggle becomes a no-op" rather than fail the whole shader compile.
bool patchXrayFullRevealBypass (std::string& fragmentSource) {
    const auto blendPos = fragmentSource.find (XRAY_BLEND_LINE);
    const auto uniformPos = fragmentSource.find (XRAY_MULTIPLY_UNIFORM);

    if (blendPos == std::string::npos || uniformPos == std::string::npos) {
	return false;
    }

    // replace the later occurrence first so the earlier one's position stays valid
    fragmentSource.replace (blendPos, XRAY_BLEND_LINE.size (), XRAY_BLEND_LINE_PATCHED);
    fragmentSource.replace (uniformPos, XRAY_MULTIPLY_UNIFORM.size (), XRAY_FULL_REVEAL_UNIFORM_DECL);
    return true;
}
}

CPass::CPass (
    CRenderable& renderable, std::shared_ptr<const FBOProvider> fboProvider, const MaterialPass& pass,
    std::optional<std::reference_wrapper<const ImageEffectPassOverride>> override,
    std::optional<std::reference_wrapper<const TextureMap>> binds,
    std::optional<std::reference_wrapper<std::string>> target
) :
    Helpers::ContextAware (renderable), m_renderable (renderable), m_fboProvider (std::move (fboProvider)),
    m_pass (pass), m_binds (binds.has_value () ? binds.value ().get () : DEFAULT_BINDS),
    m_override (override.has_value () ? override.value ().get () : DEFAULT_OVERRIDE), m_target (target),
    m_blendingmode (pass.blending), m_vao (GL_NONE) {
    this->m_effectTextureProjectionMatrix = &IDENTITY_MATRIX;
    this->m_effectTextureProjectionMatrixInverse = &IDENTITY_MATRIX;
    this->m_lightingModelMatrix = &IDENTITY_MATRIX;
    this->m_lightingNormalMatrix = &IDENTITY_MATRIX3;
    this->m_lightingViewProjectionMatrix = &IDENTITY_MATRIX;
    this->setupShaders ();
    glGenVertexArrays (1, &m_vao);
}

CPass::~CPass () {
    for (const auto& texture : this->m_playbackTextures) {
	texture->decrementUsageCount ();
    }

    for (const auto& value : this->m_uniforms | std::views::values) {
	delete value;
    }

    for (const auto& value : this->m_referenceUniforms | std::views::values) {
	delete value;
    }

    for (const auto* attrib : this->m_attribs) {
	delete attrib;
    }

    // text layers rebuild their passes on every glyph texture resize, so this can't be left to leak
    delete this->m_shader;
    this->m_shader = nullptr;

    glDeleteVertexArrays (1, &m_vao);
    this->m_vao = GL_NONE;

    if (!glIsProgram (this->m_programID)) {
	return; // program already invalid or deleted
    }

    GLint shaderCount = 0;
    glGetProgramiv (this->m_programID, GL_ATTACHED_SHADERS, &shaderCount);

    if (shaderCount > 0) {
	std::vector<GLuint> attachedShaders (shaderCount);
	glGetAttachedShaders (this->m_programID, shaderCount, nullptr, attachedShaders.data ());

	for (GLuint s : attachedShaders) {
	    if (glIsShader (s)) {
		glDeleteShader (s);
	    }
	}
    }

    glDeleteProgram (this->m_programID);
    this->m_programID = 0;
}

std::shared_ptr<const TextureProvider> CPass::resolveTexture (
    std::shared_ptr<const TextureProvider> expected, int index, std::shared_ptr<const TextureProvider> previous
) {
    if (expected == nullptr) {
	if (const auto it = this->m_fbos.find (index); it != this->m_fbos.end ()) {
	    expected = it->second;
	}
    }

    const auto it = this->m_binds.find (index);

    if (it == this->m_binds.end ()) {
	return expected;
    }

    // a bind named "previous" is just another way of telling it to use whatever texture there was already
    if (it->second == "previous") {
	return this->m_previousInput ?: (previous ?: expected);
    }

    return this->resolveFBO (it->second);
}

void CPass::trackPlayback (const std::shared_ptr<const TextureProvider>& texture) {
    // the renderable already counts its own texture, and frame buffers have nothing to play back
    if (texture == nullptr || texture == this->m_renderable.getTexture ()
	|| std::ranges::find (this->m_playbackTextures, texture) != this->m_playbackTextures.end ()) {
	return;
    }

    texture->incrementUsageCount ();
    this->m_playbackTextures.push_back (texture);
}

void CPass::updatePlaybackTextures () const {
    for (const auto& texture : this->m_playbackTextures) {
	texture->update ();
    }
}

std::optional<std::string> CPass::resolveUserTextureName (const std::string& propertyName) const {
    const auto& properties = this->m_renderable.getScene ().getScene ().project.properties;
    const auto it = properties.find (propertyName);

    if (it == properties.end ()) {
	// not actually a property reference, treat it as a literal texture name like before
	return propertyName;
    }

    // an assigned shortcut shows its icon, TextureCache loads it from outside the wallpaper
    if (it->second->is<PropertyUserShortcut> ()) {
	const auto shortcut = Desktop::UserShortcut::parse (it->second->getString ());
	const auto icon = shortcut.has_value () ? shortcut->iconPath () : std::nullopt;

	return icon.has_value () ? std::optional<std::string> ("$usershortcut:" + icon->string ()) : std::nullopt;
    }

    const std::string& value = it->second->getString ();

    if (value.empty ()) {
	// the "scenetexture" property exists but the user hasn't imported an image for it -
	// this is the normal state for most wallpapers that expose this as an optional slot
	return std::nullopt;
    }

    return value;
}

std::shared_ptr<const CFBO> CPass::resolveFBO (const std::string& name) const {
    auto fbo = this->m_fboProvider->find (name);

    if (fbo == nullptr) {
	sLog.exception ("Tried to resolve and FBO without any luck: ", name);
    }

    return fbo;
}

void CPass::setupRenderFramebuffer () const {
    glBindFramebuffer (GL_FRAMEBUFFER, this->m_drawTo->getFramebuffer ());

    // Private per-object FBOs are never cleared elsewhere, so a blending pass would otherwise
    // accumulate stale alpha across frames. The shared scene FBO must not be cleared here though,
    // since it accumulates every object drawn this frame.
    if (this->m_drawTo != this->m_renderable.getScene ().getFBO ()) {
	GLfloat previousClearColor[4] = {};
	glGetFloatv (GL_COLOR_CLEAR_VALUE, previousClearColor);
	glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
	glClear (GL_COLOR_BUFFER_BIT);
	glClearColor (previousClearColor[0], previousClearColor[1], previousClearColor[2], previousClearColor[3]);
    }

    glViewport (0, 0, this->m_drawTo->getRealWidth (), this->m_drawTo->getRealHeight ());

    // the alpha source factor must be GL_ONE, GL_SRC_ALPHA squares every blended pass's alpha and compounds through chained effects
    switch (this->getBlendingMode ()) {
	case BlendingMode_Translucent:
	    glEnable (GL_BLEND);
	    glBlendFuncSeparate (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	    break;
	case BlendingMode_Additive:
	    glEnable (GL_BLEND);
	    glBlendFuncSeparate (GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ONE);
	    break;
	case BlendingMode_Normal:
	    // "Normal" is standard alpha compositing, not a raw replace - GL_ONE/GL_ZERO discarded
	    // the destination outright regardless of source alpha, which broke passes whose source
	    // texture is partially transparent (e.g. unconfigured/placeholder effect textures).
	    // Passes that always output alpha=1 render identically either way.
	    // except into an intermediate target: blending there premultiplies RGB and darkens soft alpha edges in the final pass
	    if (this->m_drawTo == this->m_renderable.getScene ().getFBO ()) {
		glEnable (GL_BLEND);
		glBlendFuncSeparate (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	    } else {
		glDisable (GL_BLEND);
	    }
	    break;
	default:
	    glDisable (GL_BLEND);
	    break;
    }

    switch (this->m_pass.depthtest) {
	case DepthtestMode_Enabled:
	    glEnable (GL_DEPTH_TEST);
	    glDepthFunc (GL_LEQUAL);
	    break;
	case DepthtestMode_Disabled:
	default:
	    glDisable (GL_DEPTH_TEST);
	    break;
    }

    switch (this->m_pass.cullmode) {
	case CullingMode_Normal:
	    glEnable (GL_CULL_FACE);
	    break;

	case CullingMode_Disable:
	default:
	    glDisable (GL_CULL_FACE);
	    break;
    }

    switch (this->m_pass.depthwrite) {
	case DepthwriteMode_Enabled:
	    glDepthMask (true);
	    break;

	case DepthwriteMode_Disabled:
	default:
	    glDepthMask (false);
	    break;
    }
}

void CPass::setupRenderTexture () {
    glUseProgram (this->m_programID);

    auto texture0 = this->resolveTexture0 ();
    const auto animation = this->resolveTextureAnimationState (texture0);

    this->bindTextureUnit (0, texture0, animation.currentTexture);
    this->bindTextureOverrides (animation.currentTexture, texture0);

    if (texture0 != nullptr) {
	this->m_texture0Resolution = *texture0->getResolution ();
    }

    // used in animations when one of the frames is vertical instead of horizontal
    // rotation with translation = origin and end of the image to display
    if (this->g_Texture0Rotation != -1) {
	glUniform4f (
	    this->g_Texture0Rotation, animation.rotation.x, animation.rotation.y, animation.rotation.z,
	    animation.rotation.w
	);
    }
    // this actually picks the origin point of the image from the atlast
    if (this->g_Texture0Translation != -1) {
	glUniform2f (this->g_Texture0Translation, animation.translation.x, animation.translation.y);
    }
}

std::shared_ptr<const TextureProvider> CPass::resolveTexture0 () {
    auto texture0 = this->resolveTexture (this->m_input, 0, this->m_input);
    const auto it = this->m_textures.find (0);

    if (it == this->m_textures.end ()) {
	return texture0;
    }

    auto& chain = it->second;

    do {
	texture0 = chain->texture;

	if (texture0 == nullptr) {
	    if (this->m_previousInput != nullptr && this->m_previousInput->isReady ()) {
		return this->m_previousInput;
	    }

	    if (this->m_input != nullptr && this->m_input->isReady ()) {
		return this->m_input;
	    }
	} else if (texture0->isReady ()) {
	    return texture0;
	}

	chain = chain->next;
    } while (chain != nullptr);

    // got to the end of the chain, use previous input or current input if available
    if (this->m_previousInput != nullptr && this->m_previousInput->isReady ()) {
	return this->m_previousInput;
    }

    // last resort, doesn't matter if the input is ready or not
    return this->m_input;
}

CPass::TextureAnimationState
CPass::resolveTextureAnimationState (const std::shared_ptr<const TextureProvider>& texture) const {
    TextureAnimationState state;

    if (texture == nullptr || !texture->isAnimated ()) {
	return state;
    }

    // scene time like every other animation, so --speed, --disable-animations and pausing apply
    double currentRenderTime = fmod (static_cast<double> (g_Time), this->m_renderable.getAnimationTime ());

    for (const auto& frameCur : texture->getFrames ()) {
	currentRenderTime -= frameCur->frametime;

	if (currentRenderTime > 0.0f) {
	    continue;
	}

	state.currentTexture = frameCur->frameNumber;
	state.translation.x = frameCur->x / texture->getTextureWidth (state.currentTexture);
	state.translation.y = frameCur->y / texture->getTextureHeight (state.currentTexture);

	state.rotation.x = frameCur->width1 / static_cast<float> (texture->getTextureWidth (state.currentTexture));
	state.rotation.y = frameCur->width2 / static_cast<float> (texture->getTextureWidth (state.currentTexture));
	state.rotation.z = frameCur->height2 / static_cast<float> (texture->getTextureHeight (state.currentTexture));
	state.rotation.w = frameCur->height1 / static_cast<float> (texture->getTextureHeight (state.currentTexture));
	break;
    }

    return state;
}

void CPass::bindTextureUnit (int index, const std::shared_ptr<const TextureProvider>& texture, uint32_t frame) const {
    if (texture == nullptr) {
	return;
    }

    glActiveTexture (GL_TEXTURE0 + index);
    glBindTexture (GL_TEXTURE_2D, texture->getTextureID (frame));
}

void CPass::bindTextureOverrides (uint32_t currentTexture, std::shared_ptr<const TextureProvider>& texture0) const {
    for (auto [index, chain] : this->m_textures) {
	auto expectedTexture = chain->texture;

	do {
	    if (expectedTexture == nullptr) {
		if (this->m_previousInput != nullptr && this->m_previousInput->isReady ()) {
		    expectedTexture = this->m_previousInput;
		    break;
		}

		if (this->m_input != nullptr && this->m_input->isReady ()) {
		    expectedTexture = this->m_input;
		    break;
		}
	    } else if (expectedTexture->isReady ()) {
		break;
	    }

	    chain = chain->next;
	    expectedTexture = chain == nullptr ? nullptr : chain->texture;
	} while (chain != nullptr);

	if (expectedTexture == nullptr && this->m_previousInput != nullptr && this->m_previousInput->isReady ()) {
	    expectedTexture = this->m_previousInput;
	}

	if (expectedTexture == nullptr) {
	    expectedTexture = this->m_input;
	}

	this->bindTextureUnit (index, expectedTexture, index == 0 ? currentTexture : 0);

	if (index == 0) {
	    texture0 = expectedTexture;
	}
    }
}

void CPass::setupRenderReferenceUniforms () {
    for (const auto& value : this->m_referenceUniforms | std::views::values) {
	switch (value->type) {
	    case Double:
		glUniform1d (value->id, *static_cast<const double*> (*value->value));
		break;
	    case Float:
		glUniform1f (value->id, *static_cast<const float*> (*value->value));
		break;
	    case Integer:
		glUniform1i (value->id, *static_cast<const int*> (*value->value));
		break;
	    case Vector4:
		glUniform4fv (value->id, 1, glm::value_ptr (*static_cast<const glm::vec4*> (*value->value)));
		break;
	    case Vector3:
		glUniform3fv (value->id, 1, glm::value_ptr (*static_cast<const glm::vec3*> (*value->value)));
		break;
	    case Vector2:
		glUniform2fv (value->id, 1, glm::value_ptr (*static_cast<const glm::vec2*> (*value->value)));
		break;
	    case Matrix4:
		glUniformMatrix4fv (
		    value->id, 1, GL_FALSE, glm::value_ptr (*static_cast<const glm::mat4*> (*value->value))
		);
		break;
	    case Matrix3:
		glUniformMatrix3fv (
		    value->id, 1, GL_FALSE, glm::value_ptr (*static_cast<const glm::mat3*> (*value->value))
		);
		break;
	}
    }
}

void CPass::setupRenderUniforms () {
    for (const auto& value : this->m_uniforms | std::views::values) {
	switch (value->type) {
	    case Double:
		glUniform1dv (value->id, value->count, static_cast<const double*> (value->value));
		break;
	    case Float:
		glUniform1fv (value->id, value->count, static_cast<const float*> (value->value));
		break;
	    case Integer:
		glUniform1iv (value->id, value->count, static_cast<const int*> (value->value));
		break;
	    case Vector4:
		glUniform4fv (value->id, value->count, glm::value_ptr (*static_cast<const glm::vec4*> (value->value)));
		break;
	    case Vector3:
		glUniform3fv (value->id, value->count, glm::value_ptr (*static_cast<const glm::vec3*> (value->value)));
		break;
	    case Vector2:
		glUniform2fv (value->id, value->count, glm::value_ptr (*static_cast<const glm::vec2*> (value->value)));
		break;
	    case Matrix4:
		glUniformMatrix4fv (
		    value->id, value->count, GL_FALSE, glm::value_ptr (*static_cast<const glm::mat4*> (value->value))
		);
		break;
	    case Matrix3:
		glUniformMatrix3fv (
		    value->id, value->count, GL_FALSE, glm::value_ptr (*static_cast<const glm::mat3*> (value->value))
		);
		break;
	}
    }
}

void CPass::setupRenderAttributes () const {
    if (this->m_setupAttribsCallback) {
	this->m_setupAttribsCallback ();
	return;
    }

    for (const auto& cur : this->m_attribs) {
	glEnableVertexAttribArray (cur->id);
	glBindBuffer (GL_ARRAY_BUFFER, *cur->value);
	glVertexAttribPointer (cur->id, cur->elements, cur->type, GL_FALSE, 0, nullptr);

#if !NDEBUG
	glObjectLabel (
	    GL_BUFFER, *cur->value, -1,
	    ("Image " + std::to_string (this->m_renderable.getId ()) + " Pass " + this->m_pass.shader + " " + cur->name)
		.c_str ()
	);
#endif /* DEBUG */
    }
}

void CPass::renderGeometry () const {
    if (this->m_drawGeometryCallback) {
	this->m_drawGeometryCallback ();
	return;
    }

    glBindBuffer (GL_ARRAY_BUFFER, this->a_Position);
    glDrawArrays (GL_TRIANGLES, 0, 6);
}

void CPass::cleanupRenderSetup () {
    if (this->m_cleanupAttribsCallback) {
	this->m_cleanupAttribsCallback ();
    } else {
	// disable vertex attribs array
	for (const auto& cur : this->m_attribs) {
	    glDisableVertexAttribArray (cur->id);
	}
    }

    glActiveTexture (GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, 0);

    for (const auto& index : this->m_textures | std::views::keys) {
	glActiveTexture (GL_TEXTURE0 + index);
	glBindTexture (GL_TEXTURE_2D, 0);
    }
}

void CPass::refreshRenderableUniforms () {
    const auto update = [this] (const char* name, const auto& value) {
	const auto it = this->m_uniforms.find (name);

	if (it != this->m_uniforms.end () && it->second->owned) {
	    using Value = std::remove_cvref_t<decltype (value)>;
	    *static_cast<Value*> (const_cast<void*> (it->second->value)) = value;
	}
    };

    update ("g_UserAlpha", this->m_renderable.getUserAlpha ());
    update ("g_Alpha", this->m_renderable.getAlpha ());
    update ("g_Color", this->m_renderable.getColor ());
    update ("g_Color4", this->m_renderable.getColor4 ());
}

void CPass::render () {
    glBindVertexArray (this->m_vao);
    // copied when the pass was built, color and alpha scripts or animations change them every frame
    this->refreshRenderableUniforms ();

    if (this->m_pass.shader == XRAY_EFFECT_SHADER) {
	const bool fullReveal = this->getContext ().getApp ().getContext ().state.xray.fullReveal;
	this->m_xrayFullReveal = fullReveal ? 1.0f : 0.0f;
    }

    const auto& debug = this->getContext ().getApp ().getContext ().settings.render.debug;
    if (debug.passLog) {
	sLog.out (
	    "Render pass object=", this->m_renderable.getId (), " shader=", this->m_pass.shader,
	    " target=", this->m_target.has_value () ? this->m_target.value ().get () : std::string ("<screen/local>"),
	    " drawTo=", this->m_drawTo ? this->m_drawTo->getName () : std::string ("<null>"),
	    " drawSize=", textureSizeLabel (this->m_drawTo), " inputSize=", textureSizeLabel (this->m_input)
	);
	for (const auto* uniformName : { "g_TintColor", "g_CompositeColor", "g_BlendAlpha", "g_CompositeAlpha" }) {
	    const auto uniform = this->m_uniforms.find (uniformName);
	    if (uniform == this->m_uniforms.end ()) {
		continue;
	    }

	    switch (uniform->second->type) {
		case Vector3:
		    {
			const auto* v = static_cast<const glm::vec3*> (uniform->second->value);
			sLog.out ("  uniform ", uniformName, "=", v->x, " ", v->y, " ", v->z);
			break;
		    }
		case Float:
		    {
			const auto* v = static_cast<const float*> (uniform->second->value);
			sLog.out ("  uniform ", uniformName, "=", *v);
			break;
		    }
		default:
		    break;
	    }
	}
    }

    if (this->m_drawTo == nullptr) {
	sLog.error ("Skipping render pass for object ", this->m_renderable.getId (), ": no destination FBO set");
	return;
    }

    if (this->m_input == nullptr) {
	sLog.error ("Skipping render pass for object ", this->m_renderable.getId (), ": no input texture set");
	return;
    }

    this->setupRenderFramebuffer ();
    this->setupRenderTexture ();
    this->setupRenderUniforms ();
    this->setupRenderReferenceUniforms ();

    this->setupRenderAttributes ();
    this->renderGeometry ();
    this->cleanupRenderSetup ();
}

std::shared_ptr<const FBOProvider> CPass::getFBOProvider () const { return this->m_fboProvider; }

const CRenderable& CPass::getRenderable () const { return this->m_renderable; }

void CPass::setDestination (std::shared_ptr<const CFBO> drawTo) { this->m_drawTo = std::move (drawTo); }

void CPass::setInput (std::shared_ptr<const TextureProvider> input) { this->m_input = std::move (input); }

void CPass::setPreviousInput (std::shared_ptr<const TextureProvider> input) {
    this->m_previousInput = std::move (input);
}

void CPass::setModelViewProjectionMatrix (const glm::mat4* projection) {
    this->m_modelViewProjectionMatrix = projection;
}

void CPass::setModelViewProjectionMatrixInverse (const glm::mat4* projection) {
    this->m_modelViewProjectionMatrixInverse = projection;
}

void CPass::setModelMatrix (const glm::mat4* model) { this->m_modelMatrix = model; }

void CPass::setViewProjectionMatrix (const glm::mat4* viewProjection) { this->m_viewProjectionMatrix = viewProjection; }

void CPass::setLightingTransform (const glm::mat4* model, const glm::mat3* normal, const glm::mat4* viewProjection) {
    this->m_lightingModelMatrix = model;
    this->m_lightingNormalMatrix = normal;
    this->m_lightingViewProjectionMatrix = viewProjection;
}

void CPass::setEffectTextureProjectionMatrix (const glm::mat4* projection, const glm::mat4* inverse) {
    this->m_effectTextureProjectionMatrix = projection;
    this->m_effectTextureProjectionMatrixInverse = inverse;
}

void CPass::setBlendingMode (BlendingMode blendingmode) { this->m_blendingmode = blendingmode; }

BlendingMode CPass::getBlendingMode () const { return this->m_blendingmode; }

void CPass::setTexCoord (GLuint texcoord) { this->a_TexCoord = texcoord; }

void CPass::setPosition (GLuint position) { this->a_Position = position; }

const MaterialPass& CPass::getPass () const { return this->m_pass; }

std::optional<std::reference_wrapper<std::string>> CPass::getTarget () const { return this->m_target; }

Render::Shaders::Shader* CPass::getShader () const { return this->m_shader; }

GLuint CPass::getProgramID () const { return this->m_programID; }

void CPass::setGeometryCallback (
    GeometryCallback setupAttribs, GeometryCallback drawGeometry, GeometryCallback cleanupAttribs
) {
    this->m_setupAttribsCallback = std::move (setupAttribs);
    this->m_drawGeometryCallback = std::move (drawGeometry);
    this->m_cleanupAttribsCallback = std::move (cleanupAttribs);
}

GLuint CPass::compileShader (const char* shader, GLuint type) {
    const GLuint shaderID = glCreateShader (type);

    // Mesa mis-reads a vec3 uniform followed by a float used as vec4(vec3, float), use the g_Color4 the engine already exposes
    std::string patched;

    if (type == GL_FRAGMENT_SHADER) {
	patched = shader;

	const std::string declarations = "uniform vec3 g_Color;\nuniform float g_Alpha;";
	const std::string construction = "vec4(g_Color, g_Alpha)";
	const auto declarationsAt = patched.find (declarations);
	const auto constructionAt = patched.find (construction);

	if (declarationsAt != std::string::npos && constructionAt != std::string::npos) {
	    patched.replace (constructionAt, construction.size (), "g_Color4");
	    patched.replace (declarationsAt, declarations.size (), "uniform vec4 g_Color4;");

	    // only safe when that was the sole use of both
	    // the generated source keeps the original as an "#if 0" block at the end, that doesn't count
	    const size_t codeEnd = std::min (patched.size (), patched.find ("#if 0"));
	    const auto stillUsed = [&patched, codeEnd] (const std::string& name) {
		size_t pos = 0;

		while ((pos = patched.find (name, pos)) != std::string::npos && pos < codeEnd) {
		    const bool wordStart = pos == 0 || !(std::isalnum (static_cast<unsigned char> (patched[pos - 1])) || patched[pos - 1] == '_');
		    const size_t end = pos + name.size ();
		    const bool wordEnd = end >= patched.size () || !(std::isalnum (static_cast<unsigned char> (patched[end])) || patched[end] == '_');

		    if (wordStart && wordEnd) {
			return true;
		    }

		    pos = end;
		}

		return false;
	    };

	    if (stillUsed ("g_Color") || stillUsed ("g_Alpha")) {
		patched = shader;
	    }
	}

	shader = patched.c_str ();
    }

    glShaderSource (shaderID, 1, &shader, nullptr);
    glCompileShader (shaderID);

    GLint result = GL_FALSE;
    int infoLogLength = 0;

    glGetShaderiv (shaderID, GL_COMPILE_STATUS, &result);
    glGetShaderiv (shaderID, GL_INFO_LOG_LENGTH, &infoLogLength);

    if (infoLogLength > 0) {
	const auto logBuffer = new char[infoLogLength + 1];
	memset (logBuffer, 0, infoLogLength + 1);
	glGetShaderInfoLog (shaderID, infoLogLength, nullptr, logBuffer);
	std::stringstream buffer;
	buffer << logBuffer << std::endl << "Compiled source code:" << std::endl << shader;
	delete[] logBuffer;

	if (result == GL_FALSE) {
	    sLog.exception (buffer.str ());
	} else {
	    sLog.error (buffer.str ());
	}
    }

    return shaderID;
}

void CPass::setupShaders () {
    const auto texture0 = this->m_renderable.getTexture ();

    this->m_combos.insert (this->m_pass.combos.begin (), this->m_pass.combos.end ());

    const auto comboEnabled = [this] (const std::string& name) {
	const auto override = this->m_override.combos.find (name);
	if (override != this->m_override.combos.end ()) {
	    return override->second != 0;
	}
	const auto combo = this->m_combos.find (name);
	return combo != this->m_combos.end () && combo->second != 0;
    };

    if (comboEnabled ("LIGHTING") || comboEnabled ("REFLECTION")) {
	this->m_combos.insert_or_assign ("PRELIGHTING", 1);
    }

    // particle shaders read TEX0FORMAT without a formatcombo sampler, the other slots are handled below
    if (texture0 != nullptr) {
	if (texture0->getFormat () == TextureFormat_RG88) {
	    this->m_combos.insert_or_assign ("TEX0FORMAT", 8);
	} else if (texture0->getFormat () == TextureFormat_R8) {
	    this->m_combos.insert_or_assign ("TEX0FORMAT", 9);
	}
    }

    // TODO: review the shader textures here; ones passed to the shader shouldn't be in this list
    // (used later to build the textures)
    // use the combos copied from the pass so it includes the texture format
    const std::string& shaderName
	= this->m_override.shaderOverride.has_value () ? this->m_override.shaderOverride.value () : this->m_pass.shader;

    TextureMap passTextures = this->m_pass.textures;
    for (const auto& [index, propertyName] : this->m_pass.usertextures) {
	// leave the default texture (if any) in place when the user hasn't provided an override,
	// same rule applied when the actual texture chain gets built in setupTextureUniforms()
	if (const auto resolved = this->resolveUserTextureName (propertyName); resolved.has_value ()) {
	    passTextures.insert_or_assign (index, *resolved);
	}
    }

    // same for the object's own user textures, otherwise the combo that enables their slot stays off
    TextureMap overrideTextures = this->m_override.textures;
    for (const auto& [index, propertyName] : this->m_override.usertextures) {
	if (const auto resolved = this->resolveUserTextureName (propertyName); resolved.has_value ()) {
	    overrideTextures.insert_or_assign (index, *resolved);
	}
    }

    this->m_shader = new Render::Shaders::Shader (
	this->m_renderable.getAssetLocator (), shaderName, this->m_combos, this->m_override.combos, passTextures,
	overrideTextures, this->m_override.constants
    );

    // samplers marked "formatcombo" get TEX<slot>FORMAT set to their texture's format, like wallpaper64.exe
    // (sub_14015EC30). Which slots those are is only known once the shader is parsed, so rebuild it when one changed
    if (this->applyFormatCombos (passTextures, overrideTextures)) {
	delete this->m_shader;
	this->m_shader = new Render::Shaders::Shader (
	    this->m_renderable.getAssetLocator (), shaderName, this->m_combos, this->m_override.combos, passTextures,
	    overrideTextures, this->m_override.constants
	);
    }

    auto [vertex, fragment] = Shaders::GLSLContext::get ().toGlsl (this->m_shader->vertex (), this->m_shader->fragment ());

    if (shaderName == XRAY_EFFECT_SHADER) {
	this->m_xrayFullRevealPatched = patchXrayFullRevealBypass (fragment);

	if (!this->m_xrayFullRevealPatched) {
	    sLog.error ("Full xray toggle unavailable: couldn't find the expected reveal blend line in the "
			"compiled effects/xray shader (spirv-cross output format may have changed)");
	}
    }

    const GLuint vertexShaderID = compileShader (vertex.c_str (), GL_VERTEX_SHADER);
    const GLuint fragmentShaderID = compileShader (fragment.c_str (), GL_FRAGMENT_SHADER);
    this->m_programID = glCreateProgram ();
    glAttachShader (this->m_programID, vertexShaderID);
    glAttachShader (this->m_programID, fragmentShaderID);
    glLinkProgram (this->m_programID);
    GLint result = GL_FALSE;
    int infoLogLength = 0;

    glGetProgramiv (this->m_programID, GL_LINK_STATUS, &result);
    glGetProgramiv (this->m_programID, GL_INFO_LOG_LENGTH, &infoLogLength);

    if (infoLogLength > 0) {
	const auto logBuffer = new char[infoLogLength + 1];
	memset (logBuffer, 0, infoLogLength + 1);
	glGetProgramInfoLog (this->m_programID, infoLogLength, nullptr, logBuffer);
	const std::string message = logBuffer;
	delete[] logBuffer;
	if (result == GL_FALSE) {
	    sLog.exception (message);
	} else {
	    sLog.error (message);
	}
    }

#if !NDEBUG
    glObjectLabel (GL_PROGRAM, this->m_programID, -1, shaderName.c_str ());
    glObjectLabel (GL_SHADER, vertexShaderID, -1, (shaderName + ".vert").c_str ());
    glObjectLabel (GL_SHADER, fragmentShaderID, -1, (shaderName + ".frag").c_str ());
#endif /* DEBUG */

    // once linked, the shaders themselves are no longer needed and can be detached/deleted
    glDetachShader (this->m_programID, vertexShaderID);
    glDetachShader (this->m_programID, fragmentShaderID);

    glDeleteShader (vertexShaderID);
    glDeleteShader (fragmentShaderID);

    // bind each g_TextureN sampler to unit N explicitly, the translated GLSL collapses every layout(binding) to 0
    {
	glUseProgram (this->m_programID);
	for (int index = 0; index <= 9; index++) {
	    const std::string name = "g_Texture" + std::to_string (index);
	    const GLint loc = glGetUniformLocation (this->m_programID, name.c_str ());
	    if (loc != -1) {
		glUniform1i (loc, index);
	    }
	}
    }

    // first setup the default values, these will be overwritten by future values
    this->setupShaderVariables ();
    this->setupUniforms ();
    this->setupAttributes ();
    this->g_Texture0Rotation = glGetUniformLocation (this->m_programID, "g_Texture0Rotation");
    this->g_Texture0Translation = glGetUniformLocation (this->m_programID, "g_Texture0Translation");
}

bool CPass::applyFormatCombos (const TextureMap& passTextures, const TextureMap& overrideTextures) {
    const auto& fragment = this->m_shader->getFragment ();
    bool changed = false;

    for (const int slot : fragment.getFormatComboSlots ()) {
	std::shared_ptr<const TextureProvider> texture;

	if (slot == 0) {
	    texture = this->m_renderable.getTexture ();
	} else {
	    std::optional<std::string> name;

	    for (const TextureMap* map : { &overrideTextures, &passTextures, &fragment.getTextures () }) {
		if (const auto it = map->find (slot); it != map->end () && !it->second.empty ()) {
		    name = it->second;
		    break;
		}
	    }

	    if (!name.has_value () || name->starts_with ("_rt_") || name->starts_with ("_alias_")) {
		continue;
	    }

	    try {
		texture = this->getContext ().resolveTexture (*name, this->m_renderable.getScene ().getScene ().project);
	    } catch (const std::exception&) {
		continue;
	    }
	}

	if (texture == nullptr || texture->getFormat () == TextureFormat_UNKNOWN) {
	    continue;
	}

	const std::string combo = "TEX" + std::to_string (slot) + "FORMAT";
	const int format = static_cast<int> (texture->getFormat ());

	if (const auto it = this->m_combos.find (combo); it == this->m_combos.end () || it->second != format) {
	    this->m_combos.insert_or_assign (combo, format);
	    changed = true;
	}
    }

    return changed;
}

void CPass::setupAttributes () {
    this->addAttribute ("a_TexCoord", GL_FLOAT, 2, &this->a_TexCoord);
    this->addAttribute ("a_Position", GL_FLOAT, 3, &this->a_Position);
}

void CPass::setupTextureUniforms () {
    // Vertex shaders don't carry texture info in practice, but check them first anyway;
    // fragment textures are checked after and override/extend the chain.
    for (const auto& [index, textureName] : this->m_shader->getVertex ().getTextures ()) {
	try {
	    auto texture = textureName.find ("_rt_") == 0 || textureName.find ("_alias_") == 0
		? this->resolveFBO (textureName)
		: this->getContext ().resolveTexture (
		    textureName, this->m_renderable.getScene ().getScene ().project
		);

	    // create chain entry
	    this->m_textures[index] = std::make_shared<TextureChainEntry> (TextureChainEntry {
		.texture = texture,
		.next = nullptr,
	    });
	} catch (std::runtime_error& ex) {
	    sLog.error (
		"Cannot resolve texture '", textureName, "' (index=", index, ", object id=",
		this->m_renderable.getId (), ") for fragment shader ", ex.what ()
	    );
	}
    }

    for (const auto& [index, textureName] : this->m_shader->getFragment ().getTextures ()) {
	try {
	    auto texture = textureName.find ("_rt_") == 0 || textureName.find ("_alias_") == 0
		? this->resolveFBO (textureName)
		: this->getContext ().resolveTexture (
		    textureName, this->m_renderable.getScene ().getScene ().project
		);

	    const auto it = this->m_textures.find (index);
	    const auto chain = std::make_shared<TextureChainEntry> (TextureChainEntry {
		.texture = texture,
		.next = it != this->m_textures.end () ? it->second : nullptr,
	    });

	    this->m_textures[index] = chain;

	} catch (std::runtime_error& ex) {
	    sLog.error (
		"Cannot resolve texture '", textureName, "' (index=", index, ", object id=",
		this->m_renderable.getId (), ") for fragment shader ", ex.what ()
	    );
	}
    }

    for (const auto& [index, textureName] : this->m_pass.textures) {
	try {
	    auto texture = textureName.find ("_rt_") == 0 || textureName.find ("_alias_") == 0
		? this->resolveFBO (textureName)
		: this->getContext ().resolveTexture (
		    textureName, this->m_renderable.getScene ().getScene ().project
		);

	    const auto it = this->m_textures.find (index);
	    const auto chain = std::make_shared<TextureChainEntry> (TextureChainEntry {
		.texture = texture,
		.next = it != this->m_textures.end () ? it->second : nullptr,
	    });

	    this->m_textures[index] = chain;

	    if (textureName.find ("_rt_") != 0 && textureName.find ("_alias_") != 0) {
		this->trackPlayback (texture);
	    }
	} catch (std::runtime_error& ex) {
	    sLog.error (
		"Cannot resolve texture '", textureName, "' (index=", index, ", object id=",
		this->m_renderable.getId (), ") for pass ", ex.what ()
	    );
	}
    }

    for (const auto& [index, propertyName] : this->m_pass.usertextures) {
	const auto resolvedName = this->resolveUserTextureName (propertyName);
	if (!resolvedName.has_value ()) {
	    // optional user-provided texture slot, nothing configured - keep whatever the
	    // regular "textures" entry already set for this index (if any)
	    continue;
	}

	const std::string& textureName = *resolvedName;

	try {
	    auto texture = textureName.find ("_rt_") == 0 || textureName.find ("_alias_") == 0
		? this->resolveFBO (textureName)
		: this->getContext ().resolveTexture (
		    textureName, this->m_renderable.getScene ().getScene ().project
		);

	    const auto it = this->m_textures.find (index);
	    const auto chain = std::make_shared<TextureChainEntry> (TextureChainEntry {
		.texture = texture,
		.next = it != this->m_textures.end () ? it->second : nullptr,
	    });

	    this->m_textures[index] = chain;
	    if (textureName.find ("_rt_") != 0 && textureName.find ("_alias_") != 0) {
		this->trackPlayback (texture);
	    }
	} catch (std::runtime_error& ex) {
	    sLog.error (
		"Cannot resolve user texture '", textureName, "' (index=", index, ", object id=",
		this->m_renderable.getId (), ") for pass ", ex.what ()
	    );
	}
    }

    // override any texture
    for (const auto& [index, textureName] : this->m_override.textures) {
	try {
	    auto texture = textureName.find ("_rt_") == 0 || textureName.find ("_alias_") == 0
		? this->resolveFBO (textureName)
		: this->getContext ().resolveTexture (
		    textureName, this->m_renderable.getScene ().getScene ().project
		);

	    const auto it = this->m_textures.find (index);
	    const auto chain = std::make_shared<TextureChainEntry> (TextureChainEntry {
		.texture = texture,
		.next = it != this->m_textures.end () ? it->second : nullptr,
	    });

	    this->m_textures[index] = chain;

	    if (textureName.find ("_rt_") != 0 && textureName.find ("_alias_") != 0) {
		this->trackPlayback (texture);
	    }
	} catch (std::runtime_error& ex) {
	    sLog.error (
		"Cannot resolve texture '", textureName, "' (index=", index, ", object id=",
		this->m_renderable.getId (), ") for override ", ex.what ()
	    );
	}
    }

    for (const auto& [index, propertyName] : this->m_override.usertextures) {
	const auto resolvedName = this->resolveUserTextureName (propertyName);
	if (!resolvedName.has_value ()) {
	    continue;
	}

	const std::string& textureName = *resolvedName;

	try {
	    auto texture = textureName.find ("_rt_") == 0 || textureName.find ("_alias_") == 0
		? this->resolveFBO (textureName)
		: this->getContext ().resolveTexture (
		    textureName, this->m_renderable.getScene ().getScene ().project
		);

	    const auto it = this->m_textures.find (index);
	    const auto chain = std::make_shared<TextureChainEntry> (TextureChainEntry {
		.texture = texture,
		.next = it != this->m_textures.end () ? it->second : nullptr,
	    });

	    this->m_textures[index] = chain;

	    if (textureName.find ("_rt_") != 0 && textureName.find ("_alias_") != 0) {
		this->trackPlayback (texture);
	    }
	} catch (std::runtime_error& ex) {
	    sLog.error (
		"Cannot resolve user texture '", textureName, "' (index=", index, ", object id=",
		this->m_renderable.getId (), ") for override ", ex.what ()
	    );
	}
    }

    // binds are set last as they're the most important to be set
    for (const auto& [index, bind] : this->m_binds) {
	const auto texture = bind == "previous" ? nullptr : this->resolveFBO (bind);
	const auto it = this->m_textures.find (index);
	const auto chain = std::make_shared<TextureChainEntry> (TextureChainEntry {
	    .texture = texture,
	    .next = it != this->m_textures.end () ? it->second : nullptr,
	});

	this->m_textures[index] = chain;
    }

    std::shared_ptr<const TextureProvider> texture = this->resolveTexture (this->m_renderable.getTexture (), 0);
    this->addUniform ("g_Texture0", 0);
    this->addUniform ("g_Texture1", 1);
    this->addUniform ("g_Texture2", 2);
    this->addUniform ("g_Texture3", 3);
    this->addUniform ("g_Texture4", 4);
    this->addUniform ("g_Texture5", 5);
    this->addUniform ("g_Texture6", 6);
    this->addUniform ("g_Texture7", 7);
    this->addUniform ("g_TextureReductionScale", 1.0f);
    this->m_texture0Resolution = *texture->getResolution ();
    this->addUniform ("g_Texture0Resolution", &this->m_texture0Resolution);

    for (const auto& [textureIndex, expectedTexture] : this->m_textures) {
	std::ostringstream namestream;

	namestream << "g_Texture" << textureIndex << "Resolution";

	texture = this->resolveTexture (expectedTexture->texture, textureIndex, texture);
	const glm::vec4* res = texture->getResolution ();

	this->addUniform (namestream.str (), res);
    }

    this->addUniform ("g_Texture0Resolution", &this->m_texture0Resolution);
}

void CPass::setupUniforms () {
    this->setupTextureUniforms ();

    const auto& renderable = this->m_renderable;
    const auto& scene = this->m_renderable.getScene ();
    const auto& sceneData = this->m_renderable.getScene ().getScene ();
    const auto& recorder = this->m_renderable.getScene ().getAudioContext ().getRecorder ();

    // lighting variables
    this->addUniform ("g_LightAmbientColor", sceneData.colors.ambient->value->getVec3 ());
    this->addUniform ("g_LightSkylightColor", sceneData.colors.skylight->value->getVec3 ());
    this->addUniform ("g_LightsPosition", UniformType::Vector3, scene.getLightsPosition (), 4);
    this->addUniform ("g_LightsColorPremultiplied", UniformType::Vector4, scene.getLightsColorPremultiplied (), 3);
    this->addUniform ("g_AltModelMatrix", &this->m_lightingModelMatrix);
    this->addUniform ("g_AltNormalModelMatrix", &this->m_lightingNormalMatrix);
    this->addUniform ("g_AltViewProjectionMatrix", &this->m_lightingViewProjectionMatrix);
    this->addUniform (
	"g_Screen",
	glm::vec3 (
	    scene.getWidth (), scene.getHeight (),
	    static_cast<float> (scene.getWidth ()) / static_cast<float> (std::max (scene.getHeight (), 1))
	)
    );
    // register variables like brightness and alpha with some default value
    this->addUniform ("g_Brightness", renderable.getBrightness ());
    this->addUniform ("g_UserAlpha", renderable.getUserAlpha ());
    this->addUniform ("g_Alpha", renderable.getAlpha ());
    this->addUniform ("g_Color", renderable.getColor ());
    this->addUniform ("g_Color4", renderable.getColor4 ());
    if (!this->m_uniforms.contains ("g_CompositeColor")) {
	this->addUniform ("g_CompositeColor", renderable.getCompositeColor ());
    }
    // add some external variables
    this->addUniform ("g_Time", &g_Time);
    this->addUniform ("g_Daytime", &g_Daytime);
    // add model-view-projection matrix
    this->addUniform ("g_ModelViewProjectionMatrixInverse", &this->m_modelViewProjectionMatrixInverse);
    this->addUniform ("g_ModelViewProjectionMatrix", &this->m_modelViewProjectionMatrix);
    this->addUniform ("g_EffectModelViewProjectionMatrix", &this->m_modelViewProjectionMatrix);
    this->addUniform ("g_ModelMatrix", &this->m_modelMatrix);
    this->addUniform ("g_EffectModelMatrix", &this->m_modelMatrix);
    this->addUniform ("g_NormalModelMatrix", glm::identity<glm::mat3> ());
    this->addUniform ("g_ViewProjectionMatrix", &this->m_viewProjectionMatrix);
    this->addUniform ("g_PointerPosition", scene.getMousePosition ());
    this->addUniform ("g_PointerPositionLast", scene.getMousePositionLast ());
    this->addUniform ("g_ParallaxPosition", scene.getParallaxPosition ());
    this->addUniform ("g_EffectTextureProjectionMatrix", &this->m_effectTextureProjectionMatrix);
    this->addUniform ("g_EffectTextureProjectionMatrixInverse", &this->m_effectTextureProjectionMatrixInverse);
    this->addUniform ("g_TexelSize", glm::vec2 (1.0 / scene.getWidth (), 1.0 / scene.getHeight ()));
    this->addUniform ("g_TexelSizeHalf", glm::vec2 (0.5 / scene.getWidth (), 0.5 / scene.getHeight ()));
    this->addUniform ("g_AudioSpectrum16Left", recorder.audio16, 16);
    this->addUniform ("g_AudioSpectrum16Right", recorder.audio16 + 16, 16);
    this->addUniform ("g_AudioSpectrum32Left", recorder.audio32, 32);
    this->addUniform ("g_AudioSpectrum32Right", recorder.audio32 + 32, 32);
    this->addUniform ("g_AudioSpectrum64Left", recorder.audio64, 64);
    this->addUniform ("g_AudioSpectrum64Right", recorder.audio64 + 64, 64);
}

void CPass::addAttribute (const std::string& name, GLint type, GLint elements, const GLuint* value) {
    const GLint id = glGetAttribLocation (this->m_programID, name.c_str ());

    if (id == -1) {
	return;
    }

    this->m_attribs.emplace_back (new AttribEntry (id, name, type, elements, value));
}

template <typename T> void CPass::addUniform (const std::string& name, UniformType type, T value) {
    GLint id = glGetUniformLocation (this->m_programID, name.c_str ());

    // parameter not found, can be ignored
    if (id == -1) {
	return;
    }

    // frees any previously registered value for this uniform name
    const auto it = this->m_uniforms.find (name);

    if (it != this->m_uniforms.end ()) {
	delete it->second;
    }

    T* newValue = new T (value);

    this->m_uniforms.insert_or_assign (name, new UniformEntry (id, name, type, newValue, 1, true));
}

template <typename T> void CPass::addUniform (const std::string& name, UniformType type, T* value, int count) {
    // this version is used to reference to system variables so things like g_Time works fine
    GLint id = glGetUniformLocation (this->m_programID, name.c_str ());

    // parameter not found, can be ignored
    if (id == -1) {
	return;
    }

    if (const auto it = this->m_uniforms.find (name); it != this->m_uniforms.end ()) {
	delete it->second;
    }

    this->m_uniforms.insert_or_assign (name, new UniformEntry (id, name, type, value, count));
}

template <typename T> void CPass::addUniform (const std::string& name, UniformType type, T** value) {
    // this version is used to reference to system variables so things like g_Time works fine
    const GLint id = glGetUniformLocation (this->m_programID, name.c_str ());

    // parameter not found, can be ignored
    if (id == -1) {
	return;
    }

    if (const auto it = this->m_uniforms.find (name); it != this->m_uniforms.end ()) {
	delete it->second;
    }

    this->m_referenceUniforms.insert_or_assign (
	name, new ReferenceUniformEntry (id, name, type, reinterpret_cast<const void**> (value))
    );
}

void CPass::setupShaderVariables () {
    for (const auto& cur : this->m_shader->getVertex ().getParameters ()) {
	if (!this->m_uniforms.contains (cur->getName ())) {
	    this->addUniform (cur);
	}
    }

    for (const auto& cur : this->m_shader->getFragment ().getParameters ()) {
	if (!this->m_uniforms.contains (cur->getName ())) {
	    this->addUniform (cur);
	}
    }

    // apply material pass constants (e.g. constantshadervalues from the material JSON)
    for (const auto& [name, value] : this->m_pass.constants) {
	const auto [vertex, fragment] = this->m_shader->findParameter (name);

	if (vertex == nullptr && fragment == nullptr) {
	    continue;
	}

	ShaderVariable* var = vertex == nullptr ? fragment : vertex;
	this->addUniform (var, value->value.get ());
    }

    // apply override constants (highest priority, overrides both defaults and pass constants)
    for (const auto& [name, value] : this->m_override.constants) {
	const auto [vertex, fragment] = this->m_shader->findParameter (name);

	if (vertex == nullptr && fragment == nullptr) {
	    continue;
	}

	ShaderVariable* var = vertex == nullptr ? fragment : vertex;
	this->addUniform (var, value->value.get ());
    }

    // bind the full-reveal bypass uniform injected by patchXrayFullRevealBypass() (see setupShaders());
    // a no-op if the patch didn't find its anchors, since the uniform then doesn't exist in the shader
    if (this->m_pass.shader == XRAY_EFFECT_SHADER) {
	this->addUniform ("g_XrayFullReveal", &this->m_xrayFullReveal);
    }
}

void CPass::addUniform (ShaderVariable* value) {
    // delegates to the (ShaderVariable*, DynamicValue*) overload, which handles the casting
    this->addUniform (value, value);
}

void CPass::addUniform (const ShaderVariable* value, const DynamicValue* setting) {
    if (value->is<ShaderVariableFloat> ()) {
	this->addUniform (value->getName (), &setting->getFloat ());
    } else if (value->is<ShaderVariableInteger> ()) {
	this->addUniform (value->getName (), &setting->getInt ());
    } else if (value->is<ShaderVariableVector2> ()) {
	this->addUniform (value->getName (), &setting->getVec2 ());
    } else if (value->is<ShaderVariableVector3> ()) {
	this->addUniform (value->getName (), &setting->getVec3 ());
    } else if (value->is<ShaderVariableVector4> ()) {
	this->addUniform (value->getName (), &setting->getVec4 ());
    } else {
	sLog.error ("Cannot convert setting dynamic value  to ", value->getName (), ". Using default value");
    }
}

void CPass::addUniform (const std::string& name, int value) { this->addUniform (name, UniformType::Integer, value); }

void CPass::addUniform (const std::string& name, const int* value, int count) {
    this->addUniform (name, UniformType::Integer, value, count);
}

void CPass::addUniform (const std::string& name, const int** value) {
    this->addUniform (name, UniformType::Integer, value);
}

void CPass::addUniform (const std::string& name, double value) { this->addUniform (name, UniformType::Double, value); }

void CPass::addUniform (const std::string& name, const double* value, int count) {
    this->addUniform (name, UniformType::Double, value, count);
}

void CPass::addUniform (const std::string& name, const double** value) {
    this->addUniform (name, UniformType::Double, value);
}

void CPass::addUniform (const std::string& name, float value) { this->addUniform (name, UniformType::Float, value); }

void CPass::addUniform (const std::string& name, const float* value, int count) {
    this->addUniform (name, UniformType::Float, value, count);
}

void CPass::addUniform (const std::string& name, const float** value) {
    this->addUniform (name, UniformType::Float, value);
}

void CPass::addUniform (const std::string& name, glm::vec2 value) {
    this->addUniform (name, UniformType::Vector2, value);
}

void CPass::addUniform (const std::string& name, const glm::vec2* value) {
    this->addUniform (name, UniformType::Vector2, value, 1);
}

void CPass::addUniform (const std::string& name, const glm::vec2** value) {
    this->addUniform (name, UniformType::Vector2, value, 1);
}

void CPass::addUniform (const std::string& name, glm::vec3 value) {
    this->addUniform (name, UniformType::Vector3, value);
}

void CPass::addUniform (const std::string& name, const glm::vec3* value) {
    this->addUniform (name, UniformType::Vector3, value, 1);
}

void CPass::addUniform (const std::string& name, const glm::vec3** value) {
    this->addUniform (name, UniformType::Vector3, value);
}

void CPass::addUniform (const std::string& name, const glm::vec4 value) {
    this->addUniform (name, UniformType::Vector4, value);
}

GLenum CPass::getDeclaredUniformType (const std::string& name) const {
    GLint count = 0;
    glGetProgramiv (this->m_programID, GL_ACTIVE_UNIFORMS, &count);

    for (GLint index = 0; index < count; index++) {
	char buffer[256];
	GLsizei length = 0;
	GLint size = 0;
	GLenum type = GL_NONE;

	glGetActiveUniform (this->m_programID, index, sizeof (buffer), &length, &size, &type, buffer);

	if (name == buffer) {
	    return type;
	}
    }

    return GL_NONE;
}

void CPass::addUniform (const std::string& name, const glm::vec4* value) {
    // resolution uniforms are always kept as a vec4 (texture size + real size), but shaders often declare
    // them as a vec2 and glUniform4 on that is a GL error that leaves the uniform at 0 (color_key_plus
    // then divides by a zero resolution and the whole layer comes out NaN)
    switch (this->getDeclaredUniformType (name)) {
	case GL_FLOAT_VEC2:
	    this->addUniform (name, UniformType::Vector2, reinterpret_cast<const glm::vec2*> (value), 1);
	    return;
	case GL_FLOAT_VEC3:
	    this->addUniform (name, UniformType::Vector3, reinterpret_cast<const glm::vec3*> (value), 1);
	    return;
	default:
	    break;
    }

    this->addUniform (name, UniformType::Vector4, value, 1);
}

void CPass::addUniform (const std::string& name, const glm::vec4** value) {
    this->addUniform (name, UniformType::Vector4, value);
}

void CPass::addUniform (const std::string& name, const glm::mat3& value) {
    this->addUniform (name, UniformType::Matrix3, value);
}

void CPass::addUniform (const std::string& name, const glm::mat3* value) {
    this->addUniform (name, UniformType::Matrix3, value, 1);
}

void CPass::addUniform (const std::string& name, const glm::mat3** value) {
    this->addUniform (name, UniformType::Matrix3, value);
}

void CPass::addUniform (const std::string& name, const glm::mat4 value) {
    this->addUniform (name, UniformType::Matrix4, value);
}

void CPass::addUniform (const std::string& name, const glm::mat4* value) {
    this->addUniform (name, UniformType::Matrix4, value, 1);
}

void CPass::addUniform (const std::string& name, const glm::mat4** value) {
    this->addUniform (name, UniformType::Matrix4, value);
}
