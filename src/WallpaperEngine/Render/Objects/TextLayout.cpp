#include "TextLayout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_OUTLINE_H

#include <hb-ft.h>
#include <hb.h>

#include <msdfgen.h>

#include "WallpaperEngine/Logging/Log.h"

using namespace WallpaperEngine::Render::Objects;

namespace {
constexpr char32_t kEllipsis = U'…';
constexpr int kMaxAtlasSize = 4096;

// the whitespace set wallpaper64.exe breaks lines at and trims (bit mask 0x100002200: tab, CR, space)
bool isBreakSpace (char32_t c) { return c == U'\t' || c == U'\r' || c == U' '; }

std::u32string decodeUtf8 (const std::string& text) {
    std::u32string codepoints;
    size_t i = 0;

    while (i < text.size ()) {
	const auto lead = static_cast<unsigned char> (text[i]);
	size_t extraBytes;
	char32_t codepoint;

	if ((lead & 0x80) == 0x00) {
	    codepoint = lead;
	    extraBytes = 0;
	} else if ((lead & 0xE0) == 0xC0) {
	    codepoint = lead & 0x1F;
	    extraBytes = 1;
	} else if ((lead & 0xF0) == 0xE0) {
	    codepoint = lead & 0x0F;
	    extraBytes = 2;
	} else if ((lead & 0xF8) == 0xF0) {
	    codepoint = lead & 0x07;
	    extraBytes = 3;
	} else {
	    i++;
	    continue;
	}

	if (i + extraBytes >= text.size ()) {
	    break;
	}

	bool valid = true;
	for (size_t k = 1; k <= extraBytes; k++) {
	    const auto cont = static_cast<unsigned char> (text[i + k]);
	    if ((cont & 0xC0) != 0x80) {
		valid = false;
		break;
	    }
	    codepoint = (codepoint << 6) | (cont & 0x3F);
	}

	if (!valid) {
	    i++;
	    continue;
	}

	codepoints.push_back (codepoint);
	i += extraBytes + 1;
    }

    return codepoints;
}

void trimTrailingSpaces (std::u32string& line) {
    while (!line.empty () && isBreakSpace (line.back ())) {
	line.pop_back ();
    }
}

// sub_1401B0360: what the last line kept by the row limit gets
void finishLastRow (std::u32string& line, bool ellipsis) {
    trimTrailingSpaces (line);

    if (ellipsis && (line.empty () || line.back () != kEllipsis)) {
	line.push_back (kEllipsis);
    }
}

uint64_t glyphKey (size_t face, uint32_t glyph) { return (static_cast<uint64_t> (face) << 32) | glyph; }

// WE falls back to a fixed list of Windows fonts for characters the wallpaper's font lacks, ask fontconfig for any
// installed outline font that has the character instead
std::string fontForCodepoint (char32_t codepoint) {
    // every text object asks for the same characters, fc-match is only run once per character
    static std::unordered_map<char32_t, std::string> cache;

    if (const auto it = cache.find (codepoint); it != cache.end ()) {
	return it->second;
    }

    char pattern[64];
    std::snprintf (pattern, sizeof (pattern), "sans-serif:scalable=true:charset=%x", static_cast<unsigned> (codepoint));

    const std::string command = std::string ("fc-match -f '%{file}' '") + pattern + "' 2>/dev/null";
    FILE* pipe = popen (command.c_str (), "r");

    if (pipe == nullptr) {
	return {};
    }

    std::string path;
    char buffer[512];

    while (fgets (buffer, sizeof (buffer), pipe) != nullptr) {
	path += buffer;
    }

    pclose (pipe);

    if (path.empty () || !std::filesystem::exists (path)) {
	path.clear ();
    }

    return cache.emplace (codepoint, path).first->second;
}

struct OutlineContext {
    msdfgen::Shape* shape;
    msdfgen::Contour* contour = nullptr;
    msdfgen::Point2 position;
};

// FT_Outline_Decompose into a msdfgen shape at 1/64 scale (26.6 -> pixels), like wallpaper64.exe's callbacks
msdfgen::Point2 outlinePoint (const FT_Vector* vector) { return { vector->x / 64.0, vector->y / 64.0 }; }

int outlineMoveTo (const FT_Vector* to, void* user) {
    auto* context = static_cast<OutlineContext*> (user);

    if (!(context->contour != nullptr && context->contour->edges.empty ())) {
	context->contour = &context->shape->addContour ();
    }

    context->position = outlinePoint (to);
    return 0;
}

int outlineLineTo (const FT_Vector* to, void* user) {
    auto* context = static_cast<OutlineContext*> (user);
    const msdfgen::Point2 endpoint = outlinePoint (to);

    if (context->contour != nullptr && endpoint != context->position) {
	context->contour->addEdge (msdfgen::EdgeHolder (context->position, endpoint));
	context->position = endpoint;
    }

    return 0;
}

int outlineConicTo (const FT_Vector* control, const FT_Vector* to, void* user) {
    auto* context = static_cast<OutlineContext*> (user);

    if (context->contour != nullptr) {
	const msdfgen::Point2 endpoint = outlinePoint (to);
	context->contour->addEdge (msdfgen::EdgeHolder (context->position, outlinePoint (control), endpoint));
	context->position = endpoint;
    }

    return 0;
}

int outlineCubicTo (const FT_Vector* control1, const FT_Vector* control2, const FT_Vector* to, void* user) {
    auto* context = static_cast<OutlineContext*> (user);

    if (context->contour != nullptr) {
	const msdfgen::Point2 endpoint = outlinePoint (to);
	context->contour->addEdge (
	    msdfgen::EdgeHolder (context->position, outlinePoint (control1), outlinePoint (control2), endpoint)
	);
	context->position = endpoint;
    }

    return 0;
}

constexpr float kDistanceInfinity = 1e20f;

// sub_1401AC130: Felzenszwalb's 1D squared distance transform of f in place (float, like WE's)
void distanceTransform1D (std::vector<float>& f, std::vector<float>& d, std::vector<int>& v, std::vector<float>& z) {
    const int n = static_cast<int> (f.size ());

    if (n <= 0) {
	return;
    }

    int k = 0;
    v[0] = 0;
    z[0] = -kDistanceInfinity;
    z[1] = kDistanceInfinity;

    for (int q = 1; q < n; q++) {
	const float fq = static_cast<float> (q) * static_cast<float> (q) + f[q];
	float s = (fq - (static_cast<float> (v[k]) * static_cast<float> (v[k]) + f[v[k]]))
	    / static_cast<float> (2 * (q - v[k]));

	while (s <= z[k]) {
	    k--;
	    s = (fq - (static_cast<float> (v[k]) * static_cast<float> (v[k]) + f[v[k]]))
		/ static_cast<float> (2 * (q - v[k]));
	}

	k++;
	v[k] = q;
	z[k] = s;
	z[k + 1] = kDistanceInfinity;
    }

    k = 0;

    for (int q = 0; q < n; q++) {
	while (static_cast<float> (q) > z[k + 1]) {
	    k++;
	}

	d[q] = static_cast<float> ((q - v[k]) * (q - v[k])) + f[v[k]];
    }

    f = d;
}

// sub_1401AC3B0: squared distances to the nearest 0 cell, columns first, then rows
void distanceTransform2D (std::vector<float>& grid, int width, int height) {
    const int longest = std::max (width, height);
    std::vector<float> line;
    std::vector<float> scratch (longest);
    std::vector<int> v (longest);
    std::vector<float> z (longest + 1);

    for (int x = 0; x < width; x++) {
	line.resize (height);
	scratch.resize (height);

	for (int y = 0; y < height; y++) {
	    line[y] = grid[x + y * width];
	}

	distanceTransform1D (line, scratch, v, z);

	for (int y = 0; y < height; y++) {
	    grid[x + y * width] = line[y];
	}
    }

    for (int y = 0; y < height; y++) {
	line.assign (grid.begin () + static_cast<long> (y) * width, grid.begin () + static_cast<long> (y + 1) * width);
	scratch.resize (width);
	distanceTransform1D (line, scratch, v, z);
	std::ranges::copy (line, grid.begin () + static_cast<long> (y) * width);
    }
}

// sub_1401AC4E0: bilinear sample of a BGRA bitmap at pixel coordinates, RGBA out, transparent outside it
void sampleBitmapBGRA (const FT_Bitmap& bitmap, float x, float y, uint8_t* out) {
    const int maxX = static_cast<int> (bitmap.width) - 1;
    const int maxY = static_cast<int> (bitmap.rows) - 1;

    if (x < 0.0f || x > static_cast<float> (maxX) || y < 0.0f || y > static_cast<float> (maxY)) {
	out[0] = out[1] = out[2] = out[3] = 0;
	return;
    }

    const float floorX = std::floor (x);
    const float floorY = std::floor (y);
    const float fx = x - floorX;
    const float fy = y - floorY;
    const int x0 = static_cast<int> (floorX);
    const int y0 = static_cast<int> (floorY);
    const int x1 = std::min (x0 + 1, maxX);
    const int y1 = std::min (y0 + 1, maxY);
    const uint8_t* row0 = bitmap.buffer + static_cast<long> (y0) * bitmap.pitch;
    const uint8_t* row1 = bitmap.buffer + static_cast<long> (y1) * bitmap.pitch;
    uint8_t texel[4];

    for (int channel = 0; channel < 4; channel++) {
	const float value = static_cast<float> (row0[x1 * 4 + channel]) * ((1.0f - fy) * fx)
	    + static_cast<float> (row0[x0 * 4 + channel]) * ((1.0f - fy) * (1.0f - fx))
	    + static_cast<float> (row1[x0 * 4 + channel]) * ((1.0f - fx) * fy)
	    + static_cast<float> (row1[x1 * 4 + channel]) * (fy * fx);
	texel[channel] = static_cast<uint8_t> (std::clamp (static_cast<int> (value + 0.5f), 0, 255));
    }

    out[0] = texel[2];
    out[1] = texel[1];
    out[2] = texel[0];
    out[3] = texel[3];
}
} // namespace

