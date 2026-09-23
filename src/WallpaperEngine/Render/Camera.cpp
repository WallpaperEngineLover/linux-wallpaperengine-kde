#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "Camera.h"

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render;

Camera::Camera (Wallpapers::CScene& scene, const SceneData::Camera& camera) :
    m_width (0), m_height (0), m_camera (camera), m_scene (scene) {
    // TODO: ensure this is only used for non-orthographic cameras, it throws off points otherwise
    this->m_lookat = glm::lookAt (this->getEye (), this->getCenter (), this->getUp ());
}

Camera::~Camera () = default;

const glm::vec3& Camera::getCenter () const { return this->m_camera.configuration.center; }

const glm::vec3& Camera::getEye () const { return this->m_camera.configuration.eye; }

const glm::vec3& Camera::getUp () const { return this->m_camera.configuration.up; }

const glm::mat4& Camera::getProjection () const { return this->m_projection; }

const glm::mat4& Camera::getLookAt () const { return this->m_lookat; }

bool Camera::isOrthogonal () const { return this->m_isOrthogonal; }

Wallpapers::CScene& Camera::getScene () const { return this->m_scene; }

float Camera::getWidth () const { return this->m_width; }

float Camera::getHeight () const { return this->m_height; }

float Camera::getCanvasWidth () const { return this->m_canvasWidth; }

float Camera::getCanvasHeight () const { return this->m_canvasHeight; }

float Camera::getFov () const { return this->m_camera.projection.fov->value->getFloat (); }

float Camera::getNearZ () const { return this->m_camera.projection.nearz->value->getFloat (); }

float Camera::getFarZ () const { return this->m_camera.projection.farz->value->getFloat (); }

void Camera::setOrthogonalProjection (
    const float width, const float height, const float canvasWidth, const float canvasHeight
) {
    this->m_width = width;
    this->m_height = height;
    this->m_canvasWidth = canvasWidth > 0.0f ? canvasWidth : width;
    this->m_canvasHeight = canvasHeight > 0.0f ? canvasHeight : height;

    float nearz = this->m_camera.projection.nearz->value->getFloat ();
    float farz = this->m_camera.projection.farz->value->getFloat ();

    this->m_projection = glm::ortho<float> (
	-this->m_canvasWidth / 2.0, this->m_canvasWidth / 2.0, -this->m_canvasHeight / 2.0, this->m_canvasHeight / 2.0,
	nearz, farz
    );
    this->m_projection = glm::translate (this->m_projection, this->getEye ());
    this->m_isOrthogonal = true;
}