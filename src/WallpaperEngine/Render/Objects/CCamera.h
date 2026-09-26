#pragma once

#include <optional>
#include <random>
#include <vector>

#include <glm/mat4x4.hpp>

#include "WallpaperEngine/Scripting/ScriptableObject.h"

namespace WallpaperEngine::Render::Objects {
using namespace WallpaperEngine::Data::Model;

/**
 * A "camera" object draws nothing, the last visible one in the scene sets the view: its position is the eye, it looks
 * down its own -z axis with its y axis as up (wallpaper64.exe 2.8.42 sub_1401891A0). With a "path" file it plays
 * those timelines one after another instead of holding still (sub_1401F2AD0)
 */
class CCamera final : public Scripting::ScriptableObject {
public:
    struct Pose {
	/** Replaces the camera's world matrix */
	glm::mat4 world;
	/** zoom in 2D scenes, fov in 3D ones, from the timeline */
	float zoom;
	float fov;
    };

    CCamera (Wallpapers::CScene& scene, const SceneCamera& camera);

    [[nodiscard]] const SceneCamera& getCamera () const;
    /**
     * Advances the running timeline by dt and samples it. world is the camera's own world matrix, parent its
     * parent's; nothing comes back when the camera has no path
     */
    std::optional<Pose> updateTimeline (float dt, const glm::mat4& world, const glm::mat4& parent);

private:
    [[nodiscard]] int nextTimeline ();
    void advanceClock (const CameraTimeline& timeline, float dt);

    const SceneCamera& m_camera;
    int m_active = -1;
    /** Last entry the sequential queue handed out */
    int m_cursor = -1;
    float m_time = 0.0f;
    bool m_reverse = false;
    bool m_ended = false;
    std::vector<int> m_bag;
    std::optional<int> m_last;
    std::minstd_rand m_random;
};
} // namespace WallpaperEngine::Render::Objects