TextLayout::TextLayout () {
    if (FT_Init_FreeType (&m_library) != 0) {
	m_library = nullptr;
	sLog.error ("TextLayout: FT_Init_FreeType failed");
    }

    m_colorAtlas.channels = 4;
    this->resetAtlases ();
}

TextLayout::~TextLayout () {
    this->clearFaces ();

    if (m_library != nullptr) {
	FT_Done_FreeType (m_library);
    }
}

void TextLayout::clearFaces () {
    for (const auto& face : m_faces) {
	if (face->font != nullptr) {
	    hb_font_destroy (face->font);
	}
	if (face->face != nullptr) {
	    FT_Done_Face (face->face);
	}
    }

    m_faces.clear ();
    m_fallbacksLoaded = false;
    m_fallbackTried.clear ();
    m_boxes.clear ();
    m_glyphs.clear ();
    this->resetAtlases ();
}

bool TextLayout::setPrimaryFont (std::vector<uint8_t> data, const std::string& path) {
    this->clearFaces ();
    return this->addFace (
	data.empty () ? nullptr : std::make_shared<const std::vector<uint8_t>> (std::move (data)), path
    );
}

void TextLayout::setFallbackFonts (std::vector<TextFontSource> fonts) { m_fallbackSources = std::move (fonts); }

bool TextLayout::addFace (std::shared_ptr<const std::vector<uint8_t>> data, const std::string& path) {
    if (m_library == nullptr) {
	return false;
    }

    auto face = std::make_unique<Face> ();
    face->data = std::move (data);
    face->path = path;

    const FT_Error error = face->data == nullptr
	? FT_New_Face (m_library, path.c_str (), 0, &face->face)
	: FT_New_Memory_Face (
	      m_library, face->data->data (), static_cast<FT_Long> (face->data->size ()), 0, &face->face
	  );

    if (error != 0) {
	sLog.error ("TextLayout: cannot load font '", path, "' (FreeType error ", error, ")");
	return false;
    }

    // bitmap-only fonts (colour emoji strikes) can't be sized to an arbitrary raster size
    if (!FT_IS_SCALABLE (face->face)) {
	FT_Done_Face (face->face);
	return false;
    }

    FT_Select_Charmap (face->face, FT_ENCODING_UNICODE);
    face->color = FT_HAS_COLOR (face->face);

    if (m_size > 0.0f) {
	FT_Set_Char_Size (face->face, 0, static_cast<FT_F26Dot6> (m_size * 64.0f), 300, 300);
    }

    face->font = hb_ft_font_create (face->face, nullptr);
    m_faces.push_back (std::move (face));
    return true;
}

