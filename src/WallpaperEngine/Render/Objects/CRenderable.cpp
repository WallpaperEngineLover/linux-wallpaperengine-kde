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

void CRenderable::registerEffectBuffers (const ImageEffect& effect, std::vector<std::shared_ptr<CFBO>> buffers) {
    // sub_1401EA500: FBO flag 2 clears the new buffer to its clear color
    for (size_t index = 0; index < buffers.size () && index < effect.effect->fbos.size (); index++) {
	if (effect.effect->fbos[index]->clearOnCreate) {
	    clearBuffer (*buffers[index], effect.effect->fbos[index]->clearColor);
	}
    }

    this->m_effectBuffers.insert_or_assign (&effect, std::move (buffers));
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

    // WE's own slip: the loop counter picks the buffer, not the index the name resolved to
    for (size_t index = 0; index < function->fbos.size () && index < buffers->second.size (); index++) {
	clearBuffer (*buffers->second[index], effect.effect->fbos[index]->clearColor);
    }

    return true;
}