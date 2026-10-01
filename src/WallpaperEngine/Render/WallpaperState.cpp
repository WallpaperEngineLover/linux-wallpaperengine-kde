#include "WallpaperState.h"
#include "TextureProvider.h"
#include "WallpaperEngine/Logging/Log.h"
#include <algorithm>
#include <cmath>

using namespace WallpaperEngine::Render;

WallpaperState::WallpaperState (const TextureUVsScaling& textureUVsMode, const uint32_t& clampMode) :
    m_textureUVsMode (textureUVsMode), m_clampingMode (clampMode) { }

std::optional<WallpaperState::TextureUVsScaling> WallpaperState::parseScalingMode (const std::string& value) {
    if (value == "stretch") {
	return TextureUVsScaling::StretchUVs;
    }
    if (value == "fit") {
	return TextureUVsScaling::ZoomFitUVs;
    }
    if (value == "fill") {
	return TextureUVsScaling::ZoomFillUVs;
    }
    if (value == "center") {
	return TextureUVsScaling::CenterUVs;
    }
    if (value == "free") {
	return TextureUVsScaling::FreeUVs;
    }
    if (value == "default") {
	return TextureUVsScaling::DefaultUVs;
    }

    return std::nullopt;
}

bool WallpaperState::hasChanged (
    const glm::ivec4& viewport, const bool& vflip, const int& projectionWidth, const int& projectionHeight
) const {
    return this->m_uvsDirty || this->m_viewport.width != viewport.z || this->m_viewport.height != viewport.w
	|| this->m_projection.width != projectionWidth || this->m_projection.height != projectionHeight
	|| this->m_vflip != vflip;
}

void WallpaperState::resetUVs () {
    this->m_UVs.ustart = 0;
    this->m_UVs.uend = 1;

    if (m_vflip) {
	this->m_UVs.vstart = 0.0f;
	this->m_UVs.vend = 1.0f;
    } else {
	this->m_UVs.vstart = 1.0f;
	this->m_UVs.vend = 0.0f;
    }
}

glm::vec4 WallpaperState::alignmentMargins (const int mode) const {
    const float width = static_cast<float> (this->getProjectionWidth ());
    const float height = static_cast<float> (this->getProjectionHeight ());
    const float outputWidth = static_cast<float> (this->getViewportWidth ());
    const float outputHeight = static_cast<float> (this->getViewportHeight ());
    const float sceneAspect = width / height;
    const float outputAspect = outputWidth / outputHeight;
    const float position = this->m_alignment.position;
    const float x = this->m_alignment.x;
    const float y = this->m_alignment.y;
    float left = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    float top = 0.0f;

    switch (mode) {
	case 0:
	    // cover: the axis that sticks out is cut, position 0 keeps the right or the top edge
	    if (sceneAspect > outputAspect) {
		const float half = (width - height / outputHeight * outputWidth) * 0.5f;
		left = 2.0f * (1.0f - position) * half;
		right = 2.0f * position * half;
	    } else {
		const float half = (height - width / outputWidth * outputHeight) * 0.5f;
		top = 2.0f * position * half;
		bottom = 2.0f * (1.0f - position) * half;
	    }
	    break;
	case 1:
	    // fit: the other axis gets bars, position 0 puts the scene on the left or the top
	    if (outputAspect > sceneAspect) {
		const float half = (width - height / outputHeight * outputWidth) * 0.5f;
		left = 2.0f * position * half;
		right = 2.0f * (1.0f - position) * half;
	    } else {
		const float half = (height - width / outputWidth * outputHeight) * 0.5f;
		top = 2.0f * position * half;
		bottom = 2.0f * (1.0f - position) * half;
	    }
	    break;
	case 3:
	    // center: native size, x/y 0 keep the left and top edges
	    left = x * (width - outputWidth);
	    right = (1.0f - x) * (width - outputWidth);
	    top = y * (height - outputHeight);
	    bottom = (1.0f - y) * (height - outputHeight);
	    break;
	case 4:
	    {
		// free: x/y slide the native size scene from past one edge to past the other, the zoom scales the shown
		// region by (2 - zoom)^4, no smaller than 1% of it
		const float baseLeft = width - x * (outputWidth + width);
		const float baseTop = height - y * (outputHeight + height);
		const float baseRight = width - (1.0f - x) * (outputWidth + width);
		const float baseBottom = height - (1.0f - y) * (outputHeight + height);
		const float scale = std::max (-0.99f, std::pow (2.0f - this->m_alignment.zoom, 4.0f) - 1.0f);
		const float growY = (height - baseTop - baseBottom) * scale;
		const float growX = (width - baseRight - baseLeft) * scale;

		left = baseLeft - growX * x;
		right = baseRight - growX * (1.0f - x);
		top = baseTop - growY * y;
		bottom = baseBottom - growY * (1.0f - y);
		break;
	    }
	default:
	    break;
    }

    return { left, right, bottom, top };
}