void TextLayout::applySize (float size) {
    if (size == m_size) {
	return;
    }

    m_size = size;

    for (const auto& face : m_faces) {
	FT_Set_Char_Size (face->face, 0, static_cast<FT_F26Dot6> (size * 64.0f), 300, 300);
	hb_ft_font_changed (face->font);
    }

    m_boxes.clear ();
    m_glyphs.clear ();
    this->resetAtlases ();
}

int TextLayout::faceFor (char32_t codepoint) {
    const auto findLoaded = [this, codepoint] () {
	for (size_t i = 0; i < m_faces.size (); i++) {
	    if (FT_Get_Char_Index (m_faces[i]->face, codepoint) != 0) {
		return static_cast<int> (i);
	    }
	}

	return -1;
    };

    if (const int face = findLoaded (); face >= 0) {
	return face;
    }

    if (codepoint < 0x20) {
	return -1;
    }

    // WE's fixed fallback list (sub_1401AD670) comes right after the primary font, fontconfig's picks after that
    if (!m_fallbacksLoaded) {
	m_fallbacksLoaded = true;

	for (const auto& source : m_fallbackSources) {
	    this->addFace (source.data, source.path);
	}

	if (const int face = findLoaded (); face >= 0) {
	    return face;
	}
    }

    if (!m_fallbackTried.insert (codepoint).second) {
	return -1;
    }

    const std::string path = fontForCodepoint (codepoint);

    if (path.empty ()) {
	return -1;
    }

    for (const auto& face : m_faces) {
	if (face->path == path) {
	    return -1;
	}
    }

    if (!this->addFace ({}, path) || FT_Get_Char_Index (m_faces.back ()->face, codepoint) == 0) {
	return -1;
    }

    return static_cast<int> (m_faces.size () - 1);
}

// sub_1401AD670: one run per stretch of text a single face covers, zero width joiners and whitespace stay in the
// current run
std::vector<TextLayout::Run> TextLayout::itemize (const std::u32string& line) {
    std::vector<Run> runs;

    for (size_t i = 0; i < line.size (); i++) {
	const char32_t c = line[i];
	const bool keepsRun = c == 0x200D || c == U'\t' || c == U'\n' || c == U'\r' || c == U' ';

	if (keepsRun && !runs.empty ()) {
	    runs.back ().text.push_back (c);
	    continue;
	}

	int face = this->faceFor (c);

	if (face < 0) {
	    face = runs.empty () ? 0 : static_cast<int> (runs.back ().face);
	}

	if (runs.empty () || runs.back ().face != static_cast<size_t> (face)) {
	    runs.push_back ({ static_cast<size_t> (face), i, {} });
	}

	runs.back ().text.push_back (c);
    }

    return runs;
}

std::vector<TextLayout::ShapedGlyph> TextLayout::shape (const std::u32string& line, const TextLayoutParams& params) {
    std::vector<ShapedGlyph> glyphs;

    for (const auto& run : this->itemize (line)) {
	const Face& face = *m_faces[run.face];
	hb_buffer_t* buffer = hb_buffer_create ();
	hb_buffer_add_utf32 (
	    buffer, reinterpret_cast<const uint32_t*> (run.text.data ()), static_cast<int> (run.text.size ()), 0, -1
	);
	hb_buffer_guess_segment_properties (buffer);
	hb_shape (face.font, buffer, nullptr, 0);

	unsigned int count = 0;
	const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos (buffer, &count);
	const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions (buffer, nullptr);
	const float pad = params.msdf
	    ? static_cast<float> (std::max<int> (face.face->size->metrics.y_ppem, 1)) * 12.0f * 0.03125f
	    : 0.0f;

	for (unsigned int i = 0; i < count; i++) {
	    const uint32_t glyph = infos[i].codepoint;

	    // WE's glyph fetch fails on .notdef, which leaves the character out without advancing the pen
	    const AtlasGlyph* entry = glyph == 0 ? nullptr : this->atlasGlyph (run.face, glyph);

	    if (entry == nullptr) {
		continue;
	    }

	    // the quad comes from the outline's box (sub_1401ADDB0, loaded without bitmaps). Segoe UI Emoji, the colour
	    // font WE finds first on Windows, has monochrome outlines under its colour layers, Twemoji (the fallback
	    // left on Linux) has none, which would give its emoji an empty quad: their bitmap's box stands in there
	    const GlyphBox* box = this->glyphBox (run.face, glyph);
	    GlyphBox bounds = box != nullptr ? *box : GlyphBox {};

	    if (entry->color && bounds.x0 == bounds.x1 && bounds.y0 == bounds.y1) {
		bounds = entry->bitmapBox;
	    }
	    const int xOffset = positions[i].x_offset >> 6;
	    const int yOffset = positions[i].y_offset >> 6;
	    const uint32_t cluster = infos[i].cluster + static_cast<uint32_t> (run.start);

	    glyphs.push_back (
		{
		    .key = glyphKey (run.face, glyph),
		    .cluster = cluster,
		    .codepoint = cluster < line.size () ? line[cluster] : 0,
		    .advance = static_cast<float> (positions[i].x_advance >> 6) + params.spacing.x,
		    .yAdvance = static_cast<float> (positions[i].y_advance >> 6),
		    .x0 = static_cast<float> (xOffset + bounds.x0),
		    .y0 = static_cast<float> (yOffset + bounds.y0),
		    .x1 = static_cast<float> (xOffset + bounds.x1),
		    .y1 = static_cast<float> (yOffset + bounds.y1),
		    .pad = pad,
		    .color = entry->color,
		}
	    );
	}

	hb_buffer_destroy (buffer);
    }

    return glyphs;
}

