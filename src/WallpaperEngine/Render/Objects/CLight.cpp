#include "CLight.h"

#include "WallpaperEngine/Data/Model/Object.h"

using namespace WallpaperEngine::Render::Objects;

CLight::CLight (Wallpapers::CScene& scene, const Light& light, int slot) :
    CObject (scene, light), ScriptableObject (scene, light), m_light (light), m_slot (slot) {
    this->registerProperty ("color", *light.color->value);
    this->registerProperty ("intensity", *light.intensity->value);
    this->registerProperty ("radius", *light.radius->value);
    this->registerProperty ("visible", *light.visible->value);

    // sub_14025DA80
    const std::pair<const char*, const UserSettingUniquePtr&> properties[] = {
	{ "light", light.typeName },
	{ "castvolumetrics", light.castVolumetrics },
	{ "usecookie", light.useCookie },
	{ "castshadow", light.castShadow },
	{ "controlpoint", light.controlPoint },
	{ "cascadedistance0", light.cascadeDistance[0] },
	{ "cascadedistance1", light.cascadeDistance[1] },
	{ "cascadedistance2", light.cascadeDistance[2] },
	{ "volumetricsexponent", light.volumetricsExponent },
	{ "outercone", light.outerCone },
	{ "innercone", light.innerCone },
	{ "exponent", light.exponent },
	{ "lightsourcesize", light.lightSourceSize },
	{ "density", light.density },
    };

    for (const auto& [name, setting] : properties) {
	this->registerProperty (name, *setting->value);
    }
}

const Light& CLight::getLight () const { return this->m_light; }

int CLight::getSlot () const { return this->m_slot; }
