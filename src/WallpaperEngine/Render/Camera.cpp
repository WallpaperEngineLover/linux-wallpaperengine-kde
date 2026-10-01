#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "Camera.h"

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render;

Camera::Camera (Wallpapers::CScene& scene, const SceneData::Camera& camera) :
    m_width (0), m_height (0), m_eye (camera.configuration.eye), m_camera (camera), m_scene (scene) {
    // WE builds this view matrix for orthographic cameras too (sub_140159080)
    this->m_lookat = glm::lookAt (this->getEye (), this->getCenter (), this->getUp ());
}

Camera::~Camera () = default;

const glm::vec3& Camera::getCenter () const { return this->m_camera.configuration.center; }

const glm::vec3& Camera::getEye () const { return this->m_eye; }

const glm::vec3& Camera::getConfiguredEye () const { return this->m_camera.configuration.eye; }

const glm::vec3& Camera::getUp () const { return this->m_camera.configuration.up; }

const glm::mat4& Camera::getProjection () const { return this->m_projection; }

const glm::mat4& Camera::getLookAt () const { return this->m_lookat; }

const glm::mat4& Camera::getFullscreenProjection () const {
    return this->m_isOrthogonal ? this->m_fullscreen : this->m_projection;
}

const glm::mat4& Camera::getWorldView () const { return this->m_worldView; }

float Camera::getViewFov () const { return this->m_viewFov; }

const glm::mat4& Camera::getPerspectiveLayerViewProjection () const { return this->m_perspectiveLayer; }

bool Camera::isOrthogonal () const { return this->m_isOrthogonal; }

bool Camera::isPerspective () const { return this->m_isPerspective; }

const glm::mat4& Camera::getPerspective () const { return this->m_perspective; }

const glm::mat4& Camera::getView () const { return this->m_view; }

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

    // WE's 2D projection keeps -2000..2000 in depth (sub_140183A70), rotated layers and particles poke out of z = 0
    this->m_fullscreen = glm::ortho<float> (
	-this->m_canvasWidth / 2.0f, this->m_canvasWidth / 2.0f, -this->m_canvasHeight / 2.0f,
	this->m_canvasHeight / 2.0f, -2000.0f, 2000.0f
    );
    this->m_isOrthogonal = true;
    this->setWorldView (glm::mat4 (1.0f));
    this->setVisibleRegion ({}, this->m_canvasWidth / this->m_canvasHeight);
}

void Camera::setPerspectiveProjection (const float width, const float height) {
    this->m_width = width;
    this->m_height = height;
    this->m_canvasWidth = width;
    this->m_canvasHeight = height;

    this->m_projection = glm::ortho<float> (-width / 2.0f, width / 2.0f, -height / 2.0f, height / 2.0f, 0.0f, 1000.0f);
    this->m_view = this->m_lookat;
    this->m_lookat = glm::mat4 (1.0f);

    // XMMatrixPerspectiveFovRH with the vertical fov in degrees and the render target's aspect (sub_140146660)
    const float fov = glm::clamp (this->getFov (), 0.1f, 179.9f);
    this->m_perspective = glm::scale (glm::mat4 (1.0f), glm::vec3 (1.0f, -1.0f, 1.0f))
	* glm::perspective (glm::radians (fov), width / height, this->getNearZ (), this->getFarZ ());
    this->m_isPerspective = true;
}
namespace {
// the y flip between WE's world (y up) and the y down space 2D objects are laid out in here
const glm::mat4 kFlipY = glm::scale (glm::mat4 (1.0f), glm::vec3 (1.0f, -1.0f, 1.0f));
} // namespace

void Camera::setWorldView (const glm::mat4& view) {
    this->m_worldView = view;

    if (!this->m_isOrthogonal) {
	return;
    }

    // WE's 2D world has its origin at the bottom left corner, the layout here is centered
    const glm::vec3 center (this->m_width / 2.0f, this->m_height / 2.0f, 0.0f);
    this->m_lookat = kFlipY * glm::translate (glm::mat4 (1.0f), -center) * view
	* glm::translate (glm::mat4 (1.0f), center) * kFlipY;
}

