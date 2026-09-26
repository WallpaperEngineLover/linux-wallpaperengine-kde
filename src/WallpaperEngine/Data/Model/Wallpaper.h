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

/** A key of a scene camera path: where the camera is at time seconds into the path */
struct CameraPathKey {
    float time;
    glm::vec3 eye;
    glm::vec3 center;
    glm::vec3 up;
    float zoom;
};

/** One path of the files listed in scene.json's "camera" "paths", played one after another */
struct CameraPath {
    std::vector<CameraPathKey> keys;
    float duration;
};

struct SceneData {
    struct {
	UserSettingUniquePtr ambient;
	UserSettingUniquePtr skylight;
	UserSettingUniquePtr clear;
    } colors;
    /** Distance and height fog ("fogdistance*"/"fogheight*" in general) */
    struct {
	UserSettingUniquePtr distanceEnabled;
	UserSettingUniquePtr distanceColor;
	UserSettingUniquePtr distanceStart;
	UserSettingUniquePtr distanceEnd;
	UserSettingUniquePtr distanceStartDensity;
	UserSettingUniquePtr distanceEndDensity;
	UserSettingUniquePtr heightEnabled;
	UserSettingUniquePtr heightColor;
	UserSettingUniquePtr heightStart;
	UserSettingUniquePtr heightEnd;
	UserSettingUniquePtr heightStartDensity;
	UserSettingUniquePtr heightEndDensity;
    } fog;
    struct Camera {
	UserSettingUniquePtr fade;
	/** Whether the software's preview UI is allowed to show this background */
	bool preview;

	struct {
	    UserSettingUniquePtr enabled;
	    UserSettingUniquePtr strength;
	    UserSettingUniquePtr threshold;
	    /** "hdr": HDR rendering and bloom, only with WE's ultra post processing */
	    UserSettingUniquePtr hdr;
	    UserSettingUniquePtr hdrStrength;
	    UserSettingUniquePtr hdrThreshold;
	    UserSettingUniquePtr hdrFeather;
	    UserSettingUniquePtr hdrScatter;
	    UserSettingUniquePtr hdrIterations;
	    UserSettingUniquePtr tint;
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

	std::vector<CameraPath> paths;

	struct {
	    int width;
	    int height;
	    bool isAuto;
	    bool isPerspective;
	    UserSettingUniquePtr nearz;
	    UserSettingUniquePtr farz;
	    UserSettingUniquePtr fov;
	    /** fov of the perspective layers in a 2D scene */
	    UserSettingUniquePtr perspectiveOverrideFov;
	    /** 2D scenes: magnification around the scene center, times the active camera object's zoom */
	    UserSettingUniquePtr zoom;
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