float TextLayout::measureRight (const std::u32string& line) {
    float pen = 0.0f;
    float right = 0.0f;

    for (const auto& run : this->itemize (line)) {
	hb_buffer_t* buffer = hb_buffer_create ();
	hb_buffer_add_utf32 (
	    buffer, reinterpret_cast<const uint32_t*> (run.text.data ()), static_cast<int> (run.text.size ()), 0, -1
	);
	hb_buffer_guess_segment_properties (buffer);
	hb_shape (m_faces[run.face]->font, buffer, nullptr, 0);

	unsigned int count = 0;
	const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos (buffer, &count);
	const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions (buffer, nullptr);

	for (unsigned int i = 0; i < count; i++) {
	    if (const GlyphBox* box = this->glyphBox (run.face, infos[i].codepoint); box != nullptr) {
		right = static_cast<float> (positions[i].x_offset >> 6) + pen + static_cast<float> (box->x1);
		pen += static_cast<float> (positions[i].x_advance >> 6);
	    }
	}

	hb_buffer_destroy (buffer);
    }

    return right;
}

// sub_1401ADDB0: the outline's pixel grid fitted control box, loaded without bitmaps
const TextLayout::GlyphBox* TextLayout::glyphBox (size_t face, uint32_t glyph) {
    const uint64_t key = glyphKey (face, glyph);

    if (const auto it = m_boxes.find (key); it != m_boxes.end ()) {
	return &it->second;
    }

    FT_Face ftFace = m_faces[face]->face;
    FT_Glyph ftGlyph = nullptr;

    if (FT_Load_Glyph (ftFace, glyph, FT_LOAD_NO_BITMAP) != 0 || FT_Get_Glyph (ftFace->glyph, &ftGlyph) != 0) {
	return nullptr;
    }

    FT_BBox bbox;
    FT_Glyph_Get_CBox (ftGlyph, FT_GLYPH_BBOX_PIXELS, &bbox);
    FT_Done_Glyph (ftGlyph);

    return &m_boxes
		.emplace (
		    key,
		    GlyphBox { static_cast<int> (bbox.xMin), static_cast<int> (bbox.yMin), static_cast<int> (bbox.xMax),
			       static_cast<int> (bbox.yMax) }
		)
		.first->second;
}

TextLayout::AtlasGlyph* TextLayout::atlasGlyph (size_t face, uint32_t glyph) {
    const uint64_t key = glyphKey (face, glyph);

    if (const auto it = m_glyphs.find (key); it != m_glyphs.end ()) {
	return &it->second;
    }

    AtlasGlyph entry;
    const Face& source = *m_faces[face];

    // sub_1401AE080: plain glyphs are rendered with FT_LOAD_RENDER | FT_LOAD_COLOR, in MSDF mode fonts with colour
    // glyphs are too (their BGRA bitmaps become distance fields), the rest are outlines
    bool rendered;

    if (!m_msdf) {
	rendered = this->renderPlainGlyph (source.face, glyph, entry);
    } else if (source.color) {
	rendered = this->renderColorMsdfGlyph (source.face, glyph, entry);
    } else {
	rendered = this->renderMsdfGlyph (source.face, glyph, entry);
    }

    if (!rendered) {
	return nullptr;
    }

    auto& stored = m_glyphs.emplace (key, std::move (entry)).first->second;

    if (!this->pack (stored.color ? m_colorAtlas : m_atlas, key, stored)) {
	m_atlasFull = true;
    }

    return &stored;
}

bool TextLayout::renderPlainGlyph (FT_Face face, uint32_t glyph, AtlasGlyph& out) {
    if (FT_Load_Glyph (face, glyph, FT_LOAD_RENDER | FT_LOAD_COLOR) != 0) {
	return false;
    }

    const FT_GlyphSlot slot = face->glyph;
    const FT_Bitmap& bitmap = slot->bitmap;
    out.width = static_cast<int> (bitmap.width);
    out.height = static_cast<int> (bitmap.rows);

    // colour bitmaps go to the RGBA atlas as they are, premultiplied, with red and blue swapped
    if (bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
	out.color = true;
	out.bitmapBox
	    = { slot->bitmap_left, slot->bitmap_top - out.height, slot->bitmap_left + out.width, slot->bitmap_top };
	out.pixels.assign (static_cast<size_t> (out.width) * out.height * 4, 0);

	for (int row = 0; row < out.height; row++) {
	    const uint8_t* src = bitmap.buffer + static_cast<long> (row) * bitmap.pitch;
	    uint8_t* dst = &out.pixels[static_cast<size_t> (row) * out.width * 4];

	    for (int col = 0; col < out.width; col++) {
		dst[col * 4 + 0] = src[col * 4 + 2];
		dst[col * 4 + 1] = src[col * 4 + 1];
		dst[col * 4 + 2] = src[col * 4 + 0];
		dst[col * 4 + 3] = src[col * 4 + 3];
	    }
	}

	return true;
    }

    out.pixels.assign (static_cast<size_t> (out.width) * out.height, 0);

    if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
	for (int row = 0; row < out.height; row++) {
	    std::copy_n (bitmap.buffer + row * bitmap.pitch, out.width, out.pixels.begin () + row * out.width);
	}
    }

    return true;
}

