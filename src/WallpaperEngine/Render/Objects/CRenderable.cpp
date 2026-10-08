#include "CRenderable.h"

#include "WallpaperEngine/Data/Model/Effect.h"
#include "WallpaperEngine/Data/Model/Material.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Data/Parsers/MaterialParser.h"

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render::Objects;
using namespace WallpaperEngine::Render::Objects::Effects;
using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Data::Builders;

CRenderable::CRenderable (Wallpapers::CScene& scene, const Object& object, const Material& material) :
    CObject (scene, object), Render::FBOProvider (&scene), m_material (material) { }

void CRenderable::detectTexture () {
    if (TextureMap* textures = &(*this->m_material.passes.begin ())->textures; !textures->empty ()) {
	std::string textureName = textures->begin ()->second;

	if (textureName.find ("_rt_") == 0 || textureName.find ("_alias_") == 0) {
	    this->m_texture = this->getScene ().findFBO (textureName);
	} else {
	    this->m_texture = this->getContext ().resolveTexture (textureName, this->getScene ().getScene ().project);
	}
    }
}

void CRenderable::setup () {
    CObject::setup ();

    this->m_animationTime = 0.0f;

    for (const auto& cur : this->getTexture ()->getFrames ()) {
	this->m_animationTime += cur->frametime;
    }
}

std::shared_ptr<const TextureProvider> CRenderable::getTexture () const { return this->m_texture; }

double CRenderable::getAnimationTime () const { return this->m_animationTime; }

namespace {
void clearBuffer (const CFBO& buffer, const glm::vec4& color) {
    GLint previousFramebuffer = 0;
    GLfloat previousClearColor[4] = {};
    GLboolean previousColorMask[4] = {};
    glGetIntegerv (GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    glGetFloatv (GL_COLOR_CLEAR_VALUE, previousClearColor);
    glGetBooleanv (GL_COLOR_WRITEMASK, previousColorMask);

    glBindFramebuffer (GL_FRAMEBUFFER, buffer.getFramebuffer ());
    glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor (color.r, color.g, color.b, color.a);
    glClear (GL_COLOR_BUFFER_BIT);

    glClearColor (previousClearColor[0], previousClearColor[1], previousClearColor[2], previousClearColor[3]);
    glColorMask (previousColorMask[0], previousColorMask[1], previousColorMask[2], previousColorMask[3]);
    glBindFramebuffer (GL_FRAMEBUFFER, previousFramebuffer);
}
} // namespace

void CRenderable::registerEffectBuffers (const ImageEffect& effect, EffectBuffers buffers) {
    for (const auto& [base, buffer] : buffers) {
	if (base->clearOnCreate) {
	    clearBuffer (*buffer, base->clearColor);
	}
    }

    this->m_effectBuffers.insert_or_assign (&effect, std::move (buffers));
}

void CRenderable::registerEffectMaterial (const ImageEffect& effect, size_t passIndex, Effects::CPass* pass) {
    auto& passes = this->m_effectMaterials[&effect];

    if (passes.size () <= passIndex) {
	passes.resize (passIndex + 1, nullptr);
    }

    if (passes[passIndex] != nullptr) {
	return;
    }

    passes[passIndex] = pass;

    if (const auto it = this->m_releasedScriptStates.find ({ &effect, passIndex });
	it != this->m_releasedScriptStates.end ()) {
	pass->restoreScriptState (it->second);
	this->m_releasedScriptStates.erase (it);
    }
}

void CRenderable::releaseEffectMaterials () {
    for (const auto& [effect, passes] : this->m_effectMaterials) {
	for (size_t passIndex = 0; passIndex < passes.size (); passIndex++) {
	    if (passes[passIndex] != nullptr) {
		this->m_releasedScriptStates.insert_or_assign (
		    { effect, passIndex }, passes[passIndex]->getScriptState ()
		);
	    }
	}
    }

    this->m_effectMaterials.clear ();
}

Effects::CPass* CRenderable::getEffectMaterial (const ImageEffect& effect, size_t passIndex) const {
    const auto it = this->m_effectMaterials.find (&effect);

    return it == this->m_effectMaterials.end () || passIndex >= it->second.size () ? nullptr : it->second[passIndex];
}

bool CRenderable::executeEffectFunction (const ImageEffect& effect, const std::string& name) const {
    const auto buffers = this->m_effectBuffers.find (&effect);

    if (buffers == this->m_effectBuffers.end ()) {
	return false;
    }

    const auto& functions = effect.effect->functions;
    const auto function
	= std::ranges::find_if (functions, [&name] (const EffectFunction& cur) { return cur.name == name; });

    if (function == functions.end ()) {
	return true;
    }

    // WE's slip: the loop counter picks the buffer, not the index the name resolved to
    const auto made = std::ranges::count_if (function->fbos, [&effect, &buffers] (const int index) {
	return std::ranges::any_of (buffers->second, [&effect, index] (const auto& buffer) {
	    return buffer.first == effect.effect->fbos[index].get ();
	});
    });

    for (size_t index = 0; index < static_cast<size_t> (made) && index < buffers->second.size (); index++) {
	clearBuffer (*buffers->second[index].second, buffers->second[index].first->clearColor);
    }

    return true;
}