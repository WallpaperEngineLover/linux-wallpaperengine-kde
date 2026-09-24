#pragma once

#include "WallpaperEngine/Scripting/ScriptableObject.h"

namespace WallpaperEngine::Render::Objects {
using namespace WallpaperEngine::Data::Model;

/**
 * A light draws nothing itself, it only feeds the scene's light uniforms. Old "point" lights take one of the four
 * fixed slots lit image shaders read (the first free one when created, a fifth light shares slot 0 like in WE)
 */
class CLight final : public Scripting::ScriptableObject {
public:
    CLight (Wallpapers::CScene& scene, const Light& light, int slot);

    [[nodiscard]] const Light& getLight () const;
    [[nodiscard]] int getSlot () const;

private:
    const Light& m_light;
    int m_slot;
};
} // namespace WallpaperEngine::Render::Objects
