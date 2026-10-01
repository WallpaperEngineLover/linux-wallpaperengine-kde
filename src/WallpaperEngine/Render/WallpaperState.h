#pragma once

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <glm/vec4.hpp>
#include <optional>
#include <string>

#include "TextureProvider.h"

namespace WallpaperEngine::Render {
using namespace WallpaperEngine::Data::Assets;
class WallpaperState {
public:
    enum class TextureUVsScaling : uint8_t {
	DefaultUVs,
	ZoomFitUVs,
	ZoomFillUVs,
	StretchUVs,
	// Native resolution, centered, no scaling - crops if bigger than the viewport, letterboxes if smaller
	CenterUVs,
	// WE's "free" alignment: native size scaled by the alignment zoom, placed by the alignment x/y
	FreeUVs,
    };

    /** WE's alignment settings (alignmentposition, alignmentx/y/z) as fractions, their UI values divided by 100 */
    struct Alignment {
	/** Where cover crops and fit letterboxes, 0..1 */
	float position = 0.5f;
	/** Center and free placement, 0..1 */
	float x = 0.5f;
	float y = 0.5f;
	/** Free zoom, 0..2, 1 is the native size */
	float zoom = 1.0f;

	bool operator== (const Alignment&) const = default;
    };

    /**
     * Maps the CLI/hotswap scaling names ("stretch", "fit", "fill", "center", "free", "default") to their enum value,
     * or std::nullopt if value isn't a known scaling name
     */
    static std::optional<TextureUVsScaling> parseScalingMode (const std::string& value);

    WallpaperState (const TextureUVsScaling& textureUVsMode, const uint32_t& clampMode);

    [[nodiscard]] bool hasChanged (
	const glm::ivec4& viewport, const bool& vflip, const int& projectionWidth, const int& projectionHeight
    ) const;

    void resetUVs ();
    /**
     * The canvas units the output leaves out on each side (left, right, bottom, top, negative where it letterboxes)
     * for WE's alignment mode (0 cover, 1 fit, 2 stretch, 3 center, 4 free), sub_140183A70
     */
    [[nodiscard]] glm::vec4 alignmentMargins (int mode) const;
    void setMargins (const glm::vec4& margins);

    [[nodiscard]] auto getTextureUVs () const { return m_UVs; };
    /** Whether the texture's first row is the top of the screen (the UVs' v runs top down then) */
    [[nodiscard]] bool isVFlipped () const { return m_vflip; }

    template <WallpaperState::TextureUVsScaling> void updateTextureUVs ();

    void updateState (
	const glm::ivec4& viewport, const bool& vflip, const int& projectionWidth, const int& projectionHeight
    );

    [[nodiscard]] TextureUVsScaling getTextureUVsScaling () const;
    [[nodiscard]] uint32_t getClampingMode () const;

    /**
     * Can be called on a wallpaper that's already rendering (e.g. from a hotswap) - the next updateUVs()
     * call will recompute UVs for the new mode even if the viewport hasn't changed since the last frame.
     */
    void setTextureUVsStrategy (TextureUVsScaling strategy);

    [[nodiscard]] float getZoom () const;

    /**
     * Manual zoom factor applied on top of whatever the scaling mode computes: > 1 crops in tighter
     * (zoomed in), < 1 shows more / overflows past [0,1] relying on border clamping (zoomed out). Clamped to
     * a sane range. Just like setTextureUVsStrategy(), can be called live and takes effect on the next frame
     * even if the viewport hasn't changed.
     */
    void setZoom (float zoom);

    [[nodiscard]] float getOffsetX () const;
    [[nodiscard]] float getOffsetY () const;

    /** Re-centers the crop window, each axis in [-1, 1] (0 = centered), takes effect on the next frame like setZoom()
     */
    void setOffset (float offsetX, float offsetY);

    [[nodiscard]] const Alignment& getAlignment () const;
    /** WE's alignment position, x, y and zoom, takes effect on the next frame like setZoom () */
    void setAlignment (const Alignment& alignment);

    [[nodiscard]] int getViewportWidth () const;
    [[nodiscard]] int getViewportHeight () const;
    [[nodiscard]] int getProjectionWidth () const;
    [[nodiscard]] int getProjectionHeight () const;

private:
    // Cached so UVs don't need to be recalculated if viewport and projection haven't changed
    struct {
	float ustart;
	float uend;
	float vstart;
	float vend;
    } m_UVs {};

    struct {
	int width;
	int height;
    } m_viewport {};

    struct {
	int width;
	int height;
    } m_projection {};

    bool m_vflip = false;

    TextureUVsScaling m_textureUVsMode = TextureUVsScaling::DefaultUVs;
    uint32_t m_clampingMode = TextureFlags_NoFlags;
    float m_zoom = 1.0f;
    float m_offsetX = 0.0f;
    float m_offsetY = 0.0f;
    Alignment m_alignment = {};
    // set when scaling mode, zoom or offset change live, forces the UVs to be recomputed
    bool m_uvsDirty = false;
};
} // namespace WallpaperEngine::Render