// sub_1401AE080 for a colour glyph in MSDF mode: the bitmap's alpha (inside from 128 on) becomes a signed distance
// field laid out like the MSDF glyphs (32 px per em, 12 px of range around it), stored in all four channels, and the
// colours are resampled at the colour atlas' scale into its colour texture
bool TextLayout::renderColorMsdfGlyph (FT_Face face, uint32_t glyph, AtlasGlyph& out) {
    if (FT_Load_Glyph (face, glyph, FT_LOAD_RENDER | FT_LOAD_COLOR) != 0) {
	return false;
    }

    const FT_GlyphSlot slot = face->glyph;
    const FT_Bitmap& bitmap = slot->bitmap;

    // a glyph of a colour font without colour layers: WE copies its grey bitmap into the RGBA atlas as if it had one
    // byte per pixel, which leaves the glyph's own cell empty
    if (bitmap.pixel_mode != FT_PIXEL_MODE_BGRA) {
	out.width = static_cast<int> (bitmap.width);
	out.height = static_cast<int> (bitmap.rows);
	out.pixels.assign (static_cast<size_t> (out.width) * out.height * 4, 0);
	return true;
    }

    const int bitmapWidth = static_cast<int> (bitmap.width);
    const int bitmapHeight = static_cast<int> (bitmap.rows);
    // FT_Glyph_Get_CBox of a bitmap glyph in pixels
    const int boxX0 = slot->bitmap_left;
    const int boxY0 = slot->bitmap_top - bitmapHeight;
    const int boxWidth = bitmapWidth;
    const int boxHeight = bitmapHeight;
    int width = 0;
    int height = 0;

    out.color = true;
    out.bitmapBox = { boxX0, boxY0, boxX0 + boxWidth, boxY0 + boxHeight };

    // sub_1401ADF30: the first colour glyph picks the colour texture's scale for the whole font set
    if (m_colorAtlas.colorScale == 0) {
	m_colorAtlas.colorScale = std::max (1, (std::max (boxWidth, boxHeight) + 31) / 32);
	m_colorAtlas.colorPixels.assign (
	    static_cast<size_t> (m_colorAtlas.size) * m_colorAtlas.colorScale * m_colorAtlas.size
		* m_colorAtlas.colorScale * 4,
	    0
	);
    }

    if (boxWidth > 0 && boxHeight > 0) {
	const float toAtlas = 32.0f / static_cast<float> (std::max<int> (face->size->metrics.y_ppem, 1));
	width = std::max (1, static_cast<int> (static_cast<float> (boxWidth) * toAtlas));
	height = std::max (1, static_cast<int> (static_cast<float> (boxHeight) * toAtlas));
    }

    out.width = width + 24;
    out.height = height + 24;
    out.pixels.assign (static_cast<size_t> (out.width) * out.height * 4, 0);

    const int colorScale = m_colorAtlas.colorScale;
    out.colorPixels.assign (static_cast<size_t> (out.width) * colorScale * out.height * colorScale * 4, 0);

    if (width <= 0 || height <= 0 || bitmap.buffer == nullptr) {
	return true;
    }

    const float scale = static_cast<float> (std::max (width, height))
	/ std::max (static_cast<float> (boxHeight), static_cast<float> (boxWidth));
    const float range = 24.0f / scale;
    const double translateX = static_cast<double> (12.0f / scale - static_cast<float> (boxX0));
    const double translateY = static_cast<double> (12.0f / scale - static_cast<float> (boxY0));

    // sub_1401AB9C0: squared distances to the nearest inside and outside texels, signed distance at texel centers
    std::vector<float> toInside (static_cast<size_t> (bitmapWidth) * bitmapHeight);
    std::vector<float> toOutside (toInside.size ());

    for (int y = 0; y < bitmapHeight; y++) {
	for (int x = 0; x < bitmapWidth; x++) {
	    const bool inside = bitmap.buffer[static_cast<long> (y) * bitmap.pitch + x * 4 + 3] >= 0x80;
	    toInside[x + y * bitmapWidth] = inside ? 0.0f : kDistanceInfinity;
	    toOutside[x + y * bitmapWidth] = inside ? kDistanceInfinity : 0.0f;
	}
    }

    distanceTransform2D (toInside, bitmapWidth, bitmapHeight);
    distanceTransform2D (toOutside, bitmapWidth, bitmapHeight);

    std::vector<float> field (toInside.size ());

    for (int y = 0; y < bitmapHeight; y++) {
	for (int x = 0; x < bitmapWidth; x++) {
	    const size_t i = x + y * bitmapWidth;
	    const bool inside = bitmap.buffer[static_cast<long> (y) * bitmap.pitch + x * 4 + 3] >= 0x80;
	    field[i] = inside ? std::sqrt (toOutside[i]) - 0.5f : 0.5f - std::sqrt (toInside[i]);
	}
    }

    const float inverseRange = 1.0f / range;
    const auto toByte
	= [] (float value) { return static_cast<uint8_t> (std::clamp (static_cast<int> (value * 256.0f), 0, 255)); };

    for (int y = 0; y < out.height; y++) {
	const float glyphY = static_cast<float> ((static_cast<float> (y) + 0.5) / scale - translateY);
	const float bitmapY = static_cast<float> (slot->bitmap_top) - glyphY;

	for (int x = 0; x < out.width; x++) {
	    const float glyphX = static_cast<float> ((static_cast<float> (x) + 0.5) / scale - translateX);
	    const float bitmapX = glyphX - static_cast<float> (slot->bitmap_left);
	    // outside the bitmap the distance to its edge adds on
	    const float clampedX = bitmapX >= static_cast<float> (bitmapWidth - 1)
		? static_cast<float> (bitmapWidth - 1)
		: (bitmapX <= 0.0f ? 0.0f : bitmapX);
	    const float clampedY = bitmapY >= static_cast<float> (bitmapHeight - 1)
		? static_cast<float> (bitmapHeight - 1)
		: (bitmapY <= 0.0f ? 0.0f : bitmapY);
	    const float dx = bitmapX - clampedX;
	    const float dy = bitmapY - clampedY;
	    const float outside = std::sqrt (dy * dy + dx * dx);
	    const float floorX = std::floor (clampedX);
	    const float floorY = std::floor (clampedY);
	    const float fx = clampedX - floorX;
	    const float fy = clampedY - floorY;
	    const int x0 = static_cast<int> (floorX);
	    const int y0 = static_cast<int> (floorY);
	    const int x1 = std::min (x0 + 1, bitmapWidth - 1);
	    const int y1 = std::min (y0 + 1, bitmapHeight - 1);
	    const float top = (1.0f - fx) * field[x0 + y0 * bitmapWidth] + fx * field[x1 + y0 * bitmapWidth];
	    const float bottom = (1.0f - fx) * field[x0 + y1 * bitmapWidth] + fx * field[x1 + y1 * bitmapWidth];
	    const float value = ((top * (1.0f - fy) + bottom * fy) - outside) * inverseRange + 0.5f;

	    // rows go bottom up here, the atlas is stored top down
	    uint8_t* dst = &out.pixels[(static_cast<size_t> (out.height - y - 1) * out.width + x) * 4];
	    dst[0] = dst[1] = dst[2] = dst[3] = toByte (value);
	}
    }

    const int colorWidth = out.width * colorScale;
    const int colorHeight = out.height * colorScale;
    const double colorToGlyph = static_cast<double> (static_cast<float> (colorScale) * scale);

    for (int row = 0; row < colorHeight; row++) {
	const float glyphY
	    = static_cast<float> ((static_cast<float> (colorHeight - row - 1) + 0.5) / colorToGlyph - translateY);
	const float bitmapY = static_cast<float> (slot->bitmap_top) - glyphY;

	for (int col = 0; col < colorWidth; col++) {
	    const float glyphX = static_cast<float> ((static_cast<float> (col) + 0.5) / colorToGlyph - translateX);
	    sampleBitmapBGRA (
		bitmap, glyphX - static_cast<float> (slot->bitmap_left), bitmapY,
		&out.colorPixels[(static_cast<size_t> (row) * colorWidth + col) * 4]
	    );
	}
    }

    return true;
}

