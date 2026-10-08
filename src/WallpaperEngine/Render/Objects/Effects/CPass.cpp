#include "CPass.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ranges>
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

#include "WallpaperEngine/Render/Shaders/ShaderCache.h"
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

// The xray shader gates its reveal on a halo sprite sample around the pointer, so no uniform can make it cover
// the screen. g_XrayReveal replaces that sample when >= 0: 1 everywhere (full), 0 never (disabled)
const std::string XRAY_MULTIPLY_UNIFORM = "uniform float g_Multiply;";
const std::string XRAY_REVEAL_UNIFORM_DECL = "uniform float g_Multiply;\nuniform float g_XrayReveal;";
const std::string XRAY_BLEND_LINE = "blend *= (blendSample.x * blendSample.y);";
const std::string XRAY_BLEND_LINE_PATCHED
    = "blend *= (g_XrayReveal < 0.0 ? blendSample.x * blendSample.y : g_XrayReveal);";

// false (source untouched) if the anchors weren't found, xray modes then do nothing
bool patchXrayRevealOverride (std::string& fragmentSource) {
    const auto blendPos = fragmentSource.find (XRAY_BLEND_LINE);
    const auto uniformPos = fragmentSource.find (XRAY_MULTIPLY_UNIFORM);

    if (blendPos == std::string::npos || uniformPos == std::string::npos) {
	return false;
    }

    // replace the later occurrence first so the earlier one's position stays valid
    fragmentSource.replace (blendPos, XRAY_BLEND_LINE.size (), XRAY_BLEND_LINE_PATCHED);
    fragmentSource.replace (uniformPos, XRAY_MULTIPLY_UNIFORM.size (), XRAY_REVEAL_UNIFORM_DECL);
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
    this->m_shader = nullptr;
    this->m_compiled = nullptr;

    glDeleteVertexArrays (1, &m_vao);
    this->m_vao = GL_NONE;

    if (this->releaseSharedProgram ()) {
	return;
    }

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

    if (it != properties.end ()) {
	it->second->pin ();
    }

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

std::shared_ptr<const TextureProvider> CPass::resolveNamedTexture (const std::string& name) const {
    // lit materials sample every cookie spot through one alias, the scene's light cookie (sub_140190C80)
    if (name == "_alias_lightCookie") {
	return this->m_renderable.getScene ().getLightCookie ();
    }

    // WE finds only _a of a named layer buffer by name (sub_1401EA500), anything else binds the checker
    // (sub_1400EEF70). A layer's own buffers stay reachable
    constexpr std::string_view compositePrefix = "_rt_imageLayerComposite_";

    if (name.starts_with (compositePrefix)) {
	const auto separator = name.rfind ('_');
	const std::string id = name.substr (compositePrefix.size (), separator - compositePrefix.size ());
	char* end = nullptr;
	const long value = std::strtol (id.c_str (), &end, 10);
	const bool numeric = !id.empty () && end != nullptr && *end == '\0';

	if (numeric && value == this->m_renderable.getId ()) {
	    return this->resolveFBO (name);
	}

	if (!numeric || name.substr (separator + 1) != "a"
	    || !this->m_renderable.getScene ().hasNamedLayerBuffer (static_cast<int> (value))) {
	    return this->m_renderable.getScene ().getMissingTexture ();
	}
    }

    if (name.starts_with ("_rt_") || name.starts_with ("_alias_")) {
	return this->resolveFBO (name);
    }

    return this->getContext ().resolveTexture (name, this->m_renderable.getScene ().getScene ().project);
}

void CPass::bindMissingTexture (int index) {
    // WE's missing texture (sub_1400EEF70)
    const auto it = this->m_textures.find (index);
    this->m_textures[index] = std::make_shared<TextureChainEntry> (TextureChainEntry {
	.texture = this->m_renderable.getScene ().getMissingTexture (),
	.next = it != this->m_textures.end () ? it->second : nullptr,
    });
}

std::shared_ptr<const CFBO> CPass::resolveFBO (const std::string& name) const {
    std::shared_ptr<const CFBO> fbo = this->m_fboProvider->find (name);

    if (fbo == nullptr && name == "_rt_MipMappedFrameBuffer") {
	fbo = this->m_renderable.getScene ().requireMipMappedFrameBuffer ();
    }

    if (fbo == nullptr && name == "_rt_Reflection") {
	fbo = this->m_renderable.getScene ().requireReflectionFrameBuffer ();
    }

    if (fbo == nullptr) {
	sLog.exception ("Tried to resolve and FBO without any luck: ", name);
    }

    return fbo;
}

void CPass::setupRenderFramebuffer () const {
    // what would go onto the scene goes into the passthrough layer drawing its children right now
    const auto* layerTarget = this->m_drawTo == this->m_renderable.getScene ().getFBO ()
	? this->m_renderable.getScene ().getLayerTarget ()
	: nullptr;
    const auto& target = layerTarget != nullptr ? layerTarget->fbo : this->m_drawTo;

    glBindFramebuffer (
	GL_FRAMEBUFFER,
	target == this->m_renderable.getScene ().getFBO () ? this->m_renderable.getScene ().getSceneDrawFramebuffer ()
							   : target->getFramebuffer ()
    );

    // Private per-object FBOs are never cleared elsewhere, so a blending pass would otherwise
    // accumulate stale alpha across frames. The shared scene FBO must not be cleared here though,
    // since it accumulates every object drawn this frame.
    if (this->m_drawTo != this->m_renderable.getScene ().getFBO () && !this->m_keepDestination) {
	GLfloat previousClearColor[4] = {};
	glGetFloatv (GL_COLOR_CLEAR_VALUE, previousClearColor);
	if (this->m_clearColor != nullptr) {
	    glClearColor (this->m_clearColor->r, this->m_clearColor->g, this->m_clearColor->b, this->m_clearColor->a);
	} else {
	    glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
	}
	glColorMask (true, true, true, true);
	glClear (GL_COLOR_BUFFER_BIT);
	glClearColor (previousClearColor[0], previousClearColor[1], previousClearColor[2], previousClearColor[3]);
    }

    glViewport (0, 0, target->getRealWidth (), target->getRealHeight ());

    // D3D11 unbinds a resource that is also the render target, WE reads 0 there (pooled buffer shared with the target).
    // Not for the scene buffer, WE samples a copy
    this->m_boundTargetTexture
	= target == this->m_renderable.getScene ().getFBO () ? GL_NONE : target->getTextureID (0);

    const BlendingMode blending = this->m_scriptBlending.value_or (this->m_blendingmode);
    const auto depth = this->m_scriptDepth.value_or (
	this->m_depthState.value_or (std::make_pair (this->m_pass.depthtest, this->m_pass.depthwrite))
    );

    // WE's blend desc uses the colour factors for alpha too, so translucent leaves alpha squared (sub_140099F60)
    switch (blending) {
	case BlendingMode_Translucent:
	    glEnable (GL_BLEND);
	    glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	    break;
	case BlendingMode_Additive:
	    glEnable (GL_BLEND);
	    glBlendFunc (GL_SRC_ALPHA, GL_ONE);
	    break;
	// WE's blend state builder (sub_140099F60) turns blending off for normal on every target, the scene's
	// alpha is masked off anyway, so the pass's RGB replaces what's below regardless of its alpha
	case BlendingMode_Normal:
	default:
	    glDisable (GL_BLEND);
	    break;
    }

    // AlphaToCoverageEnable only matters on the multisampled scene target (sub_140099F60)
    if (blending == BlendingMode_AlphaToCoverage) {
	glEnable (GL_SAMPLE_ALPHA_TO_COVERAGE);
    }

    // no alpha writes onto the scene target unless the material's alphawriting overrides it (sub_140155FC0)
    const auto& alphaWriting = this->m_pass.alphawriting;
    glColorMask (
	true, true, true,
	alphaWriting == "enabled"
	    || (alphaWriting != "disabled" && this->m_drawTo != this->m_renderable.getScene ().getFBO ())
    );

    // sub_140099F60 with renderer state flag 0x10: alpha op MAX and all four channels written, the color blend stays
    if (layerTarget != nullptr && layerTarget->alphaMax) {
	glBlendEquationSeparate (GL_FUNC_ADD, GL_MAX);
	glColorMask (true, true, true, true);
    }

    switch (depth.first) {
	case DepthtestMode_Enabled:
	    glEnable (GL_DEPTH_TEST);
	    glDepthFunc (GL_LEQUAL);
	    break;
	case DepthtestMode_Disabled:
	default:
	    glDisable (GL_DEPTH_TEST);
	    break;
    }

    switch (this->m_scriptCulling.value_or (this->m_pass.cullmode)) {
	case CullingMode_Normal:
	    glEnable (GL_CULL_FACE);
	    // the mirrored reflection pass flips every triangle's winding
	    glCullFace (this->m_renderable.getScene ().isRenderingReflection () ? GL_FRONT : GL_BACK);
	    break;

	case CullingMode_Disable:
	default:
	    glDisable (GL_CULL_FACE);
	    break;
    }

    switch (depth.second) {
	case DepthwriteMode_Enabled:
	    glDepthMask (true);
	    break;

	case DepthwriteMode_Disabled:
	default:
	    glDepthMask (false);
	    break;
    }

    // WE picks the no-write depth state for translucent and additive blending whatever the material says
    // (sub_140099F60: state index | 1 when the blend mode isn't normal or alphatocoverage)
    if (blending == BlendingMode_Translucent || blending == BlendingMode_Additive) {
	glDepthMask (false);
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

    const auto& frames = texture->getFrames ();
    const auto setFrame = [&state, &texture] (const auto& frameCur) {
	state.currentTexture = frameCur->frameNumber;
	state.translation.x = frameCur->x / texture->getTextureWidth (state.currentTexture);
	state.translation.y = frameCur->y / texture->getTextureHeight (state.currentTexture);

	state.rotation.x = frameCur->width1 / static_cast<float> (texture->getTextureWidth (state.currentTexture));
	state.rotation.y = frameCur->width2 / static_cast<float> (texture->getTextureWidth (state.currentTexture));
	state.rotation.z = frameCur->height2 / static_cast<float> (texture->getTextureHeight (state.currentTexture));
	state.rotation.w = frameCur->height1 / static_cast<float> (texture->getTextureHeight (state.currentTexture));
    };

    // a layer's own texture animation picks the frame, past the end is the first (sub_14015F0D0)
    if (const auto frame = this->m_renderable.getTextureFrameOverride (); frame.has_value () && !frames.empty ()) {
	setFrame (frames[static_cast<size_t> (*frame) < frames.size () ? *frame : 0]);
	return state;
    }

    // scene time like every other animation, so --speed, --disable-animations and pausing apply
    double currentRenderTime = fmod (static_cast<double> (g_Time), this->m_renderable.getAnimationTime ());

    for (const auto& frameCur : frames) {
	currentRenderTime -= frameCur->frametime;

	if (currentRenderTime > 0.0f) {
	    continue;
	}

	setFrame (frameCur);
	break;
    }

    return state;
}

void CPass::bindTextureUnit (int index, const std::shared_ptr<const TextureProvider>& texture, uint32_t frame) const {
    if (texture == nullptr) {
	return;
    }

    // reading the scene buffer while the objects draw multisampled gets what they drew so far, WE resolves the bound
    // target into _rt_FullFrameBuffer (sub_1400D3310)
    if (texture == this->m_renderable.getScene ().getFBO ()) {
	this->m_renderable.getScene ().resolveMultisample ();
    }

    const GLuint id = texture->getTextureID (frame);

    glActiveTexture (GL_TEXTURE0 + index);
    glBindTexture (
	GL_TEXTURE_2D,
	id != GL_NONE && id == this->m_boundTargetTexture ? this->m_renderable.getScene ().getNullTexture () : id
    );
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
    glDisable (GL_SAMPLE_ALPHA_TO_COVERAGE);

    if (this->m_cleanupAttribsCallback) {
	this->m_cleanupAttribsCallback ();
    } else {
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

	if (it != this->m_uniforms.end () && it->second->owned && !this->m_constantUniforms.contains (name)) {
	    using Value = std::remove_cvref_t<decltype (value)>;
	    *static_cast<Value*> (const_cast<void*> (it->second->value)) = value;
	}
    };

    update ("g_UserAlpha", this->m_renderable.getUserAlpha ());
    update ("g_Alpha", this->m_renderable.getAlpha ());
    update ("g_Color", this->m_renderable.getColor ());
    update ("g_Color4", this->m_neutralColor ? glm::vec4 (1.0f) : this->m_renderable.getColor4 ());
}

void CPass::render () {
    glBindVertexArray (this->m_vao);
    // copied when the pass was built, color and alpha scripts or animations change them every frame
    this->refreshRenderableUniforms ();

    if (this->m_pass.shader == XRAY_EFFECT_SHADER) {
	const auto mode = this->getContext ().getApp ().getContext ().state.xray.mode;
	this->m_xrayReveal = mode == Application::XrayMode::Full ? 1.0f
	    : mode == Application::XrayMode::Disabled            ? 0.0f
								 : -1.0f;
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

    // a passthrough layer draws its children into its buffer: model = inverse (layer world), view = identity and an
    // ortho of the layer's size (sub_1401ECB20), which comes down to remapping the scene's clip space
    const auto* layerTarget = this->m_drawTo == this->m_renderable.getScene ().getFBO () || this->m_followLayerTarget
	? this->m_renderable.getScene ().getLayerTarget ()
	: nullptr;
    const glm::mat4* modelViewProjection = this->m_modelViewProjectionMatrix;
    const glm::mat4* modelViewProjectionInverse = this->m_modelViewProjectionMatrixInverse;
    const glm::mat4* viewProjection = this->m_viewProjectionMatrix;

    if (layerTarget != nullptr) {
	if (modelViewProjection != nullptr) {
	    this->m_layerModelViewProjection = layerTarget->transform * *modelViewProjection;
	    this->m_layerModelViewProjectionInverse = glm::inverse (this->m_layerModelViewProjection);
	    this->m_modelViewProjectionMatrix = &this->m_layerModelViewProjection;
	    this->m_modelViewProjectionMatrixInverse = &this->m_layerModelViewProjectionInverse;
	}

	if (viewProjection != nullptr) {
	    this->m_layerViewProjection = layerTarget->transform * *viewProjection;
	    this->m_viewProjectionMatrix = &this->m_layerViewProjection;
	}
    }

    // effectcomposebackground (refraction, ...) samples _rt_FullFrameBuffer where the layer is on screen
    this->m_effectModelViewProjectionMatrix = this->m_effectModelViewProjectionOverride != nullptr
	? this->m_effectModelViewProjectionOverride
	: this->m_modelViewProjectionMatrix;

    this->setupRenderFramebuffer ();
    this->setupRenderTexture ();

    for (const auto& [slot, resolution] : this->m_texelSources) {
	if (resolution->x > 0.0f && resolution->y > 0.0f) {
	    this->m_texels[slot] = glm::vec4 (1.0f / resolution->x, 1.0f / resolution->y, resolution->x, resolution->y);
	}
    }

    this->setupRenderUniforms ();
    this->setupRenderReferenceUniforms ();

    this->setupRenderAttributes ();
    this->renderGeometry ();
    this->cleanupRenderSetup ();

    if (layerTarget != nullptr) {
	this->m_modelViewProjectionMatrix = modelViewProjection;
	this->m_modelViewProjectionMatrixInverse = modelViewProjectionInverse;
	this->m_viewProjectionMatrix = viewProjection;

	if (layerTarget->alphaMax && this->m_drawTo == this->m_renderable.getScene ().getFBO ()) {
	    glBlendEquation (GL_FUNC_ADD);
	}
    }
}

void CPass::clearDestination () const {
    if (this->m_drawTo == nullptr) {
	return;
    }

    GLfloat previousClearColor[4] = {};
    glGetFloatv (GL_COLOR_CLEAR_VALUE, previousClearColor);
    glBindFramebuffer (GL_FRAMEBUFFER, this->m_drawTo->getFramebuffer ());
    glViewport (0, 0, this->m_drawTo->getRealWidth (), this->m_drawTo->getRealHeight ());
    glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
    glColorMask (true, true, true, true);
    glClear (GL_COLOR_BUFFER_BIT);
    glClearColor (previousClearColor[0], previousClearColor[1], previousClearColor[2], previousClearColor[3]);
}

std::shared_ptr<const FBOProvider> CPass::getFBOProvider () const { return this->m_fboProvider; }

const CRenderable& CPass::getRenderable () const { return this->m_renderable; }

void CPass::copyBindings (const CPass& other) {
    this->m_drawTo = other.m_drawTo;
    this->m_input = other.m_input;
    this->m_previousInput = other.m_previousInput;
    this->a_Position = other.a_Position;
    this->a_TexCoord = other.a_TexCoord;
    this->m_modelViewProjectionMatrix = other.m_modelViewProjectionMatrix;
    this->m_modelViewProjectionMatrixInverse = other.m_modelViewProjectionMatrixInverse;
    this->m_effectModelViewProjectionOverride = other.m_effectModelViewProjectionOverride;
    this->m_modelMatrix = other.m_modelMatrix;
    this->m_layerModelMatrix = other.m_layerModelMatrix;
    this->m_viewProjectionMatrix = other.m_viewProjectionMatrix;
    this->m_lightingModelMatrix = other.m_lightingModelMatrix;
    this->m_lightingNormalMatrix = other.m_lightingNormalMatrix;
    this->m_lightingViewProjectionMatrix = other.m_lightingViewProjectionMatrix;
    this->m_effectTextureProjectionMatrix = other.m_effectTextureProjectionMatrix;
    this->m_effectTextureProjectionMatrixInverse = other.m_effectTextureProjectionMatrixInverse;
}

void CPass::setDestination (std::shared_ptr<const CFBO> drawTo) { this->m_drawTo = std::move (drawTo); }

void CPass::setInput (std::shared_ptr<const TextureProvider> input) { this->m_input = std::move (input); }

void CPass::setPreviousInput (std::shared_ptr<const TextureProvider> input) {
    this->m_previousInput = std::move (input);
}

void CPass::setTexture (int index, std::shared_ptr<const TextureProvider> texture) {
    const auto it = this->m_textures.find (index);

    // the uniforms were set up with the pass, a texture set afterwards brings its own size
    if (texture != nullptr) {
	this->addUniform ("g_Texture" + std::to_string (index) + "Resolution", texture->getResolution ());

	if (index > 0) {
	    this->m_texelSources[index] = texture->getResolution ();
	}
    }

    this->m_textures[index] = std::make_shared<TextureChainEntry> (TextureChainEntry {
	.texture = std::move (texture),
	.next = it != this->m_textures.end () ? it->second : nullptr,
    });
}

void CPass::setEffectModelViewProjectionMatrix (const glm::mat4* projection) {
    this->m_effectModelViewProjectionOverride = projection;
}

void CPass::setModelViewProjectionMatrix (const glm::mat4* projection) {
    this->m_modelViewProjectionMatrix = projection;
}

void CPass::setModelViewProjectionMatrixInverse (const glm::mat4* projection) {
    this->m_modelViewProjectionMatrixInverse = projection;
}

const glm::mat4 CPass::s_identity { 1.0f };

void CPass::setModelMatrix (const glm::mat4* model) { this->m_modelMatrix = model; }

void CPass::setLayerModelMatrix (const glm::mat4* model) { this->m_layerModelMatrix = model; }

void CPass::setFogWorld (const bool world) {
    this->m_fogWorld = world;
    const auto& fog = this->m_renderable.getScene ().getFog ();
    this->addUniform ("g_FogHeightParams", world ? &fog.heightParamsWorld : &fog.heightParamsLocal);

    if (world) {
	this->addUniform ("g_EyePosition", &fog.eyeWorld);
    }
}

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

void CPass::setDepthState (std::optional<std::pair<DepthtestMode, DepthwriteMode>> state) {
    this->m_depthState = state;
}

BlendingMode CPass::getBlendingMode () const { return this->m_blendingmode; }

void CPass::setTexCoord (GLuint texcoord) { this->a_TexCoord = texcoord; }

void CPass::setClearColor (const glm::vec4* color) { this->m_clearColor = color; }

void CPass::setKeepDestination (bool keep) { this->m_keepDestination = keep; }

void CPass::setFollowLayerTarget (bool follow) { this->m_followLayerTarget = follow; }

void CPass::setNeutralColor (bool neutral) { this->m_neutralColor = neutral; }

void CPass::setPosition (GLuint position) { this->a_Position = position; }

const MaterialPass& CPass::getPass () const { return this->m_pass; }

std::optional<std::reference_wrapper<std::string>> CPass::getTarget () const { return this->m_target; }

Render::Shaders::Shader* CPass::getShader () const { return this->m_shader; }

bool CPass::hasGeometryStage () const { return this->m_compiled != nullptr && !this->m_compiled->geometry.empty (); }

GLuint CPass::getProgramID () const { return this->m_programID; }

void CPass::setGeometryCallback (
    GeometryCallback setupAttribs, GeometryCallback drawGeometry, GeometryCallback cleanupAttribs
) {
    this->m_setupAttribsCallback = std::move (setupAttribs);
    this->m_drawGeometryCallback = std::move (drawGeometry);
    this->m_cleanupAttribsCallback = std::move (cleanupAttribs);
}

namespace {
// Mesa mis-reads a vec3 uniform followed by a float used as vec4(vec3, float), use g_Color4
std::string patchColorAlpha (const std::string& shader) {
    std::string patched = shader;

    const std::string declarations = "uniform vec3 g_Color;\nuniform float g_Alpha;";
    const std::string construction = "vec4(g_Color, g_Alpha)";
    const auto declarationsAt = patched.find (declarations);
    const auto constructionAt = patched.find (construction);

    if (declarationsAt == std::string::npos || constructionAt == std::string::npos) {
	return patched;
    }

    patched.replace (constructionAt, construction.size (), "g_Color4");
    patched.replace (declarationsAt, declarations.size (), "uniform vec4 g_Color4;");

    // only safe when that was the sole use of both, the trailing "#if 0" copy doesn't count
    const size_t codeEnd = std::min (patched.size (), patched.find ("#if 0"));
    const auto stillUsed = [&patched, codeEnd] (const std::string& name) {
	size_t pos = 0;

	while ((pos = patched.find (name, pos)) != std::string::npos && pos < codeEnd) {
	    const bool wordStart = pos == 0
		|| !(std::isalnum (static_cast<unsigned char> (patched[pos - 1])) || patched[pos - 1] == '_');
	    const size_t end = pos + name.size ();
	    const bool wordEnd = end >= patched.size ()
		|| !(std::isalnum (static_cast<unsigned char> (patched[end])) || patched[end] == '_');

	    if (wordStart && wordEnd) {
		return true;
	    }

	    pos = end;
	}

	return false;
    };

    if (stillUsed ("g_Color") || stillUsed ("g_Alpha")) {
	return shader;
    }

    return patched;
}
} // namespace

GLuint CPass::compileShader (const char* shader, GLuint type) {
    const GLuint shaderID = glCreateShader (type);

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

    // a lit layer drawn straight onto the scene overrides this off
    if ((comboEnabled ("LIGHTING") || comboEnabled ("REFLECTION"))
	&& !this->m_override.combos.contains ("PRELIGHTING")) {
	this->m_combos.insert_or_assign ("PRELIGHTING", 1);
    }

    // HDR scene rendering defines HDR for every pass (sub_1401A5C40)
    if (this->m_renderable.getScene ().isHDR ()) {
	this->m_combos.insert_or_assign ("HDR", 1);
    }

    // an alphatocoverage pass gets ALPHATOCOVERAGE (sub_140154480), the shaders' GLSL path discards below 0.5 alpha
    // there (3734636606's invisible "alpha": 0 collider boxes)
    if (this->m_pass.blending == BlendingMode_AlphaToCoverage) {
	this->m_combos.insert_or_assign ("ALPHATOCOVERAGE", 1);
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

    // every pass of an orthographic scene gets SCENE_ORTHO (renderer flag 0x400, sub_1401A5C40)
    if (this->m_renderable.getScene ().getCamera ().isOrthogonal ()) {
	this->m_combos.insert_or_assign ("SCENE_ORTHO", 1);
    }

    this->m_compiled = this->compileShaderSources (shaderName, passTextures, overrideTextures);
    this->m_shader = this->m_compiled->shader.get ();

    // samplers marked "formatcombo" get TEX<slot>FORMAT set to their texture's format, like wallpaper64.exe
    // (sub_14015EC30). Which slots those are is only known once the shader is parsed, so rebuild it when one changed
    if (this->applyFormatCombos (passTextures, overrideTextures)) {
	this->m_compiled = this->compileShaderSources (shaderName, passTextures, overrideTextures);
	this->m_shader = this->m_compiled->shader.get ();
    }

    // fog turns into FOG_DIST/FOG_HEIGHT for every pass whose FOG combo (the shader default included) is on
    // (sub_1401A5C40); that default is only known once the shader is parsed
    const auto& scene = this->m_renderable.getScene ();
    const auto comboValue = [this] (const std::string& name) {
	for (const ComboMap* combos :
	     std::initializer_list<const ComboMap*> { &this->m_override.combos, &this->m_combos }) {
	    if (const auto it = combos->find (name); it != combos->end ()) {
		return it->second;
	    }
	}
	for (const auto* unit : { &this->m_shader->getFragment (), &this->m_shader->getVertex () }) {
	    if (const auto it = unit->getDiscoveredCombos ().find (name); it != unit->getDiscoveredCombos ().end ()) {
		return it->second;
	    }
	}
	return 0;
    };

    // same for LIGHTING: the light counts of the scene's lightconfig, which the LightingV1 module is generated from
    // (sub_1401A5C40). The shadow counts are 0 with WE's shadow setting off
    if (comboValue ("LIGHTING") != 0) {
	const auto& lighting = scene.getLightingV1 ();

	this->m_combos.insert_or_assign ("LIGHTS_POINT", lighting.points);
	this->m_combos.insert_or_assign ("LIGHTS_SPOT", lighting.spots);
	this->m_combos.insert_or_assign ("LIGHTS_TUBE", lighting.tubes);
	this->m_combos.insert_or_assign ("LIGHTS_DIRECTIONAL", lighting.directionals);
	this->m_combos.insert_or_assign ("LIGHTS_SPOT_SHADOW_COOKIE", lighting.spotShadowCookies);
	this->m_combos.insert_or_assign ("LIGHTS_SPOT_SHADOW", lighting.spotShadows);
	this->m_combos.insert_or_assign ("LIGHTS_SPOT_COOKIE", lighting.spotCookies);
	this->m_combos.insert_or_assign ("LIGHTS_DIRECTIONAL_SHADOW", lighting.directionalShadows);
	this->m_combos.insert_or_assign ("LIGHTS_POINT_SHADOW", lighting.pointShadows);

	if (lighting.spotShadowCookies + lighting.spotShadows + lighting.directionalShadows + lighting.pointShadows) {
	    this->m_combos.insert_or_assign ("LIGHTS_SHADOW_MAPPING", 1);
	    this->m_combos.insert_or_assign ("LIGHTS_SHADOW_MAPPING_QUALITY", scene.getShadowQuality ());
	}

	if (lighting.spotShadowCookies + lighting.spotCookies != 0) {
	    this->m_combos.insert_or_assign ("LIGHTS_COOKIE", 1);
	}

	// WE's renderer always has flag 0x1000 (sub_140110630), so every pass gets REVERSEDEPTH. Of the shaders drawn
	// through here only the shadow cascades of common_pbr_2.h read it, the shadow matrices do use reversed depth
	this->m_combos.insert_or_assign ("REVERSEDEPTH", 1);

	this->m_compiled = this->compileShaderSources (shaderName, passTextures, overrideTextures);
	this->m_shader = this->m_compiled->shader.get ();
    }

    if (scene.hasDistanceFog () || scene.hasHeightFog ()) {
	if (comboValue ("FOG") != 0) {
	    if (scene.hasDistanceFog ()) {
		this->m_combos.insert_or_assign ("FOG_DIST", 1);
	    }
	    if (scene.hasHeightFog ()) {
		this->m_combos.insert_or_assign ("FOG_HEIGHT", 1);
	    }

	    this->m_compiled = this->compileShaderSources (shaderName, passTextures, overrideTextures);
	    this->m_shader = this->m_compiled->shader.get ();
	}
    }

    std::string vertex = this->m_compiled->vertex;
    std::string fragment = this->m_compiled->fragment;

    if (shaderName == XRAY_EFFECT_SHADER) {
	this->m_xrayRevealPatched = patchXrayRevealOverride (fragment);

	if (!this->m_xrayRevealPatched) {
	    sLog.error (
		"Full/disabled xray unavailable: couldn't find the expected reveal blend line in the "
		"compiled effects/xray shader (spirv-cross output format may have changed)"
	    );
	}
    }

    // passes with the same sources share one program (every particle system instance of a child would link its own
    // otherwise, hundreds in the first seconds of a rain wallpaper). Uniforms are uploaded on every draw, the sampler
    // units below are the same for all of them
    this->m_programKey = vertex + '\0' + fragment + '\0' + this->m_compiled->geometry;
    if (const auto cached = sharedPrograms ().find (this->m_programKey); cached != sharedPrograms ().end ()) {
	this->m_programID = cached->second.program;
	cached->second.users++;
    } else {
	this->m_programID = this->linkProgram (vertex, fragment, this->m_compiled->geometry, shaderName);
	sharedPrograms ().emplace (this->m_programKey, SharedProgram { this->m_programID, 1 });
    }

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

std::unordered_map<std::string, std::weak_ptr<CPass::CompiledShader>>& CPass::sharedShaders () {
    static std::unordered_map<std::string, std::weak_ptr<CompiledShader>> shaders;
    return shaders;
}

std::shared_ptr<CPass::CompiledShader> CPass::compileShaderSources (
    const std::string& shaderName, const TextureMap& passTextures, const TextureMap& overrideTextures
) {
    // parsing and translating a shader takes a few ms, and every instance of a child particle system builds the same
    // one. Override constants can change the parsed defaults, those passes keep a shader of their own
    const bool shareable = this->m_override.constants.empty ();
    std::string key;

    if (shareable) {
	std::ostringstream stream;
	stream << shaderName << '\0' << &this->m_renderable.getAssetLocator ();
	const ComboMap* comboMaps[] = { &this->m_combos, &this->m_override.combos };
	for (const ComboMap* combos : comboMaps) {
	    stream << '\1';
	    for (const auto& [name, value] : *combos) {
		stream << name << '=' << value << ';';
	    }
	}
	for (const TextureMap* textures : { &passTextures, &overrideTextures }) {
	    stream << '\1';
	    for (const auto& [index, name] : *textures) {
		stream << index << '=' << name << ';';
	    }
	}
	key = stream.str ();

	if (const auto it = sharedShaders ().find (key); it != sharedShaders ().end ()) {
	    if (auto cached = it->second.lock ()) {
		return cached;
	    }
	}
    }

    static const ShaderConstantMap noConstants;
    auto compiled = std::make_shared<CompiledShader> ();
    compiled->combos = this->m_combos;
    compiled->overrideCombos = this->m_override.combos;
    compiled->passTextures = passTextures;
    compiled->overrideTextures = overrideTextures;
    compiled->shader = std::make_unique<Render::Shaders::Shader> (
	this->m_renderable.getAssetLocator (), shaderName, compiled->combos, compiled->overrideCombos,
	compiled->passTextures, compiled->overrideTextures, shareable ? noConstants : this->m_override.constants
    );

    auto sources = Shaders::GLSLContext::get ().toGlsl (
	compiled->shader->vertex (), compiled->shader->fragment (), shaderName, compiled->shader->geometry ()
    );
    compiled->vertex = std::move (sources.vertex);
    compiled->fragment = std::move (sources.fragment);
    compiled->geometry = std::move (sources.geometry);

    if (shareable) {
	std::erase_if (sharedShaders (), [] (const auto& entry) { return entry.second.expired (); });
	sharedShaders ().insert_or_assign (key, compiled);
    }

    return compiled;
}

std::unordered_map<std::string, CPass::SharedProgram>& CPass::sharedPrograms () {
    static std::unordered_map<std::string, SharedProgram> programs;
    return programs;
}

bool CPass::releaseSharedProgram () {
    const auto it = sharedPrograms ().find (this->m_programKey);
    if (this->m_programKey.empty () || it == sharedPrograms ().end ()) {
	return false;
    }

    if (--it->second.users == 0) {
	glDeleteProgram (it->second.program);
	sharedPrograms ().erase (it);
    }
    this->m_programID = 0;
    return true;
}

GLuint CPass::linkProgram (
    const std::string& vertex, const std::string& unpatchedFragment, const std::string& geometry,
    const std::string& shaderName
) {
    const std::string fragment = patchColorAlpha (unpatchedFragment);
    auto& cache = Shaders::ShaderCache::get ();
    const std::string cacheKey = vertex + '\0' + fragment + '\0' + geometry;

    if (const GLuint cached = cache.loadProgram (cacheKey); cached != 0) {
#if !NDEBUG
	glObjectLabel (GL_PROGRAM, cached, -1, shaderName.c_str ());
#endif /* DEBUG */
	return cached;
    }

    const GLuint vertexShaderID = compileShader (vertex.c_str (), GL_VERTEX_SHADER);
    const GLuint fragmentShaderID = compileShader (fragment.c_str (), GL_FRAGMENT_SHADER);
    const GLuint geometryShaderID = geometry.empty () ? 0 : compileShader (geometry.c_str (), GL_GEOMETRY_SHADER);
    const GLuint program = glCreateProgram ();
    glAttachShader (program, vertexShaderID);
    glAttachShader (program, fragmentShaderID);
    if (geometryShaderID != 0) {
	glAttachShader (program, geometryShaderID);
    }
    if (cache.programsSupported ()) {
	glProgramParameteri (program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    }
    glLinkProgram (program);
    GLint result = GL_FALSE;
    int infoLogLength = 0;

    glGetProgramiv (program, GL_LINK_STATUS, &result);
    glGetProgramiv (program, GL_INFO_LOG_LENGTH, &infoLogLength);

    if (infoLogLength > 0) {
	const auto logBuffer = new char[infoLogLength + 1];
	memset (logBuffer, 0, infoLogLength + 1);
	glGetProgramInfoLog (program, infoLogLength, nullptr, logBuffer);
	const std::string message = logBuffer;
	delete[] logBuffer;
	if (result == GL_FALSE) {
	    sLog.exception (message);
	} else {
	    sLog.error (message);
	}
    }

#if !NDEBUG
    glObjectLabel (GL_PROGRAM, program, -1, shaderName.c_str ());
    glObjectLabel (GL_SHADER, vertexShaderID, -1, (shaderName + ".vert").c_str ());
    glObjectLabel (GL_SHADER, fragmentShaderID, -1, (shaderName + ".frag").c_str ());
#endif /* DEBUG */

    // once linked, the shaders themselves are no longer needed and can be detached/deleted
    glDetachShader (program, vertexShaderID);
    glDetachShader (program, fragmentShaderID);

    glDeleteShader (vertexShaderID);
    glDeleteShader (fragmentShaderID);
    if (geometryShaderID != 0) {
	glDetachShader (program, geometryShaderID);
	glDeleteShader (geometryShaderID);
    }

    cache.storeProgram (cacheKey, program);

    return program;
}

bool CPass::applyFormatCombos (const TextureMap& passTextures, const TextureMap& overrideTextures) {
    const auto& fragment = this->m_shader->getFragment ();
    const auto& componentCombos = fragment.getComponentCombos ();
    bool changed = false;
    std::set<int> slots = fragment.getFormatComboSlots ();

    for (const auto& [slot, combos] : componentCombos) {
	slots.insert (slot);
    }

    for (const int slot : slots) {
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
		texture
		    = this->getContext ().resolveTexture (*name, this->m_renderable.getScene ().getScene ().project);
	    } catch (const std::exception&) {
		continue;
	    }
	}

	if (texture == nullptr) {
	    continue;
	}

	// a bound mask turns on the combos of the components its flags mark as painted (sub_14016C800)
	if (const auto it = componentCombos.find (slot); it != componentCombos.end ()) {
	    for (size_t i = 0; i < it->second.size () && i < 4; i++) {
		if (it->second[i].empty () || (texture->getFlags () & (0x100000u << i)) == 0) {
		    continue;
		}

		if (const auto combo = this->m_combos.find (it->second[i]);
		    combo == this->m_combos.end () || combo->second != 1) {
		    this->m_combos.insert_or_assign (it->second[i], 1);
		    changed = true;
		}
	    }
	}

	if (!fragment.getFormatComboSlots ().contains (slot) || texture->getFormat () == TextureFormat_UNKNOWN) {
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
	    auto texture = this->resolveNamedTexture (textureName);

	    this->m_textures[index] = std::make_shared<TextureChainEntry> (TextureChainEntry {
		.texture = texture,
		.next = nullptr,
	    });
	} catch (std::runtime_error& ex) {
	    sLog.error (
		"Cannot resolve texture '", textureName, "' (index=", index,
		", object id=", this->m_renderable.getId (), ") for fragment shader ", ex.what ()
	    );
	}
    }

    for (const auto& [index, textureName] : this->m_shader->getFragment ().getTextures ()) {
	try {
	    auto texture = this->resolveNamedTexture (textureName);

	    const auto it = this->m_textures.find (index);
	    const auto chain = std::make_shared<TextureChainEntry> (TextureChainEntry {
		.texture = texture,
		.next = it != this->m_textures.end () ? it->second : nullptr,
	    });

	    this->m_textures[index] = chain;

	} catch (std::runtime_error& ex) {
	    sLog.error (
		"Cannot resolve texture '", textureName, "' (index=", index,
		", object id=", this->m_renderable.getId (), ") for fragment shader ", ex.what ()
	    );
	    this->bindMissingTexture (index);
	}
    }

    for (const auto& [index, textureName] : this->m_pass.textures) {
	try {
	    auto texture = this->resolveNamedTexture (textureName);

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
		"Cannot resolve texture '", textureName, "' (index=", index,
		", object id=", this->m_renderable.getId (), ") for pass ", ex.what ()
	    );
	    this->bindMissingTexture (index);
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
	    auto texture = this->resolveNamedTexture (textureName);

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
		"Cannot resolve user texture '", textureName, "' (index=", index,
		", object id=", this->m_renderable.getId (), ") for pass ", ex.what ()
	    );
	}
    }

    // override any texture
    for (const auto& [index, textureName] : this->m_override.textures) {
	// WE writes every bound FBO into its slot on each draw (sub_1401EBF60), so overrides there are never sampled
	if (const auto bind = this->m_binds.find (index); bind != this->m_binds.end () && bind->second != "previous") {
	    continue;
	}

	// same for slot 0 of a pass without binds (3736099508)
	if (index == 0 && this->m_binds.empty ()) {
	    continue;
	}

	try {
	    auto texture = this->resolveNamedTexture (textureName);

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
		"Cannot resolve texture '", textureName, "' (index=", index,
		", object id=", this->m_renderable.getId (), ") for override ", ex.what ()
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
	    auto texture = this->resolveNamedTexture (textureName);

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
		"Cannot resolve user texture '", textureName, "' (index=", index,
		", object id=", this->m_renderable.getId (), ") for override ", ex.what ()
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
    // a particle whose material failed to load has no texture at all
    if (texture != nullptr) {
	this->m_texture0Resolution = *texture->getResolution ();
    }
    this->addUniform ("g_Texture0Resolution", &this->m_texture0Resolution);

    // g_TextureNTexel (uniforms 71..80, sub_1400D8300): (1 / width, 1 / height, width, height) of the bound texture,
    // (0.5, 0.5, 2, 2) for an empty slot. Sizes can change (the shadow atlas grows), so it's refreshed every draw
    for (int slot = 0; slot < 10; slot++) {
	this->m_texels[slot] = glm::vec4 (0.5f, 0.5f, 2.0f, 2.0f);
	this->addUniform ("g_Texture" + std::to_string (slot) + "Texel", &this->m_texels[slot]);
    }

    this->m_texelSources[0] = &this->m_texture0Resolution;

    for (const auto& [textureIndex, expectedTexture] : this->m_textures) {
	std::ostringstream namestream;

	namestream << "g_Texture" << textureIndex << "Resolution";

	texture = this->resolveTexture (expectedTexture->texture, textureIndex, texture);
	if (texture == nullptr) {
	    continue;
	}

	this->addUniform (namestream.str (), texture->getResolution ());

	if (textureIndex != 0) {
	    this->m_texelSources[textureIndex] = texture->getResolution ();
	}

	// the mip count of a mipmapped frame buffer, REFLECTION scales its roughness LOD by it
	if (const auto fbo = std::dynamic_pointer_cast<const CFBO> (texture);
	    fbo != nullptr && fbo->getMipLevels () > 1) {
	    this->addUniform (
		"g_Texture" + std::to_string (textureIndex) + "MipMapInfo", static_cast<float> (fbo->getMipLevels ())
	    );
	}
    }

    this->addUniform ("g_Texture0Resolution", &this->m_texture0Resolution);

    // WE ORs the requirement of every texture slot the compiled program uses into the pass (sub_1401515B0); a sampled
    // _rt_Reflection turns on the scene's reflection pass and keeps this object out of it (object flag 8)
    auto& scene = this->m_renderable.getScene ();
    const auto reflection = scene.find ("_rt_Reflection");

    for (const auto& [index, chain] : this->m_textures) {
	if (reflection == nullptr || chain == nullptr
	    || chain->texture.get () != static_cast<const TextureProvider*> (reflection.get ())) {
	    continue;
	}

	if (glGetUniformLocation (this->m_programID, ("g_Texture" + std::to_string (index)).c_str ()) != -1) {
	    scene.addReflectionReceiver (this->m_renderable.getId ());
	}
    }
}

void CPass::setupUniforms () {
    this->setupTextureUniforms ();

    const auto& renderable = this->m_renderable;
    const auto& scene = this->m_renderable.getScene ();
    const auto& sceneData = this->m_renderable.getScene ().getScene ();
    const auto& recorder = this->m_renderable.getScene ().getAudioContext ().getRecorder ();

    // lighting variables
    this->addUniform ("g_LightAmbientColor", &sceneData.colors.ambient->value->getVec3 ());
    this->addUniform ("g_LightSkylightColor", &sceneData.colors.skylight->value->getVec3 ());
    this->addUniform ("g_LightsPosition", UniformType::Vector3, scene.getLightsPosition (), 4);
    this->addUniform ("g_LightsColorPremultiplied", UniformType::Vector4, scene.getLightsColorPremultiplied (), 3);
    this->addUniform ("g_LightsColorRadius", UniformType::Vector4, scene.getLightsColorRadius (), 4);
    // LightingV1, the arrays are as long as the scene's lightconfig counts
    const auto& lighting = scene.getLightingV1 ();
    const auto addLights = [this] (const char* name, const glm::vec4* values, const int count) {
	if (count > 0) {
	    this->addUniform (name, UniformType::Vector4, values, count);
	}
    };
    addLights ("g_LPoint_Color", lighting.pointColor, lighting.points);
    addLights ("g_LPoint_Origin", lighting.pointOrigin, lighting.points);
    addLights ("g_LSpot_Color", lighting.spotColor, lighting.spots);
    addLights ("g_LSpot_Origin", lighting.spotOrigin, lighting.spots);
    addLights ("g_LSpot_Direction", lighting.spotDirection, lighting.spots);
    addLights ("g_LSpot_Exponent", lighting.spotExponent, lighting.spots);
    addLights ("g_LTube_Color", lighting.tubeColor, lighting.tubes);
    addLights ("g_LTube_OriginA", lighting.tubeOriginA, lighting.tubes);
    addLights ("g_LTube_OriginB", lighting.tubeOriginB, lighting.tubes);
    addLights ("g_LDirectional_Color", lighting.directionalColor, lighting.directionals);
    addLights ("g_LDirectional_Direction", lighting.directionalDirection, lighting.directionals);
    // WE binds the renderer's eye (+104) for every pass; lit ones get it here in the world their lighting works in
    // (models, particles and text set their own afterwards), without it the view vector points at the origin
    if (this->m_combos.contains ("LIGHTS_POINT")) {
	this->addUniform ("g_EyePosition", &scene.getFog ().eyeWorld);
    }
    if (const int features = lighting.features (); features > 0) {
	this->addUniform ("g_LFeature_ShadowProjection", UniformType::Matrix4, lighting.featureProjection, features);
	addLights ("g_LFeature_ShadowProjectionTransform", lighting.featureProjectionTransform, features);
    }
    addLights ("g_LFeature_ShadowPointProjection", lighting.pointShadowProjection, lighting.pointShadows);
    addLights (
	"g_LFeature_ShadowPointProjectionTransform", lighting.pointShadowProjectionTransform, lighting.pointShadows
    );
    this->addUniform ("g_FogDistanceColor", &scene.getFog ().distanceColor);
    this->addUniform ("g_FogDistanceParams", &scene.getFog ().distanceParams);
    this->addUniform ("g_FogHeightColor", &scene.getFog ().heightColor);
    this->addUniform (
	"g_FogHeightParams", this->m_fogWorld ? &scene.getFog ().heightParamsWorld : &scene.getFog ().heightParamsLocal
    );
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
    // register variables like brightness and alpha with the layer's values, unless the pass sets them itself
    const auto addRenderableUniform = [this] (const char* name, const auto& value) {
	if (!this->m_constantUniforms.contains (name)) {
	    this->addUniform (name, value);
	}
    };

    addRenderableUniform ("g_Brightness", renderable.getBrightness ());
    addRenderableUniform ("g_UserAlpha", renderable.getUserAlpha ());
    addRenderableUniform ("g_Alpha", renderable.getAlpha ());
    addRenderableUniform ("g_Color", renderable.getColor ());
    addRenderableUniform ("g_Color4", renderable.getColor4 ());
    if (!this->m_uniforms.contains ("g_CompositeColor")) {
	this->addUniform ("g_CompositeColor", renderable.getCompositeColor ());
    }
    this->addUniform ("g_Time", &g_Time);
    this->addUniform ("g_Daytime", &g_Daytime);
    this->addUniform ("g_ModelViewProjectionMatrixInverse", &this->m_modelViewProjectionMatrixInverse);
    this->addUniform ("g_ModelViewProjectionMatrix", &this->m_modelViewProjectionMatrix);
    this->addUniform ("g_EffectModelViewProjectionMatrix", &this->m_effectModelViewProjectionMatrix);
    this->addUniform ("g_ModelMatrix", &this->m_modelMatrix);
    this->addUniform ("g_EffectModelMatrix", &this->m_modelMatrix);
    this->addUniform ("g_LayerModelMatrix", &this->m_layerModelMatrix);
    this->addUniform ("g_NormalModelMatrix", glm::identity<glm::mat3> ());
    this->addUniform ("g_ViewProjectionMatrix", &this->m_viewProjectionMatrix);
    this->addUniform ("g_PointerPosition", scene.getPointerPosition ());
    this->addUniform ("g_PointerPositionLast", scene.getPointerPositionLast ());
    this->addUniform ("g_ParallaxPosition", scene.getParallaxPosition ());
    this->addUniform ("g_EffectTextureProjectionMatrix", &this->m_effectTextureProjectionMatrix);
    this->addUniform ("g_EffectTextureProjectionMatrixInverse", &this->m_effectTextureProjectionMatrixInverse);
    this->addUniform ("g_TexelSize", scene.getTexelSize ());
    this->addUniform ("g_TexelSizeHalf", scene.getTexelSizeHalf ());
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

namespace {
int componentCount (const ShaderVariable& var) {
    if (var.is<ShaderVariableVector4> ()) {
	return 4;
    }
    if (var.is<ShaderVariableVector3> ()) {
	return 3;
    }
    if (var.is<ShaderVariableVector2> ()) {
	return 2;
    }
    return 1;
}

int componentCount (GLenum type) {
    switch (type) {
	case GL_FLOAT_VEC4:
	    return 4;
	case GL_FLOAT_VEC3:
	    return 3;
	case GL_FLOAT_VEC2:
	    return 2;
	default:
	    return 1;
    }
}
} // namespace

int CPass::getMaterialConstantSize (const std::string& name) const {
    const ShaderVariable* var = this->m_materialConstants.contains (name) ? this->findActiveParameter (name) : nullptr;

    return var == nullptr ? 0 : componentCount (*var);
}

bool CPass::isMaterialConstantInDegrees (const std::string& name) const {
    const ShaderVariable* var = this->m_materialConstants.contains (name) ? this->findActiveParameter (name) : nullptr;

    return var != nullptr && var->isInDegrees ();
}

void CPass::setupShaderVariables () {
    // a uniform declared differently per stage takes the linked program's type
    for (const auto& cur : this->m_shader->getVertex ().getParameters ()) {
	if (this->m_uniforms.contains (cur->getName ())) {
	    continue;
	}

	ShaderVariable* var = cur;

	for (const auto& other : this->m_shader->getFragment ().getParameters ()) {
	    if (other->getName () == cur->getName () && componentCount (*other) != componentCount (*cur)
		&& componentCount (this->getDeclaredUniformType (cur->getName ())) == componentCount (*other)) {
		var = other;
		break;
	    }
	}

	this->addUniform (var);
    }

    for (const auto& cur : this->m_shader->getFragment ().getParameters ()) {
	if (!this->m_uniforms.contains (cur->getName ())) {
	    this->addUniform (cur);
	}
    }

    // shader defaults as scripts see them
    for (const auto* unit : { &this->m_shader->getVertex (), &this->m_shader->getFragment () }) {
	for (const auto* cur : unit->getParameters ()) {
	    if (!cur->getIdentifierName ().empty () && this->m_uniforms.contains (cur->getName ())) {
		this->m_materialConstants.try_emplace (cur->getIdentifierName (), cur);
	    }
	}
    }

    // material constants, then override constants, which win
    for (const auto* constants : { &this->m_pass.constants, &this->m_override.constants }) {
	for (const auto& [name, value] : *constants) {
	    ShaderVariable* var = this->findActiveParameter (name);

	    if (var == nullptr) {
		continue;
	    }

	    this->addConstantUniform (var, *value);
	    this->m_constantUniforms.insert (var->getName ());

	    if (this->m_materialConstants.contains (name)) {
		this->m_materialConstants[name] = value->value.get ();
	    }
	}
    }

    // no-op when patchXrayRevealOverride() didn't find its anchors
    if (this->m_pass.shader == XRAY_EFFECT_SHADER) {
	this->addUniform ("g_XrayReveal", &this->m_xrayReveal);
    }
}

std::vector<std::string> CPass::getMaterialConstantNames () const {
    std::vector<std::pair<std::string, std::string>> byUniform;

    for (const auto& name : this->m_materialConstants | std::views::keys) {
	if (const auto* var = this->findActiveParameter (name)) {
	    byUniform.emplace_back (var->getName (), name);
	}
    }

    std::ranges::sort (byUniform, std::greater {});
    std::vector<std::string> names;

    for (const auto& name : byUniform | std::views::values) {
	names.push_back (name);
    }

    return names;
}

const DynamicValue* CPass::getMaterialConstant (const std::string& name) const {
    const auto it = this->m_materialConstants.find (name);

    return it == this->m_materialConstants.end () ? nullptr : it->second;
}

DynamicValue* CPass::getScriptConstant (const std::string& name) {
    if (const auto it = this->m_scriptConstants.find (name); it != this->m_scriptConstants.end ()) {
	return it->second.get ();
    }

    ShaderVariable* var = this->findActiveParameter (name);
    const DynamicValue* current = this->getMaterialConstant (name);

    if (var == nullptr || current == nullptr) {
	return nullptr;
    }

    // typed like the uniform (sub_140154480), vectors get padded
    const bool scalar = current->getType () == DynamicValue::Float || current->getType () == DynamicValue::Int;
    const glm::vec4 value = scalar ? glm::vec4 (current->getFloat (), 0.0f, 0.0f, 0.0f) : current->getVec4 ();
    std::unique_ptr<DynamicValue> copy;

    if (var->is<ShaderVariableVector2> ()) {
	copy = std::make_unique<DynamicValue> (glm::vec2 (value));
    } else if (var->is<ShaderVariableVector3> ()) {
	copy = std::make_unique<DynamicValue> (glm::vec3 (value));
    } else if (var->is<ShaderVariableVector4> ()) {
	copy = std::make_unique<DynamicValue> (value);
    } else {
	copy = std::make_unique<DynamicValue> (current->getFloat ());
    }

    auto* result = copy.get ();
    this->addUniform (var, result);
    this->m_constantUniforms.insert (var->getName ());
    this->m_materialConstants[name] = result;
    this->m_scriptConstants.emplace (name, std::move (copy));
    return result;
}

void CPass::setScriptMaterialState (
    const BlendingMode blending, const std::pair<DepthtestMode, DepthwriteMode> depth, const CullingMode culling
) {
    this->m_scriptBlending = blending;
    this->m_scriptDepth = depth;
    this->m_scriptCulling = culling;
}

CPass::ScriptState CPass::getScriptState () const {
    ScriptState state {
	.blending = this->m_scriptBlending,
	.depth = this->m_scriptDepth,
	.culling = this->m_scriptCulling,
    };

    for (const auto& [name, value] : this->m_scriptConstants) {
	state.constants.emplace (name, *value);
    }

    return state;
}

void CPass::restoreScriptState (const ScriptState& state) {
    this->m_scriptBlending = state.blending;
    this->m_scriptDepth = state.depth;
    this->m_scriptCulling = state.culling;

    for (const auto& [name, value] : state.constants) {
	if (auto* target = this->getScriptConstant (name)) {
	    target->update (value, DynamicValue::UpdateSource::Script);
	}
    }
}

ShaderVariable* CPass::findActiveParameter (const std::string& name) const {
    const auto [vertex, fragment] = this->m_shader->findParameter (name);

    if (vertex == nullptr || fragment == nullptr || componentCount (*vertex) == componentCount (*fragment)) {
	return vertex == nullptr ? fragment : vertex;
    }

    const GLenum declared = this->getDeclaredUniformType (fragment->getName ());

    return declared != GL_NONE && componentCount (declared) == componentCount (*fragment) ? fragment : vertex;
}

void CPass::addConstantUniform (ShaderVariable* var, const UserSetting& setting) {
    const DynamicValue& value = *setting.value;
    const bool scalar = value.getType () == DynamicValue::UnderlyingType::Float
	|| value.getType () == DynamicValue::UnderlyingType::Int;

    // WE zero-pads constants ("0.7" on a vec4 is 0.7 0 0 0, tests/vecpad); bound user properties still spread
    if (scalar && componentCount (*var) > 1 && setting.property == nullptr && value.getAnimation () == nullptr) {
	auto& padded = this->m_paddedConstants.emplace_back (value.getFloat (), 0.0f, 0.0f, 0.0f);

	if (var->is<ShaderVariableVector2> ()) {
	    this->addUniform (var->getName (), reinterpret_cast<const glm::vec2*> (&padded));
	} else if (var->is<ShaderVariableVector3> ()) {
	    this->addUniform (var->getName (), reinterpret_cast<const glm::vec3*> (&padded));
	} else {
	    this->addUniform (var->getName (), static_cast<const glm::vec4*> (&padded));
	}
	return;
    }

    this->addUniform (var, &value);
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