void WallpaperState::setMargins (const glm::vec4& margins) {
    const float width = static_cast<float> (this->getProjectionWidth ());
    const float height = static_cast<float> (this->getProjectionHeight ());

    this->m_UVs.ustart = margins.x / width;
    this->m_UVs.uend = 1.0f - margins.y / width;

    // v runs top down in a flipped state, bottom up otherwise. The scene is laid out y down here, so WE's top margin
    // (y up) is the one at the v origin of the canvas
    if (this->m_vflip) {
	this->m_UVs.vstart = margins.z / height;
	this->m_UVs.vend = 1.0f - margins.w / height;
    } else {
	this->m_UVs.vstart = 1.0f - margins.z / height;
	this->m_UVs.vend = margins.w / height;
    }
}

template <> void WallpaperState::updateTextureUVs<WallpaperState::TextureUVsScaling::StretchUVs> () {
    this->resetUVs ();
}

template <> void WallpaperState::updateTextureUVs<WallpaperState::TextureUVsScaling::ZoomFillUVs> () {
    this->setMargins (this->alignmentMargins (0));
}

template <> void WallpaperState::updateTextureUVs<WallpaperState::TextureUVsScaling::ZoomFitUVs> () {
    this->setMargins (this->alignmentMargins (1));
}

template <> void WallpaperState::updateTextureUVs<WallpaperState::TextureUVsScaling::DefaultUVs> () {
    // WE's default scaling (mode 0 in sub_140183A70) crops whichever axis overflows so the scene covers the output,
    // the same as fill: 2977423343's 5760x2610 scene showed clamped edges letterboxed into 16:9
    this->updateTextureUVs<TextureUVsScaling::ZoomFillUVs> ();
}

template <> void WallpaperState::updateTextureUVs<WallpaperState::TextureUVsScaling::CenterUVs> () {
    // no scale factor at all: crops to native size if the wallpaper is bigger than the viewport, overflows past [0,1]
    // (relying on border clamping) to letterbox it if it's smaller
    this->setMargins (this->alignmentMargins (3));
}

template <> void WallpaperState::updateTextureUVs<WallpaperState::TextureUVsScaling::FreeUVs> () {
    this->setMargins (this->alignmentMargins (4));
}

template <WallpaperState::TextureUVsScaling T> void WallpaperState::updateTextureUVs () {
    sLog.exception (
	"Using generic template for scaling is not allowed. Write specialization template for your scaling mode.\
     This message is for developers, if you are just user it's a bug."
    );
}

WallpaperState::TextureUVsScaling WallpaperState::getTextureUVsScaling () const { return this->m_textureUVsMode; }

uint32_t WallpaperState::getClampingMode () const { return this->m_clampingMode; }

void WallpaperState::setTextureUVsStrategy (WallpaperState::TextureUVsScaling strategy) {
    if (this->m_textureUVsMode == strategy) {
	return;
    }

    this->m_textureUVsMode = strategy;
    this->m_uvsDirty = true;
}

float WallpaperState::getZoom () const { return this->m_zoom; }

void WallpaperState::setZoom (float zoom) {
    zoom = std::clamp (zoom, 0.1f, 5.0f);

    if (this->m_zoom == zoom) {
	return;
    }

    this->m_zoom = zoom;
    this->m_uvsDirty = true;
}

