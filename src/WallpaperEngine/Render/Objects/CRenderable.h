#pragma once

#include "WallpaperEngine/Render/CObject.h"
#include "WallpaperEngine/Render/FBOProvider.h"
#include "WallpaperEngine/Render/Objects/Effects/CPass.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include <optional>

#include "WallpaperEngine/Render/Shaders/Shader.h"

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render;

namespace WallpaperEngine::Render::Objects {
class CRenderable : virtual public CObject, public FBOProvider {
    friend CObject;

public:
    CRenderable (Wallpapers::CScene& scene, const Object& object, const Material& material);

    [[nodiscard]] std::shared_ptr<const TextureProvider> getTexture () const;

    [[nodiscard]] double getAnimationTime () const;

    void setup () override;

    [[nodiscard]] virtual const float& getBrightness () const = 0;
    [[nodiscard]] virtual const float& getUserAlpha () const = 0;
    [[nodiscard]] virtual const float& getAlpha () const = 0;
    [[nodiscard]] virtual const glm::vec3& getColor () const = 0;
    [[nodiscard]] virtual const glm::vec4& getColor4 () const = 0;
    [[nodiscard]] virtual const glm::vec3& getCompositeColor () const = 0;
    /** Frame (index in the texture's frame list) every animated texture shows while this draws, instead of the
     *  shared clock (renderer +4908, sub_14015F0D0) */
    [[nodiscard]] virtual std::optional<int> getTextureFrameOverride () const { return std::nullopt; }

    /** The buffers made for an effect's fbos, in their order; ones with a complete "clear" are cleared now */
    void registerEffectBuffers (const ImageEffect& effect, std::vector<std::shared_ptr<CFBO>> buffers);
    /**
     * IEffect.executeMaterialFunction (wallpaper64.exe 2.8.42 sub_1401EE3A0): a "clear" function clears as many of
     * the effect's buffers as it names, but always the first ones, each to its own clear color. False when the effect
     * isn't one of this object's
     */
    bool executeEffectFunction (const ImageEffect& effect, const std::string& name) const;

protected:
    void detectTexture ();

    double m_animationTime = 0.0;

    std::shared_ptr<const TextureProvider> m_texture = nullptr;
    const Material& m_material;
    std::map<const ImageEffect*, std::vector<std::shared_ptr<CFBO>>> m_effectBuffers = {};
};
}
