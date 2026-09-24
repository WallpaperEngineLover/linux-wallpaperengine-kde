#include "CLight.h"

#include "WallpaperEngine/Data/Model/Object.h"

using namespace WallpaperEngine::Render::Objects;

CLight::CLight (Wallpapers::CScene& scene, const Light& light, int slot) :
    CObject (scene, light), ScriptableObject (scene, light), m_light (light), m_slot (slot) {
    this->registerProperty ("color", *light.color->value);
    this->registerProperty ("intensity", *light.intensity->value);
    this->registerProperty ("radius", *light.radius->value);
    this->registerProperty ("visible", *light.visible->value);
}

const Light& CLight::getLight () const { return this->m_light; }

int CLight::getSlot () const { return this->m_slot; }
