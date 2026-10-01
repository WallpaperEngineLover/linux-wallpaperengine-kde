#pragma once

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

struct FT_LibraryRec_;
struct FT_FaceRec_;
typedef struct FT_LibraryRec_* FT_Library;
typedef struct FT_FaceRec_* FT_Face;
struct hb_font_t;

namespace WallpaperEngine::Render::Objects {

enum class TextAlign { Left = 0, Center = 1, Right = 2 };

struct TextLayoutParams {
    /** pointsize clamped to [1, 256], glyphs are rasterized at size * 300 / 72 pixels */
    float size = 32.0f;
    glm::vec2 spacing = { 0.0f, 0.0f };
    bool msdf = false;
    TextAlign align = TextAlign::Center;
    /** 0 = no wrapping */
    float maxWidth = 0.0f;
    /** 0 = no limit */
    int maxRows = 0;
    bool ellipsis = false;
    bool blockAlign = false;

    bool operator== (const TextLayoutParams&) const = default;
};

struct TextGlyphQuad {
    /** x0, y0, x1, y1 in raster pixels, y up, first baseline at 0 */
    glm::vec4 rect;
    /** u0, v0 (top), u1, v1 (bottom) */
    glm::vec4 uv;
};

struct TextLayoutResult {
    std::vector<TextGlyphQuad> quads;
    /** colour glyphs (emoji), drawn after the others from the colour atlas */
    std::vector<TextGlyphQuad> colorQuads;
    float minX = 0.0f;
    float bottom = 0.0f;
    float maxX = 0.0f;
    float top = 0.0f;
    float pitch = 0.0f;
    int lines = 0;
    float ascender = 0.0f;
    float descender = 0.0f;
    bool valid = false;
};

/** A font file WE falls back to, either read into memory (the assets' Twemoji) or a path on disk */
struct TextFontSource {
    std::shared_ptr<const std::vector<uint8_t>> data;
    std::string path;
};

/**
 * Text layout of wallpaper64.exe 2.8.42 (sub_1401B0410): HarfBuzz shaped runs per font, wrapping, row limit with
 * ellipsis, justification, and a glyph atlas of either plain coverage (R8) or MSDF glyphs (RGBA, msdfgen, 32 px per em
 * plus 12 px of range on every side). Colour glyphs (FreeType's BGRA bitmaps) go to a second RGBA atlas, as distance
 * fields plus a colour texture in MSDF mode (sub_1401AE080). Everything is in raster pixels, before the object's scale.
 */
class TextLayout {
public:
    TextLayout ();
    ~TextLayout ();

    TextLayout (const TextLayout&) = delete;
    TextLayout& operator= (const TextLayout&) = delete;

    /** Replaces every loaded face. An empty data vector loads the file at path instead */
    bool setPrimaryFont (std::vector<uint8_t> data, const std::string& path);
    /** The fonts tried in order for characters the primary font lacks, before asking fontconfig */
    void setFallbackFonts (std::vector<TextFontSource> fonts);
    [[nodiscard]] bool hasFont () const { return !m_faces.empty (); }

    TextLayoutResult layout (const std::string& text, const TextLayoutParams& params);

    [[nodiscard]] int getAtlasSize () const { return m_atlas.size; }
    [[nodiscard]] int getAtlasChannels () const { return m_atlas.channels; }
    [[nodiscard]] const std::vector<uint8_t>& getAtlasPixels () const { return m_atlas.pixels; }
    /** true once after the atlas pixels changed */
    bool takeAtlasChanged ();

    [[nodiscard]] int getColorAtlasSize () const { return m_colorAtlas.size; }
    [[nodiscard]] const std::vector<uint8_t>& getColorAtlasPixels () const { return m_colorAtlas.pixels; }
    /** MSDF mode only: the glyphs' colours at getColorScale () times the colour atlas' resolution */
    [[nodiscard]] int getColorScale () const { return m_colorAtlas.colorScale; }
    [[nodiscard]] const std::vector<uint8_t>& getColorTexturePixels () const { return m_colorAtlas.colorPixels; }
    bool takeColorAtlasChanged ();

private:
    struct Face {
	FT_Face face = nullptr;
	hb_font_t* font = nullptr;
	std::shared_ptr<const std::vector<uint8_t>> data;
	std::string path;
	bool color = false;
    };

    struct GlyphBox {
	int x0 = 0;
	int y0 = 0;
	int x1 = 0;
	int y1 = 0;
    };

    struct AtlasGlyph {
	int width = 0;
	int height = 0;
	std::vector<uint8_t> pixels;
	/** colour glyphs in MSDF mode: the colours of the cell at the atlas' colour scale */
	std::vector<uint8_t> colorPixels;
	int x = -1;
	int y = -1;
	bool color = false;
	/** the bitmap's own box, for colour glyphs whose outline is empty */
	GlyphBox bitmapBox;
    };

    struct Atlas {
	int size = 512;
	int channels = 1;
	/** colour texture size = size * colorScale, 0 when there is none */
	int colorScale = 0;
	std::vector<uint8_t> pixels;
	std::vector<uint8_t> colorPixels;
	std::vector<uint64_t> packOrder;
	int shelfY = 0;
	int shelfHeight = 0;
	int shelfX = 0;
	bool changed = true;
    };

    struct ShapedGlyph {
	uint64_t key = 0;
	uint32_t cluster = 0;
	char32_t codepoint = 0;
	float advance = 0.0f;
	float yAdvance = 0.0f;
	float x0 = 0.0f;
	float y0 = 0.0f;
	float x1 = 0.0f;
	float y1 = 0.0f;
	float pad = 0.0f;
	bool color = false;
    };

    struct Run {
	size_t face = 0;
	size_t start = 0;
	std::u32string text;
    };

    bool addFace (std::shared_ptr<const std::vector<uint8_t>> data, const std::string& path);
    void clearFaces ();
    void applySize (float size);
    int faceFor (char32_t codepoint);
    std::vector<Run> itemize (const std::u32string& line);
    std::vector<ShapedGlyph> shape (const std::u32string& line, const TextLayoutParams& params);
    /** right edge of the last glyph of a shaped string, what WE measures an ellipsis candidate by */
    float measureRight (const std::u32string& line);
    const GlyphBox* glyphBox (size_t face, uint32_t glyph);
    AtlasGlyph* atlasGlyph (size_t face, uint32_t glyph);
    bool renderPlainGlyph (FT_Face face, uint32_t glyph, AtlasGlyph& out);
    bool renderMsdfGlyph (FT_Face face, uint32_t glyph, AtlasGlyph& out);
    bool renderColorMsdfGlyph (FT_Face face, uint32_t glyph, AtlasGlyph& out);
    bool pack (Atlas& atlas, uint64_t key, AtlasGlyph& glyph);
    void repackAtlas (Atlas& atlas, int size);
    void resetAtlases ();

    FT_Library m_library = nullptr;
    std::vector<std::unique_ptr<Face>> m_faces;
    std::vector<TextFontSource> m_fallbackSources;
    bool m_fallbacksLoaded = false;
    std::set<char32_t> m_fallbackTried;
    float m_size = 0.0f;
    bool m_msdf = false;

    std::unordered_map<uint64_t, GlyphBox> m_boxes;
    std::unordered_map<uint64_t, AtlasGlyph> m_glyphs;

    Atlas m_atlas;
    Atlas m_colorAtlas;
    bool m_atlasFull = false;
};
} // namespace WallpaperEngine::Render::Objects