void Camera::setZoom (const float zoom) {
    this->m_zoom = zoom;
    this->m_projection = this->m_orthogonal * glm::scale (glm::mat4 (1.0f), glm::vec3 (zoom, zoom, 1.0f));
}

void Camera::setPerspectiveView (const glm::mat4& view, const float fov) {
    const float radians = glm::radians (glm::clamp (fov, 0.1f, 179.9f));

    this->m_worldView = view;
    this->m_viewFov = fov;
    this->m_eye = glm::vec3 (glm::inverse (view)[3]);
    this->m_view = view;
    const float aspect = this->m_outputAspect > 0.0f ? this->m_outputAspect : this->m_width / this->m_height;
    this->m_perspective = kFlipY * glm::perspective (radians, aspect, this->getNearZ (), this->getFarZ ());

    // "perspective" layers in 3D scenes: sub_140183A70 sets the renderer fov to 2 atan (tan (fov / 2) / 2000), so
    // sub_1401E5B60 always backs off to 2000, keeps the view's rotation and x/y (the visible region offsets are 0 in
    // 3D scenes) and projects with that narrow fov, near 5, far 15000
    glm::mat4 layerView = view;
    layerView[3][2] = -2000.0f;
    this->m_perspectiveLayer = kFlipY
	* glm::perspective (2.0f * std::atan (std::tan (radians / 2.0f) / 2000.0f), aspect, 5.0f, 15000.0f)
	* layerView;
}

void Camera::setVisibleRegion (const glm::vec4& margins, const float outputAspect) {
    this->m_margins = margins;
    this->m_outputAspect = outputAspect;

    if (!this->m_isOrthogonal) {
	return;
    }

    const float halfWidth = this->m_canvasWidth / 2.0f;
    const float halfHeight = this->m_canvasHeight / 2.0f;

    this->m_orthogonal = glm::ortho<float> (
	-halfWidth + margins.x, halfWidth - margins.y, -halfHeight + margins.z, halfHeight - margins.w, -2000.0f,
	2000.0f
    );
    this->setZoom (this->m_zoom);
}

void Camera::updatePerspectiveLayers () {
    if (!this->m_isOrthogonal) {
	return;
    }

    const float width = this->m_width;
    const float height = this->m_height;
    const glm::vec4& margins = this->m_margins;
    const float visibleHeight = std::max (this->m_canvasHeight - margins.z - margins.w, 1.0f);

    // renderer +248/+252: the visible region's center, where the layer camera moves to
    const float centerX = width / 2.0f + (margins.x - margins.y) / 2.0f;
    const float centerY = height / 2.0f + (margins.z - margins.w) / 2.0f;

    // sub_1401E5B60: the camera backs off until the visible height fills the fov, near 5, far max (15000, d + 1000)
    const float fov = glm::radians (
	glm::clamp (this->m_camera.projection.perspectiveOverrideFov->value->getFloat (), 0.1f, 179.9f)
    );
    // the zoom is part of the ortho projection WE derives the distance from
    const float distance = visibleHeight / (2.0f * this->m_zoom * std::tan (fov / 2.0f));
    const float farZ = std::max (15000.0f, distance + 1000.0f);

    glm::mat4 view = this->m_worldView;
    view[3][0] -= centerX;
    view[3][1] -= centerY;
    view[3][2] = -distance;

    const float aspect = this->m_outputAspect > 0.0f ? this->m_outputAspect : this->m_canvasWidth / this->m_canvasHeight;
    const glm::mat4 projection = glm::perspective (fov, aspect, 5.0f, farZ);

    const glm::vec3 center (width / 2.0f, height / 2.0f, 0.0f);
    this->m_perspectiveLayer = kFlipY * projection * view * glm::translate (glm::mat4 (1.0f), center) * kFlipY;
}
