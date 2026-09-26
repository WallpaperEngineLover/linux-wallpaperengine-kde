#include "CCamera.h"

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Scripting/AnimationSystem.h"

using namespace WallpaperEngine::Render::Objects;

CCamera::CCamera (Wallpapers::CScene& scene, const SceneCamera& camera) :
    CObject (scene, camera), ScriptableObject (scene, camera), m_camera (camera),
    m_random (static_cast<unsigned> (camera.id)) {
    this->registerProperty ("fov", *camera.fov->value);
    this->registerProperty ("zoom", *camera.zoom->value);
}

const SceneCamera& CCamera::getCamera () const { return this->m_camera; }

int CCamera::nextTimeline () {
    const auto& timelines = this->m_camera.timelines;
    const int count = static_cast<int> (timelines.size ());

    if (this->m_camera.queueMode == CameraQueueMode::Sequential) {
	for (int tries = 0; tries < count; tries++) {
	    this->m_cursor = (this->m_cursor + 1) % count;

	    if (timelines[this->m_cursor].visible) {
		return this->m_cursor;
	    }
	}

	return -1;
    }

    // random: a bag of the visible entries, refilled once empty, never the same entry twice in a row
    if (this->m_bag.empty ()) {
	for (int index = 0; index < count; index++) {
	    if (timelines[index].visible) {
		this->m_bag.push_back (index);
	    }
	}
    }

    if (this->m_bag.empty ()) {
	return -1;
    }

    const int size = static_cast<int> (this->m_bag.size ());
    int pick = std::uniform_int_distribution<int> (0, size - 1) (this->m_random);

    for (int tries = 0; tries < size && this->m_last == this->m_bag[pick]; tries++) {
	pick = (pick + 1) % size;
    }

    const int chosen = this->m_bag[pick];
    this->m_bag.erase (this->m_bag.begin () + pick);
    this->m_last = chosen;

    return chosen;
}

void CCamera::advanceClock (const CameraTimeline& timeline, const float dt) {
    // sub_1401A9F60: single stops at the end, loop wraps, mirror bounces
    const float length = timeline.length / timeline.fps;

    if (timeline.startPaused || this->m_ended || length <= 0.0f) {
	return;
    }

    float time = this->m_time + (this->m_reverse ? -dt : dt);

    switch (timeline.mode) {
	case PropertyAnimation::Mode::Single:
	    if (time >= length) {
		time = length;
		this->m_ended = true;
	    }
	    break;
	case PropertyAnimation::Mode::Loop:
	    if (time < 0.0f) {
		time += length;
	    }
	    if (time >= length) {
		time = std::fmod (time, length);
	    }
	    break;
	case PropertyAnimation::Mode::Mirror:
	    if (this->m_reverse && time <= 0.0f) {
		time = -time;
		this->m_reverse = false;
	    } else if (!this->m_reverse && time >= length) {
		time = length - (time - length);
		this->m_reverse = true;
	    }
	    break;
    }

    this->m_time = time;
}

std::optional<CCamera::Pose> CCamera::updateTimeline (const float dt, const glm::mat4& world, const glm::mat4& parent) {
    const auto& timelines = this->m_camera.timelines;

    if (timelines.empty ()) {
	return std::nullopt;
    }

    // a finished entry starts over and hands over to the next one in the queue
    if (this->m_ended) {
	this->m_ended = false;
	this->m_time = 0.0f;
	this->m_active = -1;
    }

    if (this->m_active < 0) {
	this->m_active = this->nextTimeline ();

	if (this->m_active < 0) {
	    return std::nullopt;
	}

	this->m_time = 0.0f;
	this->m_reverse = false;
    }

    const auto& timeline = timelines[this->m_active];
    this->advanceClock (timeline, dt);

    const int frames = static_cast<int> (timeline.length);
    const float frame = this->m_time * timeline.fps;
    const auto sample = [&] (const std::vector<AnimationKeyframe>& keys, float& target) {
	if (!keys.empty ()) {
	    target = Scripting::evaluateAnimationCurve (keys, frame, timeline.fps, frames);
	}
    };

    // components without a track keep the camera's own transform, the center 5 units down its view axis
    glm::vec3 eye (world[3]);
    glm::vec3 center = eye - glm::normalize (glm::vec3 (world[2])) * 5.0f;
    glm::vec3 up = glm::normalize (glm::vec3 (world[1]));

    for (int component = 0; component < 3; component++) {
	sample (timeline.center[component], center[component]);
	sample (timeline.eye[component], eye[component]);
	sample (timeline.up[component], up[component]);
    }

    Pose pose { .world = parent * glm::inverse (glm::lookAt (eye, center, up)), .zoom = 1.0f, .fov = 50.0f };
    sample (timeline.zoom, pose.zoom);
    sample (timeline.fov, pose.fov);

    return pose;
}
