#pragma once

#include <memory>

#include <glm/glm.hpp>
#include <utility>

#include "Object.h"
#include "Types.h"
#include "WallpaperEngine/Data/Utils/TypeCaster.h"

namespace WallpaperEngine::Data::Model {
using namespace WallpaperEngine::Data::Utils;

struct WallpaperData {
    std::string filename;
    Project& project;
};

class Wallpaper : public TypeCaster, public WallpaperData {
public:
    explicit Wallpaper (WallpaperData data) noexcept : TypeCaster (), WallpaperData (std::move (data)) { };
    ~Wallpaper () override = default;
};

class Video final : public Wallpaper {
public:
    explicit Video (WallpaperData data) noexcept : Wallpaper (std::move (data)) { }

    ~Video () override = default;
};

class Web final : public Wallpaper {
public:
    explicit Web (WallpaperData data) noexcept : Wallpaper (std::move (data)) { }

    ~Web () override = default;
};

struct SceneData {
    struct {
	UserSettingUniquePtr ambient;
	UserSettingUniquePtr skylight;
	UserSettingUniquePtr clear;
    } colors;
    struct Camera {
	UserSettingUniquePtr fade;
	/** Whether the software's preview UI is allowed to show this background */
	bool preview;

	struct {
	    UserSettingUniquePtr enabled;
	    UserSettingUniquePtr strength;
	    UserSettingUniquePtr threshold;
	} bloom;
	struct {
	    UserSettingUniquePtr enabled;
	    UserSettingUniquePtr amount;
	    UserSettingUniquePtr delay;
	    UserSettingUniquePtr mouseInfluence;
	} parallax;

	struct {
	    UserSettingUniquePtr enabled;
	    UserSettingUniquePtr amplitude;
	    UserSettingUniquePtr roughness;
	    UserSettingUniquePtr speed;
	} shake;

	struct {
	    glm::vec3 center;
	    glm::vec3 eye;
	    glm::vec3 up;
	} configuration;

	struct {
	    int width;
	    int height;
	    bool isAuto;
	    UserSettingUniquePtr nearz;
	    UserSettingUniquePtr farz;
	    UserSettingUniquePtr fov;
	} projection;
    } camera;

    ObjectList objects;
};

class Scene final : public Wallpaper, public SceneData {
public:
    explicit Scene (WallpaperData data, SceneData sceneData) noexcept :
	Wallpaper (std::move (data)), SceneData (std::move (sceneData)) { }

    ~Scene () override = default;
};
} // namespace WallpaperEngine::Data::Model