// Shrinks (zoom > 1) or grows (zoom < 1) the already-computed UV window around its own center, on top of
// whatever the scaling mode picked. Works regardless of axis direction (vflip swaps which of start/end is
// larger) since it only depends on the midpoint and half-range, not their sign.
namespace {
void applyZoomToAxis (float& start, float& end, float zoom) {
    const float center = (start + end) / 2.0f;
    const float half = (end - start) / 2.0f;

    start = center - half / zoom;
    end = center + half / zoom;
}

// Slides the UV window toward one edge without changing its size, offset in [-1, 1]; no-op when nothing is cropped on
// the axis
void applyOffsetToAxis (float& start, float& end, float offset) {
    if (offset == 0.0f) {
	return;
    }

    const bool inverted = start > end;
    float lo = inverted ? end : start;
    float hi = inverted ? start : end;
    const float size = hi - lo;
    const float slack = std::max (0.0f, 1.0f - size);

    if (slack <= 0.0f) {
	return;
    }

    const float shift = std::clamp (offset, -1.0f, 1.0f) * slack / 2.0f;
    lo = std::clamp (lo + shift, 0.0f, 1.0f - size);
    hi = lo + size;

    if (inverted) {
	start = hi;
	end = lo;
    } else {
	start = lo;
	end = hi;
    }
}
} // namespace

float WallpaperState::getOffsetX () const { return this->m_offsetX; }

float WallpaperState::getOffsetY () const { return this->m_offsetY; }

void WallpaperState::setOffset (float offsetX, float offsetY) {
    offsetX = std::clamp (offsetX, -1.0f, 1.0f);
    offsetY = std::clamp (offsetY, -1.0f, 1.0f);

    if (this->m_offsetX == offsetX && this->m_offsetY == offsetY) {
	return;
    }

    this->m_offsetX = offsetX;
    this->m_offsetY = offsetY;
    this->m_uvsDirty = true;
}

const WallpaperState::Alignment& WallpaperState::getAlignment () const { return this->m_alignment; }

void WallpaperState::setAlignment (const Alignment& alignment) {
    if (this->m_alignment == alignment) {
	return;
    }

    this->m_alignment = alignment;
    this->m_uvsDirty = true;
}

int WallpaperState::getViewportWidth () const { return this->m_viewport.width; }

int WallpaperState::getViewportHeight () const { return this->m_viewport.height; }

int WallpaperState::getProjectionWidth () const { return this->m_projection.width; }

int WallpaperState::getProjectionHeight () const { return this->m_projection.height; }

void WallpaperState::updateState (
    const glm::ivec4& viewport, const bool& vflip, const int& projectionWidth, const int& projectionHeight
) {
    this->m_viewport.width = viewport.z;
    this->m_viewport.height = viewport.w;
    this->m_vflip = vflip;
    this->m_projection.width = projectionWidth;
    this->m_projection.height = projectionHeight;
    this->m_uvsDirty = false;

    switch (this->getTextureUVsScaling ()) {
	case WallpaperState::TextureUVsScaling::StretchUVs:
	    this->updateTextureUVs<WallpaperState::TextureUVsScaling::StretchUVs> ();
	    break;
	case WallpaperState::TextureUVsScaling::ZoomFillUVs:
	    this->updateTextureUVs<WallpaperState::TextureUVsScaling::ZoomFillUVs> ();
	    break;
	case WallpaperState::TextureUVsScaling::ZoomFitUVs:
	    this->updateTextureUVs<WallpaperState::TextureUVsScaling::ZoomFitUVs> ();
	    break;
	case WallpaperState::TextureUVsScaling::CenterUVs:
	    this->updateTextureUVs<WallpaperState::TextureUVsScaling::CenterUVs> ();
	    break;
	case WallpaperState::TextureUVsScaling::FreeUVs:
	    this->updateTextureUVs<WallpaperState::TextureUVsScaling::FreeUVs> ();
	    break;
	case WallpaperState::TextureUVsScaling::DefaultUVs:
	    this->updateTextureUVs<WallpaperState::TextureUVsScaling::DefaultUVs> ();
	    break;
	default:
	    sLog.exception (
		"Switch case for specified scaling mode doesn't exist. Add your realisation in switch statement.\
                This message is for developers, if you are just user it's a bug."
	    );
	    break;
    }

    if (this->m_zoom != 1.0f) {
	applyZoomToAxis (this->m_UVs.ustart, this->m_UVs.uend, this->m_zoom);
	applyZoomToAxis (this->m_UVs.vstart, this->m_UVs.vend, this->m_zoom);
    }

    // applied after zoom, a more zoomed-in view has more room to pan
    applyOffsetToAxis (this->m_UVs.ustart, this->m_UVs.uend, this->m_offsetX);
    applyOffsetToAxis (this->m_UVs.vstart, this->m_UVs.vend, this->m_offsetY);
}