// sub_1401AE080: an MSDF of the glyph at 32 pixels per em with 12 pixels of distance range around it
bool TextLayout::renderMsdfGlyph (FT_Face face, uint32_t glyph, AtlasGlyph& out) {
    FT_Glyph ftGlyph = nullptr;

    if (FT_Load_Glyph (face, glyph, FT_LOAD_NO_BITMAP) != 0 || face->glyph->format != FT_GLYPH_FORMAT_OUTLINE
	|| FT_Get_Glyph (face->glyph, &ftGlyph) != 0) {
	return false;
    }

    FT_BBox bbox;
    FT_Glyph_Get_CBox (ftGlyph, FT_GLYPH_BBOX_PIXELS, &bbox);
    FT_Done_Glyph (ftGlyph);

    const int boxWidth = static_cast<int> (bbox.xMax - bbox.xMin);
    const int boxHeight = static_cast<int> (bbox.yMax - bbox.yMin);
    int width = 0;
    int height = 0;

    if (boxWidth > 0 && boxHeight > 0) {
	const float toAtlas = 32.0f / static_cast<float> (std::max<int> (face->size->metrics.y_ppem, 1));
	width = std::max (1, static_cast<int> (static_cast<float> (boxWidth) * toAtlas));
	height = std::max (1, static_cast<int> (static_cast<float> (boxHeight) * toAtlas));
    }

    out.width = width + 24;
    out.height = height + 24;
    out.pixels.assign (static_cast<size_t> (out.width) * out.height * 4, 0);

    if (width <= 0 || height <= 0) {
	return true;
    }

    msdfgen::Shape shape;
    OutlineContext context { &shape };
    FT_Outline_Funcs funcs {
	.move_to = outlineMoveTo,
	.line_to = outlineLineTo,
	.conic_to = outlineConicTo,
	.cubic_to = outlineCubicTo,
	.shift = 0,
	.delta = 0,
    };

    if (FT_Outline_Decompose (&face->glyph->outline, &funcs, &context) != 0) {
	return true;
    }

    if (!shape.contours.empty () && shape.contours.back ().edges.empty ()) {
	shape.contours.pop_back ();
    }

    if (shape.contours.empty ()) {
	return true;
    }

    shape.normalize ();
    shape.orientContours ();
    // WE's seed argument is whatever the previous call left in the register, msdfgen's default is used here
    msdfgen::edgeColoringSimple (shape, 3.0);

    const float scale = static_cast<float> (std::max (width, height))
	/ std::max (static_cast<float> (boxHeight), static_cast<float> (boxWidth));
    const float range = 24.0f / scale;
    const msdfgen::Projection projection (
	msdfgen::Vector2 (scale),
	msdfgen::Vector2 (
	    12.0f / scale - static_cast<float> (bbox.xMin), 12.0f / scale - static_cast<float> (bbox.yMin)
	)
    );
    const msdfgen::SDFTransformation transformation (projection, msdfgen::DistanceMapping (msdfgen::Range (range)));

    msdfgen::Bitmap<float, 3> msdf (out.width, out.height);
    msdfgen::MSDFGeneratorConfig generatorConfig (
	true, msdfgen::ErrorCorrectionConfig (msdfgen::ErrorCorrectionConfig::DISABLED)
    );
    msdfgen::generateMSDF (msdf, shape, transformation, generatorConfig);

    const msdfgen::MSDFGeneratorConfig correctionConfig (
	true,
	msdfgen::ErrorCorrectionConfig (
	    msdfgen::ErrorCorrectionConfig::EDGE_PRIORITY, msdfgen::ErrorCorrectionConfig::CHECK_DISTANCE_AT_EDGE,
	    1.1111111111111112, 1.1111111111111112
	)
    );
    msdfgen::msdfErrorCorrection (msdf, shape, transformation, correctionConfig);

    const auto toByte = [] (float value) {
	const int scaled = static_cast<int> (value * 256.0f);
	return static_cast<uint8_t> (std::clamp (scaled, 0, 255));
    };

    // msdfgen's rows go bottom up, the atlas is stored top down
    for (int row = 0; row < out.height; row++) {
	for (int col = 0; col < out.width; col++) {
	    const float* texel = msdf (col, out.height - row - 1);
	    uint8_t* dst = &out.pixels[(static_cast<size_t> (row) * out.width + col) * 4];
	    dst[0] = toByte (texel[0]);
	    dst[1] = toByte (texel[1]);
	    dst[2] = toByte (texel[2]);
	    dst[3] = 255;
	}
    }

    return true;
}

void TextLayout::resetAtlases () {
    for (Atlas* atlas : { &m_atlas, &m_colorAtlas }) {
	atlas->size = 512;
	atlas->pixels.assign (static_cast<size_t> (atlas->size) * atlas->size * atlas->channels, 0);
	atlas->colorScale = 0;
	atlas->colorPixels.clear ();
	atlas->packOrder.clear ();
	atlas->shelfX = atlas->shelfY = atlas->shelfHeight = 0;
	atlas->changed = true;
    }

    m_atlasFull = false;
}

bool TextLayout::pack (Atlas& atlas, uint64_t key, AtlasGlyph& glyph) {
    // one pixel between glyphs so linear filtering never picks up a neighbour
    const int width = glyph.width + 1;
    const int height = glyph.height + 1;

    while (true) {
	if (atlas.shelfX + width > atlas.size) {
	    atlas.shelfY += atlas.shelfHeight;
	    atlas.shelfX = 0;
	    atlas.shelfHeight = 0;
	}

	if (width <= atlas.size && atlas.shelfY + height <= atlas.size) {
	    break;
	}

	// the atlas doubles up to 4096 like WE's
	if (atlas.size >= kMaxAtlasSize) {
	    return false;
	}

	this->repackAtlas (atlas, atlas.size * 2);
    }

    glyph.x = atlas.shelfX;
    glyph.y = atlas.shelfY;
    atlas.shelfX += width;
    atlas.shelfHeight = std::max (atlas.shelfHeight, height);

    const int channels = atlas.channels;

    for (int row = 0; row < glyph.height; row++) {
	std::copy_n (
	    glyph.pixels.begin () + static_cast<long> (row) * glyph.width * channels, glyph.width * channels,
	    atlas.pixels.begin () + (static_cast<long> (glyph.y + row) * atlas.size + glyph.x) * channels
	);
    }

    // the colour texture holds the same cell at colorScale times the resolution
    if (atlas.colorScale > 0 && !glyph.colorPixels.empty ()) {
	const int scale = atlas.colorScale;
	const int rowLength = glyph.width * scale * 4;
	const long stride = static_cast<long> (atlas.size) * scale * 4;

	for (int row = 0; row < glyph.height * scale; row++) {
	    std::copy_n (
		glyph.colorPixels.begin () + static_cast<long> (row) * rowLength, rowLength,
		atlas.colorPixels.begin () + (static_cast<long> (glyph.y) * scale + row) * stride
		    + static_cast<long> (glyph.x) * scale * 4
	    );
	}
    }

    atlas.packOrder.push_back (key);
    atlas.changed = true;
    return true;
}

void TextLayout::repackAtlas (Atlas& atlas, int size) {
    const std::vector<uint64_t> order = std::move (atlas.packOrder);

    atlas.size = size;
    atlas.pixels.assign (static_cast<size_t> (size) * size * atlas.channels, 0);

    if (atlas.colorScale > 0) {
	atlas.colorPixels.assign (static_cast<size_t> (size) * atlas.colorScale * size * atlas.colorScale * 4, 0);
    }

    atlas.packOrder.clear ();
    atlas.shelfX = atlas.shelfY = atlas.shelfHeight = 0;

    for (const uint64_t key : order) {
	if (auto it = m_glyphs.find (key); it != m_glyphs.end ()) {
	    this->pack (atlas, key, it->second);
	}
    }
}

bool TextLayout::takeAtlasChanged () {
    const bool changed = m_atlas.changed;
    m_atlas.changed = false;
    return changed;
}

bool TextLayout::takeColorAtlasChanged () {
    const bool changed = m_colorAtlas.changed;
    m_colorAtlas.changed = false;
    return changed;
}

TextLayoutResult TextLayout::layout (const std::string& utf8, const TextLayoutParams& params) {
    TextLayoutResult result;

    if (m_faces.empty ()) {
	return result;
    }

    if (params.msdf != m_msdf) {
	m_msdf = params.msdf;
	m_atlas.channels = m_msdf ? 4 : 1;
	m_glyphs.clear ();
	this->resetAtlases ();
    }

    this->applySize (params.size);

    const std::u32string text = decodeUtf8 (utf8);

    // WE flushes a full atlas and lays the text out again with only what it needs
    for (int attempt = 0; attempt < 2; attempt++) {
	m_atlasFull = false;

	std::vector<std::u32string> lines;

	for (size_t start = 0; start < text.size ();) {
	    const size_t newline = text.find (U'\n', start);

	    if (params.maxRows > 0 && lines.size () >= static_cast<size_t> (params.maxRows)) {
		finishLastRow (lines.back (), params.ellipsis);
		break;
	    }

	    lines.push_back (text.substr (start, newline == std::u32string::npos ? newline : newline - start));

	    if (newline == std::u32string::npos) {
		break;
	    }

	    start = newline + 1;
	}

	std::vector<bool> wrapped (lines.size (), false);

	struct Line {
	    std::vector<ShapedGlyph> glyphs;
	    float width;
	};

	std::vector<Line> finished;
	size_t glyphCount = 0;
	float minX = 0.0f;
	float maxX = 0.0f;
	float minY = 0.0f;
	float maxY = 0.0f;
	float widest = 0.0f;
	const FT_Size_Metrics& metrics = m_faces.front ()->face->size->metrics;
	const float pitch = static_cast<float> (metrics.height >> 6) + params.spacing.y;

	for (size_t i = 0; i < lines.size ();) {
	    const std::vector<ShapedGlyph> glyphs = this->shape (lines[i], params);
	    std::vector<ShapedGlyph> kept;
	    float pen = 0.0f;
	    int64_t previousCluster = -1;
	    bool reshape = false;
	    bool lastLine = false;

	    for (const auto& glyph : glyphs) {
		const bool overflows = params.maxWidth > 0.0f && !kept.empty () && !isBreakSpace (glyph.codepoint)
		    && static_cast<int64_t> (glyph.cluster) != previousCluster && pen + glyph.x1 > params.maxWidth;

		if (!overflows) {
		    kept.push_back (glyph);
		    pen += glyph.advance;
		    previousCluster = glyph.cluster;
		    continue;
		}

		std::u32string& line = lines[i];

		if (params.maxRows <= 0 || finished.size () + 1 < static_cast<size_t> (params.maxRows)) {
		    // back to the last whitespace before the overflowing character, or break inside the word
		    size_t breakAt = glyph.cluster;

		    while (breakAt > 1 && isBreakSpace (line[breakAt])) {
			breakAt--;
		    }

		    for (size_t j = breakAt; j-- > 0;) {
			if (isBreakSpace (line[j])) {
			    breakAt = j;
			    break;
			}
		    }

		    std::u32string rest = line.substr (breakAt);
		    rest.erase (0, std::min (rest.size (), rest.find_first_not_of (U"\t\r ")));
		    line.resize (breakAt);
		    trimTrailingSpaces (line);
		    wrapped[i] = true;

		    std::erase_if (kept, [breakAt] (const ShapedGlyph& g) { return g.cluster >= breakAt; });
		    lines.insert (lines.begin () + static_cast<long> (i) + 1, std::move (rest));
		    wrapped.insert (wrapped.begin () + static_cast<long> (i) + 1, false);
		} else {
		    wrapped[i] = false;
		    lines.resize (i + 1);
		    wrapped.resize (i + 1);

		    if (params.ellipsis) {
			// the longest start of the line that still fits with the ellipsis after it
			size_t length = glyph.cluster;

			while (true) {
			    std::u32string candidate = line.substr (0, std::min (length, line.size ()));
			    candidate.push_back (kEllipsis);

			    if (this->measureRight (candidate) <= params.maxWidth || length == 0) {
				break;
			    }

			    length--;
			}

			line.resize (std::min (length, line.size ()));

			if (line.empty () || line.back () != kEllipsis) {
			    trimTrailingSpaces (line);
			    line.push_back (kEllipsis);
			}

			reshape = true;
		    } else {
			finishLastRow (line, false);
			lastLine = true;
		    }
		}

		break;
	    }

	    if (reshape) {
		continue;
	    }

	    float lineMinX = 0.0f;
	    float lineMinY = 0.0f;
	    float lineMaxX = 0.0f;
	    float lineMaxY = 0.0f;
	    float penX = 0.0f;
	    float penY = 0.0f;

	    for (const auto& glyph : kept) {
		lineMinX = std::min (lineMinX, penX + glyph.x0);
		lineMinY = std::min (lineMinY, penY + glyph.y0);
		lineMaxX = std::max (lineMaxX, penX + glyph.x1);
		lineMaxY = std::max (lineMaxY, penY + glyph.y1);
		penX += glyph.advance;
		penY += glyph.yAdvance;
	    }

	    // blockalign stretches the spaces of wrapped lines until they fill maxwidth
	    if (params.blockAlign && wrapped[i]) {
		const auto spaces = std::ranges::count_if (lines[i], isBreakSpace);

		if (spaces > 0) {
		    const float extra = (params.maxWidth - (lineMaxX - lineMinX)) / static_cast<float> (spaces);

		    for (auto& glyph : kept) {
			if (isBreakSpace (glyph.codepoint)) {
			    glyph.advance += extra;
			}
		    }

		    lineMaxX = lineMinX + params.maxWidth;
		}
	    }

	    const float width = lineMaxX - lineMinX;
	    const float lineY = static_cast<float> (finished.size ()) * -pitch;

	    minY = std::min (minY, lineY + lineMinY);
	    maxY = std::max (maxY, lineY + lineMaxY);
	    minX = std::min (lineMinX, minX);
	    maxX = std::max (lineMaxX, maxX);
	    widest = std::max (width, widest);
	    glyphCount += kept.size ();
	    finished.push_back ({ std::move (kept), width });

	    if (lastLine) {
		break;
	    }

	    i++;
	}

	if (m_atlasFull && attempt == 0) {
	    m_glyphs.clear ();
	    this->resetAtlases ();
	    continue;
	}

	if (finished.empty () || glyphCount == 0) {
	    return result;
	}

	const float ascender = static_cast<float> (metrics.ascender >> 6);

	result.minX = minX;
	result.maxX = maxX;
	result.top = std::max (ascender, maxY);
	result.bottom = std::min (ascender - static_cast<float> (finished.size ()) * pitch, minY);
	result.pitch = pitch;
	result.lines = static_cast<int> (finished.size ());
	result.ascender = ascender;
	result.descender = static_cast<float> (metrics.descender >> 6);

	float lineY = 0.0f;

	for (const auto& line : finished) {
	    float shift = 0.0f;

	    if (params.align == TextAlign::Center) {
		shift = (widest - line.width) * 0.5f;
	    } else if (params.align == TextAlign::Right) {
		shift = widest - line.width;
	    }

	    float pen = 0.0f;

	    for (const auto& glyph : line.glyphs) {
		const auto it = m_glyphs.find (glyph.key);

		if (it != m_glyphs.end () && it->second.x >= 0) {
		    const AtlasGlyph& entry = it->second;
		    // sub_1401B0410 puts colour glyphs into a vertex buffer of their own
		    const float atlasSize = static_cast<float> (entry.color ? m_colorAtlas.size : m_atlas.size);

		    (entry.color ? result.colorQuads : result.quads)
			.push_back (
			    {
				.rect = { pen + glyph.x0 - glyph.pad + shift, lineY + glyph.y0 - glyph.pad,
					  pen + glyph.x1 + glyph.pad + shift, lineY + glyph.y1 + glyph.pad },
				.uv
				= { static_cast<float> (entry.x) / atlasSize, static_cast<float> (entry.y) / atlasSize,
				    static_cast<float> (entry.x + entry.width) / atlasSize,
				    static_cast<float> (entry.y + entry.height) / atlasSize },
			    }
			);
		}

		pen += glyph.advance;
	    }

	    lineY -= pitch;
	}

	result.valid = true;
	break;
    }

    return result;
}
